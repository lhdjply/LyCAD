// =============================================================================
//  LyCAD - core/PolylineEntity.cpp
// =============================================================================
#include "core/PolylineEntity.h"

#include "core/Format.h"

#include <QStringList>

#include <array>

namespace lycad {

namespace {

constexpr std::array<double, 4> kQuadrantAngles = { 0.0, kHalfPi, kPi, 3.0 * kHalfPi };

/// 把一段圆弧的极值点并入包围盒
void expandBoxWithArc(Box2D& box, const PolylineSegment& seg)
{
    box.expand(seg.start);
    box.expand(seg.end);
    for (double q : kQuadrantAngles) {
        // 必须按凸度符号判定方向，否则顺时针弧段的象限点会被错误地算成一整圈
        if (seg.containsAngle(q)) {
            const Point2D p(seg.center.x + seg.radius * std::cos(q),
                            seg.center.y + seg.radius * std::sin(q));
            box.expand(p);
        }
    }
}

} // namespace

// -----------------------------------------------------------------------------
//  PolylineSegment
// -----------------------------------------------------------------------------
double PolylineSegment::spanAngle() const
{
    if (!isArc) return 0.0;
    // 逆时针圆弧（正凸度）：跨度就是 start→end 的逆时针角。
    // 顺时针圆弧（负凸度）：真实跨度是它的**补角**——start→end 的逆时针角此时
    // 是「绕远路」的那一段（例如顺时针 90° 量出来是 270°）。
    // 漏掉这一步会让顺时针圆弧按补弧绘制，也就是常见的「圆弧反了」。
    const double counterClockwise = normalizeAngle(endAngle - startAngle);
    const double span = (bulge >= 0.0) ? counterClockwise : (kTwoPi - counterClockwise);
    return span <= kEpsilon ? kTwoPi : span;
}

double PolylineSegment::length() const
{
    if (!isArc) return start.distanceTo(end);
    // 含角绝对值 = |4·atan(bulge)|
    const double theta = std::abs(4.0 * std::atan(bulge));
    return radius * theta;
}

bool PolylineSegment::containsAngle(double radians) const
{
    if (!isArc) return false;

    const double span = spanAngle();
    if (bulge >= 0.0) {
        // 逆时针：自起始角正向度量
        return normalizeAngle(radians - startAngle) <= span + kEpsilon;
    }
    // 顺时针：自起始角反向度量
    return normalizeAngle(startAngle - radians) <= span + kEpsilon;
}

Point2D PolylineSegment::middlePoint() const
{
    if (!isArc) {
        return Point2D((start.x + end.x) * 0.5, (start.y + end.y) * 0.5);
    }
    const double half = spanAngle() * 0.5;
    const double a = (bulge >= 0.0) ? (startAngle + half) : (startAngle - half);
    return Point2D(center.x + radius * std::cos(a), center.y + radius * std::sin(a));
}

Point2D PolylineSegment::closestPointTo(const Point2D& p) const
{
    if (!isArc) return closestPointOnSegment(p, start, end);

    if (containsAngle(center.angleTo(p))) {
        const Vector2D dir = p - center;
        return center + dir.normalizedOr(Vector2D(1.0, 0.0)) * radius;
    }
    return p.distanceTo(start) <= p.distanceTo(end) ? start : end;
}

// -----------------------------------------------------------------------------
//  由两顶点构造段几何
// -----------------------------------------------------------------------------
namespace {

PolylineSegment buildSegment(const PolylineVertex& a, const PolylineVertex& b)
{
    return PolylineEntity::makeSegment(a.point, b.point, a.bulge);
}

} // namespace

// -----------------------------------------------------------------------------
//  PolylineEntity
// -----------------------------------------------------------------------------
PolylineEntity::PolylineEntity()
    : Entity(EntityType::Polyline)
{
}

PolylineEntity::PolylineEntity(const std::vector<Point2D>& points, bool closed)
    : Entity(EntityType::Polyline), m_closed(closed)
{
    m_vertices.reserve(points.size());
    for (const Point2D& p : points) {
        m_vertices.push_back(Vertex{ p, 0.0 });
    }
}

PolylineEntity::PolylineEntity(std::vector<Vertex> vertices, bool closed)
    : Entity(EntityType::Polyline), m_vertices(std::move(vertices)), m_closed(closed)
{
}

PolylineEntity PolylineEntity::makeRectangle(const Point2D& corner1, const Point2D& corner2)
{
    PolylineEntity pl;
    pl.m_vertices = {
        Vertex{ Point2D(corner1.x, corner1.y), 0.0 },
        Vertex{ Point2D(corner2.x, corner1.y), 0.0 },
        Vertex{ Point2D(corner2.x, corner2.y), 0.0 },
        Vertex{ Point2D(corner1.x, corner2.y), 0.0 },
    };
    pl.m_closed = true;
    return pl;
}

PolylineSegment PolylineEntity::makeSegment(const Point2D& start, const Point2D& end,
                                            double bulge)
{
    PolylineSegment seg;
    seg.start = start;
    seg.end = end;
    seg.bulge = bulge;            // bulge 归属于段的起始顶点

    const double chord = seg.start.distanceTo(seg.end);
    if (std::abs(bulge) <= kEpsilon || chord <= kGeomTol) {
        return seg;               // 直线段（或退化为点）
    }

    const double theta = 4.0 * std::atan(bulge);        // 含角，带符号
    const double sinHalf = std::sin(std::abs(theta) * 0.5);
    if (sinHalf <= kEpsilon) {
        return seg;               // 数值上退化为直线
    }

    seg.radius = chord / (2.0 * sinHalf);

    // 圆心到弦中点的有向距离 h = (L/2)·cot(θ/2)，沿弦的左法向度量。
    // 符号由 θ 的符号定：θ>0（逆时针）时圆心在弦左侧，θ<0（顺时针）时在右侧。
    //
    // 这里**不能**写成 R·cos(θ/2)：半径是按 |sin| 算的，而 cos 是偶函数，
    // 于是负 θ 会得到相反的符号——圆心跑到弦的另一侧，画出来正好是正确圆弧的
    // 补弧（顺时针 90° 的圆角变成 270° 的大弧，看上去就是「圆弧反了」）。
    // 半圆（|θ|=180°）与逆时针圆弧不受影响，所以这种错只在一部分圆弧上露头。
    const double h = (chord * 0.5) / std::tan(theta * 0.5);
    const Vector2D dir = (seg.end - seg.start) / chord;
    const Vector2D normal = dir.perpendicular();
    const Point2D mid((seg.start.x + seg.end.x) * 0.5, (seg.start.y + seg.end.y) * 0.5);
    seg.center = mid + normal * h;

    seg.startAngle = seg.center.angleTo(seg.start);
    seg.endAngle = seg.center.angleTo(seg.end);
    seg.isArc = true;
    return seg;
}

void PolylineEntity::setVertices(std::vector<Vertex> vertices)
{
    m_vertices = std::move(vertices);
}

void PolylineEntity::appendVertex(const Point2D& p, double bulge)
{
    m_vertices.push_back(Vertex{ p, bulge });
}

void PolylineEntity::insertVertex(std::size_t index, const Vertex& v)
{
    if (index > m_vertices.size()) index = m_vertices.size();
    m_vertices.insert(m_vertices.begin() + static_cast<std::ptrdiff_t>(index), v);
}

void PolylineEntity::removeVertex(std::size_t index)
{
    if (index >= m_vertices.size()) return;
    m_vertices.erase(m_vertices.begin() + static_cast<std::ptrdiff_t>(index));
}

void PolylineEntity::setVertexPosition(std::size_t index, const Point2D& p)
{
    if (index >= m_vertices.size()) return;
    m_vertices[index].point = p;
}

void PolylineEntity::setVertexBulge(std::size_t index, double bulge)
{
    if (index >= m_vertices.size()) return;
    m_vertices[index].bulge = bulge;
}

void PolylineEntity::setVertexWidths(std::size_t index, double startWidth, double endWidth)
{
    if (index >= m_vertices.size()) return;
    m_vertices[index].startWidth = std::max(startWidth, 0.0);
    m_vertices[index].endWidth = std::max(endWidth, 0.0);
}

void PolylineEntity::clear() noexcept
{
    m_vertices.clear();
}

std::vector<Point2D> PolylineEntity::points() const
{
    std::vector<Point2D> pts;
    pts.reserve(m_vertices.size());
    for (const Vertex& v : m_vertices) pts.push_back(v.point);
    return pts;
}

std::size_t PolylineEntity::segmentCount() const noexcept
{
    const std::size_t n = m_vertices.size();
    if (n < 2) return 0;
    return m_closed ? n : n - 1;
}

std::optional<PolylineSegment> PolylineEntity::segment(std::size_t index) const
{
    const std::size_t count = segmentCount();
    if (index >= count) return std::nullopt;
    const std::size_t i0 = index;
    const std::size_t i1 = (index + 1) % m_vertices.size();
    return buildSegment(m_vertices[i0], m_vertices[i1]);
}

std::vector<PolylineSegment> PolylineEntity::segments() const
{
    std::vector<PolylineSegment> out;
    const std::size_t count = segmentCount();
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (auto s = segment(i)) out.push_back(*s);
    }
    return out;
}

