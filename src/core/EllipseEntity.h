// =============================================================================
//  LyCAD - core/EllipseEntity.h
//  椭圆 / 椭圆弧。
//
//  数据来自 DXF 的 ELLIPSE 实体：中心点、主轴端点（相对中心的向量，11 组码）、
//  副主轴比（40）、起止参数（41/42，弧度，主轴方向为参数零点）。
// =============================================================================
#pragma once

#include "core/Entity.h"

#include <cmath>

namespace lycad {

class EllipseEntity final : public Entity {
public:
    EllipseEntity();
    EllipseEntity(const Point2D& center, const Point2D& majorAxisEnd,
                  double ratio, double startParam, double endParam);

    // ---- 数据 ------------------------------------------------------------------
    const Point2D& center() const noexcept { return m_center; }
    void setCenter(const Point2D& p) noexcept;

    /// 主轴端点（相对中心的向量），其模长即长半轴
    const Point2D& majorAxisEnd() const noexcept { return m_majorAxisEnd; }
    void setMajorAxisEnd(const Point2D& v) noexcept;

    /// 副主轴比（短半轴 / 长半轴），(0, 1]
    double ratio() const noexcept { return m_ratio; }
    void setRatio(double ratio) noexcept;

    double startParam() const noexcept { return m_startParam; }
    double endParam() const noexcept { return m_endParam; }
    void setParamRange(double startRad, double endRad) noexcept;

    /// 起止参数跨度（已归一到 (0, 2π]）
    double spanParam() const noexcept;

    // ---- 派生量 ------------------------------------------------------------------
    double majorRadius() const noexcept { return std::hypot(m_majorAxisEnd.x, m_majorAxisEnd.y); }
    double minorRadius() const noexcept;
    double majorAxisAngle() const noexcept { return std::atan2(m_majorAxisEnd.y, m_majorAxisEnd.x); }

    // ---- Entity 接口 --------------------------------------------------------------
    std::unique_ptr<Entity> clone() const override;
    Box2D boundingBox() const override;
    double distanceTo(const Point2D& p) const override;
    void transform(const Transform2D& t) override;
    double length() const override;
    std::vector<Point2D> snapPoints() const override;
    std::vector<Point2D> flatten(double sagitta) const override;
    bool isClosed() const override;
    QString describe() const override;
    QString geometrySummary() const override;

private:
    Point2D m_center;
    Point2D m_majorAxisEnd;      ///< 相对中心的向量
    double m_ratio = 1.0;
    double m_startParam = 0.0;   ///< 弧度
    double m_endParam = 0.0;     ///< 弧度
};

} // namespace lycad
