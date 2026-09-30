// =============================================================================
//  LyCAD - core/HatchEntity.h
//  填充图元（DXF: HATCH）
//
//  建模范围
//    · 边界：单个闭合多边形（直线段）。DXF 的多环、样条边界暂不建模，
//      读取时取第一个外部环；导出时按「外部多段线环」写出。
//    · 图案：SOLID（实体填充）或按名称引用的图案（内置支持 ANSI31，
//      其余名称读取后按 ANSI31 式平行线渲染，名称本身完整往返保留）。
//    · 比例/角度：作用于图案线族，SOLID 时忽略。
// =============================================================================
#pragma once

#include "core/Entity.h"

namespace lycad {

class HatchEntity final : public Entity {
public:
    /// ANSI31 图案的基准线间距（世界单位，即 1/8 英寸）
    static constexpr double kAnsi31BaseSpacing = 3.175;
    /// ANSI31 图案线与 X 轴的夹角（弧度）
    static constexpr double kAnsi31BaseAngle = kPi / 4.0;

    HatchEntity();
    explicit HatchEntity(std::vector<Point2D> boundary);
    HatchEntity(std::vector<Point2D> boundary, const QString& patternName,
                double scale, double angleRad);
    /// 带凸度的边界（「2 顶点 + 凸度 1」即一个整圆，是 EDA 焊盘的常见形态）
    HatchEntity(std::vector<Point2D> boundary, std::vector<double> bulges);

    // ---- 边界 ----------------------------------------------------------------
    const std::vector<Point2D>& boundary() const noexcept { return m_boundary; }
    void setBoundary(std::vector<Point2D> pts) { m_boundary = std::move(pts); }

    std::size_t vertexCount() const noexcept { return m_boundary.size(); }

    /// 顶点很少时的 convenience：返回第 index 个顶点
    const Point2D& vertex(std::size_t index) const { return m_boundary.at(index); }

    /// 边界凸度（与顶点一一对应；缺省补 0）。凸度描述「自该顶点到下一顶点」段
    const std::vector<double>& bulges() const noexcept { return m_bulges; }
    double vertexBulge(std::size_t index) const
    { return index < m_bulges.size() ? m_bulges[index] : 0.0; }

    void setBoundaryWithBulges(std::vector<Point2D> pts, std::vector<double> bulges);

    // ---- 图案 ----------------------------------------------------------------
    const QString& patternName() const noexcept { return m_patternName; }
    void setPatternName(const QString& name) { m_patternName = name.trimmed(); }

    /// 实体填充（图案名 SOLID）
    bool isSolid() const noexcept;

    double patternScale() const noexcept { return m_scale; }
    void setPatternScale(double scale);
    /// 图案旋转角（弧度），叠加在图案自身的基准方向上
    double patternAngle() const noexcept { return m_angle; }
    void setPatternAngle(double radians) noexcept { m_angle = normalizeAngle(radians); }

    /// 图案线族的间距（世界单位）；SOLID 无意义
    double patternSpacing() const noexcept;

    /// 图案线方向（弧度）
    double patternLineAngle() const noexcept;

    /// 点是否位于边界内部（射线法）
    bool containsPoint(const Point2D& p) const;

    /// 边界多边形面积（鞋带公式）
    double polygonArea() const noexcept;

    // ---- Entity 接口 ---------------------------------------------------------
    std::unique_ptr<Entity> clone() const override;
    Box2D boundingBox() const override;
    double distanceTo(const Point2D& p) const override;
    void transform(const Transform2D& t) override;
    double length() const override;
    std::vector<Point2D> snapPoints() const override;
    std::vector<Point2D> gripPoints() const override;
    bool moveGrip(std::size_t index, const Point2D& newPos) override;
    std::vector<Point2D> flatten(double sagitta) const override;
    bool isClosed() const override { return m_boundary.size() >= 3; }
    QString describe() const override;
    QString geometrySummary() const override;

    static constexpr EntityType kType = EntityType::Hatch;

private:
    std::vector<Point2D> m_boundary;        ///< 闭合多边形顶点
    std::vector<double>  m_bulges;          ///< 与顶点一一对应的凸度（可为空 = 全直线）
    QString  m_patternName = QStringLiteral("ANSI31");
    double   m_scale = 1.0;
    double   m_angle = 0.0;                 ///< 弧度
};

} // namespace lycad