double PolylineEntity::perimeter() const
{
    double total = 0.0;
    const std::size_t count = segmentCount();
    for (std::size_t i = 0; i < count; ++i) {
        if (auto s = segment(i)) total += s->length();
    }
    return total;
}

std::unique_ptr<Entity> PolylineEntity::clone() const
{
    auto copy = std::make_unique<PolylineEntity>(m_vertices, m_closed);
    copy->m_constantWidth = m_constantWidth;
    copyCommonPropertiesTo(*copy);
    return copy;
}

double PolylineEntity::maxWidth() const noexcept
{
    double widest = m_constantWidth;
    for (const Vertex& v : m_vertices) {
        widest = std::max(widest, std::max(v.startWidth, v.endWidth));
    }
    return widest;
}

double PolylineEntity::segmentWidth(std::size_t index) const noexcept
{
    if (m_constantWidth > kGeomTol) return m_constantWidth;
    if (index >= segmentCount()) return 0.0;

    const Vertex& a = m_vertices[index];
    const Vertex& b = m_vertices[(index + 1) % m_vertices.size()];
    return std::max({a.startWidth, a.endWidth, b.startWidth, b.endWidth});
}

bool PolylineEntity::hasUniformWidth() const noexcept
{
    if (m_constantWidth > kGeomTol) return true;

    // 逐顶点宽度均匀的严格定义：每个顶点的起止宽度相等，且全线等宽——
    // 否则锥形走线需要逐段变宽描边，无法用单一画笔表达
    double first = -1.0;
    for (const Vertex& v : m_vertices) {
        if (std::abs(v.startWidth - v.endWidth) > kGeomTol) return false;
        if (first < 0.0) {
            first = v.startWidth;
        } else if (std::abs(v.startWidth - first) > kGeomTol) {
            return false;
        }
    }
    return true;
}

