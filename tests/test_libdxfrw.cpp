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
#include "core/InfiniteLineEntity.h"
#include "core/LineEntity.h"
#include "core/PointEntity.h"
#include "core/PolylineEntity.h"
#include "core/RayEntity.h"
#include "core/TextEntity.h"
#include "core/XLineEntity.h"
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

    // 无界直线：构造线（双向）与射线（单向）。
    // 方向刻意给成非单位向量：写出时必须归一化（DXF 的 11 组码约定如此）。
    document.addEntity(std::make_unique<XLineEntity>(Point2D(30.0, 45.0),
                                                     Vector2D(3.0, 4.0)));
    document.addEntity(std::make_unique<RayEntity>(Point2D(75.0, 10.0),
                                                   Vector2D(1.0, 2.0)));
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

/// 文字图元的可比指纹：**插入点 + 旋转**。
///
/// 用于「同一张图纸的两种格式必须读出同一份文字」这条比对。刻意只收录几何量：
/// MTEXT 的内容里带内联格式码（\f 字体、\W 宽度、\pxq 段落对），这些码在不同格式
/// 之间写法可以不同——实测样本的标题文字就是这样：DWG 里是 `\pxqc;{\W1;直流…}`、
/// DXF 里是 `{\fSimSun|…;\W8.05716;}{\W1.25000x;直流…}`。这类差异会连带改变内容与
/// 由内容推导出的对齐点，却与「同一张图」并不矛盾；而插入点与旋转是纯粹的几何，
/// 两条读取路径必须给出同一个值。旋转折成度并截到 1e-4：浮点末位差异不该让测试
/// 变红，而漏掉单位换算（错 57 倍）或整个丢了旋转（0 对 90）都远远超出这个精度。
QStringList textFingerprint(const Document& document)
{
    QStringList out;
    for (EntityId id : document.drawOrder()) {
        const Entity* entity = document.entity(id);
        if (!entity || entity->type() != EntityType::Text) continue;

        const auto& text = static_cast<const TextEntity&>(*entity);
        out << QStringLiteral("%1,%2|%3")
                   .arg(text.position().x, 0, 'f', 6)
                   .arg(text.position().y, 0, 'f', 6)
                   .arg(text.rotation() * 180.0 / kPi, 0, 'f', 4);
    }
    out.sort();
    return out;
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
    void mtextParagraphAlignmentAndRectAnchor();
    void mtextRotationFromXAxisDirection();
    void mtextInlineFontBecomesTextStyle();
    void writtenHatchPatternDefinitionRoundTrips();
    /// 无界图元（构造线 / 射线）经原生 dwgW 写出后必须原样读回
    void infiniteLinesRoundTripThroughNativeDwg();
    /// 普通几何（直线 / 多段线）经原生 dwgW 写出后坐标必须逐位保真
    void linesAndPolylinesRoundTripThroughNativeDwg();
    /// 标注经原生 dwgW 写出后定义点与测量值必须原样读回
    void dimensionsRoundTripThroughNativeDwg();

    // ---- 写出后的「打开即见图形」--------------------------------------------
    void writtenActiveViewportCoversDrawing();
    void writtenTextStylesRoundTrip();

    // ---- 两条独立实现互证 ----------------------------------------------------

    // ---- DWG 路径（需要外部样本，缺失时跳过）----------------------------------
    void readsDwgSample();
    void dwgAndDxfAgree();
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
    QVERIFY2(counts.value(static_cast<int>(EntityType::XLine)) >= 1, "构造线缺失");
    QVERIFY2(counts.value(static_cast<int>(EntityType::Ray)) >= 1, "射线缺失");
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

// -----------------------------------------------------------------------------
//  MTEXT 降级为单行文字：段落对齐码与「插入点 + 参考矩形宽度」都要算进去。
//
//  MTEXT 的文字排在一个参考矩形里（10/20 = 附着点，41 = 矩形宽度）：附着点决定
//  矩形哪条边贴着插入点，段落对齐码（\pxqc; 等）决定行在矩形里的横向位置。
//  只看附着点、把格式码一律剥掉，整块文字就会错位——实测某图纸的 "JK" 偏了
//  40 个单位（矩形宽 79.24），中文说明行则从居中变成左对齐。
void TestLibDxfRw::mtextParagraphAlignmentAndRectAnchor()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("mtext.dxf"));

    // 最小 DXF：一条「左上附着点 + 段落居中 + 矩形宽 40」的 MTEXT，
    // 和一条同样附着点但没有段落码的 MTEXT（对照）
    const QByteArray content =
        "0\nSECTION\n2\nENTITIES\n"
        "0\nMTEXT\n5\n100\n100\nAcDbEntity\n8\n0\n100\nAcDbMText\n"
        "10\n100.0\n20\n50.0\n40\n2.5\n41\n40.0\n71\n1\n72\n5\n"
        "1\n\\pxqc;测试文字\n"
        "0\nMTEXT\n5\n101\n100\nAcDbEntity\n8\n0\n100\nAcDbMText\n"
        "10\n200.0\n20\n50.0\n40\n2.5\n41\n40.0\n71\n1\n72\n5\n"
        "1\n左对齐文字\n"
        "0\nENDSEC\n0\nEOF\n";

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
    file.close();

    Document document;
    const IoResult result = readDxfWithLibDxfRw(path, document);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(document.entityCount(), std::size_t(2));

    std::vector<const TextEntity*> texts;
    for (EntityId id : document.drawOrder()) {
        const Entity* e = document.entity(id);
        if (e && e->type() == EntityType::Text) texts.push_back(static_cast<const TextEntity*>(e));
    }
    QCOMPARE(texts.size(), std::size_t(2));

    for (const TextEntity* text : texts) {
        if (text->content().contains(QStringLiteral("测试文字"))) {
            // 段落码 \pxqc; → 居中；锚点 = 矩形中心 = 插入点 + 宽度/2
            QCOMPARE(text->hAlign(), TextEntity::HAlign::Center);
            QVERIFY(std::abs(text->effectivePosition().x - (100.0 + 40.0 * 0.5)) < 1e-6);
            QVERIFY(std::abs(text->effectivePosition().y - 50.0) < 1e-6);
        } else {
            QCOMPARE(text->content(), QStringLiteral("左对齐文字"));
            QCOMPARE(text->hAlign(), TextEntity::HAlign::Left);
            // 没有段落码：按附着点（左上）左对齐，锚点就是插入点
            QVERIFY(std::abs(text->effectivePosition().x - 200.0) < 1e-6);
        }
    }
}

