// =============================================================================
//  LyCAD - render/EntityRenderer.cpp
// =============================================================================
#include "render/EntityRenderer.h"

#include "core/ArcEntity.h"
#include "core/AciPalette.h"
#include "core/CircleEntity.h"
#include "core/DimensionEntity.h"
#include "core/EllipseEntity.h"
#include "core/HatchEntity.h"
#include "core/InfiniteLineEntity.h"
#include "core/InsertEntity.h"
#include "core/Layer.h"
#include "core/LineEntity.h"
#include "core/LightEntity.h"
#include "core/Linetype.h"
#include "core/PointEntity.h"
#include "core/PolylineEntity.h"
#include "core/SplineEntity.h"
#include "core/TableEntity.h"
#include "core/MTextEntity.h"
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

/// 视口（设备坐标）对应的世界范围。
///
/// 视图变换只含平移、等比缩放与 Y 轴翻转，因此设备下的轴对齐矩形换算到世界
/// 后仍然轴对齐——裁剪盒不需要任何旋转补偿。
Box2D viewportWorldBox(const ViewTransform& view, const QRectF& viewport)
{
    const Point2D a = view.toWorld(Point2D(viewport.left(), viewport.top()));
    const Point2D b = view.toWorld(Point2D(viewport.right(), viewport.bottom()));
    return Box2D::fromCorners(a, b);
}

