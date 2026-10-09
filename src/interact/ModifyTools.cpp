// =============================================================================
//  LyCAD - interact/ModifyTools.cpp
// =============================================================================
#include "interact/ModifyTools.h"

#include "core/Document.h"
#include "interact/CoordinateInput.h"
#include "render/CadCanvas.h"

#include <QKeyEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace lycad {

namespace {

const QColor kPreviewColor(0x1E, 0x88, 0xE5);

/// 以虚线绘制一个图元的预览副本
void drawGhost(QPainter& painter, const Entity& entity,
               const EntityRenderer::DrawContext& context, bool thick)
{
    QPen pen(kPreviewColor);
    pen.setWidthF(thick ? 1.6 : 1.1);
    pen.setStyle(thick ? Qt::SolidLine : Qt::DashLine);
    pen.setCosmetic(true);

    painter.save();
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(EntityRenderer::toDevicePath(entity, *context.view, context.viewport));
    painter.restore();
}

/// 从基点指向当前点的指示线
void drawGuide(QPainter& painter, const EntityRenderer::DrawContext& context,
               const Point2D& from, const Point2D& to)
{
    const Point2D a = context.view->toDevice(from);
    const Point2D b = context.view->toDevice(to);

    QPen pen(kPreviewColor);
    pen.setWidthF(1.0);
    pen.setStyle(Qt::DotLine);
    pen.setCosmetic(true);

    painter.save();
    painter.setPen(pen);
    painter.drawLine(QPointF(a.x, a.y), QPointF(b.x, b.y));
    painter.restore();
}

} // namespace

// =============================================================================
//  ModifyToolBase
// =============================================================================
ModifyToolBase::ModifyToolBase(ToolContext& context)
    : Tool(context)
{
}

void ModifyToolBase::activate()
{
    if (!ctx().document) return;

    // 已有选择集时直接进入基点阶段，符合 CAD 的操作习惯
    if (!ctx().document->selection().isEmpty()) {
        m_targets = ctx().document->selection().ids();
        m_phase = Phase::BasePoint;
    } else {
        m_phase = Phase::Selecting;
    }
    notifyPromptChanged();
}

QString ModifyToolBase::prompt() const
{
    switch (m_phase) {
    case Phase::Selecting:
        return QStringLiteral("%1：选择对象 <回车确认>:").arg(m_actionName);
    case Phase::BasePoint:
        return QStringLiteral("%1：指定基点 <可输入坐标>:").arg(m_actionName);
    case Phase::TargetPoint:
        return targetPrompt();
    }
    return QString();
}

bool ModifyToolBase::mousePress(const CanvasEvent& event)
{
    if (event.button != Qt::LeftButton || !ctx().document) return false;

    Document& document = *ctx().document;

    if (m_phase == Phase::Selecting) {
        CadCanvas* canvas = ctx().canvas;
        const EntityId picked = canvas ? canvas->pickEntity(event.worldPos, ctx().pickToleranceWorld())
                                       : kInvalidEntityId;
        if (picked == kInvalidEntityId) return true;

        if (event.hasModifier(Qt::ShiftModifier)) {
            document.selection().toggle(picked);
        } else {
            document.selection().add(picked);
        }
        notifyPromptChanged();
        repaint();
        return true;
    }

    if (m_phase == Phase::BasePoint) {
        const ResolvedPoint resolved = resolve(event.worldPos);
        m_base = resolved.position;
        m_phase = Phase::TargetPoint;
        notifyPromptChanged();
        repaint();
        return true;
    }

    // 目标阶段
    const ResolvedPoint resolved = resolve(event.worldPos, m_base);
    if (applyAt(resolved.position)) {
        finish();
    } else {
        repaint();
    }
    return true;
}

bool ModifyToolBase::mouseMove(const CanvasEvent& event)
{
    const std::optional<Point2D> base = (m_phase == Phase::BasePoint)
        ? std::nullopt
        : (m_phase == Phase::TargetPoint ? std::optional<Point2D>(m_base) : std::nullopt);

    m_cursor = resolve(event.worldPos, base);
    m_hasCursor = true;

    ctx().setSnapMarker(m_cursor.kind != SnapKind::None
                            ? std::optional<SnapResult>(m_cursor.toSnapResult())
                            : std::nullopt);
    repaint();
    return true;
}