// -----------------------------------------------------------------------------
//  MTEXT 的旋转取自 11 组码的 X 轴方向向量。
//
//  MTEXT 几乎不用 50 组码表达旋转，而是给一个方向向量（11/21）；读成单行文字时
//  必须按 atan2 把它换算成角度。这里三条对照：方向朝上（90°）、方向朝右（0°）、
//  以及少数生成器直接给 50 组码的情形。
//
//  同一段逻辑在 DWG 路径上还有一道外部依赖：libdxfrw 必须置位 haveXAxis，
//  否则方向向量根本不会被认出来（见 dwgAndDxfAgree）。
void TestLibDxfRw::mtextRotationFromXAxisDirection()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("mtext_rotation.dxf"));

    const QByteArray content =
        "0\nSECTION\n2\nENTITIES\n"
        // 11/21 = (0,1)：文字沿 Y 轴排 → 90°
        "0\nMTEXT\n5\n100\n100\nAcDbEntity\n8\n0\n100\nAcDbMText\n"
        "10\n10.0\n20\n20.0\n40\n2.5\n41\n0.0\n71\n1\n"
        "11\n0.0\n21\n1.0\n31\n0.0\n"
        "1\n竖排标签\n"
        // 11/21 = (1,0)：不旋转
        "0\nMTEXT\n5\n101\n100\nAcDbEntity\n8\n0\n100\nAcDbMText\n"
        "10\n30.0\n20\n20.0\n40\n2.5\n41\n0.0\n71\n1\n"
        "11\n1.0\n21\n0.0\n31\n0.0\n"
        "1\n水平标签\n"
        // 没有 11 组码，只有 50 组码（度）
        "0\nMTEXT\n5\n102\n100\nAcDbEntity\n8\n0\n100\nAcDbMText\n"
        "10\n50.0\n20\n20.0\n40\n2.5\n41\n0.0\n71\n1\n50\n30.0\n"
        "1\n按角度写\n"
        "0\nENDSEC\n0\nEOF\n";

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
    file.close();

    Document document;
    const IoResult result = readDxfWithLibDxfRw(path, document);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(document.entityCount(), std::size_t(3));

    const auto rotationOf = [&document](const QString& needle) {
        bool found = false;
        double rotation = -1.0;
        for (EntityId id : document.drawOrder()) {
            const Entity* e = document.entity(id);
            if (!e || e->type() != EntityType::Text) continue;
            const auto& text = static_cast<const TextEntity&>(*e);
            if (!text.content().contains(needle)) continue;
            found = true;
            rotation = text.rotation();
        }
        Q_ASSERT(found);
        return rotation;
    };

    QVERIFY(std::abs(rotationOf(QStringLiteral("竖排标签")) - kPi / 2.0) < kTol);
    QVERIFY(std::abs(rotationOf(QStringLiteral("水平标签")) - 0.0) < kTol);
    QVERIFY(std::abs(rotationOf(QStringLiteral("按角度写")) - degToRad(30.0)) < kTol);
}

