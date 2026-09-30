// =============================================================================
//  LyCAD - core/SplineEntity.cpp
// =============================================================================
#include "core/SplineEntity.h"

#include "core/Format.h"

#include <algorithm>
#include <cmath>

namespace lycad {

namespace {

/// 在节点向量上找参数 t 所处的跨度：返回最大的 k 使 u[k] <= t，
/// 并夹取到有效求值范围 [p, n-1]（n 为控制点数）。
int findSpan(const std::vector<double>& knots, int degree, std::size_t controlCount,
             double t) noexcept
{
    const int last = static_cast<int>(controlCount) - 1;
    const double tMax = knots[knots.size() - 1 - static_cast<std::size_t>(degree)];
    if (t >= tMax) return last;

    // 线性查找足够（控制点数量普遍在个位到几十）；从有效范围起点向后扫
    int span = degree;
    const int limit = static_cast<int>(knots.size()) - degree - 1;
    for (int i = degree; i < limit; ++i) {
        if (t < knots[static_cast<std::size_t>(i + 1)]) {
            span = i;
            return span;
        }
    }
    return std::min(span, last);
}

/// de Boor 求值：B 样条曲线在参数 t 处的点（非有理；权重缺失按 1 处理）
Point2D deBoor(const std::vector<Point2D>& controlPoints,
               const std::vector<double>& knots, int degree, int span, double t) noexcept
{
    std::vector<Point2D> d(static_cast<std::size_t>(degree) + 1);
    for (int j = 0; j <= degree; ++j) {
        d[static_cast<std::size_t>(j)] = controlPoints[static_cast<std::size_t>(span - degree + j)];
    }
    for (int r = 1; r <= degree; ++r) {
        for (int j = degree; j >= r; --j) {
            const int knotIndex = span - degree + j;
            const double denominator =
                knots[static_cast<std::size_t>(knotIndex + degree - r + 1)]
                - knots[static_cast<std::size_t>(knotIndex)];
            const double alpha =
                denominator != 0.0 ? (t - knots[static_cast<std::size_t>(knotIndex)]) / denominator : 0.0;
            const Point2D base = d[static_cast<std::size_t>(j - 1)];
            const Point2D& cur = d[static_cast<std::size_t>(j)];
            d[static_cast<std::size_t>(j)] =
                Point2D(base.x + (cur.x - base.x) * alpha,
                        base.y + (cur.y - base.y) * alpha);
        }
    }
    return d[static_cast<std::size_t>(degree)];
}

} // namespace

SplineEntity::SplineEntity() : Entity(EntityType::Spline) {}

SplineEntity::SplineEntity(int degree, std::vector<Point2D> controlPoints,
                           std::vector<double> knots)
    : Entity(EntityType::Spline),
      m_degree(degree),
      m_controlPoints(std::move(controlPoints)),
      m_knots(std::move(knots))
{
}

void SplineEntity::setDegree(int degree) noexcept
{
    m_degree = std::clamp(degree, 1, 4);
    invalidateCache();
}

void SplineEntity::setControlPoints(std::vector<Point2D> points)
{
    m_controlPoints = std::move(points);
    invalidateCache();
}

void SplineEntity::setKnots(std::vector<double> knots)
{
    m_knots = std::move(knots);
    invalidateCache();
}

void SplineEntity::setFitPoints(std::vector<Point2D> points)
{
    m_fitPoints = std::move(points);
    invalidateCache();
}

void SplineEntity::setClosed(bool closed) noexcept
{
    m_closed = closed;
    invalidateCache();
}

std::unique_ptr<Entity> SplineEntity::clone() const
{
    auto copy = std::make_unique<SplineEntity>(m_degree, m_controlPoints, m_knots);
    copy->m_fitPoints = m_fitPoints;
    copy->m_closed = m_closed;
    copyCommonPropertiesTo(*copy);
    return copy;
}

std::vector<Point2D> SplineEntity::flatten(double sagitta) const
{
    if (m_cacheSagitta > 0.0 && std::abs(m_cacheSagitta - sagitta) < 1e-12
        && !m_cache.empty()) {
        return m_cache;
    }

    std::vector<Point2D> pts;
    const int p = std::max(m_degree, 1);
    const double tolerance = std::max(sagitta, 1e-9);

    // 控制点不足或节点向量不完整：退化为可用的点序列（过拟合点的折线 /
    // 控制点折线），宁可近似也不丢弃几何。
    if (static_cast<int>(m_controlPoints.size()) < p + 1
        || m_knots.size() < m_controlPoints.size() + static_cast<std::size_t>(p) + 1) {
        pts = m_fitPoints.size() >= 2 ? m_fitPoints : m_controlPoints;
        return pts;
    }

    const std::size_t controlCount = m_controlPoints.size();
    const double tMin = m_knots[static_cast<std::size_t>(p)];
    const double tMax = m_knots[m_knots.size() - 1 - static_cast<std::size_t>(p)];
    if (!(tMax > tMin)) return pts;

    // 逐节点区间细分。细分数由该区间控制多边形的弦长估计（曲线长度的上界），
    // 再按弦高容差换算：弦长 / (8 × 容差)，并夹取到合理范围。
    // 有效求值区间为 [u_p, u_{m-p}]，即 span ∈ [p, n-1]。
    for (int span = p; span + p + 1 < static_cast<int>(m_knots.size()); ++span) {
        const double u0 = m_knots[static_cast<std::size_t>(span)];
        const double u1 = m_knots[static_cast<std::size_t>(span + 1)];
        if (!(u1 > u0)) continue;

        double polygonLength = 0.0;
        for (int i = span - p; i < span; ++i) {
            if (i >= 0 && i + 1 < static_cast<int>(controlCount)) {
                polygonLength += m_controlPoints[static_cast<std::size_t>(i)]
                                     .distanceTo(m_controlPoints[static_cast<std::size_t>(i + 1)]);
            }
        }
        if (polygonLength <= tolerance) {
            pts.push_back(deBoor(m_controlPoints, m_knots, p, span, u0));
            continue;
        }
        int count = static_cast<int>(std::ceil(polygonLength / (8.0 * tolerance)));
        count = std::clamp(count, 4, 512);

        for (int i = 0; i < count; ++i) {
            const double t = u0 + (u1 - u0) * static_cast<double>(i) / count;
            pts.push_back(deBoor(m_controlPoints, m_knots, p, span, t));
        }
    }
    pts.push_back(deBoor(m_controlPoints, m_knots, p,
                         findSpan(m_knots, p, controlCount, tMax), tMax));

    if (m_closed && pts.size() >= 2) {
        // 闭合样条：收尾点并回接到起点
        pts.push_back(pts.front());
    }

    m_cache = pts;
    m_cacheSagitta = sagitta;
    return pts;
}

Box2D SplineEntity::boundingBox() const
{
    Box2D box;
    for (const Point2D& p : flatten(1e-3)) box.expand(p);
    return box;
}

double SplineEntity::distanceTo(const Point2D& p) const
{
    return p.distanceTo(closestPointTo(p));
}

void SplineEntity::transform(const Transform2D& t)
{
    for (Point2D& p : m_controlPoints) p = t.apply(p);
    for (Point2D& p : m_fitPoints) p = t.apply(p);
    invalidateCache();
}

double SplineEntity::length() const
{
    const std::vector<Point2D> pts = flatten(1e-3);
    double total = 0.0;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        total += pts[i - 1].distanceTo(pts[i]);
    }
    return total;
}

std::vector<Point2D> SplineEntity::snapPoints() const
{
    std::vector<Point2D> snaps;
    if (m_controlPoints.size() >= 2) {
        snaps.push_back(m_controlPoints.front());
        snaps.push_back(m_controlPoints.back());
    }
    return snaps;
}

QString SplineEntity::describe() const
{
    return QStringLiteral("样条曲线  %1 次  %2 个控制点%3")
        .arg(m_degree)
        .arg(m_controlPoints.size())
        .arg(m_closed ? QStringLiteral("（闭合）") : QString());
}

QString SplineEntity::geometrySummary() const
{
    return QStringLiteral("次数: %1\n控制点: %2 个\n节点: %3 个\n长度: %4%5")
        .arg(m_degree)
        .arg(m_controlPoints.size())
        .arg(m_knots.size())
        .arg(formatLength(length()))
        .arg(m_closed ? QStringLiteral("\n闭合: 是") : QString());
}

void SplineEntity::invalidateCache() const noexcept
{
    m_cache.clear();
    m_cacheSagitta = -1.0;
}

} // namespace lycad
