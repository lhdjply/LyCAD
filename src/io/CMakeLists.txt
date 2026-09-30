# =============================================================================
#  LyCAD - io 模块（文件解析）
#
#  职责：图形文件的读入与写出。当前实现 DXF（原生）与 DWG（后端适配）。
#  约束：依赖 core；不依赖渲染或界面，因此可脱离 GUI 单独做单元测试。
# =============================================================================
add_library(lycad_io STATIC
    CadCodec.h              CadCodec.cpp
    CadFile.h               CadFile.cpp
    DxfGroup.h              DxfGroup.cpp
    DxfReader.h             DxfReader.cpp
    DxfWriter.h             DxfWriter.cpp
    DxfCodec.h              DxfCodec.cpp
    DwgCodec.h              DwgCodec.cpp
)

add_library(LyCAD::Io ALIAS lycad_io)

target_include_directories(lycad_io PUBLIC "${LYCAD_SRC_ROOT}")

target_link_libraries(lycad_io
    PUBLIC  LyCAD::Core Qt6::Core Qt6::Gui
    PRIVATE LyCAD::Warnings
)

# 若系统提供 libdxfrw，则一并暴露其头文件目录，
# 便于后续接入原生 DWG 后端（当前实现走外部转换器路线）。
if(LYCAD_LIBDXFRW_FOUND)
    target_compile_definitions(lycad_io PUBLIC LYCAD_HAVE_LIBDXFRW=1)
endif()