bool ModifyToolBase::keyPress(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        finish();
        return true;
    }

    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (m_phase == Phase::Selecting) {
            const std::vector<EntityId> ids = ctx().document->selection().ids();
            if (ids.empty()) {
                message(QStringLiteral("未选择任何对象，命令取消。"));
                finish();
                return true;
            }
            m_targets = ids;
            m_phase = Phase::BasePoint;
            notifyPromptChanged();
            repaint();
            return true;
        }
        finish();
        return true;
    }

    return false;
}

bool ModifyToolBase::textInput(const QString& text)
{
    if (m_phase == Phase::Selecting) {
        message(QStringLiteral("请用鼠标选择对象，或按回车确认当前选择集。"));
        return true;
    }

    if (m_phase == Phase::BasePoint) {
        const std::optional<Point2D> point = parsePointInput(text, std::nullopt);
        if (!point) return true;
        m_base = *point;
        m_phase = Phase::TargetPoint;
        notifyPromptChanged();
        repaint();
        return true;
    }

    // 目标阶段：优先按数值解释（旋转角度、缩放比例）
    const QString scalarName = targetScalarName();
    if (!scalarName.isEmpty()) {
        const ParsedCoordinate parsed = parseCoordinate(text);
        if (parsed.isValid() && parsed.kind == ParsedCoordinate::Kind::Scalar) {
            if (handleTargetScalar(parsed.scalar)) {
                finish();
            }
            return true;
        }
    }

    const std::optional<Point2D> point = parsePointInput(text, m_base);
    if (!point) return true;

    if (applyAt(*point)) finish();
    else repaint();
    return true;
}

void ModifyToolBase::paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context)
{
    if (m_phase != Phase::TargetPoint || !m_hasCursor) return;
    paintTargetPreview(painter, context);
}

void ModifyToolBase::deactivate()
{
    ctx().setSnapMarker(std::nullopt);
}

// =============================================================================
//  移动
// =============================================================================
MoveTool::MoveTool(ToolContext& context)
    : ModifyToolBase(context)
{
    setActionName(QStringLiteral("移动"));
}

QString MoveTool::name() const { return QStringLiteral("MOVE"); }
QString MoveTool::displayName() const { return QStringLiteral("移动"); }

QString MoveTool::targetPrompt() const
{
    return QStringLiteral("移动：指定第二个点（位移终点）<可输入坐标>:");
}

bool MoveTool::applyAt(const Point2D& target)
{
    const Vector2D delta = target - m_base;
    if (delta.length() <= kGeomTol) return false;      // 位移为零，忽略

    const bool ok = ctx().document->transformEntities(
        m_targets, Transform2D::translation(delta.x, delta.y), QStringLiteral("移动对象"));

    if (ok) message(QStringLiteral("已移动 %1 个对象。").arg(m_targets.size()));
    return ok;
}

void MoveTool::paintTargetPreview(QPainter& painter, const EntityRenderer::DrawContext& context)
{
    const Vector2D delta = m_cursor.position - m_base;
    const Transform2D transform = Transform2D::translation(delta.x, delta.y);

    for (EntityId id : m_targets) {
        const Entity* entity = ctx().document->entity(id);
        if (!entity) continue;
        std::unique_ptr<Entity> ghost = entity->clone();
        ghost->transform(transform);
        drawGhost(painter, *ghost, context, false);
    }
    drawGuide(painter, context, m_base, m_cursor.position);
}

// =============================================================================
//  复制
// =============================================================================
CopyTool::CopyTool(ToolContext& context)
    : ModifyToolBase(context)
{
    setActionName(QStringLiteral("复制"));
}

QString CopyTool::name() const { return QStringLiteral("COPY"); }
QString CopyTool::displayName() const { return QStringLiteral("复制"); }

QString CopyTool::targetPrompt() const
{
    return QStringLiteral("复制：指定第二个点 [连续放置，回车结束]:");
}

bool CopyTool::applyAt(const Point2D& target)
{
    const Vector2D delta = target - m_base;
    if (delta.length() <= kGeomTol) return false;

    const Transform2D transform = Transform2D::translation(delta.x, delta.y);

    std::vector<std::unique_ptr<Entity>> copies;
    copies.reserve(m_targets.size());
    for (EntityId id : m_targets) {
        const Entity* entity = ctx().document->entity(id);
        if (!entity) continue;
        std::unique_ptr<Entity> copy = entity->clone();
        copy->transform(transform);
        copies.push_back(std::move(copy));
    }

    if (copies.empty()) return false;

    const std::size_t count = copies.size();
    ctx().document->addEntities(std::move(copies));
    ++m_copyCount;

    message(QStringLiteral("已复制 %1 个对象（第 %2 份）。").arg(count).arg(m_copyCount));
    return false;      // 连续复制：继续等待下一个目标点
}

