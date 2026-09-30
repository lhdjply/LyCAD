// =============================================================================
//  LyCAD - core/Types.h
//  二维几何基础类型：向量、点、包围盒、仿射变换。
//
//  坐标系约定
//    · 世界坐标系（WCS）：X 向右、Y 向上，右手系，单位默认为毫米。
//    · 角度：内部一律使用弧度，逆时针为正；仅在与 DXF 交互时转换为度。
//    · 姿态角：正东（+X）为 0，逆时针增大，值域 [0, 2π)。
// =============================================================================
#pragma once

#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace lycad {

using EntityId = quint64;
inline constexpr EntityId kInvalidEntityId = 0;

inline constexpr double kPi      = 3.14159265358979323846;
inline constexpr double kTwoPi   = 6.28318530717958647692;
inline constexpr double kHalfPi  = 1.57079632679489661923;

/// 数值比较容差（用于浮点相等判断，非几何容差）
inline constexpr double kEpsilon = 1e-12;

/// 几何容差：世界单位下的最小可分辨量
inline constexpr double kGeomTol = 1e-9;

/// 无穷大包围盒初值
inline constexpr double kHugeValue = 1e300;

double degToRad(double deg);
double radToDeg(double rad);

/// 归一化到 [0, 2π)
double normalizeAngle(double a);

/// 逆时针自 start 到 end 的角度跨度，结果位于 [0, 2π)
double angleSpanCcw(double start, double end);

/// 判断角度 a 是否位于自 start 起逆时针到 end 的弧内（两端包含）
bool angleWithin(double a, double start, double end, bool ccw);

/// 相对 + 绝对混合容差比较
bool nearlyEqual(double a, double b, double tol = kEpsilon);

/// 安全除法：|denominator| <= tol 时返回 fallback
double safeDiv(double numerator, double denominator, double fallback = 0.0);

// -----------------------------------------------------------------------------
//  Vector2D
// -----------------------------------------------------------------------------
struct Point2D;

struct Vector2D {
    double x = 0.0;
    double y = 0.0;

    constexpr Vector2D() = default;
    constexpr Vector2D(double X, double Y) noexcept : x(X), y(Y) {}

    double lengthSquared() const noexcept { return x * x + y * y; }
    double length() const noexcept { return std::hypot(x, y); }

    /// 单位化；零向量返回 (0,0)
    Vector2D normalized() const;
    Vector2D normalizedOr(const Vector2D& fallback) const;

    /// 姿态角，值域 (-π, π]
    double angle() const noexcept { return std::atan2(y, x); }

    /// 逆时针旋转 90°
    Vector2D perpendicular() const noexcept { return Vector2D(-y, x); }

    Vector2D rotated(double rad) const noexcept;
    Vector2D scaled(double sx, double sy) const noexcept { return Vector2D(x * sx, y * sy); }

    Vector2D operator-() const noexcept { return Vector2D(-x, -y); }
    Vector2D& operator+=(const Vector2D& o) noexcept { x += o.x; y += o.y; return *this; }
    Vector2D& operator-=(const Vector2D& o) noexcept { x -= o.x; y -= o.y; return *this; }
    Vector2D& operator*=(double s) noexcept { x *= s; y *= s; return *this; }
    Vector2D& operator/=(double s) noexcept { x /= s; y /= s; return *this; }
};

inline Vector2D operator+(Vector2D a, const Vector2D& b) noexcept { return a += b; }
inline Vector2D operator-(Vector2D a, const Vector2D& b) noexcept { return a -= b; }
inline Vector2D operator*(Vector2D v, double s) noexcept { return v *= s; }
inline Vector2D operator*(double s, Vector2D v) noexcept { return v *= s; }
inline Vector2D operator/(Vector2D v, double s) noexcept { return v /= s; }

/// 点积
inline double dot(const Vector2D& a, const Vector2D& b) noexcept { return a.x * b.x + a.y * b.y; }
/// 叉积的 z 分量（>0 表示 b 在 a 的逆时针方向）
inline double cross(const Vector2D& a, const Vector2D& b) noexcept { return a.x * b.y - a.y * b.x; }
/// 二维行列式，等价于 cross
inline double perpDot(const Vector2D& a, const Vector2D& b) noexcept { return cross(a, b); }

// -----------------------------------------------------------------------------
//  Point2D
// -----------------------------------------------------------------------------
struct Point2D {
    double x = 0.0;
    double y = 0.0;

    constexpr Point2D() = default;
    constexpr Point2D(double X, double Y) noexcept : x(X), y(Y) {}

