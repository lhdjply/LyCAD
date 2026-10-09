// =============================================================================
//  LyCAD - interact/DrawTools.cpp
// =============================================================================
#include "interact/DrawTools.h"

#include "core/ArcEntity.h"
#include "core/CircleEntity.h"
#include "core/Document.h"
#include "core/EllipseEntity.h"
#include "core/InfiniteLineEntity.h"
#include "core/LineEntity.h"
#include "core/PointEntity.h"
#include "core/PolylineEntity.h"
#include "core/RayEntity.h"
#include "core/TableEntity.h"
#include "core/XLineEntity.h"
#include "render/CadCanvas.h"

#include <QKeyEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lycad {

namespace {

/// 预览线条的颜色与样式，与最终实体区分开
const QColor kPreviewColor(0x1E, 0x88, 0xE5);

/// 以虚线绘制一个临时图元的预览
void drawPreviewEntity(QPainter& painter, const Entity& entity,
                       const EntityRenderer::DrawContext& context)
{
    QPen pen(kPreviewColor);
    pen.setWidthF(1.2);
    pen.setStyle(Qt::DashLine);
    pen.setCosmetic(true);

    painter.save();
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(EntityRenderer::toDevicePath(entity, *context.view, context.viewport));
    painter.restore();
}

/// 在两点之间画橡皮筋
void drawRubberBand(QPainter& painter, const EntityRenderer::DrawContext& context,
                    const Point2D& from, const Point2D& to)
{
    const Point2D a = context.view->toDevice(from);
    const Point2D b = context.view->toDevice(to);

    QPen pen(kPreviewColor);
    pen.setWidthF(1.0);
    pen.setStyle(Qt::DashLine);
    pen.setCosmetic(true);

    painter.save();
    painter.setPen(pen);
    painter.drawLine(QPointF(a.x, a.y), QPointF(b.x, b.y));
    painter.restore();
}

/// 把新图元归入当前图层，颜色随层
template <typename T>
void applyDefaults(T& entity, const ToolContext& context)
{
    if (!context.document) return;
    entity.setLayerName(context.document->layers().currentLayerName());
    entity.setColorByLayer();
}

/// 鼠标是否离开画布
bool cursorAvailable(bool hasCursor)
{
    return hasCursor;
}

/// 过基点的一条长虚线：用于「方向已吸附到正方向」时的追踪线。
/// 长度取满视口对角线，两端各伸出视口外，看起来就是一条贯穿屏幕的导轨。
void drawTrackingLine(QPainter& painter, const EntityRenderer::DrawContext& context,
                      const Point2D& from, const Point2D& towards)
{
    if (!context.view) return;

    const Point2D a = context.view->toDevice(from);
    const Point2D b = context.view->toDevice(towards);
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double length = std::hypot(dx, dy);
    if (length <= 1e-9) return;

    const double span = std::hypot(context.viewport.width(), context.viewport.height());
    const double ux = dx / length;
    const double uy = dy / length;

    QPen pen(kPreviewColor);
    pen.setWidthF(1.0);
    pen.setStyle(Qt::DotLine);
    pen.setCosmetic(true);

    painter.save();
    painter.setPen(pen);
    painter.drawLine(QPointF(a.x - ux * span, a.y - uy * span),
                     QPointF(a.x + ux * span, a.y + uy * span));
    painter.restore();
}

/// 半轴端点的落点标记：实心小方块（固定像素，不随缩放变化）
void drawTipMarker(QPainter& painter, const EntityRenderer::DrawContext& context,
                   const Point2D& world)
{
    if (!context.view) return;

    const Point2D device = context.view->toDevice(world);
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(kPreviewColor);
    painter.drawRect(QRectF(device.x - 2.5, device.y - 2.5, 5.0, 5.0));
    painter.restore();
}

/// 把 base → raw 的方向吸附到四个正方向（右 / 上 / 左 / 下）。
///
/// 只在**光圈内**才吸：判据是光标到那条射线的垂直距离（世界单位），
/// 光圈由像素换算而来，因此缩放多少都不影响手感——这与对象捕捉同一口径。
/// 命中时通过 directionOut 返回吸附到的方向单位向量。
std::optional<Point2D> snapToCardinalDirection(const Point2D& base, const Point2D& raw,
                                               double apertureWorld, Vector2D& directionOut)
{
    static const Vector2D kCardinals[4] = { Vector2D(1.0, 0.0), Vector2D(0.0, 1.0),
                                            Vector2D(-1.0, 0.0), Vector2D(0.0, -1.0) };

    const Vector2D delta = raw - base;
    // 光标贴着基点时「方向」没有意义，此时吸附等于把点钉死在基点上，反而更糟
    if (delta.length() <= apertureWorld) return std::nullopt;

    double bestOffset = std::numeric_limits<double>::max();
    Vector2D bestDirection = kCardinals[0];
    double bestAlong = 0.0;
    for (const Vector2D& direction : kCardinals) {
        const double along = dot(delta, direction);
        if (along <= 0.0) continue;                        // 反方向的射线够不到
        const double offset = std::abs(cross(delta, direction));   // 到该射线的垂直距离
        if (offset < bestOffset) {
            bestOffset = offset;
            bestDirection = direction;
            bestAlong = along;
        }
    }

    if (bestOffset > apertureWorld) return std::nullopt;

    directionOut = bestDirection;
    return base + bestDirection * bestAlong;
}

} // namespace

// =============================================================================
//  直线
// =============================================================================
DrawLineTool::DrawLineTool(ToolContext& context)
    : Tool(context)
{
}

QString DrawLineTool::name() const { return QStringLiteral("LINE"); }
QString DrawLineTool::displayName() const { return QStringLiteral("直线"); }

QString DrawLineTool::prompt() const
{
    if (m_vertices.empty()) return QStringLiteral("指定第一个点:");
    return QStringLiteral("指定下一点 [放弃(U)] <回车结束>:");
}

bool DrawLineTool::mousePress(const CanvasEvent& event)
{
    if (event.button != Qt::LeftButton || !ctx().document) return false;

    const std::optional<Point2D> base = m_vertices.empty()
        ? std::nullopt
        : std::optional<Point2D>(m_vertices.back());

    const ResolvedPoint resolved = resolve(event.worldPos, base);

    if (m_vertices.empty()) {
        m_vertices.push_back(resolved.position);
    } else {
        createSegment(m_vertices.back(), resolved.position);
        m_vertices.push_back(resolved.position);
    }

    notifyPromptChanged();
    repaint();
    return true;
}

bool DrawLineTool::mouseMove(const CanvasEvent& event)
{
    const std::optional<Point2D> base = m_vertices.empty()
        ? std::nullopt
        : std::optional<Point2D>(m_vertices.back());

    m_cursor = resolve(event.worldPos, base);
    m_hasCursor = cursorAvailable(true);

    if (m_cursor.kind != SnapKind::None) {
        ctx().setSnapMarker(m_cursor.toSnapResult());
    } else {
        ctx().setSnapMarker(std::nullopt);
    }

    repaint();
    return true;
}

