// =============================================================================
//  LyCAD - ui/MainWindow.h
//  主窗口：菜单、工具栏、停靠面板、状态栏、命令行。
//
//  它在架构中的角色是「装配者」而非「实现者」：
//    · 文档、画布、工具管理器、各面板由它创建并互相连接；
//    · 所有业务动作都委托给 Document（可撤销）或 ToolManager（交互命令）；
//    · 它自己不持有任何几何或文件格式逻辑。
//  这样界面的改动被限制在单一文件中，不会渗透到核心逻辑。
// =============================================================================
#pragma once

#include "io/CadCodec.h"

#include <QMainWindow>
#include <QStringList>

class QAction;
class QComboBox;
class QLabel;
class QScrollArea;
class QToolButton;

namespace lycad {

class Document;
class CadCanvas;
class ToolManager;
class LayerPanel;
class PropertyPanel;
class CommandLineEdit;
class RibbonBar;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /// 打开指定文件（供命令行参数与「最近文件」使用）
    bool openFile(const QString& path);

protected:
    void closeEvent(QCloseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    // ---- 构建 ----
    void createActions();
    void createMenus();
    void createRibbon();
    void createDockPanels();
    void createCentralWidget();
    void createStatusBar();
    void connectSignals();

    // ---- 文件 ----
    void newDocument();
    bool openDocument();
    bool saveDocument();
    bool saveDocumentAs();
    bool exportDrawing();
    bool loadFromPath(const QString& path);
    bool writeToPath(const QString& path);
    bool maybeSaveChanges();
    void reportIoResult(const QString& title, const IoResult& result);

    // ---- 界面同步 ----
    void updateWindowTitle();
    void updateStatusModes();
    void updateSelectionDependent();
    void refreshPanels();
    /// 重建功能区里的图层下拉（图层增删后调用）
    void refreshRibbonLayers();
    /// 把功能区的颜色 / 线宽下拉同步到当前选择集
    void refreshRibbonProperties();
    /// 深色主题：功能区外观、图标配色与画布底色一起切换
    void setDarkInterface(bool dark);
    void pushMessage(const QString& text);
    void showCommandHelp();
    void showCommandHistory();

    // ---- 命令 ----
    void runCommand(const QString& command);
    void submitCommandLineText(const QString& text);

    // ---- 数据成员 ----------------------------------------------------------
    Document*       m_document = nullptr;
    CadCanvas*      m_canvas = nullptr;
    ToolManager*    m_toolManager = nullptr;
    LayerPanel*     m_layerPanel = nullptr;
    PropertyPanel*  m_propertyPanel = nullptr;
    CommandLineEdit* m_commandLine = nullptr;

    // 功能区。m_layerCombo 编辑的是「当前图层」，m_colorCombo / m_lineweightCombo
    // 编辑的是当前选择集——前者作用于后续绘制的图元，后两者作用于已有图元，
    // 语义不同，因此分成两组而不是共用一个。
    RibbonBar*      m_ribbon = nullptr;
    /// 功能区的横向滚动容器；它的底色要跟着主题走，否则滚动条一带会露出异色
    QScrollArea*    m_ribbonViewport = nullptr;
    QComboBox*      m_layerCombo = nullptr;
    QComboBox*      m_colorCombo = nullptr;
    QComboBox*      m_lineweightCombo = nullptr;
    QToolButton*    m_darkThemeButton = nullptr;

    // 状态栏
    QLabel* m_coordinateLabel = nullptr;
    QLabel* m_messageLabel = nullptr;
    QLabel* m_scaleLabel = nullptr;
    QLabel* m_countLabel = nullptr;

    QToolButton* m_orthoButton = nullptr;
    QToolButton* m_polarButton = nullptr;
    QToolButton* m_snapButton = nullptr;
    QToolButton* m_gridButton = nullptr;
    QToolButton* m_lineweightButton = nullptr;

    // 动作
    QAction* m_undoAction = nullptr;
    QAction* m_redoAction = nullptr;
    QAction* m_deleteAction = nullptr;
    QAction* m_selectAllAction = nullptr;
    /// 与功能区上的「深色主题」按钮共享同一状态
    QAction* m_darkThemeAction = nullptr;

    // 文件状态
    QString m_lastDirectory;
    WriteOptions m_writeOptions;
    QStringList m_messageLog;
};

} // namespace lycad
