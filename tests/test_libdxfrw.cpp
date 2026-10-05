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

/// 取出写出的 DXF 中某条表记录（按 0/类型 + 2/名称 定位）的组码值。
/// 表记录不参与 LyCAD 的模型，只能按组码核对，故需要这样一个文本级小工具。
QHash<QString, QString> tableRecord(const QStringList& lines, const QString& type,
                                    const QString& name)
{
    for (int i = 0; i + 3 < lines.size(); ++i) {
        if (lines.at(i).trimmed() != QLatin1String("0")
            || lines.at(i + 1).trimmed() != type) {
            continue;
        }
        QHash<QString, QString> fields;
        int j = i + 2;
        while (j + 1 < lines.size() && lines.at(j).trimmed() != QLatin1String("0")) {
            fields.insert(lines.at(j).trimmed(), lines.at(j + 1).trimmed());
            j += 2;
        }
        if (name.isEmpty()
            || fields.value(QLatin1String("2")).compare(name, Qt::CaseInsensitive) == 0) {
            return fields;      // 名称大小写不敏感：R12 写出时会整体转大写
        }
        i = j - 1;      // 外层循环的 ++i 正好落到下一条记录
    }
    return {};
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
    void outOfRangeCoordinatesAreSkipped();
    void writtenHatchBoundaryKeepsBulgeSlots();
    void writtenHatchPatternDefinitionRoundTrips();

    // ---- 写出后的「打开即见图形」--------------------------------------------
    void writtenActiveViewportCoversDrawing();
    void writtenTextStylesRoundTrip();

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
    const QHash<QString, QString> viewport =
        tableRecord(lines, QStringLiteral("VPORT"), QStringLiteral("*ACTIVE"));

    QVERIFY2(!viewport.isEmpty(),
             "文件里没有 *ACTIVE 视口记录，CAD 打开时会退回自己的默认视图");

    const double centerX = viewport.value(QLatin1String("12")).toDouble();
    const double centerY = viewport.value(QLatin1String("22")).toDouble();
    const double viewHeight = viewport.value(QLatin1String("40")).toDouble();
    const double ratio = viewport.value(QLatin1String("41")).toDouble();
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

// -----------------------------------------------------------------------------
//  文字样式的字体必须随文件往返。
//
//  字体信息只存在于 STYLE 表里，图元只带样式名（DXF 7）。因此导出必须做两件事：
//    · 把样式表按原样写回（3/4 组码，以及 AutoCAD 用来记录 TrueType 字体的
//      ACAD 扩展数据：1001 ACAD / 1000 SimSun / 1071 字体族标志）；
//    · 给每个文字图元写回它的样式名。
//  少了任何一半，CAD 都只能退回默认的 txt.shx——那是没有汉字字形的 SHX 字体，
//  汉字整片变成 "?"。同一张图里 MTEXT 正常、TEXT 全是问号，极易被误判成编码问题。
void TestLibDxfRw::writtenTextStylesRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Document source;

    // 三种真实存在的字体描述方式，各来一条
    TextStyle& bigFont = source.textStyles().ensure(QStringLiteral("HZLZB"));
    bigFont.setFontFile(QStringLiteral("txt"));
    bigFont.setBigFontFile(QStringLiteral("hztxt.shx"));

    TextStyle& ttf = source.textStyles().ensure(QStringLiteral("PaperStyle"));
    ttf.setFontFile(QStringLiteral("simhei.ttf"));

    // 3/4 组码为空，字体**只**在扩展数据里——中文图纸的 Standard 就是这种形态
    TextStyle& xdata = source.textStyles().ensure(QStringLiteral("Standard"));
    xdata.setFontFile(QString());
    xdata.setBigFontFile(QString());
    xdata.setTtfName(QStringLiteral("SimSun"));
    xdata.setTtfFlags(34306);

    // 文件没提供任何字体信息的样式：导出时应补上中文兜底字体，而不是写空
    source.textStyles().ensure(QStringLiteral("NoFontInfo"));

    const auto addText = [&source](const QString& content, const QString& style) {
        auto text = std::make_unique<TextEntity>(content, Point2D(0.0, 0.0), 3.5, 0.0);
        text->setFontStyle(style);
        source.addEntity(std::move(text));
    };
    addText(QStringLiteral("大字体"), QStringLiteral("HZLZB"));
    addText(QStringLiteral("TrueType"), QStringLiteral("PaperStyle"));
    addText(QStringLiteral("扩展数据字体"), QStringLiteral("Standard"));
    addText(QStringLiteral("无字体信息"), QStringLiteral("NoFontInfo"));

    const QString path = dir.filePath(QStringLiteral("styles.dxf"));
    DxfCodec dxf;
    QVERIFY(dxf.write(path, source, r2000Options()).ok);

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QStringList lines = QString::fromLatin1(file.readAll()).split(QLatin1Char('\n'));
    const auto style = [&lines](const QString& name) {
        return tableRecord(lines, QStringLiteral("STYLE"), name);
    };

    QCOMPARE(style(QStringLiteral("HZLZB")).value(QLatin1String("3")), QStringLiteral("txt"));
    QCOMPARE(style(QStringLiteral("HZLZB")).value(QLatin1String("4")), QStringLiteral("hztxt.shx"));
    QCOMPARE(style(QStringLiteral("PaperStyle")).value(QLatin1String("3")), QStringLiteral("simhei.ttf"));

    // TrueType 字体：字体名写在扩展数据里，导出必须原样保留
    const QHash<QString, QString> standard = style(QStringLiteral("Standard"));
    QVERIFY2(standard.value(QLatin1String("3")).isEmpty(),
             "Standard 的主字体应当是空的（字体在扩展数据里）");
    QCOMPARE(standard.value(QLatin1String("1000")), QStringLiteral("SimSun"));
    QCOMPARE(standard.value(QLatin1String("1071")), QStringLiteral("34306"));

    // 文件没给字体的样式：补中文兜底字体，否则汉字没有字形
    QVERIFY2(!style(QStringLiteral("NoFontInfo")).value(QLatin1String("4")).isEmpty(),
             "缺字体信息的样式未补中文大字体，汉字会被 CAD 渲染成 ?");

    // R12 不支持 TrueType：字体只在扩展数据里的样式必须落到大字体上，
    // 否则这份 R12 文件里的汉字没有任何字形可用。
    {
        const QString r12Path = dir.filePath(QStringLiteral("styles_r12.dxf"));
        WriteOptions r12;
        r12.version = QStringLiteral("AC1009");
        QVERIFY(dxf.write(r12Path, source, r12).ok);

        QFile r12File(r12Path);
        QVERIFY(r12File.open(QIODevice::ReadOnly));
        const QStringList r12Lines =
            QString::fromLatin1(r12File.readAll()).split(QLatin1Char('\n'));
        const QHash<QString, QString> r12Standard =
            tableRecord(r12Lines, QStringLiteral("STYLE"), QStringLiteral("Standard"));
        QVERIFY2(!r12Standard.value(QLatin1String("4")).isEmpty(),
                 "R12 里的 Standard 未落到中文字体上，汉字会没有字形");
        QVERIFY2(r12Standard.value(QLatin1String("1000")).isEmpty(),
                 "R12 不支持 TrueType，不应写 TrueType 扩展数据");
    }

    // 图元的样式名回写
    QHash<QString, int> used;
    for (EntityId id : source.drawOrder()) {
        const Entity* entity = source.entity(id);
        if (entity && entity->type() == EntityType::Text) {
            used[static_cast<const TextEntity*>(entity)->fontStyle()] += 1;
        }
    }
    QCOMPARE(used.value(QStringLiteral("HZLZB")), 1);
    QCOMPARE(used.value(QStringLiteral("PaperStyle")), 1);
    QCOMPARE(used.value(QStringLiteral("Standard")), 1);
    QCOMPARE(used.value(QStringLiteral("NoFontInfo")), 1);

    // 读回来：样式表与图元的样式名都要还在
    Document loaded;
    QVERIFY(readDxfWithLibDxfRw(path, loaded).ok);

    const TextStyle* loadedBig = loaded.textStyles().find(QStringLiteral("HZLZB"));
    QVERIFY(loadedBig != nullptr);
    QCOMPARE(loadedBig->fontFile(), QStringLiteral("txt"));
    QCOMPARE(loadedBig->bigFontFile(), QStringLiteral("hztxt.shx"));

    const TextStyle* loadedXdata = loaded.textStyles().find(QStringLiteral("Standard"));
    QVERIFY(loadedXdata != nullptr);
    QCOMPARE(loadedXdata->ttfName(), QStringLiteral("SimSun"));
    QCOMPARE(loadedXdata->ttfFlags(), 34306);

    int styled = 0;
    for (EntityId id : loaded.drawOrder()) {
        const Entity* entity = loaded.entity(id);
        if (!entity || entity->type() != EntityType::Text) continue;
        const QString name = static_cast<const TextEntity*>(entity)->fontStyle();
        if (name == QLatin1String("HZLZB") || name == QLatin1String("PaperStyle")) ++styled;
    }
    QCOMPARE(styled, 2);
}

