cmake_minimum_required(VERSION 3.21)

# ---------------------------------------------------------------------------
# 版本号只有一个来源：仓库根目录的 VERSION 文件
#
# 界面显示的版本、Windows 安装包名与 release 产物名都取它（.github/workflows/
# release.yaml 直接读同一个文件）。debian/changelog 的版本是唯一需要手工同步的
# 第二处——包名走的是 changelog 那一份。
#
# string(STRIP) 顺带吃掉行尾的 \r：Windows 上若按 CRLF 检出，版本号会带进一个
# 回车，project(VERSION ...) 会直接解析失败。
# ---------------------------------------------------------------------------
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LYCAD_VERSION LIMIT_COUNT 1)
string(STRIP "${LYCAD_VERSION}" LYCAD_VERSION)
if(LYCAD_VERSION STREQUAL "")
    message(FATAL_ERROR "无法从 ${CMAKE_CURRENT_SOURCE_DIR}/VERSION 读取版本号（文件缺失或为空）")
endif()

# file() 只在配置期读一次，CMake 不会自动把读过的文件登记成"重配置依赖"：
# 少了这一行，改完 VERSION 直接 `cmake --build` 会继续用旧版本号（得手工再
# configure 一次才生效）。登记之后，VERSION 一变，下一次构建会自动重跑 CMake。
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

project(LyCAD
        VERSION ${LYCAD_VERSION}
        DESCRIPTION "LyCAD - 开源二维 CAD 绘图软件"
        LANGUAGES CXX)

# 模块依赖严格单向：core → io → render → interact → ui。
# 源码按目录 GLOB 收集直接编进可执行文件，不产出中间库。
set(LYCAD_SRC_ROOT "${PROJECT_SOURCE_DIR}/src")

# ---------------------------------------------------------------------------
# 选项
# ---------------------------------------------------------------------------
# 两个开关只决定目标是否被定义；测试与工具是 EXCLUDE_FROM_ALL 的，
# 默认构建只编主程序，需要时用 --target lycad_tests / lycad_tools 按需构建。
option(LYCAD_BUILD_TESTS   "定义单元测试目标（--target lycad_tests 按需构建）"  ON)
option(LYCAD_BUILD_TOOLS   "定义开发工具目标（--target lycad_tools 按需构建）"  ON)
option(LYCAD_ENABLE_ASAN   "启用 AddressSanitizer / UBSan（仅 Debug 有效）"     OFF)
option(LYCAD_WARNINGS_AS_ERRORS "将编译告警视为错误"                            OFF)

# ---------------------------------------------------------------------------
# 语言 / 构建类型 / 输出目录
# ---------------------------------------------------------------------------
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

# 主程序落 build 根目录；tests/ 与 tools/ 各自覆盖此变量归到 build/tests、build/tools
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ---------------------------------------------------------------------------
# Qt6（Svg 用于图标栅格化；PrintSupport 用于打印，仅 ui 层使用）
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Svg PrintSupport)
qt_standard_project_setup()          # AUTOMOC / AUTOUIC
add_compile_definitions(QT_DISABLE_DEPRECATED_UP_TO=0x060500)

# 应用版本以宏注入，唯一来源是根 VERSION 文件（见文件开头）。代码里**不要**再写
# 版本字面量：main.cpp 用它设置 QApplication::applicationVersion()，也就是 --version
# 与将来任何版本展示取的那一处。全局定义而非逐个目标添加，是因为 app、tools 与
# tests 都会编同一批源码（这个宏只是常量，多一份不会有副作用）。
add_compile_definitions(LYCAD_VERSION="${PROJECT_VERSION}")

# ---------------------------------------------------------------------------
# 统一编译告警
# ---------------------------------------------------------------------------
add_library(lycad_warnings INTERFACE)
add_library(LyCAD::Warnings ALIAS lycad_warnings)
target_compile_options(lycad_warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wno-unused-parameter>
    $<$<CXX_COMPILER_ID:MSVC>:/W3>)

