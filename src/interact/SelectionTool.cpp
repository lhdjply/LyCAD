// =============================================================================
//  LyCAD - interact/SelectionTool.cpp
// =============================================================================
#include "interact/SelectionTool.h"

#include "core/Document.h"
#include "core/MTextEntity.h"
#include "core/TableEntity.h"
#include "core/TextEntity.h"
#include "interact/ToolManager.h"
#include "render/CadCanvas.h"

#include <QKeyEvent>
#include <QPainter>

#include <algorithm>
#include <limits>

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
        // 夹点能做的事各不相同，由图元自己报出正在拖的是哪一个
        QString label;
        if (ctx().document) {
            if (const Entity* entity = ctx().document->entity(m_gripEntity)) {
                label = entity->gripLabel(static_cast<std::size_t>(m_gripIndex));
            }
        }
        return label.isEmpty()
                   ? QStringLiteral("拖拽夹点至新位置 <Esc 取消>:")
                   : QStringLiteral("拖拽夹点调整「%1」<Esc 取消>:").arg(label);
    }
    if (m_mode == Mode::EdgeDrag) {
        return QStringLiteral("拖拽表格界线「%1」<Esc 取消>:").arg(m_edgeLabel);
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

SelectionTool::EdgeTarget SelectionTool::findTableEdgeUnderCursor(const Point2D& world,
                                                                  double tolerance) const
{
    EdgeTarget target;
    if (!ctx().document) return target;

    // 自上而下取第一张命中界线的表格（与拾取同一顺序）
    const auto& order = ctx().document->drawOrder();
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const Entity* entity = ctx().document->entity(*it);
        if (!entity || entity->type() != EntityType::Table) continue;
        if (!ctx().document->isSelectable(*it)) continue;

        const TableEntity::EdgeHit hit =
            static_cast<const TableEntity*>(entity)->edgeAtWorld(world, tolerance);
        if (!hit.hit) continue;

        target.id = *it;
        target.hit = hit;
        return target;
    }
    return target;
}

Qt::CursorShape SelectionTool::cursorShape() const
{
    // 压在表格界线上时给出调整光标（横向拉伸 / 纵向拉伸）：这条"不必先选中就能拖"
    // 的操作没有别的可见提示，光标是它唯一的可发现性来源。
    if (m_hoverColumnEdge) return Qt::SizeHorCursor;
    if (m_hoverRowEdge) return Qt::SizeVerCursor;
    return Tool::cursorShape();
}