bool DrawLineTool::keyPress(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        finish();
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        // 至少放置了一个点即视为正常结束，否则等同取消
        finish();
        return true;
    case Qt::Key_U:
        undoLastSegment();
        return true;
    default:
        return false;
    }
}

bool DrawLineTool::textInput(const QString& text)
{
    if (matchesOption(text, QStringLiteral("U"))) {
        undoLastSegment();
        return true;
    }

    const std::optional<Point2D> base = m_vertices.empty()
        ? std::nullopt
        : std::optional<Point2D>(m_vertices.back());

    const std::optional<Point2D> point = parsePointInput(text, base);
    if (!point) return true;          // 已输出错误信息，视为已处理

    if (m_vertices.empty()) {
        m_vertices.push_back(*point);
    } else {
        createSegment(m_vertices.back(), *point);
        m_vertices.push_back(*point);
    }
    notifyPromptChanged();
    repaint();
    return true;
}

void DrawLineTool::createSegment(const Point2D& from, const Point2D& to)
{
    if (from.distanceTo(to) <= kGeomTol) return;      // 零长线段直接忽略

    auto line = std::make_unique<LineEntity>(from, to);
    applyDefaults(*line, ctx());

    const EntityId id = ctx().document->addEntity(std::move(line));
    if (id != kInvalidEntityId) m_created.push_back(id);
}

void DrawLineTool::undoLastSegment()
{
    if (!m_created.empty()) {
        ctx().document->removeEntity(m_created.back());
        m_created.pop_back();
    }
    if (!m_vertices.empty()) m_vertices.pop_back();

    notifyPromptChanged();
    repaint();
}

void DrawLineTool::paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context)
{
    // 已确定的折线
    for (std::size_t i = 0; i + 1 < m_vertices.size(); ++i) {
        const Point2D a = context.view->toDevice(m_vertices[i]);
        const Point2D b = context.view->toDevice(m_vertices[i + 1]);
        QPen pen(kPreviewColor);
        pen.setWidthF(1.2);
        pen.setCosmetic(true);
        painter.save();
        painter.setPen(pen);
        painter.drawLine(QPointF(a.x, a.y), QPointF(b.x, b.y));
        painter.restore();
    }

    // 从最后一点到光标的橡皮筋
    if (!m_vertices.empty() && m_hasCursor) {
        drawRubberBand(painter, context, m_vertices.back(), m_cursor.position);
    }
}

void DrawLineTool::deactivate()
{
    ctx().setSnapMarker(std::nullopt);
    if (!m_created.empty()) {
        message(QStringLiteral("已绘制 %1 条直线。").arg(m_created.size()));
    }
}

// =============================================================================
//  圆
// =============================================================================
DrawCircleTool::DrawCircleTool(ToolContext& context)
    : Tool(context)
{
}

QString DrawCircleTool::name() const { return QStringLiteral("CIRCLE"); }
QString DrawCircleTool::displayName() const { return QStringLiteral("圆"); }

QString DrawCircleTool::prompt() const
{
    switch (m_mode) {
    case Mode::CenterRadius:
        return m_points.empty()
            ? QStringLiteral("指定圆心 或 [三点(3P)/两点(2P)/直径(D)]:")
            : QStringLiteral("指定半径 或 [直径(D)]:");
    case Mode::ThreePoint:
        return QStringLiteral("指定圆周上的第 %1 个点 [圆弧上三点]:").arg(m_points.size() + 1);
    case Mode::TwoPoint:
        return QStringLiteral("指定直径的第 %1 个端点:").arg(m_points.size() + 1);
    case Mode::DiameterTyped:
        return QStringLiteral("输入直径:");
    }
    return QString();
}

void DrawCircleTool::reset()
{
    m_mode = Mode::CenterRadius;
    m_points.clear();
    notifyPromptChanged();
    repaint();
}

void DrawCircleTool::commitCircle(const Point2D& center, double radius)
{
    if (radius <= kGeomTol) {
        message(QStringLiteral("半径必须大于 0，本操作被忽略。"));
        return;
    }

    auto circle = std::make_unique<CircleEntity>(center, radius);
    applyDefaults(*circle, ctx());
    ctx().document->addEntity(std::move(circle));

    message(QStringLiteral("已创建圆：半径 %1").arg(radius, 0, 'g', 6));
}

bool DrawCircleTool::mousePress(const CanvasEvent& event)
{
    if (event.button != Qt::LeftButton || !ctx().document) return false;

    const std::optional<Point2D> base = m_points.empty()
        ? std::nullopt
        : std::optional<Point2D>(m_points.back());
    const ResolvedPoint resolved = resolve(event.worldPos, base);

    switch (m_mode) {
    case Mode::CenterRadius:
        if (m_points.empty()) {
            m_points.push_back(resolved.position);
            notifyPromptChanged();
        } else {
            commitCircle(m_points.front(), m_points.front().distanceTo(resolved.position));
            reset();
        }
        repaint();
        return true;

    case Mode::ThreePoint:
        m_points.push_back(resolved.position);
        if (m_points.size() == 3) {
            const auto arc = ArcEntity::fromThreePoints(m_points[0], m_points[1], m_points[2]);
            if (!arc) {
                message(QStringLiteral("三点共线，无法确定圆。请重新指定。"));
                m_points.clear();
            } else {
                auto circle = std::make_unique<CircleEntity>(arc->center(), arc->radius());
                applyDefaults(*circle, ctx());
                ctx().document->addEntity(std::move(circle));
                message(QStringLiteral("已创建圆：半径 %1").arg(arc->radius(), 0, 'g', 6));
                reset();
                return true;
            }
        }
        notifyPromptChanged();
        repaint();
        return true;

    case Mode::TwoPoint:
        m_points.push_back(resolved.position);
        if (m_points.size() == 2) {
            const Point2D center((m_points[0].x + m_points[1].x) * 0.5,
                                 (m_points[0].y + m_points[1].y) * 0.5);
            commitCircle(center, m_points[0].distanceTo(m_points[1]) * 0.5);
            reset();
            return true;
        }
        notifyPromptChanged();
        repaint();
        return true;

    case Mode::DiameterTyped:
        // 该模式只接受数值输入
        message(QStringLiteral("请直接输入直径数值。"));
        return true;
    }

    return true;
}

bool DrawCircleTool::mouseMove(const CanvasEvent& event)
{
    const std::optional<Point2D> base = m_points.empty()
        ? std::nullopt
        : std::optional<Point2D>(m_points.back());
    m_cursor = resolve(event.worldPos, base);
    m_hasCursor = true;

    ctx().setSnapMarker(m_cursor.kind != SnapKind::None ? std::optional<SnapResult>(m_cursor.toSnapResult())
                                                        : std::nullopt);
    repaint();
    return true;
}

