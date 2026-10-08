// =============================================================================
//  LyCAD - render/CadCanvas.cpp
// =============================================================================
#include "render/CadCanvas.h"

#include <QApplication>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace lycad {

namespace {

/// 滚轮每一档的缩放倍率
constexpr double kWheelZoomStep = 1.25;

/// 单方向最多绘制的网格线数量，防御性上限
constexpr int kMaxGridLines = 1200;

/// 素材：把浮点坐标转成 QPointF
inline QPointF toPointF(const Point2D& p)
{
    return QPointF(p.x, p.y);
}

} // namespace

CadCanvas::CadCanvas(QWidget* parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setMinimumSize(320, 240);

    m_display.background = palette().color(QPalette::Base);
}

CadCanvas::~CadCanvas() = default;

// -----------------------------------------------------------------------------
//  关联对象
// -----------------------------------------------------------------------------
void CadCanvas::setDocument(Document* document)
{
    if (m_document == document) return;

    if (m_document) disconnect(m_document, nullptr, this, nullptr);
    m_document = document;

    if (m_document) {
        // 文档内容变化都可能导致画面不同，统一触发重绘
        connect(m_document, &Document::entityAdded, this, [this] { update(); });
        connect(m_document, &Document::entityRemoved, this, [this] { update(); });
        connect(m_document, &Document::entityChanged, this, [this] { update(); });
        connect(m_document, &Document::documentReset, this, [this] { update(); });
        connect(m_document, &Document::layersChanged, this, [this] { update(); });
        connect(m_document, &Document::selectionChanged, this, [this] { update(); });
    }
    update();
}

void CadCanvas::setInputHandler(CanvasInputHandler* handler)
{
    m_handler = handler;
    setCursor(handler ? handler->cursorShape() : Qt::ArrowCursor);
    update();
}

void CadCanvas::applyTheme(bool dark)
{
    m_display.themeIsDark = dark;

    if (dark) {
        m_display.background     = QColor(0x1E, 0x1E, 0x1E);
        m_display.gridMajorColor = QColor(0x3C, 0x3C, 0x3C);
        m_display.gridMinorColor = QColor(0x2A, 0x2A, 0x2A);
        m_display.axisXColor     = QColor(0xE5, 0x5A, 0x5A);
        m_display.axisYColor     = QColor(0x5A, 0xC8, 0x6A);
        m_display.selectionColor = QColor(0x53, 0xB0, 0xFF);
        m_display.crosshairColor = QColor(0x9A, 0x9A, 0x9A);
    } else {
        m_display.background     = QColor(0xFF, 0xFF, 0xFF);
        m_display.gridMajorColor = QColor(0xD0, 0xD0, 0xD0);
        m_display.gridMinorColor = QColor(0xEC, 0xEC, 0xEC);
        m_display.axisXColor     = QColor(0xC8, 0x30, 0x30);
        m_display.axisYColor     = QColor(0x30, 0x80, 0x30);
        m_display.selectionColor = QColor(0x1E, 0x88, 0xE5);
        m_display.crosshairColor = QColor(0x88, 0x88, 0x88);
    }
    update();
}

double CadCanvas::pickTolerance() const
{
    return m_view.deviceLengthToWorld(m_display.pickBoxPixels);
}

EntityRenderer::DrawContext CadCanvas::drawContext() const
{
    EntityRenderer::DrawContext context;
    context.view = &m_view;
    context.settings = &m_display;
    context.viewport = QRectF(rect());
    return context;
}

void CadCanvas::requestRepaint()
{
    if (m_repaintPending) return;
    m_repaintPending = true;
    update();
}

void CadCanvas::emitViewChanged()
{
    emit viewChanged();
}

// -----------------------------------------------------------------------------
//  视图操作
// -----------------------------------------------------------------------------
void CadCanvas::zoomIn()
{
    m_view.zoomAt(Point2D(width() * 0.5, height() * 0.5), 1.0 / kWheelZoomStep);
    emitViewChanged();
    update();
}

