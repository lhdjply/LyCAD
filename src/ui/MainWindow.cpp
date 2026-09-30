// =============================================================================
//  LyCAD - ui/MainWindow.cpp
// =============================================================================
#include "ui/MainWindow.h"

#include "core/Commands.h"
#include "core/Document.h"
#include "core/Entity.h"
#include "core/Format.h"
#include "core/Layer.h"
#include "core/TextEntity.h"
#include "interact/ToolManager.h"
#include "io/CadFile.h"
#include "render/CadCanvas.h"
#include "ui/CommandLineEdit.h"
#include "ui/LayerPanel.h"
#include "ui/PropertyChoices.h"
#include "ui/PropertyPanel.h"
#include "ui/RibbonBar.h"
#include "ui/Theme.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QSettings>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QToolButton>
#include <QUndoStack>
#include <QTextCursor>
#include <QUrl>
#include <QVBoxLayout>

namespace lycad {

namespace {

constexpr int kMaxMessageLog = 500;

/// 可拖入的文件扩展名。
///
/// 刻意使用「已知格式」而非「当前可用格式」：未安装 DWG 后端时，
/// 仍应允许把 .dwg 拖进来，随后由导入流程告知用户缺什么、怎么装；
/// 若在拖入阶段就静默拒绝，用户只能看到「拖拽无反应」。
QStringList droppableExtensions()
{
    return CodecRegistry::instance().knownReadExtensions();
}

/// 让两个开关按钮共享同一份状态。
///
/// 功能区和状态栏上有同名的开关（栅格、正交、捕捉、线宽）——它们表达的是
/// 同一个设置，必须双向同步：只做单向的话，从功能区打开栅格后状态栏仍显示
/// 未按下，用户会以为没生效，然后去状态栏再点一次，反而把它关掉。
void linkToggles(QAbstractButton* first, QAbstractButton* second)
{
    QObject::connect(first, &QAbstractButton::toggled, second, &QAbstractButton::setChecked);
    QObject::connect(second, &QAbstractButton::toggled, first, &QAbstractButton::setChecked);
    first->setChecked(second->isChecked());
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    registerBuiltinCodecs();

    setWindowTitle(QStringLiteral("LyCAD"));
    resize(1280, 820);
    setAcceptDrops(true);

    // ---- 模型与视图对象 ----
    m_document = new Document(this);
    m_canvas = new CadCanvas(this);
    m_canvas->setDocument(m_document);

    m_toolManager = new ToolManager(this);
    m_toolManager->setDocument(m_document);
    m_toolManager->setCanvas(m_canvas);
    m_canvas->setInputHandler(m_toolManager);

    // 恢复上次的界面与显示设置
    QSettings settings;
    m_writeOptions.version = settings.value(QStringLiteral("dxf/version"),
                                            m_writeOptions.version).toString();
    m_lastDirectory = settings.value(QStringLiteral("path/lastDirectory"),
                                     QDir::homePath()).toString();

    // 深色主题覆盖界面与画布两部分，统一由一个设置决定。
    // 旧版本只持久化了画布，因此这里回退读取旧键名，升级后维持原有外观。
    const bool darkTheme = settings.value(QStringLiteral("ui/darkTheme"),
                                          settings.value(QStringLiteral("ui/darkCanvas"), false))
                               .toBool();
    // 调色板必须在创建控件之前落定：Qt 的换肤是「事件通知 + 重绘」，
    // 对构造时就要按主题取色的控件（功能区的样式表、图标配色）来说，
    // 事后补救会留下一次不必要的重绘，还容易漏掉某个自绘控件。
    theme::apply(darkTheme);

    m_canvas->display().showGrid = settings.value(QStringLiteral("ui/showGrid"), true).toBool();

    if (m_canvas->document()) {
        m_toolManager->snapSettings().gridSpacing = m_canvas->display().gridSpacing;
    }

    // ---- 界面 ----
    // 功能区排在状态栏之后构建：其中的视图开关要与状态栏上的同名按钮
    // 互相绑定，需要那些按钮已经存在。
    createActions();
    createMenus();
    createCentralWidget();
    createDockPanels();
    createStatusBar();
    createRibbon();
    connectSignals();

    updateWindowTitle();
    updateStatusModes();
    updateSelectionDependent();
    refreshPanels();
    setDarkInterface(darkTheme);

    m_canvas->setFocus();
}

MainWindow::~MainWindow() = default;

// -----------------------------------------------------------------------------
//  动作
// -----------------------------------------------------------------------------
void MainWindow::createActions()
{
    m_undoAction = new QAction(QStringLiteral("撤销"), this);
    m_undoAction->setShortcut(QKeySequence::Undo);
    connect(m_undoAction, &QAction::triggered, this, [this] {
        m_document->undoStack()->undo();
    });

    m_redoAction = new QAction(QStringLiteral("重做"), this);
    m_redoAction->setShortcut(QKeySequence::Redo);
    connect(m_redoAction, &QAction::triggered, this, [this] {
        m_document->undoStack()->redo();
    });

    m_deleteAction = new QAction(QStringLiteral("删除"), this);
    m_deleteAction->setShortcut(QKeySequence::Delete);
    connect(m_deleteAction, &QAction::triggered, this, [this] { runCommand(QStringLiteral("ERASE")); });

    m_selectAllAction = new QAction(QStringLiteral("全选"), this);
    m_selectAllAction->setShortcut(QKeySequence::SelectAll);
    connect(m_selectAllAction, &QAction::triggered, this, [this] {
        std::vector<EntityId> all;
        for (EntityId id : m_document->drawOrder()) {
            if (m_document->isSelectable(id)) all.push_back(id);
        }
        m_document->selection().setIds(all);
        pushMessage(QStringLiteral("已选择全部 %1 个对象。").arg(all.size()));
    });
}

void MainWindow::createMenus()
{
    // ---- 文件 ----
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("文件(&F)"));

    QAction* newAction = fileMenu->addAction(QStringLiteral("新建(&N)"));
    newAction->setShortcut(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, &MainWindow::newDocument);

    QAction* openAction = fileMenu->addAction(QStringLiteral("打开(&O)…"));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, [this] { openDocument(); });

    fileMenu->addSeparator();

    QAction* saveAction = fileMenu->addAction(QStringLiteral("保存(&S)"));
    saveAction->setShortcut(QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, [this] { saveDocument(); });

