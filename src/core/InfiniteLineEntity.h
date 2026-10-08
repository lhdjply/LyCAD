// =============================================================================
//  LyCAD - core/InfiniteLineEntity.h
//  无界直线图元的公共基类：构造线（XLINE）与射线（RAY）。
//
//  几何约定（与 DXF 一致）
//    · 由「基点 + 单位方向向量」定义，而不是两点——因为它们没有终点。
//      DXF 的 XLINE / RAY 也正是这么存的（10 组码为基点，11 组码为单位方向）。
//    · 构造线沿方向双向无限延伸，射线只沿方向的一侧延伸，两者仅差一个
//      extendsBackward()，因此共享全部几何实现。
//
//  包围盒
//    无界图元没有有限范围，boundingBox() 返回**无效盒**（详见 Entity.h）。
//    由此得到两个正确的副产品，无需任何额外分支：
//      · 范围缩放 / 出图 / $EXTMIN 处的 Box2D::expand 会自然跳过它们；
//      · 窗选（isInside）恒为假——无限长的线不可能「完全落在框内」。
//
//  裁剪
//    无界图元唯一可绘制、可比较的形式是「被裁到某个矩形内的线段」，因此本类
//    提供 clippedTo()：渲染按视口裁、捕捉按光标邻域裁、框选与拾取按选区裁，
//    三处共用同一份实现，避免各自写一遍梁-巴斯基算法而在边界条件上跑偏。
// =============================================================================
#pragma once

#include "core/Entity.h"

#include <optional>
#include <utility>

namespace lycad {

/// flatten() 的虚拟延展半长（世界单位）。
///
/// 无界图元的离散化没有规范答案，本类按此长度在基点两侧（射线的反向侧除外）
/// 截断，得到一个「足够长的线段」。它**只服务于不关心真实范围、只需要一条过
/// 基点的直线的内部算法**；绘制走 EntityRenderer 的视口裁剪、捕捉走裁剪到光标
/// 邻域的专用路径，二者都不依赖这个值。取 1e3 米量级：比任何真实图纸都长，
/// 又远未到让浮点相乘失真的地步。
inline constexpr double kUnboundedFlattenExtent = 1.0e6;

class InfiniteLineEntity : public Entity {
public:
    const Point2D& basePoint() const noexcept { return m_base; }
    /// 单位方向向量（正常状态下模长恒为 1）
    const Vector2D& direction() const noexcept { return m_direction; }

    /// 姿态角，值域 (-π, π]
    double angle() const noexcept { return m_direction.angle(); }

    /// 方向是否退化（零向量）——此时图元不可绘制，也不与任何东西相交
    bool isDegenerate() const noexcept
    {
        return m_direction.lengthSquared() <= kGeomTol * kGeomTol;
    }

    void setBasePoint(const Point2D& p) noexcept { m_base = p; }

    /// 设置方向并按单位向量存储。零向量被忽略（保持原方向）：
    /// 让「拖到基点上了」这类退化输入留下一个仍可绘制、仍可再拖的图元，
    /// 而不是一个画不出来、也选不中的幽灵。
    void setDirection(const Vector2D& dir);

    /// 以两点定义：基点 = from，方向 = normalize(through - from)。
    /// 两点重合时保持原方向，由调用方决定是否视为无效输入。
    void setByPoints(const Point2D& from, const Point2D& through);

    /// 是否向基点后方（-direction）延伸：构造线为真，射线为假
    virtual bool extendsBackward() const noexcept = 0;

    /// 参数域下界。位置定义为 P(t) = basePoint() + direction() * t
    double minParameter() const noexcept { return extendsBackward() ? -kHugeValue : 0.0; }

    /// 参数 t 处的位置
    Point2D pointAt(double t) const noexcept { return m_base + m_direction * t; }

    /// 把无界直线裁到轴对齐盒内，返回盒内的那一段（按参数从小到大）。
    /// 与盒无交集、或方向退化时返回 std::nullopt；
    /// 擦角/相切时返回的两个端点重合。
    std::optional<std::pair<Point2D, Point2D>> clippedTo(const Box2D& box) const;

    // ---- Entity 接口 ---------------------------------------------------------
    /// 恒为无效盒：无界图元没有有限范围
    Box2D boundingBox() const override;
    double distanceTo(const Point2D& p) const override;
    Point2D closestPointTo(const Point2D& p) const override;
    bool crosses(const Box2D& box) const override;
    void transform(const Transform2D& t) override;
    /// 无限长；界面与统计处必须先行排除（见 isUnbounded()）
    double length() const override;
    std::vector<Point2D> snapPoints() const override;
    /// 夹点：唯一一个，即定义点（拖拽它整体平移，方向不变）
    bool moveGrip(std::size_t index, const Point2D& newPos) override;
    std::vector<Point2D> flatten(double sagitta) const override;

    bool isUnbounded() const noexcept override { return true; }
    std::optional<Point2D> definitionPoint() const override { return m_base; }

    QString describe() const override;
    QString geometrySummary() const override;

protected:
    InfiniteLineEntity(EntityType type, const Point2D& base, const Vector2D& direction);

    /// 复制几何（供子类 clone 复用；公共显示属性走 copyCommonPropertiesTo）
    void copyGeometryTo(InfiniteLineEntity& target) const;

    /// 基点在文案中的称呼（构造线「基点」/ 射线「起点」）
    virtual QString baseLabel() const = 0;

private:
    /// 到基点的参数（把 p 投影到直线上；射线在基点后方时截到 0）
    double parameterOf(const Point2D& p) const noexcept;

    Point2D  m_base;
    Vector2D m_direction{ 1.0, 0.0 };
};

} // namespace lycad
