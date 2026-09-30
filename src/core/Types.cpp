// =============================================================================
//  LyCAD - core/Types.cpp
// =============================================================================
#include "core/Types.h"

namespace lycad {

double degToRad(double deg) { return deg * kPi / 180.0; }
double radToDeg(double rad) { return rad * 180.0 / kPi; }

double normalizeAngle(double a)
{
    if (!std::isfinite(a)) return 0.0;
    double r = std::fmod(a, kTwoPi);
    if (r < 0.0) r += kTwoPi;
    // fmod 可能因舍入返回恰好 2π
    if (r >= kTwoPi) r = 0.0;
    return r;
}

double angleSpanCcw(double start, double end)
{
    double span = normalizeAngle(end - start);
    // 跨度 0 视为整圈（起止同位）——由调用方按需解释
    return span;
}

bool angleWithin(double a, double start, double end, bool ccw)
{
    const double span = normalizeAngle(end - start);
    const double delta = normalizeAngle(a - start);
    if (ccw) {
        if (span <= kEpsilon) return delta <= kEpsilon || nearlyEqual(delta, kTwoPi);
        return delta <= span + kEpsilon;
    }
    // 顺时针：自 start 递减至 end
    const double cwSpan = kTwoPi - span;
    if (cwSpan <= kEpsilon) return delta <= kEpsilon;
    const double cwDelta = normalizeAngle(start - a);
    return cwDelta <= cwSpan + kEpsilon;
}

bool nearlyEqual(double a, double b, double tol)
{
    const double diff = std::abs(a - b);
    if (diff <= tol) return true;
    const double scale = std::max(std::abs(a), std::abs(b));
    return diff <= tol * scale;
}

double safeDiv(double numerator, double denominator, double fallback)
{
    if (std::abs(denominator) <= kEpsilon) return fallback;
    return numerator / denominator;
}

// -----------------------------------------------------------------------------
//  Vector2D
// -----------------------------------------------------------------------------
Vector2D Vector2D::normalized() const
{
    const double len = length();
    if (len <= kEpsilon) return Vector2D();
    return Vector2D(x / len, y / len);
}

Vector2D Vector2D::normalizedOr(const Vector2D& fallback) const
{
    const double len = length();
    if (len <= kEpsilon) return fallback;
    return Vector2D(x / len, y / len);
}

Vector2D Vector2D::rotated(double rad) const noexcept
{
    const double cs = std::cos(rad);
    const double sn = std::sin(rad);
    return Vector2D(x * cs - y * sn, x * sn + y * cs);
}

// -----------------------------------------------------------------------------
//  Point2D
// -----------------------------------------------------------------------------
Point2D Point2D::rotatedAround(const Point2D& c, double rad) const noexcept
{
    const double dx = x - c.x;
    const double dy = y - c.y;
    const double cs = std::cos(rad);
    const double sn = std::sin(rad);
    return Point2D(c.x + dx * cs - dy * sn, c.y + dx * sn + dy * cs);
}

double Point2D::distanceSquaredTo(const Point2D& o) const noexcept
{
    const double dx = x - o.x;
    const double dy = y - o.y;
    return dx * dx + dy * dy;
}

// -----------------------------------------------------------------------------
//  Box2D
// -----------------------------------------------------------------------------
void Box2D::expand(const Point2D& p) noexcept
{
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) return;
    if (!valid) {
        minX = maxX = p.x;
        minY = maxY = p.y;
        valid = true;
        return;
    }
    minX = std::min(minX, p.x);
    minY = std::min(minY, p.y);
    maxX = std::max(maxX, p.x);
    maxY = std::max(maxY, p.y);
}

void Box2D::expand(const Box2D& b) noexcept
{
    if (!b.valid) return;
    expand(Point2D(b.minX, b.minY));
    expand(Point2D(b.maxX, b.maxY));
}

void Box2D::expand(double margin) noexcept
{
    if (!valid) return;
    minX -= margin;
    minY -= margin;
    maxX += margin;
    maxY += margin;
}

bool Box2D::contains(const Point2D& p, double tol) const noexcept
{
    if (!valid) return false;
    return p.x >= minX - tol && p.x <= maxX + tol
        && p.y >= minY - tol && p.y <= maxY + tol;
}

bool Box2D::contains(const Box2D& b, double tol) const noexcept
{
    if (!valid || !b.valid) return false;
    return b.minX >= minX - tol && b.maxX <= maxX + tol
        && b.minY >= minY - tol && b.maxY <= maxY + tol;
}

