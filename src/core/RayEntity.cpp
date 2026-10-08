// =============================================================================
//  LyCAD - core/RayEntity.cpp
// =============================================================================
#include "core/RayEntity.h"

namespace lycad {

RayEntity::RayEntity()
    : InfiniteLineEntity(EntityType::Ray, Point2D(), Vector2D(1.0, 0.0))
{
}

RayEntity::RayEntity(const Point2D& start, const Vector2D& direction)
    : InfiniteLineEntity(EntityType::Ray, start, direction)
{
}

RayEntity RayEntity::throughPoints(const Point2D& start, const Point2D& second)
{
    RayEntity ray;
    ray.setByPoints(start, second);
    return ray;
}

std::unique_ptr<Entity> RayEntity::clone() const
{
    auto copy = std::make_unique<RayEntity>();
    copyGeometryTo(*copy);
    copyCommonPropertiesTo(*copy);
    return copy;
}

} // namespace lycad
