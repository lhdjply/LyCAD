// =============================================================================
//  LyCAD - tests/test_geometry.cpp
//  几何模型与坐标解析的单元测试。
// =============================================================================
#include <QtTest>

#include "core/ArcEntity.h"
#include "core/CircleEntity.h"
#include "core/DimensionEntity.h"
#include "core/EllipseEntity.h"
#include "core/HatchEntity.h"
#include "core/InfiniteLineEntity.h"
#include "core/LineEntity.h"
#include "core/PolylineEntity.h"
#include "core/RayEntity.h"
#include "core/TextEntity.h"
#include "core/Types.h"
#include "core/XLineEntity.h"
#include "interact/CoordinateInput.h"

using namespace lycad;

class TestGeometry : public QObject {
    Q_OBJECT

private slots:
    // ---- 变换 ---------------------------------------------------------------
    void transformCompositionOrder();
    void transformInverseRoundTrip();
    void mirrorIsItsOwnInverse();

    // ---- 基础几何 -----------------------------------------------------------
    void boxAccumulation();
    void pointToSegmentDistance();

    // ---- 图元 ---------------------------------------------------------------
    void lineBasics();
    void circleBoundsAndCrossing();
    void arcBoundingBoxCoversQuadrants();
    void arcFromThreePointsDeterminesDirection();
    void arcMirrorReversesOrientation();

    // ---- 多段线与凸度 -------------------------------------------------------
    void polylineBulgeSemicircle();
    void polylineClockwiseBulgeIsNotMirrored();
    void polylineMirrorFlipsBulgeSign();
    void rectangleIsClosedPolyline();
    void polylineWidthSemantics();

    // ---- 椭圆 ---------------------------------------------------------------
    void ellipseHalfAxesNormalization();
    void ellipseBoundsAndSnapPoints();

    // ---- 构造线 / 射线（无界图元）--------------------------------------------
    void infiniteLineDirectionIsNormalized();
    void infiniteLineClippingRespectsDirection();
    void infiniteLineDistanceAndCrossing();
    void infiniteLineBoundingBoxIsUndefined();
    void infiniteLineTransformAndGrips();

    // ---- 文字 / 填充 / 标注 ---------------------------------------------------
    void textBoundingBoxAndAlignment();
    void hatchContainmentAndArea();
    void hatchTransformMovesBoundary();
    void hatchBulgeFormsCircle();
    void dimensionMeasurementAllTypes();
    void dimensionGeometryDerivation();
    void dimensionGripsReflowGeometry();
    void dimensionTypeNameIsNotMojibake();

    // ---- 坐标解析 -----------------------------------------------------------
    void coordinateParsing();
};

// -----------------------------------------------------------------------------
void TestGeometry::transformCompositionOrder()
{
    // (A*B) 的语义为「先作用 B，再作用 A」
    const Transform2D transform = Transform2D::translation(10.0, 0.0)
                                * Transform2D::rotation(kHalfPi);

    const Point2D result = transform.apply(Point2D(1.0, 0.0));

    // 先逆时针旋转 90°：(1,0) -> (0,1)；再平移 (10,0) -> (10,1)
    QVERIFY(std::abs(result.x - 10.0) < 1e-9);
    QVERIFY(std::abs(result.y - 1.0) < 1e-9);
}

void TestGeometry::transformInverseRoundTrip()
{
    const Transform2D transform = Transform2D::translation(3.5, -2.5)
                                * Transform2D::rotation(degToRad(37.0))
                                * Transform2D::scaling(2.5, 2.5);

    const auto inverse = transform.inverted();
    QVERIFY(inverse.has_value());

    const Point2D original(12.25, -7.75);
    const Point2D restored = inverse->apply(transform.apply(original));

    QVERIFY(std::abs(restored.x - original.x) < 1e-9);
    QVERIFY(std::abs(restored.y - original.y) < 1e-9);
}

void TestGeometry::mirrorIsItsOwnInverse()
{
    const Point2D a(1.0, 2.0);
    const Point2D b(4.0, 6.0);
    const Transform2D mirror = Transform2D::mirror(a, b);

    const Point2D p(5.0, -1.0);
    const Point2D twice = mirror.apply(mirror.apply(p));

    QVERIFY(std::abs(twice.x - p.x) < 1e-9);
    QVERIFY(std::abs(twice.y - p.y) < 1e-9);

    // 镜像的行列式为负，是判定方向反转的依据
    QVERIFY(mirror.determinant() < 0.0);
    QVERIFY(mirror.isMirroring());
}

// -----------------------------------------------------------------------------
void TestGeometry::boxAccumulation()
{
    Box2D box;
    QVERIFY(!box.isValid());

    box.expand(Point2D(1.0, 1.0));
    box.expand(Point2D(-3.0, 5.0));

    QVERIFY(box.isValid());
    QVERIFY(std::abs(box.minX + 3.0) < 1e-12);
    QVERIFY(std::abs(box.maxY - 5.0) < 1e-12);
    QVERIFY(std::abs(box.width() - 4.0) < 1e-12);
    QVERIFY(std::abs(box.height() - 4.0) < 1e-12);

    QVERIFY(box.contains(Point2D(0.0, 3.0)));
    QVERIFY(!box.contains(Point2D(0.0, 0.0)));   // y 落在 [1,5] 之外
    QVERIFY(!box.contains(Point2D(10.0, 0.0)));

    // 与另一个矩形部分重叠
    QVERIFY(box.intersects(Box2D::fromCorners(Point2D(-4.0, -4.0), Point2D(0.0, 2.0))));
    // x 区间有交但 y 区间完全分离
    QVERIFY(!box.intersects(Box2D::fromCorners(Point2D(-4.0, -4.0), Point2D(0.0, 0.0))));
}

void TestGeometry::pointToSegmentDistance()
{
    const Point2D a(0.0, 0.0);
    const Point2D b(10.0, 0.0);

    QVERIFY(std::abs(distancePointToSegment(Point2D(5.0, 3.0), a, b) - 3.0) < 1e-12);

    // 投影落在延长线上时，距离取到最近端点的距离
    QVERIFY(std::abs(distancePointToSegment(Point2D(-4.0, 3.0), a, b) - 5.0) < 1e-12);
}

// -----------------------------------------------------------------------------
void TestGeometry::lineBasics()
{
    LineEntity line(Point2D(0.0, 0.0), Point2D(3.0, 4.0));

    QVERIFY(std::abs(line.length() - 5.0) < 1e-12);
    QCOMPARE(line.type(), EntityType::Line);

    const Box2D box = line.boundingBox();
    QVERIFY(std::abs(box.maxX - 3.0) < 1e-12);
    QVERIFY(std::abs(box.maxY - 4.0) < 1e-12);

    // 端点在线上，距离应为 0
    QVERIFY(line.distanceTo(Point2D(0.0, 0.0)) < 1e-12);
    QVERIFY(std::abs(line.distanceTo(Point2D(0.0, 2.0)) - 1.2) < 1e-12);

    // 中点夹点应可拖拽
    auto edited = std::unique_ptr<Entity>(line.clone());
    QVERIFY(edited->moveGrip(0, Point2D(-1.0, 0.0)));
    QVERIFY(std::abs(static_cast<LineEntity&>(*edited).length() - std::hypot(4.0, 4.0)) < 1e-9);
}