# ---------------------------------------------------------------------------
# 消毒器 / 告警即错误
# ---------------------------------------------------------------------------
if(LYCAD_ENABLE_ASAN AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(LYCAD_WARNINGS_AS_ERRORS)
    add_compile_options($<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>)
endif()

# ---------------------------------------------------------------------------
# 源码（CONFIGURE_DEPENDS：每次构建检查目录变化，新增文件自动生效）
# ---------------------------------------------------------------------------
file(GLOB_RECURSE LYCAD_SOURCES CONFIGURE_DEPENDS
    "${LYCAD_SRC_ROOT}/core/*.h"     "${LYCAD_SRC_ROOT}/core/*.cpp"
    "${LYCAD_SRC_ROOT}/io/*.h"       "${LYCAD_SRC_ROOT}/io/*.cpp"
    "${LYCAD_SRC_ROOT}/render/*.h"   "${LYCAD_SRC_ROOT}/render/*.cpp"
    "${LYCAD_SRC_ROOT}/interact/*.h" "${LYCAD_SRC_ROOT}/interact/*.cpp"
    "${LYCAD_SRC_ROOT}/ui/*.h"       "${LYCAD_SRC_ROOT}/ui/*.cpp")

# qrc 不是源码，glob 收不到，必须显式列出
set(LYCAD_RESOURCES "${PROJECT_SOURCE_DIR}/resources/lycad.qrc")

# 源码子集：headless（不含 ui，单元测试用）、core+io（样例生成器用，
# 兼作依赖方向检查——下层反向依赖上层时会因符号缺失链接失败）
set(LYCAD_HEADLESS_SOURCES ${LYCAD_SOURCES})
list(FILTER LYCAD_HEADLESS_SOURCES EXCLUDE REGEX "/ui/")

set(LYCAD_CORE_IO_SOURCES ${LYCAD_SOURCES})
list(FILTER LYCAD_CORE_IO_SOURCES EXCLUDE REGEX "/(render|interact|ui)/")

# ---------------------------------------------------------------------------
# libdxfrw-modify：DWG / DXF 原生解析（libdxfrw 0.6.3 永久修改版）
#
# 修改点：文本解码器只看当前文件版本；可见性按规范还原；二进制 DXF 组码按
# 1 字节读；未知实体经回调上报。回归测试见 tests/test_libdxfrw_codec.cpp，
# 详见 README「第三方组件」。许可证 GPL-2.0-or-later。
#
# 单独建 OBJECT 目标：第三方代码要隔离告警（-w），且对象库只编译一次、
# 各可执行文件复用，不产出 .a/.so。
# ---------------------------------------------------------------------------
file(GLOB_RECURSE LYCAD_LIBDXFRW_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/third-party/libdxfrw-modify/src/*.cpp")

add_library(lycad_dxfrw OBJECT ${LYCAD_LIBDXFRW_SOURCES})
target_include_directories(lycad_dxfrw PRIVATE
    "${PROJECT_SOURCE_DIR}/third-party/libdxfrw-modify/src"
    "${PROJECT_SOURCE_DIR}/third-party/libdxfrw-modify/src/intern")
target_compile_options(lycad_dxfrw PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>
    $<$<CXX_COMPILER_ID:MSVC>:/w>)

# iconv：intern/drw_textcodec.cpp 用它做文本编码转换（DWG/DXF 的代码页 → UTF-8）。
#   · glibc / macOS 把 iconv 放在 C 库里，Iconv::Iconv 是个空的 INTERFACE 目标；
#   · Windows 没有自带 iconv，得靠 MSYS2 的 libiconv（包名 libiconv）——
#     不链接的话，链接期报 `undefined reference to 'libiconv_open'`
#     （GNU libiconv 的头文件把 iconv_open 宏映射成 libiconv_open，符号名带前缀）。
# Windows 上找不到就直接失败：这条依赖缺了必然链接不过，配置期报错比链接期清楚；
# 本仓库的 Windows 构建只支持 MinGW，MSVC 侧取不到 iconv.h，也会走到这条报错。
find_package(Iconv QUIET)
if(WIN32 AND NOT Iconv_FOUND)
    message(FATAL_ERROR
        "Windows 上找不到 iconv，而 drw_textcodec.cpp 需要它（缺了链接期会报 libiconv_open 未定义）。\n"
        "  MinGW（MSYS2）：pacboy libiconv:p\n"
        "  注：本仓库的 Windows 构建只支持 MinGW，MSVC 侧没有可链接的 iconv。")
endif()
if(Iconv_FOUND)
    target_link_libraries(lycad_dxfrw PRIVATE Iconv::Iconv)
endif()

# 消费 libdxfrw 对象的方式：target_link_libraries(<目标> PRIVATE lycad_dxfrw)
#
# 不要改成把 $<TARGET_OBJECTS:lycad_dxfrw> 塞进 add_executable 的源码列表：
# 那个写法只把 .o 放进链接行，**不携带**本目标的使用要求（CMake 文档
# 「Linking Object Libraries via $<TARGET_OBJECTS>」一节写明了这点），
# 上面挂的 PRIVATE 依赖 Iconv::Iconv 就会丢。Linux/macOS 上 iconv 在 libc 里，
# 丢了也看不出来；Windows/MinGW 上表现为链接期 libiconv_open 未定义。
# 链接目标本身则两者都给：对象文件 + 使用要求（PRIVATE 依赖以 $<LINK_ONLY:> 传递）。
set(LYCAD_DXFRW_INCLUDES
    "${PROJECT_SOURCE_DIR}/third-party/libdxfrw-modify/src"
    "${PROJECT_SOURCE_DIR}/third-party/libdxfrw-modify/src/intern")

# ---------------------------------------------------------------------------
# 主程序
# ---------------------------------------------------------------------------
set(LYCAD_QT_LIBS Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg Qt6::PrintSupport)

add_executable(lycad
    src/main.cpp
    ${LYCAD_SOURCES}
    ${LYCAD_RESOURCES}
)
target_include_directories(lycad PRIVATE "${LYCAD_SRC_ROOT}")
# libdxfrw 头文件以 SYSTEM 引入：其自带告警与本项目无关，交给编译器忽略
target_include_directories(lycad SYSTEM PRIVATE ${LYCAD_DXFRW_INCLUDES})
target_link_libraries(lycad PRIVATE lycad_dxfrw ${LYCAD_QT_LIBS} LyCAD::Warnings)

set_target_properties(lycad PROPERTIES
    AUTORCC          ON
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE    ON
    OUTPUT_NAME      lycad
)

# ---------------------------------------------------------------------------
# 应用图标（各平台的「系统层面」）
#
# 窗口与对话框的图标来自 qrc（resources/lycad.qrc 的 /app 前缀，见
# ui/IconLibrary::applicationIcon）。但系统层面看不到 Qt 资源，各平台各要一份：
#   · Windows 读 PE 资源段里的图标 → .rc；
#   · macOS  读 bundle 内的 .icns；
#   · Linux  读图标主题目录里的 PNG 与 .desktop 条目（见下方安装规则）。
# 图标源文件与环境无关的那份 SVG 在 resources/app/app-icon.svg，
# 位图集（16~1024）在 resources/app/png，改图标时以 SVG 为准再重新导出。
# ---------------------------------------------------------------------------
if(WIN32)
    target_sources(lycad PRIVATE resources/win/lycad.rc)
elseif(APPLE)
    set(LYCAD_MACOS_ICON "${PROJECT_SOURCE_DIR}/resources/app/lycad.icns")
    set_source_files_properties("${LYCAD_MACOS_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(lycad PRIVATE "${LYCAD_MACOS_ICON}")
    set_target_properties(lycad PROPERTIES MACOSX_BUNDLE_ICON_FILE lycad.icns)
endif()

# ---------------------------------------------------------------------------
# 安装 / 子目录 / 构建摘要
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS lycad
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    BUNDLE  DESTINATION "."
)

# Linux 桌面集成：菜单 / 任务栏 / 文件管理器按图标名 "lycad" 到图标主题里找，
# 因此文件名必须是 lycad.png 并落在 hicolor 的标准尺寸目录下（.desktop 的
# Icon=lycad 与之对应）。缺了这些，装完只能从终端启动，且任务栏没有图标。
if(UNIX AND NOT APPLE)
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/lycad.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    foreach(size 16 24 32 48 64 128 256 512)
        install(FILES "${PROJECT_SOURCE_DIR}/resources/app/png/lycad-${size}.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps"
            RENAME lycad.png)
    endforeach()
endif()

if(LYCAD_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(LYCAD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "==================== LyCAD 构建配置 ====================")
message(STATUS "  版本            : ${PROJECT_VERSION}")
message(STATUS "  构建类型        : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ 标准        : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  编译器          : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Qt 版本         : ${Qt6_VERSION}")
message(STATUS "  单元测试目标    : ${LYCAD_BUILD_TESTS}（按需：--target lycad_tests）")
message(STATUS "  开发工具目标    : ${LYCAD_BUILD_TOOLS}（按需：--target lycad_tools）")
message(STATUS "  DWG 原生解析    : libdxfrw（内置源码，编译期编入）")
message(STATUS "  可执行文件      : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/lycad")
message(STATUS "========================================================")
message(STATUS "")