// -----------------------------------------------------------------------------
//  MTEXT 的内联字体码 \f<字体>|...; 必须还原成文字样式。
//
//  内联字体的优先级高于图元样式；剥码时把它一并丢掉，导出后整段字退回样式里的
//  字体——实测某图纸的英文 "KC-DZS05"/"JK" 因此从宋体变成单线笔画字体（浩辰里
//  一眼就能看出）。还原方式：按字体名建一条文字样式（TrueType 族名 + 字符集标志，
//  或 .shx 文件名），图元挂到该样式上，显示与导出都能还原原字体。
void TestLibDxfRw::mtextInlineFontBecomesTextStyle()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("mtext_font.dxf"));

    const QByteArray content =
        "0\nSECTION\n2\nENTITIES\n"
        "0\nMTEXT\n5\n100\n100\nAcDbEntity\n8\n0\n100\nAcDbMText\n"
        "10\n10.0\n20\n20.0\n40\n2.5\n41\n40.0\n71\n5\n72\n5\n"
        "1\n{\\fSimSun|b0|i0|c134|p2;KC-DZS05}\n"
        "0\nENDSEC\n0\nEOF\n";

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
    file.close();

    Document document;
    const IoResult result = readDxfWithLibDxfRw(path, document);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(document.entityCount(), std::size_t(1));

    const auto* text = static_cast<const TextEntity*>(
        document.entity(document.drawOrder().front()));
    QVERIFY(text != nullptr);
    QCOMPARE(text->content(), QStringLiteral("KC-DZS05"));
    QCOMPARE(text->fontStyle(), QStringLiteral("SimSun"));

    const TextStyle* style = document.textStyles().find(QStringLiteral("SimSun"));
    QVERIFY2(style != nullptr, "内联字体没有变成文字样式");
    QCOMPARE(style->ttfName(), QStringLiteral("SimSun"));
    // 字符集 134（GB2312）与 pitch 2 打包进字体族标志（AutoCAD 的 1071 组码）
    QCOMPARE(style->ttfFlags(), (134 << 8) | 2);

    // 写出：图元引用该样式，且样式记录带上 TrueType 扩展数据
    const QString out = dir.filePath(QStringLiteral("mtext_font_out.dxf"));
    DxfCodec dxf;
    QVERIFY(dxf.write(out, document, r2000Options()).ok);
    QFile written(out);
    QVERIFY(written.open(QIODevice::ReadOnly));
    const QString text2 = QString::fromLatin1(written.readAll());
    written.close();
    QVERIFY2(text2.contains(QStringLiteral("\n  2\nSimSun\n")), "样式表里没有 SimSun");
    QVERIFY2(text2.contains(QStringLiteral("\n1000\nSimSun\n")), "缺少 TrueType 族名扩展数据");
    QVERIFY2(text2.contains(QStringLiteral("\n1071\n34306\n")), "缺少字体族标志");
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

