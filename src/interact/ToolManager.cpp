// =============================================================================
//  LyCAD - interact/ToolManager.cpp
// =============================================================================
#include "interact/ToolManager.h"

#include "core/Document.h"
#include "interact/AnnotationTools.h"
#include "interact/DrawTools.h"
#include "interact/ModifyTools.h"
#include "interact/SelectionTool.h"
#include "render/CadCanvas.h"

#include <QKeyEvent>
#include <QPainter>

#include <algorithm>

namespace lycad {

ToolManager::ToolManager(QObject* parent)
    : QObject(parent)
{
    m_context.manager = this;
    registerBuiltinTools();
}

ToolManager::~ToolManager() = default;

// -----------------------------------------------------------------------------
//  关联对象
// -----------------------------------------------------------------------------
void ToolManager::setDocument(Document* document)
{
    m_document = document;
    m_context.document = document;
    ensureDefaultTool();
}

void ToolManager::setCanvas(CadCanvas* canvas)
{
    m_canvas = canvas;
    m_context.canvas = canvas;
    ensureDefaultTool();
}

void ToolManager::ensureDefaultTool()
{
    if (!m_context.isValid()) return;
    if (!m_defaultTool) {
        m_defaultTool = std::make_unique<SelectionTool>(m_context);
        notifyPromptChanged();
    }
}

// -----------------------------------------------------------------------------
//  命令注册表
// -----------------------------------------------------------------------------
void ToolManager::registerTool(const QString& command, const QStringList& aliases,
                               const QString& description, ToolFactory factory)
{
    const QString name = command.trimmed().toUpper();
    if (name.isEmpty()) return;

    for (CommandEntry& entry : m_commands) {
        if (entry.name == name) {
            entry.aliases = aliases;
            entry.description = description;
            entry.factory = std::move(factory);
            return;
        }
    }

    CommandEntry entry;
    entry.name = name;
    entry.aliases = aliases;
    entry.description = description;
    entry.factory = std::move(factory);
    m_commands.push_back(std::move(entry));
}

void ToolManager::registerBuiltinTools()
{
    // ---- 绘制 ---------------------------------------------------------------
    registerTool(QStringLiteral("LINE"), { QStringLiteral("L"), QStringLiteral("LINE") },
                 QStringLiteral("绘制直线（连续绘制，回车结束）"),
                 [](ToolContext& c) { return std::make_unique<DrawLineTool>(c); });

    registerTool(QStringLiteral("CIRCLE"), { QStringLiteral("C") },
                 QStringLiteral("绘制圆（圆心半径 / 三点 / 两点 / 直径）"),
                 [](ToolContext& c) { return std::make_unique<DrawCircleTool>(c); });

    registerTool(QStringLiteral("ARC"), { QStringLiteral("A") },
                 QStringLiteral("绘制圆弧（三点 / 圆心起点终点）"),
                 [](ToolContext& c) { return std::make_unique<DrawArcTool>(c); });

    registerTool(QStringLiteral("ELLIPSE"), { QStringLiteral("EL") },
                 QStringLiteral("绘制椭圆（轴端点 [+ 另一条半轴长度] / 中心点 C）"),
                 [](ToolContext& c) { return std::make_unique<DrawEllipseTool>(c); });

    registerTool(QStringLiteral("PLINE"), { QStringLiteral("PL"), QStringLiteral("POLYLINE") },
                 QStringLiteral("绘制多段线（C 闭合 / U 放弃）"),
                 [](ToolContext& c) { return std::make_unique<DrawPolylineTool>(c); });

    registerTool(QStringLiteral("XLINE"), { QStringLiteral("XL") },
                 QStringLiteral("绘制构造线（点 + 通过点；H 水平 / V 垂直）"),
                 [](ToolContext& c) {
                     return std::make_unique<DrawInfiniteLineTool>(c, EntityType::XLine);
                 });

    registerTool(QStringLiteral("RAY"), {},
                 QStringLiteral("绘制射线（起点 + 通过点）"),
                 [](ToolContext& c) {
                     return std::make_unique<DrawInfiniteLineTool>(c, EntityType::Ray);
                 });

    registerTool(QStringLiteral("POINT"), { QStringLiteral("PO") },
                 QStringLiteral("绘制点"),
                 [](ToolContext& c) { return std::make_unique<DrawPointTool>(c); });

    registerTool(QStringLiteral("RECTANGLE"),
                 { QStringLiteral("REC"), QStringLiteral("RECT") },
                 QStringLiteral("绘制矩形（两个对角点）"),
                 [](ToolContext& c) { return std::make_unique<DrawRectangleTool>(c); });

    // ---- 注释 ---------------------------------------------------------------
    registerTool(QStringLiteral("TEXT"), { QStringLiteral("DT") },
                 QStringLiteral("标注文字（先输入内容，再指定插入点）"),
                 [](ToolContext& c) { return std::make_unique<DrawTextTool>(c); });

    registerTool(QStringLiteral("HATCH"), { QStringLiteral("H"), QStringLiteral("BHATCH") },
                 QStringLiteral("填充（取点构成闭合边界，回车填充）"),
                 [](ToolContext& c) { return std::make_unique<DrawHatchTool>(c); });

    registerTool(QStringLiteral("DIMALIGNED"), { QStringLiteral("DAL") },
                 QStringLiteral("对齐标注（两条界线原点 + 尺寸线位置）"),
                 [](ToolContext& c) { return std::make_unique<DrawDimAlignedTool>(c); });

    registerTool(QStringLiteral("DIMLINEAR"), { QStringLiteral("DLI") },
                 QStringLiteral("线性标注（水平方向测量）"),
                 [](ToolContext& c) { return std::make_unique<DrawDimLinearTool>(c); });

    registerTool(QStringLiteral("DIMRADIUS"), { QStringLiteral("DRA") },
                 QStringLiteral("半径标注（拾取圆或圆弧）"),
                 [](ToolContext& c) { return std::make_unique<DrawDimRadialTool>(c); });

    registerTool(QStringLiteral("DIMDIAMETER"), { QStringLiteral("DDI") },
                 QStringLiteral("直径标注（拾取圆或圆弧）"),
                 [](ToolContext& c) { return std::make_unique<DrawDimDiametricTool>(c); });

    // ---- 编辑 ---------------------------------------------------------------
    registerTool(QStringLiteral("MOVE"), { QStringLiteral("M") },
                 QStringLiteral("移动对象"),
                 [](ToolContext& c) { return std::make_unique<MoveTool>(c); });

    registerTool(QStringLiteral("COPY"), { QStringLiteral("CO"), QStringLiteral("CP") },
                 QStringLiteral("复制对象（可连续放置）"),
                 [](ToolContext& c) { return std::make_unique<CopyTool>(c); });

    registerTool(QStringLiteral("ROTATE"), { QStringLiteral("RO") },
                 QStringLiteral("旋转对象（按角度）"),
                 [](ToolContext& c) { return std::make_unique<RotateTool>(c); });

    registerTool(QStringLiteral("SCALE"), { QStringLiteral("SC") },
                 QStringLiteral("缩放对象（按比例因子）"),
                 [](ToolContext& c) { return std::make_unique<ScaleTool>(c); });

    registerTool(QStringLiteral("MIRROR"), { QStringLiteral("MI") },
                 QStringLiteral("镜像对象（指定镜像线两点）"),
                 [](ToolContext& c) { return std::make_unique<MirrorTool>(c); });

    registerTool(QStringLiteral("ERASE"), { QStringLiteral("E"), QStringLiteral("DELETE") },
                 QStringLiteral("删除对象"),
                 [](ToolContext& c) { return std::make_unique<EraseTool>(c); });

    // ---- 选择 ---------------------------------------------------------------
    registerTool(QStringLiteral("SELECT"), { QStringLiteral("SE") },
                 QStringLiteral("激活选择模式（默认状态）"),
                 [](ToolContext& c) { Q_UNUSED(c); return nullptr; });
}

const ToolManager::CommandEntry* ToolManager::findCommand(const QString& name) const
{
    const QString upper = name.trimmed().toUpper();
    if (upper.isEmpty()) return nullptr;

    // 先匹配命令名，再匹配别名，保证 "C"（别名）不会遮蔽命令 "COPY"
    for (const CommandEntry& entry : m_commands) {
        if (entry.name == upper) return &entry;
    }
    for (const CommandEntry& entry : m_commands) {
        if (entry.aliases.contains(upper)) return &entry;
    }
    return nullptr;
}

bool ToolManager::hasCommand(const QString& name) const
{
    return findCommand(name) != nullptr;
}

QStringList ToolManager::commandNames() const
{
    QStringList names;
    names.reserve(static_cast<qsizetype>(m_commands.size()));
    for (const CommandEntry& entry : m_commands) names << entry.name;
    names.sort();
    return names;
}

QStringList ToolManager::commandHelp() const
{
    QStringList out;
    for (const CommandEntry& entry : m_commands) {
        QString line = entry.name;
        if (!entry.aliases.isEmpty()) {
            line += QStringLiteral(" (%1)").arg(entry.aliases.join(QLatin1Char('/')));
        }
        if (!entry.description.isEmpty()) {
            line += QStringLiteral("  —  %1").arg(entry.description);
        }
        out << line;
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

QStringList ToolManager::complete(const QString& prefix, int limit) const
{
    QStringList matches;
    const QString upper = prefix.trimmed().toUpper();
    if (upper.isEmpty()) return matches;

    for (const CommandEntry& entry : m_commands) {
        if (entry.name.startsWith(upper) || entry.aliases.contains(upper)) {
            matches << entry.name;
        }
        if (matches.size() >= limit) break;
    }
    return matches;
}

// -----------------------------------------------------------------------------
//  执行
// -----------------------------------------------------------------------------
bool ToolManager::execute(const QString& rawCommand)
{
    const QString command = rawCommand.trimmed().toUpper();
    if (command.isEmpty()) return false;

    const CommandEntry* entry = findCommand(command);
    if (!entry) {
        // 别名找不到时给出补全建议，减少用户的记忆负担
        const QStringList suggestions = complete(command.left(1), 5);
        QString hint = QStringLiteral("未知命令 \"%1\"。").arg(rawCommand);
        if (!suggestions.isEmpty()) {
            hint += QStringLiteral(" 相关命令：%1").arg(suggestions.join(QStringLiteral(", ")));
        }
        postMessage(hint);
        return false;
    }

    // 新命令一律终止正在执行的命令
    cancel();

    if (entry->name == QLatin1String("SELECT")) {
        m_lastCommand = entry->name;
        postMessage(QStringLiteral("已切换到选择模式。"));
        emit commandStarted(entry->name);
        notifyPromptChanged();
        if (m_canvas) m_canvas->update();
        return true;
    }

    if (!entry->factory) {
        postMessage(QStringLiteral("命令 \"%1\" 尚未实现。").arg(entry->name));
        return false;
    }

    if (!m_context.isValid()) {
        postMessage(QStringLiteral("尚未建立文档，无法执行命令。"));
        return false;
    }

    std::unique_ptr<Tool> tool = entry->factory(m_context);
    if (!tool) {
        postMessage(QStringLiteral("命令 \"%1\" 无法启动。").arg(entry->name));
        return false;
    }

    m_lastCommand = entry->name;
    m_activeTool = std::move(tool);
    m_activeTool->activate();

    postMessage(QStringLiteral("命令: %1").arg(entry->name));
    emit commandStarted(entry->name);
    notifyPromptChanged();

    if (m_canvas) {
        m_canvas->setCursor(cursorShape());
        m_canvas->update();
    }
    return true;
}

bool ToolManager::submitText(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return false;

    // 1) 有活动命令时优先交给它
    if (m_activeTool && m_activeTool->textInput(trimmed)) {
        notifyPromptChanged();
        if (m_canvas) m_canvas->update();
        return true;
    }

    // 2) 选择模式下输入点坐标无意义，按命令处理
    return execute(trimmed);
}

void ToolManager::cancel()
{
    if (m_activeTool) {
        const QString name = m_activeTool->name();
        m_activeTool->deactivate();
        m_activeTool.reset();

        emit commandEnded(name);
        postMessage(QStringLiteral("*取消*"));
    }

    notifyPromptChanged();
    if (m_canvas) {
        m_canvas->setSnapResult(std::nullopt);
        m_canvas->setCursor(cursorShape());
        m_canvas->update();
    }
}

void ToolManager::finishCurrentTool()
{
    if (!m_activeTool) return;

    const QString name = m_activeTool->name();
    m_activeTool->deactivate();
    m_activeTool.reset();

    emit commandEnded(name);
    notifyPromptChanged();

    if (m_canvas) {
        m_canvas->setSnapResult(std::nullopt);
        m_canvas->setCursor(cursorShape());
        m_canvas->update();
    }
}

bool ToolManager::repeatLastCommand()
{
    if (m_lastCommand.isEmpty()) return false;
    return execute(m_lastCommand);
}

// -----------------------------------------------------------------------------
//  模式开关
// -----------------------------------------------------------------------------
void ToolManager::setOrthoMode(bool enabled)
{
    if (m_context.orthoMode == enabled) return;
    m_context.orthoMode = enabled;

    // 正交与极轴互斥：同时开启会让方向约束的优先级变得难以预测
    if (enabled && m_context.polarTracking) {
        m_context.polarTracking = false;
    }
    emit modeChanged();
    postMessage(enabled ? QStringLiteral("正交模式：开")
                        : QStringLiteral("正交模式：关"));
}

void ToolManager::setPolarTracking(bool enabled)
{
    if (m_context.polarTracking == enabled) return;
    m_context.polarTracking = enabled;
    if (enabled && m_context.orthoMode) {
        m_context.orthoMode = false;
    }
    emit modeChanged();
    postMessage(enabled ? QStringLiteral("极轴追踪：开")
                        : QStringLiteral("极轴追踪：关"));
}

void ToolManager::setGridSnap(bool enabled)
{
    if (m_context.snap.grid == enabled) return;
    m_context.snap.grid = enabled;

    // 栅格吸附开启时按画布的栅格间距对齐，保证「看得见 = 抓得到」
    if (enabled && m_canvas) {
        m_context.snap.gridSpacing = m_canvas->display().gridSpacing;
    }
    emit modeChanged();
    postMessage(enabled ? QStringLiteral("栅格吸附：开")
                        : QStringLiteral("栅格吸附：关"));
}

// -----------------------------------------------------------------------------
//  回调
// -----------------------------------------------------------------------------
void ToolManager::postMessage(const QString& text)
{
    if (text.isEmpty()) return;
    emit commandMessage(text);
}

void ToolManager::notifyPromptChanged()
{
    emit promptChanged(promptText());
}

void ToolManager::requestTextEdit(lycad::EntityId id)
{
    // 工具层不持有对话框：编辑意图经信号交给界面层，与
    // 「面板只发意图信号」的既有约定保持同构
    emit textEditRequested(id);
}

// -----------------------------------------------------------------------------
//  CanvasInputHandler
// -----------------------------------------------------------------------------
Tool* ToolManager::eventTarget() const
{
    if (m_activeTool) return m_activeTool.get();
    return m_defaultTool.get();
}

bool ToolManager::handleMousePress(const CanvasEvent& event)
{
    Tool* target = eventTarget();
    if (!target) return false;

    // 绘制类命令独占左键；默认的选择工具在空白处按下时会自行进入框选
    return target->mousePress(event);
}

bool ToolManager::handleMouseMove(const CanvasEvent& event)
{
    Tool* target = eventTarget();
    if (!target) return false;
    return target->mouseMove(event);
}

bool ToolManager::handleMouseRelease(const CanvasEvent& event)
{
    Tool* target = eventTarget();
    if (!target) return false;
    return target->mouseRelease(event);
}

bool ToolManager::handleMouseDoubleClick(const CanvasEvent& event)
{
    Tool* target = eventTarget();
    if (!target) return false;
    return target->mouseDoubleClick(event);
}

bool ToolManager::handleKeyPress(QKeyEvent* event)
{
    // Esc 是全局的「终止当前命令」语义，优先于工具自身的处理
    if (event->key() == Qt::Key_Escape) {
        if (m_activeTool) {
            cancel();
            return true;
        }
    }

    Tool* target = eventTarget();
    if (target && target->keyPress(event)) return true;

    return false;
}

void ToolManager::paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context)
{
    Tool* target = eventTarget();
    if (!target) return;
    target->paintOverlay(painter, context);
}

QString ToolManager::promptText() const
{
    if (m_activeTool) return m_activeTool->prompt();
    if (m_defaultTool) return m_defaultTool->prompt();
    return QString();
}

Qt::CursorShape ToolManager::cursorShape() const
{
    if (m_activeTool) return m_activeTool->cursorShape();
    if (m_defaultTool) return m_defaultTool->cursorShape();
    return Qt::CrossCursor;
}

} // namespace lycad
