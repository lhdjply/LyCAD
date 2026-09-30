// =============================================================================
//  LyCAD - core/HatchEntity.cpp
// =============================================================================
#include "core/HatchEntity.h"

#include "core/Format.h"

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
    const std::size_t n = m_boundary.size();
    if (n < 3) return false;

    // 射线法：向 +X 方向发射，统计与各边的穿越次数
    bool inside = false;
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const Point2D& a = m_boundary[i];
        const Point2D& b = m_boundary[j];

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
    const std::size_t n = m_boundary.size();
    if (n < 3) return 0.0;

    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const Point2D& a = m_boundary[i];
        const Point2D& b = m_boundary[(i + 1) % n];
        sum += a.x * b.y - b.x * a.y;
    }
    return std::abs(sum) * 0.5;
}

std::unique_ptr<Entity> HatchEntity::clone() const
{
    auto copy = std::make_unique<HatchEntity>();
    copy->m_boundary     = m_boundary;
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
    return box;
}

double HatchEntity::distanceTo(const Point2D& p) const
{
    if (containsPoint(p)) return 0.0;

    double best = std::numeric_limits<double>::max();
    const std::size_t n = m_boundary.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Point2D& a = m_boundary[i];
        const Point2D& b = m_boundary[(i + 1) % n];
        best = std::min(best, distancePointToSegment(p, a, b));
    }
    return best;
}

void HatchEntity::transform(const Transform2D& t)
{
    for (Point2D& p : m_boundary) p = t.apply(p);
}

double HatchEntity::length() const
{
    // 填充的「长度」取边界周长，供属性面板汇总
    double perimeter = 0.0;
    const std::size_t n = m_boundary.size();
    for (std::size_t i = 0; i < n; ++i) {
        perimeter += m_boundary[i].distanceTo(m_boundary[(i + 1) % n]);
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
    Q_UNUSED(sagitta);
    return m_boundary;
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
