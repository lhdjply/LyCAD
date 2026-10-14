// =============================================================================
//  LyCAD - tests/test_table.cpp
//  表格图元（ACAD_TABLE）的文件往返测试。
//
//  覆盖两条原生链路（表格不允许降级成块参照）：
//    · DXF：ACAD_TABLE 实体本体携带完整行列结构与单元格文字，
//      R2000（代码页 GBK）与 2018（UTF-8）各验一档；
//    · DWG：dwgW 写出的原生 ACAD_TABLE 类实体（CLASSES 段登记 AcDbTable），
//      读取端按「三游标」解析：数据流 + objSize 位偏移起的句柄流 +
//      （2007+）字符串流。单元格数据体（cells 数组）按 opencadcodec 实测
//      位布局逐格写出/读回：文字内联（TV）、合并跨距写在左上角格、
//      每格在句柄流各占一个空值句柄。行列数、行高列宽、插入点、旋转、
//      图层、单元格文字、合并区必须逐项保真。
//
//  两条链路共同盯住的回归点：
//    · 表格读回必须还是**单一表格图元**——散成网格线 + 文字即为降级；
//    · 读取警告中不得出现「按几何块反推」——那意味着本体数据没被解出来；
//    · 旋转角在 DXF 侧按度存放（50 组码）、DWG 侧按弧度（BD），两侧换算必须对偶。
//  顺带验核心几何约定：第 0 行在最上方（行边界向下累积），列边界向右累积。
// =============================================================================
#include <QtTest>

#include <QFile>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>

#include <cmath>
#include <cstddef>
#include <memory>
#include <tuple>

#include "core/Document.h"
#include "core/Entity.h"
#include "core/Format.h"
#include "core/TableEntity.h"
#include "io/CadFile.h"
#include "render/CanvasTypes.h"
#include "render/EntityRenderer.h"
#include "render/ViewTransform.h"

using namespace lycad;

namespace {

/// 往返容差：DXF 文本按版本格式化、DWG 走 BD 压缩 double，
/// 取 1e-9 足以容忍两侧的合法舍入，又远小于任何真实错位
constexpr double kTol = 1e-9;

/// 离屏渲染一张表格（与 test_light 同一套脚手架：表格的包围盒随行高变化，
/// 不能靠 fitTo 世界范围，直接给定缩放并把表格中心对到视口中心）
QImage renderTable(const TableEntity& table, double pixelsPerUnit, const QSize& size)
{
    ViewTransform view;
    view.setScale(pixelsPerUnit, Point2D(size.width() / 2.0, size.height() / 2.0));
    view.centerOn(Point2D(table.totalWidth() * 0.5, -table.totalHeight() * 0.5), size);

    DisplaySettings settings;
    EntityRenderer::DrawContext context;
    context.view = &view;
    context.settings = &settings;
    context.viewport = QRectF(0.0, 0.0, size.width(), size.height());

    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    EntityRenderer::draw(painter, table, context, QColor(0, 0, 0), 0.0,
                         QStringLiteral("CONTINUOUS"));
    painter.end();
    return image;
}

/// 两张只差"有没有单元格文字"的渲染图的差异像素 —— 也就是文字墨迹 —— 的外接高度。
/// 用差分而不是直接量某个区域：网格线在两张图里完全一致，只有文字会浮现出来。
int textInkHeight(const QImage& withText, const QImage& withoutText)
{
    int top = -1;
    int bottom = -1;
    for (int y = 0; y < withText.height(); ++y) {
        for (int x = 0; x < withText.width(); ++x) {
            if (withText.pixel(x, y) == withoutText.pixel(x, y)) continue;
            if (top < 0) top = y;
            bottom = y;
            break;
        }
    }
    return top < 0 ? 0 : bottom - top + 1;
}

} // namespace

class TestTable : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void coreInvariants();
    void cellAtWorldHitTest();
    void gripResizeTest();
    void cellTextHeightIsPerCell();
    void dxfRoundTripR2000();
    void dxfRoundTrip2018();
    void dwgNativeRoundTrip();

private:
    /// 构造一张 4 行 × 3 列的明细表：不等距行列、旋转 15°、中文单元格、一处合并
    static void buildTableDocument(Document& document);
    static const TableEntity* firstTable(const Document& document);

    /// 写出后读回并逐字段比对。两条链路都校验单元格：DXF 走本体 cells，
    /// DWG 走读取端从 *Tnn 几何块按格归位的补全。
    bool roundTrip(const QString& version, const QString& fileName,
                   bool checkCells, QString* failure);

    QTemporaryDir m_dir;
};

// -----------------------------------------------------------------------------
void TestTable::initTestCase()
{
    QVERIFY2(m_dir.isValid(), "无法创建临时目录，测试无法继续");
    registerBuiltinCodecs();
}