void TestGeometry::circleBoundsAndCrossing()
{
    CircleEntity circle(Point2D(5.0, 5.0), 2.0);

    const Box2D box = circle.boundingBox();
    QVERIFY(std::abs(box.minX - 3.0) < 1e-12);
    QVERIFY(std::abs(box.maxY - 7.0) < 1e-12);

    QVERIFY(std::abs(circle.length() - kTwoPi * 2.0) < 1e-9);
    QVERIFY(std::abs(circle.distanceTo(Point2D(5.0, 0.0)) - 3.0) < 1e-12);

    // 圆周与一个横跨圆环的细长框相交
    QVERIFY(circle.crosses(Box2D::fromCorners(Point2D(2.0, 4.9), Point2D(4.0, 5.1))));

    // 完全位于圆内部的小框不与圆周相交
    QVERIFY(!circle.crosses(Box2D::fromCorners(Point2D(4.8, 4.8), Point2D(5.2, 5.2))));

    // 完全位于圆外部且不接触
    QVERIFY(!circle.crosses(Box2D::fromCorners(Point2D(20.0, 20.0), Point2D(21.0, 21.0))));

    // 圆心夹点拖拽平移整圆
    auto edited = std::unique_ptr<Entity>(circle.clone());
    QVERIFY(edited->moveGrip(0, Point2D(0.0, 0.0)));
    QCOMPARE(static_cast<CircleEntity&>(*edited).center().x, 0.0);
    QVERIFY(std::abs(static_cast<CircleEntity&>(*edited).radius() - 2.0) < 1e-12);
}

void TestGeometry::arcBoundingBoxCoversQuadrants()
{
    // 自 0° 逆时针扫到 180°：包围盒的 x 取到 ±R，y 只到 +R
    ArcEntity arc(Point2D(0.0, 0.0), 2.0, 0.0, kPi);

    const Box2D box = arc.boundingBox();
    QVERIFY(std::abs(box.minX + 2.0) < 1e-9);
    QVERIFY(std::abs(box.maxX - 2.0) < 1e-9);
    QVERIFY(std::abs(box.minY - 0.0) < 1e-9);
    QVERIFY(std::abs(box.maxY - 2.0) < 1e-9);

    QVERIFY(std::abs(arc.spanAngle() - kPi) < 1e-12);
    QVERIFY(std::abs(arc.length() - kPi * 2.0) < 1e-9);

    // 起点、终点、弧中点
    QVERIFY(arc.startPoint().distanceTo(Point2D(2.0, 0.0)) < 1e-9);
    QVERIFY(arc.endPoint().distanceTo(Point2D(-2.0, 0.0)) < 1e-9);
    QVERIFY(arc.middlePoint().distanceTo(Point2D(0.0, 2.0)) < 1e-9);
}

void TestGeometry::arcFromThreePointsDeterminesDirection()
{
    // 起点 (1,0)、弧上点 (0,1)、终点 (-1,0)：逆时针的 180° 弧
    const auto arc = ArcEntity::fromThreePoints(Point2D(1.0, 0.0),
                                                Point2D(0.0, 1.0),
                                                Point2D(-1.0, 0.0));
    QVERIFY(arc.has_value());
    QVERIFY(arc->center().distanceTo(Point2D(0.0, 0.0)) < 1e-9);
    QVERIFY(std::abs(arc->radius() - 1.0) < 1e-9);
    QVERIFY(std::abs(arc->spanAngle() - kPi) < 1e-9);

    // 同样的三点顺序但弧上点换到下方：方向应反转，仍为 180° 弧
    const auto flipped = ArcEntity::fromThreePoints(Point2D(1.0, 0.0),
                                                    Point2D(0.0, -1.0),
                                                    Point2D(-1.0, 0.0));
    QVERIFY(flipped.has_value());
    QVERIFY(std::abs(flipped->spanAngle() - kPi) < 1e-9);

    // 三点共线无法确定圆弧
    const auto degenerate = ArcEntity::fromThreePoints(Point2D(0.0, 0.0),
                                                       Point2D(1.0, 0.0),
                                                       Point2D(2.0, 0.0));
    QVERIFY(!degenerate.has_value());
}

void TestGeometry::arcMirrorReversesOrientation()
{
    // 自 (1,0) 逆时针到 (0,1)
    ArcEntity arc(Point2D(0.0, 0.0), 1.0, 0.0, kHalfPi);

    // 关于世界 X 轴镜像
    arc.transform(Transform2D::mirror(Point2D(0.0, 0.0), Point2D(1.0, 0.0)));

    QVERIFY(arc.center().distanceTo(Point2D(0.0, 0.0)) < 1e-9);
    QVERIFY(std::abs(arc.radius() - 1.0) < 1e-9);
    QVERIFY(std::abs(arc.spanAngle() - kHalfPi) < 1e-9);

    // 镜像把逆时针弧变成顺时针弧。以逆时针参数化表达同一段几何时，
    // 起点与终点互换：(1,0) 成为终点，(0,-1) 成为起点。
    QVERIFY(arc.startPoint().distanceTo(Point2D(0.0, -1.0)) < 1e-9);
    QVERIFY(arc.endPoint().distanceTo(Point2D(1.0, 0.0)) < 1e-9);
}

// -----------------------------------------------------------------------------
void TestGeometry::polylineBulgeSemicircle()
{
    // 凸度 1 = 半圆。逆时针弧自 (0,0) 到 (2,0)，
    // 圆心应在弦中点 (1,0)，弧体位于行进方向的右侧（y 为负）。
    PolylineEntity polyline;
    polyline.appendVertex(Point2D(0.0, 0.0), 1.0);
    polyline.appendVertex(Point2D(2.0, 0.0));

    QCOMPARE(polyline.vertexCount(), std::size_t(2));
    QCOMPARE(polyline.segmentCount(), std::size_t(1));

    const auto segment = polyline.segment(0);
    QVERIFY(segment.has_value());
    QVERIFY(segment->isArc);
    QVERIFY(segment->center.distanceTo(Point2D(1.0, 0.0)) < 1e-9);
    QVERIFY(std::abs(segment->radius - 1.0) < 1e-9);

    QVERIFY(std::abs(polyline.length() - kPi) < 1e-9);
    QVERIFY(segment->middlePoint().distanceTo(Point2D(1.0, -1.0)) < 1e-9);

    const Box2D box = polyline.boundingBox();
    QVERIFY(std::abs(box.minY + 1.0) < 1e-9);
    QVERIFY(std::abs(box.maxY - 0.0) < 1e-9);
    QVERIFY(std::abs(box.maxX - 2.0) < 1e-9);

    // 离散点应落在半圆上
    const std::vector<Point2D> points = polyline.flatten(1e-3);
    QVERIFY(points.size() > 8);
    for (const Point2D& p : points) {
        QVERIFY(std::abs(p.distanceTo(Point2D(1.0, 0.0)) - 1.0) < 1e-6);
    }
}