    QAction* saveAsAction = fileMenu->addAction(QStringLiteral("另存为(&A)…"));
    saveAsAction->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAction, &QAction::triggered, this, [this] { saveDocumentAs(); });

    QAction* exportAction = fileMenu->addAction(QStringLiteral("导出为其它格式(&E)…"));
    connect(exportAction, &QAction::triggered, this, [this] { exportDrawing(); });

    fileMenu->addSeparator();

    QAction* dxfVersionAction = fileMenu->addAction(QStringLiteral("DXF 目标版本…"));
    connect(dxfVersionAction, &QAction::triggered, this, [this] {
        const QStringList versions = WriteOptions::availableDxfVersions();
        QStringList labels;
        for (const QString& v : versions) labels << WriteOptions::versionDisplayName(v);

        bool ok = false;
        const QString chosen = QInputDialog::getItem(
            this, QStringLiteral("DXF 目标版本"),
            QStringLiteral("保存为 DXF 时使用的版本：\n"
                           "AC1009 (R12) 兼容性最广，AC1015 (2000) 支持 LWPOLYLINE。"),
            labels, static_cast<int>(std::max<qsizetype>(0, versions.indexOf(m_writeOptions.version))),
            false, &ok);
        if (!ok) return;

        const int index = labels.indexOf(chosen);
        if (index >= 0) {
            m_writeOptions.version = versions.at(index);
            QSettings settings;
            settings.setValue(QStringLiteral("dxf/version"), m_writeOptions.version);
            pushMessage(QStringLiteral("DXF 目标版本已设为 %1。")
                            .arg(WriteOptions::versionDisplayName(m_writeOptions.version)));
        }
    });

    fileMenu->addSeparator();
    QAction* quitAction = fileMenu->addAction(QStringLiteral("退出(&Q)"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    // ---- 编辑 ----
    QMenu* editMenu = menuBar()->addMenu(QStringLiteral("编辑(&E)"));
    editMenu->addAction(m_undoAction);
    editMenu->addAction(m_redoAction);
    editMenu->addSeparator();
    editMenu->addAction(m_deleteAction);
    editMenu->addAction(m_selectAllAction);
    editMenu->addSeparator();

    QAction* modifyMenuAction = editMenu->addAction(QStringLiteral("移动(&M)"));
    connect(modifyMenuAction, &QAction::triggered, this, [this] { runCommand(QStringLiteral("MOVE")); });
    QAction* copyAct = editMenu->addAction(QStringLiteral("复制(&C)"));
    connect(copyAct, &QAction::triggered, this, [this] { runCommand(QStringLiteral("COPY")); });
    QAction* rotateAct = editMenu->addAction(QStringLiteral("旋转(&R)"));
    connect(rotateAct, &QAction::triggered, this, [this] { runCommand(QStringLiteral("ROTATE")); });
    QAction* scaleAct = editMenu->addAction(QStringLiteral("缩放(&S)"));
    connect(scaleAct, &QAction::triggered, this, [this] { runCommand(QStringLiteral("SCALE")); });
    QAction* mirrorAct = editMenu->addAction(QStringLiteral("镜像(&I)"));
    connect(mirrorAct, &QAction::triggered, this, [this] { runCommand(QStringLiteral("MIRROR")); });

    // ---- 绘图 ----
    QMenu* drawMenu = menuBar()->addMenu(QStringLiteral("绘图(&D)"));
    struct DrawEntry { const char* label; const char* command; };
    static const DrawEntry kDrawEntries[] = {
        { "直线(&L)", "LINE" },
        { "多段线(&P)", "PLINE" },
        { "圆(&C)", "CIRCLE" },
        { "圆弧(&A)", "ARC" },
        { "矩形(&R)", "RECTANGLE" },
        { "点(&O)", "POINT" },
        { nullptr, "" },                    // 分隔线
        { "文字(&T)", "TEXT" },
        { "填充(&H)", "HATCH" },
        { nullptr, "" },
        { "对齐标注(&G)", "DIMALIGNED" },
        { "线性标注(&N)", "DIMLINEAR" },
        { "半径标注(&U)", "DIMRADIUS" },
        { "直径标注(&M)", "DIMDIAMETER" },
    };
    for (const DrawEntry& entry : kDrawEntries) {
        if (!entry.label) {
            drawMenu->addSeparator();
            continue;
        }
        QAction* action = drawMenu->addAction(QString::fromUtf8(entry.label));
        const QString command = QString::fromLatin1(entry.command);
        connect(action, &QAction::triggered, this, [this, command] { runCommand(command); });
    }

    // ---- 视图 ----
    QMenu* viewMenu = menuBar()->addMenu(QStringLiteral("视图(&V)"));

    QAction* zoomExtentsAction = viewMenu->addAction(QStringLiteral("缩放至图形范围(&E)"));
    connect(zoomExtentsAction, &QAction::triggered, m_canvas, &CadCanvas::zoomExtents);

    QAction* zoomSelectionAction = viewMenu->addAction(QStringLiteral("缩放至选择集(&S)"));
    connect(zoomSelectionAction, &QAction::triggered, m_canvas, &CadCanvas::zoomToSelection);

    QAction* zoomInAction = viewMenu->addAction(QStringLiteral("放大(&I)"));
    zoomInAction->setShortcut(QKeySequence::ZoomIn);
    connect(zoomInAction, &QAction::triggered, m_canvas, &CadCanvas::zoomIn);

    QAction* zoomOutAction = viewMenu->addAction(QStringLiteral("缩小(&O)"));
    zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAction, &QAction::triggered, m_canvas, &CadCanvas::zoomOut);

    viewMenu->addSeparator();

    QAction* gridAction = viewMenu->addAction(QStringLiteral("显示网格(&G)"));
    gridAction->setCheckable(true);
    gridAction->setChecked(m_canvas->display().showGrid);
    gridAction->setShortcut(Qt::Key_F7);
    connect(gridAction, &QAction::toggled, this, [this](bool on) {
        m_canvas->display().showGrid = on;
        QSettings()
            .setValue(QStringLiteral("ui/showGrid"), on);
        m_canvas->update();
    });

    QAction* lineweightAction = viewMenu->addAction(QStringLiteral("显示线宽(&W)"));
    lineweightAction->setCheckable(true);
    lineweightAction->setChecked(m_canvas->display().showLineweight);
    connect(lineweightAction, &QAction::toggled, this, [this](bool on) {
        m_canvas->display().showLineweight = on;
        m_canvas->update();
    });

    // 「深色主题」同时切换功能区外观与画布底色，取代原先只管画布的
    // 「深色画布」——两个各自为政的深色开关只会让人猜哪个该按。
    m_darkThemeAction = viewMenu->addAction(QStringLiteral("深色主题(&D)"));
    m_darkThemeAction->setCheckable(true);
    m_darkThemeAction->setChecked(m_canvas->display().themeIsDark);
    connect(m_darkThemeAction, &QAction::toggled, this, &MainWindow::setDarkInterface);

    // ---- 图层 ----
    QMenu* layerMenu = menuBar()->addMenu(QStringLiteral("图层(&L)"));
    QAction* newLayerAction = layerMenu->addAction(QStringLiteral("新建图层(&N)…"));
    connect(newLayerAction, &QAction::triggered, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("新建图层"),
                                                   QStringLiteral("图层名称:"),
                                                   QLineEdit::Normal, QString(), &ok);
        if (!ok || name.trimmed().isEmpty()) return;

        QString error;
        if (!m_document->addLayer(Layer(name.trimmed()), &error)) {
            QMessageBox::warning(this, QStringLiteral("新建图层"), error);
            return;
        }
        pushMessage(QStringLiteral("已新建图层 \"%1\"。").arg(name.trimmed()));
    });

    QAction* layerManagerAction = layerMenu->addAction(QStringLiteral("图层管理器(&M)"));
    connect(layerManagerAction, &QAction::triggered, this, [this] {
        if (m_layerPanel && m_layerPanel->parentWidget()) {
            static_cast<QDockWidget*>(m_layerPanel->parentWidget())->show();
            static_cast<QDockWidget*>(m_layerPanel->parentWidget())->raise();
        }
    });

    // ---- 帮助 ----
    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("帮助(&H)"));

    QAction* cmdHelpAction = helpMenu->addAction(QStringLiteral("命令列表(&C)…"));
    cmdHelpAction->setShortcut(Qt::Key_F1);
    connect(cmdHelpAction, &QAction::triggered, this, &MainWindow::showCommandHelp);

    QAction* historyAction = helpMenu->addAction(QStringLiteral("命令历史(&H)…"));
    connect(historyAction, &QAction::triggered, this, &MainWindow::showCommandHistory);

    helpMenu->addSeparator();
    QAction* aboutAction = helpMenu->addAction(QStringLiteral("关于 LyCAD(&A)…"));
    connect(aboutAction, &QAction::triggered, this, [this] {
        QStringList backends = CodecRegistry::instance().supportedReadExtensions();
        QString dwgNote;
        if (ICadCodec* dwg = CodecRegistry::instance().codecByName(QStringLiteral("DWG"))) {
            dwgNote = dwg->canRead() ? QStringLiteral("DWG 读写：可用")
                                     : QStringLiteral("DWG 读写：不可用（未检测到转换后端）");
        }

        QMessageBox::about(
            this, QStringLiteral("关于 LyCAD"),
            QStringLiteral(
                "<h3>LyCAD</h3>"
                "<p>基于 C++20 与 Qt6 构建的二维 CAD 绘图软件。</p>"
                "<p><b>模块结构</b><br>"
                "core（几何模型） · io（文件解析） · render（渲染显示） · "
                "interact（用户交互） · ui（界面）</p>"
                "<p><b>支持格式</b><br>DXF 读写（R12 / R2000，ASCII 与二进制）<br>%1</p>"
                "<p><b>可用文件类型</b>：%2</p>"
                "<p style='color:#888'>渲染后端：QPainter 光栅；"
                "撤销模型：QUndoCommand 快照式。</p>")
                .arg(dwgNote, backends.join(QStringLiteral(", ")).toUpper()));
    });
}