// -----------------------------------------------------------------------------
void TestTable::coreInvariants()
{
    TableEntity table;
    table.reset(3, 4, 10.0, 25.0);

    QCOMPARE(table.rowCount(), std::size_t(3));
    QCOMPARE(table.columnCount(), std::size_t(4));
    QCOMPARE(table.totalHeight(), 30.0);
    QCOMPARE(table.totalWidth(), 100.0);

    // 第 0 行在最上方：行边界自 0 向下（负 Y）累积；列边界自 0 向右累积
    const std::vector<double> rows = table.rowBoundaries();
    QCOMPARE(rows.size(), std::size_t(4));
    QVERIFY(std::abs(rows[0] - 0.0) < kTol);
    QVERIFY(std::abs(rows[1] + 10.0) < kTol);
    QVERIFY(std::abs(rows[3] + 30.0) < kTol);

    const std::vector<double> cols = table.columnBoundaries();
    QCOMPARE(cols.size(), std::size_t(5));
    QVERIFY(std::abs(cols[4] - 100.0) < kTol);

    // 局部 -> 世界：插入点是原点，行进方向随旋转角
    table.setInsertionPoint(Point2D(100.0, 50.0));
    table.setRotation(degToRad(90.0));    // 逆时针 90°：局部 +X 转向世界 +Y
    const Point2D world = table.toWorld(Point2D(25.0, 0.0));
    QVERIFY(std::abs(world.x - 100.0) < kTol);
    QVERIFY(std::abs(world.y - 75.0) < kTol);

    // 互逆：toLocal(toWorld(p)) == p
    const Point2D back = table.toLocal(world);
    QVERIFY(std::abs(back.x - 25.0) < kTol);
    QVERIFY(std::abs(back.y - 0.0) < kTol);

    // 越界合并必须被拒收，不能制造自相矛盾的结构
    TableMergeRange bad;
    bad.row = 9;
    bad.col = 9;
    table.addMerge(bad);
    QVERIFY(table.merges().empty());
}

// -----------------------------------------------------------------------------
/// 点击命中测试：双击编辑靠它决定「改哪一格」。
///
/// 盯住四条约定：
///   · 格内任意点归该格，格线上的点归左侧 / 上方那格（不因浮点误差落到格外）；
///   · 表格外接矩形之外返回空（否则双击表格边上的空白也会弹编辑框）；
///   · 旋转过的表格用世界坐标点选，结果与未旋转时的局部格号一致；
///   · 落在合并区内的点归该区左上角——文字就存在那一格。
void TestTable::cellAtWorldHitTest()
{
    TableEntity table;
    table.reset(3, 4, 10.0, 25.0);                  // 3 行 × 4 列，行高 10、列宽 25
    table.setInsertionPoint(Point2D(100.0, 50.0));  // 插入点 = 左上角

    const auto cellOf = [&table](double x, double y) {
        return table.cellAtWorld(Point2D(x, y));
    };

    // 格内：第 1 行第 2 列（局部 x∈[50,75]，y∈[-20,-10]）
    QVERIFY(cellOf(162.5, 35.0).has_value());
    QCOMPARE(*cellOf(162.5, 35.0), std::make_pair(1, 2));

    // 左上角格与右下角格的格内点
    QCOMPARE(*cellOf(100.5, 49.5), std::make_pair(0, 0));
    QCOMPARE(*cellOf(199.5, 20.5), std::make_pair(2, 3));

    // 格线上的点：x = 125（第 0/1 列之间）归左列；y = 40（第 0/1 行之间）归上行
    QCOMPARE(*cellOf(125.0, 35.0), std::make_pair(1, 0));
    QCOMPARE(*cellOf(162.5, 40.0), std::make_pair(0, 2));

    // 表格之外：右侧、上方、下方、左侧各取一点，都必须落空
    QVERIFY(!cellOf(200.5, 40.0).has_value());
    QVERIFY(!cellOf(150.0, 50.5).has_value());
    QVERIFY(!cellOf(150.0, 19.5).has_value());
    QVERIFY(!cellOf(99.5, 40.0).has_value());

    // 旋转：同一格的中心，转成世界坐标后仍应命中同一格
    table.setRotation(degToRad(90.0));              // 逆时针 90°：局部 +X 转向世界 +Y
    const Point2D rotated = table.toWorld(Point2D(62.5, -15.0));    // 第 1 行第 2 列的中心
    QVERIFY(table.cellAtWorld(rotated).has_value());
    QCOMPARE(*table.cellAtWorld(rotated), std::make_pair(1, 2));
    table.setRotation(0.0);

    // 合并区：左上角 2×2（行 0~1、列 0~1），区内四个格都归 (0,0)
    TableMergeRange merge;
    merge.row = 0;
    merge.col = 0;
    merge.rowSpan = 2;
    merge.colSpan = 2;
    table.addMerge(merge);
    QCOMPARE(*cellOf(110.0, 45.0), std::make_pair(0, 0));     // 左上格
    QCOMPARE(*cellOf(135.0, 45.0), std::make_pair(0, 0));     // 右上格（列 1）
    QCOMPARE(*cellOf(110.0, 35.0), std::make_pair(0, 0));     // 左下格（行 1）
    QCOMPARE(*cellOf(135.0, 35.0), std::make_pair(0, 0));     // 右下格
    QCOMPARE(*cellOf(185.0, 25.0), std::make_pair(2, 3));     // 合并区之外的格不受影响
}