bool Box2D::intersects(const Box2D& b) const noexcept
{
    if (!valid || !b.valid) return false;
    if (maxX < b.minX || b.maxX < minX) return false;
    if (maxY < b.minY || b.maxY < minY) return false;
    return true;
}

std::optional<Box2D> Box2D::intersected(const Box2D& b) const noexcept
{
    if (!intersects(b)) return std::nullopt;
    return Box2D(std::max(minX, b.minX), std::max(minY, b.minY),
                 std::min(maxX, b.maxX), std::min(maxY, b.maxY));
}

Box2D Box2D::inflatedBy(double ratio) const
{
    if (!valid) return *this;
    const double mx = width() * ratio;
    const double my = height() * ratio;
    // 对退化盒给一个最小余量，避免缩放后仍不可见
    const double minMargin = 1e-6;
    const double px = std::max(mx, minMargin);
    const double py = std::max(my, minMargin);
    Box2D out = *this;
    out.minX -= px;
    out.maxX += px;
    out.minY -= py;
    out.maxY += py;
    return out;
}

// -----------------------------------------------------------------------------
//  Transform2D
// -----------------------------------------------------------------------------
Transform2D Transform2D::rotation(double rad) noexcept
{
    const double cs = std::cos(rad);
    const double sn = std::sin(rad);
    return Transform2D(cs, sn, -sn, cs, 0.0, 0.0);
}

Transform2D Transform2D::rotationAround(const Point2D& c, double rad) noexcept
{
    // 先平移到原点，旋转，再平移回去
    const double cs = std::cos(rad);
    const double sn = std::sin(rad);
    return Transform2D(cs, sn, -sn, cs,
                       c.x - cs * c.x + sn * c.y,
                       c.y - sn * c.x - cs * c.y);
}

Transform2D Transform2D::scalingAround(const Point2D& c, double sx, double sy) noexcept
{
    return Transform2D(sx, 0.0, 0.0, sy,
                       c.x * (1.0 - sx),
                       c.y * (1.0 - sy));
}

Transform2D Transform2D::mirror(const Point2D& p1, const Point2D& p2) noexcept
{
    const Vector2D axis = p2 - p1;
    const double len = axis.length();
    if (len <= kEpsilon) return identity();

    const double ux = axis.x / len;
    const double uy = axis.y / len;

    // 关于过原点、方向 u 的直线镜像： [[ux²-uy², 2uxuy], [2uxuy, uy²-ux²]]
    const double m00 = ux * ux - uy * uy;
    const double m01 = 2.0 * ux * uy;
    const double m10 = m01;
    const double m11 = uy * uy - ux * ux;

    // 平移到 p1 处执行
    return Transform2D(m00, m10, m01, m11,
                       p1.x - (m00 * p1.x + m01 * p1.y),
                       p1.y - (m10 * p1.x + m11 * p1.y));
}

double Transform2D::lengthScaleAlong(const Vector2D& v) const noexcept
{
    const double len = v.length();
    if (len <= kEpsilon) return 1.0;
    return applyVector(v).length() / len;
}

bool Transform2D::isSimilarity(double tol) const noexcept
{
    // 相似变换：两列向量等长且正交
    const Vector2D col0(a, b);
    const Vector2D col1(c, d);
    const double l0 = col0.length();
    const double l1 = col1.length();
    if (l0 <= kEpsilon || l1 <= kEpsilon) return false;
    if (std::abs(l0 - l1) > tol * std::max(l0, l1)) return false;
    if (std::abs(dot(col0, col1)) > tol * l0 * l1) return false;
    return true;
}

bool Transform2D::isIdentity(double tol) const noexcept
{
    return nearlyEqual(a, 1.0, tol) && nearlyEqual(b, 0.0, tol)
        && nearlyEqual(c, 0.0, tol) && nearlyEqual(d, 1.0, tol)
        && nearlyEqual(e, 0.0, tol) && nearlyEqual(f, 0.0, tol);
}

std::optional<Transform2D> Transform2D::inverted() const noexcept
{
    const double det = determinant();
    if (std::abs(det) <= kEpsilon) return std::nullopt;
    const double inv = 1.0 / det;
    Transform2D out;
    out.a =  d * inv;
    out.b = -b * inv;
    out.c = -c * inv;
    out.d =  a * inv;
    out.e = (c * f - d * e) * inv;
    out.f = (b * e - a * f) * inv;
    return out;
}

double Transform2D::rotationAngle() const noexcept
{
    return std::atan2(b, a);
}

