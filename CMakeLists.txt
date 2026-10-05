cmake_minimum_required(VERSION 3.21)

project(LyCAD
        VERSION 0.1.0
        DESCRIPTION "LyCAD - 开源二维 CAD 绘图软件"
        LANGUAGES CXX)

# =============================================================================
#  LyCAD 构建脚本
#
#  构建描述只有三处：本文件、tools/CMakeLists.txt、tests/CMakeLists.txt。
#  src/ 与 resources/ 下只有内容，没有任何构建脚本。
#
#  代码按模块划分，但模块不再编译成独立的库，也没有任何中间产物
#  （.a / .so）：源码按目录收集，直接编进各个可执行文件。构建过程因此只有
#  一件事——编译并链接可执行文件，脚本里没有目标图、也没有对象文件传递规则。
#
#  模块划分（依赖方向严格单向、无循环）：
#
#    core      几何模型   基础数学类型 / 图元 / 图层 / 文档模型 / 撤销栈
#    io        文件解析   DXF 编解码器、DWG 后端适配、编解码器注册表
#    render    渲染显示   视图变换 / 图元绘制 / 画布控件
#    interact  用户交互   工具状态机 / 选择 / 编辑 / 捕捉 / 命令行
#    ui        界面装配   主窗口 / 功能区 / 主题 / 图标库 / 面板
#
#    core ──> io ──> render ──> interact ──> ui
#
#  依赖方向靠两件事守住：一是各模块的源码只从自己那一层及以下取头文件，
#  二是 tools/lycad_sample 只编入 core 与 io——一旦下层反向依赖了渲染或界面，
#  它会立刻因符号缺失而链接失败，等于每次构建都做一次契约检查。
#
#  源文件按目录用 file(GLOB_RECURSE) 收集，新增 .h/.cpp 不需要改本文件。
#  关键是要带 CONFIGURE_DEPENDS：裸的 glob 只在配置阶段展开一次，之后新增的
#  文件不会被编译，而且不报错——这种遗漏往往只在别人机器上才暴露。带上它之后，
#  构建系统会在每次构建时检查目录内容，有变化就自动重新配置。
# =============================================================================

set(LYCAD_SRC_ROOT "${PROJECT_SOURCE_DIR}/src")

# =============================================================================
#  1. 构建选项
# =============================================================================
option(LYCAD_BUILD_TESTS   "构建单元测试与自检程序"            ON)
option(LYCAD_BUILD_TOOLS   "构建开发期辅助工具（样例生成器等）"   ON)
option(LYCAD_ENABLE_ASAN   "启用 AddressSanitizer / UBSan（仅 Debug 有效）" OFF)
option(LYCAD_WARNINGS_AS_ERRORS "将编译告警视为错误"            OFF)

# =============================================================================
#  2. 语言与构建类型
# =============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
endif()

# 只产出可执行文件：没有库，自然也不需要库的输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# =============================================================================
#  3. Qt6
#
#  Svg 用于把界面图标（resources/icons/*.svg）栅格化：图标要在 16 / 24 像素
#  下都清晰，矢量源 + 按需渲染比预置多套 PNG 更简单，也更省资源。
#  Debian/Ubuntu 上对应包名为 qt6-base-dev 与 qt6-svg-dev。
# =============================================================================
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Svg PrintSupport)
qt_standard_project_setup()          # 打开 AUTOMOC / AUTOUIC

# 撤销栈 (QUndoStack) 与颜色 (QColor) 位于 QtGui，全工程都会用到
add_compile_definitions(QT_DISABLE_DEPRECATED_UP_TO=0x060500)

# =============================================================================
#  4. 统一编译告警
# =============================================================================
add_library(lycad_warnings INTERFACE)
add_library(LyCAD::Warnings ALIAS lycad_warnings)
target_compile_options(lycad_warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wno-unused-parameter>
    $<$<CXX_COMPILER_ID:MSVC>:/W3>)

# =============================================================================
#  5. 外部 DWG 转换器探测
#
#  DWG 的**读取**由内置的 libdxfrw 完成（见第 8 节），不需要任何外部程序。
#  但 libdxfrw 不具备 DWG 写出能力，因此**导出**仍依赖外部转换器：
#  LibreDWG（dwg2dxf / dxf2dwg）、ODA File Converter，或用户自定义命令。
#
#  这里只做探测，真正的调用在 io/DwgCodec.cpp。一个都找不到时，DWG 导出会在
#  界面上说明原因与安装方法，而不是静默失败。
# =============================================================================
find_program(LYCAD_ODA_CONVERTER NAMES ODAFileConverter)
find_program(LYCAD_LIBREDWG_DWG2DXF NAMES dwg2dxf)
find_program(LYCAD_LIBREDWG_DXF2DWG NAMES dxf2dwg)