// 顺时针圆弧（负凸度）的圆心必须在弦的**右侧**。
//
// 这里曾用一个看似等价的式子算圆心：h = R·cos(θ/2)。半径是按 |sin| 算的，
// 而 cos 是偶函数，于是负 θ 时 h 符号相反——圆心落到弦的另一侧，画出来正好是
// 正确圆弧的补弧：顺时针 90° 的圆角变成 270° 的大弧，看上去就是「圆弧反了」
// （PCB 图纸里电容/器件的圆角、键帽轮廓最容易看出来）。
// 半圆与逆时针圆弧都不受影响，所以这个错误只在部分圆弧上露头。
void TestGeometry::polylineClockwiseBulgeIsNotMirrored()
{
    // 自 (0,0) 到 (1,0) 的顺时针 90° 圆弧：bulge = tan(θ/4) = tan(-22.5°)
    // （PCB 图纸里器件的圆角就是这个形状，bulge ≈ -0.414）
    PolylineEntity polyline;
    polyline.appendVertex(Point2D(0.0, 0.0), -std::tan(kPi / 8.0));
    polyline.appendVertex(Point2D(1.0, 0.0));

    const auto segment = polyline.segment(0);
    QVERIFY(segment.has_value());
    QVERIFY(segment->isArc);

    // θ = -90°，R = 弦长 / (2·sin(45°))
    const double radius = 1.0 / (2.0 * std::sin(kPi / 4.0));
    QVERIFY(std::abs(segment->radius - radius) < 1e-9);

    // 圆心在弦的右侧（y 为负）：到弦中点的有向距离 = (L/2)·cot(θ/2)
    const double expectedOffset = 0.5 / std::tan(-kPi / 4.0);
    QVERIFY2(segment->center.distanceTo(Point2D(0.5, expectedOffset)) < 1e-9,
             qPrintable(QStringLiteral("圆心 (\%1, \%2)，期望 (0.5, \%3)")
                            .arg(segment->center.x).arg(segment->center.y)
                            .arg(expectedOffset)));

    // 弧顶在弦上方 0.2071（顺时针弧朝上鼓）；若圆心算反，弧顶会跑到下方
    const Point2D middle = segment->middlePoint();
    QVERIFY(std::abs(middle.y - (expectedOffset + radius)) < 1e-9);
    QVERIFY(middle.y > 0.0);

    // 包围盒必须罩住真实圆弧，而不是它的补弧
    const Box2D box = polyline.boundingBox();
    QVERIFY(std::abs(box.minY - 0.0) < 1e-9);
    QVERIFY(std::abs(box.maxY - (expectedOffset + radius)) < 1e-9);
    QVERIFY(std::abs(box.minX - 0.0) < 1e-9);
    QVERIFY(std::abs(box.maxX - 1.0) < 1e-9);

    // 弧长 = R·|θ|；起止点仍落在弦的两端
    QVERIFY(std::abs(polyline.length() - radius * kPi / 2.0) < 1e-9);

    const std::vector<Point2D> points = polyline.flatten(1e-4);
    QVERIFY(points.size() > 4);
    QVERIFY(points.front().distanceTo(Point2D(0.0, 0.0)) < 1e-9);
    QVERIFY(points.back().distanceTo(Point2D(1.0, 0.0)) < 1e-9);
    for (const Point2D& p : points) {
        QVERIFY(std::abs(p.distanceTo(segment->center) - radius) < 1e-6);
    }

    // 多段线的链条不能断：把直线段接在圆弧后面，离散点必须仍按顶点顺序前进。
    // （曾经「交换起止角再逆时针走」的写法会让圆弧段的点序从终点折回起点，
    //  于是相邻两段之间会多出一条横穿的直线。）
    {
        PolylineEntity chained;
        chained.appendVertex(Point2D(0.0, 0.0), -std::tan(kPi / 8.0));
        chained.appendVertex(Point2D(1.0, 0.0));
        chained.appendVertex(Point2D(1.0, -1.0));

        const std::vector<Point2D> path = chained.flatten(1e-4);
        QVERIFY(path.size() > 5);
        QVERIFY(path.front().distanceTo(Point2D(0.0, 0.0)) < 1e-9);
        QVERIFY(path.back().distanceTo(Point2D(1.0, -1.0)) < 1e-9);

        double walked = 0.0;
        for (std::size_t i = 1; i < path.size(); ++i) {
            walked += path[i - 1].distanceTo(path[i]);
        }
        QVERIFY2(std::abs(walked - chained.length()) < 1e-3,
                 qPrintable(QStringLiteral("离散路径长度 %1，折线长度 %2（链条断开）")
                                .arg(walked).arg(chained.length())));
    }

    // 离散点必须沿顺时针方向自起点走到终点（总转角为 -90°）
    const Vector2D startAnglePoint = points.front() - segment->center;
    const Vector2D endAnglePoint = points.back() - segment->center;
    double sweep = std::atan2(endAnglePoint.y, endAnglePoint.x)
                   - std::atan2(startAnglePoint.y, startAnglePoint.x);
    while (sweep > kPi) sweep -= 2.0 * kPi;
    while (sweep < -kPi) sweep += 2.0 * kPi;
    QVERIFY2(std::abs(sweep + kPi / 2.0) < 1e-6,
             qPrintable(QStringLiteral("实际跨度 %1°，期望 -90°")
                            .arg(sweep * 180.0 / kPi)));
}

void TestGeometry::polylineMirrorFlipsBulgeSign()
{
    PolylineEntity polyline;
    polyline.appendVertex(Point2D(0.0, 0.0), 1.0);
    polyline.appendVertex(Point2D(2.0, 0.0));

    // 关于 X 轴镜像后，圆弧应翻到另一侧，凸度符号必须随之取反
    polyline.transform(Transform2D::mirror(Point2D(0.0, 0.0), Point2D(1.0, 0.0)));

    QVERIFY(polyline.vertex(0).bulge < 0.0);

    const Box2D box = polyline.boundingBox();
    QVERIFY(std::abs(box.maxY - 1.0) < 1e-9);
    QVERIFY(std::abs(box.minY - 0.0) < 1e-9);

    // 长度不随镜像改变
    QVERIFY(std::abs(polyline.length() - kPi) < 1e-9);
}

void TestGeometry::rectangleIsClosedPolyline()
{
    const PolylineEntity rectangle = PolylineEntity::makeRectangle(Point2D(0.0, 0.0),
                                                                   Point2D(4.0, 3.0));

    QCOMPARE(rectangle.vertexCount(), std::size_t(4));
    QVERIFY(rectangle.isClosed());
    QCOMPARE(rectangle.segmentCount(), std::size_t(4));
    QVERIFY(std::abs(rectangle.perimeter() - 14.0) < 1e-9);

    const Box2D box = rectangle.boundingBox();
    QVERIFY(std::abs(box.width() - 4.0) < 1e-12);
    QVERIFY(std::abs(box.height() - 3.0) < 1e-12);

    // 窗选判定：矩形完全在框内
    QVERIFY(rectangle.isInside(Box2D::fromCorners(Point2D(-1.0, -1.0), Point2D(5.0, 4.0))));
    // 交叉选判定：框切开矩形
    QVERIFY(rectangle.crosses(Box2D::fromCorners(Point2D(2.0, 1.0), Point2D(6.0, 2.0))));
    // 框完全位于矩形内部，不与边相交
    QVERIFY(!rectangle.crosses(Box2D::fromCorners(Point2D(1.0, 1.0), Point2D(2.0, 2.0))));
}

