// =============================================================================
//  LyCAD - core/HatchEntity.cpp
// =============================================================================
#include "core/HatchEntity.h"

#include "core/Format.h"
#include "core/PolylineEntity.h"

#include <algorithm>
#include <cmath>

namespace lycad {

HatchEntity::HatchEntity()
    : Entity(EntityType::Hatch)
{
}

HatchEntity::HatchEntity(std::vector<Point2D> boundary)
    : Entity(EntityType::Hatch)
    , m_boundary(std::move(boundary))
{
}

HatchEntity::HatchEntity(std::vector<Point2D> boundary, const QString& patternName,
                         double scale, double angleRad)
    : Entity(EntityType::Hatch)
    , m_boundary(std::move(boundary))
    , m_patternName(patternName.trimmed())
    , m_angle(normalizeAngle(angleRad))
{
    setPatternScale(scale);
}

HatchEntity::HatchEntity(std::vector<Point2D> boundary, std::vector<double> bulges)
    : Entity(EntityType::Hatch)
    , m_boundary(std::move(boundary))
    , m_bulges(std::move(bulges))
{
    m_bulges.resize(m_boundary.size(), 0.0);
}

void HatchEntity::setBoundaryWithBulges(std::vector<Point2D> pts, std::vector<double> bulges)
{
    m_boundary = std::move(pts);
    m_bulges = std::move(bulges);
    m_bulges.resize(m_boundary.size(), 0.0);
}

/// 第 index 段的几何（自动按凸度还原为直线段或圆弧段），
/// 段序列与闭合多段线一致：0..n-1，末段回到首顶点
namespace {

PolylineSegment hatchSegment(const HatchEntity& hatch, std::size_t index)
{
    const std::size_t n = hatch.vertexCount();
    const std::size_t next = (index + 1) % n;
    return PolylineEntity::makeSegment(hatch.vertex(index), hatch.vertex(next),
                                       hatch.vertexBulge(index));
}

} // namespace

bool HatchEntity::isSolid() const noexcept
{
    return m_patternName.compare(QLatin1String("SOLID"), Qt::CaseInsensitive) == 0;
}

void HatchEntity::setPatternScale(double scale)
{
    m_scale = scale > kGeomTol ? scale : 1.0;
}

double HatchEntity::patternSpacing() const noexcept
{
    return kAnsi31BaseSpacing * m_scale;
}

double HatchEntity::patternLineAngle() const noexcept
{
    // 目前内置的平行线图案（ANSI31 系）基准方向均为 45°
    return normalizeAngle(kAnsi31BaseAngle + m_angle);
}

bool HatchEntity::containsPoint(const Point2D& p) const
{
    // 有凸度时边界不是直线多边形，统一用离散化后的点列做射线法
    const std::vector<Point2D> poly = flatten(kGeomTol);
    const std::size_t n = poly.size();
    if (n < 3) return false;

    // 射线法：向 +X 方向发射，统计与各边的穿越次数
    bool inside = false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Point2D& a = poly[i];
        const Point2D& b = poly[j];

        if ((a.y > p.y) != (b.y > p.y)) {
            const double t = (p.y - a.y) / (b.y - a.y);
            const double x = a.x + t * (b.x - a.x);
            if (p.x < x) inside = !inside;
        }
    }
    return inside;
}

double HatchEntity::polygonArea() const noexcept
{
    const std::vector<Point2D> poly = flatten(kGeomTol);
    const std::size_t n = poly.size();
    if (n < 3) return 0.0;

    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const Point2D& a = poly[i];
        const Point2D& b = poly[(i + 1) % n];
        sum += a.x * b.y - b.x * a.y;
    }
    return std::abs(sum) * 0.5;
}

std::unique_ptr<Entity> HatchEntity::clone() const
{
    auto copy = std::make_unique<HatchEntity>();
    copy->m_boundary     = m_boundary;
    copy->m_bulges       = m_bulges;
    copy->m_patternName  = m_patternName;
    copy->m_scale        = m_scale;
    copy->m_angle        = m_angle;
    copyCommonPropertiesTo(*copy);
    return copy;
}

