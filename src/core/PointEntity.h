// =============================================================================
//  LyCAD - core/PointEntity.h
//  点图元（DXF: POINT）
//
//  虽然不属于题目要求的四类基本图元，但 DXF 文件中 POINT 极为常见，
//  若不支持会导致导入时静默丢数据，因此一并实现以保证往返保真。
// =============================================================================
#pragma once

#include "core/Entity.h"

namespace lycad {

class PointEntity final : public Entity {
public:
    PointEntity();
    explicit PointEntity(const Point2D& position);

    const Point2D& position() const noexcept { return m_position; }
    void setPosition(const Point2D& p) noexcept { m_position = p; }

    // ---- Entity 接口 ---------------------------------------------------------
    std::unique_ptr<Entity> clone() const override;
    Box2D boundingBox() const override;
    double distanceTo(const Point2D& p) const override;
    Point2D closestPointTo(const Point2D& p) const override;
    bool crosses(const Box2D& box) const override;
    void transform(const Transform2D& t) override;
    double length() const override;
    std::vector<Point2D> snapPoints() const override;
    std::vector<Point2D> flatten(double sagitta) const override;
    QString describe() const override;
    QString geometrySummary() const override;

    static constexpr EntityType kType = EntityType::Point;

private:
    Point2D m_position;
};

} // namespace lycad