// -----------------------------------------------------------------------------
//  同一张图纸的 DWG 与 DXF 必须读出**同一份文字**。
//
//  这条不变量是实测踩出来的（2026-10）：libdxfrw 的 DWG 读取路径在解析 MTEXT 时
//  没有置位「11 组码是 X 轴方向向量」这个标记，hasXAxisDirection() 恒为 false，
//  适配层取旋转时只能退回 angle——而 DWG 路径的 MTEXT angle 永远是 0。后果是
//  实测一张 2018 版 DWG（AC1032）：272 条文字里 202 条旋转 90° 的 MTEXT 被画成
//  水平，同一张图纸的 DXF 却完全正确，用户看到的就是「好多文字方向错了」。
//
//  两条路径的旋转来源不同（TEXT 走 50 组码且单位不同、MTEXT 走 11 组码方向向量），
//  漏掉任何一边都不会崩溃、只会静静画错，所以这里把「两种格式得到同一份文字」钉死。
//  比的是几何量（插入点 + 旋转）而不是 MTEXT 的内容：内联格式码在两种格式之间的
//  写法可以合法地不同，详见 textFingerprint() 的说明。
//
//  DWG 样本不随仓库分发（体积大、多为第三方图纸），因此由环境变量驱动：
//    LYCAD_DWG_SAMPLE=/path/a.dwg LYCAD_DXF_SAMPLE=/path/b.dxf ctest -R test_libdxfrw
//  未给出 LYCAD_DXF_SAMPLE 时，取与 DWG 主名相同的 .dxf 作为对照。
void TestLibDxfRw::dwgAndDxfAgree()
{
    const QByteArray dwgSample = qgetenv("LYCAD_DWG_SAMPLE");
    if (dwgSample.isEmpty()) {
        QSKIP("未设置 LYCAD_DWG_SAMPLE，跳过 DWG/DXF 对照测试");
    }

    const QFileInfo dwgInfo(QString::fromLocal8Bit(dwgSample));
    if (!dwgInfo.exists()) {
        QSKIP("LYCAD_DWG_SAMPLE 指向的文件不存在");
    }

    const QByteArray dxfSample = qgetenv("LYCAD_DXF_SAMPLE");
    const QFileInfo dxfInfo = dxfSample.isEmpty()
                                  ? QFileInfo(dwgInfo.dir(), dwgInfo.completeBaseName()
                                                                + QStringLiteral(".dxf"))
                                  : QFileInfo(QString::fromLocal8Bit(dxfSample));
    if (!dxfInfo.exists()) {
        QSKIP("找不到与 DWG 对照的 DXF；可用 LYCAD_DXF_SAMPLE 指定");
    }

    Document fromDwg;
    const IoResult dwgResult = readDwgWithLibDxfRw(dwgInfo.absoluteFilePath(), fromDwg);
    QVERIFY2(dwgResult.ok, qPrintable(dwgResult.error));

    Document fromDxf;
    const IoResult dxfResult = readDxfWithLibDxfRw(dxfInfo.absoluteFilePath(), fromDxf);
    QVERIFY2(dxfResult.ok, qPrintable(dxfResult.error));

    QCOMPARE(fromDwg.entityCount(), fromDxf.entityCount());

    const QStringList dwgTexts = textFingerprint(fromDwg);
    const QStringList dxfTexts = textFingerprint(fromDxf);
    QVERIFY2(!dwgTexts.isEmpty(), "样本里没有文字，这个对照测试失去意义");
    QCOMPARE(dwgTexts.size(), dxfTexts.size());

    // 逐条给出差异，避免只报「两个 QList 不相等」而看不出是哪一条错了
    for (int i = 0; i < dwgTexts.size(); ++i) {
        if (dwgTexts.at(i) == dxfTexts.at(i)) continue;
        QVERIFY2(false, qPrintable(
                            QStringLiteral("同一位置的文字旋转不一致（第 %1 条，共 %2 条）：\n"
                                           "  DWG: %3（插入点 X,Y | 旋转°）\n"
                                           "  DXF: %4")
                                .arg(i).arg(dwgTexts.size())
                                .arg(dwgTexts.at(i), dxfTexts.at(i))));
    }
}