// -----------------------------------------------------------------------------
void TestGeometry::polylineWidthSemantics()
{
    PolylineEntity track;
    track.appendVertex(Point2D(0.0, 0.0));
    track.appendVertex(Point2D(10.0, 0.0));
    track.appendVertex(Point2D(10.0, 10.0));

    QVERIFY(!track.hasWidth());
    QCOMPARE(track.distanceTo(Point2D(5.0, 0.3)), 0.3);

    // 恒定宽度：拾取按宽度命中，包围盒按半宽外扩
    track.setConstantWidth(2.0);
    QVERIFY(track.hasWidth());
    QVERIFY(track.hasUniformWidth());
    QCOMPARE(track.maxWidth(), 2.0);
    QCOMPARE(track.segmentWidth(0), 2.0);

    // 线内点命中距离为 0（原中心线距离 0.3 < 半宽 1.0）
    QCOMPARE(track.distanceTo(Point2D(5.0, 0.3)), 0.0);
    // 线外点距离扣除半宽
    QVERIFY(std::abs(track.distanceTo(Point2D(5.0, 3.0)) - 2.0) < 1e-9);

    const Box2D box = track.boundingBox();
    QVERIFY(std::abs(box.minX - (-1.0)) < 1e-9);
    QVERIFY(std::abs(box.maxY - 11.0) < 1e-9);

    // 等比缩放：宽度同步缩放
    track.transform(Transform2D::scalingAround(Point2D(0.0, 0.0), 2.0, 2.0));
    QVERIFY(std::abs(track.maxWidth() - 4.0) < 1e-9);

    // 逐顶点宽度：宽度不均匀时渲染走逐段通道
    PolylineEntity taper;
    taper.appendVertex(Point2D(0.0, 0.0));
    taper.appendVertex(Point2D(10.0, 0.0));
    taper.setVertexWidths(0, 1.0, 3.0);
    QVERIFY(taper.hasWidth());
    QVERIFY(!taper.hasUniformWidth());
    QCOMPARE(taper.segmentWidth(0), 3.0);
    // 变宽段的包围盒仍按最大宽度外扩
    const Box2D taperBox = taper.boundingBox();
    QVERIFY(std::abs(taperBox.minY - (-1.5)) < 1e-9);
    QVERIFY(std::abs(taperBox.maxY - 1.5) < 1e-9);
}

// -----------------------------------------------------------------------------
//  椭圆
// -----------------------------------------------------------------------------
void TestGeometry::ellipseHalfAxesNormalization()
{
    // 先画的那条轴是长轴：主轴就是它，比值 = 短/长
    EllipseEntity::HalfAxes axes =
        EllipseEntity::normalizeHalfAxes(Vector2D(50.0, 0.0), 30.0);
    QVERIFY(std::abs(axes.majorAxisEnd.x - 50.0) < 1e-12);
    QVERIFY(std::abs(axes.majorAxisEnd.y) < 1e-12);
    QVERIFY(std::abs(axes.ratio - 0.6) < 1e-12);

    // 先画的那条轴反而是短轴：主轴要换成与它垂直的那条，比值 < 1
    //（漏掉这一步会让 40 组码 > 1 被夹成 1，椭圆退化成一条线段）
    axes = EllipseEntity::normalizeHalfAxes(Vector2D(30.0, 0.0), 50.0);
    QVERIFY(std::abs(axes.majorAxisEnd.x) < 1e-12);
    QVERIFY(std::abs(axes.majorAxisEnd.y - 50.0) < 1e-12);
    QVERIFY(std::abs(axes.ratio - 0.6) < 1e-12);

    // 指定的轴是斜的：主轴方向随之旋转，而不是回到坐标轴
    axes = EllipseEntity::normalizeHalfAxes(Vector2D(3.0, 4.0), 100.0);
    QVERIFY(std::abs(axes.majorAxisEnd.length() - 100.0) < 1e-9);
    QVERIFY(std::abs(axes.ratio - 0.05) < 1e-12);
    // 垂直关系：主轴与指定的轴点积为 0
    QVERIFY(std::abs(dot(axes.majorAxisEnd, Vector2D(3.0, 4.0))) < 1e-9);

    // 退化输入：任一半轴为 0 时不给出可用的椭圆（调用方负责拦下并提示）
    axes = EllipseEntity::normalizeHalfAxes(Vector2D(0.0, 0.0), 30.0);
    QVERIFY(axes.majorAxisEnd.length() == 0.0);
    axes = EllipseEntity::normalizeHalfAxes(Vector2D(50.0, 0.0), 0.0);
    QVERIFY(axes.majorAxisEnd.length() == 0.0);
}

// -----------------------------------------------------------------------------
void TestGeometry::ellipseBoundsAndSnapPoints()
{
    // 长半轴 50 沿 +x、比值 0.5（短半轴 25）的整椭圆
    const EllipseEntity ellipse(Point2D(10.0, 20.0), Point2D(50.0, 0.0),
                                0.5, 0.0, kTwoPi);

    QVERIFY(ellipse.isClosed());
    QVERIFY(std::abs(ellipse.majorRadius() - 50.0) < 1e-12);
    QVERIFY(std::abs(ellipse.minorRadius() - 25.0) < 1e-12);
    QVERIFY(std::abs(ellipse.majorAxisAngle()) < 1e-12);

    const Box2D box = ellipse.boundingBox();
    QVERIFY(std::abs(box.minX - (-40.0)) < 1e-9);
    QVERIFY(std::abs(box.maxX - 60.0) < 1e-9);
    QVERIFY(std::abs(box.minY - (-5.0)) < 1e-9);
    QVERIFY(std::abs(box.maxY - 45.0) < 1e-9);

    // 捕捉点：中心 + 长短轴四个端点
    const std::vector<Point2D> snaps = ellipse.snapPoints();
    QCOMPARE(snaps.size(), std::size_t(5));
    QVERIFY(snaps.front() == Point2D(10.0, 20.0));
    QVERIFY(std::abs(snaps[1].x - 60.0) < 1e-9);      // +主轴
    QVERIFY(std::abs(snaps[2].x - (-40.0)) < 1e-9);   // −主轴
    QVERIFY(std::abs(snaps[3].y - 45.0) < 1e-9);      // +副轴
    QVERIFY(std::abs(snaps[4].y - (-5.0)) < 1e-9);    // −副轴

    // 边界上的点距离为 0（容差取 2e-3：椭圆没有解析的最近点，走的是基类按
    // 弦高 1e-3 离散后的折线求最近点，误差就是这个量级），
    // 中心到边界即短半轴
    const double onCurveTol = 2e-3;
    QVERIFY(ellipse.distanceTo(Point2D(60.0, 20.0)) < onCurveTol);
    QVERIFY(ellipse.distanceTo(Point2D(10.0, 45.0)) < onCurveTol);
    QVERIFY(std::abs(ellipse.distanceTo(Point2D(10.0, 20.0)) - 25.0) < onCurveTol);
}

