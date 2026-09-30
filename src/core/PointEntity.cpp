// =============================================================================
//  LyCAD - core/PointEntity.cpp
// =============================================================================
#include "core/PointEntity.h"

#include "core/Format.h"

namespace lycad {

PointEntity::PointEntity()
    : Entity(EntityType::Point)
{
}

PointEntity::PointEntity(const Point2D& position)
    : Entity(EntityType::Point), m_position(position)
{
}

std::unique_ptr<Entity> PointEntity::clone() const
{
    auto copy = std::make_unique<PointEntity>(m_position);
    copyCommonPropertiesTo(*copy);
    return copy;
}

Box2D PointEntity::boundingBox() const
{
    Box2D box;
    box.expand(m_position);
    return box;
}

double PointEntity::distanceTo(const Point2D& p) const
{
    return p.distanceTo(m_position);
}

Point2D PointEntity::closestPointTo(const Point2D& p) const
{
    Q_UNUSED(p);
    return m_position;
}

bool PointEntity::crosses(const Box2D& box) const
{
    return box.contains(m_position, 1e-9);
}

void PointEntity::transform(const Transform2D& t)
{
    m_position = t.apply(m_position);
}

double PointEntity::length() const
{
    return 0.0;
}

std::vector<Point2D> PointEntity::snapPoints() const
{
    return { m_position };
}

std::vector<Point2D> PointEntity::flatten(double sagitta) const
{
    Q_UNUSED(sagitta);
    return { m_position };
}

QString PointEntity::describe() const
{
    return QStringLiteral("点  (%1)").arg(formatPoint(m_position));
}

QString PointEntity::geometrySummary() const
{
    return QStringLiteral("位置: %1\nX: %2\nY: %3")
        .arg(formatPoint(m_position),
             formatLength(m_position.x), formatLength(m_position.y));
}

} // namespace lycad
