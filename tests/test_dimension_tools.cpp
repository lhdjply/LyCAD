// =============================================================================
//  LyCAD - tests/test_dimension_tools.cpp
//  坐标标注 / 弧长标注这两条交互命令的输入序列与产出的定义点。
//
//  这两类标注的定义点与组码对应关系与线性标注不同（改动很容易「只是点了几下，
//  存出来的却是另一回事」），因此这里盯住的是：
//    · 命令行提示随步骤推进（提示就是状态机的投影）
//    · 点完之后落进文档的定义点**逐个**对得上组码语义
//    · 坐标标注的度量轴由引线方向决定（偏竖直量 X、偏水平量 Y）
//    · 弧长标注量的是被拾取圆弧的弧长，尺寸弧位置只影响显示半径
// =============================================================================
#include <QtTest/QtTest>

#include "core/ArcEntity.h"
#include "core/DimensionEntity.h"
#include "core/Document.h"
#include "interact/AnnotationTools.h"
#include "interact/ToolManager.h"
#include "render/CadCanvas.h"

using namespace lycad;

namespace {

/// 发一次左键点击
void click(Tool& tool, const Point2D& world)
{
    CanvasEvent event;
    event.button = Qt::LeftButton;
    event.worldPos = world;
    tool.mousePress(event);
}

/// 移动一次鼠标（不改状态，只为让工具记下光标位置）
void move(Tool& tool, const Point2D& world)
{
    CanvasEvent event;
    event.worldPos = world;
    tool.mouseMove(event);
}

/// 文档里唯一的那个标注（弧长标注的用例里还有被测量的圆弧，不能按图元总数断言）
DimensionEntity* onlyDimension(const Document& doc)
{
    DimensionEntity* found = nullptr;
    for (EntityId id : doc.drawOrder()) {
        if (auto* dim = dynamic_cast<DimensionEntity*>(doc.entity(id))) {
            if (found != nullptr) return nullptr;   // 不止一个：用例写错了
            found = dim;
        }
    }
    return found;
}

} // namespace

class TestDimensionTools : public QObject {
    Q_OBJECT

private slots:
    void commandsAreRegistered();
    void ordinateToolMeasuresXForVerticalLeader();
    void ordinateToolMeasuresYForHorizontalLeader();
    void arcLengthToolTakesArcGeometry();
};

// -----------------------------------------------------------------------------
void TestDimensionTools::commandsAreRegistered()
{
    ToolManager manager;
    QVERIFY(manager.hasCommand(QStringLiteral("DIMORDINATE")));
    QVERIFY(manager.hasCommand(QStringLiteral("DIMORD")));   // 别名
    QVERIFY(manager.hasCommand(QStringLiteral("DIMARC")));
    QVERIFY(manager.hasCommand(QStringLiteral("DAR")));
}

// -----------------------------------------------------------------------------
void TestDimensionTools::ordinateToolMeasuresXForVerticalLeader()
{
    Document document;
    CadCanvas canvas;
    canvas.setDocument(&document);
    ToolManager manager;
    manager.setDocument(&document);
    manager.setCanvas(&canvas);

    DrawDimOrdinateTool tool(manager.context());

    // 第一步：被测点
    QVERIFY(tool.prompt().contains(QStringLiteral("指定点坐标")));
    click(tool, Point2D(120.0, 45.0));
    QVERIFY(tool.prompt().contains(QStringLiteral("指定引线端点")));

    // 第二步：引线端点偏**竖直**（|Δy| > |Δx|）→ 量 X 坐标
    move(tool, Point2D(140.0, -80.0));
    click(tool, Point2D(140.0, -80.0));

    DimensionEntity* dim = onlyDimension(document);
    QVERIFY(dim != nullptr);
    QCOMPARE(dim->dimType(), DimensionEntity::DimType::Ordinate);
    QVERIFY(dim->isOrdinateXType());

    // 定义点与组码语义对齐：13 被测点、14 引线端点、10 坐标基准（WCS 原点）
    QVERIFY(dim->firstPoint().distanceTo(Point2D(120.0, 45.0)) < 1e-9);
    QVERIFY(dim->secondPoint().distanceTo(Point2D(140.0, -80.0)) < 1e-9);
    QVERIFY(dim->dimLinePoint().distanceTo(Point2D(0.0, 0.0)) < 1e-9);
    QVERIFY(std::abs(dim->measurement() - 120.0) < 1e-9);
}

