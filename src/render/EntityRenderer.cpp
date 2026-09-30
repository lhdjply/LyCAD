// =============================================================================
//  LyCAD - render/EntityRenderer.cpp
// =============================================================================
#include "render/EntityRenderer.h"

#include "core/ArcEntity.h"
#include "core/CircleEntity.h"
#include "core/DimensionEntity.h"
#include "core/EllipseEntity.h"
#include "core/HatchEntity.h"
#include "core/LineEntity.h"
#include "core/Linetype.h"
#include "core/PointEntity.h"
#include "core/PolylineEntity.h"
#include "core/SplineEntity.h"
#include "core/TextEntity.h"

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainter>

#include <algorithm>
#include <array>

namespace lycad {
namespace EntityRenderer {

namespace {

/// 圆在可见盒内的角度窗口
struct AngleWindow {
    bool   full = false;                 ///< 圆心位于盒内，整圆可见
    double start = 0.0;                  ///< 弧度
    double span = kTwoPi;
};

/**
 * 求可见矩形相对圆心的角度覆盖窗口。
 * 圆心在盒外时窗口必然小于 2π，等价于「四角角度」的最小覆盖弧。
 */
AngleWindow computeAngleWindow(const Point2D& center, const QRectF& viewport)
{
    AngleWindow window;

    if (viewport.contains(QPointF(center.x, center.y))) {
        window.full = true;
        return window;
    }

    std::array<double, 4> angles = {
        std::atan2(viewport.top() - center.y,    viewport.left() - center.x),
        std::atan2(viewport.top() - center.y,    viewport.right() - center.x),
        std::atan2(viewport.bottom() - center.y, viewport.right() - center.x),
        std::atan2(viewport.bottom() - center.y, viewport.left() - center.x),
    };
    std::sort(angles.begin(), angles.end());

    // 四个角之外的最大间隙即为「没有图形的方向」，其补集就是覆盖窗口
    double largestGap = -1.0;
    int    gapIndex = 0;
    for (int i = 0; i < 4; ++i) {
        double gap = angles[(i + 1) % 4] - angles[i];
        if (i == 3) gap += kTwoPi;
        if (gap > largestGap) {
            largestGap = gap;
            gapIndex = i;
        }
    }

    window.start = angles[(gapIndex + 1) % 4];
    window.span = kTwoPi - largestGap;
    return window;
}

/// 两段圆弧在圆上是否重叠
bool arcsOverlap(double start1, double span1, double start2, double span2)
{
    if (span1 >= kTwoPi - 1e-9 || span2 >= kTwoPi - 1e-9) return true;

    const auto within = [](double angle, double start, double span) {
        return normalizeAngle(angle - start) <= span + 1e-9;
    };
    return within(start2, start1, span1) || within(start1, start2, span2);
}

/// 由线宽推导画笔宽度（像素）
double penWidthPixels(double lineweightMm, const DrawContext& context)
{
    const DisplaySettings& s = *context.settings;
    if (!s.showLineweight || lineweightMm <= 0.0) return s.minLinewidthPixels;

    const double px = context.view->worldLengthToDevice(lineweightMm);
    return std::clamp(px, s.minLinewidthPixels, s.maxLinewidthPixels);
}

/// 依据线型构造画笔样式；虚线段过密时退化为实线，避免屏幕上一片灰雾
Qt::PenStyle penStyleFor(const QString& linetypeName, const DrawContext& context)
{
    if (linetypeName.isEmpty()) return Qt::SolidLine;

    const LinetypePattern pattern = linetypes::resolve(linetypeName);
    if (pattern.pattern.empty()) return Qt::SolidLine;

    // 以模式中最短的实线段判断：若其在屏幕上不足 2 像素则线条无法分辨
    double shortest = std::numeric_limits<double>::max();
    for (double v : pattern.pattern) {
        if (v > 0.0) shortest = std::min(shortest, v);
    }
    if (shortest == std::numeric_limits<double>::max()) return Qt::DotLine;   // 纯点线

    const double px = context.view->worldLengthToDevice(shortest);
    if (px < 2.0) return Qt::SolidLine;

    const QString upper = pattern.name.toUpper();
    if (upper == QLatin1String("DASHED") || upper == QLatin1String("HIDDEN")) return Qt::DashLine;
    if (upper == QLatin1String("DOT")) return Qt::DotLine;
    if (upper == QLatin1String("DASHDOT")) return Qt::DashDotLine;
    if (upper == QLatin1String("DASHDOTDOT")) return Qt::DashDotDotLine;

    // 其余模式（CENTER / PHANTOM / BORDER 等）用自定义虚线近似，
    // 以屏幕像素为单位设定，保证任意缩放下疏密一致
    return Qt::CustomDashLine;
}

void applyCustomDash(QPen& pen, const LinetypePattern& pattern, const DrawContext& context)
{
    QList<qreal> dashes;
    dashes.reserve(static_cast<qsizetype>(pattern.pattern.size()));

    for (double v : pattern.pattern) {
        double px = context.view->worldLengthToDevice(std::abs(v));
        // 屏幕上的虚线过长或过短都不好看，限制在 1~40 像素
        px = std::clamp(px, 1.0, 40.0);
        dashes.append(v < 0.0 ? px : px);
    }
    if (!dashes.isEmpty()) pen.setDashPattern(dashes);
}

/**
 * 超大半径的圆/圆弧：屏幕半径已大到曲率低于一个像素，
 * 以切线线段近似即可达到视觉精确，同时规避 Qt 光栅化定点坐标的上限。
 */
void drawAsTangentLine(QPainter& painter, const Point2D& centerDevice, double radiusPixels,
                       const AngleWindow& window, bool isArc,
                       double arcStart, double arcSpan, const QRectF& viewport)
{
    const QPointF viewCenter = viewport.center();
    const Vector2D toView(viewCenter.x() - centerDevice.x, viewCenter.y() - centerDevice.y);
    const double distToCenter = toView.length();

    // 视口中心到圆周的最近/最远方向角
    const double baseAngle = (distToCenter <= kEpsilon)
        ? 0.0
        : std::atan2(toView.y, toView.x);

    // 视口对角线长度，足以覆盖整个可见区域
    const double diagonal = std::hypot(viewport.width(), viewport.height()) * 1.5;

    // 可见角度半宽：视口半径 / 圆半径
    const double halfWidth = (radiusPixels > kEpsilon)
        ? std::min(kPi, diagonal * 0.5 / radiusPixels + 1e-3)
        : kPi;

    double start = baseAngle - halfWidth;
    double span = halfWidth * 2.0;

    if (isArc) {
        // 与弧自身角度区间求交：把窗口起点归一到弧的参照系
        const double relStart = normalizeAngle(start - arcStart);
        const double arcSpanNorm = std::min(arcSpan, kTwoPi);
        double lo = std::max(0.0, relStart);
        double hi = std::min(arcSpanNorm, relStart + span);
        if (relStart > arcSpanNorm) {
            // 窗口整体位于弧之外侧，尝试绕回
            lo = 0.0;
            hi = std::min(arcSpanNorm, relStart + span - kTwoPi);
        }
        if (hi <= lo) return;
        start = arcStart + lo;
        span = hi - lo;
    } else if (!window.full) {
        // 整圆情形同样只画可见窗口
        const double relStart = normalizeAngle(start - window.start);
        if (relStart > window.span) return;
        span = std::min(span, window.span - relStart);
        start = window.start + relStart;
    }

    // 用两条弦近似这段极短的弧
    const double steps = 2.0;
    Point2D previous(centerDevice.x + radiusPixels * std::cos(start),
                     centerDevice.y - radiusPixels * std::sin(start));
    for (int i = 1; i <= static_cast<int>(steps); ++i) {
        const double a = start + span * (i / steps);
        const Point2D current(centerDevice.x + radiusPixels * std::cos(a),
                              centerDevice.y - radiusPixels * std::sin(a));
        painter.drawLine(QPointF(previous.x, previous.y), QPointF(current.x, current.y));
        previous = current;
    }
}

/// 图元是否与视口相交（粗略剔除，用世界包围盒换算后的设备矩形判断）
bool visibleInViewport(const Entity& entity, const ViewTransform& view, const QRectF& viewport)
{
    const Box2D worldBox = entity.boundingBox();
    if (!worldBox.isValid()) return false;

    const Point2D a = view.toDevice(Point2D(worldBox.minX, worldBox.minY));
    const Point2D b = view.toDevice(Point2D(worldBox.maxX, worldBox.maxY));

    const QRectF rect(QPointF(std::min(a.x, b.x), std::min(a.y, b.y)),
                      QPointF(std::max(a.x, b.x), std::max(a.y, b.y)));
    // 点状/线状图元（竖直的线、样条）包围盒宽或高为 0，而 QRectF::intersects
    // 把零宽高一律判为不相交——竖直线会被整体剔除（实测一幅端子图里全部
    // 竖直样条因此消失）。膨胀半个像素的余量再判。
    const QRectF padded = rect.adjusted(-0.5, -0.5, 0.5, 0.5);
    return viewport.intersects(padded);
}

void drawCircleEntity(QPainter& painter, const CircleEntity& circle, const DrawContext& context,
                      const AngleWindow& window)
{
    const Point2D centerDevice = context.view->toDevice(circle.center());
    const double radiusPixels = circle.radius() * context.view->scale();

    // 屏幕半径不足半个像素：退化为一个点，否则会画出一团模糊的圆
    if (radiusPixels < 0.5) {
        painter.drawPoint(QPointF(centerDevice.x, centerDevice.y));
        return;
    }

    if (radiusPixels <= kMaxExactRadiusPixels) {
        painter.drawEllipse(QPointF(centerDevice.x, centerDevice.y), radiusPixels, radiusPixels);
        return;
    }

    drawAsTangentLine(painter, centerDevice, radiusPixels, window,
                      false, 0.0, kTwoPi, context.viewport);
}

void drawArcEntity(QPainter& painter, const ArcEntity& arc, const DrawContext& context,
                   const AngleWindow& window)
{
    const Point2D centerDevice = context.view->toDevice(arc.center());
    const double radiusPixels = arc.radius() * context.view->scale();
    const double startRad = arc.startAngle();
    const double spanRad = arc.spanAngle();

    if (radiusPixels < 0.5) {
        // 弧长不足一个像素时用一段短线表示，避免缩到很远后图元凭空消失
        const Point2D s = context.view->toDevice(arc.startPoint());
        const Point2D e = context.view->toDevice(arc.endPoint());
        painter.drawLine(QPointF(s.x, s.y), QPointF(e.x, e.y));
        return;
    }

    if (radiusPixels <= kMaxExactRadiusPixels) {
        const QRectF rect(centerDevice.x - radiusPixels, centerDevice.y - radiusPixels,
                          radiusPixels * 2.0, radiusPixels * 2.0);
        // Qt 的 drawArc 以 1/16 度为单位，自 3 点钟方向逆时针为正，
        // 且在逻辑坐标中按 Y 轴向上解释角度——与内部的世界坐标约定完全一致，
        // 因此角度可直接映射，无需换算或取负。
        const int start16 = static_cast<int>(std::lround(radToDeg(startRad) * 16.0));
        const int span16  = static_cast<int>(std::lround(radToDeg(spanRad) * 16.0));
        painter.drawArc(rect, start16, span16);
        return;
    }

    drawAsTangentLine(painter, centerDevice, radiusPixels, window,
                      true, startRad, spanRad, context.viewport);
}

/// 用离散点序列画一条开放/闭合曲线（样条、椭圆等 flatten 后统一走这里）
void drawFlattenedCurve(QPainter& painter, const std::vector<Point2D>& points,
                        bool closed, const DrawContext& context)
{
    if (points.empty()) return;
    if (points.size() == 1) {
        const Point2D d = context.view->toDevice(points.front());
        drawPointMarker(painter, d, painter.pen().color(), 3.0);
        return;
    }

    QPainterPath path;
    const Point2D first = context.view->toDevice(points.front());
    path.moveTo(first.x, first.y);
    for (std::size_t i = 1; i < points.size(); ++i) {
        const Point2D d = context.view->toDevice(points[i]);
        path.lineTo(d.x, d.y);
    }
    if (closed) path.closeSubpath();
    painter.drawPath(path);
}

void drawPolylineEntity(QPainter& painter, const PolylineEntity& polyline,
                        const DrawContext& context)
{
    const std::vector<Point2D> points = polyline.flatten(adaptiveSagitta(context));
    if (points.empty()) return;

    if (points.size() == 1) {
        const Point2D d = context.view->toDevice(points.front());
        drawPointMarker(painter, d, painter.pen().color(), 3.0);
        return;
    }

    QPainterPath path;
    const Point2D first = context.view->toDevice(points.front());
    path.moveTo(first.x, first.y);
    for (std::size_t i = 1; i < points.size(); ++i) {
        const Point2D d = context.view->toDevice(points[i]);
        path.lineTo(d.x, d.y);
    }
    if (polyline.isClosed()) path.closeSubpath();

    // 宽度是真实几何（PCB 走线等）：以像素级线宽描边，而非发丝线
    if (polyline.hasWidth()) {
        const double widthPixels = polyline.maxWidth() * context.view->scale();
        // 屏幕上不足 1.2 像素的宽度无法分辨，退回细线；上限防止巨量填充
        if (widthPixels >= 1.2) {
            QPen pen = painter.pen();
            pen.setWidthF(std::min(widthPixels, 600.0));
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(pen);
        }
        painter.drawPath(path);
        return;
    }

    painter.drawPath(path);
}

/// 逐段绘制宽度不均匀的多段线：每段取该段两端宽度的较大值
void drawPolylineWithVaryingWidth(QPainter& painter, const PolylineEntity& polyline,
                                  const DrawContext& context)
{
    const double sagitta = adaptiveSagitta(context);
    const auto segments = polyline.segments();

    for (std::size_t i = 0; i < segments.size(); ++i) {
        const PolylineSegment& seg = segments[i];
        const double widthPixels =
            polyline.segmentWidth(i) * context.view->scale();

        QPainterPath segPath;
        const Point2D startDevice = context.view->toDevice(seg.start);
        segPath.moveTo(startDevice.x, startDevice.y);

        if (!seg.isArc) {
            const Point2D endDevice = context.view->toDevice(seg.end);
            segPath.lineTo(endDevice.x, endDevice.y);
        } else {
            // 与 flatten 同策略：按 bulge 符号决定行进方向
            double a0 = seg.startAngle;
            double a1 = seg.endAngle;
            if (seg.bulge < 0.0) std::swap(a0, a1);
            const double span = seg.spanAngle();
            const int steps = std::max(2, segmentCountForArc(seg.radius, span, sagitta));
            const double step = span / steps;
            for (int k = 1; k <= steps; ++k) {
                const double a = a0 + step * k;
                const Point2D world(seg.center.x + seg.radius * std::cos(a),
                                    seg.center.y + seg.radius * std::sin(a));
                const Point2D d = context.view->toDevice(world);
                segPath.lineTo(d.x, d.y);
            }
        }

        QPen pen = painter.pen();
        pen.setWidthF(std::min(std::max(widthPixels, 1.0), 600.0));
        painter.strokePath(segPath, pen);
    }
}

// -----------------------------------------------------------------------------
//  文字
// -----------------------------------------------------------------------------
/// DXF 的字高（组码 40）定义为**大写字母高度（cap height）**，
/// 而 QFont 的 pixelSize 是 em 尺寸（≈ ascent + descent）。
/// 典型字体的 cap-height ≈ 0.72 em，需按此换算才能与 AutoCAD 的字号一致。
constexpr double kCapHeightToEmRatio = 0.72;

void drawTextEntity(QPainter& painter, const TextEntity& text, const DrawContext& context)
{
    const QString content = text.displayString();
    if (content.isEmpty()) return;

    const Point2D anchorDevice = context.view->toDevice(text.effectivePosition());
    const double capHeightPixels = text.height() * context.view->scale();

    // 缩得太小时画一个点占位，保持「这里有文字」的可见性
    if (capHeightPixels < 2.0) {
        painter.drawPoint(QPointF(anchorDevice.x, anchorDevice.y));
        return;
    }

    QFont font = painter.font();
    font.setPixelSize(std::max(2, static_cast<int>(std::lround(capHeightPixels
                                                              / kCapHeightToEmRatio))));
    // 宽度因子（DXF 41）：用字体横向拉伸实现，字形与步进同步缩放
    if (std::abs(text.widthFactor() - 1.0) > 1e-3) {
        font.setStretch(std::clamp(static_cast<int>(std::lround(text.widthFactor() * 100)),
                                   1, 4000));
    }
    // 文字样式名（DXF 7）：系统存在同名字体家族时采用（如 "黑体"/"Arial"）
    if (!text.fontStyle().isEmpty()
        && QFontDatabase::hasFamily(text.fontStyle())) {
        font.setFamily(text.fontStyle());
    }
    painter.setFont(font);
    const QFontMetricsF metrics(font);

    painter.save();
    painter.translate(anchorDevice.x, anchorDevice.y);
    // 世界坐标逆时针为正；设备坐标 Y 轴向下，方向相反
    painter.rotate(-radToDeg(text.rotation()));

    // 生成标志（DXF 71）：bit2 = 水平镜像（背面文字），bit4 = 垂直镜像。
    // 镜像施加在旋转之后的文字局部坐标系内，与 AutoCAD 行为一致：
    // 水平镜像时文字自插入点向左延伸（左对齐语义下）。
    if (text.isBackwards() || text.isUpsideDown()) {
        painter.scale(text.isBackwards() ? -1.0 : 1.0,
                      text.isUpsideDown() ? -1.0 : 1.0);
    }

    // 多行（MTEXT 降级，\n 分段）：整块参与对齐，逐行绘制
    const QStringList lines = content.split(QLatin1Char('\n'));
    const double blockSpan =
        static_cast<double>(lines.size() - 1) * metrics.lineSpacing();

    // 垂直对齐：换算为**首行**基线相对锚点的偏移（设备帧内 y 向下为正）。
    // 单行时与原行为完全一致；多行时块的扩展方向：
    //   顶部对齐 → 块向下长    底部对齐 → 块向上长    中部对齐 → 两头伸
    double baselineOffset = 0.0;
    switch (text.vAlign()) {
    case TextEntity::VAlign::Baseline: baselineOffset = 0.0; break;
    case TextEntity::VAlign::Bottom:   baselineOffset = -metrics.descent() - blockSpan; break;
    case TextEntity::VAlign::Middle:   baselineOffset = (metrics.ascent() - metrics.descent() - blockSpan) * 0.5; break;
    case TextEntity::VAlign::Top:      baselineOffset = metrics.ascent(); break;
    }

    // 水平对齐按**各行**的宽度计算（多行时每行起点不同）
    double y = baselineOffset;
    for (const QString& line : lines) {
        if (!line.isEmpty()) {
            const double advance = metrics.horizontalAdvance(line);
            double x = 0.0;
            switch (text.hAlign()) {
            case TextEntity::HAlign::Left:   x = 0.0; break;
            case TextEntity::HAlign::Center: x = -advance * 0.5; break;
            case TextEntity::HAlign::Right:  x = -advance; break;
            }
            painter.drawText(QPointF(x, y), line);
        }
        y += metrics.lineSpacing();
    }
    painter.restore();
}

// -----------------------------------------------------------------------------
//  填充
// -----------------------------------------------------------------------------
void drawHatchEntity(QPainter& painter, const HatchEntity& hatch, const DrawContext& context)
{
    // 边界可能含凸度（圆弧焊盘），统一经 flatten 离散
    const std::vector<Point2D> pts = hatch.flatten(adaptiveSagitta(context));
    if (pts.size() < 3) return;

    QPainterPath path;
    const Point2D first = context.view->toDevice(pts.front());
    path.moveTo(first.x, first.y);
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const Point2D d = context.view->toDevice(pts[i]);
        path.lineTo(d.x, d.y);
    }
    path.closeSubpath();

