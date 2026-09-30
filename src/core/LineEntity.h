// =============================================================================
//  LyCAD - core/LineEntity.h
//  直线段图元（DXF: LINE）
// =============================================================================
#pragma once

#include "core/Entity.h"

namespace lycad {

class LineEntity final : public Entity {
public:
    LineEntity();
    LineEntity(const Point2D& start, const Point2D& end);

    const Point2D& start() const noexcept { return m_start; }
    const Point2D& end() const noexcept { return m_end; }
    void setStart(const Point2D& p) noexcept { m_start = p; }
    void setEnd(const Point2D& p) noexcept { m_end = p; }

    Point2D midpoint() const noexcept { return Point2D((m_start.x + m_end.x) * 0.5,
                                                       (m_start.y + m_end.y) * 0.5); }

    /// 方向向量（未单位化）
    Vector2D direction() const noexcept { return m_end - m_start; }

    /// 姿态角，值域 (-π, π]
    double angle() const noexcept { return (m_end - m_start).angle(); }

    /// 直线是否退化（两端点重合）
    bool isDegenerate() const noexcept { return m_start.distanceTo(m_end) <= kGeomTol; }

    void reverse() noexcept { std::swap(m_start, m_end); }

    /// 与另一直线的交点（含延长线选项）
    std::optional<Point2D> intersectionWith(const LineEntity& other, bool extendBoth = false) const;

    // ---- Entity 接口 ---------------------------------------------------------
    std::unique_ptr<Entity> clone() const override;
    Box2D boundingBox() const override;
    double distanceTo(const Point2D& p) const override;
    Point2D closestPointTo(const Point2D& p) const override;
    bool crosses(const Box2D& box) const override;
    void transform(const Transform2D& t) override;
    double length() const override;
    std::vector<Point2D> snapPoints() const override;
    std::vector<Point2D> gripPoints() const override;
    bool moveGrip(std::size_t index, const Point2D& newPos) override;
    std::vector<Point2D> flatten(double sagitta) const override;
    QString describe() const override;
    QString geometrySummary() const override;

    /// 图元类型判别捷径，避免调用方使用 dynamic_cast
    static constexpr EntityType kType = EntityType::Line;

private:
    Point2D m_start;
    Point2D m_end;
};

} // namespace lycad