// -----------------------------------------------------------------------------
//  坐标体检：天文数字坐标不能进模型。
//
//  二进制格式里读错一个字节就能造出 1e100 这种坐标（实测某 2007 版 DWG 有两条
//  样条各一个控制点如此，数据本身不可用）。放进来会把全图范围撑爆——用户看到的
//  又是「打开是空白」（缩放范围失效），拾取与包围盒判断也跟着失真。
//  适配层在入口挡掉这类图元并计数上报，不静默丢弃。
void TestLibDxfRw::outOfRangeCoordinatesAreSkipped()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // 先写出两份「文件」：一份正常，一份含 1e30 的坐标
    Document source;
    source.addEntity(std::make_unique<LineEntity>(Point2D(0.0, 0.0), Point2D(10.0, 10.0)));
    source.addEntity(std::make_unique<LineEntity>(Point2D(1e30, 0.0), Point2D(1e30, 10.0)));

    const QString path = dir.filePath(QStringLiteral("wild.dxf"));
    DxfCodec dxf;
    QVERIFY(dxf.write(path, source, r2000Options()).ok);

    Document loaded;
    const IoResult result = readDxfWithLibDxfRw(path, loaded);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(loaded.entityCount(), std::size_t(1));

    bool reported = false;
    for (const QString& warning : result.warnings) {
        if (warning.contains(QStringLiteral("坐标超出范围"))) reported = true;
    }
    QVERIFY2(reported, "跳过了坐标异常的图元却没有如实上报");
}