// -----------------------------------------------------------------------------
//  无界图元（构造线 / 射线）的原生 DWG 往返
//
//  R2000 的 RAY / XLINE 对象只有「基点 3BD + 方向 3BD」两个字段（见 libdxfrw
//  DRW_Ray::parseDwg），写出端必须与之严格对称——多写或少写一个字节都会让
//  句柄流错位，文件在 AutoCAD 里直接打不开。因此这里做一次真实的写-读往返，
//  而不是只看写出是否返回成功。
// -----------------------------------------------------------------------------
void TestLibDxfRw::infiniteLinesRoundTripThroughNativeDwg()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Document source;
    source.beginBatchLoad();
    // 方向刻意给成非单位向量，顺带验证「按单位向量存储」的约定
    source.addEntity(std::make_unique<XLineEntity>(Point2D(12.5, -30.25), Vector2D(0.0, 5.0)));
    source.addEntity(std::make_unique<RayEntity>(Point2D(-40.0, 8.0), Vector2D(-3.0, -4.0)));
    source.endBatchLoad();

    const QString path = dir.filePath(QStringLiteral("infinite.dwg"));
    const IoResult written = writeDwgWithLibDxfRw(path, source, WriteOptions{});
    QVERIFY2(written.ok, qPrintable(written.error));

    Document loaded;
    const IoResult result = readDwgWithLibDxfRw(path, loaded);
    QVERIFY2(result.ok, qPrintable(result.error));

    const Entity* xline = firstOfType(loaded, EntityType::XLine);
    const Entity* ray = firstOfType(loaded, EntityType::Ray);
    QVERIFY2(xline != nullptr, "构造线在 DWG 往返中丢失");
    QVERIFY2(ray != nullptr, "射线在 DWG 往返中丢失");

    const auto& x = static_cast<const InfiniteLineEntity&>(*xline);
    QVERIFY(std::abs(x.basePoint().x - 12.5) < kTol);
    QVERIFY(std::abs(x.basePoint().y + 30.25) < kTol);
    // 方向 (0, 5) 归一化后应为 (0, 1)
    QVERIFY2(std::abs(x.direction().y - 1.0) < kTol && std::abs(x.direction().x) < kTol,
             qPrintable(QStringLiteral("构造线方向未归一化：%1, %2")
                            .arg(x.direction().x).arg(x.direction().y)));

    const auto& r = static_cast<const InfiniteLineEntity&>(*ray);
    QVERIFY(std::abs(r.basePoint().x + 40.0) < kTol);
    QVERIFY(std::abs(r.basePoint().y - 8.0) < kTol);
    // 方向 (-3, -4) 归一化后应为 (-0.6, -0.8)
    QVERIFY2(std::abs(r.direction().x + 0.6) < kTol && std::abs(r.direction().y + 0.8) < kTol,
             qPrintable(QStringLiteral("射线方向错误：%1, %2")
                            .arg(r.direction().x).arg(r.direction().y)));
}

