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
#include <QTemporaryDir>

#include <cmath>
#include <cstddef>
#include <tuple>

#include "core/Document.h"
#include "core/Entity.h"
#include "core/Format.h"
#include "core/TableEntity.h"
#include "io/CadFile.h"

using namespace lycad;

namespace {

/// 往返容差：DXF 文本按版本格式化、DWG 走 BD 压缩 double，
/// 取 1e-9 足以容忍两侧的合法舍入，又远小于任何真实错位
constexpr double kTol = 1e-9;

} // namespace

class TestTable : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void coreInvariants();
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
    QCOMPARE(file.read(6), QByteArray("AC1027"));   // 默认写出版本 AC1027/R2013
}

// -----------------------------------------------------------------------------
QTEST_MAIN(TestTable)
#include "test_table.moc"