bool DrawCircleTool::keyPress(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Return
        || event->key() == Qt::Key_Enter) {
        finish();
        return true;
    }
    return false;
}

bool DrawCircleTool::textInput(const QString& text)
{
    // 选项
    if (m_mode == Mode::CenterRadius) {
        if (matchesOption(text, QStringLiteral("3P"))) {
            m_mode = Mode::ThreePoint;
            m_points.clear();
            notifyPromptChanged();
            return true;
        }
        if (matchesOption(text, QStringLiteral("2P"))) {
            m_mode = Mode::TwoPoint;
            m_points.clear();
            notifyPromptChanged();
            return true;
        }
        if (matchesOption(text, QStringLiteral("D"))) {
            m_mode = Mode::DiameterTyped;
            m_points.clear();
            notifyPromptChanged();
            return true;
        }
    }

    if (m_mode == Mode::DiameterTyped) {
        const std::optional<double> diameter = parseScalarInput(text, QStringLiteral("直径"));
        if (!diameter) return true;
        const double radius = *diameter * 0.5;
        if (radius <= kGeomTol) {
            message(QStringLiteral("直径必须大于 0。"));
            return true;
        }
        if (m_points.empty()) m_points.push_back(Point2D(0.0, 0.0));
        commitCircle(m_points.front(), radius);
        reset();
        return true;
    }

    // 半径阶段直接输入数值
    if (m_mode == Mode::CenterRadius && m_points.size() == 1) {
        if (const std::optional<double> radius = parseScalarInput(text, QStringLiteral("半径"))) {
            commitCircle(m_points.front(), *radius);
            reset();
            return true;
        }
        return true;
    }

    // 坐标输入
    const std::optional<Point2D> base = m_points.empty() ? std::nullopt
                                                         : std::optional<Point2D>(m_points.back());
    const std::optional<Point2D> point = parsePointInput(text, base);
    if (!point) return true;

    CanvasEvent synthetic;
    synthetic.worldPos = *point;
    synthetic.button = Qt::LeftButton;
    return mousePress(synthetic);
}

void DrawCircleTool::paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context)
{
    if (!m_hasCursor) return;

    switch (m_mode) {
    case Mode::CenterRadius:
        if (m_points.size() == 1) {
            const double radius = m_points.front().distanceTo(m_cursor.position);
            if (radius > kGeomTol) {
                CircleEntity preview(m_points.front(), radius);
                drawPreviewEntity(painter, preview, context);
            }
            drawRubberBand(painter, context, m_points.front(), m_cursor.position);
        }
        break;

    case Mode::TwoPoint:
        if (m_points.size() == 1) {
            const Point2D center((m_points[0].x + m_cursor.position.x) * 0.5,
                                 (m_points[0].y + m_cursor.position.y) * 0.5);
            const double radius = m_points[0].distanceTo(m_cursor.position) * 0.5;
            if (radius > kGeomTol) {
                CircleEntity preview(center, radius);
                drawPreviewEntity(painter, preview, context);
            }
        }
        break;

    case Mode::ThreePoint:
        if (m_points.size() == 2) {
            const auto arc = ArcEntity::fromThreePoints(m_points[0], m_points[1], m_cursor.position);
            if (arc) {
                CircleEntity preview(arc->center(), arc->radius());
                drawPreviewEntity(painter, preview, context);
            }
        } else if (m_points.size() == 1) {
            drawRubberBand(painter, context, m_points.front(), m_cursor.position);
        }
        break;

    case Mode::DiameterTyped:
        break;
    }
}

void DrawCircleTool::deactivate()
{
    ctx().setSnapMarker(std::nullopt);
}

// =============================================================================
//  圆弧
// =============================================================================
DrawArcTool::DrawArcTool(ToolContext& context)
    : Tool(context)
{
}

QString DrawArcTool::name() const { return QStringLiteral("ARC"); }
QString DrawArcTool::displayName() const { return QStringLiteral("圆弧"); }

QString DrawArcTool::prompt() const
{
    if (m_mode == Mode::ThreePoint) {
        static const char* kNames[3] = { "起点", "圆弧上的第二点", "终点" };
        return QStringLiteral("指定%1 [圆心(C)]:")
            .arg(QString::fromUtf8(kNames[std::min<std::size_t>(m_points.size(), 2)]));
    }
    static const char* kCenterNames[3] = { "圆心", "起点", "终点" };
    return QStringLiteral("指定%1:")
        .arg(QString::fromUtf8(kCenterNames[std::min<std::size_t>(m_points.size(), 2)]));
}

void DrawArcTool::reset()
{
    m_points.clear();
    notifyPromptChanged();
    repaint();
}

void DrawArcTool::commit()
{
    if (m_points.size() != 3) return;

    std::optional<ArcEntity> arc;
    if (m_mode == Mode::ThreePoint) {
        arc = ArcEntity::fromThreePoints(m_points[0], m_points[1], m_points[2]);
        if (!arc) {
            message(QStringLiteral("三点共线，无法确定圆弧。请重新指定。"));
            reset();
            return;
        }
    } else {
        arc = ArcEntity::fromCenterStartEnd(m_points[0], m_points[1], m_points[2]);
        if (!arc) {
            message(QStringLiteral("半径为零，无法确定圆弧。请重新指定。"));
            reset();
            return;
        }
    }

    auto entity = std::make_unique<ArcEntity>(std::move(*arc));
    applyDefaults(*entity, ctx());
    ctx().document->addEntity(std::move(entity));

    message(QStringLiteral("已创建圆弧：半径 %1，包角 %2°")
                .arg(arc->radius(), 0, 'g', 6)
                .arg(radToDeg(arc->spanAngle()), 0, 'f', 2));
    reset();
}

bool DrawArcTool::mousePress(const CanvasEvent& event)
{
    if (event.button != Qt::LeftButton || !ctx().document) return false;

    const std::optional<Point2D> base = m_points.empty()
        ? std::nullopt
        : std::optional<Point2D>(m_points.back());
    const ResolvedPoint resolved = resolve(event.worldPos, base);

    m_points.push_back(resolved.position);
    if (m_points.size() == 3) commit();

    notifyPromptChanged();
    repaint();
    return true;
}

bool DrawArcTool::mouseMove(const CanvasEvent& event)
{
    const std::optional<Point2D> base = m_points.empty()
        ? std::nullopt
        : std::optional<Point2D>(m_points.back());
    m_cursor = resolve(event.worldPos, base);
    m_hasCursor = true;

    ctx().setSnapMarker(m_cursor.kind != SnapKind::None ? std::optional<SnapResult>(m_cursor.toSnapResult())
                                                        : std::nullopt);
    repaint();
    return true;
}

bool DrawArcTool::keyPress(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Return
        || event->key() == Qt::Key_Enter) {
        finish();
        return true;
    }
    return false;
}