void MainWindow::createRibbon()
{
    // 宿主 QToolBar 只是个容器。只有放进工具栏区域，功能区才能横跨整个窗口
    // 宽度（包括左右停靠面板之上），这正是 AutoCAD 的布局；若放进中央控件，
    // 左右两侧会被停靠面板切掉。容器自身的边框、间距、拖动手柄全部去掉，
    // 免得在功能区外面再套一层可见框架。
    auto* host = new QToolBar(QStringLiteral("功能区"), this);
    host->setObjectName(QStringLiteral("toolbar.ribbon"));
    host->setMovable(false);
    host->setFloatable(false);
    host->setIconSize(QSize(RibbonPanel::kLargeIcon, RibbonPanel::kLargeIcon));
    // 容器只负责摆放，不留自己的边框与间距——它里面就是功能区本身。
    // 这里用边距设置而不是样式表：给容器挂样式表会让整棵子树改用
    // QStyleSheetStyle，那会破坏调色板的继承（见 Theme 模块的说明）。
    if (host->layout()) {
        host->layout()->setContentsMargins(0, 0, 0, 0);
        host->layout()->setSpacing(0);
    }
    host->setContentsMargins(0, 0, 0, 0);

    // 面板排开的总宽度明显大于窗口。若把 RibbonBar 直接放进工具栏区域，
    // 它会把窗口的最小宽度顶到一千八百多像素——用户就再也无法把窗口调窄，
    // 窄窗口下按钮上的文字还会被挤成省略号。因此用一层横向滚动区包住：
    // 窗口够宽时看不出区别，变窄时出现横向滚动条，面板本身保持完整。
    auto* viewport = new QScrollArea(host);
    viewport->setObjectName(QStringLiteral("ribbonViewport"));
    viewport->setFrameShape(QFrame::NoFrame);
    viewport->setWidgetResizable(true);
    viewport->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    viewport->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    viewport->setFocusPolicy(Qt::NoFocus);
    // 视口自身不填背景，露出滚动区的底色（由主题设置，见 setDarkInterface）
    viewport->viewport()->setAutoFillBackground(false);
    m_ribbonViewport = viewport;

    m_ribbon = new RibbonBar(viewport);
    viewport->setWidget(m_ribbon);
    host->addWidget(viewport);
    addToolBar(Qt::TopToolBarArea, host);

    // 面板里所有命令型按钮都汇到这一个信号，接到命令行执行上——
    // 相当于功能区只是命令行的一个图形外壳，二者行为必然一致。
    connect(m_ribbon, &RibbonBar::commandTriggered, this, &MainWindow::runCommand);

    // ---- 文件 ----
    {
        RibbonPanel* panel = m_ribbon->addPanel(QStringLiteral("文件"));
        connect(panel->addLarge(QStringLiteral("new"), QStringLiteral("新建"),
                                QStringLiteral("新建图形 (Ctrl+N)")),
                &QToolButton::clicked, this, &MainWindow::newDocument);
        connect(panel->addLarge(QStringLiteral("open"), QStringLiteral("打开"),
                                QStringLiteral("打开图形 (Ctrl+O)")),
                &QToolButton::clicked, this, &MainWindow::openDocument);
        connect(panel->addLarge(QStringLiteral("save"), QStringLiteral("保存"),
                                QStringLiteral("保存图形 (Ctrl+S)")),
                &QToolButton::clicked, this, &MainWindow::saveDocument);

        // 撤销 / 重做接到 QAction 上而不是自己实现：快捷键、菜单项、
        // 可用状态三处从此只有一份来源。
        QToolButton* undoButton = panel->addSmall(QStringLiteral("undo"), QStringLiteral("撤销"),
                                                  QStringLiteral("撤销 (Ctrl+Z)"));
        connect(undoButton, &QToolButton::clicked, m_undoAction, &QAction::trigger);
        connect(m_undoAction, &QAction::enabledChanged, undoButton, &QWidget::setEnabled);

        QToolButton* redoButton = panel->addSmall(QStringLiteral("redo"), QStringLiteral("重做"),
                                                  QStringLiteral("重做 (Ctrl+Y)"));
        connect(redoButton, &QToolButton::clicked, m_redoAction, &QAction::trigger);
        connect(m_redoAction, &QAction::enabledChanged, redoButton, &QWidget::setEnabled);
    }

    // ---- 绘图 ----
    {
        RibbonPanel* panel = m_ribbon->addPanel(QStringLiteral("绘图"));
        panel->addLargeCommand(QStringLiteral("line"), QStringLiteral("直线"),
                               QStringLiteral("LINE"), QStringLiteral("直线 (LINE / L)"));
        panel->addLargeCommand(QStringLiteral("polyline"), QStringLiteral("多段线"),
                               QStringLiteral("PLINE"), QStringLiteral("多段线 (PLINE / PL)"));
        panel->addLargeCommand(QStringLiteral("circle"), QStringLiteral("圆"),
                               QStringLiteral("CIRCLE"), QStringLiteral("圆 (CIRCLE / C)"));
        panel->addLargeCommand(QStringLiteral("arc"), QStringLiteral("圆弧"),
                               QStringLiteral("ARC"), QStringLiteral("圆弧 (ARC / A)"));
        panel->addSeparator();
        panel->addSmallCommand(QStringLiteral("rectangle"), QStringLiteral("矩形"),
                               QStringLiteral("RECTANGLE"), QStringLiteral("矩形 (RECTANGLE / REC)"));
        panel->addSmallCommand(QStringLiteral("point"), QStringLiteral("点"),
                               QStringLiteral("POINT"), QStringLiteral("点 (POINT / PO)"));
        panel->addSeparator();
        panel->addSmallCommand(QStringLiteral("hatch"), QStringLiteral("填充"),
                               QStringLiteral("HATCH"), QStringLiteral("图案填充 (HATCH / H)"));
    }

    // ---- 修改 ----
    {
        RibbonPanel* panel = m_ribbon->addPanel(QStringLiteral("修改"));
        panel->addLargeCommand(QStringLiteral("move"), QStringLiteral("移动"),
                               QStringLiteral("MOVE"), QStringLiteral("移动 (MOVE / M)"));
        panel->addLargeCommand(QStringLiteral("copy"), QStringLiteral("复制"),
                               QStringLiteral("COPY"), QStringLiteral("复制 (COPY / CO)"));
        panel->addSeparator();
        panel->addSmallCommand(QStringLiteral("rotate"), QStringLiteral("旋转"),
                               QStringLiteral("ROTATE"), QStringLiteral("旋转 (ROTATE / RO)"));
        panel->addSmallCommand(QStringLiteral("scale"), QStringLiteral("缩放"),
                               QStringLiteral("SCALE"), QStringLiteral("缩放 (SCALE / SC)"));
        panel->addSmallCommand(QStringLiteral("mirror"), QStringLiteral("镜像"),
                               QStringLiteral("MIRROR"), QStringLiteral("镜像 (MIRROR / MI)"));
        panel->addSeparator();
        panel->addSmallCommand(QStringLiteral("erase"), QStringLiteral("删除"),
                               QStringLiteral("ERASE"), QStringLiteral("删除 (ERASE / E)"));
        panel->addSmallCommand(QStringLiteral("select-all"), QStringLiteral("全选"),
                               QStringLiteral("SELECT"), QStringLiteral("选择全部对象 (SELECT)"));
    }

    // ---- 注释 ----
    {
        RibbonPanel* panel = m_ribbon->addPanel(QStringLiteral("注释"));
        panel->addLargeCommand(QStringLiteral("text"), QStringLiteral("文字"),
                               QStringLiteral("TEXT"), QStringLiteral("单行文字 (TEXT / DT)"));
        panel->addLargeCommand(QStringLiteral("dim-linear"), QStringLiteral("线性标注"),
                               QStringLiteral("DIMLINEAR"),
                               QStringLiteral("线性标注 (DIMLINEAR / DLI)"));
        panel->addLargeCommand(QStringLiteral("dim-aligned"), QStringLiteral("对齐标注"),
                               QStringLiteral("DIMALIGNED"),
                               QStringLiteral("对齐标注 (DIMALIGNED / DAL)"));
        panel->addSeparator();
        panel->addSmallCommand(QStringLiteral("dim-radius"), QStringLiteral("半径标注"),
                               QStringLiteral("DIMRADIUS"),
                               QStringLiteral("半径标注 (DIMRADIUS / DRA)"));
        panel->addSmallCommand(QStringLiteral("dim-diameter"), QStringLiteral("直径标注"),
                               QStringLiteral("DIMDIAMETER"),
                               QStringLiteral("直径标注 (DIMDIAMETER / DDI)"));
    }

    // ---- 图层 ----
    {
        RibbonPanel* panel = m_ribbon->addPanel(QStringLiteral("图层"));
        m_layerCombo = new QComboBox(panel);
        m_layerCombo->setMinimumWidth(170);
        m_layerCombo->setToolTip(QStringLiteral("当前图层——新建的图元归属于它"));
        // 用 activated 而不是 currentIndexChanged：后者在程序同步选择时也会发出，
        // 会把「刷新界面」误当成用户改图层。
        connect(m_layerCombo, &QComboBox::activated, this, [this](int index) {
            if (index < 0) return;
            const QString name = m_layerCombo->itemText(index);
            if (m_document->setCurrentLayer(name)) {
                pushMessage(QStringLiteral("当前图层：%1").arg(name));
            }
        });
        panel->addWidget(m_layerCombo);
    }

    // ---- 特性 ----
    {
        RibbonPanel* panel = m_ribbon->addPanel(QStringLiteral("特性"));

        m_colorCombo = new QComboBox(panel);
        for (int aci : propertyChoices::colors()) {
            // 注意 Qt 的带图标重载是「图标在前、文字在后」
            m_colorCombo->addItem(propertyChoices::colorSwatch(aci),
                                  propertyChoices::colorText(aci), QVariant(aci));
        }
        m_colorCombo->setMinimumWidth(104);
        m_colorCombo->setToolTip(QStringLiteral("把选中对象的颜色改为指定值"));
        connect(m_colorCombo, &QComboBox::activated, this, [this](int index) {
            if (index < 0 || m_document->selection().isEmpty()) return;
            const int aci = m_colorCombo->itemData(index).toInt();
            const std::vector<EntityId> ids = m_document->selection().ids();
            if (m_document->setEntitiesColor(ids, aci)) {
                pushMessage(QStringLiteral("已将 %1 个对象的颜色改为 %2。")
                                .arg(ids.size())
                                .arg(propertyChoices::colorText(aci)));
            }
        });
        panel->addWidget(m_colorCombo);

        m_lineweightCombo = new QComboBox(panel);
        for (int value : propertyChoices::lineweights()) {
            m_lineweightCombo->addItem(propertyChoices::lineweightText(value), value);
        }
        m_lineweightCombo->setMinimumWidth(104);
        m_lineweightCombo->setToolTip(QStringLiteral("把选中对象的线宽改为指定值"));
        connect(m_lineweightCombo, &QComboBox::activated, this, [this](int index) {
            if (index < 0 || m_document->selection().isEmpty()) return;
            const int value = m_lineweightCombo->itemData(index).toInt();
            const std::vector<EntityId> ids = m_document->selection().ids();
            if (m_document->setEntitiesLineweight(ids, value)) {
                pushMessage(QStringLiteral("已将 %1 个对象的线宽改为 %2。")
                                .arg(ids.size())
                                .arg(propertyChoices::lineweightText(value)));
            }
        });
        panel->addWidget(m_lineweightCombo);
    }

    // ---- 视图 ----
    {
        RibbonPanel* panel = m_ribbon->addPanel(QStringLiteral("视图"));
        connect(panel->addLarge(QStringLiteral("zoom-extents"), QStringLiteral("缩放范围"),
                                QStringLiteral("缩放到图形范围")),
                &QToolButton::clicked, m_canvas, &CadCanvas::zoomExtents);
        connect(panel->addLarge(QStringLiteral("zoom-selection"), QStringLiteral("缩放选择集"),
                                QStringLiteral("缩放到当前选择集")),
                &QToolButton::clicked, m_canvas, &CadCanvas::zoomToSelection);
        panel->addSeparator();

        // 与状态栏上的同名开关共享状态，见 linkToggles 的说明
        struct ToggleEntry {
            const char* icon;
            const char* label;
            const char* tip;
            QToolButton* statusBarButton;
        };
        const ToggleEntry toggles[] = {
            { "grid",       "栅格", "栅格吸附 (F9)",       m_gridButton },
            { "ortho",      "正交", "正交模式 (F8)",       m_orthoButton },
            { "snap",       "捕捉", "对象捕捉 (F3)",       m_snapButton },
            { "lineweight", "线宽", "按真实线宽显示",       m_lineweightButton },
        };
        for (const ToggleEntry& entry : toggles) {
            QToolButton* button = panel->addSmall(QString::fromLatin1(entry.icon),
                                                  QString::fromUtf8(entry.label),
                                                  QString::fromUtf8(entry.tip));
            button->setCheckable(true);
            if (entry.statusBarButton) linkToggles(button, entry.statusBarButton);
        }

        QToolButton* themeButton = panel->addSmall(QStringLiteral("theme-dark"),
                                                   QStringLiteral("深色主题"),
                                                   QStringLiteral("切换深色 / 浅色界面"));
        themeButton->setCheckable(true);
        connect(themeButton, &QToolButton::toggled, this, &MainWindow::setDarkInterface);
        m_darkThemeButton = themeButton;
    }

    // ---- 帮助 ----
    {
        RibbonPanel* panel = m_ribbon->addPanel(QStringLiteral("帮助"));
        connect(panel->addLarge(QStringLiteral("help"), QStringLiteral("命令列表"),
                                QStringLiteral("查看全部可用命令与快捷键 (F1)")),
                &QToolButton::clicked, this, &MainWindow::showCommandHelp);
    }

    // 滚动条一旦出现就会占用十几像素高度，而这部分 sizeHint 是算不出来的
    // （按需显示的滚动条在布局时还不存在）。不计入的话，滚动条会从功能区内部
    // 「吃掉」这段高度，把最下面的面板标题裁掉。这里显式补上——注意必须等
    // 面板全部建完再算，否则量到的是空功能区的尺寸。
    const int scrollbarExtent = viewport->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    viewport->setFixedHeight(m_ribbon->sizeHint().height() + scrollbarExtent);

    // 撤销/重做的可用状态由 updateSelectionDependent 维护，
    // 这里补一次初始同步，免得刚启动时两个按钮看起来是可点的。
    updateSelectionDependent();
}

