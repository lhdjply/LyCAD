# =============================================================================
#  LyCAD - core 模块（几何模型）
#
#  职责：基础数学类型、图元、图层、文档模型、撤销栈。
#  约束：只依赖 Qt6::Core 与 Qt6::Gui（QColor），不依赖任何上层模块。
# =============================================================================
add_library(lycad_core STATIC
    Types.h                 Types.cpp
    AciPalette.h            AciPalette.cpp
    Format.h                Format.cpp
    Entity.h                Entity.cpp
    PointEntity.h           PointEntity.cpp
    LineEntity.h            LineEntity.cpp
    CircleEntity.h          CircleEntity.cpp
    ArcEntity.h             ArcEntity.cpp
    PolylineEntity.h        PolylineEntity.cpp
    TextEntity.h            TextEntity.cpp
    HatchEntity.h           HatchEntity.cpp
    DimensionEntity.h       DimensionEntity.cpp
    Layer.h                 Layer.cpp
    Linetype.h              Linetype.cpp
    Selection.h             Selection.cpp
    Document.h              Document.cpp
    Commands.h              Commands.cpp
)

add_library(LyCAD::Core ALIAS lycad_core)

target_include_directories(lycad_core PUBLIC "${LYCAD_SRC_ROOT}")

# 撤销栈 (QUndoStack) 与颜色 (QColor) 位于 QtGui
target_link_libraries(lycad_core
    PUBLIC  Qt6::Core Qt6::Gui
    PRIVATE LyCAD::Warnings
)

target_compile_definitions(lycad_core PUBLIC QT_DISABLE_DEPRECATED_UP_TO=0x060500)
