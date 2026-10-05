// =============================================================================
//  LyCAD - core/EllipseEntity.cpp
// =============================================================================
#include "core/EllipseEntity.h"

#include "core/Format.h"

#include <algorithm>
#include <cmath>

namespace lycad {

namespace {

/// 与圆弧同源的细分准则：弦高 = R(1 - cos(span/2n))
int segmentCountForEllipse(double majorRadius, double span, double sagitta) noexcept
{
    const double r = std::max(majorRadius, 1e-9);
    const double halfAngle = 2.0 * std::acos(std::clamp(1.0 - sagitta / r, -1.0, 1.0));
    if (halfAngle <= 0.0) return 512;
    const int count = static_cast<int>(std::ceil(span / halfAngle));
    return std::clamp(count, 8, 2048);
}

} // namespace

EllipseEntity::HalfAxes EllipseEntity::normalizeHalfAxes(const Vector2D& halfAxis,
                                                        double otherHalfLength)
{
    const double a = halfAxis.length();
    const double b = std::abs(otherHalfLength);

    if (!(a > 0.0) || !(b > 0.0) || !std::isfinite(a) || !std::isfinite(b)) {
        return HalfAxes{};
    }
    if (a >= b) {
        return HalfAxes{ halfAxis, b / a };
    }
    // 用户指定的那条轴其实更短：主轴换成与它垂直的那条（长度 b），
    // 方向取指定轴的逆时针 90°——椭圆是中心对称的，两个方向等价
    const Vector2D perpendicular = halfAxis.perpendicular();
    const Vector2D major = perpendicular.normalizedOr(Vector2D(1.0, 0.0)) * b;
    return HalfAxes{ major, a / b };
}

EllipseEntity::EllipseEntity() : Entity(EntityType::Ellipse) {}

EllipseEntity::EllipseEntity(const Point2D& center, const Point2D& majorAxisEnd,
                             double ratio, double startParam, double endParam)
    : Entity(EntityType::Ellipse),
      m_center(center),
      m_majorAxisEnd(majorAxisEnd),
      m_ratio(std::clamp(ratio, 1e-6, 1.0)),
      m_startParam(startParam),
      m_endParam(endParam)
{
}

void EllipseEntity::setCenter(const Point2D& p) noexcept { m_center = p; }

void EllipseEntity::setMajorAxisEnd(const Point2D& v) noexcept { m_majorAxisEnd = v; }

void EllipseEntity::setRatio(double ratio) noexcept
{
    m_ratio = std::clamp(ratio, 1e-6, 1.0);
}

void EllipseEntity::setParamRange(double startRad, double endRad) noexcept
{
    m_startParam = startRad;
    m_endParam = endRad;
}

double EllipseEntity::minorRadius() const noexcept
{
    return majorRadius() * m_ratio;
}

std::unique_ptr<Entity> EllipseEntity::clone() const
{
    auto copy = std::make_unique<EllipseEntity>(m_center, m_majorAxisEnd,
                                                m_ratio, m_startParam, m_endParam);
    copyCommonPropertiesTo(*copy);
    return copy;
}

std::vector<Point2D> EllipseEntity::flatten(double sagitta) const
{
    std::vector<Point2D> pts;
    if (majorRadius() <= 0.0 || m_ratio <= 0.0) return pts;

    // 参数范围归一到 (0, 2π]：DXF 规定 end < start 时表示跨过 0 点
    double start = m_startParam;
    double end = m_endParam;
    while (end <= start) end += kTwoPi;
    const double span = end - start;

    const double angle = majorAxisAngle();
    const double a = majorRadius();
    const double b = minorRadius();
    const double cosAngle = std::cos(angle);
    const double sinAngle = std::sin(angle);

    const int count = segmentCountForEllipse(a, span, std::max(sagitta, 1e-9));
    pts.reserve(static_cast<std::size_t>(count) + 1);
    for (int i = 0; i <= count; ++i) {
        const double t = start + span * static_cast<double>(i) / count;
        const double px = a * std::cos(t);
        const double py = b * std::sin(t);
        // 椭圆位于「先按主轴方向旋转、再平移」的局部坐标系中
        pts.push_back(Point2D(m_center.x + px * cosAngle - py * sinAngle,
                              m_center.y + px * sinAngle + py * cosAngle));
    }
    if (span >= kTwoPi - 1e-9 && pts.size() >= 2) {
        pts.back() = pts.front();       // 整圆首尾精确闭合
    }
    return pts;
}

Box2D EllipseEntity::boundingBox() const
{
    Box2D box;
    if (majorRadius() <= 0.0 || m_ratio <= 0.0) return box;

    if (isClosed()) {
        // 整圆的轴对齐包围盒有解析解：R(θ)·diag(a, b) 的极值半径
        const double angle = majorAxisAngle();
        const double a = majorRadius();
        const double b = minorRadius();
        const double halfX = std::sqrt(a * a * std::cos(angle) * std::cos(angle)
                                       + b * b * std::sin(angle) * std::sin(angle));
        const double halfY = std::sqrt(a * a * std::sin(angle) * std::sin(angle)
                                       + b * b * std::cos(angle) * std::cos(angle));
        box.expand(Point2D(m_center.x - halfX, m_center.y - halfY));
        box.expand(Point2D(m_center.x + halfX, m_center.y + halfY));
        return box;
    }

    // 椭圆弧：以足够密的采样保证不漏掉参数极值附近的顶点
    for (const Point2D& p : flatten(1e-4)) box.expand(p);
    return box;
}

double EllipseEntity::distanceTo(const Point2D& p) const
{
    return p.distanceTo(closestPointTo(p));
}

void EllipseEntity::transform(const Transform2D& t)
{
    // 主轴端点是相对中心的向量：先随中心一起变换，再作差得到新向量，
    // 保证旋转 / 镜像 / 缩放后主轴方向与实际曲线一致
    const Point2D transformedCenter = t.apply(m_center);
    const Point2D transformedAxisTip =
        t.apply(m_center + Vector2D(m_majorAxisEnd.x, m_majorAxisEnd.y));
    m_center = transformedCenter;
    const Vector2D newAxis = transformedAxisTip - transformedCenter;
    m_majorAxisEnd = Point2D(transformedCenter.x + newAxis.x,
                             transformedCenter.y + newAxis.y);
    // 注意：newAxis 本身就是相对新中心的向量，直接存它即可
    m_majorAxisEnd = Point2D(newAxis.x, newAxis.y);
}

double EllipseEntity::length() const
{
    const std::vector<Point2D> pts = flatten(1e-3);
    double total = 0.0;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        total += pts[i - 1].distanceTo(pts[i]);
    }
    return total;
}