// -----------------------------------------------------------------------------
//  填充写出：带凸度的边界必须「每个顶点都写 42 组码」。
//
//  只给非零凸度的顶点写 42 的稀疏写法，会让 GstarCAD 的填充边界解析错位：
//  实测同一份文件只把零凸度的 42 组码删掉，打开就是一大片乱纹盖住整张图
//  （选中它时属性面板显示面积为 0）。无凸度时（72=0）不写 42，与 AutoCAD 一致。
void TestLibDxfRw::writtenHatchBoundaryKeepsBulgeSlots()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // 一个带圆角的四边形：第 1、3 段是圆弧，其余是直线
    std::vector<Point2D> boundary{{0.0, 0.0}, {20.0, 0.0}, {20.0, 10.0}, {0.0, 10.0}};
    std::vector<double> bulges{0.0, 0.4142135623730951, 0.0, 0.4142135623730951};
    Document source;
    auto hatch = std::make_unique<HatchEntity>(boundary, bulges);
    hatch->setPatternName(QStringLiteral("ANSI31"));
    hatch->setPatternScale(0.5);
    source.addEntity(std::move(hatch));

    const QString path = dir.filePath(QStringLiteral("hatch.dxf"));
    DxfCodec dxf;
    QVERIFY(dxf.write(path, source, r2000Options()).ok);

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString text = QString::fromLatin1(file.readAll());
    file.close();

    // 取出 HATCH 记录里边界那一段（91 之后、97 之前）
    const int hatchAt = text.indexOf(QStringLiteral("AcDbHatch"));
    QVERIFY(hatchAt > 0);
    const int boundaryAt = text.indexOf(QStringLiteral("\n 93\n"), hatchAt);
    QVERIFY(boundaryAt > 0);
    const int endAt = text.indexOf(QStringLiteral("\n 97\n"), boundaryAt);
    QVERIFY(endAt > boundaryAt);
    const QString section = text.mid(boundaryAt, endAt - boundaryAt);

    // 93 = 顶点数；42 的个数必须与它相等（不能只写非零的那几个）
    const QRegularExpression vertexCount(QStringLiteral("\\n 93\\n\\s*(\\d+)"));
    const QRegularExpressionMatch match = vertexCount.match(section);
    QVERIFY(match.hasMatch());
    const int declared = match.captured(1).toInt();
    QCOMPARE(section.count(QStringLiteral("\n 42\n")), declared);
}