// -----------------------------------------------------------------------------
void TestDimensionTools::ordinateToolMeasuresYForHorizontalLeader()
{
    Document document;
    CadCanvas canvas;
    canvas.setDocument(&document);
    ToolManager manager;
    manager.setDocument(&document);
    manager.setCanvas(&canvas);

    DrawDimOrdinateTool tool(manager.context());
    click(tool, Point2D(120.0, 45.0));

    // 引线端点偏**水平**（|Δx| > |Δy|）→ 量 Y 坐标
    move(tool, Point2D(400.0, 60.0));
    click(tool, Point2D(400.0, 60.0));

    DimensionEntity* dim = onlyDimension(document);
    QVERIFY(dim != nullptr);
    QVERIFY(!dim->isOrdinateXType());
    QVERIFY(std::abs(dim->measurement() - 45.0) < 1e-9);

    // 显示几何：引线沿测量轴的垂直方向（这里量 Y，引线沿 X 拉开）
    const DimensionEntity::Geometry geo = dim->geometry();
    QVERIFY(geo.valid);
    QCOMPARE(geo.segments.size(), std::size_t(3));
    QCOMPARE(geo.arrows.size(), std::size_t(0));
    // 第一段水平（在被测点的 Y 处沿 X 拉开）
    QVERIFY(std::abs(geo.segments[0].first.y - 45.0) < 1e-9);
    QVERIFY(std::abs(geo.segments[0].second.y - 45.0) < 1e-9);
}

// -----------------------------------------------------------------------------
void TestDimensionTools::arcLengthToolTakesArcGeometry()
{
    Document document;
    CadCanvas canvas;
    canvas.setDocument(&document);
    ToolManager manager;
    manager.setDocument(&document);
    manager.setCanvas(&canvas);

    // 文档里放一条半径 100、从 0° 到 90° 的圆弧
    auto arc = std::make_unique<ArcEntity>(Point2D(0.0, 0.0), 100.0, 0.0, kHalfPi);
    document.addEntity(std::move(arc));

    DrawDimArcTool tool(manager.context());

    // 第一步：点圆弧边线
    QVERIFY(tool.prompt().contains(QStringLiteral("选择圆弧")));
    click(tool, Point2D(100.0, 0.0));           // 正好在弧的起点上
    QVERIFY(tool.prompt().contains(QStringLiteral("指定尺寸弧位置")));

    // 第二步：指定尺寸弧经过的位置 → 尺寸弧半径 = 该点到弧心的距离
    move(tool, Point2D(60.0, 0.0));
    click(tool, Point2D(60.0, 0.0));

    DimensionEntity* dim = onlyDimension(document);
    QVERIFY(dim != nullptr);
    QCOMPARE(dim->dimType(), DimensionEntity::DimType::ArcLength);

    QVERIFY(dim->arcCenter().distanceTo(Point2D(0.0, 0.0)) < 1e-9);
    QVERIFY(std::abs(dim->arcStartAngle() - 0.0) < 1e-9);
    QVERIFY(std::abs(dim->arcEndAngle() - kHalfPi) < 1e-9);
    QVERIFY(dim->dimLinePoint().distanceTo(Point2D(60.0, 0.0)) < 1e-9);

    // 被测弧端点由「弧心 + 半径·方向」推出，半径是拾取到的 100
    QVERIFY(std::abs(dim->measuredArcRadius() - 100.0) < 1e-9);
    QVERIFY(dim->firstPoint().distanceTo(Point2D(100.0, 0.0)) < 1e-9);
    QVERIFY(dim->secondPoint().distanceTo(Point2D(0.0, 100.0)) < 1e-9);

    // 量的是弧长（半径 × 包角），不是弦长
    QVERIFY(std::abs(dim->measurement() - 100.0 * kHalfPi) < 1e-6);
}

QTEST_MAIN(TestDimensionTools)
#include "test_dimension_tools.moc"
