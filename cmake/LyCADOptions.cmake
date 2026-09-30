# =============================================================================
#  LyCAD 构建选项与第三方依赖探测
# =============================================================================

option(LYCAD_BUILD_TESTS   "构建单元测试与自检程序"            ON)
option(LYCAD_BUILD_TOOLS   "构建开发期辅助工具（样例生成器等）"   ON)
option(LYCAD_WITH_LIBDXFRW "强制启用 libdxfrw 后端（支持原生 DWG 读写）" OFF)
option(LYCAD_ENABLE_ASAN   "启用 AddressSanitizer / UBSan（仅 Debug 有效）" OFF)
option(LYCAD_WARNINGS_AS_ERRORS "将编译告警视为错误"            OFF)

# -----------------------------------------------------------------------------
#  libdxfrw 探测
#
#  libdxfrw 是 LGPL-2.1 授权的 DXF/DWG 底层库，是目前少数可用的开源 DWG
#  读取实现。LyCAD 不内置该库（避免把第三方二进制/源码混入本仓库），
#  仅在系统已安装时自动接入，提供 READ 能力。
#
#  探测顺序：pkg-config -> find_path/find_library 手工组合
# -----------------------------------------------------------------------------
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

# -----------------------------------------------------------------------------
#  外部 DWG 转换器探测（ODA File Converter / LibreDWG）
#  用于在没有 libdxfrw 时仍能提供 DWG 导入导出能力。
# -----------------------------------------------------------------------------
find_program(LYCAD_ODA_CONVERTER NAMES ODAFileConverter)
find_program(LYCAD_LIBREDWG_DWG2DXF NAMES dwg2dxf)
find_program(LYCAD_LIBREDWG_DXF2DWG NAMES dxf2dwg)

# -----------------------------------------------------------------------------
#  消毒器
# -----------------------------------------------------------------------------
if(LYCAD_ENABLE_ASAN AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(LYCAD_WARNINGS_AS_ERRORS)
    add_compile_options($<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>)
endif()
