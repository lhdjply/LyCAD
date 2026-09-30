// =============================================================================
//  LyCAD - tests/test_geometry.cpp
//  几何模型与坐标解析的单元测试。
// =============================================================================
#include <QtTest>

#include "core/ArcEntity.h"
#include "core/CircleEntity.h"
#include "core/DimensionEntity.h"
#include "core/HatchEntity.h"
#include "core/LineEntity.h"
#include "core/PolylineEntity.h"
#include "core/TextEntity.h"
#include "core/Types.h"
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
    void polylineMirrorFlipsBulgeSign();
    void rectangleIsClosedPolyline();

    // ---- 文字 / 填充 / 标注 ---------------------------------------------------
    void textBoundingBoxAndAlignment();
    void hatchContainmentAndArea();
    void hatchTransformMovesBoundary();
    void dimensionMeasurementAllTypes();
    void dimensionGeometryDerivation();
    void dimensionGripsReflowGeometry();

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
void TestGeometry::textBoundingBoxAndAlignment()
{
    const TextEntity text(QStringLiteral("测试AB"), Point2D(10.0, 20.0), 4.0);

    // 宽度估算：2 个 CJK（各 1.0 字高）+ 2 个 ASCII（各 0.55 字高）= 3.1 × 4
    QVERIFY(std::abs(text.estimatedWidth() - 3.1 * 4.0) < 1e-9);

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
