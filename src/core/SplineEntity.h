// =============================================================================
//  LyCAD - core/SplineEntity.h
//  样条曲线（B 样条 / NURBS 的非有理情形）。
//
//  数据来自 DXF 的 SPLINE 实体：控制点 + 节点向量 + 次数。libdxfrw 不保留
//  权重（41 组码），因此这里按非有理 B 样条处理——电气图、PDF/AI 转换产物
//  几乎都是这一类；带非均匀权重的有理曲线暂按权重 = 1 近似，如有需要可
//  在 flatten 中加入权重项升级为有理 de Boor。
//
//  曲线不在内存里持久展开：flatten(sagitta) 按弦高容差现算并缓存最近一次
//  结果，包围盒 / 拾取 / 长度都建立在它之上。
// =============================================================================
#pragma once

#include "core/Entity.h"

#include <vector>

namespace lycad {

class SplineEntity final : public Entity {
public:
    SplineEntity();
    SplineEntity(int degree, std::vector<Point2D> controlPoints,
                 std::vector<double> knots);

    // ---- 数据 ------------------------------------------------------------------
    int degree() const noexcept { return m_degree; }
    void setDegree(int degree) noexcept;

    const std::vector<Point2D>& controlPoints() const noexcept { return m_controlPoints; }
    void setControlPoints(std::vector<Point2D> points);

    const std::vector<double>& knots() const noexcept { return m_knots; }
    void setKnots(std::vector<double> knots);

    /// 仅由拟合点定义的样条（无控制点）：按过点折线近似处理
    const std::vector<Point2D>& fitPoints() const noexcept { return m_fitPoints; }
    void setFitPoints(std::vector<Point2D> points);

    void setClosed(bool closed) noexcept;

    // ---- Entity 接口 --------------------------------------------------------------
    std::unique_ptr<Entity> clone() const override;
    Box2D boundingBox() const override;
    double distanceTo(const Point2D& p) const override;
    void transform(const Transform2D& t) override;
    double length() const override;
    std::vector<Point2D> snapPoints() const override;
    std::vector<Point2D> flatten(double sagitta) const override;
    bool isClosed() const override { return m_closed; }
    QString describe() const override;
    QString geometrySummary() const override;

private:
    void invalidateCache() const noexcept;

    int m_degree = 3;
    std::vector<Point2D> m_controlPoints;
    std::vector<double> m_knots;
    std::vector<Point2D> m_fitPoints;
    bool m_closed = false;

    mutable std::vector<Point2D> m_cache;
    mutable double m_cacheSagitta = -1.0;
};

} // namespace lycad
