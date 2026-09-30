# =============================================================================
#  LyCAD - ui 模块（界面装配）
#
#  职责：主窗口、图层面板、属性面板、命令行控件。
#  约束：依赖全部下层模块；除程序入口外，不在其它地方引用本模块。
# =============================================================================
add_library(lycad_ui STATIC
    CommandLineEdit.h       CommandLineEdit.cpp
    LayerPanel.h            LayerPanel.cpp
    PropertyPanel.h         PropertyPanel.cpp
    MainWindow.h            MainWindow.cpp
)

add_library(LyCAD::Ui ALIAS lycad_ui)

target_include_directories(lycad_ui PUBLIC "${LYCAD_SRC_ROOT}")

target_link_libraries(lycad_ui
    PUBLIC  LyCAD::Core LyCAD::Io LyCAD::Render LyCAD::Interact
            Qt6::Core Qt6::Gui Qt6::Widgets
    PRIVATE LyCAD::Warnings
)
