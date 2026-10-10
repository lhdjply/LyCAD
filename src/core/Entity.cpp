// =============================================================================
//  LyCAD - core/Entity.cpp
// =============================================================================
#include "core/Entity.h"

#include "core/AciPalette.h"
#include "core/Layer.h"
#include "core/Linetype.h"

#include <algorithm>
#include <array>
#include <limits>

namespace lycad {

QString entityTypeName(EntityType type)
{
    switch (type) {
    case EntityType::Point:    return QStringLiteral("POINT");
    case EntityType::Line:     return QStringLiteral("LINE");
    case EntityType::Circle:   return QStringLiteral("CIRCLE");
    case EntityType::Arc:      return QStringLiteral("ARC");
    case EntityType::Polyline: return QStringLiteral("LWPOLYLINE");
    case EntityType::Text:     return QStringLiteral("TEXT");
    case EntityType::MText:    return QStringLiteral("MTEXT");
    case EntityType::Hatch:    return QStringLiteral("HATCH");
    case EntityType::Dimension:return QStringLiteral("DIMENSION");
    case EntityType::Spline:   return QStringLiteral("SPLINE");
    case EntityType::Ellipse:  return QStringLiteral("ELLIPSE");
    case EntityType::XLine:    return QStringLiteral("XLINE");
    case EntityType::Ray:      return QStringLiteral("RAY");
    case EntityType::Insert:   return QStringLiteral("INSERT");
    case EntityType::Table:    return QStringLiteral("ACAD_TABLE");
    }
    return QStringLiteral("UNKNOWN");
}

QString entityTypeDisplayName(EntityType type)
{
    switch (type) {
    case EntityType::Point:    return QStringLiteral("点");
    case EntityType::Line:     return QStringLiteral("直线");
    case EntityType::Circle:   return QStringLiteral("圆");
    case EntityType::Arc:      return QStringLiteral("圆弧");
    case EntityType::Polyline: return QStringLiteral("多段线");
    case EntityType::Text:     return QStringLiteral("文字");
    case EntityType::MText:    return QStringLiteral("多行文字");
    case EntityType::Hatch:    return QStringLiteral("填充");
    case EntityType::Dimension:return QStringLiteral("尺寸标注");
    case EntityType::Spline:   return QStringLiteral("样条曲线");
    case EntityType::Ellipse:  return QStringLiteral("椭圆");
    case EntityType::XLine:    return QStringLiteral("构造线");
    case EntityType::Ray:      return QStringLiteral("射线");
    case EntityType::Insert:   return QStringLiteral("块参照");
    case EntityType::Table:    return QStringLiteral("表格");
    }
    return QStringLiteral("未知图元");
}

namespace lineweight {

double toMillimeters(int dxfValue)
{
    switch (dxfValue) {
    case kByLayer: return -1.0;
    case kByBlock: return -2.0;
    case kDefault: return -3.0;
    default:
        if (dxfValue <= 0) return -3.0;
        return dxfValue / 100.0;
    }
}

QString toDisplayString(int dxfValue)
{
    switch (dxfValue) {
    case kByLayer: return QStringLiteral("随层");
    case kByBlock: return QStringLiteral("随块");
    case kDefault: return QStringLiteral("默认");
    default: break;
    }
    if (dxfValue <= 0) return QStringLiteral("默认");
    return QStringLiteral("%1 mm").arg(dxfValue / 100.0, 0, 'f', 2);
}

int fromMillimeters(double mm)
{
    if (mm <= 0.0) return kDefault;
    // DXF 370 的合法标准值集合
    static constexpr std::array<int, 20> kStandard = {
        0, 5, 9, 13, 15, 18, 20, 25, 30, 35, 40, 50, 53, 60, 70, 80, 90, 100, 106, 120
    };
    const int raw = static_cast<int>(std::lround(mm * 100.0));
    int best = kStandard.front();
    int bestDelta = std::abs(raw - best);
    for (int v : kStandard) {
        const int delta = std::abs(raw - v);
        if (delta < bestDelta) { bestDelta = delta; best = v; }
    }
    return best;
}

} // namespace lineweight

// -----------------------------------------------------------------------------
//  Entity
// -----------------------------------------------------------------------------
QColor Entity::resolvedColor(const LayerManager& layers, bool themeIsDark) const
{
    int aci = m_colorIndex;
    if (aci <= 0) {
        // 随层（<0）与随块（0）都落到图层颜色上：块内的随块图元在展开阶段
        // 已被 InsertEntity::collectInto 换成块参照自身的颜色，走到这里说明
        // 这条图元不在任何块里（或块参照本身也随层），按图层解析即是正解。
        if (const Layer* lyr = layers.find(m_layerName)) {
            aci = lyr->colorIndex();
        } else {
            aci = 7;
        }
    }
    return lycad::aci::toColor(aci, themeIsDark);
}

double Entity::resolvedLineweight(const LayerManager& layers, double defaultMm) const
{
    if (m_lineweight >= 0) return m_lineweight / 100.0;
    if (m_lineweight == lineweight::kByLayer) {
        if (const Layer* lyr = layers.find(m_layerName)) {
            const double lw = lineweight::toMillimeters(lyr->lineweightDxf());
            if (lw > 0.0) return lw;
        }
    }
    return defaultMm;
}

void Entity::setLinetypeName(const QString& name)
{
    // 空字符串表示「随层」，需原样保留；其余名称归一化到内置线型的规范拼写
    m_linetypeName = name.trimmed().isEmpty() ? QString() : linetypes::canonicalName(name);
}

bool Entity::isDisplayable(const LayerManager& layers) const
{
    if (!m_visible) return false;
    if (const Layer* lyr = layers.find(m_layerName)) {
        return !lyr->isHidden();
    }
    return true;   // 图层缺失时按可见处理，避免图元凭空消失
}

bool Entity::isInside(const Box2D& box) const
{
    if (!box.isValid()) return false;
    const Box2D bb = boundingBox();
    return bb.isValid() && box.contains(bb, 1e-9);
}

bool Entity::crosses(const Box2D& box) const
{
    if (!box.isValid()) return false;
    return detail::pointsCrossBox(flatten(detail::sagittaForBox(box)), isClosed(), box);
}

bool Entity::moveGrip(std::size_t index, const Point2D& newPos)
{
    Q_UNUSED(index);
    Q_UNUSED(newPos);
    return false;
}

Point2D Entity::closestPointTo(const Point2D& p) const
{
    const std::vector<Point2D> pts = flatten(1e-3);
    if (pts.empty()) return p;
    if (pts.size() == 1) return pts.front();

    Point2D best = pts.front();
    double bestDist = std::numeric_limits<double>::max();
    const std::size_t segCount = isClosed() ? pts.size() : pts.size() - 1;
    for (std::size_t i = 0; i < segCount; ++i) {
        const Point2D& a = pts[i];
        const Point2D& b = pts[(i + 1) % pts.size()];
        const Point2D c = closestPointOnSegment(p, a, b);
        const double d = c.distanceTo(p);
        if (d < bestDist) { bestDist = d; best = c; }
    }
    return best;
}

void Entity::copyCommonPropertiesTo(Entity& target) const
{
    target.m_layerName = m_layerName;
    target.m_colorIndex = m_colorIndex;
    target.m_lineweight = m_lineweight;
    target.m_linetypeName = m_linetypeName;
    target.m_visible = m_visible;
}

// -----------------------------------------------------------------------------
//  detail
// -----------------------------------------------------------------------------
namespace detail {

bool pointsInsideBox(const std::vector<Point2D>& pts, bool closed, const Box2D& box)
{
    Q_UNUSED(closed);
    if (pts.empty() || !box.isValid()) return false;
    for (const Point2D& p : pts) {
        if (!box.contains(p, 1e-9)) return false;
    }
    return true;
}

bool pointsCrossBox(const std::vector<Point2D>& pts, bool closed, const Box2D& box)
{
    if (pts.empty() || !box.isValid()) return false;
    if (pts.size() == 1) return box.contains(pts.front(), 1e-9);
    return polylineIntersectsBox(pts.data(), pts.size(), closed, box);
}

double sagittaForBox(const Box2D& box)
{
    if (!box.isValid()) return 0.1;
    // 以框对角线为尺度，取千分之一作为离散精度上限
    const double d = box.diagonal();
    if (d <= 0.0) return 0.1;
    return std::clamp(d / 2000.0, 1e-6, 50.0);
}

} // namespace detail

} // namespace lycad
