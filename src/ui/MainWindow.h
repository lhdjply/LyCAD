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
class QLabel;
class QToolButton;

namespace lycad {

class Document;
class CadCanvas;
class ToolManager;
class LayerPanel;
class PropertyPanel;
class CommandLineEdit;

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
    void createToolBars();
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

    // 文件状态
    QString m_lastDirectory;
    WriteOptions m_writeOptions;
    QStringList m_messageLog;
};

} // namespace lycad