void MainWindow::createCentralWidget()
{
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    layout->addWidget(m_canvas, 1);

    m_commandLine = new CommandLineEdit(central);
    m_commandLine->setCompletionSource(m_toolManager->commandNames());
    layout->addWidget(m_commandLine);

    setCentralWidget(central);
    m_canvas->setFocus();
}

void MainWindow::createDockPanels()
{
    m_layerPanel = new LayerPanel(this);
    auto* layerDock = new QDockWidget(QStringLiteral("图层"), this);
    layerDock->setObjectName(QStringLiteral("dock.layers"));
    layerDock->setWidget(m_layerPanel);
    layerDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::LeftDockWidgetArea, layerDock);

    m_propertyPanel = new PropertyPanel(this);
    auto* propertyDock = new QDockWidget(QStringLiteral("属性"), this);
    propertyDock->setObjectName(QStringLiteral("dock.properties"));
    propertyDock->setWidget(m_propertyPanel);
    propertyDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, propertyDock);

    resizeDocks({ layerDock, propertyDock }, { 300, 320 }, Qt::Horizontal);
}

void MainWindow::createStatusBar()
{
    m_messageLabel = new QLabel(this);
    m_messageLabel->setMinimumWidth(320);

    m_orthoButton = new QToolButton(this);
    m_orthoButton->setText(QStringLiteral("正交"));
    m_orthoButton->setCheckable(true);
    m_orthoButton->setToolTip(QStringLiteral("正交模式 (F8)"));

    m_polarButton = new QToolButton(this);
    m_polarButton->setText(QStringLiteral("极轴"));
    m_polarButton->setCheckable(true);
    m_polarButton->setToolTip(QStringLiteral("极轴追踪 (F10)"));

    m_snapButton = new QToolButton(this);
    m_snapButton->setText(QStringLiteral("捕捉"));
    m_snapButton->setCheckable(true);
    m_snapButton->setChecked(true);
    m_snapButton->setToolTip(QStringLiteral("对象捕捉 (F3)"));

    m_gridButton = new QToolButton(this);
    m_gridButton->setText(QStringLiteral("栅格"));
    m_gridButton->setCheckable(true);
    m_gridButton->setToolTip(QStringLiteral("栅格吸附 (F9)"));

    m_lineweightButton = new QToolButton(this);
    m_lineweightButton->setText(QStringLiteral("线宽"));
    m_lineweightButton->setCheckable(true);
    m_lineweightButton->setChecked(m_canvas->display().showLineweight);
    m_lineweightButton->setToolTip(QStringLiteral("按真实线宽显示"));

    m_scaleLabel = new QLabel(this);
    m_countLabel = new QLabel(this);
    m_coordinateLabel = new QLabel(this);
    m_coordinateLabel->setMinimumWidth(190);

    statusBar()->addWidget(m_messageLabel, 1);
    statusBar()->addPermanentWidget(m_coordinateLabel);
    statusBar()->addPermanentWidget(m_scaleLabel);
    statusBar()->addPermanentWidget(m_countLabel);
    statusBar()->addPermanentWidget(m_orthoButton);
    statusBar()->addPermanentWidget(m_polarButton);
    statusBar()->addPermanentWidget(m_snapButton);
    statusBar()->addPermanentWidget(m_gridButton);
    statusBar()->addPermanentWidget(m_lineweightButton);

    connect(m_orthoButton, &QToolButton::toggled, this, [this](bool on) {
        m_toolManager->setOrthoMode(on);
        updateStatusModes();
    });
    connect(m_polarButton, &QToolButton::toggled, this, [this](bool on) {
        m_toolManager->setPolarTracking(on);
        updateStatusModes();
    });
    connect(m_snapButton, &QToolButton::toggled, this, [this](bool on) {
        m_toolManager->snapSettings().enabled = on;
        pushMessage(on ? QStringLiteral("对象捕捉：开") : QStringLiteral("对象捕捉：关"));
    });
    connect(m_gridButton, &QToolButton::toggled, this, [this](bool on) {
        m_toolManager->setGridSnap(on);
    });
    connect(m_lineweightButton, &QToolButton::toggled, this, [this](bool on) {
        m_canvas->display().showLineweight = on;
        m_canvas->update();
    });

    m_coordinateLabel->setText(QStringLiteral("X: --   Y: --"));
}