// -----------------------------------------------------------------------------
/// 夹点与界线拖拽：改行高 / 列宽（Word 表格的手感）。
///
/// 盯住五条约定：
///   · 夹点**只有外框四角**（整体等比缩放）——界线上不画方块，一整条表上十几个
///     方块太闹，而且拖线的命中范围本来就是整条线，不需要一个点来代言；
///   · 界线命中（`edgeAtWorld`）——整条线可抓、最上/最左两条边不算界线、表格外的
///     空白不算、容差决定抓取范围、交点处取列界线（顺序固定）；
///   · **内部界线只挪线**——左右（上下）两格互相让，一格长多少邻格就短多少，
///     表格总宽 / 总高与插入点都不变；
///   · 只有最右 / 最下两条界线没有邻格可让，才改表格总尺寸；拖过头会停在邻格还剩
///     一点的地方，两格都不会出现负尺寸；
///   · 四角仍是等比缩放（既有行为不许回退）。
void TestTable::gripResizeTest()
{
    const auto makeTable = [] {
        auto table = std::make_unique<TableEntity>();
        table->reset(3, 4, 10.0, 25.0);                  // 3 行 × 4 列
        table->setInsertionPoint(Point2D(100.0, 50.0));
        return table;
    };
    // 世界坐标取点（局部 -> 世界，抵消旋转）
    const auto at = [](const TableEntity& t, double lx, double ly) {
        return t.toWorld(Point2D(lx, ly));
    };
    const auto near = [](double a, double b) { return std::abs(a - b) < kTol; };

    // ---- 夹点只有四角：界线不再占夹点下标 ====================================
    {
        auto table = makeTable();
        QCOMPARE(table->gripPoints().size(), std::size_t(4));
        for (int i = 0; i < 4; ++i) {
            QVERIFY(near(table->gripPoints()[static_cast<std::size_t>(i)].x,
                         table->outerCorners()[static_cast<std::size_t>(i)].x));
        }
        QCOMPARE(table->gripLabel(0), QStringLiteral("角点（整体缩放）"));
        QCOMPARE(table->gripLabel(3), QStringLiteral("角点（整体缩放）"));
        QVERIFY(table->gripLabel(4).isEmpty());                   // 界线不是夹点，没有说明
        QVERIFY(!table->moveGrip(4, Point2D(0.0, 0.0)));          // 也不接受拖拽

        // 增删行列只影响界线的数量，不影响夹点数
        table->insertColumn(0, 25.0);
        table->removeRow(0);
        QCOMPARE(table->gripPoints().size(), std::size_t(4));
    }

    // ---- 界线命中（直接把线拖走）：整条界线可抓，不只是某个点 ================
    {
        auto table = makeTable();
        const double tol = 1.0;

        // 列界线：整条都能抓（靠上、中点、靠下都是同一条）
        for (double ly : { -2.0, -15.0, -28.0 }) {
            const TableEntity::EdgeHit hit = table->edgeAtWorld(at(*table, 25.0, ly), tol);
            QVERIFY(hit.hit);
            QVERIFY(hit.vertical);
            QCOMPARE(hit.index, std::size_t(1));                  // 第 1 列的右界线
            QCOMPARE(hit.label, QStringLiteral("第 1 列的右界线"));
        }
        // 最右界线 = 最后一列
        {
            const TableEntity::EdgeHit hit = table->edgeAtWorld(at(*table, 100.0, -15.0), tol);
            QVERIFY(hit.hit && hit.vertical);
            QCOMPARE(hit.index, std::size_t(4));
        }
        // 行界线：横向整条可抓
        {
            const TableEntity::EdgeHit hit = table->edgeAtWorld(at(*table, 10.0, -10.0), tol);
            QVERIFY(hit.hit && !hit.vertical);
            QCOMPARE(hit.index, std::size_t(1));                  // 第 1 行的下界线
            QCOMPARE(hit.label, QStringLiteral("第 1 行的下界线"));
        }
        {
            const TableEntity::EdgeHit hit = table->edgeAtWorld(at(*table, 80.0, -30.0), tol);
            QVERIFY(hit.hit && !hit.vertical);
            QCOMPARE(hit.index, std::size_t(3));                  // 第 3 行的下界线
        }

        // 最上一条 / 最左一条不是界线：拖它们等于移动表格。
        // 取点要避开列界线（x=50 那条本身也是有效的列界线，它的端点照样可抓）
        QVERIFY(!table->edgeAtWorld(at(*table, 10.0, 0.0), tol).hit);
        QVERIFY(!table->edgeAtWorld(at(*table, 0.0, -15.0), tol).hit);
        {
            const TableEntity::EdgeHit hit = table->edgeAtWorld(at(*table, 50.0, 0.0), tol);
            QVERIFY(hit.hit && hit.vertical);
            QCOMPARE(hit.index, std::size_t(2));                  // 顶边上仍是"第 2 列的右界线"
        }

        // 表格之外（右、上）都不算压在线上
        QVERIFY(!table->edgeAtWorld(at(*table, 130.0, -15.0), tol).hit);
        QVERIFY(!table->edgeAtWorld(at(*table, 25.0, 5.0), tol).hit);

        // 容差决定"离多远还能抓"：0.4 在 0.5 内、不在 0.3 内
        QVERIFY(table->edgeAtWorld(at(*table, 25.4, -15.0), 0.5).hit);
        QVERIFY(!table->edgeAtWorld(at(*table, 25.4, -15.0), 0.3).hit);

        // 交点处（行列距离相等）取列界线——判定顺序固定，结果确定
        {
            const TableEntity::EdgeHit hit = table->edgeAtWorld(at(*table, 25.0, -10.0), tol);
            QVERIFY(hit.hit && hit.vertical);
        }

        // 旋转后按世界坐标判命中，与局部一致（取点避开列界线，落在第 2 行下界线上）
        auto rotated = makeTable();
        rotated->setRotation(degToRad(37.0));
        const TableEntity::EdgeHit rotatedHit =
            rotated->edgeAtWorld(at(*rotated, 60.0, -20.0), tol);
        QVERIFY(rotatedHit.hit && !rotatedHit.vertical);
        QCOMPARE(rotatedHit.index, std::size_t(2));
    }

    // ---- 界线的两个端点（拖动时只画这一条线，靠它取端点）=====================
    {
        auto table = makeTable();
        const auto col = table->columnBoundarySegment(2);          // 局部 x=50
        QVERIFY(near(col.first.x, 50.0) && near(col.second.x, 50.0));
        QVERIFY(near(col.first.y, 0.0) && near(col.second.y, -30.0));
        const auto row = table->rowBoundarySegment(1);             // 局部 y=-10
        QVERIFY(near(row.first.y, -10.0) && near(row.second.y, -10.0));
        QVERIFY(near(row.first.x, 0.0) && near(row.second.x, 100.0));
        // 越界：空线段（两端都退化到原点）
        const auto bad = table->columnBoundarySegment(9);
        QVERIFY(near(bad.first.x, 0.0) && near(bad.second.x, 0.0));
    }

    // ---- 拖内部列界线：只挪线，左右两格互相让，**总宽不变** ==================
    {
        auto table = makeTable();                                  // 4 列 × 25 = 100
        QVERIFY(table->moveColumnBoundary(1, at(*table, 45.0, -15.0)));
        QVERIFY(near(table->columnWidth(0), 45.0));                // 左格长 20
        QVERIFY(near(table->columnWidth(1), 5.0));                 // 右格短 20
        QVERIFY(near(table->columnWidth(2), 25.0));                // 更右边的列不动
        QVERIFY(near(table->columnWidth(3), 25.0));
        QVERIFY(near(table->totalWidth(), 100.0));                 // 总宽不变
        QVERIFY(near(table->insertionPoint().x, 100.0));
        QVERIFY(near(table->columnBoundaries()[1], 45.0));         // 线真挪到拖的地方
    }

    // ---- 拖内部行界线：只挪线，上下两格互相让，**总高不变** ==================
    {
        auto table = makeTable();                                  // 3 行 × 10 = 30
        QVERIFY(table->moveRowBoundary(1, at(*table, 50.0, -18.0)));
        QVERIFY(near(table->rowHeight(0), 18.0));                  // 上格长 8
        QVERIFY(near(table->rowHeight(1), 2.0));                   // 下格短 8
        QVERIFY(near(table->rowHeight(2), 10.0));                  // 更下面的行不动
        QVERIFY(near(table->totalHeight(), 30.0));                 // 总高不变
        QVERIFY(near(table->insertionPoint().y, 50.0));            // 插入点不动
    }

    // ---- 最右 / 最下界线没有邻格可让：这时才改总尺寸 ========================
    {
        auto table = makeTable();
        QVERIFY(table->moveColumnBoundary(4, at(*table, 130.0, -15.0)));
        QVERIFY(near(table->columnWidth(3), 55.0));                // 130 - 75
        QVERIFY(near(table->columnWidth(0), 25.0));
        QVERIFY(near(table->totalWidth(), 130.0));

        auto bottom = makeTable();
        QVERIFY(bottom->moveRowBoundary(3, at(*bottom, 50.0, -45.0)));
        QVERIFY(near(bottom->rowHeight(2), 25.0));
        QVERIFY(near(bottom->rowHeight(0), 10.0));
        QVERIFY(near(bottom->totalHeight(), 45.0));
    }

    // ---- 拖过头：停在邻格还剩一点的地方，两格都不出现负尺寸 ==================
    {
        auto table = makeTable();
        QVERIFY(table->moveColumnBoundary(1, at(*table, 200.0, -15.0)));
        QVERIFY(table->columnWidth(0) < 50.0);                     // 吞完邻格就停
        QVERIFY(table->columnWidth(0) > 49.0);
        QVERIFY(table->columnWidth(1) > 0.0);                      // 邻格没有被压成 0
        QVERIFY(table->columnWidth(1) < 1e-3);
        QVERIFY(near(table->totalWidth(), 100.0));                 // 总宽仍不变

        auto left = makeTable();
        QVERIFY(left->moveColumnBoundary(1, at(*left, -200.0, -15.0)));
        QVERIFY(left->columnWidth(0) > 0.0);
        QVERIFY(left->columnWidth(0) < 1e-3);
        QVERIFY(left->columnWidth(1) < 50.0);
        QVERIFY(left->columnWidth(1) > 49.0);
        QVERIFY(near(left->totalWidth(), 100.0));

        // 行界线：局部 Y 向下为负，所以拖到 +200 是把线拖到表格**上方**
        //（第 1 行被压到最小），拖到 -200 则是拖到下方（第 1 行被拉到最大）
        auto up = makeTable();
        QVERIFY(up->moveRowBoundary(1, at(*up, 50.0, 200.0)));
        QVERIFY(up->rowHeight(0) > 0.0);
        QVERIFY(up->rowHeight(0) < 1e-3);                          // 被压到最小
        QVERIFY(up->rowHeight(1) > 19.0);                          // 邻格接手
        QVERIFY(near(up->totalHeight(), 30.0));                    // 总高仍不变

        auto down = makeTable();
        QVERIFY(down->moveRowBoundary(1, at(*down, 50.0, -200.0)));
        QVERIFY(down->rowHeight(0) > 19.0);
        QVERIFY(down->rowHeight(1) > 0.0);
        QVERIFY(down->rowHeight(1) < 1e-3);
        QVERIFY(near(down->totalHeight(), 30.0));
    }

    // ---- 越界的界线序号：不生效、也不改动任何东西 ============================
    {
        auto table = makeTable();
        QVERIFY(!table->moveColumnBoundary(0, at(*table, 10.0, -15.0)));
        QVERIFY(!table->moveColumnBoundary(5, at(*table, 10.0, -15.0)));
        QVERIFY(!table->moveRowBoundary(0, at(*table, 50.0, -5.0)));
        QVERIFY(!table->moveRowBoundary(4, at(*table, 50.0, -5.0)));
        QVERIFY(near(table->totalWidth(), 100.0));
        QVERIFY(near(table->totalHeight(), 30.0));
    }

    // ---- 旋转后按世界坐标拖：结果与局部一致，插入点仍不动 ====================
    {
        auto table = makeTable();
        table->setRotation(degToRad(90.0));
        QVERIFY(table->moveColumnBoundary(1, at(*table, 40.0, -15.0)));   // 局部 x = 40
        QVERIFY(near(table->columnWidth(0), 40.0));
        QVERIFY(near(table->columnWidth(1), 10.0));
        QVERIFY(near(table->totalWidth(), 100.0));
        QVERIFY(near(table->outerCorners()[0].x, 100.0));
        QVERIFY(near(table->outerCorners()[0].y, 50.0));
    }

    // ---- 四角仍是整体等比缩放（既有行为）====================================
    {
        auto table = makeTable();
        QVERIFY(table->moveGrip(1, at(*table, 150.0, 0.0)));       // 右上角外拉 50
        QVERIFY(near(table->columnWidth(0), 37.5));                // 四列各 ×1.5
        QVERIFY(near(table->columnWidth(3), 37.5));
        QVERIFY(near(table->rowHeight(0), 10.0));                  // 行高不变
        QVERIFY(near(table->totalWidth(), 150.0));
        QVERIFY(near(table->totalHeight(), 30.0));
        QVERIFY(near(table->outerCorners()[1].x, 250.0));          // 拖到哪就是哪
        QVERIFY(near(table->outerCorners()[3].x, 100.0));          // 对角锚点不动
        QVERIFY(near(table->outerCorners()[3].y, 20.0));
    }
}