// -----------------------------------------------------------------------------
void TestGeometry::textBoundingBoxAndAlignment()
{
    const TextEntity text(QStringLiteral("测试AB"), Point2D(10.0, 20.0), 4.0);

    // 宽度估算：2 个 CJK（各 1.45 字高）+ 2 个 ASCII（各 0.85 字高）= 4.6 × 4
    QVERIFY(std::abs(text.estimatedWidth() - 4.6 * 4.0) < 1e-9);

    // 包围盒必须覆盖插入点且高度为正
    const Box2D box = text.boundingBox();
    QVERIFY(box.isValid());
    QVERIFY(box.width() > 0.0 && box.height() > 0.0);
    QVERIFY(box.contains(Point2D(10.0, 20.0), 0.1));

    // 旋转 90° 后包围盒宽高互换
    {
        TextEntity rotated(QStringLiteral("测试AB"), Point2D(10.0, 20.0), 4.0);
        rotated.setRotation(kHalfPi);
        const Box2D rotatedBox = rotated.boundingBox();
        QVERIFY(std::abs(rotatedBox.width() - box.height()) < 1e-9);
        QVERIFY(std::abs(rotatedBox.height() - box.width()) < 1e-9);
    }

    // 左/基线对齐使用第一对齐点；居中/居中使用第二对齐点
    QCOMPARE(text.effectivePosition(), Point2D(10.0, 20.0));

    {
        TextEntity centered(QStringLiteral("测试AB"), Point2D(10.0, 20.0), 4.0);
        centered.setHAlign(TextEntity::HAlign::Center);
        centered.setVAlign(TextEntity::VAlign::Middle);
        centered.setAlignmentPoint(Point2D(50.0, 60.0));
        QCOMPARE(centered.effectivePosition(), Point2D(50.0, 60.0));

        // 夹点拖拽应移动参与定位的那个点
        QVERIFY(centered.moveGrip(0, Point2D(70.0, 80.0)));
        QCOMPARE(centered.alignmentPoint(), Point2D(70.0, 80.0));
        QCOMPARE(centered.position(), Point2D(10.0, 20.0));
    }

    // 等比变换：字高随缩放变化，旋转角叠加
    {
        TextEntity scaled(QStringLiteral("测试AB"), Point2D(10.0, 20.0), 4.0);
        scaled.transform(Transform2D::translation(1.0, 1.0)
                         * Transform2D::scalingAround(Point2D(0.0, 0.0), 2.0, 2.0));
        QVERIFY(std::abs(scaled.height() - 8.0) < 1e-9);
    }

    // 水平镜像（背面文字）：文字自锚点向左延伸，包围盒随之翻转
    {
        TextEntity mirrored(QStringLiteral("AB"), Point2D(10.0, 20.0), 4.0);
        const Box2D normalBox = mirrored.boundingBox();
        QVERIFY(normalBox.minX >= 10.0 - 1e-9);          // 正常文字向右延伸

        mirrored.setBackwards(true);
        const Box2D mirroredBox = mirrored.boundingBox();
        QVERIFY(mirroredBox.maxX <= 10.0 + 1e-9);        // 镜像文字向左延伸
        QVERIFY(std::abs(mirroredBox.minX - (10.0 - normalBox.width())) < 1e-9);

        // 镜像变换命令切换标志
        mirrored.transform(Transform2D::mirror(Point2D(0.0, 0.0), Point2D(1.0, 1.0)));
        QVERIFY(!mirrored.isBackwards());
    }

    // 宽度因子：估算宽度线性缩放；负值归一为镜像标志
    {
        TextEntity wide(QStringLiteral("AB"), Point2D(0.0, 0.0), 4.0);
        const double base = wide.estimatedWidth();
        wide.setWidthFactor(1.1);
        QVERIFY(std::abs(wide.estimatedWidth() - base * 1.1) < 1e-9);
        QVERIFY(!wide.isBackwards());

        wide.setWidthFactor(-1.0);
        QVERIFY(wide.isBackwards());
        QCOMPARE(wide.widthFactor(), 1.0);
    }
}

// -----------------------------------------------------------------------------
void TestGeometry::hatchContainmentAndArea()
{
    // 单位正方形边界
    const HatchEntity hatch({ Point2D(0.0, 0.0), Point2D(4.0, 0.0),
                              Point2D(4.0, 3.0), Point2D(0.0, 3.0) },
                            QStringLiteral("ANSI31"), 2.0, 0.0);

    QVERIFY(hatch.containsPoint(Point2D(2.0, 1.5)));
    QVERIFY(!hatch.containsPoint(Point2D(5.0, 5.0)));

    // 内部点拾取距离为 0，外部点为到边界的最短距离
    QCOMPARE(hatch.distanceTo(Point2D(2.0, 1.5)), 0.0);
    QVERIFY(std::abs(hatch.distanceTo(Point2D(6.0, 1.5)) - 2.0) < 1e-9);

    QVERIFY(std::abs(hatch.polygonArea() - 12.0) < 1e-9);

    // 图案参数
    QVERIFY(!hatch.isSolid());
    QVERIFY(std::abs(hatch.patternSpacing() - HatchEntity::kAnsi31BaseSpacing * 2.0) < 1e-9);

    const HatchEntity solid({ Point2D(0.0, 0.0), Point2D(1.0, 0.0), Point2D(0.0, 1.0) },
                            QStringLiteral("SOLID"), 1.0, 0.0);
    QVERIFY(solid.isSolid());
}

// -----------------------------------------------------------------------------
void TestGeometry::hatchTransformMovesBoundary()
{
    HatchEntity hatch({ Point2D(0.0, 0.0), Point2D(4.0, 0.0),
                        Point2D(4.0, 3.0), Point2D(0.0, 3.0) });

    hatch.transform(Transform2D::translation(Vector2D(10.0, 20.0)));

    QCOMPARE(hatch.vertex(0), Point2D(10.0, 20.0));
    QCOMPARE(hatch.vertex(2), Point2D(14.0, 23.0));
    QVERIFY(hatch.containsPoint(Point2D(12.0, 21.5)));
    QVERIFY(!hatch.containsPoint(Point2D(2.0, 1.5)));
}

