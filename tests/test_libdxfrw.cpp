// =============================================================================
//  LyCAD - tests/test_libdxfrw.cpp
//  内置 libdxfrw 适配层的测试。
//
//  这一组测试的重点不在「能读」，而在**单位与坐标的换算是否正确**：
//  libdxfrw 在 DXF 与 DWG 两条路径下对文字旋转的单位处理不同（DXF 是度、
//  DWG 是弧度），适配器必须按来源分别换算。这类错误不会让程序崩溃，
//  只会让图形悄悄错位——正是最需要测试盯住的地方。
//
//  测试数据由 libdxfrw-modify 的导出路径生成，再用它的读取路径读回。
//  读取已统一切换到 libdxfrw（自研 DxfReader 已删除，见 io/DxfCodec.cpp），
//  独立性的交叉校验基准是 ezdxf（tools/ 下的校验脚本与 CI 流程）。
// =============================================================================
#include <QtTest>

#include "core/ArcEntity.h"
#include "core/CircleEntity.h"
#include "core/DimensionEntity.h"
#include "core/Document.h"
#include "core/HatchEntity.h"
#include "core/LineEntity.h"
#include "core/PointEntity.h"
#include "core/PolylineEntity.h"
#include "core/TextEntity.h"
#include "io/DxfCodec.h"
#include "io/LibDxfRwAdapter.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QTemporaryDir>

using namespace lycad;

namespace {

constexpr double kTol = 1e-6;

/// 样例一律写 R2000：R12 不支持填充，写出器会把它降级成闭合多段线，
/// 那样就测不到填充的映射了。
WriteOptions r2000Options()
{
    WriteOptions options;
    options.version = QStringLiteral("AC1015");
    return options;
}

/// 造一份含全部已建模图元的文档：直线、圆、圆弧、点、多段线（带凸度）、
/// 文字、填充、标注。
void buildSample(Document& document)
{
    document.addEntity(std::make_unique<LineEntity>(Point2D(0.0, 0.0), Point2D(50.0, 25.0)));
    document.addEntity(std::make_unique<CircleEntity>(Point2D(20.0, 20.0), 12.5));
    document.addEntity(std::make_unique<PointEntity>(Point2D(7.5, -3.25)));

    // 圆弧：30° → 250°（写进 DXF 是度，libdxfrw 读出来会换成弧度）
    document.addEntity(std::make_unique<ArcEntity>(Point2D(120.0, 55.0), 25.0,
                                                   degToRad(30.0), degToRad(250.0)));

    {
        auto polyline = std::make_unique<PolylineEntity>();
        polyline->appendVertex(Point2D(0.0, 100.0), 0.5);      // 逆时针弧段
        polyline->appendVertex(Point2D(50.0, 100.0), -0.25);   // 顺时针弧段
        polyline->appendVertex(Point2D(50.0, 130.0), 0.0);
        polyline->appendVertex(Point2D(0.0, 130.0), 0.0);
        polyline->setClosed(true);
        document.addEntity(std::move(polyline));
    }

    // 文字：旋转 30°（写入 DXF 为 30.0 度）
    {
        auto text = std::make_unique<TextEntity>(QStringLiteral("旋转测试"), Point2D(10.0, 40.0),
                                                 5.0, degToRad(30.0));
        document.addEntity(std::move(text));
    }

    // 填充：实体填充，边界为矩形
    document.addEntity(std::make_unique<HatchEntity>(
        std::vector<Point2D>{ Point2D(60.0, 60.0), Point2D(90.0, 60.0),
                              Point2D(90.0, 80.0), Point2D(60.0, 80.0) },
        QStringLiteral("SOLID"), 1.0, 0.0));

    // 对齐标注
    {
        auto dim = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Aligned);
        dim->setFirstPoint(Point2D(0.0, 0.0));
        dim->setSecondPoint(Point2D(80.0, 0.0));
        dim->setDimLinePoint(Point2D(40.0, -15.0));
        document.addEntity(std::move(dim));
    }
}

/// 按类型统计图元个数
QHash<int, int> countByType(const Document& document)
{
    QHash<int, int> counts;
    for (EntityId id : document.drawOrder()) {
        const Entity* entity = document.entity(id);
        if (entity) counts[static_cast<int>(entity->type())] += 1;
    }
    return counts;
}

