// =============================================================================
//  LyCAD - core/CircleEntity.h
//  整圆图元（DXF: CIRCLE）
// =============================================================================
#pragma once

#include "core/Entity.h"

#include <algorithm>

namespace lycad {

class LineEntity;

class CircleEntity final : public Entity {
public:
    CircleEntity();
    CircleEntity(const Point2D& center, double radius);

    const Point2D& center() const noexcept { return m_center; }
    double radius() const noexcept { return m_radius; }
    double diameter() const noexcept { return m_radius * 2.0; }
    double circumference() const noexcept { return kTwoPi * m_radius; }
    double area() const noexcept { return kPi * m_radius * m_radius; }

    void setCenter(const Point2D& c) noexcept { m_center = c; }
    void setRadius(double r) noexcept { m_radius = std::max(0.0, r); }
    void setDiameter(double d) noexcept { setRadius(d * 0.5); }

    /// 象限点：0=正东, 1=正北, 2=正西, 3=正南
    Point2D quadrantPoint(int index) const;
    /// 圆上指定角度的点
    Point2D pointAtAngle(double radians) const;

    /// 是否为退化圆（半径为 0）
    bool isDegenerate() const noexcept { return m_radius <= kGeomTol; }

    /// 圆与直线的交点（用于捕捉与修剪）
    std::vector<Point2D> intersectionsWith(const LineEntity& line) const;

    // ---- Entity 接口 ---------------------------------------------------------
    std::unique_ptr<Entity> clone() const override;
    Box2D boundingBox() const override;
    double distanceTo(const Point2D& p) const override;
    Point2D closestPointTo(const Point2D& p) const override;
    bool crosses(const Box2D& box) const override;
    void transform(const Transform2D& t) override;
    double length() const override;
    std::vector<Point2D> snapPoints() const override;
    bool moveGrip(std::size_t index, const Point2D& newPos) override;
    std::vector<Point2D> flatten(double sagitta) const override;
    bool isClosed() const override { return true; }
    QString describe() const override;
    QString geometrySummary() const override;

    static constexpr EntityType kType = EntityType::Circle;

private:
    Point2D m_center;
    double  m_radius = 0.0;
};

} // namespace lycad