// -----------------------------------------------------------------------------
//  普通几何（直线 / 多段线）的原生 DWG 往返
//
//  DWG 的坐标用「带默认值的压缩 double（DD）」编码：第二点/顶点按位与参考值
//  比较，能省则省。写出端的分支语义必须与读取端 getDefaultDouble 严格对偶，
//  否则坐标会悄悄变成垃圾值——实测曾因此让几乎所有直线读回时长度变成 1e-5、
//  部分线段更因「零长度」被当作坏数据丢弃。这里用覆盖各压缩分支的坐标钉死约定。
// -----------------------------------------------------------------------------
void TestLibDxfRw::linesAndPolylinesRoundTripThroughNativeDwg()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Document source;
    source.beginBatchLoad();
    source.addEntity(std::make_unique<LineEntity>(Point2D(10.0, 10.0), Point2D(170.0, 100.0)));
    source.addEntity(std::make_unique<LineEntity>(Point2D(10.0, 100.0), Point2D(170.0, 10.0)));
    source.addEntity(std::make_unique<LineEntity>(Point2D(0.0, 55.0), Point2D(180.0, 55.0)));
    source.addEntity(std::make_unique<LineEntity>(Point2D(90.0, 0.0), Point2D(90.0, 110.0)));
    {
        auto polyline = std::make_unique<PolylineEntity>();
        polyline->appendVertex(Point2D(0.0, 0.0), 0.0);
        polyline->appendVertex(Point2D(180.0, 0.0), 0.0);
        polyline->appendVertex(Point2D(180.0, 110.0), 0.0);
        polyline->appendVertex(Point2D(0.0, 110.0), 0.0);
        polyline->setClosed(true);
        source.addEntity(std::move(polyline));
    }
    source.endBatchLoad();

    const QString path = dir.filePath(QStringLiteral("lines.dwg"));
    QVERIFY2(writeDwgWithLibDxfRw(path, source, WriteOptions{}).ok, "DWG 写出失败");

    Document loaded;
    QVERIFY2(readDwgWithLibDxfRw(path, loaded).ok, "DWG 读回失败");

    // 一条不少：零长度在读取端是坏数据，坐标一旦被写坏，线段会在这里凭空消失
    QCOMPARE(loaded.entityCount(), source.entityCount());

    const auto expectLine = [&loaded](std::size_t index, const Point2D& start,
                                      const Point2D& end) {
        const Entity* entity = loaded.entity(loaded.drawOrder().at(index));
        QVERIFY(entity != nullptr);
        QCOMPARE(entity->type(), EntityType::Line);
        const auto& line = static_cast<const LineEntity&>(*entity);
        QVERIFY2(line.start().distanceTo(start) < kTol && line.end().distanceTo(end) < kTol,
                 qPrintable(QStringLiteral("第 %1 条直线几何不一致：读回 (%2, %3) → (%4, %5)")
                                .arg(index)
                                .arg(line.start().x, 0, 'g', 12)
                                .arg(line.start().y, 0, 'g', 12)
                                .arg(line.end().x, 0, 'g', 12)
                                .arg(line.end().y, 0, 'g', 12)));
    };
    expectLine(0, Point2D(10.0, 10.0), Point2D(170.0, 100.0));
    expectLine(1, Point2D(10.0, 100.0), Point2D(170.0, 10.0));
    expectLine(2, Point2D(0.0, 55.0), Point2D(180.0, 55.0));
    expectLine(3, Point2D(90.0, 0.0), Point2D(90.0, 110.0));

    const Entity* polylineEntity = loaded.entity(loaded.drawOrder().at(4));
    QVERIFY(polylineEntity != nullptr);
    QCOMPARE(polylineEntity->type(), EntityType::Polyline);
    const auto& polyline = static_cast<const PolylineEntity&>(*polylineEntity);
    QCOMPARE(polyline.vertexCount(), std::size_t(4));
    QVERIFY(polyline.vertex(0).point == Point2D(0.0, 0.0));
    QVERIFY(polyline.vertex(1).point == Point2D(180.0, 0.0));
    QVERIFY(polyline.vertex(2).point == Point2D(180.0, 110.0));
    QVERIFY(polyline.vertex(3).point == Point2D(0.0, 110.0));
}