bool DrawArcTool::textInput(const QString& text)
{
    if (m_points.empty() && matchesOption(text, QStringLiteral("C"))) {
        m_mode = Mode::CenterStartEnd;
        notifyPromptChanged();
        return true;
    }

    const std::optional<Point2D> base = m_points.empty() ? std::nullopt
                                                         : std::optional<Point2D>(m_points.back());
    const std::optional<Point2D> point = parsePointInput(text, base);
    if (!point) return true;

    CanvasEvent synthetic;
    synthetic.worldPos = *point;
    synthetic.button = Qt::LeftButton;
    return mousePress(synthetic);
}

void DrawArcTool::paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context)
{
    if (!m_hasCursor) return;

    if (m_points.size() == 2) {
        std::optional<ArcEntity> preview;
        if (m_mode == Mode::ThreePoint) {
            preview = ArcEntity::fromThreePoints(m_points[0], m_points[1], m_cursor.position);
        } else {
            preview = ArcEntity::fromCenterStartEnd(m_points[0], m_points[1], m_cursor.position);
        }
        if (preview) drawPreviewEntity(painter, *preview, context);
    } else if (m_points.size() == 1) {
        drawRubberBand(painter, context, m_points.front(), m_cursor.position);
    }
}

void DrawArcTool::deactivate()
{
    ctx().setSnapMarker(std::nullopt);
}

// =============================================================================
//  多段线
// =============================================================================
DrawPolylineTool::DrawPolylineTool(ToolContext& context)
    : Tool(context)
{
}

QString DrawPolylineTool::name() const { return QStringLiteral("PLINE"); }
QString DrawPolylineTool::displayName() const { return QStringLiteral("多段线"); }

QString DrawPolylineTool::prompt() const
{
    if (m_vertices.empty()) return QStringLiteral("指定起点:");
    return QStringLiteral("指定下一点 [闭合(C)/放弃(U)] <回车结束>:");
}

void DrawPolylineTool::commit(bool closed)
{
    if (m_vertices.size() < 2) {
        message(QStringLiteral("至少需要两个点才能构成多段线。"));
        return;
    }

    auto polyline = std::make_unique<PolylineEntity>(m_vertices, closed);
    applyDefaults(*polyline, ctx());
    ctx().document->addEntity(std::move(polyline));

    message(QStringLiteral("已创建多段线：%1 个顶点%2")
                .arg(m_vertices.size())
                .arg(closed ? QStringLiteral("（闭合）") : QString()));
    m_vertices.clear();
}

bool DrawPolylineTool::mousePress(const CanvasEvent& event)
{
    if (event.button != Qt::LeftButton || !ctx().document) return false;

    const std::optional<Point2D> base = m_vertices.empty()
        ? std::nullopt
        : std::optional<Point2D>(m_vertices.back());
    const ResolvedPoint resolved = resolve(event.worldPos, base);

    m_vertices.push_back(resolved.position);
    notifyPromptChanged();
    repaint();
    return true;
}

bool DrawPolylineTool::mouseMove(const CanvasEvent& event)
{
    const std::optional<Point2D> base = m_vertices.empty()
        ? std::nullopt
        : std::optional<Point2D>(m_vertices.back());
    m_cursor = resolve(event.worldPos, base);
    m_hasCursor = true;

    ctx().setSnapMarker(m_cursor.kind != SnapKind::None ? std::optional<SnapResult>(m_cursor.toSnapResult())
                                                        : std::nullopt);
    repaint();
    return true;
}

bool DrawPolylineTool::keyPress(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        m_vertices.clear();
        finish();
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        commit(false);
        finish();
        return true;
    case Qt::Key_U:
        if (!m_vertices.empty()) m_vertices.pop_back();
        notifyPromptChanged();
        repaint();
        return true;
    case Qt::Key_C:
        if (m_vertices.size() >= 3) {
            commit(true);
            finish();
        }
        return true;
    default:
        return false;
    }
}

bool DrawPolylineTool::textInput(const QString& text)
{
    if (matchesOption(text, QStringLiteral("C"))) {
        if (m_vertices.size() >= 3) {
            commit(true);
            finish();
        } else {
            message(QStringLiteral("闭合多段线至少需要 3 个顶点。"));
        }
        return true;
    }
    if (matchesOption(text, QStringLiteral("U"))) {
        if (!m_vertices.empty()) m_vertices.pop_back();
        notifyPromptChanged();
        repaint();
        return true;
    }

    const std::optional<Point2D> base = m_vertices.empty() ? std::nullopt
                                                           : std::optional<Point2D>(m_vertices.back());
    const std::optional<Point2D> point = parsePointInput(text, base);
    if (!point) return true;

    CanvasEvent synthetic;
    synthetic.worldPos = *point;
    synthetic.button = Qt::LeftButton;
    return mousePress(synthetic);
}

void DrawPolylineTool::paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context)
{
    if (m_vertices.size() < 2) {
        if (!m_vertices.empty() && m_hasCursor) {
            drawRubberBand(painter, context, m_vertices.front(), m_cursor.position);
        }
        return;
    }

    std::vector<Point2D> preview = m_vertices;
    if (m_hasCursor) preview.push_back(m_cursor.position);

    QPainterPath path;
    const Point2D first = context.view->toDevice(preview.front());
    path.moveTo(first.x, first.y);
    for (std::size_t i = 1; i < preview.size(); ++i) {
        const Point2D d = context.view->toDevice(preview[i]);
        path.lineTo(d.x, d.y);
    }

    QPen pen(kPreviewColor);
    pen.setWidthF(1.2);
    pen.setStyle(Qt::DashLine);
    pen.setCosmetic(true);

    painter.save();
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
    painter.restore();
}

void DrawPolylineTool::deactivate()
{
    ctx().setSnapMarker(std::nullopt);
}

// =============================================================================
//  点
// =============================================================================
DrawPointTool::DrawPointTool(ToolContext& context)
    : Tool(context)
{
}

QString DrawPointTool::name() const { return QStringLiteral("POINT"); }
QString DrawPointTool::displayName() const { return QStringLiteral("点"); }

QString DrawPointTool::prompt() const
{
    return QStringLiteral("指定点位置 <回车结束>%1:")
        .arg(m_placed > 0 ? QStringLiteral("（已放置 %1 个）").arg(m_placed) : QString());
}

bool DrawPointTool::mousePress(const CanvasEvent& event)
{
    if (event.button != Qt::LeftButton || !ctx().document) return false;

    const ResolvedPoint resolved = resolve(event.worldPos);

    auto point = std::make_unique<PointEntity>(resolved.position);
    applyDefaults(*point, ctx());
    ctx().document->addEntity(std::move(point));
    ++m_placed;

    notifyPromptChanged();
    repaint();
    return true;
}