    const QColor color = painter.pen().color();

    if (hatch.isSolid()) {
        painter.fillPath(path, color);
        painter.drawPath(path);
        return;
    }

    // 平行线图案：间距不足时退化为边界轮廓，避免糊成一团
    const double spacingPixels = hatch.patternSpacing() * context.view->scale();
    if (spacingPixels >= 2.5) {
        // 图案线方向（世界逆时针角 → 设备坐标 Y 向下取负）
        const double theta = hatch.patternLineAngle();
        const Vector2D dir(std::cos(theta), -std::sin(theta));
        const Vector2D perp = dir.perpendicular();

        // 在路径包围盒上求图案线族的参数范围
        const QRectF bounds = path.boundingRect();
        const QPointF corners[4] = {
            bounds.topLeft(), bounds.topRight(), bounds.bottomLeft(), bounds.bottomRight()
        };
        double sMin = std::numeric_limits<double>::max();
        double sMax = std::numeric_limits<double>::lowest();
        for (const QPointF& c : corners) {
            const double s = c.x() * perp.x + c.y() * perp.y;
            sMin = std::min(sMin, s);
            sMax = std::max(sMax, s);
        }

        const double diagonal = std::hypot(bounds.width(), bounds.height());
        const Vector2D base(perp.x * sMin, perp.y * sMin);

        painter.save();
        painter.setClipPath(path);

        QVector<QLineF> lines;
        for (double s = sMin; s <= sMax; s += spacingPixels) {
            const QPointF center(base.x + perp.x * (s - sMin) + dir.x * diagonal * 0.5,
                                 base.y + perp.y * (s - sMin) + dir.y * diagonal * 0.5);
            lines.append(QLineF(center.x() - dir.x * diagonal, center.y() - dir.y * diagonal,
                                center.x() + dir.x * diagonal, center.y() + dir.y * diagonal));
        }
        painter.drawLines(lines);
        painter.restore();
    }