    Vector2D asVector() const noexcept { return Vector2D(x, y); }

    Point2D translated(const Vector2D& v) const noexcept { return Point2D(x + v.x, y + v.y); }
    Point2D rotatedAround(const Point2D& c, double rad) const noexcept;

    double distanceTo(const Point2D& o) const noexcept { return std::hypot(x - o.x, y - o.y); }
    double distanceSquaredTo(const Point2D& o) const noexcept;

    /// 相对本点的向量
    Vector2D to(const Point2D& o) const noexcept { return Vector2D(o.x - x, o.y - y); }
    /// 相对本点的姿态角
    double angleTo(const Point2D& o) const noexcept { return std::atan2(o.y - y, o.x - x); }

    Point2D& operator+=(const Vector2D& v) noexcept { x += v.x; y += v.y; return *this; }
    Point2D& operator-=(const Vector2D& v) noexcept { x -= v.x; y -= v.y; return *this; }
};

inline Point2D operator+(Point2D p, const Vector2D& v) noexcept { return p += v; }
inline Point2D operator+(const Vector2D& v, Point2D p) noexcept { return p += v; }
inline Point2D operator-(Point2D p, const Vector2D& v) noexcept { return p -= v; }
inline Vector2D operator-(const Point2D& a, const Point2D& b) noexcept { return Vector2D(a.x - b.x, a.y - b.y); }

inline bool operator==(const Point2D& a, const Point2D& b) noexcept
{
    return nearlyEqual(a.x, b.x) && nearlyEqual(a.y, b.y);
}

// -----------------------------------------------------------------------------
//  Box2D - 轴对齐包围盒
// -----------------------------------------------------------------------------
struct Box2D {
    double minX = 0.0;
    double minY = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;
    bool   valid = false;   ///< false 表示空盒（未包含任何点）

    Box2D() = default;
    Box2D(double x0, double y0, double x1, double y1)
        : minX(std::min(x0, x1)), minY(std::min(y0, y1)),
          maxX(std::max(x0, x1)), maxY(std::max(y0, y1)), valid(true) {}

    /// 由两对角点构造
    static Box2D fromCorners(const Point2D& a, const Point2D& b)
    {
        return Box2D(a.x, a.y, b.x, b.y);
    }

    /// 由中心与半宽半高构造
    static Box2D fromCenter(const Point2D& c, double halfW, double halfH)
    {
        return Box2D(c.x - halfW, c.y - halfH, c.x + halfW, c.y + halfH);
    }

    static Box2D aroundPoint(const Point2D& p, double radius)
    {
        return Box2D(p.x - radius, p.y - radius, p.x + radius, p.y + radius);
    }

    void clear() noexcept { minX = minY = maxX = maxY = 0.0; valid = false; }

    bool isValid() const noexcept { return valid; }

    /// 退化盒：有效但宽或高为 0（点、水平/垂直线段）
    bool isDegenerate() const noexcept
    {
        return !valid || std::abs(maxX - minX) <= kGeomTol || std::abs(maxY - minY) <= kGeomTol;
    }

    double width() const noexcept { return valid ? maxX - minX : 0.0; }
    double height() const noexcept { return valid ? maxY - minY : 0.0; }
    double maxDimension() const noexcept { return std::max(width(), height()); }
    double diagonal() const noexcept { return std::hypot(width(), height()); }
    double area() const noexcept { return valid ? width() * height() : 0.0; }

    Point2D center() const noexcept { return Point2D((minX + maxX) * 0.5, (minY + maxY) * 0.5); }
    Point2D minCorner() const noexcept { return Point2D(minX, minY); }
    Point2D maxCorner() const noexcept { return Point2D(maxX, maxY); }

    void expand(const Point2D& p) noexcept;
    void expand(const Box2D& b) noexcept;
    void expand(double margin) noexcept;

    bool contains(const Point2D& p, double tol = 0.0) const noexcept;
    bool contains(const Box2D& b, double tol = 0.0) const noexcept;
    bool intersects(const Box2D& b) const noexcept;
    std::optional<Box2D> intersected(const Box2D& b) const noexcept;

    /// 以自身为中心按比例放大 margin ratio（用于视图缩放的余量）
    Box2D inflatedBy(double ratio) const;

    bool operator==(const Box2D& o) const noexcept
    {
        if (valid != o.valid) return false;
        if (!valid) return true;
        return nearlyEqual(minX, o.minX) && nearlyEqual(minY, o.minY)
            && nearlyEqual(maxX, o.maxX) && nearlyEqual(maxY, o.maxY);
    }
};