// -----------------------------------------------------------------------------
//  信号连接
// -----------------------------------------------------------------------------
void MainWindow::connectSignals()
{
    // ---- 文档 -> 界面 ----
    connect(m_document, &Document::modificationChanged, this, [this] {
        updateWindowTitle();
        updateSelectionDependent();
    });
    connect(m_document, &Document::filePathChanged, this, [this] { updateWindowTitle(); });
    connect(m_document, &Document::documentReset, this, [this] {
        updateWindowTitle();
        refreshPanels();
        updateSelectionDependent();
    });
    connect(m_document, &Document::layersChanged, this, [this] { refreshPanels(); });
    connect(m_document, &Document::selectionChanged, this, [this] {
        refreshPanels();
        updateSelectionDependent();
    });
    // 增删图元时除了重绘，还必须刷新状态栏计数与撤销/重做按钮的可用状态。
    // 这些信息不能只依赖 modificationChanged——「已修改」标记只在
    // 干净↔脏 的边界翻转一次，连续新增多个图元时不会重复触发。
    connect(m_document, &Document::entityAdded, this, [this] {
        m_canvas->update();
        updateSelectionDependent();
    });
    connect(m_document, &Document::entityRemoved, this, [this] {
        m_canvas->update();
        updateSelectionDependent();
    });
    connect(m_document, &Document::entityChanged, this, [this] { m_canvas->update(); });

    // ---- 画布 -> 界面 ----
    connect(m_canvas, &CadCanvas::mouseWorldPositionChanged, this,
            [this](const Point2D& world, const Point2D&) {
                m_coordinateLabel->setText(QStringLiteral("X: %1   Y: %2")
                                               .arg(formatLength(world.x, 4), formatLength(world.y, 4)));
            });
    connect(m_canvas, &CadCanvas::cursorLeftCanvas, this, [this] {
        m_coordinateLabel->setText(QStringLiteral("X: --   Y: --"));
    });
    connect(m_canvas, &CadCanvas::viewChanged, this, [this] {
        const double scale = m_canvas->view().scale();
        m_scaleLabel->setText(QStringLiteral("缩放 1:%1").arg(1.0 / scale, 0, 'g', 3));
    });

    connect(m_canvas, &CadCanvas::contextMenuRequested, this, [this](const QPoint& globalPos) {
        QMenu menu(this);

        if (m_toolManager->isCommandRunning()) {
            menu.addAction(QStringLiteral("取消当前命令"), this, [this] { m_toolManager->cancel(); });
            menu.addSeparator();
        } else if (!m_toolManager->lastCommandName().isEmpty()) {
            menu.addAction(QStringLiteral("重复 %1").arg(m_toolManager->lastCommandName()),
                           this, [this] { m_toolManager->repeatLastCommand(); });
            menu.addSeparator();
        }

        menu.addAction(QStringLiteral("全选"), this, [this] { m_selectAllAction->trigger(); });
        menu.addAction(QStringLiteral("删除所选"), this, [this] {
            if (!m_document->selection().isEmpty()) runCommand(QStringLiteral("ERASE"));
        });
        menu.addSeparator();
        menu.addAction(QStringLiteral("缩放至图形范围"), m_canvas, &CadCanvas::zoomExtents);
        menu.addAction(QStringLiteral("缩放至选择集"), m_canvas, &CadCanvas::zoomToSelection);

        menu.exec(globalPos);
    });

    // ---- 工具管理器 -> 命令行 ----
    connect(m_toolManager, &ToolManager::promptChanged, this, [this](const QString& prompt) {
        m_commandLine->setPrompt(prompt);
    });
    connect(m_toolManager, &ToolManager::commandMessage, this, [this](const QString& text) {
        pushMessage(text);
    });
    connect(m_toolManager, &ToolManager::commandStarted, this, [this](const QString&) {
        m_canvas->setFocus();
    });
    connect(m_toolManager, &ToolManager::commandEnded, this, [this](const QString&) {
        refreshPanels();
    });

    // ---- 双击文字 -> 就地编辑 ----
    connect(m_toolManager, &ToolManager::textEditRequested, this, [this](EntityId id) {
        Entity* entity = m_document->entity(id);
        if (!entity || entity->type() != EntityType::Text) return;
        auto* text = static_cast<TextEntity*>(entity);

        bool ok = false;
        const QString input = QInputDialog::getText(
            this, QStringLiteral("编辑文字"),
            QStringLiteral("内容（字高/旋转等几何属性在属性面板修改）:"),
            QLineEdit::Normal, text->content(), &ok);
        if (!ok) return;

        const QString newContent = input.trimmed();
        if (newContent.isEmpty()) {
            pushMessage(QStringLiteral("文字内容不能为空，编辑已取消。"));
            return;
        }
        if (newContent == text->content()) return;   // 未变化不产生撤销记录

        auto replacement = text->clone();
        static_cast<TextEntity*>(replacement.get())->setContent(newContent);
        if (m_document->replaceEntity(id, std::move(replacement),
                                      QStringLiteral("编辑文字"))) {
            pushMessage(QStringLiteral("已编辑文字。"));
            refreshPanels();
        }
    });

    // ---- 命令行 ----
    connect(m_commandLine, &CommandLineEdit::submitted, this, &MainWindow::submitCommandLineText);
    connect(m_commandLine, &CommandLineEdit::escapePressed, this, [this] {
        m_toolManager->cancel();
        m_canvas->setFocus();
    });
    connect(m_commandLine, &CommandLineEdit::emptySubmitted, this, [this] {
        if (m_toolManager->isCommandRunning()) {
            m_toolManager->cancel();
        } else {
            m_toolManager->repeatLastCommand();
        }
        m_canvas->setFocus();
    });

    // ---- 面板 -> 文档 ----
    connect(m_layerPanel, &LayerPanel::currentLayerChangeRequested, this,
            [this](const QString& name) { m_document->setCurrentLayer(name); });

    connect(m_layerPanel, &LayerPanel::addLayerRequested, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("新建图层"),
                                                   QStringLiteral("图层名称:"),
                                                   QLineEdit::Normal, QString(), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        QString error;
        if (!m_document->addLayer(Layer(name.trimmed()), &error)) {
            QMessageBox::warning(this, QStringLiteral("新建图层"), error);
        }
    });

    connect(m_layerPanel, &LayerPanel::removeLayerRequested, this, [this](const QString& name) {
        QString error;
        if (!m_document->removeLayer(name, true, &error)) {
            QMessageBox::warning(this, QStringLiteral("删除图层"), error);
        } else {
            pushMessage(QStringLiteral("已删除图层 \"%1\"。").arg(name));
        }
    });

    connect(m_layerPanel, &LayerPanel::renameLayerRequested, this,
            [this](const QString& oldName, const QString& newName) {
                QString error;
                if (!m_document->renameLayer(oldName, newName, &error)) {
                    QMessageBox::warning(this, QStringLiteral("重命名图层"), error);
                }
            });

    connect(m_layerPanel, &LayerPanel::layerVisibilityChangeRequested, this,
            [this](const QString& name, bool visible) {
                m_document->setLayerVisible(name, visible);
            });
    connect(m_layerPanel, &LayerPanel::layerFrozenChangeRequested, this,
            [this](const QString& name, bool frozen) {
                m_document->setLayerFrozen(name, frozen);
            });
    connect(m_layerPanel, &LayerPanel::layerLockedChangeRequested, this,
            [this](const QString& name, bool locked) {
                m_document->setLayerLocked(name, locked);
            });
    connect(m_layerPanel, &LayerPanel::layerColorChangeRequested, this,
            [this](const QString& name, int aci) {
                m_document->setLayerColor(name, aci);
            });
    connect(m_layerPanel, &LayerPanel::layerLineweightChangeRequested, this,
            [this](const QString& name, int dxfValue) {
                m_document->setLayerLineweight(name, dxfValue);
            });

    // ---- 属性面板 -> 文档 ----
    connect(m_propertyPanel, &PropertyPanel::layerChangeRequested, this,
            [this](const QString& layerName) {
                const std::vector<EntityId> ids = m_document->selection().ids();
                if (!m_document->setEntitiesLayer(ids, layerName)) {
                    pushMessage(QStringLiteral("无法把对象移动到图层 \"%1\"。").arg(layerName));
                }
            });
    connect(m_propertyPanel, &PropertyPanel::colorChangeRequested, this, [this](int aci) {
        m_document->setEntitiesColor(m_document->selection().ids(), aci);
    });
    connect(m_propertyPanel, &PropertyPanel::lineweightChangeRequested, this, [this](int value) {
        m_document->setEntitiesLineweight(m_document->selection().ids(), value);
    });
    connect(m_propertyPanel, &PropertyPanel::visibleChangeRequested, this, [this](bool visible) {
        m_document->setEntitiesVisible(m_document->selection().ids(), visible);
    });
}