void CadCanvas::zoomOut()
{
    m_view.zoomAt(Point2D(width() * 0.5, height() * 0.5), kWheelZoomStep);
    emitViewChanged();
    update();
}

void CadCanvas::zoomExtents()
{
    const Box2D extents = m_document ? m_document->extents() : Box2D();
    m_view.fitTo(extents, size(), 0.06);
    emitViewChanged();
    update();
}

void CadCanvas::zoomToSelection()
{
    if (!m_document || m_document->selection().isEmpty()) {
        zoomExtents();
        return;
    }
    const Box2D box = m_document->extentsOf(m_document->selection().ids());
    if (!box.isValid()) {
        zoomExtents();
        return;
    }
    m_view.fitTo(box, size(), 0.08);
    emitViewChanged();
    update();
}

void CadCanvas::zoomToBox(const Box2D& worldBox)
{
    m_view.fitTo(worldBox, size(), 0.02);
    emitViewChanged();
    update();
}

void CadCanvas::setZoomScale(double pixelsPerWorldUnit)
{
    m_view.setScale(pixelsPerWorldUnit, Point2D(width() * 0.5, height() * 0.5));
    emitViewChanged();
    update();
}

void CadCanvas::centerOn(const Point2D& worldPoint)
{
    m_view.centerOn(worldPoint, size());
    emitViewChanged();
    update();
}

// -----------------------------------------------------------------------------
//  捕捉标记
// -----------------------------------------------------------------------------
void CadCanvas::setSnapResult(const std::optional<SnapResult>& snap)
{
    m_snap = snap;
    update();
}

// -----------------------------------------------------------------------------
//  拾取
// -----------------------------------------------------------------------------
EntityId CadCanvas::pickEntity(const Point2D& world, double tolerance) const
{
    if (!m_document) return kInvalidEntityId;

    // 自上而下遍历，命中最上层者胜出
    const std::vector<EntityId>& order = m_document->drawOrder();
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (!m_document->isSelectable(*it)) continue;
        const Entity* entity = m_document->entity(*it);
        if (!entity) continue;
        if (entity->distanceTo(world) <= tolerance) return *it;
    }
    return kInvalidEntityId;
}

std::vector<EntityId> CadCanvas::pickEntitiesInBox(const Box2D& worldBox, bool crossing) const
{
    std::vector<EntityId> result;
    if (!m_document || !worldBox.isValid()) return result;

    for (EntityId id : m_document->drawOrder()) {
        if (!m_document->isSelectable(id)) continue;
        const Entity* entity = m_document->entity(id);
        if (!entity) continue;

        const bool hit = crossing ? entity->crosses(worldBox) : entity->isInside(worldBox);
        if (hit) result.push_back(id);
    }
    return result;
}

// -----------------------------------------------------------------------------
//  事件组装
// -----------------------------------------------------------------------------
CanvasEvent CadCanvas::makeEvent(const QPointF& devicePos, Qt::MouseButton button,
                                 Qt::MouseButtons buttons,
                                 Qt::KeyboardModifiers modifiers) const
{
    CanvasEvent event;
    event.devicePos = Point2D(devicePos.x(), devicePos.y());
    event.devicePosRounded = QPoint(static_cast<int>(std::lround(devicePos.x())),
                                    static_cast<int>(std::lround(devicePos.y())));
    event.worldPos = m_view.toWorld(event.devicePos);
    event.button = button;
    event.buttons = buttons;
    event.modifiers = modifiers;
    event.pickBoxPixels = m_display.pickBoxPixels;
    event.pickToleranceWorld = pickTolerance();
    return event;
}