Box2D PolylineEntity::boundingBox() const
{
    Box2D box;
    for (const Vertex& v : m_vertices) box.expand(v.point);

    // 圆弧段的极值可能越出顶点范围，需把象限点并入
    const std::size_t count = segmentCount();
    for (std::size_t i = 0; i < count; ++i) {
        if (auto s = segment(i); s && s->isArc) {
            expandBoxWithArc(box, *s);
        }
    }

    // 宽度是真实几何：包围盒必须按最大半宽外扩，否则粗线在
    // 视图裁剪与范围缩放下会被边缘裁掉一段
    if (const double half = maxWidth() * 0.5; half > kGeomTol) {
        box.expand(half);
    }
    return box;
}

double PolylineEntity::distanceTo(const Point2D& p) const
{
    if (m_vertices.empty()) return std::numeric_limits<double>::max();
    if (m_vertices.size() == 1) return p.distanceTo(m_vertices.front().point);

    double best = std::numeric_limits<double>::max();
    const std::size_t count = segmentCount();
    for (std::size_t i = 0; i < count; ++i) {
        if (auto s = segment(i)) {
            best = std::min(best, p.distanceTo(s->closestPointTo(p)));
        }
    }

    // 宽度属于几何：落在粗线内部即视为命中
    if (const double half = maxWidth() * 0.5; half > kGeomTol) {
        best = std::max(0.0, best - half);
    }
    return best;
}

Point2D PolylineEntity::closestPointTo(const Point2D& p) const
{
    if (m_vertices.empty()) return p;
    if (m_vertices.size() == 1) return m_vertices.front().point;

    Point2D best = m_vertices.front().point;
    double bestDist = std::numeric_limits<double>::max();
    const std::size_t count = segmentCount();
    for (std::size_t i = 0; i < count; ++i) {
        auto s = segment(i);
        if (!s) continue;
        const Point2D c = s->closestPointTo(p);
        const double d = c.distanceTo(p);
        if (d < bestDist) { bestDist = d; best = c; }
    }
    return best;
}

bool PolylineEntity::crosses(const Box2D& box) const
{
    if (!box.isValid() || m_vertices.size() < 2) {
        return !m_vertices.empty() && box.contains(m_vertices.front().point, 1e-9);
    }
    return detail::pointsCrossBox(flatten(detail::sagittaForBox(box)), m_closed, box);
}

void PolylineEntity::transform(const Transform2D& t)
{
    for (Vertex& v : m_vertices) v.point = t.apply(v.point);

    // 镜像会反转弧段的绕行方向，凸度符号须同步取反，
    // 否则圆弧会翻到弦的另一侧。
    if (t.isMirroring()) {
        for (Vertex& v : m_vertices) v.bulge = -v.bulge;
    }

    // 宽度是几何量：随等比缩放同步缩放。非等比变换下宽度方向歧义，
    // 退化为按均匀缩放系数近似
    if (const double s = t.uniformScale(); std::abs(s - 1.0) > 1e-12) {
        m_constantWidth *= s;
        for (Vertex& v : m_vertices) {
            v.startWidth *= s;
            v.endWidth *= s;
        }
    }
}