    painter.drawPath(path);
}

// -----------------------------------------------------------------------------
//  尺寸标注
// -----------------------------------------------------------------------------
void drawDimensionEntity(QPainter& painter, const DimensionEntity& dim,
                         const DrawContext& context)
{
    const DimensionEntity::Geometry geo = dim.geometry();
    if (!geo.valid) return;

    const QColor color = painter.pen().color();

    // 尺寸线 / 界线 / 引线
    for (const auto& seg : geo.segments) {
        const Point2D a = context.view->toDevice(seg.first);
        const Point2D b = context.view->toDevice(seg.second);
        painter.drawLine(QPointF(a.x, a.y), QPointF(b.x, b.y));
    }

    // 箭头（实心三角）
    QPainterPath arrows;
    for (const DimensionEntity::Arrow& arrow : geo.arrows) {
        const Point2D tip = context.view->toDevice(arrow.tip);
        const Point2D b1 = context.view->toDevice(arrow.base1);
        const Point2D b2 = context.view->toDevice(arrow.base2);
        arrows.moveTo(tip.x, tip.y);
        arrows.lineTo(b1.x, b1.y);
        arrows.lineTo(b2.x, b2.y);
        arrows.closeSubpath();
    }
    painter.fillPath(arrows, color);

    // 测量文字
    const double capHeightPixels = geo.textHeight * context.view->scale();
    if (geo.text.isEmpty() || capHeightPixels < 2.0) return;

    const Point2D anchor = context.view->toDevice(geo.textAnchor);
    QFont font = painter.font();
    font.setPixelSize(std::max(2, static_cast<int>(std::lround(capHeightPixels
                                                              / kCapHeightToEmRatio))));
    const QFontMetricsF metrics(font);

    painter.save();
    painter.setFont(font);
    painter.translate(anchor.x, anchor.y);
    painter.rotate(-radToDeg(geo.textAngle));
    // 文字锚点即字符串的中心（与 DXF 匿名块的居中写法一致）
    painter.drawText(QPointF(-metrics.horizontalAdvance(geo.text) * 0.5,
                             (metrics.ascent() - metrics.descent()) * 0.5),
                     geo.text);
    painter.restore();
}

} // namespace

// -----------------------------------------------------------------------------
double adaptiveSagitta(const DrawContext& context)
{
    if (!context.isValid()) return 0.1;
    return context.view->pixelsToWorld(context.settings->maxSagittaPixels);
}

QRectF deviceBounds(const Entity& entity, const ViewTransform& view)
{
    const Box2D worldBox = entity.boundingBox();
    if (!worldBox.isValid()) return QRectF();

    const Point2D a = view.toDevice(Point2D(worldBox.minX, worldBox.minY));
    const Point2D b = view.toDevice(Point2D(worldBox.maxX, worldBox.maxY));

    return QRectF(QPointF(std::min(a.x, b.x), std::min(a.y, b.y)),
                  QPointF(std::max(a.x, b.x), std::max(a.y, b.y)));
}

QPainterPath toDevicePath(const Entity& entity, const ViewTransform& view)
{
    QPainterPath path;

    const double sagitta = std::max(view.pixelsToWorld(0.3), kGeomTol);
    const std::vector<Point2D> points = entity.flatten(sagitta);
    if (points.empty()) return path;

    const Point2D first = view.toDevice(points.front());
    path.moveTo(first.x, first.y);
    for (std::size_t i = 1; i < points.size(); ++i) {
        const Point2D d = view.toDevice(points[i]);
        path.lineTo(d.x, d.y);
    }
    if (entity.isClosed()) path.closeSubpath();
    return path;
}

void drawPointMarker(QPainter& painter, const Point2D& devicePos, const QColor& color,
                     double sizePixels)
{
    const QPen saved = painter.pen();
    const QBrush savedBrush = painter.brush();

    QPen pen(color);
    pen.setWidthF(std::max(1.0, sizePixels * 0.18));
    pen.setStyle(Qt::SolidLine);
    painter.setPen(pen);
    painter.setBrush(color);

    const double half = std::max(1.5, sizePixels);
    const QPointF c(devicePos.x, devicePos.y);

    painter.drawLine(QPointF(c.x() - half, c.y()), QPointF(c.x() + half, c.y()));
    painter.drawLine(QPointF(c.x(), c.y() - half), QPointF(c.x(), c.y() + half));
    painter.drawEllipse(c, half * 0.30, half * 0.30);

    painter.setPen(saved);
    painter.setBrush(savedBrush);
}

void draw(QPainter& painter, const Entity& entity, const DrawContext& context,
          const QColor& color, double lineweightMm, const QString& linetypeName)
{
    if (!context.isValid()) return;
    if (!visibleInViewport(entity, *context.view, context.viewport)) return;

    const DisplaySettings& settings = *context.settings;

    QPen pen(color);
    pen.setWidthF(penWidthPixels(lineweightMm, context));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);