/// 取出第一个指定类型的图元
const Entity* firstOfType(const Document& document, EntityType type)
{
    for (EntityId id : document.drawOrder()) {
        const Entity* entity = document.entity(id);
        if (entity && entity->type() == type) return entity;
    }
    return nullptr;
}

} // namespace

class TestLibDxfRw : public QObject {
    Q_OBJECT

private slots:
    // ---- 基本可用性 ----------------------------------------------------------
    void adapterIsAvailable();

    // ---- DXF 路径：几何与单位 ------------------------------------------------
    void readsAllModelledEntities();
    void readsArcAnglesInRadians();
    void readsTextRotationAsDegrees();
    void readsPolylineBulge();
    void readsDimensionDefinitionPoints();

    // ---- 写出后的「打开即见图形」--------------------------------------------
    void writtenActiveViewportCoversDrawing();

    // ---- 两条独立实现互证 ----------------------------------------------------

    // ---- DWG 路径（需要外部样本，缺失时跳过）----------------------------------
    void readsDwgSample();
};

// -----------------------------------------------------------------------------
void TestLibDxfRw::adapterIsAvailable()
{
    // 源码已随项目编入，恒为可用
    QVERIFY(libDxfRwAvailable());
}

// -----------------------------------------------------------------------------
void TestLibDxfRw::readsAllModelledEntities()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Document source;
    buildSample(source);

    const QString path = dir.filePath(QStringLiteral("sample.dxf"));
    DxfCodec dxf;
    const IoResult written = dxf.write(path, source, r2000Options());
    QVERIFY2(written.ok, qPrintable(written.error));

    Document loaded;
    const IoResult result = readDxfWithLibDxfRw(path, loaded);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.entityCount, loaded.entityCount());

    const QHash<int, int> counts = countByType(loaded);
    QVERIFY2(counts.value(static_cast<int>(EntityType::Line)) >= 1, "直线缺失");
    QVERIFY2(counts.value(static_cast<int>(EntityType::Circle)) >= 1, "圆缺失");
    QVERIFY2(counts.value(static_cast<int>(EntityType::Arc)) >= 1, "圆弧缺失");
    QVERIFY2(counts.value(static_cast<int>(EntityType::Point)) >= 1, "点缺失");
    QVERIFY2(counts.value(static_cast<int>(EntityType::Polyline)) >= 1, "多段线缺失");
    QVERIFY2(counts.value(static_cast<int>(EntityType::Text)) >= 1, "文字缺失");
    QVERIFY2(counts.value(static_cast<int>(EntityType::Hatch)) >= 1, "填充缺失");
    QVERIFY2(counts.value(static_cast<int>(EntityType::Dimension)) >= 1, "标注缺失");
}

// -----------------------------------------------------------------------------
void TestLibDxfRw::readsArcAnglesInRadians()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Document source;
    buildSample(source);

    const QString path = dir.filePath(QStringLiteral("arc.dxf"));
    DxfCodec dxf;
    QVERIFY(dxf.write(path, source, r2000Options()).ok);

    Document loaded;
    QVERIFY(readDxfWithLibDxfRw(path, loaded).ok);

    const Entity* entity = firstOfType(loaded, EntityType::Arc);
    QVERIFY(entity != nullptr);
    const auto& arc = static_cast<const ArcEntity&>(*entity);

    // DXF 里写的是 30°/250°，读回来必须还原成弧度。
    // 若适配器误按弧度解读（把 30 当 30 rad），这里会差 57 倍。
    QVERIFY2(std::abs(arc.startAngle() - degToRad(30.0)) < kTol,
             qPrintable(QStringLiteral("起始角 %1，期望 %2")
                            .arg(arc.startAngle()).arg(degToRad(30.0))));
    QVERIFY2(std::abs(arc.endAngle() - degToRad(250.0)) < kTol,
             qPrintable(QStringLiteral("终止角 %1，期望 %2")
                            .arg(arc.endAngle()).arg(degToRad(250.0))));
    QVERIFY(std::abs(arc.radius() - 25.0) < kTol);
}