void CopyTool::paintTargetPreview(QPainter& painter, const EntityRenderer::DrawContext& context)
{
    const Vector2D delta = m_cursor.position - m_base;
    const Transform2D transform = Transform2D::translation(delta.x, delta.y);

    for (EntityId id : m_targets) {
        const Entity* entity = ctx().document->entity(id);
        if (!entity) continue;
        std::unique_ptr<Entity> ghost = entity->clone();
        ghost->transform(transform);
        drawGhost(painter, *ghost, context, false);
    }
    drawGuide(painter, context, m_base, m_cursor.position);
}

// =============================================================================
//  旋转
// =============================================================================
RotateTool::RotateTool(ToolContext& context)
    : ModifyToolBase(context)
{
    setActionName(QStringLiteral("旋转"));
}

QString RotateTool::name() const { return QStringLiteral("ROTATE"); }
QString RotateTool::displayName() const { return QStringLiteral("旋转"); }

QString RotateTool::targetPrompt() const
{
    return QStringLiteral("旋转：指定旋转角度 <拖动或输入角度值>:");
}

QString RotateTool::targetScalarName() const
{
    return QStringLiteral("旋转角度（度）");
}

bool RotateTool::rotateBy(double radians)
{
    if (std::abs(radians) <= kGeomTol) return false;

    const bool ok = ctx().document->transformEntities(
        m_targets, Transform2D::rotationAround(m_base, radians), QStringLiteral("旋转对象"));

    if (ok) {
        message(QStringLiteral("已旋转 %1 个对象，角度 %2°。")
                    .arg(m_targets.size())
                    .arg(radToDeg(radians), 0, 'f', 2));
    }
    return ok;
}

bool RotateTool::applyAt(const Point2D& target)
{
    const Vector2D arm = target - m_base;
    if (arm.length() <= kGeomTol) return false;

    // 以「基点 -> 目标点」的方位角作为旋转角，与 AutoCAD 的拖动行为一致
    return rotateBy(std::atan2(arm.y, arm.x));
}

bool RotateTool::handleTargetScalar(double degrees)
{
    return rotateBy(degToRad(degrees));
}

void RotateTool::paintTargetPreview(QPainter& painter, const EntityRenderer::DrawContext& context)
{
    const Vector2D arm = m_cursor.position - m_base;
    if (arm.length() <= kGeomTol) return;

    const double radians = std::atan2(arm.y, arm.x);
    const Transform2D transform = Transform2D::rotationAround(m_base, radians);

    for (EntityId id : m_targets) {
        const Entity* entity = ctx().document->entity(id);
        if (!entity) continue;
        std::unique_ptr<Entity> ghost = entity->clone();
        ghost->transform(transform);
        drawGhost(painter, *ghost, context, false);
    }
    drawGuide(painter, context, m_base, m_cursor.position);
}

// =============================================================================
//  缩放
// =============================================================================
ScaleTool::ScaleTool(ToolContext& context)
    : ModifyToolBase(context)
{
    setActionName(QStringLiteral("缩放"));
}

QString ScaleTool::name() const { return QStringLiteral("SCALE"); }
QString ScaleTool::displayName() const { return QStringLiteral("缩放"); }

void ScaleTool::activate()
{
    m_referenceLength = 0.0;
    ModifyToolBase::activate();
}

QString ScaleTool::targetPrompt() const
{
    return QStringLiteral("缩放：指定比例（拖动设定）<或输入比例因子>:");
}

QString ScaleTool::targetScalarName() const
{
    return QStringLiteral("比例因子");
}

bool ScaleTool::scaleBy(double factor)
{
    if (!std::isfinite(factor) || factor <= kGeomTol) {
        message(QStringLiteral("比例因子必须为大于 0 的有限数值。"));
        return false;
    }

    const bool ok = ctx().document->transformEntities(
        m_targets, Transform2D::scalingAround(m_base, factor, factor), QStringLiteral("缩放对象"));

    if (ok) {
        message(QStringLiteral("已缩放 %1 个对象，比例 %2。")
                    .arg(m_targets.size())
                    .arg(factor, 0, 'f', 4));
    }
    return ok;
}

