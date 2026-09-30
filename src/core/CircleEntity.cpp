// =============================================================================
//  LyCAD - core/CircleEntity.cpp
// =============================================================================
#include "core/CircleEntity.h"

#include "core/Format.h"
#include "core/LineEntity.h"

#include <algorithm>

namespace lycad {

CircleEntity::CircleEntity()
    : Entity(EntityType::Circle)
{
}

CircleEntity::CircleEntity(const Point2D& center, double radius)
    : Entity(EntityType::Circle), m_center(center), m_radius(std::max(0.0, radius))
{
}

Point2D CircleEntity::quadrantPoint(int index) const
{
    switch (((index % 4) + 4) % 4) {
    case 0: return Point2D(m_center.x + m_radius, m_center.y);
    case 1: return Point2D(m_center.x, m_center.y + m_radius);
    case 2: return Point2D(m_center.x - m_radius, m_center.y);
    default: return Point2D(m_center.x, m_center.y - m_radius);
    }
}

Point2D CircleEntity::pointAtAngle(double radians) const
{
    return Point2D(m_center.x + m_radius * std::cos(radians),
                   m_center.y + m_radius * std::sin(radians));
}

std::unique_ptr<Entity> CircleEntity::clone() const
{
    auto copy = std::make_unique<CircleEntity>(m_center, m_radius);
    copyCommonPropertiesTo(*copy);
    return copy;
}

Box2D CircleEntity::boundingBox() const
{
    return Box2D::aroundPoint(m_center, m_radius);
}

double CircleEntity::distanceTo(const Point2D& p) const
{
    // 点到圆周的距离为「到圆心的距离」与半径之差的绝对值
    return std::abs(p.distanceTo(m_center) - m_radius);
}

Point2D CircleEntity::closestPointTo(const Point2D& p) const
{
    const Vector2D dir = p - m_center;
    const double len = dir.length();
    if (len <= kEpsilon) return Point2D(m_center.x + m_radius, m_center.y);   // 圆心处任取一点
    return m_center + dir * (m_radius / len);
}

bool CircleEntity::crosses(const Box2D& box) const
{
    if (!box.isValid() || isDegenerate()) return false;

    // 圆心到盒的最近距离
    const double cx = std::clamp(m_center.x, box.minX, box.maxX);
    const double cy = std::clamp(m_center.y, box.minY, box.maxY);
    const double dMin = std::hypot(m_center.x - cx, m_center.y - cy);

    // 圆心到盒的最远距离（最远顶点）
    const double dx = std::max(std::abs(m_center.x - box.minX), std::abs(m_center.x - box.maxX));
    const double dy = std::max(std::abs(m_center.y - box.minY), std::abs(m_center.y - box.maxY));
    const double dMax = std::hypot(dx, dy);

    // 盒为凸集，自最近点至最远顶点的线段完全位于盒内，
    // 该线段上到圆心的距离连续覆盖 [dMin, dMax]，
    // 因此圆周与盒相交 <=> 半径落在该区间内。
    const double tol = kGeomTol;
    return (dMin - tol) <= m_radius && m_radius <= (dMax + tol);
}

void CircleEntity::transform(const Transform2D& t)
{
    m_center = t.apply(m_center);
    // 非等比变换会把圆变为椭圆，本软件不表达椭圆，
    // 统一按等效均匀缩放处理（等比缩放与镜像下为精确结果）。
    m_radius *= t.uniformScale();
}

double CircleEntity::length() const
{
    return circumference();
}

std::vector<Point2D> CircleEntity::snapPoints() const
{
    return { m_center, quadrantPoint(0), quadrantPoint(1), quadrantPoint(2), quadrantPoint(3) };
}

bool CircleEntity::moveGrip(std::size_t index, const Point2D& newPos)
{
    if (index == 0) {
        m_center = newPos;               // 拖拽圆心 = 平移整圆
        return true;
    }
    if (index >= 1 && index <= 4) {
        m_radius = std::max(0.0, m_center.distanceTo(newPos));   // 拖拽象限点 = 改半径
        return true;
    }
    return false;
}

std::vector<Point2D> CircleEntity::flatten(double sagitta) const
{
    std::vector<Point2D> pts;
    if (isDegenerate()) return pts;

    const int count = segmentCountForArc(m_radius, kTwoPi, sagitta);
    pts.reserve(static_cast<std::size_t>(count));
    const double step = kTwoPi / count;
    for (int i = 0; i < count; ++i) {
        pts.push_back(pointAtAngle(i * step));
    }
    return pts;   // 闭合环：首尾不重复
}

QString CircleEntity::describe() const
{
    return QStringLiteral("圆  圆心 (%1)  半径 %2")
        .arg(formatPoint(m_center), formatLength(m_radius));
}

QString CircleEntity::geometrySummary() const
{
    return QStringLiteral("圆心: %1\n半径: %2\n直径: %3\n周长: %4\n面积: %5")
        .arg(formatPoint(m_center), formatLength(m_radius), formatLength(diameter()),
             formatLength(circumference()), formatArea(area()));
}

std::vector<Point2D> CircleEntity::intersectionsWith(const LineEntity& line) const
{
    std::vector<Point2D> result;
    if (isDegenerate()) return result;

    // 参数化直线 P(t) = A + t·(B-A)，代入圆方程求 t
    const Point2D a = line.start();
    const Vector2D d = line.end() - a;
    const double dd = d.lengthSquared();
    if (dd <= kEpsilon) return result;

    const Vector2D f = a - m_center;
    const double b = 2.0 * dot(f, d);
    const double c = f.lengthSquared() - m_radius * m_radius;
    const double disc = b * b - 4.0 * dd * c;
    if (disc < 0.0) return result;

    const double sq = std::sqrt(disc);
    const double t1 = (-b - sq) / (2.0 * dd);
    const double t2 = (-b + sq) / (2.0 * dd);

    auto accept = [&](double t) {
        if (t >= -kEpsilon && t <= 1.0 + kEpsilon) {
            result.push_back(a + d * std::clamp(t, 0.0, 1.0));
        }
    };
    accept(t1);
    if (disc > kEpsilon) accept(t2);        // 相切时只取一个点
    return result;
}

} // namespace lycad