// -----------------------------------------------------------------------------
void TestLibDxfRw::readsTextRotationAsDegrees()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Document source;
    buildSample(source);

    const QString path = dir.filePath(QStringLiteral("text.dxf"));
    DxfCodec dxf;
    QVERIFY(dxf.write(path, source, r2000Options()).ok);

    Document loaded;
    QVERIFY(readDxfWithLibDxfRw(path, loaded).ok);

    const Entity* entity = firstOfType(loaded, EntityType::Text);
    QVERIFY(entity != nullptr);
    const auto& text = static_cast<const TextEntity&>(*entity);

    // 关键断言：DXF 路径下 libdxfrw 不做换算（单位是度），适配器负责换成弧度。
    // 这是两条读取路径中唯一需要按来源分支的地方，必须被测试盯住。
    QVERIFY2(std::abs(text.rotation() - degToRad(30.0)) < 1e-5,
             qPrintable(QStringLiteral("文字旋转 %1 rad，期望 %2 rad（即 30°）")
                            .arg(text.rotation()).arg(degToRad(30.0))));
    QCOMPARE(text.content(), QStringLiteral("旋转测试"));
    QVERIFY(std::abs(text.height() - 5.0) < kTol);
}

// -----------------------------------------------------------------------------
void TestLibDxfRw::readsPolylineBulge()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Document source;
    buildSample(source);

    const QString path = dir.filePath(QStringLiteral("pline.dxf"));
    DxfCodec dxf;
    QVERIFY(dxf.write(path, source, r2000Options()).ok);

    Document loaded;
    QVERIFY(readDxfWithLibDxfRw(path, loaded).ok);

    const Entity* entity = firstOfType(loaded, EntityType::Polyline);
    QVERIFY(entity != nullptr);
    const auto& polyline = static_cast<const PolylineEntity&>(*entity);

    QCOMPARE(polyline.vertexCount(), std::size_t(4));
    QVERIFY(polyline.isClosed());

    // 凸度的符号必须原样保留：正负决定弧段绕行方向
    QVERIFY(std::abs(polyline.vertices().at(0).bulge - 0.5) < kTol);
    QVERIFY(std::abs(polyline.vertices().at(1).bulge - (-0.25)) < kTol);
}

// -----------------------------------------------------------------------------
void TestLibDxfRw::readsDimensionDefinitionPoints()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Document source;
    buildSample(source);

    const QString path = dir.filePath(QStringLiteral("dim.dxf"));
    DxfCodec dxf;
    QVERIFY(dxf.write(path, source, r2000Options()).ok);

    Document loaded;
    QVERIFY(readDxfWithLibDxfRw(path, loaded).ok);

    const Entity* entity = firstOfType(loaded, EntityType::Dimension);
    QVERIFY(entity != nullptr);
    const auto& dim = static_cast<const DimensionEntity&>(*entity);

    QCOMPARE(dim.dimType(), DimensionEntity::DimType::Aligned);
    QVERIFY(dim.firstPoint().distanceTo(Point2D(0.0, 0.0)) < kTol);
    QVERIFY(dim.secondPoint().distanceTo(Point2D(80.0, 0.0)) < kTol);
    QVERIFY(dim.dimLinePoint().distanceTo(Point2D(40.0, -15.0)) < kTol);
    QVERIFY(std::abs(dim.measurement() - 80.0) < kTol);
}