// -----------------------------------------------------------------------------
void TestGeometry::hatchBulgeFormsCircle()
{
    // 「2 顶点 + 凸度 1」= 整圆（EDA 焊盘的常见 DXF 形态）
    // 两顶点 (0,0)→(4,0)，两个凸度 1：两段各为半圆，合为半径 2 的圆
    HatchEntity circle({ Point2D(0.0, 0.0), Point2D(4.0, 0.0) },
                       { 1.0, 1.0 });

    QCOMPARE(circle.vertexCount(), std::size_t(2));

    // 包围盒应覆盖整圆：圆心 (2,0)、半径 2，即 x ∈ [0,4]，y ∈ [-2,2]
    const Box2D box = circle.boundingBox();
    QVERIFY(std::abs(box.minX - 0.0) < 1e-6);
    QVERIFY(std::abs(box.maxX - 4.0) < 1e-6);
    QVERIFY(std::abs(box.minY - (-2.0)) < 1e-6);
    QVERIFY(std::abs(box.maxY - 2.0) < 1e-6);

    // 圆心与内部/外部判定
    QVERIFY(circle.containsPoint(Point2D(2.0, 0.0)));
    QVERIFY(!circle.containsPoint(Point2D(2.0, 3.0)));
    QCOMPARE(circle.distanceTo(Point2D(2.0, 0.0)), 0.0);
    QVERIFY(std::abs(circle.distanceTo(Point2D(2.0, 3.0)) - 1.0) < 1e-6);

    // 离散化后周长 = 2πr
    const std::vector<Point2D> flat = circle.flatten(1e-6);
    double perimeter = 0.0;
    for (std::size_t i = 0; i < flat.size(); ++i) {
        perimeter += flat[i].distanceTo(flat[(i + 1) % flat.size()]);
    }
    QVERIFY(std::abs(perimeter - 2.0 * kPi * 2.0) < 0.01);

    // 镜像：凸度符号取反，几何不变
    circle.transform(Transform2D::mirror(Point2D(0.0, 0.0), Point2D(1.0, 0.0)));
    QCOMPARE(circle.vertexBulge(0), -1.0);
    QVERIFY(circle.containsPoint(Point2D(2.0, 0.0)));
}

// -----------------------------------------------------------------------------
void TestGeometry::dimensionMeasurementAllTypes()
{
    // 对齐：两点距离
    DimensionEntity aligned(DimensionEntity::DimType::Aligned);
    aligned.setFirstPoint(Point2D(0.0, 0.0));
    aligned.setSecondPoint(Point2D(3.0, 4.0));
    QVERIFY(std::abs(aligned.measurement() - 5.0) < 1e-9);

    // 线性：沿尺寸线方向投影。水平标注测 X 距离
    DimensionEntity horizontal(DimensionEntity::DimType::Linear);
    horizontal.setFirstPoint(Point2D(1.0, 10.0));
    horizontal.setSecondPoint(Point2D(4.0, 20.0));
    horizontal.setRotation(0.0);
    QVERIFY(std::abs(horizontal.measurement() - 3.0) < 1e-9);

    // 垂直标注测 Y 距离
    horizontal.setRotation(kHalfPi);
    QVERIFY(std::abs(horizontal.measurement() - 10.0) < 1e-9);

    // 半径 / 直径
    DimensionEntity radial(DimensionEntity::DimType::Radial);
    radial.setFirstPoint(Point2D(0.0, 0.0));
    radial.setSecondPoint(Point2D(5.0, 0.0));
    QVERIFY(std::abs(radial.measurement() - 5.0) < 1e-9);
    QVERIFY(radial.displayText().startsWith(QLatin1Char('R')));

    DimensionEntity diametric(DimensionEntity::DimType::Diametric);
    diametric.setFirstPoint(Point2D(-5.0, 0.0));
    diametric.setSecondPoint(Point2D(5.0, 0.0));
    QVERIFY(std::abs(diametric.measurement() - 10.0) < 1e-9);
    QVERIFY(diametric.displayText().contains(QChar(0x00D8)));   // ⌀

    // 文字覆盖优先于自动测量
    diametric.setTextOverride(QStringLiteral("参照图纸"));
    QCOMPARE(diametric.displayText(), QStringLiteral("参照图纸"));
}

// -----------------------------------------------------------------------------
void TestGeometry::dimensionGeometryDerivation()
{
    // 水平对齐标注：界线垂直引至尺寸线，文字位于尺寸线上方
    DimensionEntity dim(DimensionEntity::DimType::Aligned);
    dim.setFirstPoint(Point2D(0.0, 0.0));
    dim.setSecondPoint(Point2D(100.0, 0.0));
    dim.setDimLinePoint(Point2D(50.0, -20.0));
    dim.setTextHeight(4.0);

    const DimensionEntity::Geometry geo = dim.geometry();
    QVERIFY(geo.valid);
    QCOMPARE(geo.segments.size(), std::size_t(3));      // 2 条界线 + 1 条尺寸线
    QCOMPARE(geo.arrows.size(), std::size_t(2));
    QCOMPARE(geo.text, dim.displayText());

    // 尺寸线两端垂足
    QVERIFY(geo.segments[2].first.distanceTo(Point2D(0.0, -20.0)) < 1e-9);
    QVERIFY(geo.segments[2].second.distanceTo(Point2D(100.0, -20.0)) < 1e-9);

    // 文字锚点在尺寸线上方（可读方向一侧）
    QVERIFY(geo.textAnchor.y > -20.0);
    QVERIFY(std::abs(geo.textAnchor.x - 50.0) < 1e-9);

    // 文字方向保持水平（0° 附近）
    QVERIFY(std::abs(normalizeAngle(geo.textAngle)) < 1e-9
            || std::abs(normalizeAngle(geo.textAngle) - kTwoPi) < 1e-9);

    // 拾取：靠近尺寸线时应命中
    QVERIFY(dim.distanceTo(Point2D(50.0, -20.0)) < 1e-9);
    // 远离时距离单调增大
    QVERIFY(dim.distanceTo(Point2D(50.0, 30.0)) > dim.distanceTo(Point2D(50.0, 0.0)));
}

// -----------------------------------------------------------------------------
void TestGeometry::dimensionTypeNameIsNotMojibake()
{
    // 回归：类型名一度是 const char* + QString::fromLatin1，UTF-8 字节被按
    // Latin-1 解码，属性面板的「几何信息」里「线性」显示成「çº¿æ§」。
    // 逐类型比对完整首行，顺带钉住类型与名字的对应关系。
    const struct {
        DimensionEntity::DimType type;
        const char*              name;   ///< UTF-8 字面量
    } cases[] = {
        { DimensionEntity::DimType::Linear,    "线性" },
        { DimensionEntity::DimType::Aligned,   "对齐" },
        { DimensionEntity::DimType::Diametric, "直径" },
        { DimensionEntity::DimType::Radial,    "半径" },
    };

    for (const auto& c : cases) {
        DimensionEntity dim(c.type);
        dim.setFirstPoint(Point2D(0.0, 0.0));
        dim.setSecondPoint(Point2D(10.0, 0.0));
        dim.setDimLinePoint(Point2D(5.0, -5.0));

        const QString name = QString::fromUtf8(c.name);

        // 摘要首行 =「类型: <名字>」；还有后续行，否则说明摘要被截断
        const QStringList lines = dim.geometrySummary().split(QLatin1Char('\n'));
        QVERIFY(lines.size() > 4);
        QCOMPARE(lines.first(), QStringLiteral("类型: %1").arg(name));

        // describe() 走同一份映射，同样不能出现乱码
        QVERIFY(dim.describe().startsWith(QStringLiteral("标注  %1").arg(name)));
    }
}