bool DrawPointTool::mouseMove(const CanvasEvent& event)
{
    m_cursor = resolve(event.worldPos);
    m_hasCursor = true;
    ctx().setSnapMarker(m_cursor.kind != SnapKind::None ? std::optional<SnapResult>(m_cursor.toSnapResult())
                                                        : std::nullopt);
    repaint();
    return true;
}

bool DrawPointTool::keyPress(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Return
        || event->key() == Qt::Key_Enter) {
        finish();
        return true;
    }
    return false;
}

bool DrawPointTool::textInput(const QString& text)
{
    const std::optional<Point2D> point = parsePointInput(text, std::nullopt);
    if (!point) return true;

    CanvasEvent synthetic;
    synthetic.worldPos = *point;
    synthetic.button = Qt::LeftButton;
    return mousePress(synthetic);
}

void DrawPointTool::deactivate()
{
    ctx().setSnapMarker(std::nullopt);
    if (m_placed > 0) message(QStringLiteral("已放置 %1 个点。").arg(m_placed));
}

// =============================================================================
//  矩形
// =============================================================================
DrawRectangleTool::DrawRectangleTool(ToolContext& context)
    : Tool(context)
{
}

QString DrawRectangleTool::name() const { return QStringLiteral("RECTANGLE"); }
QString DrawRectangleTool::displayName() const { return QStringLiteral("矩形"); }

QString DrawRectangleTool::prompt() const
{
    return m_first ? QStringLiteral("指定另一个角点 <回车结束>:")
                   : QStringLiteral("指定第一个角点:");
}

bool DrawRectangleTool::mousePress(const CanvasEvent& event)
{
    if (event.button != Qt::LeftButton || !ctx().document) return false;

    const ResolvedPoint resolved = resolve(event.worldPos, m_first);

    if (!m_first) {
        m_first = resolved.position;
        notifyPromptChanged();
        repaint();
        return true;
    }

    const Point2D& a = *m_first;
    const Point2D& b = resolved.position;

    if (std::abs(a.x - b.x) <= kGeomTol || std::abs(a.y - b.y) <= kGeomTol) {
        message(QStringLiteral("退化的矩形（宽或高为零），已忽略。"));
        m_first.reset();
        notifyPromptChanged();
        repaint();
        return true;
    }

    auto rectangle = std::make_unique<PolylineEntity>(
        PolylineEntity::makeRectangle(a, b));
    applyDefaults(*rectangle, ctx());
    ctx().document->addEntity(std::move(rectangle));

    message(QStringLiteral("已创建矩形：%1 × %2")
                .arg(std::abs(b.x - a.x), 0, 'g', 6)
                .arg(std::abs(b.y - a.y), 0, 'g', 6));

    m_first.reset();
    notifyPromptChanged();
    repaint();
    return true;
}

bool DrawRectangleTool::mouseMove(const CanvasEvent& event)
{
    m_cursor = resolve(event.worldPos, m_first);
    m_hasCursor = true;
    ctx().setSnapMarker(m_cursor.kind != SnapKind::None ? std::optional<SnapResult>(m_cursor.toSnapResult())
                                                        : std::nullopt);
    repaint();
    return true;
}

bool DrawRectangleTool::keyPress(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Return
        || event->key() == Qt::Key_Enter) {
        finish();
        return true;
    }
    return false;
}

bool DrawRectangleTool::textInput(const QString& text)
{
    const std::optional<Point2D> point = parsePointInput(text, m_first);
    if (!point) return true;

    CanvasEvent synthetic;
    synthetic.worldPos = *point;
    synthetic.button = Qt::LeftButton;
    return mousePress(synthetic);
}

void DrawRectangleTool::paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context)
{
    if (!m_first || !m_hasCursor) return;
    PolylineEntity preview = PolylineEntity::makeRectangle(*m_first, m_cursor.position);
    drawPreviewEntity(painter, preview, context);
}

void DrawRectangleTool::deactivate()
{
    ctx().setSnapMarker(std::nullopt);
}

// =============================================================================
//  椭圆
// =============================================================================
DrawEllipseTool::DrawEllipseTool(ToolContext& context)
    : Tool(context)
{
}

QString DrawEllipseTool::name() const { return QStringLiteral("ELLIPSE"); }
QString DrawEllipseTool::displayName() const { return QStringLiteral("椭圆"); }

QString DrawEllipseTool::prompt() const
{
    switch (m_points.size()) {
    case 0:
        return m_mode == Mode::AxisEnds
                   ? QStringLiteral("指定椭圆的轴端点 [中心(C)]:")
                   : QStringLiteral("指定椭圆的中心点:");
    case 1:
        return m_mode == Mode::AxisEnds ? QStringLiteral("指定轴的另一个端点:")
                                        : QStringLiteral("指定轴的端点:");
    default:
        return QStringLiteral("指定另一条半轴长度 <回车结束>:");
    }
}

std::optional<DrawEllipseTool::Axis> DrawEllipseTool::currentAxis() const
{
    if (m_points.size() < 2) return std::nullopt;

    Axis axis;
    if (m_mode == Mode::AxisEnds) {
        // 两个轴端点：中心取中点，半轴取两端点之差的一半
        axis.center = Point2D((m_points[0].x + m_points[1].x) * 0.5,
                              (m_points[0].y + m_points[1].y) * 0.5);
        axis.halfAxis = Vector2D((m_points[1].x - m_points[0].x) * 0.5,
                                 (m_points[1].y - m_points[0].y) * 0.5);
    } else {
        axis.center = m_points[0];
        axis.halfAxis = Vector2D(m_points[1].x - m_points[0].x,
                                 m_points[1].y - m_points[0].y);
    }

    if (!(axis.halfAxis.length() > kGeomTol)) return std::nullopt;   // 两点重合
    return axis;
}

std::optional<double> DrawEllipseTool::otherHalfLengthFrom(const Point2D& reference) const
{
    const std::optional<Axis> axis = currentAxis();
    if (!axis) return std::nullopt;

    // 参考点到轴线的垂直距离就是「另一条半轴长度」
    const Vector2D direction = axis->halfAxis.normalizedOr(Vector2D(1.0, 0.0));
    const double other = std::abs(cross(reference - axis->center, direction));
    if (!(other > kGeomTol)) return std::nullopt;
    return other;
}

DrawEllipseTool::StepPoint DrawEllipseTool::resolveStep(const Point2D& raw) const
{
    StepPoint step;
    const std::optional<Point2D> base = m_points.empty()
        ? std::nullopt
        : std::optional<Point2D>(m_points.back());
    step.point = resolve(raw, base);

    // 正在定这条轴的方向（轴端点法的第二个点，或中心点法里从中心出发的那个点）：
    // 方向吸附到上下左右，用户不必靠肉眼把轴摆平
    if (m_points.size() == 1) {
        Vector2D direction;
        if (const std::optional<Point2D> snapped =
                snapToCardinalDirection(m_points.front(), step.point.position,
                                        ctx().apertureWorld(), direction)) {
            step.point.position = *snapped;
            step.point.kind = SnapKind::Ortho;
            step.point.label = std::abs(direction.x) > 0.5 ? QStringLiteral("水平")
                                                           : QStringLiteral("竖直");
            step.directionSnapped = true;
        }
    }
    return step;
}