// -----------------------------------------------------------------------------
//  绘制
// -----------------------------------------------------------------------------
void CadCanvas::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    m_repaintPending = false;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, m_display.antialias);

    drawBackground(painter);
    if (m_display.showGrid) drawGrid(painter);
    if (m_display.showAxes) drawAxes(painter);

    // 选中高亮先行绘制，使图元本体覆盖在其之上，视觉上更接近 CAD 的观感
    drawSelectionHighlight(painter);
    drawEntities(painter);
    drawGrips(painter);

    const EntityRenderer::DrawContext context = drawContext();
    if (m_handler) m_handler->paintOverlay(painter, context);

    drawSnapMarker(painter);
    if (m_display.showCrosshair && m_cursorInside) drawCrosshair(painter);
}

void CadCanvas::drawBackground(QPainter& painter)
{
    painter.fillRect(rect(), m_display.background);
}

double CadCanvas::majorGridSpacing() const
{
    if (!m_display.adaptiveGrid) return m_display.gridSpacing;

    const double worldPerPixel = m_view.worldPerDevice();
    const double targetWorld = worldPerPixel * 80.0;      // 主格期望约 80 像素
    if (!(targetWorld > 0.0) || !std::isfinite(targetWorld)) return m_display.gridSpacing;

    const double exponent = std::floor(std::log10(targetWorld));
    const double base = std::pow(10.0, exponent);
    const double normalized = targetWorld / base;

    double multiplier = 10.0;
    if (normalized < 1.5) multiplier = 1.0;
    else if (normalized < 3.5) multiplier = 2.0;
    else if (normalized < 7.5) multiplier = 5.0;

    return base * multiplier;
}

void CadCanvas::drawGrid(QPainter& painter)
{
    const Box2D visible = m_view.visibleWorld(size());
    if (!visible.isValid()) return;

    const double major = majorGridSpacing();
    if (!(major > 0.0) || !std::isfinite(major)) return;

    const int subdivisions = std::max(1, m_display.gridSubdivisions);
    const double minor = major / subdivisions;

    // 视口尺寸取出来备用：lambda 内部若直接调用 width()/height() 会与
    // Box2D 的同名方法产生名字查找歧义，显式缓存更清晰也更快。
    const double viewWidth = width();
    const double viewHeight = height();

    auto drawLinesAt = [&](double spacing, const QColor& color, double lineWidth) {
        if (!(spacing > 0.0)) return;

        const double pixels = m_view.worldLengthToDevice(spacing);
        if (pixels < 4.0) return;                       // 屏幕上过密，绘制只会变成灰块

        const int verticalCount = static_cast<int>(visible.width() / spacing) + 2;
        const int horizontalCount = static_cast<int>(visible.height() / spacing) + 2;
        if (verticalCount > kMaxGridLines || horizontalCount > kMaxGridLines) return;

        QPen pen(color);
        pen.setWidthF(lineWidth);
        pen.setCosmetic(true);
        painter.setPen(pen);

        QVector<QLineF> lines;
        lines.reserve(verticalCount + horizontalCount);

        const double firstX = std::floor(visible.minX / spacing) * spacing;
        for (int i = 0; i < verticalCount; ++i) {
            const double wx = firstX + i * spacing;
            const double dx = m_view.toDevice(Point2D(wx, 0.0)).x;
            lines.append(QLineF(dx, 0.0, dx, viewHeight));
        }

        const double firstY = std::floor(visible.minY / spacing) * spacing;
        for (int i = 0; i < horizontalCount; ++i) {
            const double wy = firstY + i * spacing;
            const double dy = m_view.toDevice(Point2D(0.0, wy)).y;
            lines.append(QLineF(0.0, dy, viewWidth, dy));
        }

        painter.drawLines(lines);
    };

    if (m_display.gridSubdivisions > 1) {
        drawLinesAt(minor, m_display.gridMinorColor, 1.0);
    }
    drawLinesAt(major, m_display.gridMajorColor, 1.0);
}