std::vector<Point2D> EllipseEntity::snapPoints() const
{
    // 中心 + 长短轴四个端点
    std::vector<Point2D> snaps{m_center};
    const Vector2D major(m_majorAxisEnd.x, m_majorAxisEnd.y);
    const double angle = majorAxisAngle();
    const Vector2D minor = Vector2D(-std::sin(angle), std::cos(angle))
                           * (major.length() * m_ratio);
    snaps.push_back(m_center + major);
    snaps.push_back(m_center - major);
    snaps.push_back(m_center + minor);
    snaps.push_back(m_center - minor);
    return snaps;
}

QString EllipseEntity::describe() const
{
    if (isClosed()) {
        return QStringLiteral("椭圆  中心 (%1)  长轴 %2  短轴 %3")
            .arg(formatPoint(m_center), formatLength(majorRadius()),
                 formatLength(minorRadius()));
    }
    return QStringLiteral("椭圆弧  中心 (%1)  长轴 %2  短轴 %3  包角 %4")
        .arg(formatPoint(m_center), formatLength(majorRadius()),
             formatLength(minorRadius()), formatAngle(spanParam()));
}

QString EllipseEntity::geometrySummary() const
{
    return QStringLiteral("中心: %1\n长半轴: %2\n短半轴: %3\n起始角: %4\n终止角: %5\n周长: %6")
        .arg(formatPoint(m_center), formatLength(majorRadius()),
             formatLength(minorRadius()), formatAngle(m_startParam),
             formatAngle(m_endParam), formatLength(length()));
}

bool EllipseEntity::isClosed() const
{
    return spanParam() >= kTwoPi - 1e-9;
}

double EllipseEntity::spanParam() const noexcept
{
    double span = m_endParam - m_startParam;
    while (span < 0.0) span += kTwoPi;
    return span;
}

} // namespace lycad