# =============================================================================
#  6. 消毒器
# =============================================================================
if(LYCAD_ENABLE_ASAN AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(LYCAD_WARNINGS_AS_ERRORS)
    add_compile_options($<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>)
endif()

# =============================================================================
#  7. 源码
#
#  五个模块的源文件一次收齐——模块划分体现在目录上（见文件开头的模块表），
#  不再是一个模块一个变量。新增 .h/.cpp 放进对应目录即可，不必改本文件。
#
#  依赖方向靠「谁 include 谁」守住，另由下面的 LYCAD_CORE_IO_SOURCES 兜底检查。
# =============================================================================
file(GLOB_RECURSE LYCAD_SOURCES CONFIGURE_DEPENDS
    "${LYCAD_SRC_ROOT}/core/*.h"
    "${LYCAD_SRC_ROOT}/core/*.cpp"
    "${LYCAD_SRC_ROOT}/io/*.h"
    "${LYCAD_SRC_ROOT}/io/*.cpp"
    "${LYCAD_SRC_ROOT}/render/*.h"
    "${LYCAD_SRC_ROOT}/render/*.cpp"
    "${LYCAD_SRC_ROOT}/interact/*.h"
    "${LYCAD_SRC_ROOT}/interact/*.cpp"
    "${LYCAD_SRC_ROOT}/ui/*.h"
    "${LYCAD_SRC_ROOT}/ui/*.cpp")

# 图标资源。qrc 不是源码，glob 收不到，必须显式列出。
set(LYCAD_RESOURCES "${PROJECT_SOURCE_DIR}/resources/lycad.qrc")

# ---------------------------------------------------------------- 子集
#
#  从同一份清单里滤出两种子集，供不需要完整功能的可执行文件使用。
#
#  · LYCAD_HEADLESS_SOURCES  到 interact 为止，不含 ui —— 单元测试用到。
#    画布逻辑与几何逻辑已经分离，测试不必拉进整个界面层。
#
#  · LYCAD_CORE_IO_SOURCES   只有最底下两层 —— 样例生成器用到。
#    这个是刻意留着的检查：一旦 core / io 反向依赖了渲染或界面层，
#    lycad_sample 会立刻因符号缺失而链接失败，等于每次构建做一次依赖方向检查。
set(LYCAD_HEADLESS_SOURCES ${LYCAD_SOURCES})
list(FILTER LYCAD_HEADLESS_SOURCES EXCLUDE REGEX "/ui/")

set(LYCAD_CORE_IO_SOURCES ${LYCAD_SOURCES})
list(FILTER LYCAD_CORE_IO_SOURCES EXCLUDE REGEX "/(render|interact|ui)/")

# =============================================================================
#  8. third-party/libdxfrw-modify —— DWG / DXF 原生解析（libdxfrw 修改版）
#
#  职责：DWG（R14 ~ 2018）与 DXF 的读取与 DXF 写出。io/LibDxfRwAdapter 是
#        它与 LyCAD 模型之间的适配层；DWG 写出由外部转换器承担（库无此能力）。
#
#  这是 libdxfrw 0.6.3 的**永久修改版**（已裁剪为只含源码，不再跟随上游），
#  相对上游的修改：
#    · 文本解码器只看当前文件的版本——上游用进程级状态选择解码器，
#      先读老版本文件会让 2007+ 文件的中文全部乱码；
#    · 可见性按规范还原——上游 DXF 路径把 60 组码语义取反、DWG 路径丢弃标志；
#    · 二进制 DXF 组码按规范 1 字节读取——上游按 2 字节读，合法文件全部解析失败；
#    · 未知实体类型经 addUnknownEntity 回调上报计数——上游静默跳过。
#  对应回归测试：tests/test_libdxfrw_codec.cpp，详见 README 第 8 节。
#
#  许可证：GPL-2.0-or-later（修改版同样适用，且必须以同等许可分发）。
#          它随本项目一起分发时，整个二进制的分发受 GPL 约束，
#          详见 third-party/libdxfrw-modify/COPYING 与 README 的「第三方组件」一节。
#
#  为什么单独建一个目标，而不是像 src/ 那样把源码拼进各可执行文件：
#
#    1. 编译选项必须隔离。第三方代码不能套用本项目的 -Wall -Wextra——它有自己的
#       书写风格，上百条告警会把真正的问题淹没；而它也不该被要求遵守本项目的规范。
#       CMake 的「源文件属性」（set_source_files_properties）只在同一目录内可见，
#       tools/ 与 tests/ 下的目标读不到在根目录设的属性，所以必须由独立目标承载。
#    2. 它被全部可执行文件用到（凡含 io 源码者皆然）。对象库只编译一次、
#       多个目标复用，避免同一份第三方代码被编译 8 遍。
#
#  这里用 OBJECT 而不是 STATIC：对象库不产出 .a / .so，
#  与「构建过程只产出可执行文件」的约定一致。
# =============================================================================
file(GLOB_RECURSE LYCAD_LIBDXFRW_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/third-party/libdxfrw-modify/src/*.cpp")

add_library(lycad_dxfrw OBJECT ${LYCAD_LIBDXFRW_SOURCES})

target_include_directories(lycad_dxfrw PRIVATE
    "${PROJECT_SOURCE_DIR}/third-party/libdxfrw-modify/src"
    "${PROJECT_SOURCE_DIR}/third-party/libdxfrw-modify/src/intern")

# 第三方代码：关闭告警。它不按本项目的规范书写，也不该因此挡住构建输出。
target_compile_options(lycad_dxfrw PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>
    $<$<CXX_COMPILER_ID:MSVC>:/w>)

# libdxfrw 的字符编码转换依赖 iconv。glibc 自带（无需额外链接），
# macOS / 部分 Windows 工具链需要显式链接。
find_package(Iconv QUIET)
if(Iconv_FOUND)
    target_link_libraries(lycad_dxfrw PRIVATE Iconv::Iconv)
endif()

# 它的目标文件与头文件目录：凡是编入了 io 源码的目标都要带上
set(LYCAD_DXFRW_OBJECTS "$<TARGET_OBJECTS:lycad_dxfrw>")
set(LYCAD_DXFRW_INCLUDES
    "${PROJECT_SOURCE_DIR}/third-party/libdxfrw-modify/src"
    "${PROJECT_SOURCE_DIR}/third-party/libdxfrw-modify/src/intern")

# =============================================================================
#  9. 可执行程序
#
#  这里只定义随软件发布的主程序；开发期工具在 tools/ 下。
#  它们都用全部源码，因此 Qt 依赖取最高一层需要的：Widgets（画布与界面）、
#  Svg（图标栅格化）与 PrintSupport（打印与打印预览）。
#
#  PrintSupport 只在界面层出现（打印对话框与 QPrinter 都在 ui/MainWindow）：
#  render/PlotRenderer 只认 QPainter 与目标矩形，不认识打印设备，因此
#  「到 interact 为止」的单元测试不必链接它——依赖方向仍然只有一条。
#
#  凡是编入了 io 源码的目标，都要一并带上 libdxfrw 的对象文件与头文件目录——
#  io 的 DWG 解析依赖它。
# =============================================================================
set(LYCAD_QT_LIBS Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::PrintSupport)

# ---------------------------------------------------------------- 主程序
add_executable(lycad
    src/main.cpp
    ${LYCAD_SOURCES}
    ${LYCAD_RESOURCES}
    ${LYCAD_DXFRW_OBJECTS}
)

target_include_directories(lycad PRIVATE "${LYCAD_SRC_ROOT}")
# libdxfrw 的头文件以 SYSTEM 方式引入：它自带 #pragma warning 与隐式拷贝构造等告警，
# 与本项目无关，用 -isystem 交给编译器忽略，免得刷屏埋掉真正的问题。
target_include_directories(lycad SYSTEM PRIVATE ${LYCAD_DXFRW_INCLUDES})
target_link_libraries(lycad PRIVATE ${LYCAD_QT_LIBS} LyCAD::Warnings)

# .qrc 要交给 rcc 处理
set_target_properties(lycad PROPERTIES
    AUTORCC          ON
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE    ON
    OUTPUT_NAME      lycad
)

# =============================================================================
#  10. 安装规则
# =============================================================================
include(GNUInstallDirs)
install(TARGETS lycad
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    BUNDLE  DESTINATION "."
)

# =============================================================================
#  11. 开发工具与单元测试
#
#  这两类目标各自带一个 CMakeLists.txt：它们组合源码的方式与主程序不同
#  （用到的模块范围不一样），放在自己目录里更贴近内容。
# =============================================================================
if(LYCAD_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(LYCAD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# =============================================================================
#  12. 构建摘要
# =============================================================================
message(STATUS "")
message(STATUS "==================== LyCAD 构建配置 ====================")
message(STATUS "  版本            : ${PROJECT_VERSION}")
message(STATUS "  构建类型        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ 标准        : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  编译器          : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Qt 版本         : ${Qt6_VERSION}")
message(STATUS "  构建单元测试    : ${LYCAD_BUILD_TESTS}")
message(STATUS "  构建开发工具    : ${LYCAD_BUILD_TOOLS}")
message(STATUS "  DWG 原生解析    : libdxfrw（内置源码，编译期编入）")
message(STATUS "  可执行文件      : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/lycad")
message(STATUS "========================================================")
message(STATUS "")
