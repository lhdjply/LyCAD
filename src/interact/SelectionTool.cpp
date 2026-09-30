// =============================================================================
//  LyCAD - interact/SelectionTool.cpp
// =============================================================================
#include "interact/SelectionTool.h"

#include "core/Document.h"
#include "interact/ToolManager.h"
#include "render/CadCanvas.h"

#include <QKeyEvent>
#include <QPainter>

#include <algorithm>

namespace lycad {

SelectionTool::SelectionTool(ToolContext& context)
    : Tool(context)
{
}

QString SelectionTool::name() const
{
    return QStringLiteral("SELECT");
}

QString SelectionTool::displayName() const
{
    return QStringLiteral("选择");
}

QString SelectionTool::prompt() const
{
    if (!ctx().document) return QString();

    if (m_mode == Mode::BoxSelect) {
        const bool crossing = m_cursorWorld.x < m_pressWorld.x;
        return crossing ? QStringLiteral("交叉选择（接触到即选中）:")
                        : QStringLiteral("窗选（完全包含才选中）:");
    }
    if (m_mode == Mode::GripDrag) {
        return QStringLiteral("拖拽夹点至新位置 <Esc 取消>:");
    }

    const std::size_t count = ctx().document->selection().size();
    if (count == 0) return QString();
    return QStringLiteral("已选择 %1 个对象 <Delete 删除，Esc 取消选择>:").arg(count);
}

bool SelectionTool::findGripUnderCursor(const Point2D& world, double tolerance,
                                        EntityId& entityId, int& gripIndex) const
{
    if (!ctx().document) return false;

    const Document& document = *ctx().document;
    for (EntityId id : document.selection().ids()) {
        const Entity* entity = document.entity(id);
        if (!entity) continue;

        const std::vector<Point2D> grips = entity->gripPoints();
        for (std::size_t i = 0; i < grips.size(); ++i) {
            if (grips[i].distanceTo(world) <= tolerance) {
                entityId = id;
                gripIndex = static_cast<int>(i);
                return true;
            }
        }
    }
    return false;
}

bool SelectionTool::mousePress(const CanvasEvent& event)
{
    if (!ctx().document) return false;
    if (event.button != Qt::LeftButton) return false;

    CadCanvas* canvas = ctx().canvas;
    const double tolerance = ctx().pickToleranceWorld();

    m_cursorWorld = event.worldPos;
    m_pressWorld = event.worldPos;
    m_additive = event.hasModifier(Qt::ShiftModifier);

    // 1) 优先判定夹点：选中对象上的夹点应先于实体本身被响应
    EntityId gripEntity = kInvalidEntityId;
    int gripIndex = -1;
    if (canvas && findGripUnderCursor(event.worldPos, tolerance, gripEntity, gripIndex)) {
        const Entity* entity = ctx().document->entity(gripEntity);
        if (entity) {
            m_mode = Mode::GripDrag;
            m_gripEntity = gripEntity;
            m_gripIndex = gripIndex;
            const std::vector<Point2D> grips = entity->gripPoints();
            m_gripOriginal = grips[static_cast<std::size_t>(gripIndex)];
            notifyPromptChanged();
            return true;
        }
    }

    // 2) 拾取实体
    const EntityId picked = canvas ? canvas->pickEntity(event.worldPos, tolerance)
                                   : kInvalidEntityId;
    if (picked != kInvalidEntityId) {
        Selection& selection = ctx().document->selection();
        if (m_additive) {
            selection.toggle(picked);
        } else if (!selection.contains(picked)) {
            selection.setIds({ picked });
        }
        notifyPromptChanged();
        return true;
    }

    // 3) 空白处按下：进入框选。若未按 Shift，先清空选择集
    if (!m_additive) {
        ctx().document->selection().clear();
    }
    m_mode = Mode::BoxSelect;
    notifyPromptChanged();
    return true;
}

bool SelectionTool::mouseMove(const CanvasEvent& event)
{
    if (m_mode == Mode::Idle) return false;

    m_cursorWorld = event.worldPos;

    // 夹点拖拽时同步显示捕捉结果，便于拖到精确位置
    if (m_mode == Mode::GripDrag) {
        const ResolvedPoint resolved = resolve(event.worldPos, m_gripOriginal);
        m_cursorWorld = resolved.position;
        if (resolved.isObjectSnap()) {
            ctx().setSnapMarker(resolved.toSnapResult());
        } else {
            ctx().setSnapMarker(std::nullopt);
        }
    }

    notifyPromptChanged();
    repaint();
    return true;
}

bool SelectionTool::mouseRelease(const CanvasEvent& event)
{
    if (event.button != Qt::LeftButton) return false;

    if (m_mode == Mode::GripDrag) {
        commitGripDrag();
        m_mode = Mode::Idle;
        ctx().setSnapMarker(std::nullopt);
        notifyPromptChanged();
        repaint();
        return true;
    }

    if (m_mode == Mode::BoxSelect) {
        Box2D box;
        box.expand(m_pressWorld);
        box.expand(event.worldPos);

        // 拖拽距离过小视为「在空白处单击」，仅完成清空选择集的动作
        const bool negligible = m_pressWorld.distanceTo(event.worldPos)
                              <= ctx().pickToleranceWorld();
        if (!negligible && ctx().canvas) {
            const bool crossing = event.worldPos.x < m_pressWorld.x;
            const std::vector<EntityId> hits =
                ctx().canvas->pickEntitiesInBox(box, crossing);

            Selection& selection = ctx().document->selection();
            if (m_additive) {
                selection.add(hits);
            } else {
                selection.setIds(hits);
            }
        }

        m_mode = Mode::Idle;
        notifyPromptChanged();
        repaint();
        return true;
    }

    return false;
}

void SelectionTool::commitGripDrag()
{
    if (!ctx().document || m_gripEntity == kInvalidEntityId || m_gripIndex < 0) return;

    const Entity* entity = ctx().document->entity(m_gripEntity);
    if (!entity) return;

    if (m_cursorWorld.distanceTo(m_gripOriginal) <= kGeomTol) return;   // 未移动，不产生撤销记录

    std::unique_ptr<Entity> edited = entity->clone();
    if (!edited->moveGrip(static_cast<std::size_t>(m_gripIndex), m_cursorWorld)) {
        message(QStringLiteral("该夹点不支持拖拽编辑。"));
        return;
    }

    ctx().document->replaceEntity(m_gripEntity, std::move(edited),
                                  QStringLiteral("夹点编辑"));
    message(QStringLiteral("已通过夹点修改对象。"));
}

bool SelectionTool::keyPress(QKeyEvent* event)
{
    if (!ctx().document) return false;

    switch (event->key()) {
    case Qt::Key_Escape:
        if (m_mode != Mode::Idle) {
            m_mode = Mode::Idle;
            ctx().setSnapMarker(std::nullopt);
            notifyPromptChanged();
            repaint();
            return true;
        }
        if (!ctx().document->selection().isEmpty()) {
            ctx().document->selection().clear();
            notifyPromptChanged();
            repaint();
            return true;
        }
        return false;

    case Qt::Key_Delete: {
        const std::vector<EntityId> ids = ctx().document->selection().ids();
        if (ids.empty()) return false;
        ctx().document->removeEntities(ids);
        message(QStringLiteral("已删除 %1 个对象。").arg(ids.size()));
        notifyPromptChanged();
        return true;
    }

    case Qt::Key_A:
        // Ctrl+A 全选
        if (event->modifiers().testFlag(Qt::ControlModifier)) {
            std::vector<EntityId> all;
            for (EntityId id : ctx().document->drawOrder()) {
                if (ctx().document->isSelectable(id)) all.push_back(id);
            }
            ctx().document->selection().setIds(all);
            message(QStringLiteral("已选择全部 %1 个对象。").arg(all.size()));
            notifyPromptChanged();
            return true;
        }
        return false;

    default:
        return false;
    }
}

void SelectionTool::deactivate()
{
    m_mode = Mode::Idle;
    m_gripEntity = kInvalidEntityId;
    m_gripIndex = -1;
    ctx().setSnapMarker(std::nullopt);
}

void SelectionTool::paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context)
{
    if (m_mode == Mode::BoxSelect) {
        const bool crossing = m_cursorWorld.x < m_pressWorld.x;

        const Point2D a = context.view->toDevice(m_pressWorld);
        const Point2D b = context.view->toDevice(m_cursorWorld);
        const QRectF rect(QPointF(std::min(a.x, b.x), std::min(a.y, b.y)),
                          QPointF(std::max(a.x, b.x), std::max(a.y, b.y)));

        // 窗选：实线蓝框（背景淡蓝）；交叉选：虚线绿框（背景淡绿）
        const QColor border = crossing ? QColor(0x2E, 0xA0, 0x43) : QColor(0x1E, 0x88, 0xE5);
        QColor fill = border;
        fill.setAlpha(28);

        QPen pen(border);
        pen.setWidthF(1.0);
        pen.setCosmetic(true);
        pen.setStyle(crossing ? Qt::DashLine : Qt::SolidLine);

        painter.save();
        painter.setPen(pen);
        painter.setBrush(fill);
        painter.drawRect(rect);
        painter.restore();
        return;
    }

    if (m_mode == Mode::GripDrag && ctx().document) {
        const Entity* entity = ctx().document->entity(m_gripEntity);
        if (!entity) return;

        // 用「已应用新夹点位置」的副本绘制实时预览，所见即所得
        std::unique_ptr<Entity> preview = entity->clone();
        if (!preview->moveGrip(static_cast<std::size_t>(m_gripIndex), m_cursorWorld)) return;

        QPen pen(QColor(0x1E, 0x88, 0xE5));
        pen.setWidthF(1.4);
        pen.setStyle(Qt::DashLine);
        painter.save();
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(EntityRenderer::toDevicePath(*preview, *context.view));
        painter.restore();

        // 自原夹点到新位置的牵引线
        const Point2D from = context.view->toDevice(m_gripOriginal);
        const Point2D to = context.view->toDevice(m_cursorWorld);
        painter.save();
        QPen guide(QColor(0x1E, 0x88, 0xE5));
        guide.setWidthF(1.0);
        guide.setStyle(Qt::DotLine);
        painter.setPen(guide);
        painter.drawLine(QPointF(from.x, from.y), QPointF(to.x, to.y));
        painter.restore();
    }
}

} // namespace lycad