bool ScaleTool::applyAt(const Point2D& target)
{
    const double distance = target.distanceTo(m_base);
    if (distance <= kGeomTol) return false;

    // 以「进入目标阶段后的首次鼠标距离」为 1 倍基准，
    // 这样拖拽的缩放倍率与手势幅度成正比，比直接用距离更符合直觉。
    if (m_referenceLength <= kGeomTol) {
        m_referenceLength = distance;
        message(QStringLiteral("已设定缩放基准，移动鼠标调整比例。"));
        return false;
    }

    return scaleBy(distance / m_referenceLength);
}

bool ScaleTool::handleTargetScalar(double factor)
{
    return scaleBy(factor);
}

void ScaleTool::paintTargetPreview(QPainter& painter, const EntityRenderer::DrawContext& context)
{
    const double distance = m_cursor.position.distanceTo(m_base);
    if (distance <= kGeomTol) return;

    if (m_referenceLength <= kGeomTol) {
        // 尚未确定基准，先显示引导线
        drawGuide(painter, context, m_base, m_cursor.position);
        return;
    }

    const double factor = distance / m_referenceLength;
    if (factor <= kGeomTol) return;

    const Transform2D transform = Transform2D::scalingAround(m_base, factor, factor);

    for (EntityId id : m_targets) {
        const Entity* entity = ctx().document->entity(id);
        if (!entity) continue;
        std::unique_ptr<Entity> ghost = entity->clone();
        ghost->transform(transform);
        drawGhost(painter, *ghost, context, false);
    }
    drawGuide(painter, context, m_base, m_cursor.position);
}

// =============================================================================
//  镜像
// =============================================================================
MirrorTool::MirrorTool(ToolContext& context)
    : ModifyToolBase(context)
{
    setActionName(QStringLiteral("镜像"));
}

QString MirrorTool::name() const { return QStringLiteral("MIRROR"); }
QString MirrorTool::displayName() const { return QStringLiteral("镜像"); }

QString MirrorTool::targetPrompt() const
{
    return m_axisFirst
        ? QStringLiteral("镜像：指定镜像线的第二点:")
        : QStringLiteral("镜像：指定镜像线的第一点:");
}

bool MirrorTool::applyAt(const Point2D& target)
{
    if (!m_axisFirst) {
        m_axisFirst = target;
        notifyPromptChanged();
        repaint();
        return false;
    }

    if (target.distanceTo(*m_axisFirst) <= kGeomTol) {
        message(QStringLiteral("镜像线两点重合，请重新指定第二点。"));
        return false;
    }

    const Transform2D transform = Transform2D::mirror(*m_axisFirst, target);
    const bool ok = ctx().document->transformEntities(
        m_targets, transform, QStringLiteral("镜像对象"));

    if (ok) {
        message(QStringLiteral("已镜像 %1 个对象（源对象已保留，可用 MIRROR 后手动删除）。")
                    .arg(m_targets.size()));
    }
    return ok;
}

void MirrorTool::paintTargetPreview(QPainter& painter, const EntityRenderer::DrawContext& context)
{
    if (!m_axisFirst) {
        drawGuide(painter, context, m_base, m_cursor.position);
        return;
    }

    if (m_cursor.position.distanceTo(*m_axisFirst) <= kGeomTol) return;

    const Transform2D transform = Transform2D::mirror(*m_axisFirst, m_cursor.position);

    for (EntityId id : m_targets) {
        const Entity* entity = ctx().document->entity(id);
        if (!entity) continue;
        std::unique_ptr<Entity> ghost = entity->clone();
        ghost->transform(transform);
        drawGhost(painter, *ghost, context, false);
    }

    // 镜像轴线
    const Point2D a = context.view->toDevice(*m_axisFirst);
    const Point2D b = context.view->toDevice(m_cursor.position);
    const Vector2D direction(b.x - a.x, b.y - a.y);
    const Vector2D unit = direction.normalizedOr(Vector2D(1.0, 0.0));
    const double extend = std::hypot(context.viewport.width(), context.viewport.height());

    QPen pen(QColor(0xC2, 0x18, 0x5B));
    pen.setWidthF(1.0);
    pen.setStyle(Qt::DashDotLine);
    pen.setCosmetic(true);
    painter.save();
    painter.setPen(pen);
    painter.drawLine(QPointF(a.x - unit.x * extend, a.y - unit.y * extend),
                     QPointF(a.x + unit.x * extend, a.y + unit.y * extend));
    painter.restore();
}

// =============================================================================
//  分解块参照
// =============================================================================
ExplodeTool::ExplodeTool(ToolContext& context)
    : Tool(context)
{
}

QString ExplodeTool::name() const { return QStringLiteral("EXPLODE"); }
QString ExplodeTool::displayName() const { return QStringLiteral("分解"); }