    const Qt::PenStyle style = penStyleFor(linetypeName, context);
    pen.setStyle(style);
    if (style == Qt::CustomDashLine) {
        applyCustomDash(pen, linetypes::resolve(linetypeName), context);
    }

    painter.save();
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    switch (entity.type()) {
    case EntityType::Line: {
        const auto& line = static_cast<const LineEntity&>(entity);
        const Point2D a = context.view->toDevice(line.start());
        const Point2D b = context.view->toDevice(line.end());
        painter.drawLine(QPointF(a.x, a.y), QPointF(b.x, b.y));
        break;
    }
    case EntityType::Circle: {
        const auto& circle = static_cast<const CircleEntity&>(entity);
        drawCircleEntity(painter, circle, context,
                         computeAngleWindow(context.view->toDevice(circle.center()),
                                            context.viewport));
        break;
    }
    case EntityType::Arc: {
        const auto& arc = static_cast<const ArcEntity&>(entity);
        const Point2D centerDevice = context.view->toDevice(arc.center());
        const double radiusPixels = arc.radius() * context.view->scale();

        const AngleWindow window = computeAngleWindow(centerDevice, context.viewport);
        const bool arcWindowFull = window.full;

        // 可见性与相交剔除：圆弧的角度区间必须与可见角度窗口有交集
        if (arcWindowFull) {
            // 圆心在视口内，整弧都可能可见，无需进一步剔除
        } else if (!arcsOverlap(arc.startAngle(), arc.spanAngle(), window.start, window.span)) {
            break;
        }

        if (radiusPixels >= kMaxExactRadiusPixels && !arcWindowFull) {
            painter.setClipRect(context.viewport);
        }
        drawArcEntity(painter, arc, context, window);
        break;
    }
    case EntityType::Polyline: {
        const auto& polyline = static_cast<const PolylineEntity&>(entity);
        if (polyline.hasWidth() && !polyline.hasUniformWidth()) {
            // 宽度逐段变化：无法用单一画笔表达，退化为逐段描边
            drawPolylineWithVaryingWidth(painter, polyline, context);
        } else {
            drawPolylineEntity(painter, polyline, context);
        }
        break;
    }
    case EntityType::Point: {
        const auto& point = static_cast<const PointEntity&>(entity);
        const Point2D d = context.view->toDevice(point.position());
        drawPointMarker(painter, d, color, settings.pickBoxPixels * 0.7);
        break;
    }
    case EntityType::Text: {
        drawTextEntity(painter, static_cast<const TextEntity&>(entity), context);
        break;
    }
    case EntityType::Hatch: {
        drawHatchEntity(painter, static_cast<const HatchEntity&>(entity), context);
        break;
    }
    case EntityType::Dimension: {
        drawDimensionEntity(painter, static_cast<const DimensionEntity&>(entity), context);
        break;
    }
    case EntityType::Spline: {
        const auto& spline = static_cast<const SplineEntity&>(entity);
        drawFlattenedCurve(painter, spline.flatten(adaptiveSagitta(context)),
                           spline.isClosed(), context);
        break;
    }
    case EntityType::Ellipse: {
        const auto& ellipse = static_cast<const EllipseEntity&>(entity);
        drawFlattenedCurve(painter, ellipse.flatten(adaptiveSagitta(context)),
                           ellipse.isClosed(), context);
        break;
    }
    }

    painter.restore();
}

} // namespace EntityRenderer
} // namespace lycad