// -----------------------------------------------------------------------------
//  文件操作
// -----------------------------------------------------------------------------
void MainWindow::newDocument()
{
    if (!maybeSaveChanges()) return;

    m_document->clear(true);
    m_document->setFilePath(QString());
    m_canvas->view().reset();
    m_canvas->zoomExtents();
    pushMessage(QStringLiteral("已新建图纸。"));
    updateWindowTitle();
    refreshPanels();
}

bool MainWindow::openDocument()
{
    if (!maybeSaveChanges()) return false;

    const QString filter = CodecRegistry::instance().importFilters().join(QStringLiteral(";;"));
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开图形文件"), m_lastDirectory, filter);
    if (path.isEmpty()) return false;

    return loadFromPath(path);
}

bool MainWindow::loadFromPath(const QString& path)
{
    QApplication::setOverrideCursor(Qt::WaitCursor);

    IoResult result = CadFile::load(path, *m_document);

    QApplication::restoreOverrideCursor();

    reportIoResult(QStringLiteral("打开文件"), result);
    if (!result.ok) {
        updateWindowTitle();
        refreshPanels();
        return false;
    }

    m_lastDirectory = QFileInfo(path).absolutePath();
    QSettings()
        .setValue(QStringLiteral("path/lastDirectory"), m_lastDirectory);

    m_canvas->zoomExtents();
    updateWindowTitle();
    refreshPanels();
    updateSelectionDependent();

    pushMessage(QStringLiteral("已打开 %1：%2")
                    .arg(QFileInfo(path).fileName())
                    .arg(result.summary()));
    return true;
}

