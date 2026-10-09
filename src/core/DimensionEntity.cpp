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
/// 坐标标注：界线起点与被测点之间的间隙（单位：字高）
constexpr double kOrdinateGap = 0.25;
/// 坐标标注：折点相对引线端点的回退量（单位：字高）
constexpr double kOrdinateLanding = 2.0;
/// 坐标标注：落地小段的长度（单位：字高）
constexpr double kOrdinateTick = 8.25;
/// 弧长标注：弧长符号（尺寸弧上的小半圆）半径（单位：字高）
constexpr double kArcSymbolRadius = 0.933;

/// 估算显示文字宽度（单位：字高）。
/// 与 TextEntity 的字宽估算保持同一比例（渲染 em = h/0.72），
/// 略宽松以保证包围盒完整覆盖测量文字。
double textWidthFactor(const QString& text)
{
    double factor = 0.0;
    for (const QChar ch : text) {
        const ushort code = ch.unicode();
        if (code >= 0x2E80) factor += 1.45;             // CJK / 全角
        else if (code == U'.') factor += 0.45;
        else factor += 0.85;
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

/// 标注类型的中文名（与 DXF 70 组码的低 3 位一致）。
/// 名字一律走 QStringLiteral：这里曾用 const char* + QString::fromLatin1，
/// UTF-8 字节被当成 Latin-1 解码，「线性」在属性面板里显示成「çº¿æ§」。
QString dimTypeDisplayName(DimensionEntity::DimType type)
{
    switch (type) {
    case DimensionEntity::DimType::Linear:    return QStringLiteral("线性");
    case DimensionEntity::DimType::Aligned:   return QStringLiteral("对齐");
    case DimensionEntity::DimType::Diametric: return QStringLiteral("直径");
    case DimensionEntity::DimType::Radial:    return QStringLiteral("半径");
    case DimensionEntity::DimType::Ordinate:  return QStringLiteral("坐标");
    case DimensionEntity::DimType::ArcLength: return QStringLiteral("弧长");
    }
    return QStringLiteral("标注");
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
    case DimType::Ordinate: {
        // 坐标标注量的是**被测点在某一轴上的坐标**（相对基准原点 10），
        // 不是两点距离——这就是它与线性标注的本质区别。
        const Point2D base = m_dimLinePoint;
        return m_ordinateX ? (m_firstPoint.x - base.x) : (m_firstPoint.y - base.y);
    }
    case DimType::ArcLength: {
        const double radius = measuredArcRadius();
        return radius * angleSpanCcw(m_arcStartAngle, m_arcEndAngle);
    }
    }
    return 0.0;
}

QString DimensionEntity::autoText() const
{
    switch (m_dimType) {
    case DimType::Radial:    return QStringLiteral("R%1").arg(formatLength(measurement()));
    case DimType::Diametric: return QStringLiteral("%1%2").arg(QChar(0x00D8), formatLength(measurement()));
    // 坐标标注写的就是坐标值本身（formatLength 保留负号与小数位）
    case DimType::Ordinate:  return formatLength(measurement());
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

    case DimType::Ordinate: {
        // 坐标标注的形状是一个「L」：沿测量轴从被测点拉出的界线、一段落地线、
        // 以及端点处的短竖线。**没有箭头**——AutoCAD 的坐标标注就是不带箭头的。
        // 尺寸比例（间隙/回退/落地段）实测自一张 AutoCAD 图纸的匿名几何块
        //（*D6：0.25h / 2h / 8.25h，h 为字高），与该块里的三条 LINE 逐点吻合。
        const Point2D feature = m_firstPoint;
        const Point2D leader = m_secondPoint;

        const Vector2D axis = m_ordinateX ? Vector2D(1.0, 0.0) : Vector2D(0.0, 1.0);
        const Vector2D perp = m_ordinateX ? Vector2D(0.0, 1.0) : Vector2D(1.0, 0.0);

        const double dv = dot(leader - feature, perp);
        const double du = dot(leader - feature, axis);
        const double side = (dv >= 0.0) ? 1.0 : -1.0;   // 引线朝哪一侧拉开

        const double gap = kOrdinateGap * m_textHeight;
        const double landing = kOrdinateLanding * m_textHeight;
        const double tick = kOrdinateTick * m_textHeight;

        const Point2D start = feature + perp * (side * gap);
        const Point2D elbow = feature + perp * (dv - side * landing);
        const Point2D landEnd = elbow + axis * du;
        const Point2D tickEnd = landEnd + perp * (side * tick);

        geo.segments.emplace_back(start, elbow);
        geo.segments.emplace_back(elbow, landEnd);
        geo.segments.emplace_back(landEnd, tickEnd);

        // 文字贴在落地小段旁；文件给了 11 组码就用它，否则取小段中点
        geo.textAngle = 0.0;
        geo.textAnchor = m_textPoint.value_or(
            Point2D((landEnd.x + tickEnd.x) * 0.5, (landEnd.y + tickEnd.y) * 0.5));
        break;
    }

    case DimType::ArcLength: {
        // 弧长标注：一段与被测弧同心的**尺寸弧**，两端各有界线连回被测弧，
        // 弧端各一个沿切线方向的箭头，弧的中点处放弧长符号与测量文字。
        //
        // 尺寸参数实测自图纸的匿名几何块（*D4）：
        //   · 尺寸弧半径 = |弧心 - 10 组码定义点|（不是被测弧半径）
        //   · 尺寸弧两端各内缩「一个箭头长」对应的圆心角（箭头要占位置）
        //   · 界线是径向的，从被测弧内侧一点点画到尺寸弧内侧
        const Point2D center = m_arcCenter;
        const double rMeasured = measuredArcRadius();
        const double rDim = center.distanceTo(m_dimLinePoint);

        if (rDim <= kGeomTol || rMeasured <= kGeomTol) {
            geo.valid = false;
            return geo;
        }

        const double span = angleSpanCcw(m_arcStartAngle, m_arcEndAngle);
        const double inset = std::min(arrowLen / rDim, span * 0.4);
        const double arcStart = m_arcStartAngle + inset;
        const double arcEnd = m_arcEndAngle - inset;
        const double arcSpan = angleSpanCcw(arcStart, arcEnd);

        // 尺寸弧离散成线段：留 0.2% 的矢高余量，屏幕上已看不出折线
        const int steps = std::max(8, segmentCountForArc(rDim, arcSpan, rDim * 0.002));
        for (int i = 0; i < steps; ++i) {
            const double a0 = arcStart + arcSpan * (static_cast<double>(i) / steps);
            const double a1 = arcStart + arcSpan * (static_cast<double>(i + 1) / steps);
            geo.segments.emplace_back(center + Vector2D(std::cos(a0), std::sin(a0)) * rDim,
                                      center + Vector2D(std::cos(a1), std::sin(a1)) * rDim);
        }

        // 两端界线：径向，留出与被测弧的间隙，并让开尺寸弧上的箭头
        const double endGap = kExtensionGap * m_textHeight;
        auto addRadial = [&](double angle) {
            const Vector2D dir(std::cos(angle), std::sin(angle));
            const Point2D from = center + dir * (rMeasured - endGap);
            const Point2D to = center + dir * (rDim - arrowLen * 0.5);
            if (from.distanceTo(to) > kGeomTol) geo.segments.emplace_back(from, to);
        };
        addRadial(m_arcStartAngle);
        addRadial(m_arcEndAngle);

        // 箭头沿切线，两个都指向弧的中段（与实测的实心三角形方向一致）
        const Vector2D tangentStart(-std::sin(arcStart), std::cos(arcStart));
        const Vector2D tangentEnd(-std::sin(arcEnd), std::cos(arcEnd));
        addArrow(center + Vector2D(std::cos(arcStart), std::sin(arcStart)) * rDim, -tangentStart);
        addArrow(center + Vector2D(std::cos(arcEnd), std::sin(arcEnd)) * rDim, tangentEnd);

        // 文字方向取尺寸弧中点的切线方向（实测：MTEXT 的 11 组码正是这个方向）
        const double midAngle = normalizeAngle(arcStart + arcSpan * 0.5);
        geo.textAngle = readableAngle(midAngle + kHalfPi);
        geo.textAnchor = m_textPoint.value_or(center + Vector2D(std::cos(midAngle), std::sin(midAngle))
                                                          * (rDim + m_textHeight * 0.8));

        // 弧长符号：尺寸弧中点上的一个小半圆，朝弧内侧张开
        const Point2D symbolCenter = center + Vector2D(std::cos(midAngle), std::sin(midAngle)) * rDim;
        const double symbolRadius = kArcSymbolRadius * m_textHeight;
        constexpr int kSymbolSteps = 12;
        for (int i = 0; i < kSymbolSteps; ++i) {
            const double a0 = midAngle + kHalfPi + kPi * (static_cast<double>(i) / kSymbolSteps);
            const double a1 = midAngle + kHalfPi + kPi * (static_cast<double>(i + 1) / kSymbolSteps);
            geo.segments.emplace_back(symbolCenter + Vector2D(std::cos(a0), std::sin(a0)) * symbolRadius,
                                      symbolCenter + Vector2D(std::cos(a1), std::sin(a1)) * symbolRadius);
        }
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
    copy->m_ordinateX     = m_ordinateX;
    copy->m_arcCenter     = m_arcCenter;
    copy->m_arcStartAngle = m_arcStartAngle;
    copy->m_arcEndAngle   = m_arcEndAngle;
    copy->m_textPoint     = m_textPoint;
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
    m_arcCenter = t.apply(m_arcCenter);
    if (m_textPoint) m_textPoint = t.apply(*m_textPoint);

    if (!t.isSimilarity()) return;

    m_textHeight *= t.uniformScale();
    if (m_dimType == DimType::Linear) {
        m_rotation = normalizeAngle(m_rotation + t.rotationAngle());
    }
    if (m_dimType == DimType::ArcLength) {
        // 弧参数要跟着方向走。直接加旋转角只在「旋转 + 等比缩放」下成立；
        // 镜像是唯一会反转绕行方向的变换，此时起止参数必须互换，否则尺寸弧
        // 会从另一侧绕过去。
        const Vector2D startDir(std::cos(m_arcStartAngle), std::sin(m_arcStartAngle));
        const Vector2D endDir(std::cos(m_arcEndAngle), std::sin(m_arcEndAngle));
        const double newStart = t.applyVector(startDir).angle();
        const double newEnd = t.applyVector(endDir).angle();
        if (t.isMirroring()) {
            m_arcStartAngle = normalizeAngle(newEnd);
            m_arcEndAngle = normalizeAngle(newStart);
        } else {
            m_arcStartAngle = normalizeAngle(newStart);
            m_arcEndAngle = normalizeAngle(newEnd);
        }
    }
}

double DimensionEntity::length() const
{
    return 0.0;
}

std::vector<Point2D> DimensionEntity::snapPoints() const
{
    std::vector<Point2D> points { m_firstPoint, m_secondPoint, m_dimLinePoint };
    if (m_textPoint) points.push_back(*m_textPoint);
    return points;
}

std::vector<Point2D> DimensionEntity::gripPoints() const
{
    return snapPoints();
}

bool DimensionEntity::moveGrip(std::size_t index, const Point2D& newPos)
{
    switch (index) {
    case 0: m_firstPoint = newPos; return true;
    case 1: m_secondPoint = newPos; return true;
    case 2: m_dimLinePoint = newPos; return true;
    case 3:
        if (!m_textPoint) return false;
        m_textPoint = newPos;
        return true;
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
    return QStringLiteral("标注  %1  %2")
        .arg(dimTypeDisplayName(m_dimType), displayText());
}

QString DimensionEntity::geometrySummary() const
{
    const Geometry geo = geometry();
    QString text = QStringLiteral("类型: %1\n测量值: %2\n文字: %3\n字高: %4")
                       .arg(dimTypeDisplayName(m_dimType),
                            formatLength(measurement()),
                            displayText(),
                            formatLength(m_textHeight));
    if (m_dimType == DimType::Linear) {
        text += QStringLiteral("\n尺寸线角度: %1").arg(formatAngle(m_rotation));
    }
    if (m_dimType == DimType::Ordinate) {
        text += QStringLiteral("\n度量轴: %1").arg(m_ordinateX ? QStringLiteral("X") : QStringLiteral("Y"));
    }
    if (m_dimType == DimType::ArcLength) {
        text += QStringLiteral("\n弧心: %1\n被测弧半径: %2\n包角: %3")
                    .arg(formatPoint(m_arcCenter),
                         formatLength(measuredArcRadius()),
                         formatAngle(angleSpanCcw(m_arcStartAngle, m_arcEndAngle)));
    }
    text += QStringLiteral("\n文字位置: %1").arg(formatPoint(geo.textAnchor));
    return text;
}

} // namespace lycad
