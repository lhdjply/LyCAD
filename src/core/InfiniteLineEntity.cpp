// =============================================================================
//  LyCAD - core/InfiniteLineEntity.cpp
// =============================================================================
#include "core/InfiniteLineEntity.h"

#include "core/Format.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lycad {

InfiniteLineEntity::InfiniteLineEntity(EntityType type, const Point2D& base,
                                       const Vector2D& direction)
    : Entity(type), m_base(base)
{
    setDirection(direction);
}

void InfiniteLineEntity::setDirection(const Vector2D& dir)
{
    const double len = dir.length();
    if (len <= kGeomTol) return;      // 零向量：保持原方向（见头文件说明）
    m_direction = dir / len;
}

void InfiniteLineEntity::setByPoints(const Point2D& from, const Point2D& through)
{
    m_base = from;
    setDirection(through - from);
}

void InfiniteLineEntity::copyGeometryTo(InfiniteLineEntity& target) const
{
    target.m_base = m_base;
    target.m_direction = m_direction;
}

// -----------------------------------------------------------------------------
//  裁剪
// -----------------------------------------------------------------------------
std::optional<std::pair<Point2D, Point2D>> InfiniteLineEntity::clippedTo(const Box2D& box) const
{
    if (!box.isValid() || isDegenerate()) return std::nullopt;

    // 梁-巴斯基（Liang–Barsky）参数裁剪：四条边界各给出一个参数区间，取交集。
    // 参数域下界由 minParameter() 给出——构造线为 -∞、射线为 0——
    // 于是「射线不从基点往后画」这件事自动落进同一套算法里，无需另写分支。
    double t0 = minParameter();
    double t1 = kHugeValue;

    const auto clip = [&](double p, double q) {
        if (std::abs(p) <= kEpsilon) return q >= 0.0;   // 与该边界平行：q<0 表示整条线都在外侧
        const double r = q / p;
        if (p < 0.0) {
            if (r > t1) return false;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return false;
            if (r < t1) t1 = r;
        }
        return true;
    };

    if (!clip(-m_direction.x, m_base.x - box.minX)) return std::nullopt;
    if (!clip( m_direction.x, box.maxX - m_base.x)) return std::nullopt;
    if (!clip(-m_direction.y, m_base.y - box.minY)) return std::nullopt;
    if (!clip( m_direction.y, box.maxY - m_base.y)) return std::nullopt;

    if (t1 < t0) return std::nullopt;

    // 基点离视口很远时（大坐标图纸 + 深度缩放），base + dir*t 会有抵消误差。
    // 端点的正确位置必然落在盒边界上，因此把结果夹回盒内：误差被投影回边界，
    // 得到的两点始终贴在盒边上，而不是浮在盒外一两个 ULP 处来回抖动。
    const auto pointAtClamped = [&](double t) {
        const Point2D p = m_base + m_direction * t;
        return Point2D(std::clamp(p.x, box.minX, box.maxX),
                       std::clamp(p.y, box.minY, box.maxY));
    };
    return std::make_pair(pointAtClamped(t0), pointAtClamped(t1));
}

// -----------------------------------------------------------------------------
//  几何查询
// -----------------------------------------------------------------------------
double InfiniteLineEntity::parameterOf(const Point2D& p) const noexcept
{
    // 单位方向向量下，点积即为「沿方向的投影长度」= 参数 t
    return std::max(dot(p - m_base, m_direction), minParameter());
}

double InfiniteLineEntity::distanceTo(const Point2D& p) const
{
    if (isDegenerate()) return p.distanceTo(m_base);
    return (p - m_base - m_direction * parameterOf(p)).length();
}

Point2D InfiniteLineEntity::closestPointTo(const Point2D& p) const
{
    if (isDegenerate()) return m_base;
    return pointAt(parameterOf(p));
}

bool InfiniteLineEntity::crosses(const Box2D& box) const
{
    // 「与框相交」对无界图元的精确语义就是「裁得出一段非空线段」。
    // 不能用基类基于 flatten 的默认实现：那会拖着一百万单位的线段做判定。
    return clippedTo(box).has_value();
}

void InfiniteLineEntity::transform(const Transform2D& t)
{
    m_base = t.apply(m_base);
    const Vector2D d = t.applyVector(m_direction);
    const double len = d.length();
    // 非等比缩放会改变方向（这正是期望的结果）；退化时保持原方向，
    // 不把图元变成一个零方向的空壳。
    if (len > kGeomTol) m_direction = d / len;
}

double InfiniteLineEntity::length() const
{
    return std::numeric_limits<double>::infinity();
}

std::vector<Point2D> InfiniteLineEntity::snapPoints() const
{
    // 无界图元没有端点与中点，只有定义点（AutoCAD 的 XLINE/RAY 亦如此）
    return { m_base };
}

bool InfiniteLineEntity::moveGrip(std::size_t index, const Point2D& newPos)
{
    // 唯一夹点：定义点。拖动它整体平移，方向保持不变。
    if (index != 0) return false;
    m_base = newPos;
    return true;
}

std::vector<Point2D> InfiniteLineEntity::flatten(double sagitta) const
{
    Q_UNUSED(sagitta);
    if (isDegenerate()) return { m_base };
    if (extendsBackward()) {
        return { pointAt(-kUnboundedFlattenExtent), pointAt(kUnboundedFlattenExtent) };
    }
    return { m_base, pointAt(kUnboundedFlattenExtent) };
}

Box2D InfiniteLineEntity::boundingBox() const
{
    return Box2D();      // 无界：没有有限范围（见 Entity.h）
}

// -----------------------------------------------------------------------------
//  文案
// -----------------------------------------------------------------------------
QString InfiniteLineEntity::describe() const
{
    return QStringLiteral("%1  %2 (%3)  方向 %4")
        .arg(typeDisplayName(), baseLabel(), formatPoint(m_base), formatAngle(angle()));
}

QString InfiniteLineEntity::geometrySummary() const
{
    return QStringLiteral("%1: %2\n方向: %3\n方向向量: %4, %5\n长度: 无限")
        .arg(baseLabel(), formatPoint(m_base), formatAngle(angle()),
             formatLength(m_direction.x), formatLength(m_direction.y));
}

} // namespace lycad