Transform2D Transform2D::operator*(const Transform2D& r) const noexcept
{
    Transform2D out;
    out.a = a * r.a + c * r.b;
    out.b = b * r.a + d * r.b;
    out.c = a * r.c + c * r.d;
    out.d = b * r.c + d * r.d;
    out.e = a * r.e + c * r.f + e;
    out.f = b * r.e + d * r.f + f;
    return out;
}

// -----------------------------------------------------------------------------
//  线段 / 点集辅助
// -----------------------------------------------------------------------------
double distancePointToSegment(const Point2D& p, const Point2D& a, const Point2D& b) noexcept
{
    return p.distanceTo(closestPointOnSegment(p, a, b));
}

Point2D closestPointOnSegment(const Point2D& p, const Point2D& a, const Point2D& b) noexcept
{
    const Vector2D ab = b - a;
    const double lenSq = ab.lengthSquared();
    if (lenSq <= kEpsilon * kEpsilon) return a;
    double t = dot(p - a, ab) / lenSq;
    t = std::clamp(t, 0.0, 1.0);
    return a + ab * t;
}

bool segmentIntersectsBox(const Point2D& a, const Point2D& b, const Box2D& box) noexcept
{
    if (!box.valid) return false;

    // 任一端点在盒内即相交
    if (box.contains(a) || box.contains(b)) return true;

    // 线段包围盒与目标盒不相交则可快速排除
    Box2D segBox;
    segBox.expand(a);
    segBox.expand(b);
    if (!segBox.intersects(box)) return false;

    // Liang-Barsky 裁剪
    double t0 = 0.0;
    double t1 = 1.0;
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;

    const double p[4] = { -dx, dx, -dy, dy };
    const double q[4] = { a.x - box.minX, box.maxX - a.x, a.y - box.minY, box.maxY - a.y };

    for (int i = 0; i < 4; ++i) {
        if (std::abs(p[i]) <= kEpsilon) {
            if (q[i] < 0.0) return false;      // 平行且在外侧
            continue;
        }
        const double r = q[i] / p[i];
        if (p[i] < 0.0) {
            if (r > t1) return false;
            if (r > t0) t0 = r;
        } else {
            if (r < t0) return false;
            if (r < t1) t1 = r;
        }
    }
    return t0 <= t1;
}

std::optional<Point2D> segmentSegmentIntersection(const Point2D& a1, const Point2D& a2,
                                                  const Point2D& b1, const Point2D& b2,
                                                  double tol) noexcept
{
    const Vector2D r = a2 - a1;
    const Vector2D s = b2 - b1;
    const double denom = cross(r, s);
    if (std::abs(denom) <= tol) return std::nullopt;   // 平行或共线

    const Vector2D qp = b1 - a1;
    const double t = cross(qp, s) / denom;
    const double u = cross(qp, r) / denom;
    if (t < -tol || t > 1.0 + tol || u < -tol || u > 1.0 + tol) return std::nullopt;

    return a1 + r * std::clamp(t, 0.0, 1.0);
}

bool polylineIntersectsBox(const Point2D* pts, std::size_t count, bool closed, const Box2D& box) noexcept
{
    if (!pts || count == 0 || !box.valid) return false;
    if (count == 1) return box.contains(pts[0]);

    for (std::size_t i = 0; i + 1 < count; ++i) {
        if (segmentIntersectsBox(pts[i], pts[i + 1], box)) return true;
    }
    if (closed && segmentIntersectsBox(pts[count - 1], pts[0], box)) return true;
    return false;
}

Box2D boxOfPoints(const Point2D* pts, std::size_t count) noexcept
{
    Box2D box;
    for (std::size_t i = 0; i < count; ++i) box.expand(pts[i]);
    return box;
}

int segmentCountForArc(double radius, double angleSpan, double sagitta) noexcept
{
    const double absSpan = std::abs(angleSpan);
    if (radius <= kEpsilon || absSpan <= kEpsilon) return 1;

    const double sag = (sagitta > kEpsilon) ? sagitta : 1e-3;
    if (sag >= radius) {
        // 矢高比半径还大：直接按角度粗略分段
        return std::clamp(static_cast<int>(std::ceil(absSpan / (kPi / 8.0))), 4, 512);
    }

    // 单段半角： θ/2 = acos(1 - sag/r)
    const double halfTheta = std::acos(std::clamp(1.0 - sag / radius, -1.0, 1.0));
    if (halfTheta <= kEpsilon) return 512;
    const int n = static_cast<int>(std::ceil(absSpan / (2.0 * halfTheta)));
    return std::clamp(n, 8, 2048);
}

} // namespace lycad
