# =============================================================================
#  LyCAD - ui 模块（界面装配）
#
#  职责：主窗口、功能区、面板、命令行控件、图标资源。
#  约束：依赖全部下层模块；除程序入口外，不在其它地方引用本模块。
# =============================================================================
add_library(lycad_ui STATIC
    Theme.h                 Theme.cpp
    IconLibrary.h           IconLibrary.cpp
    RibbonBar.h             RibbonBar.cpp
    CommandLineEdit.h       CommandLineEdit.cpp
    LayerPanel.h            LayerPanel.cpp
    PropertyChoices.h       PropertyChoices.cpp
    PropertyPanel.h         PropertyPanel.cpp
    MainWindow.h            MainWindow.cpp

    # 图标资源由本模块携带。rcc 生成的目标文件必须被显式引用才会进入链接，
    # 这一点由 IconLibrary 构造函数里的 Q_INIT_RESOURCE 保证。
    "${CMAKE_SOURCE_DIR}/resources/lycad.qrc"
)

# .qrc 要交给 rcc 处理，需要打开 AUTORCC
set_target_properties(lycad_ui PROPERTIES AUTORCC ON)

add_library(LyCAD::Ui ALIAS lycad_ui)

target_include_directories(lycad_ui PUBLIC "${LYCAD_SRC_ROOT}")

target_link_libraries(lycad_ui
    PUBLIC  LyCAD::Core LyCAD::Io LyCAD::Render LyCAD::Interact
            Qt6::Core Qt6::Gui Qt6::Widgets
    # Svg 只用于把图标栅格化，属实现细节，不进接口
    PRIVATE Qt6::Svg LyCAD::Warnings
)