Box2D HatchEntity::boundingBox() const
{
    Box2D box;
    for (const Point2D& p : m_boundary) box.expand(p);

    // 圆弧段（凸度）的极值可能越出顶点范围，按离散矢高并入
    const std::size_t n = m_boundary.size();
    for (std::size_t i = 0; i < n; ++i) {
        const PolylineSegment seg = hatchSegment(*this, i);
        if (!seg.isArc) continue;

        // 象限方向落在弧内时并入对应极值点
        const double span = seg.spanAngle();
        for (const double q : { 0.0, kHalfPi, kPi, 3.0 * kHalfPi }) {
            const double delta = normalizeAngle(q - seg.startAngle);
            const double ccwSpan = (seg.bulge >= 0.0) ? delta
                                                      : normalizeAngle(seg.startAngle - q);
            if (ccwSpan <= span + kGeomTol) {
                box.expand(Point2D(seg.center.x + seg.radius * std::cos(q),
                                   seg.center.y + seg.radius * std::sin(q)));
            }
        }
    }
    return box;
}

double HatchEntity::distanceTo(const Point2D& p) const
{
    if (containsPoint(p)) return 0.0;

    double best = std::numeric_limits<double>::max();
    const std::size_t n = m_boundary.size();
    for (std::size_t i = 0; i < n; ++i) {
        const PolylineSegment seg = hatchSegment(*this, i);
        best = std::min(best, p.distanceTo(seg.closestPointTo(p)));
    }
    return best;
}

void HatchEntity::transform(const Transform2D& t)
{
    for (Point2D& p : m_boundary) p = t.apply(p);

    // 镜像反转弧段绕行方向，凸度符号同步取反（与多段线约定一致）
    if (t.isMirroring()) {
        for (double& b : m_bulges) b = -b;
    }
}

double HatchEntity::length() const
{
    // 填充的「长度」取边界周长（圆弧段按弧长），供属性面板汇总
    double perimeter = 0.0;
    const std::size_t n = m_boundary.size();
    for (std::size_t i = 0; i < n; ++i) {
        perimeter += hatchSegment(*this, i).length();
    }
    return perimeter;
}

std::vector<Point2D> HatchEntity::snapPoints() const
{
    return m_boundary;
}

std::vector<Point2D> HatchEntity::gripPoints() const
{
    return m_boundary;
}

bool HatchEntity::moveGrip(std::size_t index, const Point2D& newPos)
{
    if (index >= m_boundary.size()) {
        // 越界夹点约定为「整体平移」
        const Box2D box = boundingBox();
        if (!box.isValid()) return false;
        const Vector2D delta = newPos - box.center();
        for (Point2D& p : m_boundary) p += delta;
        return true;
    }
    m_boundary[index] = newPos;
    return true;
}

std::vector<Point2D> HatchEntity::flatten(double sagitta) const
{
    // 无凸度时直接返回顶点；有凸度时按矢高把弧段离散为折线
    const bool hasBulge = std::any_of(m_bulges.begin(), m_bulges.end(),
                                      [](double b) { return std::abs(b) > kGeomTol; });
    if (!hasBulge) return m_boundary;

    std::vector<Point2D> pts;
    const std::size_t n = m_boundary.size();
    if (n < 2) return pts;

    const double sagg = std::max(sagitta, kGeomTol);

    for (std::size_t i = 0; i < n; ++i) {
        const PolylineSegment seg = hatchSegment(*this, i);
        pts.push_back(seg.start);

        if (!seg.isArc) continue;

        // 与多段线离散同策略：自起始角出发，按 bulge 的符号决定推进方向。
        // 不能「交换起止角再逆时针走」——那会让点序从终点折回起点，边界自交。
        const double span = seg.spanAngle();
        const double direction = (seg.bulge >= 0.0) ? 1.0 : -1.0;
        const int steps = segmentCountForArc(seg.radius, span, sagg);
        const double step = span / steps;
        for (int k = 1; k < steps; ++k) {
            const double a = seg.startAngle + direction * step * k;
            pts.push_back(Point2D(seg.center.x + seg.radius * std::cos(a),
                                  seg.center.y + seg.radius * std::sin(a)));
        }
    }
    return pts;
}

QString HatchEntity::describe() const
{
    return QStringLiteral("填充  %1  边界 %2 点")
        .arg(isSolid() ? QStringLiteral("实体") : m_patternName)
        .arg(m_boundary.size());
}

QString HatchEntity::geometrySummary() const
{
    const Box2D box = boundingBox();
    return QStringLiteral("图案: %1\n比例: %2\n角度: %3\n边界顶点: %4\n面积(近似): %5\n包围范围: (%6, %7) - (%8, %9)")
        .arg(isSolid() ? QStringLiteral("SOLID") : m_patternName,
             formatLength(m_scale),
             formatAngle(m_angle))
        .arg(m_boundary.size())
        .arg(formatArea(polygonArea()), formatLength(box.minX), formatLength(box.minY),
             formatLength(box.maxX), formatLength(box.maxY));
}

} // namespace lycad