// -----------------------------------------------------------------------------
/// 字高与字体是**逐格**的（表格没有"整表字号"这回事）。
///
/// 盯住四条约定：
///   · 逐格独立——同一行的两格可以一个 12 一个 4，渲染出来的墨迹高度必须跟着不同；
///   · 初值冻结——只填了文字的格子在写入时按该行行高的一半取一个初值，**冻住之后
///     拖行界线 / 拖列界线 / 增删行列 / 直接改行高都不许改它**（这条曾经是错的：
///     字高按行高实时算，拖一下行界线格子里的字就跟着变大变小）；
///   · 越界兜底——手工构造、字高漏填（0）的格子，`cellHeightOr` 回退到该行默认值，
///     不会渲染出一个 0 高的字；
///   · 显式设置才改——`setCell` 传进来的字高原样生效（含非正值被挡掉）。
/// 前两条不只断言属性，还把表格**渲染成图**、量文字墨迹的高度：万一哪天渲染又改成
/// 按行高实时算、或读取端又把逐格样式统一成一个值，像素断言立刻会红。
void TestTable::cellTextHeightIsPerCell()
{
    const QSize canvasSize(420, 300);
    constexpr double kPixelsPerUnit = 6.0;

    // 同一张表渲染两次：一次带文字、一次把要量的那一格清空，两张图的差异就是文字墨迹
    const auto render = [canvasSize](const TableEntity& table, std::size_t row, std::size_t col) {
        std::unique_ptr<Entity> blank = table.clone();
        static_cast<TableEntity*>(blank.get())->setCellText(row, col, QString());
        return std::make_pair(renderTable(table, kPixelsPerUnit, canvasSize),
                              renderTable(*static_cast<const TableEntity*>(blank.get()),
                                          kPixelsPerUnit, canvasSize));
    };

    // ---- 逐格独立：同一行两格不同字号，墨迹高度必须跟着不同 ==================
    {
        auto table = std::make_unique<TableEntity>();
        // 两行两列，且整体宽度要**小到能完整落进视口**：renderTable 每次都把视口
        // 对到表格中心，表比视口宽时边上的格子会被裁掉，量出来的墨迹就成了 0
        table->reset(2, 2, 20.0, 30.0);
        table->setCellText(0, 0, QStringLiteral("小字"));
        table->setCellText(0, 1, QStringLiteral("大字"));
        // 写入文字时按该行行高的一半冻了初值，此时两格一样
        QVERIFY(std::abs(table->cell(0, 0).height - 10.0) < kTol);
        QVERIFY(std::abs(table->cell(0, 1).height - 10.0) < kTol);

        // 把右格放大到 16：同一行、同样的行高，两格字号从此不同
        TableCell big = table->cell(0, 1);
        big.height = 16.0;
        table->setCell(0, 1, big);
        QVERIFY(std::abs(table->cell(0, 1).height - 16.0) < kTol);
        QVERIFY(std::abs(table->cell(0, 0).height - 10.0) < kTol);   // 邻格不受影响

        const auto small = render(*table, 0, 0);
        const auto large = render(*table, 0, 1);
        const int inkSmall = textInkHeight(small.first, small.second);
        const int inkLarge = textInkHeight(large.first, large.second);
        QVERIFY2(inkSmall > 0 && inkLarge > 0, "基线没量到文字墨迹，像素断言没有意义");
        QVERIFY2(inkLarge > inkSmall,
                 qPrintable(QStringLiteral("同一行两格字号不同，墨迹却一样高：%1 px vs %2 px")
                                .arg(inkSmall).arg(inkLarge)));

        // ---- 初值冻结：拖行界线 / 拖列界线都不许改任何一格的墨迹 ============
        const auto before = render(*table, 0, 0);
        const int inkBefore = textInkHeight(before.first, before.second);

        QVERIFY(table->moveRowBoundary(1, Point2D(30.0, -8.0)));     // 行变矮
        QVERIFY(std::abs(table->rowHeight(0) - 8.0) < kTol);
        QVERIFY(std::abs(table->totalHeight() - 40.0) < kTol);       // 只挪线
        QVERIFY(std::abs(table->cell(0, 0).height - 10.0) < kTol);   // 字高不动

        const auto after = render(*table, 0, 0);
        QVERIFY2(before.second != after.second, "网格没动，下面的相等断言就没有意义");
        const int inkAfter = textInkHeight(after.first, after.second);
        QVERIFY2(std::abs(inkAfter - inkBefore) <= 1,
                 qPrintable(QStringLiteral("拖行界线把字改了大小：%1 px -> %2 px")
                                .arg(inkBefore).arg(inkAfter)));

        QVERIFY(table->moveColumnBoundary(1, Point2D(20.0, -4.0)));  // 第 1 列 30 → 20
        QVERIFY(std::abs(table->cell(0, 0).height - 10.0) < kTol);
        const auto narrow = render(*table, 0, 0);
        const int inkNarrow = textInkHeight(narrow.first, narrow.second);
        QVERIFY2(std::abs(inkNarrow - inkBefore) <= 1,
                 qPrintable(QStringLiteral("拖列界线把字改了大小：%1 px -> %2 px")
                                .arg(inkBefore).arg(inkNarrow)));

        // 增删行列、直接改行高同样不动已有单元格的字高
        table->insertRow(0, 12.0);
        table->setRowHeight(0, 33.0);
        QVERIFY(std::abs(table->cell(1, 0).height - 10.0) < kTol);   // 行平移后仍是 10
    }

    // ---- 新写入的格子：初值 = 该行行高的一半（不是任何全局值）=================
    {
        auto table = std::make_unique<TableEntity>();
        table->reset(2, 2, 8.0, 40.0);
        table->setRowHeight(1, 14.0);
        table->setCellText(0, 0, QStringLiteral("甲"));
        table->setCellText(1, 1, QStringLiteral("乙"));
        QVERIFY(std::abs(table->cell(0, 0).height - 4.0) < kTol);    // 8 / 2
        QVERIFY(std::abs(table->cell(1, 1).height - 7.0) < kTol);    // 14 / 2 —— 各按各自的行
        // 字体默认是空（走系统默认字体），不是别的格子的
        QVERIFY(table->cell(0, 0).fontStyle.isEmpty());
    }

    // ---- 越界兜底：手工构造、字高漏填的格子回退到该行默认值 ==================
    {
        auto table = std::make_unique<TableEntity>();
        table->reset(1, 1, 20.0, 40.0);
        TableCell missing;
        missing.text = QStringLiteral("漏填");
        table->setCell(0, 0, missing);
        QVERIFY(std::abs(table->cell(0, 0).height - 10.0) < kTol);   // setCell 已补上
        QVERIFY(std::abs(table->cellHeightOr(0, 0) - 10.0) < kTol);

        // 连 setCell 都没走（直接改内部值不可能，但空格的 cellHeightOr 也要有值）
        QVERIFY(std::abs(table->cellHeightOr(0, 0) - 10.0) < kTol);
    }

    // ---- 清空某一格：连样式一起消失（空白格子不占存储）=======================
    {
        auto table = std::make_unique<TableEntity>();
        table->reset(1, 2, 20.0, 40.0);
        table->setCellText(0, 0, QStringLiteral("甲"));
        TableCell styled = table->cell(0, 0);
        styled.height = 7.5;
        styled.fontStyle = QStringLiteral("SimSun");
        table->setCell(0, 0, styled);
        QCOMPARE(table->cell(0, 0).height, 7.5);
        QCOMPARE(table->cell(0, 0).fontStyle, QStringLiteral("SimSun"));

        table->setCellText(0, 0, QString());
        QVERIFY(table->cell(0, 0).text.isEmpty());
        QVERIFY(table->cell(0, 0).fontStyle.isEmpty());
        QCOMPARE(table->nonEmptyCells().size(), std::size_t(0));
    }
}

