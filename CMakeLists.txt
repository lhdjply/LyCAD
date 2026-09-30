cmake_minimum_required(VERSION 3.21)

project(LyCAD
        VERSION 0.1.0
        DESCRIPTION "LyCAD - 开源二维 CAD 绘图软件"
        LANGUAGES CXX)

# =============================================================================
#  LyCAD 顶层构建脚本
#
#  模块划分（每个模块编译为独立静态库，依赖关系单向、无循环）：
#
#    core      几何模型   基础数学类型 / 图元 / 图层 / 文档模型 / 撤销栈
#    io        文件解析   DXF-DWG 编解码器、编解码器注册表
#    render    渲染显示   视图变换 / 图元绘制 / 画布控件
#    interact  用户交互   工具状态机 / 选择 / 编辑 / 捕捉 / 命令行
#    ui        界面装配   主窗口 / 面板 / 程序入口
#
#  依赖方向：  ui -> interact -> render -> io -> core
# =============================================================================

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LyCADOptions)

# ---------------------------------------------------------------- 语言与构建类型
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

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# ---------------------------------------------------------------- Qt6
#  Svg 用于把界面图标（resources/icons/*.svg）栅格化：图标要在 16 / 24 像素
#  下都清晰，矢量源 + 按需渲染比预置多套 PNG 更简单，也更省资源。
#  Debian/Ubuntu 上对应包名为 qt6-svg-dev。
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Svg)
qt_standard_project_setup()

# ---------------------------------------------------------------- 统一编译告警
add_library(lycad_warnings INTERFACE)
add_library(LyCAD::Warnings ALIAS lycad_warnings)
target_compile_options(lycad_warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wno-unused-parameter>
    $<$<CXX_COMPILER_ID:MSVC>:/W3>)

# ---------------------------------------------------------------- 模块
add_subdirectory(src)

if(LYCAD_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(LYCAD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------- 构建摘要
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