// -----------------------------------------------------------------------------
//  标注的原生 DWG 往返
//
//  R2000 标注专属数据区的字段序与公共实体数据不同：拉伸方向是**普通 3BD**
//  （没有「默认标志位」），其后依次是 2RD 文字中点、BD 高程、RC flag1、……
//  写出端若套用公共实体数据的 BE 编码，位流从第一个字段就错位，AutoCAD 系
//  软件（实测 GstarCAD）会把全部标注的定义点解成垃圾值，图形缩成一团挤在
//  原点。此约定已用 LibreDWG 对照真实 AutoCAD 文件逐字段钉死，这里做写-读
//  往返防止回退。
// -----------------------------------------------------------------------------
void TestLibDxfRw::dimensionsRoundTripThroughNativeDwg()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Document source;
    source.beginBatchLoad();
    {
        auto linear = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Linear);
        linear->setFirstPoint(Point2D(180.0, 0.0));
        linear->setSecondPoint(Point2D(180.0, 110.0));
        linear->setDimLinePoint(Point2D(198.0, 55.0));
        linear->setRotation(degToRad(90.0));                    // 竖直尺寸线
        source.addEntity(std::move(linear));
    }
    {
        auto aligned = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Aligned);
        aligned->setFirstPoint(Point2D(0.0, 0.0));
        aligned->setSecondPoint(Point2D(80.0, 0.0));
        aligned->setDimLinePoint(Point2D(40.0, -15.0));
        source.addEntity(std::move(aligned));
    }
    {
        auto radial = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Radial);
        radial->setFirstPoint(Point2D(60.0, 55.0));             // 圆心
        radial->setSecondPoint(Point2D(77.6776695296637, 72.6776695296637)); // 45°，半径 25
        source.addEntity(std::move(radial));
    }
    {
        auto diametric = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Diametric);
        diametric->setFirstPoint(Point2D(95.0, 55.0));
        diametric->setSecondPoint(Point2D(145.0, 55.0));        // 直径 50
        source.addEntity(std::move(diametric));
    }
    source.endBatchLoad();

    const QString path = dir.filePath(QStringLiteral("dims.dwg"));
    QVERIFY2(writeDwgWithLibDxfRw(path, source, WriteOptions{}).ok, "DWG 写出失败");

    Document loaded;
    QVERIFY2(readDwgWithLibDxfRw(path, loaded).ok, "DWG 读回失败");

    // 一条不少：标注一旦被读坏（位流错位 → 定义点成垃圾、测量值为零），
    // 读取端会把它当坏数据丢弃，数量对不上
    QCOMPARE(loaded.entityCount(), source.entityCount());

    const DimensionEntity* linear = nullptr;
    const DimensionEntity* aligned = nullptr;
    const DimensionEntity* radial = nullptr;
    const DimensionEntity* diametric = nullptr;
    for (EntityId id : loaded.drawOrder()) {
        const Entity* entity = loaded.entity(id);
        if (!entity || entity->type() != EntityType::Dimension) continue;
        const auto& dim = static_cast<const DimensionEntity&>(*entity);
        switch (dim.dimType()) {
        case DimensionEntity::DimType::Linear:    linear = &dim; break;
        case DimensionEntity::DimType::Aligned:   aligned = &dim; break;
        case DimensionEntity::DimType::Radial:    radial = &dim; break;
        case DimensionEntity::DimType::Diametric: diametric = &dim; break;
        default: break;
        }
    }
    QVERIFY2(linear && aligned && radial && diametric,
             "四类标注没有全部读回，DWG 标注编码有误");

    QVERIFY(linear->firstPoint().distanceTo(Point2D(180.0, 0.0)) < kTol);
    QVERIFY(linear->secondPoint().distanceTo(Point2D(180.0, 110.0)) < kTol);
    QVERIFY(linear->dimLinePoint().distanceTo(Point2D(198.0, 55.0)) < kTol);
    QVERIFY(std::abs(linear->measurement() - 110.0) < kTol);

    QVERIFY(aligned->firstPoint().distanceTo(Point2D(0.0, 0.0)) < kTol);
    QVERIFY(aligned->secondPoint().distanceTo(Point2D(80.0, 0.0)) < kTol);
    QVERIFY(aligned->dimLinePoint().distanceTo(Point2D(40.0, -15.0)) < kTol);
    QVERIFY(std::abs(aligned->measurement() - 80.0) < kTol);

    QVERIFY(radial->firstPoint().distanceTo(Point2D(60.0, 55.0)) < kTol);
    QVERIFY(std::abs(radial->measurement() - 25.0) < kTol);

    QVERIFY(diametric->firstPoint().distanceTo(Point2D(95.0, 55.0)) < kTol);
    QVERIFY(diametric->secondPoint().distanceTo(Point2D(145.0, 55.0)) < kTol);
    QVERIFY(std::abs(diametric->measurement() - 50.0) < kTol);
}

QTEST_MAIN(TestLibDxfRw)
#include "test_libdxfrw.moc"