bool MainWindow::openFile(const QString& path)
{
    if (!QFileInfo::exists(path)) {
        QMessageBox::warning(this, QStringLiteral("打开文件"),
                             QStringLiteral("文件不存在：%1").arg(path));
        return false;
    }
    return loadFromPath(path);
}

bool MainWindow::saveDocument()
{
    if (m_document->filePath().isEmpty()) return saveDocumentAs();
    return writeToPath(m_document->filePath());
}

bool MainWindow::saveDocumentAs()
{
    const QString filter = CodecRegistry::instance().exportFilters().join(QStringLiteral(";;"));
    const QString suggested = m_document->filePath().isEmpty()
        ? QDir(m_lastDirectory).filePath(QStringLiteral("drawing.dxf"))
        : m_document->filePath();

    QString selectedFilter = CodecRegistry::instance().defaultExportFilter();
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存图形文件"), suggested, filter, &selectedFilter);
    if (path.isEmpty()) return false;

    // 用户没有输入扩展名时，按所选过滤器补全；DXF 是本软件的默认格式
    QString finalPath = path;
    if (QFileInfo(finalPath).suffix().isEmpty()) {
        finalPath += QStringLiteral(".dxf");
    }

    return writeToPath(finalPath);
}

bool MainWindow::exportDrawing()
{
    const QString filter = CodecRegistry::instance().exportFilters().join(QStringLiteral(";;"));
    const QString suggested = QDir(m_lastDirectory).filePath(QStringLiteral("drawing.dxf"));

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出图形文件"), suggested, filter);
    if (path.isEmpty()) return false;

    return writeToPath(path);
}

bool MainWindow::writeToPath(const QString& path)
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const IoResult result = CadFile::save(path, *m_document, m_writeOptions);
    QApplication::restoreOverrideCursor();

    reportIoResult(QStringLiteral("保存文件"), result);
    if (!result.ok) return false;

    m_document->setFilePath(path);
    m_document->markClean();

    m_lastDirectory = QFileInfo(path).absolutePath();
    QSettings()
        .setValue(QStringLiteral("path/lastDirectory"), m_lastDirectory);

    updateWindowTitle();
    pushMessage(QStringLiteral("已保存 %1：%2")
                    .arg(QFileInfo(path).fileName())
                    .arg(result.summary()));
    return true;
}