void CadCanvas::drawAxes(QPainter& painter)
{
    const Box2D visible = m_view.visibleWorld(size());

    QPen penX(m_display.axisXColor);
    penX.setWidthF(1.2);
    penX.setCosmetic(true);

    QPen penY(m_display.axisYColor);
    penY.setWidthF(1.2);
    penY.setCosmetic(true);

    // 世界 X 轴（y = 0）：仅当其落在可见范围内才绘制
    if (visible.valid && visible.minY <= 0.0 && visible.maxY >= 0.0) {
        painter.setPen(penX);
        const double dy = m_view.toDevice(Point2D(0.0, 0.0)).y;
        painter.drawLine(QPointF(0.0, dy), QPointF(static_cast<double>(width()), dy));
    }

    // 世界 Y 轴（x = 0）
    if (visible.valid && visible.minX <= 0.0 && visible.maxX >= 0.0) {
        painter.setPen(penY);
        const double dx = m_view.toDevice(Point2D(0.0, 0.0)).x;
        painter.drawLine(QPointF(dx, 0.0), QPointF(dx, static_cast<double>(height())));
    }
}

void CadCanvas::drawEntities(QPainter& painter)
{
    if (!m_document) return;

    const LayerManager& layers = m_document->layers();
    const EntityRenderer::DrawContext context = drawContext();

    for (EntityId id : m_document->drawOrder()) {
        const Entity* entity = m_document->entity(id);
        if (!entity) continue;
        if (!entity->isDisplayable(layers)) continue;

        const QColor color = entity->resolvedColor(layers, m_display.themeIsDark);
        const double lineweight = entity->resolvedLineweight(layers);

        QString linetype = entity->linetypeName();
        if (linetype.isEmpty()) {
            linetype = layers.linetypeOf(entity->layerName(), QStringLiteral("CONTINUOUS"));
        }

        EntityRenderer::draw(painter, *entity, context, color, lineweight, linetype);
    }

    if (qgetenv("LYCAD_RENDER_STATS") == "1") {
        // 单轮汇总：drawOrder 遍历结束。剔除计数在 EntityRenderer::draw 里累计。
        // 通过一个弱符号方案太绕，这里直接访问 draw 的静态计数器不现实；
        // 改为在 draw() 里打印（见 EntityRenderer.cpp）。
    }
}

void CadCanvas::drawSelectionHighlight(QPainter& painter)
{
    if (!m_document || !m_display.showSelection) return;
    if (m_document->selection().isEmpty()) return;

    const EntityRenderer::DrawContext context = drawContext();

    // 高亮是「描边」而非「覆盖」：过宽过浓会盖住图元自身的颜色，
    // 反而让用户看不出哪条线被选中。这里按线宽小幅外扩，并保持较低不透明度，
    // 使图元原色仍然可辨，同时夹点仍能明确指示选中状态。
    QColor halo = m_display.selectionColor;
    halo.setAlpha(80);

    for (EntityId id : m_document->selection().ids()) {
        const Entity* entity = m_document->entity(id);
        if (!entity || !entity->isDisplayable(m_document->layers())) continue;

        // 以较宽的半透明线条勾出轮廓，形成「描边高亮」效果
        const double baseWidth = std::clamp(
            m_view.worldLengthToDevice(entity->resolvedLineweight(m_document->layers())),
            m_display.minLinewidthPixels, m_display.maxLinewidthPixels);

        QPen pen(halo);
        pen.setWidthF(baseWidth + 3.0);
        pen.setStyle(Qt::SolidLine);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);

        painter.save();
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(EntityRenderer::toDevicePath(*entity, m_view, QRectF(rect())));
        painter.restore();

        Q_UNUSED(context);
    }
}

void CadCanvas::drawGrips(QPainter& painter)
{
    if (!m_document || !m_display.showSelection) return;
    if (m_document->selection().isEmpty()) return;

    const double half = 3.0;
    painter.save();
    painter.setPen(QPen(m_display.selectionColor.darker(120), 1.0));
    painter.setBrush(m_display.selectionColor);

    for (EntityId id : m_document->selection().ids()) {
        const Entity* entity = m_document->entity(id);
        if (!entity || !entity->isDisplayable(m_document->layers())) continue;

        for (const Point2D& grip : entity->gripPoints()) {
            const Point2D device = m_view.toDevice(grip);
            painter.drawRect(QRectF(device.x - half, device.y - half, half * 2.0, half * 2.0));
        }
    }
    painter.restore();
}