/// 把给定矩形（设备坐标）内可见的那一段无界直线裁出来（世界坐标）。
/// 与视口无交集时返回空。
std::optional<std::pair<Point2D, Point2D>> clipToDeviceRect(const InfiniteLineEntity& line,
                                                            const ViewTransform& view,
                                                            const QRectF& rect)
{
    if (!rect.isValid()) return std::nullopt;
    return line.clippedTo(viewportWorldBox(view, rect));
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
            // 与 flatten 同策略：自起始角出发，按 bulge 的符号决定推进方向
            const double span = seg.spanAngle();
            const double direction = (seg.bulge >= 0.0) ? 1.0 : -1.0;
            const int steps = std::max(2, segmentCountForArc(seg.radius, span, sagitta));
            const double step = span / steps;
            for (int k = 1; k <= steps; ++k) {
                const double a = seg.startAngle + direction * step * k;
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
//  构造线 / 射线
// -----------------------------------------------------------------------------
/// 无界图元按「裁到视口」绘制：先求出与视口相交的那一段，再画这条普通线段。
/// 直接画「无穷长」既不可能也没必要——屏幕上可见的部分本来就是线段。
void drawInfiniteLine(QPainter& painter, const InfiniteLineEntity& line,
                      const DrawContext& context)
{
    const auto segment = clipToDeviceRect(line, *context.view, context.viewport);
    if (!segment) return;                     // 与本视口无交集

    const Point2D a = context.view->toDevice(segment->first);
    const Point2D b = context.view->toDevice(segment->second);
    painter.drawLine(QPointF(a.x, a.y), QPointF(b.x, b.y));
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
    //
    // 换算按 DXF 73 的语义：top/middle/bottom 量的是**字高（大写高度）框**，
    // 不是字体的行框。QFontMetrics 的 ascent/descent 含字体内部行距，直接拿它
    // 当字高会把整块文字压低约 0.1~0.4 倍字高——实测圆内的 "JK" 因此在圆里偏下
    // （AutoCAD/浩辰把字高框的上沿贴在插入点上，字正好落在圆心）。
    const double capPixels = capHeightPixels;                       // 字高（设备像素）
    const double descentPixels = capPixels * TextEntity::kDescentRatio;
    double baselineOffset = 0.0;
    switch (text.vAlign()) {
    case TextEntity::VAlign::Baseline: baselineOffset = 0.0; break;
    case TextEntity::VAlign::Bottom:   baselineOffset = -descentPixels - blockSpan; break;
    case TextEntity::VAlign::Middle:   baselineOffset = (capPixels - descentPixels - blockSpan) * 0.5; break;
    case TextEntity::VAlign::Top:      baselineOffset = capPixels; break;
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
//  多行文字
// -----------------------------------------------------------------------------
/// 绘制 MTEXT。
///
/// 排版（分行、行起点、首行基线）全部取自 MTextEntity::layout()——模型与渲染共用
/// 同一份结果，包围盒、拾取框与画出来的字形因此不会各算一套。分行只认内容里的
/// 段落分隔（\P），不依据参考栏宽（41）重新折行，与 AutoCAD / 浩辰的显示一致。
///
/// 垂直基准按 MTEXT 自身的语义：附着点的**中间行**量的是「首行小写高度顶与末行基线
/// 的中点」，不是 TEXT 的大写高度框（两者差 (capHeight − xHeight)/2 ≈ 0.13 倍字高，
/// 这也正是把 MTEXT 降级成 TEXT 会在 AutoCAD / 浩辰里整体错位的原因）。
///
/// 内联格式码按「行内片段」还原：模型（parseRuns）已把每行切成若干同格式片段
/// （`\f…|b1;` 粗体、`\f…|i1;` 斜体、`\C…;` 颜色、`\H`/`\W`/`\Q`/`\T` 字高/宽度/
/// 倾斜/字距），这里逐段设字体与颜色绘制。段起点、行宽与包围盒都取自同一份排版结果，
/// 因此屏幕上的字形位置与拾取框一致。文件里的原文一字未改，别的 CAD 仍按原样显示。
void drawMTextEntity(QPainter& painter, const MTextEntity& text,
                     const DrawContext& context)
{
    const MTextEntity::Layout laid = text.layout();
    if (!laid.valid || laid.lines.empty()) return;

    const double capHeightPixels = text.height() * context.view->scale();
    if (capHeightPixels < 2.0) {
        // 缩得太小时画一个点占位，保持「这里有文字」的可见性
        const Point2D anchor = context.view->toDevice(text.insertionPoint());
        painter.drawPoint(QPointF(anchor.x, anchor.y));
        return;
    }

    QFont baseFont = painter.font();
    baseFont.setPixelSize(std::max(2, static_cast<int>(std::lround(capHeightPixels
                                                              / kCapHeightToEmRatio))));
    // 内联字体码声明了字体时优先采用它；否则退回图元的样式名
    const QString inlineName = text.inlineFontName();
    const QString family = (!inlineName.isEmpty() && QFontDatabase::hasFamily(inlineName))
                               ? inlineName
                               : text.fontStyle();
    if (!family.isEmpty() && QFontDatabase::hasFamily(family)) {
        baseFont.setFamily(family);
    }

    // 图层/随层解析后的颜色：没有内联颜色的片段沿用它
    const QColor baseColor = painter.pen().color();
    const bool themeIsDark = context.settings->themeIsDark;

    const Point2D anchorDevice = context.view->toDevice(text.insertionPoint());
    const double scale = context.view->scale();

    painter.save();
    painter.translate(anchorDevice.x, anchorDevice.y);
    // 世界坐标逆时针为正；设备坐标 Y 轴向下，方向相反
    painter.rotate(-radToDeg(text.rotation()));

    // 局部坐标 -> 设备像素：X 沿文字方向、Y 向上（设备帧里取负）
    for (std::size_t i = 0; i < laid.lines.size(); ++i) {
        const MTextEntity::LayoutLine& line = laid.lines[i];
        if (line.text.isEmpty()) continue;
        const double y = -(laid.firstBaseline - static_cast<double>(i) * laid.lineHeight) * scale;

        // 快路径：整行没有任何内联格式（绝大多数文字）——与早期实现完全同画法
        const bool plainLine = line.runs.empty()
                               || (line.runs.size() == 1 && line.runs.front().style.isPlain());
        if (plainLine) {
            painter.setFont(baseFont);
            painter.setPen(baseColor);
            painter.drawText(QPointF(line.x * scale, y), line.text);
            continue;
        }

        // 逐段绘制：每段自带字体（粗/斜/族）、字号、颜色、宽度因子、倾斜与字距。
        // 段起点来自模型的 offsetX（与行宽用的是同一份数据），故画出来不会互相错位。
        for (const MTextEntity::Run& run : line.runs) {
            if (run.text.isEmpty()) continue;
            const MTextEntity::RunStyle& style = run.style;

            QFont font = baseFont;
            if (!style.fontFamily.isEmpty() && QFontDatabase::hasFamily(style.fontFamily)) {
                font.setFamily(style.fontFamily);
            }
            if (style.bold) font.setBold(true);
            if (style.italic) font.setItalic(true);
            if (std::abs(style.heightFactor - 1.0) > 1e-9) {
                // `\H` 改的是本段的字高（图元字高 × 倍数）
                const double pixels = capHeightPixels * style.heightFactor / kCapHeightToEmRatio;
                font.setPixelSize(std::max(2, static_cast<int>(std::lround(pixels))));
            }
            if (std::abs(style.tracking - 1.0) > 1e-3) {
                // `\T`：字距因子。附加量加在字符之间，与模型算宽度用的口径一致
                const double extra = (style.tracking - 1.0) * capHeightPixels * style.heightFactor;
                font.setLetterSpacing(QFont::AbsoluteSpacing, extra);
            }
            painter.setFont(font);
            painter.setPen(style.hasColor ? aci::toColor(style.colorAci, themeIsDark) : baseColor);

            painter.save();
            painter.translate((line.x + run.offsetX) * scale, 0.0);
            // 宽度因子（`\W`）用横向缩放实现：字形与步进同步压/拉，
            // 与模型按 widthFactor 估出来的段宽一致（QFont::setStretch 在非可变字体上会被忽略，
            // 那样画出来的宽度会与排版不符、后续段就会压在一起）。
            if (std::abs(style.widthFactor - 1.0) > 1e-9) {
                painter.scale(style.widthFactor, 1.0);
            }
            // 倾斜角（`\Q`）：设备帧 y 向下，右倾需要负的剪切系数
            if (std::abs(style.obliqueDeg) > 1e-9) {
                painter.shear(-std::tan(degToRad(style.obliqueDeg)), 0.0);
            }
            painter.drawText(QPointF(0.0, y), run.text);
            painter.restore();
        }
    }
    painter.restore();
}

// -----------------------------------------------------------------------------
//  表格
// -----------------------------------------------------------------------------
/// 单元格对齐（表格枚举）-> 文字图元的水平/垂直对齐
void mapCellAlign(TableCellAlign align, TextEntity::HAlign& h, TextEntity::VAlign& v)
{
    switch (align) {
    case TableCellAlign::TopLeft:      h = TextEntity::HAlign::Left;   v = TextEntity::VAlign::Top;    break;
    case TableCellAlign::TopCenter:    h = TextEntity::HAlign::Center; v = TextEntity::VAlign::Top;    break;
    case TableCellAlign::TopRight:     h = TextEntity::HAlign::Right;  v = TextEntity::VAlign::Top;    break;
    case TableCellAlign::MiddleLeft:   h = TextEntity::HAlign::Left;   v = TextEntity::VAlign::Middle; break;
    case TableCellAlign::MiddleCenter: h = TextEntity::HAlign::Center; v = TextEntity::VAlign::Middle; break;
    case TableCellAlign::MiddleRight:  h = TextEntity::HAlign::Right;  v = TextEntity::VAlign::Middle; break;
    case TableCellAlign::BottomLeft:   h = TextEntity::HAlign::Left;   v = TextEntity::VAlign::Bottom; break;
    case TableCellAlign::BottomCenter: h = TextEntity::HAlign::Center; v = TextEntity::VAlign::Bottom; break;
    case TableCellAlign::BottomRight:  h = TextEntity::HAlign::Right;  v = TextEntity::VAlign::Bottom; break;
    }
}

void drawTableEntity(QPainter& painter, const TableEntity& table,
                     const DrawContext& context, const QColor& color)
{
    const std::vector<double> rows = table.rowBoundaries();
    const std::vector<double> cols = table.columnBoundaries();
    const std::size_t rowCount = table.rowCount();
    const std::size_t colCount = table.columnCount();
    if (rowCount == 0 || colCount == 0) return;

    // 网格线：逐段画（合并区内部已被 TableEntity 擦除），颜色沿用图元自身颜色。
    // 用稍细的 1px 画笔，避免线宽把单元格文字挤没。
    QPen gridPen(color);
    gridPen.setWidthF(1.0);
    gridPen.setCosmetic(true);
    painter.setPen(gridPen);
    painter.setBrush(Qt::NoBrush);

    // 直接把每个矩形四条边逐条绘制：相邻单元格的公共边会重复画一次，
    // 但对 1px 相似色描边而言视觉无差别，换来的是不必处理合并区的擦除逻辑
    // 之外的额外边界情况。合并区内部边由 TableEntity 提供的外框表达。
    for (std::size_t r = 0; r < rowCount; ++r) {
        for (std::size_t c = 0; c < colCount; ++c) {
            // 若该单元格是合并区的一员且不是左上角，则跳过其内部网格
            const auto merge = table.mergeContaining(r, c);
            if (merge && (static_cast<int>(r) != merge->row
                          || static_cast<int>(c) != merge->col)) {
                continue;
            }

            int rEnd = static_cast<int>(r);
            int cEnd = static_cast<int>(c);
            if (merge) {
                rEnd = merge->row + merge->rowSpan - 1;
                cEnd = merge->col + merge->colSpan - 1;
            }
            if (static_cast<std::size_t>(rEnd) >= rowCount) rEnd = static_cast<int>(rowCount) - 1;
            if (static_cast<std::size_t>(cEnd) >= colCount) cEnd = static_cast<int>(colCount) - 1;

            const Point2D lt = context.view->toDevice(table.toWorld(Point2D(cols[c], rows[r])));
            const Point2D rb = context.view->toDevice(table.toWorld(Point2D(cols[cEnd + 1], rows[rEnd + 1])));

            painter.drawLine(QPointF(lt.x, lt.y), QPointF(rb.x, lt.y));
            painter.drawLine(QPointF(rb.x, lt.y), QPointF(rb.x, rb.y));
            painter.drawLine(QPointF(rb.x, rb.y), QPointF(lt.x, rb.y));
            painter.drawLine(QPointF(lt.x, rb.y), QPointF(lt.x, lt.y));
        }
    }

    // 单元格文字：字高取所在行高的 0.5，居中对齐（除非单元格显式指定对齐）
    for (std::size_t r = 0; r < rowCount; ++r) {
        for (std::size_t c = 0; c < colCount; ++c) {
            const TableCell cell = table.cell(r, c);
            if (cell.text.isEmpty()) continue;

            // 合并区：只有左上角单元格显示文字，且用整个合并区的中心定位
            const auto merge = table.mergeContaining(r, c);
            int rEnd = static_cast<int>(r);
            int cEnd = static_cast<int>(c);
            if (merge) {
                if (static_cast<int>(r) != merge->row || static_cast<int>(c) != merge->col) continue;
                rEnd = std::min(merge->row + merge->rowSpan - 1, static_cast<int>(rowCount) - 1);
                cEnd = std::min(merge->col + merge->colSpan - 1, static_cast<int>(colCount) - 1);
            }

            const double cx = 0.5 * (cols[c] + cols[cEnd + 1]);
            const double cy = 0.5 * (rows[r] + rows[rEnd + 1]);

            const double rowHeight = rows[r] - rows[rEnd + 1];
            const double textHeight = std::max(rowHeight * 0.5, 1e-6);

            TextEntity label(cell.text, table.toWorld(Point2D(cx, cy)), textHeight,
                             table.rotation());
            TextEntity::HAlign hAlign = TextEntity::HAlign::Center;
            TextEntity::VAlign vAlign = TextEntity::VAlign::Middle;
            mapCellAlign(cell.align, hAlign, vAlign);
            label.setHAlign(hAlign);
            label.setVAlign(vAlign);

            drawTextEntity(painter, label, context);
        }
    }
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
    // 无界图元的设备范围取决于裁剪矩形，单看变换无法给出——交由绘制路径负责
    if (entity.isUnbounded()) return QRectF();

    const Box2D worldBox = entity.boundingBox();
    if (!worldBox.isValid()) return QRectF();

    const Point2D a = view.toDevice(Point2D(worldBox.minX, worldBox.minY));
    const Point2D b = view.toDevice(Point2D(worldBox.maxX, worldBox.maxY));

    return QRectF(QPointF(std::min(a.x, b.x), std::min(a.y, b.y)),
                  QPointF(std::max(a.x, b.x), std::max(a.y, b.y)));
}

QPainterPath toDevicePath(const Entity& entity, const ViewTransform& view, const QRectF& clip)
{
    QPainterPath path;

    // 块参照：由若干**互不相连**的子图元组成，必须各自成一条子路径。
    // 走下面的 flatten 会把「上一个子图元的末点」与「下一个的首点」连起来，
    // 选择高亮与拖拽预览里会凭空多出几条斜线。
    if (entity.type() == EntityType::Insert) {
        const auto& insert = static_cast<const InsertEntity&>(entity);
        QPainterPath combined;
        for (const std::unique_ptr<Entity>& child : insert.expanded()) {
            if (!child) continue;
            combined.addPath(toDevicePath(*child, view, clip));
        }
        return combined;
    }

    // 无界图元：形状 = 裁到给定矩形的线段。isUnbounded() 为真的只有
    // InfiniteLineEntity 一族，因此这里的向下转换是安全的。
    if (entity.isUnbounded()) {
        const auto* line = static_cast<const InfiniteLineEntity*>(&entity);
        const auto segment = clipToDeviceRect(*line, view, clip);
        if (!segment) return path;

        const Point2D a = view.toDevice(segment->first);
        const Point2D b = view.toDevice(segment->second);
        path.moveTo(a.x, a.y);
        path.lineTo(b.x, b.y);
        return path;
    }

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

// -----------------------------------------------------------------------------
//  光源
// -----------------------------------------------------------------------------
/// 绘制光源的屏幕标记（LIGHT 本体没有几何）。
///
/// 与 AutoCAD / 浩辰 / 中望 同一约定：标记是**固定像素尺寸**的——放大图纸时它不会
/// 跟着变大（否则 1:100 的图里一个光标记会涨成一团墨）。画法按类型区分：
///   · 点光源  —— 圆 + 八向光芒
///   · 聚光灯  —— 圆 + 光芒 + 指向目标的圆锥（开口朝目标）
///   · 光域网  —— 同聚光灯
///   · 平行光  —— 圆 + 光芒 + 带箭头的方向线
/// 已关闭的光源（DXF 290 = 0）画成虚线，与 AutoCAD 的灰显语义一致。
void drawLightEntity(QPainter& painter, const LightEntity& light, const DrawContext& context)
{
    const Point2D device = context.view->toDevice(light.position());
    const double r = LightEntity::kGlyphRadiusPixels;
    const QPointF c(device.x, device.y);

    // 剔除：标记很小，按外接方框判可见即可
    const double pad = r * 4.0;
    if (!context.viewport.adjusted(-pad, -pad, pad, pad).contains(c)) return;

    const QPen savedPen = painter.pen();
    QPen pen(savedPen);
    pen.setStyle(light.isOn() ? Qt::SolidLine : Qt::DashLine);
    pen.setWidthF(std::max(1.0, pen.widthF()));
    painter.setPen(pen);

    painter.drawEllipse(c, r, r);

    constexpr double kPi = 3.14159265358979323846;
    for (int i = 0; i < 8; ++i) {
        const double angle = static_cast<double>(i) * kPi / 4.0;
        const double ca = std::cos(angle);
        const double sa = std::sin(angle);
        painter.drawLine(QPointF(c.x() + ca * r * 1.30, c.y() + sa * r * 1.30),
                         QPointF(c.x() + ca * r * 2.00, c.y() + sa * r * 2.00));
    }

    if (light.lightType() == LightEntity::LightType::Point) {
        painter.setPen(savedPen);
        return;
    }

    // 方向：设备坐标下「位置 -> 目标」的单位向量（目标可能远在图外，只取方向）
    const Point2D targetDevice = context.view->toDevice(light.target());
    const double vx = targetDevice.x - device.x;
    const double vy = targetDevice.y - device.y;
    const double len = std::hypot(vx, vy);
    if (len > 1e-9) {
        const double dx = vx / len;
        const double dy = vy / len;
        const double nx = -dy;                 // 方向线的法向
        const double ny = dx;

        if (light.lightType() == LightEntity::LightType::Distant) {
            const QPointF from(c.x() + dx * r * 1.20, c.y() + dy * r * 1.20);
            const QPointF tip(c.x() + dx * r * 3.60, c.y() + dy * r * 3.60);
            painter.drawLine(from, tip);
            const double head = r * 0.85;
            painter.drawLine(tip, QPointF(tip.x() - dx * head + nx * head * 0.55,
                                          tip.y() - dy * head + ny * head * 0.55));
            painter.drawLine(tip, QPointF(tip.x() - dx * head - nx * head * 0.55,
                                          tip.y() - dy * head - ny * head * 0.55));
        } else {
            // 圆锥：顶点在圆边上，两条母线 + 底边弧（弧用折线逼近——Qt 的 drawArc
            // 角度约定在 y 轴向下的设备坐标里容易绕错，这里宁可用明确的折线）
            const QPointF apex(c.x() + dx * r * 1.10, c.y() + dy * r * 1.10);
            const double far = r * 2.40;
            const double spread = r * 1.00;
            const double theta = std::atan2(dy, dx);
            const double phi = std::atan2(spread, far);      // 锥的半角

            constexpr int kSegments = 8;
            QPointF previous(apex.x() + std::cos(theta - phi) * far,
                             apex.y() + std::sin(theta - phi) * far);
            painter.drawLine(apex, previous);
            for (int k = 1; k <= kSegments; ++k) {
                const double angle = theta - phi + 2.0 * phi * static_cast<double>(k) / kSegments;
                const QPointF current(apex.x() + std::cos(angle) * far,
                                      apex.y() + std::sin(angle) * far);
                painter.drawLine(previous, current);
                previous = current;
            }
            painter.drawLine(apex, previous);
        }
    }
    painter.setPen(savedPen);
}

void draw(QPainter& painter, const Entity& entity, const DrawContext& context,
          const QColor& color, double lineweightMm, const QString& linetypeName)
{
    if (!context.isValid()) return;
    // 无界图元（构造线、射线）没有包围盒，剔除必须改用「与本视口是否相交」，
    // 这一步在无界分支内部完成（见 drawInfiniteLine）。
    if (!entity.isUnbounded() && !visibleInViewport(entity, *context.view, context.viewport)) {
        return;
    }

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
    case EntityType::MText: {
        drawMTextEntity(painter, static_cast<const MTextEntity&>(entity), context);
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
    case EntityType::XLine:
    case EntityType::Ray: {
        drawInfiniteLine(painter, static_cast<const InfiniteLineEntity&>(entity), context);
        break;
    }
    case EntityType::Insert: {
        // 块参照自己没有几何：把展开后的子图元逐个交给同一个绘制函数，每个
        // 子图元用**它自己**的图层与样式（随层/随块已在展开阶段解析完毕）。
        //
        // 刻意不用 painter 变换去套整块内容：本文件里所有像素量——线宽
        // （penWidthPixels）、虚线节距、文字像素高、填充间距、视口剔除——
        // 都是按未变换的 view 算出来的，套上 painter 变换后它们会随块内比例
        // 一起被缩放，块内缩得越小线越细、字越小，与块外不是一套规则。
        const auto& insert = static_cast<const InsertEntity&>(entity);
        if (context.layers == nullptr) break;

        for (const std::unique_ptr<Entity>& child : insert.expanded()) {
            if (!child || !child->isDisplayable(*context.layers)) continue;

            const QColor childColor = child->resolvedColor(*context.layers, settings.themeIsDark);
            const double childLineweight =
                child->resolvedLineweight(*context.layers) * context.lineweightWorldScale;

            QString childLinetype = child->linetypeName();
            if (childLinetype.isEmpty()) {
                childLinetype = context.layers->linetypeOf(child->layerName(),
                                                           QStringLiteral("CONTINUOUS"));
            }

            draw(painter, *child, context, childColor, childLineweight, childLinetype);
        }
        break;
    }
    case EntityType::Table: {
        drawTableEntity(painter, static_cast<const TableEntity&>(entity), context, color);
        break;
    }
    case EntityType::Light: {
        drawLightEntity(painter, static_cast<const LightEntity&>(entity), context);
        break;
    }
    }

    painter.restore();
}

} // namespace EntityRenderer
} // namespace lycad
