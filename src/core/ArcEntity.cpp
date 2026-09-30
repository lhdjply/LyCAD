// =============================================================================
//  LyCAD - core/ArcEntity.cpp
// =============================================================================
#include "core/ArcEntity.h"

#include "core/CircleEntity.h"
#include "core/Format.h"

#include <array>

namespace lycad {

namespace {

/// 弧的四个象限角
constexpr std::array<double, 4> kQuadrantAngles = { 0.0, kHalfPi, kPi, 3.0 * kHalfPi };

/// 由三点求外接圆圆心
std::optional<Point2D> circumcenter(const Point2D& a, const Point2D& b, const Point2D& c)
{
    const double d = 2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
    if (std::abs(d) <= kEpsilon) return std::nullopt;   // 三点共线

    const double a2 = a.x * a.x + a.y * a.y;
    const double b2 = b.x * b.x + b.y * b.y;
    const double c2 = c.x * c.x + c.y * c.y;

    const double ux = (a2 * (b.y - c.y) + b2 * (c.y - a.y) + c2 * (a.y - b.y)) / d;
    const double uy = (a2 * (c.x - b.x) + b2 * (a.x - c.x) + c2 * (b.x - a.x)) / d;
    return Point2D(ux, uy);
}

} // namespace

ArcEntity::ArcEntity()
    : Entity(EntityType::Arc)
{
}

ArcEntity::ArcEntity(const Point2D& center, double radius, double startAngleRad, double endAngleRad)
    : Entity(EntityType::Arc)
    , m_center(center)
    , m_radius(std::max(0.0, radius))
    , m_startAngle(normalizeAngle(startAngleRad))
    , m_endAngle(normalizeAngle(endAngleRad))
{
}

double ArcEntity::spanAngle() const
{
    const double span = normalizeAngle(m_endAngle - m_startAngle);
    // 起止角重合按整圆处理：既覆盖「三点法」得到的近整圆，也避免零长弧
    return span <= kEpsilon ? kTwoPi : span;
}

double ArcEntity::area() const
{
    return 0.5 * m_radius * m_radius * spanAngle();
}

void ArcEntity::setAngles(double startRad, double endRad) noexcept
{
    m_startAngle = normalizeAngle(startRad);
    m_endAngle = normalizeAngle(endRad);
}

std::optional<ArcEntity> ArcEntity::fromThreePoints(const Point2D& start,
                                                    const Point2D& onArc,
                                                    const Point2D& end)
{
    const auto center = circumcenter(start, onArc, end);
    if (!center) return std::nullopt;

    const double radius = center->distanceTo(start);
    if (radius <= kGeomTol) return std::nullopt;

    const double a1 = (*center).angleTo(start);
    const double a2 = (*center).angleTo(end);
    const double am = (*center).angleTo(onArc);

    ArcEntity arc;
    arc.setCenter(*center);
    arc.setRadius(radius);
    // 以三点法绘弧时，中间点决定方向：若中间点不在「起点→终点」的逆时针弧上，则反转
    if (angleWithin(am, a1, a2, true)) {
        arc.setAngles(a1, a2);
    } else {
        arc.setAngles(a2, a1);
    }
    return arc;
}

std::optional<ArcEntity> ArcEntity::fromCenterStartEnd(const Point2D& center,
                                                       const Point2D& start,
                                                       const Point2D& end)
{
    const double radius = center.distanceTo(start);
    if (radius <= kGeomTol) return std::nullopt;

    ArcEntity arc;
    arc.setCenter(center);
    arc.setRadius(radius);
    arc.setAngles(center.angleTo(start), center.angleTo(end));
    return arc;
}

Point2D ArcEntity::pointAtAngle(double radians) const
{
    return Point2D(m_center.x + m_radius * std::cos(radians),
                   m_center.y + m_radius * std::sin(radians));
}

bool ArcEntity::containsAngle(double radians) const
{
    return angleWithin(normalizeAngle(radians), m_startAngle, m_endAngle, true);
}

CircleEntity ArcEntity::toCircle() const
{
    CircleEntity c(m_center, m_radius);
    // 把显示属性一并带过去
    copyCommonPropertiesTo(c);
    return c;
}

std::optional<ArcEntity> ArcEntity::trimmed(double fromAngle, double toAngle) const
{
    const double span = normalizeAngle(toAngle - fromAngle);
    if (span <= kEpsilon) return std::nullopt;      // 退化区间

    ArcEntity out;
    out.setCenter(m_center);
    out.setRadius(m_radius);
    out.setAngles(fromAngle, toAngle);
    copyCommonPropertiesTo(out);
    return out;
}

std::unique_ptr<Entity> ArcEntity::clone() const
{
    auto copy = std::make_unique<ArcEntity>(m_center, m_radius, m_startAngle, m_endAngle);
    copyCommonPropertiesTo(*copy);
    return copy;
}

Box2D ArcEntity::boundingBox() const
{
    Box2D box;
    if (isDegenerate()) {
        box.expand(m_center);
        return box;
    }

    // 端点
    box.expand(startPoint());
    box.expand(endPoint());

    // 落在弧范围内的象限点决定了极值
    for (double q : kQuadrantAngles) {
        if (containsAngle(q)) box.expand(pointAtAngle(q));
    }
    return box;
}

double ArcEntity::distanceTo(const Point2D& p) const
{
    if (isDegenerate()) return p.distanceTo(m_center);

    const double ang = m_center.angleTo(p);
    if (containsAngle(ang)) {
        return std::abs(m_center.distanceTo(p) - m_radius);
    }
    const double ds = p.distanceTo(startPoint());
    const double de = p.distanceTo(endPoint());
    return std::min(ds, de);
}

Point2D ArcEntity::closestPointTo(const Point2D& p) const
{
    if (isDegenerate()) return m_center;

    const double ang = m_center.angleTo(p);
    if (containsAngle(ang)) {
        const Vector2D dir = p - m_center;
        return m_center + dir.normalizedOr(Vector2D(1.0, 0.0)) * m_radius;
    }
    const Point2D s = startPoint();
    const Point2D e = endPoint();
    return p.distanceTo(s) <= p.distanceTo(e) ? s : e;
}

bool ArcEntity::crosses(const Box2D& box) const
{
    if (!box.isValid() || isDegenerate()) return false;
    // 以盒尺度自适应离散精度后做折线与盒的相交判定
    return detail::pointsCrossBox(flatten(detail::sagittaForBox(box)), false, box);
}

void ArcEntity::transform(const Transform2D& t)
{
    if (isDegenerate()) {
        m_center = t.apply(m_center);
        return;
    }

    const double span = spanAngle();

    // 相似变换下圆弧仍为圆弧：圆心、半径按变换走，跨度不变。
    // 变换后起点的新位置决定起始角；若含镜像（行列式为负），
    // 逆时针方向在新坐标系中反转为顺时针，因此起止角需要互换。
    const Point2D newCenter = t.apply(m_center);
    const Point2D newStartGeom = t.apply(startPoint());

    const double scale = t.isSimilarity(1e-6) ? t.uniformScale() : t.uniformScale();
    const double newRadius = m_radius * scale;
    if (newRadius <= kGeomTol) {
        m_radius = 0.0;
        m_center = newCenter;
        return;
    }

    const double a1 = newCenter.angleTo(newStartGeom);

    m_center = newCenter;
    m_radius = newRadius;
    if (!t.isMirroring()) {
        m_startAngle = normalizeAngle(a1);
        m_endAngle = normalizeAngle(a1 + span);
    } else {
        m_startAngle = normalizeAngle(a1 - span);
        m_endAngle = normalizeAngle(a1);
    }
}

double ArcEntity::length() const
{
    return m_radius * spanAngle();
}

std::vector<Point2D> ArcEntity::snapPoints() const
{
    std::vector<Point2D> pts;
    pts.reserve(8);
    pts.push_back(m_center);          // 圆心
    pts.push_back(startPoint());      // 端点
    pts.push_back(endPoint());        // 端点
    pts.push_back(middlePoint());     // 中点

    for (double q : kQuadrantAngles) {
        if (containsAngle(q)) pts.push_back(pointAtAngle(q));   // 象限点
    }
    return pts;
}

bool ArcEntity::moveGrip(std::size_t index, const Point2D& newPos)
{
    switch (index) {
    case 0:
        m_center = newPos;                             // 拖拽圆心 = 平移
        return true;
    case 1: {                                          // 拖拽起点 = 改起始角与半径
        m_radius = std::max(0.0, m_center.distanceTo(newPos));
        m_startAngle = normalizeAngle(m_center.angleTo(newPos));
        return true;
    }
    case 2: {                                          // 拖拽终点 = 改终止角与半径
        m_radius = std::max(0.0, m_center.distanceTo(newPos));
        m_endAngle = normalizeAngle(m_center.angleTo(newPos));
        return true;
    }
    case 3: {                                          // 拖拽弧中点 = 改半径
        m_radius = std::max(0.0, m_center.distanceTo(newPos));
        return true;
    }
    default:
        return false;
    }
}

std::vector<Point2D> ArcEntity::flatten(double sagitta) const
{
    std::vector<Point2D> pts;
    if (isDegenerate()) return pts;

    const double span = spanAngle();
    const int count = segmentCountForArc(m_radius, span, sagitta);
    pts.reserve(static_cast<std::size_t>(count) + 1);
    const double step = span / count;
    for (int i = 0; i <= count; ++i) {
        pts.push_back(pointAtAngle(m_startAngle + step * i));
    }
    return pts;
}

QString ArcEntity::describe() const
{
    return QStringLiteral("圆弧  圆心 (%1)  半径 %2  包角 %3")
        .arg(formatPoint(m_center), formatLength(m_radius), formatAngle(spanAngle()));
}

QString ArcEntity::geometrySummary() const
{
    return QStringLiteral("圆心: %1\n半径: %2\n起始角: %3\n终止角: %4\n包角: %5\n弧长: %6")
        .arg(formatPoint(m_center), formatLength(m_radius),
             formatAngle(m_startAngle), formatAngle(m_endAngle),
             formatAngle(spanAngle()), formatLength(length()));
}

} // namespace lycad
