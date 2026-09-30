// =============================================================================
//  LyCAD - core/DimensionEntity.cpp
// =============================================================================
#include "core/DimensionEntity.h"

#include "core/Format.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace lycad {

namespace {

/// 界线起点与定义点的间隙（单位：字高）
constexpr double kExtensionGap = 0.18;
/// 界线超出尺寸线的长度（单位：字高）
constexpr double kExtensionBeyond = 0.30;
/// 测量文字基线相对尺寸线的偏移（单位：字高）
constexpr double kTextOffset = 0.62;
/// 箭头宽高比
constexpr double kArrowWidthRatio = 1.0 / 3.0;

/// 估算显示文字宽度（单位：字高）。标注文字以数字为主，取略宽的系数
double textWidthFactor(const QString& text)
{
    double factor = 0.0;
    for (const QChar ch : text) {
        const ushort code = ch.unicode();
        if (code >= 0x2E80) factor += 1.0;              // CJK / 全角
        else if (code == U'.') factor += 0.3;
        else factor += 0.58;
    }
    return std::max(factor, 1.0);
}

/// 让文字方向保持可读：倾角落在 (90°, 270°] 时翻转 180°
double readableAngle(double radians)
{
    radians = normalizeAngle(radians);
    const double deg = radToDeg(radians);
    if (deg > 90.0 && deg <= 270.0) {
        radians = normalizeAngle(radians + kPi);
    }
    return radians;
}

} // namespace

DimensionEntity::DimensionEntity()
    : Entity(EntityType::Dimension)
{
}

DimensionEntity::DimensionEntity(DimType type)
    : Entity(EntityType::Dimension)
    , m_dimType(type)
{
}

void DimensionEntity::setTextHeight(double h)
{
    if (h > kGeomTol) m_textHeight = h;
}

double DimensionEntity::measurement() const noexcept
{
    switch (m_dimType) {
    case DimType::Linear: {
        // 线性标注沿尺寸线方向度量：水平标注（旋转 0°）测 X 距离，
        // 垂直标注（旋转 90°）测 Y 距离，与 AutoCAD 语义一致
        const Vector2D u(std::cos(m_rotation), std::sin(m_rotation));
        return std::abs(dot(m_secondPoint - m_firstPoint, u));
    }
    case DimType::Aligned:
    case DimType::Radial:
    case DimType::Diametric:
        return m_firstPoint.distanceTo(m_secondPoint);
    }
    return 0.0;
}

QString DimensionEntity::autoText() const
{
    switch (m_dimType) {
    case DimType::Radial:    return QStringLiteral("R%1").arg(formatLength(measurement()));
    case DimType::Diametric: return QStringLiteral("%1%2").arg(QChar(0x00D8), formatLength(measurement()));
    default:                 return formatLength(measurement());
    }
}

QString DimensionEntity::displayText(bool dxfStyle) const
{
    if (!m_textOverride.isEmpty() && m_textOverride != QLatin1String("<>")) {
        return m_textOverride;
    }
    QString text = autoText();
    if (dxfStyle) text.replace(QChar(0x00D8), QStringLiteral("%%c"));
    return text;
}