void SelectionTool::updateHoverEdge(const Point2D& world)
{
    const EdgeTarget target = ctx().document
        ? findTableEdgeUnderCursor(world, ctx().pickToleranceWorld())
        : EdgeTarget{};
    m_hoverColumnEdge = target && target.hit.vertical;
    m_hoverRowEdge = target && !target.hit.vertical;
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

    // 2) 表格的行列界线：压在线上就能直接拖，**不必先选中表格**（Excel 的习惯）。
    //    界线上不画夹点方块，所以这是改行高 / 列宽的唯一入口。
    const EdgeTarget edge = canvas ? findTableEdgeUnderCursor(event.worldPos, tolerance)
                                   : EdgeTarget{};
    if (edge) {
        const auto* table = static_cast<const TableEntity*>(ctx().document->entity(edge.id));
        if (table) {
            // 顺手选中：拖的时候能看见四角夹点，也说明「改的是这张表」
            ctx().document->selection().setIds({ edge.id });

            const std::pair<Point2D, Point2D> seg = edge.hit.vertical
                ? table->columnBoundarySegment(edge.hit.index)
                : table->rowBoundarySegment(edge.hit.index);

            m_mode = Mode::EdgeDrag;
            m_edgeEntity = edge.id;
            m_edgeVertical = edge.hit.vertical;
            m_edgeIndex = edge.hit.index;
            m_edgeLabel = edge.hit.label;
            m_edgeArmed = false;      // 还没真的拖：见 mouseMove 里的阈值判定
            m_edgeOriginal = table->toWorld(Point2D(0.5 * (seg.first.x + seg.second.x),
                                                    0.5 * (seg.first.y + seg.second.y)));
            notifyPromptChanged();
            return true;
        }
    }

    // 3) 拾取实体
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
    if (m_mode == Mode::Idle) {
        // 空闲时也要跟住光标：悬停在表格界线上要换成调整光标（见 cursorShape）
        m_cursorWorld = event.worldPos;
        updateHoverEdge(event.worldPos);
        return false;
    }

    m_cursorWorld = event.worldPos;

    // 夹点 / 界线拖拽时同步显示捕捉结果，便于拖到精确位置
    if (m_mode == Mode::GripDrag || m_mode == Mode::EdgeDrag) {
        const Point2D base = (m_mode == Mode::GripDrag) ? m_gripOriginal : m_edgeOriginal;
        const ResolvedPoint resolved = resolve(event.worldPos, base);
        m_cursorWorld = resolved.position;
        if (resolved.isObjectSnap()) {
            ctx().setSnapMarker(resolved.toSnapResult());
        } else {
            ctx().setSnapMarker(std::nullopt);
        }
    }

    // 界线拖拽要分清"点一下"与"拖一下"：光标从按下点移出阈值之前不算在拖。
    // 否则在线上单击（哪怕手抖几个像素）就会当场改掉行高 / 列宽——门槛与
    // 「框选时判定拖拽距离」同一条标准，不另外发明一个数。
    if (m_mode == Mode::EdgeDrag && !m_edgeArmed
        && m_cursorWorld.distanceTo(m_pressWorld) > ctx().pickToleranceWorld()) {
        m_edgeArmed = true;
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

    if (m_mode == Mode::EdgeDrag) {
        // 只是点了一下（没越过拖动阈值）就不算改尺寸：表格保持选中即可
        if (m_edgeArmed) commitEdgeDrag();
        m_mode = Mode::Idle;
        m_edgeArmed = false;
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

// -----------------------------------------------------------------------------
//  双击：文字图元与表格单元格进入编辑（对话框由界面层弹出，工具层只发意图）
// -----------------------------------------------------------------------------
bool SelectionTool::mouseDoubleClick(const CanvasEvent& event)
{
    if (event.button != Qt::LeftButton || !ctx().document || !ctx().canvas) return false;

    const double tolerance = ctx().pickToleranceWorld();
    const auto& order = ctx().document->drawOrder();

    // 文字优先：实心填充常常盖在文字上方，按常规拾取（自上而下）会先命中
    // 填充而不是文字，导致文字永远无法被双击。这里在全部图元中找
    // 光标下的文字图元；多条文字的包围盒重叠时，取锚点离点击处最近者——
    // 「点谁编辑谁」。
    EntityId target = kInvalidEntityId;
    double nearestAnchor = std::numeric_limits<double>::max();
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const Entity* e = ctx().document->entity(*it);
        if (!e) continue;
        const bool isText = e->type() == EntityType::Text;
        const bool isMText = e->type() == EntityType::MText;
        if (!isText && !isMText) continue;
        if (!ctx().document->isSelectable(*it)) continue;
        if (e->distanceTo(event.worldPos) > tolerance) continue;

        const double anchorDist =
            (isText ? static_cast<const TextEntity*>(e)->effectivePosition()
                    : static_cast<const MTextEntity*>(e)->insertionPoint())
                .distanceTo(event.worldPos);
        if (anchorDist < nearestAnchor) {
            nearestAnchor = anchorDist;
            target = *it;
        }
    }

    // 光标下没有文字时，退回常规拾取（命中文字才触发编辑）
    if (target == kInvalidEntityId) {
        const EntityId picked = ctx().canvas->pickEntity(event.worldPos, tolerance);
        if (picked != kInvalidEntityId) {
            const Entity* e = ctx().document->entity(picked);
            if (e && (e->type() == EntityType::Text || e->type() == EntityType::MText)) {
                target = picked;
            }
        }
    }

    if (target != kInvalidEntityId) {
        if (!ctx().document->isSelectable(target)) return false;
        if (ctx().manager) {
            ctx().manager->requestTextEdit(target);
            return true;
        }
        return false;
    }

    // 光标下没有文字：看是不是双击在表格的某一格里。表格是**单一图元**，
    // 「改哪一格」由落点决定，所以要自上而下逐个表格问它的命中结果，
    // 而不是拿拾取到的图元（拾取只回答「命中了哪张表」）。
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const Entity* e = ctx().document->entity(*it);
        if (!e || e->type() != EntityType::Table) continue;
        if (!ctx().document->isSelectable(*it)) continue;

        const auto cell = static_cast<const TableEntity*>(e)->cellAtWorld(event.worldPos);
        if (!cell) continue;
        if (ctx().manager) {
            ctx().manager->requestTableCellEdit(*it, cell->first, cell->second);
            return true;
        }
        return false;
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

    // 夹点说明要在 replaceEntity 之前取：那一步会把旧图元换成克隆件，
    // 之后再用 entity 就是悬空指针。
    const QString label = entity->gripLabel(static_cast<std::size_t>(m_gripIndex));

    ctx().document->replaceEntity(m_gripEntity, std::move(edited),
                                  QStringLiteral("夹点编辑"));
    // 说清改的是哪一项：表格上"拖了一下"可能是缩放整表，也可能只是改了某一行
    message(label.isEmpty() ? QStringLiteral("已通过夹点修改对象。")
                            : QStringLiteral("已通过夹点修改对象（%1）。").arg(label));
}

void SelectionTool::commitEdgeDrag()
{
    if (!ctx().document || m_edgeEntity == kInvalidEntityId) return;
    if (!m_edgeArmed) return;               // 只有"真的拖动过"才产生撤销记录

    const Entity* entity = ctx().document->entity(m_edgeEntity);
    if (!entity || entity->type() != EntityType::Table) return;

    // 没真的移动就不产生撤销记录
    if (m_cursorWorld.distanceTo(m_edgeOriginal) <= kGeomTol) return;

    auto replacement = entity->clone();
    auto* table = static_cast<TableEntity*>(replacement.get());
    const bool moved = m_edgeVertical ? table->moveColumnBoundary(m_edgeIndex, m_cursorWorld)
                                      : table->moveRowBoundary(m_edgeIndex, m_cursorWorld);
    if (!moved) return;

    const QString label = m_edgeVertical ? QStringLiteral("调整列宽") : QStringLiteral("调整行高");
    const QString detail = m_edgeLabel;
    if (ctx().document->replaceEntity(m_edgeEntity, std::move(replacement), label)) {
        message(QStringLiteral("已调整%1。").arg(detail));
    }
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
    m_edgeEntity = kInvalidEntityId;
    m_edgeIndex = 0;
    m_edgeLabel.clear();
    m_edgeArmed = false;
    m_hoverColumnEdge = false;
    m_hoverRowEdge = false;
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

    if (m_mode == Mode::EdgeDrag && m_edgeArmed && ctx().document) {
        const Entity* entity = ctx().document->entity(m_edgeEntity);
        if (!entity) return;

        // 只把**正在挪的那一条线**画出来（虚线、整条、跟着光标走）。
        // 两种都不取：把整表画成虚线看不出是哪条在动；从线中点牵一条引线，
        // 看起来像"中点在走"——而用户要看的是这条线在移动。
        std::unique_ptr<Entity> preview = entity->clone();
        auto* table = static_cast<TableEntity*>(preview.get());
        const bool moved = m_edgeVertical ? table->moveColumnBoundary(m_edgeIndex, m_cursorWorld)
                                          : table->moveRowBoundary(m_edgeIndex, m_cursorWorld);
        if (!moved) return;

        const std::pair<Point2D, Point2D> seg = m_edgeVertical
            ? table->columnBoundarySegment(m_edgeIndex)
            : table->rowBoundarySegment(m_edgeIndex);
        const Point2D a = context.view->toDevice(table->toWorld(seg.first));
        const Point2D b = context.view->toDevice(table->toWorld(seg.second));

        QPen pen(QColor(0x1E, 0x88, 0xE5));
        pen.setWidthF(1.8);
        pen.setStyle(Qt::DashLine);
        pen.setCosmetic(true);
        painter.save();
        painter.setPen(pen);
        painter.drawLine(QPointF(a.x, a.y), QPointF(b.x, b.y));
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
        painter.drawPath(EntityRenderer::toDevicePath(*preview, *context.view, context.viewport));
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