// -----------------------------------------------------------------------------
//  「打开文件是空白」的回归测试。
//
//  AutoCAD 系软件（AutoCAD / GstarCAD）打开图形时，第一眼看到哪里由 DXF 的
//  `*ACTIVE` 视口记录决定，而不是自动缩放到范围。写出器若把这一步交给
//  libdxfrw 的默认视口（视图中心 (0.651828, -0.16)、视高 5.13732），
//  画面就停在原点附近；图元一旦不在原点附近，用户打开后看到的就是一片空白
//  ——实测 GstarCAD 正是如此。这里把图元有意放在远离原点处，让默认视口必然失败。
void TestLibDxfRw::writtenActiveViewportCoversDrawing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Document source;
    source.addEntity(std::make_unique<LineEntity>(Point2D(1000.0, 500.0),
                                                 Point2D(1200.0, 700.0)));
    source.addEntity(std::make_unique<CircleEntity>(Point2D(1300.0, 600.0), 40.0));

    const QString path = dir.filePath(QStringLiteral("viewport.dxf"));
    DxfCodec dxf;
    QVERIFY(dxf.write(path, source, r2000Options()).ok);

    // VPORT 只是表记录，LyCAD 自身不建模它，因此按组码核对原始文本。
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QStringList lines = QString::fromLatin1(file.readAll()).split(QLatin1Char('\n'));

    double centerX = 0.0;
    double centerY = 0.0;
    double viewHeight = 0.0;
    double ratio = 0.0;
    bool found = false;
    for (int i = 0; i + 1 < lines.size() && !found; ++i) {
        if (lines.at(i).trimmed() != QLatin1String("2")
            || lines.at(i + 1).trimmed() != QLatin1String("*ACTIVE")) {
            continue;
        }
        found = true;
        for (int j = i + 2; j + 1 < lines.size(); j += 2) {
            const QString code = lines.at(j).trimmed();
            if (code == QLatin1String("0")) break;      // 记录结束
            const double value = lines.at(j + 1).trimmed().toDouble();
            if (code == QLatin1String("12")) centerX = value;
            else if (code == QLatin1String("22")) centerY = value;
            else if (code == QLatin1String("40")) viewHeight = value;
            else if (code == QLatin1String("41")) ratio = value;
        }
    }
    QVERIFY2(found, "文件里没有 *ACTIVE 视口记录，CAD 打开时会退回自己的默认视图");
    QVERIFY2(viewHeight > 0.0 && ratio > 0.0,
             qPrintable(QStringLiteral("视口尺寸非法：视高 %1，纵横比 %2")
                            .arg(viewHeight).arg(ratio)));

    const double halfWidth = viewHeight * ratio / 2.0;
    const double halfHeight = viewHeight / 2.0;
    const double minX = centerX - halfWidth;
    const double maxX = centerX + halfWidth;
    const double minY = centerY - halfHeight;
    const double maxY = centerY + halfHeight;

    QVERIFY2(minX <= 1000.0 && maxX >= 1340.0 && minY <= 500.0 && maxY >= 700.0,
             qPrintable(QStringLiteral("视口 x[%1, %2] y[%3, %4] 未覆盖图元范围 "
                                       "x[1000, 1340] y[500, 700]")
                            .arg(minX).arg(maxX).arg(minY).arg(maxY)));
}

void TestLibDxfRw::readsDwgSample()
{
    // DWG 样本不随仓库分发（体积大，且多为第三方图纸）。需要时由使用者指定：
    //   LYCAD_DWG_SAMPLE=/path/to/file.dwg ctest -R test_libdxfrw
    const QByteArray sample = qgetenv("LYCAD_DWG_SAMPLE");
    if (sample.isEmpty()) {
        QSKIP("未设置 LYCAD_DWG_SAMPLE，跳过 DWG 实样测试");
    }

    const QString path = QString::fromLocal8Bit(sample);
    if (!QFileInfo::exists(path)) {
        QSKIP("LYCAD_DWG_SAMPLE 指向的文件不存在");
    }

    Document document;
    const IoResult result = readDwgWithLibDxfRw(path, document);
    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(document.entityCount() > 0);

    // DWG 路径的文字旋转是**弧度**（与 DXF 路径不同）。这里不预设具体数值，
    // 只确认它落在一个合理的弧度区间内——若适配器漏了换算，
    // 数值会跑到几十甚至上百，一眼可辨。
    for (EntityId id : document.drawOrder()) {
        const Entity* entity = document.entity(id);
        if (entity && entity->type() == EntityType::Text) {
            const auto& text = static_cast<const TextEntity&>(*entity);
            QVERIFY2(std::abs(text.rotation()) <= kTwoPi + 1e-6,
                     qPrintable(QStringLiteral("文字旋转 %1 超出弧度范围，疑似漏了单位换算")
                                    .arg(text.rotation())));
        }
    }
}

QTEST_MAIN(TestLibDxfRw)
#include "test_libdxfrw.moc"