double PolylineEntity::length() const
{
    return perimeter();
}

std::vector<Point2D> PolylineEntity::snapPoints() const
{
    std::vector<Point2D> pts;
    pts.reserve(m_vertices.size() * 2);
    for (const Vertex& v : m_vertices) pts.push_back(v.point);

    const std::size_t count = segmentCount();
    for (std::size_t i = 0; i < count; ++i) {
        if (auto s = segment(i)) pts.push_back(s->middlePoint());
    }
    return pts;
}

std::vector<Point2D> PolylineEntity::gripPoints() const
{
    std::vector<Point2D> pts;
    pts.reserve(m_vertices.size());
    for (const Vertex& v : m_vertices) pts.push_back(v.point);
    return pts;
}

bool PolylineEntity::moveGrip(std::size_t index, const Point2D& newPos)
{
    if (index >= m_vertices.size()) return false;
    m_vertices[index].point = newPos;
    return true;
}

std::vector<Point2D> PolylineEntity::flatten(double sagitta) const
{
    std::vector<Point2D> pts;
    const std::size_t count = segmentCount();
    if (count == 0) {
        // 单个顶点：退化为一个点，保证拾取与显示仍可用
        if (!m_vertices.empty()) pts.push_back(m_vertices.front().point);
        return pts;
    }

    for (std::size_t i = 0; i < count; ++i) {
        const auto seg = segment(i);
        if (!seg) continue;

        const bool firstSegment = (i == 0);
        if (!seg->isArc) {
            if (firstSegment) pts.push_back(seg->start);
            pts.push_back(seg->end);
            continue;
        }

        // 圆弧段：自**起始角**出发，按 bulge 的符号决定推进方向
        // （正=逆时针，负=顺时针）。不能写成「交换起止角再逆时针走」——
        // 那样点序会从终点走回起点，多段线的链条在段与段之间就断了。
        const double span = seg->spanAngle();
        const double direction = (seg->bulge >= 0.0) ? 1.0 : -1.0;
        const int n = segmentCountForArc(seg->radius, span, sagitta);
        const double step = span / n;
        const int beginIdx = firstSegment ? 0 : 1;   // 避免与上一段末点重复
        for (int k = beginIdx; k <= n; ++k) {
            const double a = seg->startAngle + direction * step * k;
            pts.push_back(Point2D(seg->center.x + seg->radius * std::cos(a),
                                  seg->center.y + seg->radius * std::sin(a)));
        }
    }

    // 闭合多段线的末点与首点重合，去重以形成标准闭合环
    if (m_closed && pts.size() > 1 && pts.front().distanceTo(pts.back()) <= kGeomTol) {
        pts.pop_back();
    }
    return pts;
}

QString PolylineEntity::describe() const
{
    QString text = QStringLiteral("多段线  %1 个顶点 %2  长度 %3")
        .arg(m_vertices.size())
        .arg(m_closed ? QStringLiteral("(闭合)") : QString())
        .arg(formatLength(length()));
    if (hasWidth()) {
        text += QStringLiteral("  宽度 %1").arg(formatLength(maxWidth()));
    }
    return text;
}

QString PolylineEntity::geometrySummary() const
{
    QStringList lines;
    lines << QStringLiteral("顶点数: %1").arg(m_vertices.size());
    lines << QStringLiteral("闭合: %1").arg(m_closed ? QStringLiteral("是") : QStringLiteral("否"));
    lines << QStringLiteral("总长: %1").arg(formatLength(perimeter()));
    lines << QStringLiteral("宽度: %1")
                 .arg(hasWidth() ? formatLength(maxWidth()) : QStringLiteral("无"));

    int arcSegments = 0;
    const auto segs = segments();
    for (const auto& s : segs) {
        if (s.isArc) ++arcSegments;
    }
    lines << QStringLiteral("圆弧段: %1 / %2").arg(arcSegments).arg(segs.size());

    // 顶点坐标明细，最多列出前 20 个，避免面板过长
    const std::size_t limit = std::min<std::size_t>(m_vertices.size(), 20);
    for (std::size_t i = 0; i < limit; ++i) {
        lines << QStringLiteral("  V%1: %2").arg(i).arg(formatPoint(m_vertices[i].point));
    }
    if (m_vertices.size() > limit) {
        lines << QStringLiteral("  ... 其余 %1 个顶点省略").arg(m_vertices.size() - limit);
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace lycad