std::unique_ptr<Entity> DrawEllipseTool::makePreview(const Point2D& reference) const
{
    const std::optional<Axis> axis = currentAxis();
    const std::optional<double> other = otherHalfLengthFrom(reference);
    if (!axis || !other) return nullptr;

    const EllipseEntity::HalfAxes axes =
        EllipseEntity::normalizeHalfAxes(axis->halfAxis, *other);
    return std::make_unique<EllipseEntity>(axis->center,
                                           Point2D(axes.majorAxisEnd.x, axes.majorAxisEnd.y),
                                           axes.ratio, 0.0, kTwoPi);
}

void DrawEllipseTool::commit(const Point2D& reference)
{
    const std::optional<Axis> axis = currentAxis();
    if (!axis) {
        message(QStringLiteral("轴的两个端点重合，无法确定椭圆，已取消。"));
        reset();
        return;
    }

    const std::optional<double> other = otherHalfLengthFrom(reference);
    if (!other) {
        message(QStringLiteral("另一条半轴长度为 0（椭圆退化成线段），已忽略。"));
        reset();
        return;
    }

    // 用户先画的那条轴未必是长轴，交给实体的归一化助手决定谁当主轴
    const EllipseEntity::HalfAxes axes =
        EllipseEntity::normalizeHalfAxes(axis->halfAxis, *other);
    auto ellipse = std::make_unique<EllipseEntity>(axis->center,
                                                   Point2D(axes.majorAxisEnd.x,
                                                           axes.majorAxisEnd.y),
                                                   axes.ratio, 0.0, kTwoPi);
    applyDefaults(*ellipse, ctx());
    ctx().document->addEntity(std::move(ellipse));

    message(QStringLiteral("已创建椭圆：长半轴 %1，短半轴 %2")
                .arg(axes.majorAxisEnd.length(), 0, 'g', 6)
                .arg(axes.majorAxisEnd.length() * axes.ratio, 0, 'g', 6));

    reset();
}

void DrawEllipseTool::reset()
{
    // 与圆工具一致：一次绘制结束后回到默认方式，选项 C 只对本次生效
    m_mode = Mode::AxisEnds;
    m_points.clear();
    m_directionSnapped = false;
    notifyPromptChanged();
    repaint();
}

bool DrawEllipseTool::mousePress(const CanvasEvent& event)
{
    if (event.button != Qt::LeftButton || !ctx().document) return false;

    const ResolvedPoint resolved = resolveStep(event.worldPos).point;

    if (m_points.size() < 2) {
        m_points.push_back(resolved.position);
        notifyPromptChanged();
        repaint();
        return true;
    }

    commit(resolved.position);   // 第三点只用来量长度，不落在图上
    return true;
}

bool DrawEllipseTool::mouseMove(const CanvasEvent& event)
{
    const StepPoint step = resolveStep(event.worldPos);
    m_cursor = step.point;
    m_hasCursor = true;
    m_directionSnapped = step.directionSnapped;

    std::optional<SnapResult> marker;
    if (step.directionSnapped || m_cursor.isObjectSnap()) {
        marker = m_cursor.toSnapResult();          // 「水平 / 竖直」或对象捕捉
    } else if (const std::optional<Axis> axis = currentAxis()) {
        // 定另一条半轴时，把光标沿垂直方向投到该轴上并标出来：
        // 这一步量的是「垂距」，光标本身可以离得很远，投影点才是真正在量的位置
        const Vector2D normal = axis->halfAxis.normalizedOr(Vector2D(1.0, 0.0)).perpendicular();
        const double along = dot(m_cursor.position - axis->center, normal);
        marker = SnapResult{ axis->center + normal * along, SnapKind::Perpendicular,
                             QStringLiteral("半轴") };
    }
    ctx().setSnapMarker(marker);

    repaint();
    return true;
}

bool DrawEllipseTool::keyPress(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Return
        || event->key() == Qt::Key_Enter) {
        finish();
        return true;
    }
    return false;
}

bool DrawEllipseTool::textInput(const QString& text)
{
    // 第一步的选项：C 改用中心点方式
    if (m_points.empty() && matchesOption(text, QStringLiteral("C"))) {
        m_mode = Mode::Center;
        message(QStringLiteral("中心点方式：先指定中心，再指定一个轴的端点。"));
        notifyPromptChanged();
        repaint();
        return true;
    }

    const std::optional<Point2D> base = m_points.empty()
        ? std::nullopt
        : std::optional<Point2D>(m_points.back());

    if (m_points.size() < 2) {
        const std::optional<Point2D> point = parsePointInput(text, base);
        if (!point) return true;

        // 直接收下这个点，不走 mousePress：键盘输入的数字是「我要这个精确值」
        // 的意思，不该被方向吸附改掉（吸附只服务于鼠标的模糊指向）
        m_points.push_back(*point);
        notifyPromptChanged();
        repaint();
        return true;
    }

    // 第三步是长度而不是点：把长度折成「沿垂直方向偏移该长度的一点」
    const std::optional<double> length =
        parseScalarInput(text, QStringLiteral("另一条半轴长度"));
    if (!length) return true;

    const std::optional<Axis> axis = currentAxis();
    if (!axis) {
        message(QStringLiteral("轴的两个端点重合，无法确定椭圆，已取消。"));
        reset();
        return true;
    }
    const Vector2D normal = axis->halfAxis.normalizedOr(Vector2D(1.0, 0.0)).perpendicular();
    commit(axis->center + normal * (*length));
    return true;
}

void DrawEllipseTool::paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context)
{
    if (!m_hasCursor || m_points.empty()) return;

    // 轴还没定：橡皮筋 + （吸附到正方向时）过基点的长追踪线
    if (m_points.size() == 1) {
        drawRubberBand(painter, context, m_points.front(), m_cursor.position);
        if (m_directionSnapped) {
            drawTrackingLine(painter, context, m_points.front(), m_cursor.position);
        }
        return;
    }

    const std::optional<Axis> axis = currentAxis();
    if (!axis) return;

    // 轴已定：整条轴画全（中心 ± 半轴），中心点法下这样才对称
    const Vector2D direction = axis->halfAxis.normalizedOr(Vector2D(1.0, 0.0));
    const Vector2D normal = direction.perpendicular();
    drawRubberBand(painter, context, axis->center - direction * axis->halfAxis.length(),
                   axis->center + direction * axis->halfAxis.length());

    // 另一条半轴：端点标出来，让人一眼看清椭圆会伸到哪里
    if (const std::optional<double> other = otherHalfLengthFrom(m_cursor.position)) {
        const Point2D tipA = axis->center - normal * (*other);
        const Point2D tipB = axis->center + normal * (*other);
        drawRubberBand(painter, context, tipA, tipB);
        drawTipMarker(painter, context, tipA);
        drawTipMarker(painter, context, tipB);
    }

    if (std::unique_ptr<Entity> preview = makePreview(m_cursor.position)) {
        drawPreviewEntity(painter, *preview, context);
    }
}

