# =============================================================================
#  LyCAD - interact 模块（用户交互）
#
#  职责：对象捕捉、工具状态机、命令注册表、命令行解析。
#  约束：依赖 core 与 render；不依赖界面模块，因此可用离屏画布做自动化测试。
# =============================================================================
add_library(lycad_interact STATIC
    SnapEngine.h            SnapEngine.cpp
    CoordinateInput.h       CoordinateInput.cpp
    ToolContext.h           ToolContext.cpp
    Tool.h                  Tool.cpp
    SelectionTool.h         SelectionTool.cpp
    DrawTools.h             DrawTools.cpp
    AnnotationTools.h       AnnotationTools.cpp
    ModifyTools.h           ModifyTools.cpp
    ToolManager.h           ToolManager.cpp
)

add_library(LyCAD::Interact ALIAS lycad_interact)

target_include_directories(lycad_interact PUBLIC "${LYCAD_SRC_ROOT}")

target_link_libraries(lycad_interact
    PUBLIC  LyCAD::Core LyCAD::Render Qt6::Core Qt6::Gui Qt6::Widgets
    PRIVATE LyCAD::Warnings
)