bool MainWindow::maybeSaveChanges()
{
    if (!m_document->isModified()) return true;

    const auto answer = QMessageBox::question(
        this, QStringLiteral("LyCAD"),
        QStringLiteral("当前图纸 \"%1\" 尚未保存，是否保存？").arg(m_document->displayName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (answer == QMessageBox::Cancel) return false;
    if (answer == QMessageBox::Discard) return true;
    return saveDocument();
}

void MainWindow::reportIoResult(const QString& title, const IoResult& result)
{
    if (!result.ok) {
        QMessageBox::warning(this, title, result.error);
        pushMessage(QStringLiteral("[%1] 失败：%2").arg(title, result.error));
        return;
    }

    for (const QString& warning : result.warnings) {
        pushMessage(QStringLiteral("[%1] %2").arg(title, warning));
    }

    if (result.hasWarnings()) {
        // 有警告时不打断流程，但给出可查阅的提示
        const QString detail = result.warnings.join(QStringLiteral("\n• "));
        statusBar()->showMessage(
            QStringLiteral("%1 完成，有 %2 条提示（详见 帮助 → 命令历史）")
                .arg(title).arg(result.warnings.size()), 8000);
        Q_UNUSED(detail);
    }
}

// -----------------------------------------------------------------------------
//  界面同步
// -----------------------------------------------------------------------------
void MainWindow::updateWindowTitle()
{
    setWindowTitle(m_document->displayTitle());
}

void MainWindow::updateStatusModes()
{
    if (m_orthoButton) m_orthoButton->setChecked(m_toolManager->orthoMode());
    if (m_polarButton) m_polarButton->setChecked(m_toolManager->polarTracking());
    if (m_snapButton) m_snapButton->setChecked(m_toolManager->snapSettings().enabled);
    if (m_gridButton) m_gridButton->setChecked(m_toolManager->gridSnap());
}

void MainWindow::updateSelectionDependent()
{
    const bool hasSelection = !m_document->selection().isEmpty();
    if (m_deleteAction) m_deleteAction->setEnabled(hasSelection);

    if (m_countLabel) {
        m_countLabel->setText(QStringLiteral("图元 %1").arg(m_document->entityCount()));
    }
    if (m_undoAction) {
        m_undoAction->setEnabled(m_document->undoStack()->canUndo());
    }
    if (m_redoAction) {
        m_redoAction->setEnabled(m_document->undoStack()->canRedo());
    }

    refreshRibbonProperties();
}

void MainWindow::refreshPanels()
{
    if (m_layerPanel) {
        m_layerPanel->refresh(m_document->layers(), m_document->layers().currentLayerName());
    }
    if (m_propertyPanel) {
        m_propertyPanel->refresh(m_document);
    }
    refreshRibbonLayers();
    updateSelectionDependent();
}

void MainWindow::refreshRibbonLayers()
{
    if (!m_layerCombo) return;

    // 重建列表本身不是用户操作，屏蔽信号以免误触发「切换当前图层」
    QSignalBlocker blocker(m_layerCombo);

    const QString current = m_document->layers().currentLayerName();
    m_layerCombo->clear();
    for (const Layer& layer : m_document->layers().all()) {
        m_layerCombo->addItem(propertyChoices::colorSwatch(layer.colorIndex()), layer.name());
    }

    const int index = m_layerCombo->findText(current);
    if (index >= 0) m_layerCombo->setCurrentIndex(index);
}

void MainWindow::refreshRibbonProperties()
{
    if (!m_colorCombo || !m_lineweightCombo) return;

    const std::vector<EntityId> ids = m_document->selection().ids();
    const bool hasSelection = !ids.empty();

    QSignalBlocker colorBlocker(m_colorCombo);
    QSignalBlocker lineweightBlocker(m_lineweightCombo);

    // 没有选择集时这两项无从作用，直接置灰比留在可点状态更诚实
    m_colorCombo->setEnabled(hasSelection);
    m_lineweightCombo->setEnabled(hasSelection);

    if (!hasSelection) {
        m_colorCombo->setCurrentIndex(0);
        m_lineweightCombo->setCurrentIndex(0);
        return;
    }

    // 多选时只显示公共值，取值不一致就把下拉框留空——与属性面板同一约定
    bool seenColor = false;
    bool mixedColor = false;
    int color = 0;
    bool seenLineweight = false;
    bool mixedLineweight = false;
    int lineweight = 0;

    for (EntityId id : ids) {
        const Entity* entity = m_document->entity(id);
        if (!entity) continue;

        if (!seenColor) {
            color = entity->colorIndex();
            seenColor = true;
        } else if (color != entity->colorIndex()) {
            mixedColor = true;
        }

        if (!seenLineweight) {
            lineweight = entity->lineweightDxf();
            seenLineweight = true;
        } else if (lineweight != entity->lineweightDxf()) {
            mixedLineweight = true;
        }
    }

    m_colorCombo->setCurrentIndex(mixedColor ? -1 : m_colorCombo->findData(color));
    m_lineweightCombo->setCurrentIndex(mixedLineweight ? -1
                                                       : m_lineweightCombo->findData(lineweight));
}

void MainWindow::setDarkInterface(bool dark)
{
    // 顺序有讲究：先换应用调色板（菜单、停靠面板、状态栏、表格等由它统一接管），
    // 再让两个自绘部分跟上——功能区的外观与图标配色、画布的底色。
    theme::apply(dark);
    if (m_ribbon) m_ribbon->setDarkTheme(dark);

    // 功能区的滚动容器也要跟着换色：它比功能区多出滚动条那一条，
    // 颜色不一致就会在功能区下方露出一道异色边。
    if (m_ribbonViewport) {
        const theme::Palette& c = theme::palette(dark);
        // 只改底色，不碰滚动条本身：Fusion 原生的滚动条带两端箭头，
        // 且深浅两个主题下都清楚（滑块比底色亮三十多级）。一旦在这里用
        // 样式表重画，箭头会被规则里的 `width: 0` 抹掉，配色也要跟着维护一份，
        // 反而是自找麻烦。
        m_ribbonViewport->setStyleSheet(
            QStringLiteral("QScrollArea#ribbonViewport { background: %1; border: 0; }")
                .arg(c.ribbonBackground.name()));
    }

    m_canvas->applyTheme(dark);
    QSettings().setValue(QStringLiteral("ui/darkTheme"), dark);

    // 菜单项与功能区按钮是同一状态的两个入口，这里统一回写两者的勾选状态；
    // 屏蔽信号，免得两边互相触发绕成环。
    if (m_darkThemeAction) {
        QSignalBlocker blocker(m_darkThemeAction);
        m_darkThemeAction->setChecked(dark);
    }
    if (m_darkThemeButton) {
        QSignalBlocker blocker(m_darkThemeButton);
        m_darkThemeButton->setChecked(dark);
    }
}

void MainWindow::pushMessage(const QString& text)
{
    if (text.isEmpty()) return;

    m_messageLog.append(text);
    while (m_messageLog.size() > kMaxMessageLog) m_messageLog.removeFirst();

    if (m_messageLabel) m_messageLabel->setText(text);
    if (m_commandLine) m_commandLine->appendHistory(text);
}

void MainWindow::showCommandHelp()
{
    const QStringList help = m_toolManager->commandHelp();

    QStringList shortcuts;
    shortcuts << QStringLiteral("Esc\t取消当前命令")
              << QStringLiteral("空格 / 回车\t重复上一命令")
              << QStringLiteral("F1\t命令列表")
              << QStringLiteral("F3\t对象捕捉开关")
              << QStringLiteral("F7\t网格显示开关")
              << QStringLiteral("F8\t正交模式开关")
              << QStringLiteral("F9\t栅格吸附开关")
              << QStringLiteral("F10\t极轴追踪开关")
              << QStringLiteral("Delete\t删除选中对象")
              << QStringLiteral("Ctrl+A\t全选")
              << QStringLiteral("Ctrl+Z / Ctrl+Y\t撤销 / 重做")
              << QStringLiteral("中键拖拽\t平移视图")
              << QStringLiteral("滚轮\t缩放视图")
              << QStringLiteral("左→右框选\t窗选（完全包含）")
              << QStringLiteral("右→左框选\t交叉选（接触到）")
              << QStringLiteral("@x,y  或  长度<角度\t相对坐标 / 极坐标");

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("LyCAD 命令与快捷键"));
    dialog.resize(720, 560);

    auto* text = new QPlainTextEdit(&dialog);
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    text->setPlainText(
        QStringLiteral("可用命令\n"
                       "────────────────────────────────────────\n%1\n\n"
                       "输入约定\n"
                       "────────────────────────────────────────\n"
                       "100,50        绝对直角坐标\n"
                       "@50,0         相对上一点的直角坐标\n"
                       "50<45         长度 50、角度 45° 的极坐标\n"
                       "120           裸数值（半径 / 角度 / 比例，视命令而定）\n"
                       "25*2          支持 + - * / 与括号的算式\n\n"
                       "快捷键与鼠标\n"
                       "────────────────────────────────────────\n%2\n")
            .arg(help.join(QLatin1Char('\n')), shortcuts.join(QLatin1Char('\n'))));

    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(text);

    dialog.exec();
}

void MainWindow::showCommandHistory()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("命令历史"));
    dialog.resize(720, 480);

    auto* text = new QPlainTextEdit(&dialog);
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);

    if (m_messageLog.isEmpty()) {
        text->setPlainText(QStringLiteral("暂无历史消息。"));
    } else {
        text->setPlainText(m_messageLog.join(QLatin1Char('\n')));
        text->moveCursor(QTextCursor::End);
    }

    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(text);
    dialog.exec();
}

// -----------------------------------------------------------------------------
//  命令
// -----------------------------------------------------------------------------
void MainWindow::runCommand(const QString& command)
{
    m_toolManager->execute(command);
    m_canvas->setFocus();
}

void MainWindow::submitCommandLineText(const QString& text)
{
    // 先把文本交给活动工具（坐标、选项、数值），未被消费时再按命令解析
    m_toolManager->submitText(text);
    m_canvas->setFocus();
}

// -----------------------------------------------------------------------------
//  事件
// -----------------------------------------------------------------------------
void MainWindow::closeEvent(QCloseEvent* event)
{
    if (!maybeSaveChanges()) {
        event->ignore();
        return;
    }

    QSettings settings;
    settings.setValue(QStringLiteral("path/lastDirectory"), m_lastDirectory);
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/state"), saveState());

    event->accept();
}

void MainWindow::keyPressEvent(QKeyEvent* event)
{
    // 在画布上直接打字时，把输入引导到命令行——这是 CAD 的高频操作
    if (m_canvas && m_canvas->hasFocus() && !event->text().isEmpty()) {
        const QChar first = event->text().at(0);
        if (first.isPrint()) {
            m_commandLine->focusInput();
            m_commandLine->insertText(event->text());
            event->accept();
            return;
        }
    }

    if (event->key() == Qt::Key_Space && m_canvas && m_canvas->hasFocus()) {
        if (m_toolManager->isCommandRunning()) {
            m_toolManager->cancel();
        } else {
            m_toolManager->repeatLastCommand();
        }
        event->accept();
        return;
    }

    QMainWindow::keyPressEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
    if (!event->mimeData()->hasUrls()) return;

    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty()) return;

    const QString suffix = QFileInfo(urls.first().toLocalFile()).suffix().toLower();
    if (droppableExtensions().contains(suffix)) {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent* event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (urls.isEmpty()) return;

    const QString path = urls.first().toLocalFile();
    if (path.isEmpty()) return;

    if (maybeSaveChanges()) {
        loadFromPath(path);
    }
    event->acceptProposedAction();
}

} // namespace lycad