// -----------------------------------------------------------------------------
void TestGeometry::dimensionGripsReflowGeometry()
{
    DimensionEntity dim(DimensionEntity::DimType::Aligned);
    dim.setFirstPoint(Point2D(0.0, 0.0));
    dim.setSecondPoint(Point2D(100.0, 0.0));
    dim.setDimLinePoint(Point2D(50.0, -20.0));

    // 拖动尺寸线夹点：测量值不变，几何随之重排
    QVERIFY(dim.moveGrip(2, Point2D(50.0, 30.0)));
    QVERIFY(std::abs(dim.measurement() - 100.0) < 1e-9);
    const DimensionEntity::Geometry geo = dim.geometry();
    QVERIFY(geo.segments[2].first.distanceTo(Point2D(0.0, 30.0)) < 1e-9);

    // 拖动界线原点：测量值改变（对齐标注取两点直线距离）
    QVERIFY(dim.moveGrip(1, Point2D(140.0, 30.0)));
    QVERIFY(std::abs(dim.measurement() - std::hypot(140.0, 30.0)) < 1e-9);
}

// -----------------------------------------------------------------------------
//  构造线 / 射线
// -----------------------------------------------------------------------------
void TestGeometry::infiniteLineDirectionIsNormalized()
{
    const XLineEntity xline(Point2D(10.0, 20.0), Vector2D(3.0, 4.0));

    // 方向一律按单位向量存储：DXF 的 11 组码就是这么约定的，
    // 归一化后相距任意远近的「方向」才有一致的语义。
    QVERIFY(std::abs(xline.direction().length() - 1.0) < 1e-12);
    QVERIFY(std::abs(xline.direction().x - 0.6) < 1e-12);
    QVERIFY(std::abs(xline.direction().y - 0.8) < 1e-12);
    QVERIFY(std::abs(xline.angle() - std::atan2(4.0, 3.0)) < 1e-12);

    // 零方向被忽略而不是把图元变成空壳——否则画出来什么都没有，还选不中
    XLineEntity zero(Point2D(1.0, 2.0), Vector2D(0.0, 0.0));
    QVERIFY(!zero.isDegenerate());
    QCOMPARE(zero.direction().length(), 1.0);

    // 两点式：基点 + 过两点连线的方向
    const RayEntity ray = RayEntity::throughPoints(Point2D(5.0, 5.0), Point2D(5.0, 15.0));
    QVERIFY(std::abs(ray.basePoint().x - 5.0) < 1e-12);
    QVERIFY(std::abs(ray.direction().y - 1.0) < 1e-12);
    QVERIFY(std::abs(ray.direction().x) < 1e-12);
}

void TestGeometry::infiniteLineClippingRespectsDirection()
{
    const Box2D box(0.0, 0.0, 10.0, 10.0);

    // 构造线：双向延伸到盒外，得到的端点落在盒边界上
    const XLineEntity xline(Point2D(5.0, 5.0), Vector2D(1.0, 1.0));
    const auto xSeg = xline.clippedTo(box);
    QVERIFY(xSeg.has_value());
    QVERIFY(std::abs(xSeg->first.x - 0.0) < 1e-9 && std::abs(xSeg->first.y - 0.0) < 1e-9);
    QVERIFY(std::abs(xSeg->second.x - 10.0) < 1e-9 && std::abs(xSeg->second.y - 10.0) < 1e-9);

    // 射线：只朝方向一侧延伸，反向的半个盒内没有线
    const RayEntity ray(Point2D(5.0, 5.0), Vector2D(1.0, 1.0));
    const auto raySeg = ray.clippedTo(box);
    QVERIFY(raySeg.has_value());
    QVERIFY(std::abs(raySeg->first.x - 5.0) < 1e-9 && std::abs(raySeg->first.y - 5.0) < 1e-9);
    QVERIFY(std::abs(raySeg->second.x - 10.0) < 1e-9);

    // 射线整条都在盒外（起点在盒上方且朝上）时，裁不出任何东西
    const RayEntity away(Point2D(5.0, 50.0), Vector2D(0.0, 1.0));
    QVERIFY(!away.clippedTo(box).has_value());

    // 同一条射线朝下则穿过整个盒子：端点贴在上、下两条边上，
    // 起点 (5,50) 在盒外，因此不是端点——裁剪结果永远是「盒内的那一段」
    const RayEntity toward(Point2D(5.0, 50.0), Vector2D(0.0, -1.0));
    const auto towardSeg = toward.clippedTo(box);
    QVERIFY(towardSeg.has_value());
    QVERIFY(std::abs(towardSeg->first.y - 10.0) < 1e-9);
    QVERIFY(std::abs(towardSeg->second.y - 0.0) < 1e-9);

    // 起点在盒内时，裁剪结果必然从起点开始（射线不向反方向延伸）
    const RayEntity fromInside(Point2D(5.0, 5.0), Vector2D(0.0, -1.0));
    const auto insideSeg = fromInside.clippedTo(box);
    QVERIFY(insideSeg.has_value());
    QVERIFY(std::abs(insideSeg->first.y - 5.0) < 1e-9);
    QVERIFY(std::abs(insideSeg->second.y - 0.0) < 1e-9);
    // 反向的构造线则会同时穿出去两侧
    const XLineEntity throughInside(Point2D(5.0, 5.0), Vector2D(0.0, -1.0));
    const auto throughSeg = throughInside.clippedTo(box);
    QVERIFY(throughSeg.has_value());
    QVERIFY(std::abs(throughSeg->first.y - 10.0) < 1e-9);
    QVERIFY(std::abs(throughSeg->second.y - 0.0) < 1e-9);

    // 水平线：与盒平行且贴边时仍然裁得出一段（不能因为「平行」就判为不相交）
    const XLineEntity horizontal(Point2D(5.0, 0.0), Vector2D(1.0, 0.0));
    const auto hSeg = horizontal.clippedTo(box);
    QVERIFY(hSeg.has_value());
    QVERIFY(std::abs(hSeg->first.x - 0.0) < 1e-9);
    QVERIFY(std::abs(hSeg->second.x - 10.0) < 1e-9);

    // 与盒完全无关的平行线：不相交
    const XLineEntity outside(Point2D(5.0, 20.0), Vector2D(1.0, 0.0));
    QVERIFY(!outside.clippedTo(box).has_value());
}