QString ExplodeTool::prompt() const
{
    if (!ctx().document) return QString();
    const std::size_t count = ctx().document->selection().size();
    return QStringLiteral("分解块参照：选择对象 <回车确认>（已选 %1 个）:").arg(count);
}

void ExplodeTool::explodeSelection()
{
    if (!ctx().document) return;

    int count = 0;
    if (!ctx().document->explodeEntities(ctx().document->selection().ids(), &count)) {
        message(QStringLiteral("选择集中没有可分解的块参照。"));
        return;
    }
    message(QStringLiteral("已分解 %1 个块参照。").arg(count));
}

void ExplodeTool::activate()
{
    // 与 ERASE 一致：已有选择集时立即执行，符合 CAD 的「先选后改」习惯
    if (ctx().document && !ctx().document->selection().isEmpty()) {
        explodeSelection();
        finish();
        return;
    }
    notifyPromptChanged();
}

bool ExplodeTool::mousePress(const CanvasEvent& event)
{
    if (event.button != Qt::LeftButton || !ctx().document) return false;

    CadCanvas* canvas = ctx().canvas;
    const EntityId picked = canvas ? canvas->pickEntity(event.worldPos, ctx().pickToleranceWorld())
                                   : kInvalidEntityId;
    if (picked == kInvalidEntityId) return true;

    if (event.hasModifier(Qt::ShiftModifier)) {
        ctx().document->selection().toggle(picked);
    } else {
        ctx().document->selection().add(picked);
    }
    notifyPromptChanged();
    repaint();
    return true;
}

bool ExplodeTool::keyPress(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        finish();
        return true;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (ctx().document && !ctx().document->selection().isEmpty()) {
            explodeSelection();
        } else {
            message(QStringLiteral("未选择任何对象，命令取消。"));
        }
        finish();
        return true;
    }
    return false;
}

bool ExplodeTool::textInput(const QString& text)
{
    Q_UNUSED(text);
    message(QStringLiteral("请用鼠标选择要分解的块参照，按回车确认。"));
    return true;
}

void ExplodeTool::deactivate()
{
    ctx().setSnapMarker(std::nullopt);
}

// =============================================================================
//  删除
// =============================================================================
EraseTool::EraseTool(ToolContext& context)
    : Tool(context)
{
}

QString EraseTool::name() const { return QStringLiteral("ERASE"); }
QString EraseTool::displayName() const { return QStringLiteral("删除"); }

QString EraseTool::prompt() const
{
    if (!ctx().document) return QString();
    const std::size_t count = ctx().document->selection().size();
    return QStringLiteral("删除：选择对象 <回车确认>（已选 %1 个）:").arg(count);
}

void EraseTool::activate()
{
    // 已有选择集时立即执行，符合 CAD 的「先选后改」习惯
    if (ctx().document && !ctx().document->selection().isEmpty()) {
        const std::vector<EntityId> ids = ctx().document->selection().ids();
        ctx().document->removeEntities(ids);
        message(QStringLiteral("已删除 %1 个对象。").arg(ids.size()));
        finish();
        return;
    }
    notifyPromptChanged();
}

bool EraseTool::mousePress(const CanvasEvent& event)
{
    if (event.button != Qt::LeftButton || !ctx().document) return false;

    CadCanvas* canvas = ctx().canvas;
    const EntityId picked = canvas ? canvas->pickEntity(event.worldPos, ctx().pickToleranceWorld())
                                   : kInvalidEntityId;
    if (picked == kInvalidEntityId) return true;

    if (event.hasModifier(Qt::ShiftModifier)) {
        ctx().document->selection().toggle(picked);
    } else {
        ctx().document->selection().add(picked);
    }
    notifyPromptChanged();
    repaint();
    return true;
}

bool EraseTool::keyPress(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape) {
        finish();
        return true;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        const std::vector<EntityId> ids = ctx().document->selection().ids();
        if (ids.empty()) {
            message(QStringLiteral("未选择任何对象，命令取消。"));
        } else {
            ctx().document->removeEntities(ids);
            message(QStringLiteral("已删除 %1 个对象。").arg(ids.size()));
        }
        finish();
        return true;
    }
    return false;
}

bool EraseTool::textInput(const QString& text)
{
    Q_UNUSED(text);
    message(QStringLiteral("请用鼠标选择要删除的对象，按回车确认。"));
    return true;
}

void EraseTool::deactivate()
{
    ctx().setSnapMarker(std::nullopt);
}

} // namespace lycad
