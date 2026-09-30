// =============================================================================
//  LyCAD - core/ArcEntity.h
//  圆弧图元（DXF: ARC）
//
//  参数化约定
//    · 以圆心、半径、起始角、终止角描述，角度为弧度。
//    · 圆弧自起始角逆时针（CCW）扫至终止角，跨度属于 (0, 2π]。
//    · 该约定与 DXF 一致（DXF 以度存储、逆时针为正），读入时仅需换算单位。
//    · 镜像变换会使圆弧方向反转，故 transform() 在第 4 类变换下需交换起止角。
// =============================================================================
#pragma once

#include "core/Entity.h"

#include <algorithm>

namespace lycad {

class CircleEntity;
class LineEntity;

class ArcEntity final : public Entity {
public:
    ArcEntity();
    ArcEntity(const Point2D& center, double radius, double startAngleRad, double endAngleRad);

    /// 由三点（起点、弧上一点、终点）构造圆弧；三点共线时返回 std::nullopt
    static std::optional<ArcEntity> fromThreePoints(const Point2D& start,
                                                    const Point2D& onArc,
                                                    const Point2D& end);

    /// 由圆心、起点、终点构造圆弧，方向取逆时针自起点至终点
    static std::optional<ArcEntity> fromCenterStartEnd(const Point2D& center,
                                                       const Point2D& start,
                                                       const Point2D& end);

    const Point2D& center() const noexcept { return m_center; }
    double radius() const noexcept { return m_radius; }
    double startAngle() const noexcept { return m_startAngle; }
    double endAngle() const noexcept { return m_endAngle; }

    /// 逆时针跨度，值域 (0, 2π]
    double spanAngle() const;
    double circumference() const noexcept { return kTwoPi * m_radius; }
    double area() const;            ///< 扇形面积

    void setCenter(const Point2D& c) noexcept { m_center = c; }
    void setRadius(double r) noexcept { m_radius = std::max(0.0, r); }
    void setStartAngle(double a) noexcept { m_startAngle = normalizeAngle(a); }
    void setEndAngle(double a) noexcept { m_endAngle = normalizeAngle(a); }
    void setAngles(double startRad, double endRad) noexcept;

    Point2D pointAtAngle(double radians) const;
    Point2D startPoint() const { return pointAtAngle(m_startAngle); }
    Point2D endPoint() const { return pointAtAngle(m_endAngle); }
    /// 弧的中点（按角度中值取，非弦中点）
    Point2D middlePoint() const { return pointAtAngle(m_startAngle + spanAngle() * 0.5); }

    /// 判断某角度是否落在弧范围内
    bool containsAngle(double radians) const;

    bool isDegenerate() const noexcept { return m_radius <= kGeomTol; }

    /// 本弧所属的整圆
    CircleEntity toCircle() const;

    /// 以起点、终点为界把圆弧裁成两段（用于打断/修剪）
    /// keepFromStart 为 true 时保留自起点起 spanRatio 比例的部分。
    std::optional<ArcEntity> trimmed(double fromAngle, double toAngle) const;

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
    QString describe() const override;
    QString geometrySummary() const override;

    static constexpr EntityType kType = EntityType::Arc;

private:
    Point2D m_center;
    double  m_radius = 0.0;
    double  m_startAngle = 0.0;
    double  m_endAngle = 0.0;
};

} // namespace lycad