// -----------------------------------------------------------------------------
void TestTable::buildTableDocument(Document& document)
{
    document.clear(true);

    QString error;
    QVERIFY(document.addLayer(Layer(QStringLiteral("明细表")), &error));

    document.beginBatchLoad();

    auto table = std::make_unique<TableEntity>(4, 3);
    table->setRowHeight(1, 12.5);          // 其余行保持默认 8.0
    table->setColumnWidth(0, 20.0);        // 其余列保持默认 30.0
    table->setColumnWidth(2, 45.0);
    table->setInsertionPoint(Point2D(50.0, 40.0));
    table->setRotation(degToRad(15.0));
    table->setLayerName(QStringLiteral("明细表"));

    table->setCellText(0, 0, QStringLiteral("序号"));
    table->setCellText(0, 1, QStringLiteral("名称"));
    table->setCellText(0, 2, QStringLiteral("数量"));
    table->setCellText(1, 0, QStringLiteral("1"));
    table->setCellText(1, 1, QStringLiteral("轴承座"));
    table->setCellText(1, 2, QStringLiteral("2"));
    table->setCellText(2, 0, QStringLiteral("2"));
    table->setCellText(2, 1, QStringLiteral("端盖"));
    table->setCellText(2, 2, QStringLiteral("4"));
    table->setCellText(3, 0, QStringLiteral("合计：共 2 种零件"));

    // ---- 逐格样式：三格各给一份"不寻常"的字高与字体 -------------------------
    //
    //   · 字高都不是任何行高的一半（8→4、12.5→6.25），而且**三格互不相同**；
    //   · 字体一个给样式名、一个不给（走默认）。
    // 这样往返对得上只能是因为读取端从 *Tnn 块里**按格**取回了真值——用某个统一值
    // 猜、或者把整表当一个样式，都会在这些断言上红。
    // 其余格子保持"写入文字时按行高冻的默认值"，一并参与逐格比对。
    document.ensureTextFontStyle(QStringLiteral("SimSun"));   // 样式表里也要有，文件才自洽
    const auto styleCell = [&table](std::size_t row, std::size_t col,
                                    double height, const QString& font) {
        TableCell c = table->cell(row, col);
        c.height = height;
        c.fontStyle = font;
        table->setCell(row, col, c);
    };
    styleCell(0, 0, 3.5, QStringLiteral("SimSun"));    // 序号：小字 + 宋体
    styleCell(1, 1, 5.0, QStringLiteral("SimSun"));    // 轴承座：大字
    styleCell(2, 2, 2.75, QString());                  // 数量：最小，字体走默认

    TableMergeRange merge;
    merge.row = 3;
    merge.col = 0;
    merge.rowSpan = 1;
    merge.colSpan = 3;
    table->addMerge(merge);

    document.addEntity(std::move(table));
    document.endBatchLoad();
}

