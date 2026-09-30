// =============================================================================
//  LyCAD - core/LineEntity.cpp
// =============================================================================
#include "core/LineEntity.h"

#include "core/Format.h"

namespace lycad {

LineEntity::LineEntity()
    : Entity(EntityType::Line)
{
}

LineEntity::LineEntity(const Point2D& start, const Point2D& end)
    : Entity(EntityType::Line), m_start(start), m_end(end)
{
}

std::unique_ptr<Entity> LineEntity::clone() const
{
    auto copy = std::make_unique<LineEntity>(m_start, m_end);
    copyCommonPropertiesTo(*copy);
    return copy;
}

Box2D LineEntity::boundingBox() const
{
    return Box2D::fromCorners(m_start, m_end);
}

double LineEntity::distanceTo(const Point2D& p) const
{
    return distancePointToSegment(p, m_start, m_end);
}

Point2D LineEntity::closestPointTo(const Point2D& p) const
{
    return closestPointOnSegment(p, m_start, m_end);
}

bool LineEntity::crosses(const Box2D& box) const
{
    if (!box.isValid()) return false;
    return segmentIntersectsBox(m_start, m_end, box);
}

void LineEntity::transform(const Transform2D& t)
{
    m_start = t.apply(m_start);
    m_end = t.apply(m_end);
}

double LineEntity::length() const
{
    return m_start.distanceTo(m_end);
}

std::vector<Point2D> LineEntity::snapPoints() const
{
    // 端点 + 中点：AutoCAD 的「端点」「中点」捕捉
    return { m_start, m_end, midpoint() };
}

std::vector<Point2D> LineEntity::gripPoints() const
{
    // 夹点 0/1 为端点，夹点 2 为整段移动
    return { m_start, m_end, midpoint() };
}

bool LineEntity::moveGrip(std::size_t index, const Point2D& newPos)
{
    switch (index) {
    case 0: m_start = newPos; return true;
    case 1: m_end = newPos; return true;
    case 2: {
        const Vector2D delta = newPos - midpoint();
        m_start += delta;
        m_end += delta;
        return true;
    }
    default:
        return false;
    }
}

std::vector<Point2D> LineEntity::flatten(double sagitta) const
{
    Q_UNUSED(sagitta);
    return { m_start, m_end };
}

QString LineEntity::describe() const
{
    return QStringLiteral("直线  (%1) → (%2)  长度 %3")
        .arg(formatPoint(m_start), formatPoint(m_end), formatLength(length()));
}

QString LineEntity::geometrySummary() const
{
    return QStringLiteral("起点: %1\n终点: %2\n长度: %3\n倾角: %4")
        .arg(formatPoint(m_start), formatPoint(m_end),
             formatLength(length()), formatAngle(angle()));
}

std::optional<Point2D> LineEntity::intersectionWith(const LineEntity& other, bool extendBoth) const
{
    const Vector2D r = m_end - m_start;
    const Vector2D s = other.m_end - other.m_start;
    const double denom = cross(r, s);
    if (std::abs(denom) <= kEpsilon) return std::nullopt;

    const Vector2D qp = other.m_start - m_start;
    const double t = cross(qp, s) / denom;
    const double u = cross(qp, r) / denom;

    const double lo = extendBoth ? -kHugeValue : 0.0;
    const double hi = extendBoth ? kHugeValue : 1.0;
    if (t < lo - kEpsilon || t > hi + kEpsilon) return std::nullopt;
    if (u < lo - kEpsilon || u > hi + kEpsilon) return std::nullopt;

    return m_start + r * t;
}

} // namespace lycad