DimensionEntity::Geometry DimensionEntity::geometry() const
{
    Geometry geo;
    geo.textHeight = m_textHeight;
    geo.text = displayText();
    geo.valid = true;

    const double gap = kExtensionGap * m_textHeight;
    const double beyond = kExtensionBeyond * m_textHeight;
    const double arrowLen = arrowLength();
    const double arrowHalf = arrowLen * kArrowWidthRatio;

    auto addArrow = [&geo, arrowLen, arrowHalf](const Point2D& tip, const Vector2D& outward) {
        const Vector2D u = outward.normalizedOr(Vector2D(1.0, 0.0));
        const Vector2D perp = u.perpendicular();
        const Point2D baseCenter = tip + u * arrowLen;
        Arrow arrow;
        arrow.tip = tip;
        arrow.base1 = baseCenter + perp * arrowHalf;
        arrow.base2 = baseCenter - perp * arrowHalf;
        geo.arrows.push_back(arrow);
    };

    switch (m_dimType) {
    case DimType::Linear:
    case DimType::Aligned: {
        const Vector2D dir = (m_dimType == DimType::Linear)
            ? Vector2D(std::cos(m_rotation), std::sin(m_rotation))
            : (m_secondPoint - m_firstPoint).normalizedOr(Vector2D(1.0, 0.0));
        const Vector2D n = dir.perpendicular();

        // 界线垂足：定义点沿 n 投影到尺寸线所在直线
        const Vector2D offset1 = m_dimLinePoint - m_firstPoint;
        const Vector2D offset2 = m_dimLinePoint - m_secondPoint;
        const double d1 = dot(offset1, n);
        const double d2 = dot(offset2, n);

        const Point2D foot1 = m_firstPoint + n * d1;
        const Point2D foot2 = m_secondPoint + n * d2;

        // 界线：从定义点留出间隙，画到垂足并越过尺寸线一小段
        auto addExtension = [&](const Point2D& p, const Point2D& foot, double d) {
            if (std::abs(d) <= kGeomTol) return;
            const Vector2D u = (foot - p).normalizedOr(n);
            const Point2D start = p + u * std::min(gap, std::abs(d) * 0.5);
            const Point2D end = foot + u * beyond;
            geo.segments.emplace_back(start, end);
        };
        addExtension(m_firstPoint, foot1, d1);
        addExtension(m_secondPoint, foot2, d2);

        // 尺寸线
        geo.segments.emplace_back(foot1, foot2);

        // 箭头指向两端的界线
        addArrow(foot1, -dir);
        addArrow(foot2, dir);

        // 文字：位于尺寸线的可读方向一侧
        const double textDirAngle = readableAngle(std::atan2(dir.y, dir.x));
        geo.textAngle = textDirAngle;
        const Vector2D textDir(std::cos(textDirAngle), std::sin(textDirAngle));
        const Point2D mid((foot1.x + foot2.x) * 0.5, (foot1.y + foot2.y) * 0.5);
        geo.textAnchor = mid + textDir.perpendicular() * (kTextOffset * m_textHeight);
        break;
    }

    case DimType::Radial: {
        const Point2D center = m_firstPoint;
        const Point2D onCircle = m_secondPoint;
        const Vector2D u = (onCircle - center).normalizedOr(Vector2D(1.0, 0.0));

        geo.segments.emplace_back(center, onCircle);
        addArrow(onCircle, u);

        // 引线：从圆上点画向文字位置，避开文字本身
        Point2D anchor = m_dimLinePoint;
        if (anchor.distanceTo(onCircle) <= kGeomTol) {
            anchor = onCircle + u * (m_textHeight * 2.0);   // 未指定时沿径向外移
        }
        geo.textAngle = readableAngle(center.angleTo(anchor));
        geo.textAnchor = anchor;

        const double halfText = textWidthFactor(geo.text) * m_textHeight * 0.5;
        const double leaderLen = onCircle.distanceTo(anchor);
        const double trim = std::min(halfText + m_textHeight * 0.15, leaderLen * 0.8);
        if (leaderLen > trim + kGeomTol) {
            const Vector2D lu = (anchor - onCircle).normalizedOr(u);
            geo.segments.emplace_back(onCircle, anchor - lu * trim);
        }
        break;
    }

    case DimType::Diametric: {
        const Point2D p1 = m_firstPoint;
        const Point2D p2 = m_secondPoint;

        geo.segments.emplace_back(p1, p2);
        addArrow(p1, (p1 - p2).normalizedOr(Vector2D(1.0, 0.0)));
        addArrow(p2, (p2 - p1).normalizedOr(Vector2D(1.0, 0.0)));

        Point2D anchor = m_dimLinePoint;
        if (anchor.distanceTo(Point2D((p1.x + p2.x) * 0.5, (p1.y + p2.y) * 0.5)) <= kGeomTol) {
            anchor = p2 + (p2 - p1).normalizedOr(Vector2D(1.0, 0.0)) * (m_textHeight * 2.0);
        }
        geo.textAngle = readableAngle(p1.angleTo(anchor));
        geo.textAnchor = anchor;
        break;
    }
    }

    return geo;
}

std::unique_ptr<Entity> DimensionEntity::clone() const
{
    auto copy = std::make_unique<DimensionEntity>();
    copy->m_dimType       = m_dimType;
    copy->m_firstPoint    = m_firstPoint;
    copy->m_secondPoint   = m_secondPoint;
    copy->m_dimLinePoint  = m_dimLinePoint;
    copy->m_rotation      = m_rotation;
    copy->m_textOverride  = m_textOverride;
    copy->m_textHeight    = m_textHeight;
    copyCommonPropertiesTo(*copy);
    return copy;
}

Box2D DimensionEntity::boundingBox() const
{
    const Geometry geo = geometry();
    if (!geo.valid) return Box2D();

    Box2D box;
    for (const auto& seg : geo.segments) {
        box.expand(seg.first);
        box.expand(seg.second);
    }
    for (const Arrow& arrow : geo.arrows) {
        box.expand(arrow.tip);
        box.expand(arrow.base1);
        box.expand(arrow.base2);
    }
    if (!geo.text.isEmpty()) {
        const double halfW = textWidthFactor(geo.text) * m_textHeight * 0.5;
        const double halfH = m_textHeight * 0.6;
        const double cosA = std::cos(geo.textAngle);
        const double sinA = std::sin(geo.textAngle);
        // 旋转矩形的四个角
        const std::array<Vector2D, 4> corners = {
            Vector2D(-halfW, -halfH), Vector2D(halfW, -halfH),
            Vector2D(halfW, halfH),  Vector2D(-halfW, halfH)
        };
        for (const Vector2D& c : corners) {
            const Vector2D rotated(c.x * cosA - c.y * sinA, c.x * sinA + c.y * cosA);
            box.expand(geo.textAnchor + rotated);
        }
    }
    return box;
}