void CadCanvas::drawSnapMarker(QPainter& painter)
{
    if (!m_snap || !m_snap->isValid()) return;

    const Point2D device = m_view.toDevice(m_snap->position);
    const QColor color = snapKindColor(m_snap->kind);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPen pen(color);
    pen.setWidthF(1.6);
    pen.setCosmetic(true);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const double r = 5.0;
    const QPointF c(device.x, device.y);

    switch (m_snap->kind) {
    case SnapKind::Endpoint:
    case SnapKind::NodePoint:
        painter.drawRect(QRectF(c.x() - r, c.y() - r, r * 2.0, r * 2.0));
        break;
    case SnapKind::Midpoint:
        painter.drawPolygon(QPolygonF({ QPointF(c.x(), c.y() - r),
                                        QPointF(c.x() + r, c.y() + r),
                                        QPointF(c.x() - r, c.y() + r) }));
        break;
    case SnapKind::Center:
        painter.drawEllipse(c, r, r);
        break;
    case SnapKind::Quadrant:
        painter.drawPolygon(QPolygonF({ QPointF(c.x(), c.y() - r),
                                        QPointF(c.x() + r, c.y()),
                                        QPointF(c.x(), c.y() + r),
                                        QPointF(c.x() - r, c.y()) }));
        break;
    case SnapKind::Intersection:
        painter.drawLine(QPointF(c.x() - r, c.y() - r), QPointF(c.x() + r, c.y() + r));
        painter.drawLine(QPointF(c.x() + r, c.y() - r), QPointF(c.x() - r, c.y() + r));
        break;
    case SnapKind::Perpendicular:
        painter.drawLine(QPointF(c.x(), c.y() - r), QPointF(c.x(), c.y() + r));
        painter.drawLine(QPointF(c.x() - r, c.y() + r), QPointF(c.x() + r, c.y() + r));
        painter.drawLine(QPointF(c.x() - r, c.y()), QPointF(c.x() + r, c.y()));
        break;
    case SnapKind::Tangent:
        painter.drawEllipse(c, r, r);
        painter.drawLine(QPointF(c.x() - r, c.y() - r), QPointF(c.x() + r, c.y() - r));
        break;
    case SnapKind::Grid:
        painter.drawLine(QPointF(c.x() - r, c.y()), QPointF(c.x() + r, c.y()));
        painter.drawLine(QPointF(c.x(), c.y() - r), QPointF(c.x(), c.y() + r));
        break;
    default:
        painter.drawRect(QRectF(c.x() - r, c.y() - r, r * 2.0, r * 2.0));
        break;
    }

    // 标记旁的文字提示
    const QString label = m_snap->label.isEmpty()
        ? snapKindShortLabel(m_snap->kind)
        : m_snap->label;
    if (!label.isEmpty()) {
        QFont font = painter.font();
        font.setPointSizeF(std::max(7.5, font.pointSizeF() - 1.0));
        painter.setFont(font);

        const QFontMetrics metrics(font);
        const int textWidth = metrics.horizontalAdvance(label);
        const QRectF textRect(c.x() + r + 4.0, c.y() - r - 4.0,
                              textWidth + 8.0, metrics.height() + 2.0);

        painter.setBrush(QColor(255, 255, 255, 220));
        painter.setPen(QPen(color, 1.0));
        painter.drawRoundedRect(textRect, 3.0, 3.0);
        painter.setPen(color);
        painter.drawText(textRect, Qt::AlignCenter, label);
    }

    painter.restore();
}