void TestGeometry::infiniteLineDistanceAndCrossing()
{
    // 水平构造线 y = 0
    const XLineEntity xline(Point2D(0.0, 0.0), Vector2D(1.0, 0.0));
    QVERIFY(std::abs(xline.distanceTo(Point2D(50.0, 3.0)) - 3.0) < 1e-9);
    // 到无限直线的最短距离与「投影是否落在基点之后」无关
    QVERIFY(std::abs(xline.distanceTo(Point2D(-80.0, 4.0)) - 4.0) < 1e-9);
    const Point2D closest = xline.closestPointTo(Point2D(-80.0, 4.0));
    QVERIFY(std::abs(closest.x + 80.0) < 1e-9 && std::abs(closest.y) < 1e-9);

    // 射线只覆盖 t >= 0：基点之后的点最近处就是基点本身
    const RayEntity ray(Point2D(0.0, 0.0), Vector2D(1.0, 0.0));
    QVERIFY(std::abs(ray.distanceTo(Point2D(-30.0, 4.0)) - std::hypot(30.0, 4.0)) < 1e-9);
    QVERIFY(std::abs(ray.distanceTo(Point2D(30.0, 4.0)) - 4.0) < 1e-9);

    // 与框相交：无限直线穿过框即算相交；射线要看方向
    const Box2D box(10.0, -5.0, 20.0, 5.0);
    QVERIFY(xline.crosses(box));
    QVERIFY(ray.crosses(box));
    const RayEntity backward(Point2D(0.0, 0.0), Vector2D(-1.0, 0.0));
    QVERIFY(!backward.crosses(box));
    // 窗选：无限长的线永远不会「完全落在框内」
    QVERIFY(!xline.isInside(Box2D(-1e6, -1e6, 1e6, 1e6)));
}

void TestGeometry::infiniteLineBoundingBoxIsUndefined()
{
    const XLineEntity xline(Point2D(3.0, 4.0), Vector2D(1.0, 0.0));
    const RayEntity ray(Point2D(3.0, 4.0), Vector2D(1.0, 0.0));

    // 无界图元没有有限范围：包围盒必须是无效盒，这样范围缩放的
    // Box2D::expand 会自然跳过它们，而不是把视口撑到无穷
    QVERIFY(!xline.boundingBox().isValid());
    QVERIFY(!ray.boundingBox().isValid());
    QVERIFY(xline.isUnbounded());
    QVERIFY(ray.isUnbounded());

    Box2D accumulated;
    accumulated.expand(Point2D(0.0, 0.0));
    accumulated.expand(xline.boundingBox());
    QVERIFY(accumulated.valid);
    QVERIFY(std::abs(accumulated.maxX) < 1e-12);      // 未被无界图元污染

    // 定义点用于坐标体检与属性展示
    QVERIFY(xline.definitionPoint().has_value());
    QVERIFY(std::abs(xline.definitionPoint()->x - 3.0) < 1e-12);
    // 普通图元没有定义点
    const LineEntity line(Point2D(0.0, 0.0), Point2D(1.0, 1.0));
    QVERIFY(!line.definitionPoint().has_value());
    QVERIFY(!line.isUnbounded());
}

void TestGeometry::infiniteLineTransformAndGrips()
{
    XLineEntity xline(Point2D(0.0, 0.0), Vector2D(1.0, 0.0));
    xline.transform(Transform2D::rotationAround(Point2D(0.0, 0.0), kHalfPi));
    QVERIFY(std::abs(xline.direction().x) < 1e-12);
    QVERIFY(std::abs(xline.direction().y - 1.0) < 1e-12);

    xline.transform(Transform2D::translation(5.0, -2.0));
    QVERIFY(std::abs(xline.basePoint().x - 5.0) < 1e-12);
    QVERIFY(std::abs(xline.basePoint().y + 2.0) < 1e-12);

    // 镜像保持方向为单位向量（行列式为负，方向随之翻转）
    xline.transform(Transform2D::mirror(Point2D(0.0, 0.0), Point2D(1.0, 0.0)));
    QVERIFY(std::abs(xline.direction().length() - 1.0) < 1e-12);

    // 夹点只有定义点一个
    const std::vector<Point2D> grips = xline.gripPoints();
    QCOMPARE(grips.size(), std::size_t(1));
    QVERIFY(grips.front() == xline.basePoint());

    // 拖动定义点整体平移，方向不变
    const Vector2D before = xline.direction();
    QVERIFY(xline.moveGrip(0, Point2D(100.0, 200.0)));
    QVERIFY(std::abs(xline.basePoint().x - 100.0) < 1e-12);
    QVERIFY(std::abs(xline.direction().x - before.x) < 1e-12
            && std::abs(xline.direction().y - before.y) < 1e-12);
    QVERIFY(!xline.moveGrip(1, Point2D(0.0, 0.0)));    // 只有一个夹点

    // 克隆保真
    const std::unique_ptr<Entity> copy = xline.clone();
    QCOMPARE(copy->type(), EntityType::XLine);
    const auto& cloned = static_cast<const InfiniteLineEntity&>(*copy);
    QVERIFY(cloned.basePoint() == xline.basePoint());
    QVERIFY(std::abs(cloned.direction().x - xline.direction().x) < 1e-12
            && std::abs(cloned.direction().y - xline.direction().y) < 1e-12);
}

// -----------------------------------------------------------------------------
void TestGeometry::coordinateParsing()
{
    // 绝对直角坐标
    {
        const ParsedCoordinate parsed = parseCoordinate(QStringLiteral("100,50"));
        QVERIFY(parsed.isValid());
        QCOMPARE(parsed.kind, ParsedCoordinate::Kind::Absolute);
        const auto point = parsed.resolve(std::nullopt);
        QVERIFY(point.has_value());
        QVERIFY(std::abs(point->x - 100.0) < 1e-12);
        QVERIFY(std::abs(point->y - 50.0) < 1e-12);
    }

    // 相对直角坐标
    {
        const ParsedCoordinate parsed = parseCoordinate(QStringLiteral("@50,-10"));
        QCOMPARE(parsed.kind, ParsedCoordinate::Kind::Relative);
        const auto point = parsed.resolve(Point2D(10.0, 10.0));
        QVERIFY(point.has_value());
        QVERIFY(std::abs(point->x - 60.0) < 1e-9);
        QVERIFY(std::abs(point->y - 0.0) < 1e-9);

        // 缺少基准点时无法解析
        QVERIFY(!parsed.resolve(std::nullopt).has_value());
    }

    // 极坐标
    {
        const ParsedCoordinate parsed = parseCoordinate(QStringLiteral("50<90"));
        QCOMPARE(parsed.kind, ParsedCoordinate::Kind::Polar);
        const auto point = parsed.resolve(Point2D(0.0, 0.0));
        QVERIFY(point.has_value());
        QVERIFY(std::abs(point->x - 0.0) < 1e-9);
        QVERIFY(std::abs(point->y - 50.0) < 1e-9);
    }

    // 算式与裸数值
    {
        const ParsedCoordinate parsed = parseCoordinate(QStringLiteral("25*2"));
        QCOMPARE(parsed.kind, ParsedCoordinate::Kind::Scalar);
        QVERIFY(std::abs(parsed.scalar - 50.0) < 1e-12);
    }
    {
        const ParsedCoordinate parsed = parseCoordinate(QStringLiteral("(10+5)/3"));
        QCOMPARE(parsed.kind, ParsedCoordinate::Kind::Scalar);
        QVERIFY(std::abs(parsed.scalar - 5.0) < 1e-12);
    }

    // 非法输入
    QVERIFY(!parseCoordinate(QStringLiteral("abc")).isValid());
    QVERIFY(!parseCoordinate(QString()).isValid());
    QVERIFY(!evaluateExpression(QStringLiteral("1/0")).has_value());
}

QTEST_MAIN(TestGeometry)
#include "test_geometry.moc"