double DimensionEntity::distanceTo(const Point2D& p) const
{
    const Geometry geo = geometry();
    if (!geo.valid) return std::numeric_limits<double>::max();

    double best = std::numeric_limits<double>::max();
    for (const auto& seg : geo.segments) {
        best = std::min(best, distancePointToSegment(p, seg.first, seg.second));
    }
    for (const Arrow& arrow : geo.arrows) {
        best = std::min(best, distancePointToSegment(p, arrow.tip, arrow.base1));
        best = std::min(best, distancePointToSegment(p, arrow.tip, arrow.base2));
        best = std::min(best, distancePointToSegment(p, arrow.base1, arrow.base2));
    }
    if (!geo.text.isEmpty()) {
        // 文字附近同样可拾取：以文字包围圆近似
        const double halfW = textWidthFactor(geo.text) * m_textHeight * 0.5;
        const double radius = std::hypot(halfW, m_textHeight * 0.5);
        best = std::min(best, std::max(0.0, geo.textAnchor.distanceTo(p) - radius));
    }
    return best;
}

void DimensionEntity::transform(const Transform2D& t)
{
    m_firstPoint = t.apply(m_firstPoint);
    m_secondPoint = t.apply(m_secondPoint);
    m_dimLinePoint = t.apply(m_dimLinePoint);

    if (t.isSimilarity()) {
        m_textHeight *= t.uniformScale();
        if (m_dimType == DimType::Linear) {
            m_rotation = normalizeAngle(m_rotation + t.rotationAngle());
        }
    }
}

double DimensionEntity::length() const
{
    return 0.0;
}

std::vector<Point2D> DimensionEntity::snapPoints() const
{
    return { m_firstPoint, m_secondPoint, m_dimLinePoint };
}

std::vector<Point2D> DimensionEntity::gripPoints() const
{
    return { m_firstPoint, m_secondPoint, m_dimLinePoint };
}

bool DimensionEntity::moveGrip(std::size_t index, const Point2D& newPos)
{
    switch (index) {
    case 0: m_firstPoint = newPos; return true;
    case 1: m_secondPoint = newPos; return true;
    case 2: m_dimLinePoint = newPos; return true;
    default: return false;
    }
}

std::vector<Point2D> DimensionEntity::flatten(double sagitta) const
{
    Q_UNUSED(sagitta);

    // 折线化的骨架：界线 -> 尺寸线 -> 界线（或 引线路径），
    // 用于框选相交判定与移动预览
    const Geometry geo = geometry();
    if (!geo.valid) return {};

    std::vector<Point2D> pts;
    if (geo.segments.empty()) return pts;

    pts.push_back(geo.segments.front().first);
    for (const auto& seg : geo.segments) {
        pts.push_back(seg.second);
    }
    return pts;
}

QString DimensionEntity::describe() const
{
    static const char* kNames[] = { "线性", "对齐", "直径", "半径" };
    const char* name = "标注";
    switch (m_dimType) {
    case DimType::Linear:    name = kNames[0]; break;
    case DimType::Aligned:   name = kNames[1]; break;
    case DimType::Diametric: name = kNames[2]; break;
    case DimType::Radial:    name = kNames[3]; break;
    }
    return QStringLiteral("标注  %1  %2")
        .arg(QString::fromLatin1(name), displayText());
}

QString DimensionEntity::geometrySummary() const
{
    static const char* kNames[] = { "线性", "对齐", "直径", "半径" };
    const char* name = "标注";
    switch (m_dimType) {
    case DimType::Linear:    name = kNames[0]; break;
    case DimType::Aligned:   name = kNames[1]; break;
    case DimType::Diametric: name = kNames[2]; break;
    case DimType::Radial:    name = kNames[3]; break;
    }

    const Geometry geo = geometry();
    QString text = QStringLiteral("类型: %1\n测量值: %2\n文字: %3\n字高: %4")
                       .arg(QString::fromLatin1(name),
                            formatLength(measurement()),
                            displayText(),
                            formatLength(m_textHeight));
    if (m_dimType == DimType::Linear) {
        text += QStringLiteral("\n尺寸线角度: %1").arg(formatAngle(m_rotation));
    }
    text += QStringLiteral("\n文字位置: %1").arg(formatPoint(geo.textAnchor));
    return text;
}

} // namespace lycad