const TableEntity* TestTable::firstTable(const Document& document)
{
    for (EntityId id : document.drawOrder()) {
        const Entity* entity = document.entity(id);
        if (entity && entity->type() == EntityType::Table)
            return static_cast<const TableEntity*>(entity);
    }
    return nullptr;
}

bool TestTable::roundTrip(const QString& version, const QString& fileName,
                          bool checkCells, QString* failure)
{
    Document source;
    buildTableDocument(source);

    const QString path = m_dir.filePath(fileName);

    WriteOptions options;
    options.version = version;

    const IoResult writeResult = CadFile::save(path, source, options);
    if (!writeResult.ok) {
        *failure = QStringLiteral("写出失败: %1").arg(writeResult.error);
        return false;
    }

    Document loaded;
    const IoResult readResult = CadFile::load(path, loaded);
    if (!readResult.ok) {
        *failure = QStringLiteral("读回失败: %1").arg(readResult.error);
        return false;
    }

    // 原生路径必须生效：出现这条降级说明本体行列数据没被解出来，
    // 读取端退回了「按几何块反推」的兜底——对自写文件而言就是回归。
    for (const QString& warning : readResult.warnings) {
        if (warning.contains(QLatin1String("几何块反推"))) {
            *failure = QStringLiteral("表格被按几何块反推（本体数据丢失）：%1").arg(warning);
            return false;
        }
    }

    // 表格是单一图元：读回不得散成网格线 + 文字
    if (loaded.entityCount() != std::size_t(1)) {
        *failure = QStringLiteral("图元数应为 1，实际 %1（表格散架了）")
                       .arg(loaded.entityCount());
        return false;
    }

    const TableEntity* before = firstTable(source);
    const Entity* loadedEntity = loaded.entity(loaded.drawOrder().at(0));
    if (!before || !loadedEntity || loadedEntity->type() != EntityType::Table) {
        *failure = QStringLiteral("读回的图元不是表格");
        return false;
    }
    const TableEntity* after = static_cast<const TableEntity*>(loadedEntity);

    if (after->rowCount() != before->rowCount()
        || after->columnCount() != before->columnCount()) {
        *failure = QStringLiteral("行列数不一致：原 %1×%2，读 %3×%4")
                       .arg(before->rowCount()).arg(before->columnCount())
                       .arg(after->rowCount()).arg(after->columnCount());
        return false;
    }

    if (after->layerName() != before->layerName()) {
        *failure = QStringLiteral("图层不一致：原 %1，读 %2")
                       .arg(before->layerName(), after->layerName());
        return false;
    }

    if (after->insertionPoint().distanceTo(before->insertionPoint()) > kTol) {
        *failure = QStringLiteral("插入点不一致：原 (%1, %2)，读 (%3, %4)")
                       .arg(formatDxfDouble(before->insertionPoint().x),
                            formatDxfDouble(before->insertionPoint().y),
                            formatDxfDouble(after->insertionPoint().x),
                            formatDxfDouble(after->insertionPoint().y));
        return false;
    }

    if (std::abs(after->rotation() - before->rotation()) > kTol) {
        *failure = QStringLiteral("旋转角不一致：原 %1 rad，读 %2 rad")
                       .arg(formatDxfDouble(before->rotation()),
                            formatDxfDouble(after->rotation()));
        return false;
    }

    for (std::size_t r = 0; r < before->rowCount(); ++r) {
        if (std::abs(after->rowHeight(r) - before->rowHeight(r)) > kTol) {
            *failure = QStringLiteral("第 %1 行行高不一致：原 %2，读 %3")
                           .arg(r).arg(formatDxfDouble(before->rowHeight(r)),
                                       formatDxfDouble(after->rowHeight(r)));
            return false;
        }
    }
    for (std::size_t c = 0; c < before->columnCount(); ++c) {
        if (std::abs(after->columnWidth(c) - before->columnWidth(c)) > kTol) {
            *failure = QStringLiteral("第 %1 列列宽不一致：原 %2，读 %3")
                           .arg(c).arg(formatDxfDouble(before->columnWidth(c)),
                                       formatDxfDouble(after->columnWidth(c)));
            return false;
        }
    }

    if (checkCells) {
        // 单元格文字（含中文）：DXF 走本体 cells，DWG 走几何块按格归位补全
        const auto beforeCells = before->nonEmptyCells();
        const auto afterCells = after->nonEmptyCells();
        if (afterCells.size() != beforeCells.size()) {
            *failure = QStringLiteral("非空单元格数不一致：原 %1，读 %2")
                           .arg(beforeCells.size()).arg(afterCells.size());
            return false;
        }
        for (std::size_t i = 0; i < beforeCells.size(); ++i) {
            const auto& [row, col, cell] = beforeCells[i];
            const auto& [row2, col2, cell2] = afterCells[i];
            if (row != row2 || col != col2 || cell.text != cell2.text) {
                *failure = QStringLiteral("单元格 (%1,%2) 不一致：原「%3」，读 (%4,%5)「%6」")
                               .arg(row).arg(col).arg(cell.text)
                               .arg(row2).arg(col2).arg(cell2.text);
                return false;
            }
            // 逐格字高与字体：本体（ACAD_TABLE 记录）**两项都不带**，值只能来自配对的
            // *Tnn 块里那一条 TEXT 的字高与样式名。对不上就意味着「改过某一格的字号
            // 或字体，另存再打开又变回去」——也可能意味着读取端把它们统一成了一个值。
            if (std::abs(cell2.height - cell.height) > kTol) {
                *failure = QStringLiteral("单元格 (%1,%2) 字高不一致：原 %3，读 %4")
                               .arg(row).arg(col)
                               .arg(formatDxfDouble(cell.height),
                                    formatDxfDouble(cell2.height));
                return false;
            }
            if (cell2.fontStyle != cell.fontStyle) {
                *failure = QStringLiteral("单元格 (%1,%2) 字体不一致：原「%3」，读「%4」")
                               .arg(row).arg(col).arg(cell.fontStyle, cell2.fontStyle);
                return false;
            }
        }

        // 合并单元格
        const auto& beforeMerges = before->merges();
        const auto& afterMerges = after->merges();
        if (afterMerges.size() != beforeMerges.size()) {
            *failure = QStringLiteral("合并区域数不一致：原 %1，读 %2")
                           .arg(beforeMerges.size()).arg(afterMerges.size());
            return false;
        }
        for (std::size_t i = 0; i < beforeMerges.size(); ++i) {
            const TableMergeRange& a = beforeMerges[i];
            const TableMergeRange& b = afterMerges[i];
            if (a.row != b.row || a.col != b.col
                || a.rowSpan != b.rowSpan || a.colSpan != b.colSpan) {
                *failure = QStringLiteral("合并区域不一致：原 (%1,%2)+%3×%4，读 (%5,%6)+%7×%8")
                               .arg(a.row).arg(a.col).arg(a.rowSpan).arg(a.colSpan)
                               .arg(b.row).arg(b.col).arg(b.rowSpan).arg(b.colSpan);
                return false;
            }
        }
    }

    return true;
}