void DrawEllipseTool::deactivate()
{
    ctx().setSnapMarker(std::nullopt);
}

// =============================================================================
//  构造线 / 射线
// =============================================================================
DrawInfiniteLineTool::DrawInfiniteLineTool(ToolContext& context, EntityType type)
    : Tool(context), m_type(type)
{
    Q_ASSERT(type == EntityType::XLine || type == EntityType::Ray);
}

QString DrawInfiniteLineTool::name() const
{
    return isRay() ? QStringLiteral("RAY") : QStringLiteral("XLINE");
}

QString DrawInfiniteLineTool::displayName() const
{
    return isRay() ? QStringLiteral("射线") : QStringLiteral("构造线");
}

QString DrawInfiniteLineTool::prompt() const
{
    // 方向已锁定（H/V）：第一个点就是通过点本身
    if (m_lockedDirection) return QStringLiteral("指定通过点:");
    if (!m_base) {
        return isRay() ? QStringLiteral("指定起点:")
                       : QStringLiteral("指定点或 [水平(H)/垂直(V)]:");
    }
    return QStringLiteral("指定通过点:");
}

DrawInfiniteLineTool::StepPoint DrawInfiniteLineTool::resolveStep(const Point2D& raw) const
{
    StepPoint step;
    step.point = resolve(raw, m_base);

    // 定方向：吸附到上下左右四个正方向，用户不必靠肉眼把辅助线摆平
    if (m_base) {
        Vector2D direction;
        if (const std::optional<Point2D> snapped =
                snapToCardinalDirection(*m_base, step.point.position,
                                        ctx().apertureWorld(), direction)) {
            step.point.position = *snapped;
            step.point.kind = SnapKind::Ortho;
            step.point.label = std::abs(direction.x) > 0.5 ? QStringLiteral("水平")
                                                           : QStringLiteral("竖直");
            step.directionSnapped = true;
        }
    }
    return step;
}

void DrawInfiniteLineTool::commit(const Point2D& base, const Point2D& through)
{
    const Vector2D direction = through - base;
    if (direction.length() <= kGeomTol) {
        // 方向为零的构造线画不出来也选不中，宁可拒绝并说清楚
        message(QStringLiteral("通过点与定位点重合，方向无法确定，已忽略。"));
        return;
    }

    std::unique_ptr<InfiniteLineEntity> entity;
    if (isRay()) entity = std::make_unique<RayEntity>(base, direction);
    else         entity = std::make_unique<XLineEntity>(base, direction);

    applyDefaults(*entity, ctx());
    ctx().document->addEntity(std::move(entity));
}

std::unique_ptr<Entity> DrawInfiniteLineTool::makePreview(const Point2D& through) const
{
    Point2D base;
    Vector2D direction;

    if (m_lockedDirection) {
        // 方向已锁定：通过点即定位点
        base = through;
        direction = *m_lockedDirection;
    } else if (m_base) {
        base = *m_base;
        direction = through - *m_base;
    } else {
        return nullptr;
    }

    if (direction.length() <= kGeomTol) return nullptr;

    if (isRay()) return std::make_unique<RayEntity>(base, direction);
    return std::make_unique<XLineEntity>(base, direction);
}

void DrawInfiniteLineTool::reset()
{
    // 与其它绘制工具一致：一次绘制结束后回到默认方式，H/V 选项只对本次生效
    m_lockedDirection.reset();
    m_base.reset();
    m_directionSnapped = false;
    notifyPromptChanged();
    repaint();
}

bool DrawInfiniteLineTool::mousePress(const CanvasEvent& event)
{
    if (event.button != Qt::LeftButton || !ctx().document) return false;

    const StepPoint step = resolveStep(event.worldPos);

    // 方向已锁定：落点直接作为通过点，图元一次成型
    if (m_lockedDirection) {
        commit(step.point.position, step.point.position + *m_lockedDirection);
        reset();
        return true;
    }

    if (!m_base) {
        m_base = step.point.position;
        notifyPromptChanged();
        repaint();
        return true;
    }

    commit(*m_base, step.point.position);
    reset();      // 命令不结束，可继续画下一条（与 AutoCAD 一致）
    return true;
}

bool DrawInfiniteLineTool::mouseMove(const CanvasEvent& event)
{
    const StepPoint step = resolveStep(event.worldPos);
    m_cursor = step.point;
    m_hasCursor = true;
    m_directionSnapped = step.directionSnapped;

    if (step.directionSnapped || m_cursor.isObjectSnap()) {
        ctx().setSnapMarker(m_cursor.toSnapResult());
    } else {
        ctx().setSnapMarker(std::nullopt);
    }

    repaint();
    return true;
}

bool DrawInfiniteLineTool::keyPress(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
    case Qt::Key_Return:
    case Qt::Key_Enter:
        finish();
        return true;
    default:
        return false;
    }
}

bool DrawInfiniteLineTool::textInput(const QString& text)
{
    // 第一步的选项：构造线的 H 水平 / V 垂直。方向由此锁定，只需再给通过点。
    // （射线必须先有起点，AutoCAD 的 RAY 也没有这两个选项。）
    if (!isRay() && !m_lockedDirection && !m_base) {
        if (matchesOption(text, QStringLiteral("H")) || matchesOption(text, QStringLiteral("V"))) {
            const bool horizontal = matchesOption(text, QStringLiteral("H"));
            m_lockedDirection = horizontal ? Vector2D(1.0, 0.0) : Vector2D(0.0, 1.0);
            message(horizontal ? QStringLiteral("方向锁定为水平，指定通过点。")
                               : QStringLiteral("方向锁定为竖直，指定通过点。"));
            notifyPromptChanged();
            repaint();
            return true;
        }
    }

    const std::optional<Point2D> point = parsePointInput(text, m_base);
    if (!point) return true;          // 已输出错误信息，视为已处理

    // 键盘输入的数字是「我要这个精确值」，不参与方向吸附（吸附只服务于鼠标的模糊指向）
    if (m_lockedDirection) {
        commit(*point, *point + *m_lockedDirection);
        reset();
        return true;
    }

    if (!m_base) {
        m_base = *point;
        notifyPromptChanged();
        repaint();
        return true;
    }

    commit(*m_base, *point);
    reset();
    return true;
}