// -----------------------------------------------------------------------------
//  填充图案定义：文件里带来的定义（角度 + 间距）必须随文件走完一个来回。
//
//  只写图案名、不写定义（78 组码为 0）时，实测 GstarCAD 会把填充画成一大片
//  乱纹；定义里的偏移还必须换算到「图案线坐标系」，并且写出的间距要乘回比例，
//  否则密度与原图不符。模型内部统一按「已经乘过比例的最终间距」保存。
void TestLibDxfRw::writtenHatchPatternDefinitionRoundTrips()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const std::vector<Point2D> boundary{{0.0, 0.0}, {40.0, 0.0}, {40.0, 20.0}, {0.0, 20.0}};
    Document source;
    auto hatch = std::make_unique<HatchEntity>(boundary);
    hatch->setPatternName(QStringLiteral("ANSI31"));
    hatch->setPatternScale(0.5);

    HatchEntity::PatternLine line;
    line.angle = kPi / 4.0;
    line.offset = Point2D(0.0, 3.175);        // 最终间距 3.175（比例之外的部分由文件携带）
    line.base = Point2D(-12392.0, -720.0);    // 远基点：写出时必须归零
    hatch->setPatternLines({line});
    source.addEntity(std::move(hatch));

    const QString path = dir.filePath(QStringLiteral("hatch_pattern.dxf"));
    DxfCodec dxf;
    QVERIFY(dxf.write(path, source, r2000Options()).ok);

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString text = QString::fromLatin1(file.readAll());
    file.close();
    QVERIFY2(text.contains(QStringLiteral("\n 78\n    1\n")), "没有写出图案定义线条数");
    QVERIFY2(text.contains(QStringLiteral("\n 53\n45\n")), "没有写出图案线角度");
    // 偏移必须是图案线坐标系里的 (0, 间距/比例)
    QVERIFY2(text.contains(QStringLiteral("\n 45\n0\n")), "偏移没有落在图案线坐标系里");
    QVERIFY2(text.contains(QStringLiteral("\n 46\n6.35\n")), "偏移没有按比例换算");

    Document loaded;
    QVERIFY(readDxfWithLibDxfRw(path, loaded).ok);
    QCOMPARE(loaded.entityCount(), std::size_t(1));
    const auto* readBack = static_cast<const HatchEntity*>(loaded.entity(loaded.drawOrder().front()));
    QVERIFY(readBack != nullptr);
    QCOMPARE(readBack->patternLines().size(), std::size_t(1));
    // 读回来仍是「最终间距」：定义偏移 6.35 × 比例 0.5 = 3.175
    QVERIFY(std::abs(readBack->patternLines().front().angle - kPi / 4.0) < kTol);
    QVERIFY(std::abs(readBack->definedSpacing() - 3.175) < 1e-9);
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
