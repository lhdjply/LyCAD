// =============================================================================
//  LyCAD - core/XLineEntity.cpp
// =============================================================================
#include "core/XLineEntity.h"

namespace lycad {

XLineEntity::XLineEntity()
    : InfiniteLineEntity(EntityType::XLine, Point2D(), Vector2D(1.0, 0.0))
{
}

XLineEntity::XLineEntity(const Point2D& base, const Vector2D& direction)
    : InfiniteLineEntity(EntityType::XLine, base, direction)
{
}

XLineEntity XLineEntity::throughPoints(const Point2D& first, const Point2D& second)
{
    XLineEntity line;
    line.setByPoints(first, second);
    return line;
}

std::unique_ptr<Entity> XLineEntity::clone() const
{
    auto copy = std::make_unique<XLineEntity>();
    copyGeometryTo(*copy);
    copyCommonPropertiesTo(*copy);
    return copy;
}

} // namespace lycad