// -----------------------------------------------------------------------------
//  Transform2D - 2D 仿射变换
//
//      | a  c  e |        x' = a·x + c·y + e
//      | b  d  f |        y' = b·x + d·y + f
//      | 0  0  1 |
//
//  复合运算约定： (A * B) 表示「先施加 B，再施加 A」，即 apply(A*B, p) == A.apply(B.apply(p))
// -----------------------------------------------------------------------------
struct Transform2D {
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 1.0;
    double e = 0.0;
    double f = 0.0;

    constexpr Transform2D() = default;
    constexpr Transform2D(double A, double B, double C, double D, double E, double F) noexcept
        : a(A), b(B), c(C), d(D), e(E), f(F) {}

    static Transform2D identity() noexcept { return Transform2D(); }

    static Transform2D translation(double dx, double dy) noexcept
    {
        return Transform2D(1.0, 0.0, 0.0, 1.0, dx, dy);
    }

    static Transform2D translation(const Vector2D& v) noexcept { return translation(v.x, v.y); }

    static Transform2D rotation(double rad) noexcept;

    static Transform2D rotationAround(const Point2D& c, double rad) noexcept;

    static Transform2D scaling(double sx, double sy) noexcept
    {
        return Transform2D(sx, 0.0, 0.0, sy, 0.0, 0.0);
    }

    static Transform2D scalingAround(const Point2D& c, double sx, double sy) noexcept;

    /// 关于经过 p1、p2 的直线镜像；两点重合时退化为恒等变换
    static Transform2D mirror(const Point2D& p1, const Point2D& p2) noexcept;

    Point2D  apply(const Point2D& p) const noexcept
    {
        return Point2D(a * p.x + c * p.y + e, b * p.x + d * p.y + f);
    }

    Vector2D applyVector(const Vector2D& v) const noexcept
    {
        return Vector2D(a * v.x + c * v.y, b * v.x + d * v.y);
    }

    /// 本变换作用于 v 之后的长度缩放系数（角度相关，非等比时随方向变化）
    double lengthScaleAlong(const Vector2D& v) const noexcept;

    /// 旋转 + 等比缩放变换下的等效均匀缩放系数 sqrt(|det|)
    double uniformScale() const noexcept { return std::sqrt(std::abs(determinant())); }

    double determinant() const noexcept { return a * d - b * c; }

    /// 是否含镜像（行列式为负）
    bool isMirroring() const noexcept { return determinant() < 0.0; }

    /// 是否为「相似变换」：仅含平移、旋转、等比缩放，不含错切与非等比拉伸。
    /// 圆与圆弧只有在此条件下才能保持为圆与圆弧。
    bool isSimilarity(double tol = 1e-9) const noexcept;

    bool isIdentity(double tol = 1e-12) const noexcept;

    std::optional<Transform2D> inverted() const noexcept;

    /// 相似变换下的旋转分量
    double rotationAngle() const noexcept;

    /// 复合：先 rhs，后 this
    Transform2D operator*(const Transform2D& rhs) const noexcept;
};

// -----------------------------------------------------------------------------
//  通用线段 / 点集辅助
// -----------------------------------------------------------------------------
/// 点到线段的最短距离
double distancePointToSegment(const Point2D& p, const Point2D& a, const Point2D& b) noexcept;

/// 点到线段的最近点（端点范围内）
Point2D closestPointOnSegment(const Point2D& p, const Point2D& a, const Point2D& b) noexcept;

/// 线段与轴对齐盒是否相交（含完全包含的情形）
bool segmentIntersectsBox(const Point2D& a, const Point2D& b, const Box2D& box) noexcept;

/// 两个线段的交点；平行或共线重叠时返回 std::nullopt
std::optional<Point2D> segmentSegmentIntersection(const Point2D& a1, const Point2D& a2,
                                                  const Point2D& b1, const Point2D& b2,
                                                  double tol = 1e-9) noexcept;

/// 折线（隐式闭合）与轴对齐盒是否相交
bool polylineIntersectsBox(const Point2D* pts, std::size_t count, bool closed, const Box2D& box) noexcept;

/// 折线包围盒
Box2D boxOfPoints(const Point2D* pts, std::size_t count) noexcept;

/// 依据半径选择离散圆弧的段数，保证矢高误差不超过 sagitta
int segmentCountForArc(double radius, double angleSpan, double sagitta) noexcept;

} // namespace lycad
