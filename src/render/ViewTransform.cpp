// =============================================================================
//  LyCAD - render/ViewTransform.cpp
// =============================================================================
#include "render/ViewTransform.h"

#include <algorithm>

namespace lycad {

Point2D ViewTransform::toDevice(const Point2D& world) const noexcept
{
    return Point2D((world.x - m_origin.x) * m_scale,
                   (m_origin.y - world.y) * m_scale);
}

Point2D ViewTransform::toWorld(const Point2D& device) const noexcept
{
    if (m_scale <= kEpsilon) return m_origin;
    return Point2D(m_origin.x + device.x / m_scale,
                   m_origin.y - device.y / m_scale);
}

void ViewTransform::setScale(double pixelsPerWorld, const Point2D& deviceAnchor)
{
    const double clamped = std::clamp(pixelsPerWorld, kMinScale, kMaxScale);
    if (std::abs(clamped - m_scale) <= kEpsilon) return;

    // 保持锚点下的世界坐标不变
    const Point2D anchorWorld = toWorld(deviceAnchor);

    m_scale = clamped;
    m_origin = Point2D(anchorWorld.x - deviceAnchor.x / m_scale,
                       anchorWorld.y + deviceAnchor.y / m_scale);
}

void ViewTransform::zoomAt(const Point2D& deviceAnchor, double factor)
{
    if (factor <= 0.0) return;
    setScale(m_scale * factor, deviceAnchor);
}

void ViewTransform::panByDevice(const Vector2D& deviceDelta) noexcept
{
    if (m_scale <= kEpsilon) return;
    // 内容随鼠标同向移动，等价于视口原点反向平移
    m_origin.x -= deviceDelta.x / m_scale;
    m_origin.y += deviceDelta.y / m_scale;
}

void ViewTransform::panByWorld(const Vector2D& worldDelta) noexcept
{
    m_origin.x -= worldDelta.x;
    m_origin.y -= worldDelta.y;
}

Box2D ViewTransform::visibleWorld(const QSize& viewportSize) const
{
    Box2D box;
    box.expand(toWorld(Point2D(0.0, 0.0)));
    box.expand(toWorld(Point2D(static_cast<double>(viewportSize.width()),
                                 static_cast<double>(viewportSize.height()))));
    return box;
}

void ViewTransform::centerOn(const Point2D& worldPoint, const QSize& viewportSize)
{
    const double halfW = viewportSize.width() * 0.5;
    const double halfH = viewportSize.height() * 0.5;

    m_origin = Point2D(worldPoint.x - halfW / m_scale,
                       worldPoint.y + halfH / m_scale);
}

void ViewTransform::fitTo(const Box2D& worldBox, const QSize& viewportSize, double marginRatio)
{
    if (viewportSize.width() <= 0 || viewportSize.height() <= 0) return;

    Box2D box = worldBox;
    if (!box.isValid()) {
        box.expand(Point2D(0.0, 0.0));
        box.expand(Point2D(100.0, 100.0));
    }

    // 退化范围（单点、水平线、垂直线）需要人为撑开，否则缩放比例无解
    const double minExtent = 1e-3;
    if (box.width() < minExtent || box.height() < minExtent) {
        box.expand(box.center());
        box.expand(Point2D(box.minX - std::max(box.width(), minExtent) * 0.5,
                           box.minY - std::max(box.height(), minExtent) * 0.5));
        box.expand(Point2D(box.maxX + std::max(box.width(), minExtent) * 0.5,
                           box.maxY + std::max(box.height(), minExtent) * 0.5));
    }

    const double margin = std::clamp(marginRatio, 0.0, 0.4);
    const double usableW = viewportSize.width() * (1.0 - 2.0 * margin);
    const double usableH = viewportSize.height() * (1.0 - 2.0 * margin);

    const double sx = usableW / box.width();
    const double sy = usableH / box.height();

    m_scale = std::clamp(std::min(sx, sy), kMinScale, kMaxScale);
    centerOn(box.center(), viewportSize);
}

void ViewTransform::reset()
{
    m_origin = Point2D(0.0, 0.0);
    m_scale = 1.0;
}

double ViewTransform::worldPerDevice() const noexcept
{
    return m_scale <= kEpsilon ? 1.0 : 1.0 / m_scale;
}

double ViewTransform::worldLengthToDevice(double worldLength) const noexcept
{
    return worldLength * m_scale;
}

double ViewTransform::deviceLengthToWorld(double deviceLength) const noexcept
{
    return deviceLength * worldPerDevice();
}

} // namespace lycad