void DrawInfiniteLineTool::paintOverlay(QPainter& painter,
                                        const EntityRenderer::DrawContext& context)
{
    if (!m_hasCursor) return;

    // 方向已锁定但还没落点：只画一条过光标的预览线
    if (m_lockedDirection) {
        if (std::unique_ptr<Entity> preview = makePreview(m_cursor.position)) {
            drawPreviewEntity(painter, *preview, context);
        }
        return;
    }

    if (!m_base) return;

    // 定位点已定：橡皮筋指向光标，吸附到正方向时再补一条贯穿视口的追踪线
    drawRubberBand(painter, context, *m_base, m_cursor.position);
    if (m_directionSnapped) {
        drawTrackingLine(painter, context, *m_base, m_cursor.position);
    }
    if (std::unique_ptr<Entity> preview = makePreview(m_cursor.position)) {
        drawPreviewEntity(painter, *preview, context);
    }
}

void DrawInfiniteLineTool::deactivate()
{
    ctx().setSnapMarker(std::nullopt);
}

// =============================================================================
//  表格
// =============================================================================
DrawTableTool::DrawTableTool(ToolContext& context)
    : Tool(context)
{
}

QString DrawTableTool::name() const { return QStringLiteral("TABLE"); }
QString DrawTableTool::displayName() const { return QStringLiteral("表格"); }

QString DrawTableTool::prompt() const
{
    if (m_pending == PendingNumber::Rows) {
        return QStringLiteral("输入行数 <%1>:").arg(m_rows);
    }
    if (m_pending == PendingNumber::Columns) {
        return QStringLiteral("输入列数 <%1>:").arg(m_columns);
    }
    if (!m_first) {
        return QStringLiteral("指定表格插入点 [行数(R)/列数(C)] <%1 行 × %2 列>:")
            .arg(m_rows)
            .arg(m_columns);
    }
    return QStringLiteral("指定表格另一个角点 <回车结束>:");
}

void DrawTableTool::spanOf(const Point2D& a, const Point2D& b, double& width, double& height)
{
    width = std::abs(b.x - a.x);
    height = std::abs(b.y - a.y);
}

void DrawTableTool::commit(const Point2D& insertion, const Point2D& opposite)
{
    double width = 0.0;
    double height = 0.0;
    spanOf(insertion, opposite, width, height);

    if (width <= kGeomTol || height <= kGeomTol) {
        message(QStringLiteral("退化的表格（宽或高为零），已忽略。"));
        m_first.reset();
        notifyPromptChanged();
        repaint();
        return;
    }

    // 插入点始终取左上角：用户从左下往右上拖时，把插入点上移到实际左上角。
    Point2D topLeft = insertion;
    if (opposite.y > insertion.y) topLeft.y = opposite.y;   // 向上展开
    if (opposite.x < insertion.x) topLeft.x = opposite.x;   // 向左展开

    auto table = std::make_unique<TableEntity>(m_rows, m_columns);
    table->setInsertionPoint(topLeft);
    // 行列尺寸按总尺寸均分（行列数由选项 R/C 设定）
    for (std::size_t r = 0; r < m_rows; ++r) {
        table->setRowHeight(r, height / static_cast<double>(m_rows));
    }
    for (std::size_t c = 0; c < m_columns; ++c) {
        table->setColumnWidth(c, width / static_cast<double>(m_columns));
    }
    applyDefaults(*table, ctx());
    ctx().document->addEntity(std::move(table));

    message(QStringLiteral("已创建表格：%1 行 × %2 列，%3 × %4")
                .arg(m_rows)
                .arg(m_columns)
                .arg(width, 0, 'g', 6)
                .arg(height, 0, 'g', 6));

    m_first.reset();
    notifyPromptChanged();
    repaint();
}

bool DrawTableTool::mousePress(const CanvasEvent& event)
{
    if (event.button != Qt::LeftButton || !ctx().document) return false;
    if (m_pending != PendingNumber::None) return true;   // 正在等待数值输入

    const ResolvedPoint resolved = resolve(event.worldPos, m_first);

    if (!m_first) {
        m_first = resolved.position;
        notifyPromptChanged();
        repaint();
        return true;
    }

    commit(*m_first, resolved.position);
    return true;
}

bool DrawTableTool::mouseMove(const CanvasEvent& event)
{
    m_cursor = resolve(event.worldPos, m_first);
    m_hasCursor = true;
    ctx().setSnapMarker(m_cursor.kind != SnapKind::None
                            ? std::optional<SnapResult>(m_cursor.toSnapResult())
                            : std::nullopt);
    repaint();
    return true;
}

bool DrawTableTool::keyPress(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Return
        || event->key() == Qt::Key_Enter) {
        finish();
        return true;
    }
    return false;
}

bool DrawTableTool::textInput(const QString& text)
{
    const QString trimmed = text.trimmed();

    // 正在等待行数 / 列数
    if (m_pending != PendingNumber::None) {
        bool ok = false;
        const int value = trimmed.toInt(&ok);
        if (!ok || value < 1) {
            message(QStringLiteral("请输入不小于 1 的整数。"));
            return true;
        }
        if (m_pending == PendingNumber::Rows)   m_rows = static_cast<std::size_t>(value);
        if (m_pending == PendingNumber::Columns) m_columns = static_cast<std::size_t>(value);
        m_pending = PendingNumber::None;
        notifyPromptChanged();
        repaint();
        return true;
    }

    // 选项关键字
    if (matchesOption(trimmed, QStringLiteral("R"))
        || matchesOption(trimmed, QStringLiteral("ROWS"))) {
        m_pending = PendingNumber::Rows;
        notifyPromptChanged();
        return true;
    }
    if (matchesOption(trimmed, QStringLiteral("C"))
        || matchesOption(trimmed, QStringLiteral("COLUMNS"))) {
        m_pending = PendingNumber::Columns;
        notifyPromptChanged();
        return true;
    }

    // 坐标输入
    const std::optional<Point2D> point = parsePointInput(trimmed, m_first);
    if (!point) return true;

    CanvasEvent synthetic;
    synthetic.worldPos = *point;
    synthetic.button = Qt::LeftButton;
    return mousePress(synthetic);
}

void DrawTableTool::paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context)
{
    if (!m_first || !m_hasCursor) return;

    double width = 0.0;
    double height = 0.0;
    spanOf(*m_first, m_cursor.position, width, height);
    if (width <= kGeomTol || height <= kGeomTol) return;

    Point2D topLeft = *m_first;
    if (m_cursor.position.y > m_first->y) topLeft.y = m_cursor.position.y;
    if (m_cursor.position.x < m_first->x) topLeft.x = m_cursor.position.x;

    TableEntity preview(m_rows, m_columns);
    preview.setInsertionPoint(topLeft);
    for (std::size_t r = 0; r < m_rows; ++r) {
        preview.setRowHeight(r, height / static_cast<double>(m_rows));
    }
    for (std::size_t c = 0; c < m_columns; ++c) {
        preview.setColumnWidth(c, width / static_cast<double>(m_columns));
    }
    drawPreviewEntity(painter, preview, context);
}

void DrawTableTool::deactivate()
{
    ctx().setSnapMarker(std::nullopt);
}

} // namespace lycad