void CadCanvas::drawCrosshair(QPainter& painter)
{
    QPen pen(m_display.crosshairColor);
    pen.setWidthF(1.0);
    pen.setCosmetic(true);
    painter.save();
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const double cx = m_cursorDevice.x;
    const double cy = m_cursorDevice.y;
    const double half = m_display.crosshairSizePixels;

    painter.drawLine(QPointF(cx - half, cy), QPointF(cx + half, cy));
    painter.drawLine(QPointF(cx, cy - half), QPointF(cx, cy + half));

    // 拾取框
    const double box = m_display.pickBoxPixels;
    painter.drawRect(QRectF(cx - box, cy - box, box * 2.0, box * 2.0));

    painter.restore();
}

// -----------------------------------------------------------------------------
//  输入事件
// -----------------------------------------------------------------------------
void CadCanvas::mousePressEvent(QMouseEvent* event)
{
    setFocus(Qt::MouseFocusReason);

    const QPointF pos = event->position();

    if (event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_lastPanPos = pos;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    if (event->button() == Qt::RightButton) {
        emit contextMenuRequested(event->globalPosition().toPoint());
        event->accept();
        return;
    }

    if (m_handler
        && m_handler->handleMousePress(makeEvent(pos, event->button(),
                                                 event->buttons(), event->modifiers()))) {
        update();
        event->accept();
        return;
    }

    QWidget::mousePressEvent(event);
}

void CadCanvas::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();

    m_cursorInside = true;
    m_cursorDevice = Point2D(pos.x(), pos.y());
    m_cursorWorld = m_view.toWorld(m_cursorDevice);
    emit mouseWorldPositionChanged(m_cursorWorld, m_cursorDevice);

    if (m_panning) {
        const QPointF delta = pos - m_lastPanPos;
        m_lastPanPos = pos;
        m_view.panByDevice(Vector2D(delta.x(), delta.y()));
        emitViewChanged();
        update();
        event->accept();
        return;
    }

    if (m_handler) {
        m_handler->handleMouseMove(makeEvent(pos, Qt::NoButton,
                                             event->buttons(), event->modifiers()));
    }

    update();      // 十字光标与捕捉标记需要跟随
    event->accept();
}

void CadCanvas::mouseReleaseEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();

    if (event->button() == Qt::MiddleButton && m_panning) {
        m_panning = false;
        setCursor(m_handler ? m_handler->cursorShape() : Qt::ArrowCursor);
        event->accept();
        return;
    }

    if (m_handler
        && m_handler->handleMouseRelease(makeEvent(pos, event->button(),
                                                   event->buttons(), event->modifiers()))) {
        update();
        event->accept();
        return;
    }

    QWidget::mouseReleaseEvent(event);
}

void CadCanvas::mouseDoubleClickEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    if (m_handler
        && m_handler->handleMouseDoubleClick(makeEvent(pos, event->button(),
                                                       event->buttons(), event->modifiers()))) {
        update();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void CadCanvas::wheelEvent(QWheelEvent* event)
{
    const double steps = event->angleDelta().y() / 120.0;
    if (std::abs(steps) < 1e-9) {
        event->ignore();
        return;
    }

    const QPointF pos = event->position();
    const double factor = std::pow(kWheelZoomStep, steps);

    m_view.zoomAt(Point2D(pos.x(), pos.y()), factor);

    // 缩放后光标下的世界坐标已改变，需重新广播
    m_cursorWorld = m_view.toWorld(Point2D(pos.x(), pos.y()));
    m_cursorDevice = Point2D(pos.x(), pos.y());
    emit mouseWorldPositionChanged(m_cursorWorld, m_cursorDevice);

    emitViewChanged();
    update();
    event->accept();
}

void CadCanvas::keyPressEvent(QKeyEvent* event)
{
    if (m_handler && m_handler->handleKeyPress(event)) {
        update();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void CadCanvas::leaveEvent(QEvent* event)
{
    m_cursorInside = false;
    emit cursorLeftCanvas();
    update();
    QWidget::leaveEvent(event);
}

void CadCanvas::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    emitViewChanged();
}

} // namespace lycad
