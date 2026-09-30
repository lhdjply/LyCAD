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
option(LYCAD_WITH_LIBDXFRW "强制启用 libdxfrw 后端（支持原生 DWG 读写）" OFF)
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
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Svg)
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
#  5. libdxfrw 探测
#
#  libdxfrw 是 LGPL-2.1 授权的 DXF/DWG 底层库，是目前少数可用的开源 DWG
#  读取实现。LyCAD 不内置该库（避免把第三方二进制/源码混入本仓库），
#  仅在系统已安装时自动接入。
#
#  探测顺序：pkg-config -> find_path/find_library 手工组合
# =============================================================================
set(LYCAD_LIBDXFRW_FOUND FALSE)
set(LYCAD_LIBDXFRW_TARGET "")

if(LYCAD_WITH_LIBDXFRW OR NOT DEFINED LYCAD_AUTO_DETECT_DXFRW)
    set(LYCAD_AUTO_DETECT_DXFRW TRUE)
endif()

if(LYCAD_AUTO_DETECT_DXFRW OR LYCAD_WITH_LIBDXFRW)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PC_LIBDXFRW QUIET libdxfrw)
    endif()

    if(PC_LIBDXFRW_FOUND)
        add_library(lycad_dxfrw UNKNOWN IMPORTED)
        set_target_properties(lycad_dxfrw PROPERTIES
            IMPORTED_LOCATION "${PC_LIBDXFRW_LINK_LIBRARIES}"
            INTERFACE_INCLUDE_DIRECTORIES "${PC_LIBDXFRW_INCLUDE_DIRS}")
        set(LYCAD_LIBDXFRW_FOUND TRUE)
        set(LYCAD_LIBDXFRW_TARGET lycad_dxfrw)
    else()
        find_path(LIBDXFRW_INCLUDE_DIR NAMES libdxfrw.h
                  PATHS /usr/include /usr/local/include /opt/local/include)
        find_library(LIBDXFRW_LIBRARY NAMES dxfrw libdxfrw
                     PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu)
        if(LIBDXFRW_INCLUDE_DIR AND LIBDXFRW_LIBRARY)
            add_library(lycad_dxfrw UNKNOWN IMPORTED)
            set_target_properties(lycad_dxfrw PROPERTIES
                IMPORTED_LOCATION "${LIBDXFRW_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${LIBDXFRW_INCLUDE_DIR}")
            set(LYCAD_LIBDXFRW_FOUND TRUE)
            set(LYCAD_LIBDXFRW_TARGET lycad_dxfrw)
        endif()
    endif()

    if(LYCAD_WITH_LIBDXFRW AND NOT LYCAD_LIBDXFRW_FOUND)
        message(WARNING
            "LYCAD_WITH_LIBDXFRW=ON 但未找到 libdxfrw。DWG 将退化为外部转换器/报错模式。"
            "Debian/Ubuntu 可尝试: apt install libdxfrw-dev")
    endif()
endif()

# 找到 libdxfrw 时让 io 模块启用原生后端分支
if(LYCAD_LIBDXFRW_FOUND)
    add_compile_definitions(LYCAD_HAVE_LIBDXFRW=1)
endif()

# 外部 DWG 转换器探测（ODA File Converter / LibreDWG）：
# 用于在没有 libdxfrw 时仍能提供 DWG 导入导出能力。
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
#  8. 可执行程序
#
#  这里只定义随软件发布的主程序；开发期工具在 tools/ 下。
#  它们都用全部源码，因此 Qt 依赖取最高一层需要的：Widgets（画布与界面）
#  与 Svg（图标栅格化）。
# =============================================================================
set(LYCAD_QT_LIBS Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg)

# ---------------------------------------------------------------- 主程序
add_executable(lycad
    src/main.cpp
    ${LYCAD_SOURCES}
    ${LYCAD_RESOURCES}
)

target_include_directories(lycad PRIVATE "${LYCAD_SRC_ROOT}")
target_link_libraries(lycad PRIVATE ${LYCAD_QT_LIBS} LyCAD::Warnings)

# .qrc 要交给 rcc 处理
set_target_properties(lycad PROPERTIES
    AUTORCC          ON
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE    ON
    OUTPUT_NAME      lycad
)

# =============================================================================
#  9. 安装规则
# =============================================================================
include(GNUInstallDirs)
install(TARGETS lycad
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    BUNDLE  DESTINATION "."
)

# =============================================================================
#  10. 开发工具与单元测试
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
#  11. 构建摘要
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
message(STATUS "  libdxfrw 后端   : ${LYCAD_LIBDXFRW_FOUND}")
message(STATUS "  可执行文件      : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/lycad")
message(STATUS "========================================================")
message(STATUS "")
