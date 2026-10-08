cmake_minimum_required(VERSION 3.21)

project(LyCAD
        VERSION 0.1.0
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

find_package(Iconv QUIET)
if(Iconv_FOUND)
    target_link_libraries(lycad_dxfrw PRIVATE Iconv::Iconv)
endif()

set(LYCAD_DXFRW_OBJECTS "$<TARGET_OBJECTS:lycad_dxfrw>")
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
    ${LYCAD_DXFRW_OBJECTS}
)
target_include_directories(lycad PRIVATE "${LYCAD_SRC_ROOT}")
# libdxfrw 头文件以 SYSTEM 引入：其自带告警与本项目无关，交给编译器忽略
target_include_directories(lycad SYSTEM PRIVATE ${LYCAD_DXFRW_INCLUDES})
target_link_libraries(lycad PRIVATE ${LYCAD_QT_LIBS} LyCAD::Warnings)

set_target_properties(lycad PROPERTIES
    AUTORCC          ON
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE    ON
    OUTPUT_NAME      lycad
)

# ---------------------------------------------------------------------------
# 安装 / 子目录 / 构建摘要
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS lycad
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    BUNDLE  DESTINATION "."
)

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
