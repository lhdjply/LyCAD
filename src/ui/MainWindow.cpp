// =============================================================================
//  LyCAD - ui/MainWindow.cpp
// =============================================================================
#include "ui/MainWindow.h"

#include "core/Commands.h"
#include "core/Document.h"
#include "core/Format.h"
#include "interact/ToolManager.h"
#include "io/CadFile.h"
#include "render/CadCanvas.h"
#include "ui/CommandLineEdit.h"
#include "ui/LayerPanel.h"
#include "ui/PropertyPanel.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
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
#include <QStatusBar>
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

    const bool darkTheme = settings.value(QStringLiteral("ui/darkCanvas"), false).toBool();
    m_canvas->applyTheme(darkTheme);
    m_canvas->display().showGrid = settings.value(QStringLiteral("ui/showGrid"), true).toBool();

    if (m_canvas->document()) {
        m_toolManager->snapSettings().gridSpacing = m_canvas->display().gridSpacing;
    }

    // ---- 界面 ----
    createActions();
    createMenus();
    createToolBars();
    createCentralWidget();
    createDockPanels();
    createStatusBar();
    connectSignals();

    updateWindowTitle();
    updateStatusModes();
    updateSelectionDependent();
    refreshPanels();

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

    QAction* darkAction = viewMenu->addAction(QStringLiteral("深色画布(&D)"));
    darkAction->setCheckable(true);
    darkAction->setChecked(m_canvas->display().themeIsDark);
    connect(darkAction, &QAction::toggled, this, [this](bool on) {
        m_canvas->applyTheme(on);
        QSettings()
            .setValue(QStringLiteral("ui/darkCanvas"), on);
    });

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

void MainWindow::createToolBars()
{
    QToolBar* fileBar = addToolBar(QStringLiteral("常用"));
    fileBar->setObjectName(QStringLiteral("toolbar.file"));
    fileBar->setMovable(false);

    fileBar->addAction(QStringLiteral("新建"), this, [this] { newDocument(); });
    fileBar->addAction(QStringLiteral("打开"), this, [this] { openDocument(); });
    fileBar->addAction(QStringLiteral("保存"), this, [this] { saveDocument(); });
    fileBar->addSeparator();
    fileBar->addAction(m_undoAction);
    fileBar->addAction(m_redoAction);
    fileBar->addSeparator();

    QToolBar* drawBar = addToolBar(QStringLiteral("绘图与修改"));
    drawBar->setObjectName(QStringLiteral("toolbar.draw"));
    drawBar->setMovable(false);

    struct Entry { const char* label; const char* command; const char* tip; };
    static const Entry kEntries[] = {
        { "直线",   "LINE",      "直线 (LINE / L)" },
        { "多段线", "PLINE",     "多段线 (PLINE / PL)" },
        { "圆",     "CIRCLE",    "圆 (CIRCLE / C)" },
        { "圆弧",   "ARC",       "圆弧 (ARC / A)" },
        { "矩形",   "RECTANGLE", "矩形 (RECTANGLE / REC)" },
        { "点",     "POINT",     "点 (POINT / PO)" },
        { "|",      "",          "" },
        { "文字",   "TEXT",      "文字 (TEXT / DT)" },
        { "填充",   "HATCH",     "填充 (HATCH / H)" },
        { "对齐标注", "DIMALIGNED", "对齐标注 (DIMALIGNED / DAL)" },
        { "线性标注", "DIMLINEAR",  "线性标注 (DIMLINEAR / DLI)" },
        { "半径标注", "DIMRADIUS",  "半径标注 (DIMRADIUS / DRA)" },
        { "直径标注", "DIMDIAMETER","直径标注 (DIMDIAMETER / DDI)" },
        { "|",      "",          "" },
        { "移动",   "MOVE",      "移动 (MOVE / M)" },
        { "复制",   "COPY",      "复制 (COPY / CO)" },
        { "旋转",   "ROTATE",    "旋转 (ROTATE / RO)" },
        { "缩放",   "SCALE",     "缩放 (SCALE / SC)" },
        { "镜像",   "MIRROR",    "镜像 (MIRROR / MI)" },
        { "删除",   "ERASE",     "删除 (ERASE / E)" },
    };

    for (const Entry& entry : kEntries) {
        const QString label = QString::fromUtf8(entry.label);
        if (label == QLatin1String("|")) {
            drawBar->addSeparator();
            continue;
        }
        QAction* action = drawBar->addAction(label);
        action->setToolTip(QString::fromUtf8(entry.tip));
        const QString command = QString::fromLatin1(entry.command);
        connect(action, &QAction::triggered, this, [this, command] { runCommand(command); });
    }

    drawBar->addSeparator();
    drawBar->addAction(QStringLiteral("缩放范围"), m_canvas, &CadCanvas::zoomExtents);
    drawBar->addAction(QStringLiteral("缩放选择集"), m_canvas, &CadCanvas::zoomToSelection);
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
}

void MainWindow::refreshPanels()
{
    if (m_layerPanel) {
        m_layerPanel->refresh(m_document->layers(), m_document->layers().currentLayerName());
    }
    if (m_propertyPanel) {
        m_propertyPanel->refresh(m_document);
    }
    updateSelectionDependent();
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