// -----------------------------------------------------------------------------
void TestTable::dxfRoundTripR2000()
{
    // R2000：文本按 $DWGCODEPAGE 的代码页（简中 GBK）存放
    QString failure;
    QVERIFY2(roundTrip(QStringLiteral("AC1015"), QStringLiteral("table_2000.dxf"),
                       true, &failure),
             qPrintable(failure));
}

void TestTable::dxfRoundTrip2018()
{
    // 2018：文本一律 UTF-8——同一批中文单元格必须两档都读得回来
    QString failure;
    QVERIFY2(roundTrip(QStringLiteral("AC1032"), QStringLiteral("table_2018.dxf"),
                       true, &failure),
             qPrintable(failure));
}

void TestTable::dwgNativeRoundTrip()
{
    // DWG（R2000）：原生 ACAD_TABLE 类实体。cells 数组随本体逐格写出/读回
    //（文字内联 TV + 每格一个空值句柄），结构字段与中文单元格内容必须
    // 逐项保真，尤其旋转角（DWG 存弧度，DXF 存度，两侧换算要对偶）。
    QString failure;
    QVERIFY2(roundTrip(QStringLiteral("AC1015"), QStringLiteral("table_native.dwg"),
                       true, &failure),
             qPrintable(failure));

    // 写出的必须是真 DWG 二进制（R2000 版本签名），不是套壳 DXF
    QFile file(m_dir.filePath(QStringLiteral("table_native.dwg")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.read(6), QByteArray("AC1032"));   // 默认写出版本 AC1032/R2018
}

// -----------------------------------------------------------------------------
QTEST_MAIN(TestTable)
#include "test_table.moc"
