# =============================================================================
#  LyCAD - render 模块（渲染显示）
#
#  职责：世界/设备坐标换算、图元绘制、画布控件（网格、轴、捕捉标记、光标）。
#  约束：依赖 core；不依赖 io 与交互。交互层反向依赖本模块。
# =============================================================================
add_library(lycad_render STATIC
    CanvasTypes.h           CanvasTypes.cpp
    ViewTransform.h         ViewTransform.cpp
    EntityRenderer.h        EntityRenderer.cpp
    CadCanvas.h             CadCanvas.cpp
)

add_library(LyCAD::Render ALIAS lycad_render)

target_include_directories(lycad_render PUBLIC "${LYCAD_SRC_ROOT}")

# QPainter / QWidget / QColor 分别位于 QtGui 与 QtWidgets
target_link_libraries(lycad_render
    PUBLIC  LyCAD::Core Qt6::Core Qt6::Gui Qt6::Widgets
    PRIVATE LyCAD::Warnings
)
