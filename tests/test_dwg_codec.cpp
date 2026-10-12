// =============================================================================
//  LyCAD - tests/test_dwg_codec.cpp
//  DWG 编解码器的测试。
//
//  覆盖范围
//    DWG 的版本签名识别、内容探测、二进制内容预检（挡住伪装成 DWG 的文本），
//    以及原生读写在编解码器这一层的行为：能力上报、"不是真 DWG 就别静默成功"、
//    写出的是二进制而非 DXF 副本。
//
//    图元级的原生读-写往返（坐标的 DD 压缩编码、RAY/XLINE 的字段布局等）
//    在 test_libdxfrw 里，此处不重复。
// =============================================================================
#include <QtTest>

#include <QTemporaryDir>

#include "core/CircleEntity.h"
#include "core/HatchEntity.h"
#include "core/Document.h"
#include "core/LineEntity.h"
#include "core/MTextEntity.h"
#include "core/TextEntity.h"
#include "io/CadFile.h"
#include "io/DwgCodec.h"

#include <QFile>
#include <QFileInfo>

using namespace lycad;

class TestDwgCodec : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void versionSignatureTable();
    void probeRecognizesDwgOnly();
    void dwgWriteVersionIsSelectable();
    void nativeReadWriteRoundTrip();
    void dwgR2000ReadWriteRoundTrip();
    void hatchSurvivesDwgRoundTrip();
    void textStyleMetricsAndMiddleAlignSurviveDwgRoundTrip();
    void mtextKeepsItsOwnAnchorAndLayoutOnDwgRoundTrip();
    void realSampleMtextSurvivesRoundTrip();
    void textMasqueradingAsDwgIsRejectedBeforeParsing();
    void brokenDwgIsReportedNotSwallowed();

private:
    QTemporaryDir m_dir;
};

// -----------------------------------------------------------------------------
void TestDwgCodec::initTestCase()
{
    QVERIFY2(m_dir.isValid(), "无法创建临时目录");
    registerBuiltinCodecs();
}

// -----------------------------------------------------------------------------
void TestDwgCodec::versionSignatureTable()
{
    struct Case { const char* signature; const char* code; bool recognized; };
    static const Case kCases[] = {
        { "AC1009", "AC1009", true },   // R12
        { "AC1015", "AC1015", true },   // 2000
        { "AC1018", "AC1018", true },   // 2004
        { "AC1021", "AC1021", true },   // 2007
        { "AC1024", "AC1024", true },   // 2010
        { "AC1027", "AC1027", true },   // 2013
        { "AC1032", "AC1032", true },   // 2018
        { "AC1055", "AC1055", false },  // 结构正确但版本未知
        { "DXF12",  "",       false },  // 完全不是 DWG
    };

    for (const Case& c : kCases) {
        const DwgVersionInfo info = dwgVersionFromSignature(QByteArray(c.signature));
        QCOMPARE(info.recognized, c.recognized);
        if (c.recognized) {
            QCOMPARE(info.code, QString::fromLatin1(c.code));
            QVERIFY(!info.displayName.isEmpty());
        }
    }

    // 长度不足时不应误判
    QVERIFY(!dwgVersionFromSignature(QByteArray("AC10")).recognized);
    QVERIFY(!dwgVersionFromSignature(QByteArray()).recognized);
}

void TestDwgCodec::probeRecognizesDwgOnly()
{
    const QString dwgPath = m_dir.filePath(QStringLiteral("probe.dwg"));
    {
        QFile file(dwgPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("AC1015");
        file.write(QByteArray(512, '\0'));
    }

    const QString dxfPath = m_dir.filePath(QStringLiteral("probe.dxf"));
    {
        QFile file(dxfPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("0\nSECTION\n2\nENTITIES\n0\nENDSEC\n0\nEOF\n");
    }

    DwgCodec dwg;
    CodecRegistry& registry = CodecRegistry::instance();

    // 按扩展名路由必须命中 DWG 编解码器
    ICadCodec* dwgCodec = registry.codecForPath(dwgPath, false);
    QVERIFY(dwgCodec != nullptr);
    QCOMPARE(dwgCodec->name(), QStringLiteral("DWG"));

    QCOMPARE(registry.codecForPath(dxfPath, false)->name(), QStringLiteral("DXF"));

    // DWG 的读写能力来自内置 libdxfrw，不依赖任何外部程序，因此恒为可用
    QVERIFY(dwgCodec->canRead());
    QVERIFY(dwgCodec->canWrite());
    QVERIFY(CadFile::canLoad(dwgPath));
    QVERIFY(CadFile::canSave(dwgPath));
    QVERIFY(!registry.codecForPath(dwgPath, true)->capabilityNote().isEmpty());

    // 内容探测：DWG 签名命中，DXF 内容不命中
    {
        QFile file(dwgPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(dwg.probe(file));
    }
    {
        QFile file(dxfPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!dwg.probe(file));
    }
}

void TestDwgCodec::dwgWriteVersionIsSelectable()
{
    // 「另存为」的文件类型列表就是版本选择器：DWG 的档位由 WriteOptions 给出，
    // 默认档必须在清单内（界面据此选中默认项），且所选档位要真正落到文件签名上。
    const QStringList versions = WriteOptions::availableDwgVersions();
    QVERIFY(versions.contains(QStringLiteral("AC1015")));
    QVERIFY(versions.contains(QStringLiteral("AC1018")));
    QVERIFY(versions.contains(QStringLiteral("AC1021")));
    QVERIFY(versions.contains(WriteOptions::defaultDwgVersion()));
    QVERIFY(WriteOptions::isWritableDwgVersion(QStringLiteral("AC1018")));
    QVERIFY(WriteOptions::isWritableDwgVersion(QStringLiteral("AC1021")));
    QVERIFY(!WriteOptions::isWritableDwgVersion(QStringLiteral("AC1032")));

    Document document;
    document.beginBatchLoad();
    document.addEntity(std::make_unique<LineEntity>(Point2D(0.0, 0.0), Point2D(1.0, 1.0)));
    document.endBatchLoad();

    // 选 AC1018 时文件头签名必须是 AC1018（版本参数不得被忽略）
    const QString path = m_dir.filePath(QStringLiteral("v2004.dwg"));
    WriteOptions options;
    options.dwgVersion = QStringLiteral("AC1018");
    const IoResult result = CadFile::save(path, document, options);
    QVERIFY2(result.ok, qPrintable(result.error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.read(6), QByteArray("AC1018"));
    file.close();

    // 选 AC1021（R2007）同样必须落到签名上：它走的是 RS 纠错 + 分页容器，
    // 与 R2004 的 LZ77 分页容器是两条不同的写出路径，各自都要能被选中并生效。
    const QString path2007 = m_dir.filePath(QStringLiteral("v2007.dwg"));
    options.dwgVersion = QStringLiteral("AC1021");
    const IoResult result2007 = CadFile::save(path2007, document, options);
    QVERIFY2(result2007.ok, qPrintable(result2007.error));

    QFile file2007(path2007);
    QVERIFY(file2007.open(QIODevice::ReadOnly));
    QCOMPARE(file2007.read(6), QByteArray("AC1021"));
}

void TestDwgCodec::nativeReadWriteRoundTrip()
{
    Document document;
    document.beginBatchLoad();
    document.layers().ensure(QStringLiteral("测试层")).setColorIndex(2);
    document.addEntity(std::make_unique<LineEntity>(Point2D(0.0, 0.0), Point2D(30.0, 40.0)));
    document.addEntity(std::make_unique<CircleEntity>(Point2D(5.0, 5.0), 2.5));
    document.endBatchLoad();

    const QString dwgPath = m_dir.filePath(QStringLiteral("roundtrip.dwg"));
    const IoResult writeResult = CadFile::save(dwgPath, document, WriteOptions{});
    QVERIFY2(writeResult.ok, qPrintable(writeResult.error));
    QVERIFY(QFileInfo::exists(dwgPath));
    QVERIFY(QFileInfo(dwgPath).size() > 0);

    // 写出的是二进制 DWG，而不是中间 DXF 的副本——两者可以这样区分：
    // 二进制 DWG 只以版本签名开头，正文里没有 DXF 的段落名。
    QFile file(dwgPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray content = file.readAll();
    QCOMPARE(content.left(6), QByteArray("AC1021"));   // 默认写出版本 AC1021/R2007
    QVERIFY2(!content.contains("ENTITIES"), "DWG 应是二进制格式，而不是 DXF 文本");

    // 走出原生写出时应在提示中说明，便于用户追溯
    QVERIFY2(writeResult.warnings.join(QLatin1Char('\n')).contains(QStringLiteral("dwgW")),
             "原生写出应在提示中说明用的是 dwgW");

    Document loaded;
    const IoResult readResult = CadFile::load(dwgPath, loaded);
    QVERIFY2(readResult.ok, qPrintable(readResult.error));
    QCOMPARE(loaded.entityCount(), std::size_t(2));
    QCOMPARE(loaded.layers().count(), std::size_t(2));      // 0 层 + 测试层

    const Entity* first = loaded.entity(loaded.drawOrder().at(0));
    QCOMPARE(first->type(), EntityType::Line);
    QVERIFY(std::abs(first->length() - 50.0) < 1e-9);       // 3-4-5 直角三角形
}

void TestDwgCodec::dwgR2000ReadWriteRoundTrip()
{
    // R2000（AC1015）是另一条写出路径：节载荷与 R2004 共用，但容器是线性节，
    // 且对象层**没有** 2004+ 的那些差异（颜色 CMC、xDictFlag 位、块记录实体表…）。
    // 必须单独往返：共用代码里任何一处按版本判断写错，都会让这一档整体错位。
    // （实测：R2004 省略 XDicObj 空句柄的改动一度没加版本判断，R2000 输出因此
    // 少写一个句柄，外部 CAD 全部打不开，而 R2004 一切正常。）
    Document document;
    document.beginBatchLoad();
    document.layers().ensure(QStringLiteral("测试层")).setColorIndex(2);
    document.addEntity(std::make_unique<LineEntity>(Point2D(0.0, 0.0), Point2D(30.0, 40.0)));
    document.addEntity(std::make_unique<CircleEntity>(Point2D(5.0, 5.0), 2.5));
    document.endBatchLoad();

    const QString path = m_dir.filePath(QStringLiteral("roundtrip2000.dwg"));
    WriteOptions options;
    options.dwgVersion = QStringLiteral("AC1015");
    const IoResult writeResult = CadFile::save(path, document, options);
    QVERIFY2(writeResult.ok, qPrintable(writeResult.error));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.read(6), QByteArray("AC1015"));           // 档位要真正落到签名上

    Document loaded;
    const IoResult readResult = CadFile::load(path, loaded);
    QVERIFY2(readResult.ok, qPrintable(readResult.error));
    QCOMPARE(loaded.entityCount(), std::size_t(2));
    QCOMPARE(loaded.layers().count(), std::size_t(2));      // 0 层 + 测试层

    // 图层的线型是**句柄引用**解析出来的：表格记录句柄流少一个/错一位，
    // 线型名就会解析成空或错值——这条断言正是用来抓句柄流错位的。
    const Layer* layer = loaded.layers().find(QStringLiteral("测试层"));
    QVERIFY(layer != nullptr);
    QCOMPARE(layer->linetype(), QStringLiteral("CONTINUOUS"));

    const Entity* first = loaded.entity(loaded.drawOrder().at(0));
    QCOMPARE(first->type(), EntityType::Line);
    QVERIFY(std::abs(first->length() - 50.0) < 1e-9);
}

void TestDwgCodec::hatchSurvivesDwgRoundTrip()
{
    // R2004 的 HATCH 数据段开头比 R2000 **多一整块渐变填充字段**（is_gradient /
    // reserved / 角度 / 偏移 / 单色 / 色调 / 颜色数 / 名称）。漏写时读取端把我们
    // 的高程当成 is_gradient，其后 num_colors 与种子点数解析成天文数字——自家
    // 读取端直接死循环（此前 AC1015 正常、AC1018 全部读不回，含填充的图纸在
    // 外部 CAD 同样无法打开）。
    Document document;
    document.beginBatchLoad();
    document.layers().ensure(QStringLiteral("轮廓"));
    auto hatch = std::make_unique<HatchEntity>(
        std::vector<Point2D>{ Point2D(0.0, 0.0), Point2D(10.0, 0.0),
                              Point2D(10.0, 10.0), Point2D(0.0, 10.0) },
        QStringLiteral("ANSI31"), 2.0, 0.0);
    hatch->setLayerName(QStringLiteral("轮廓"));
    document.addEntity(std::move(hatch));
    document.endBatchLoad();

    const QString path = m_dir.filePath(QStringLiteral("hatch.dwg"));
    const IoResult writeResult = CadFile::save(path, document, WriteOptions{});
    QVERIFY2(writeResult.ok, qPrintable(writeResult.error));

    Document loaded;
    const IoResult readResult = CadFile::load(path, loaded);
    QVERIFY2(readResult.ok, qPrintable(readResult.error));
    QCOMPARE(loaded.entityCount(), std::size_t(1));
    QCOMPARE(loaded.entity(loaded.drawOrder().at(0))->type(), EntityType::Hatch);
    const auto& back = static_cast<const HatchEntity&>(*loaded.entity(loaded.drawOrder().at(0)));
    QCOMPARE(back.patternName(), QStringLiteral("ANSI31"));
}

void TestDwgCodec::textStyleMetricsAndMiddleAlignSurviveDwgRoundTrip()
{
    // 文字样式的度量字段（宽度因子 DXF 41）与 TEXT 的「Middle」对齐（DXF 72=4）
    // 必须随 DWG 往返保留：样式宽度丢成 1.0 会让浩辰/AutoCAD 按错误宽度重排
    // 全部文字（实测中文图纸 Standard=0.8，重存后所有文字横向拉宽 25%、
    // 图面整体错位）；Middle 丢成（居中，基线）会让文字抬高约半个字高。
    Document document;
    document.beginBatchLoad();

    TextStyle& narrow = document.textStyles().ensure(QStringLiteral("窄体"));
    narrow.setFontFile(QStringLiteral("txt"));
    narrow.setBigFontFile(QStringLiteral("gbcbig.shx"));
    narrow.setWidthFactor(0.8);

    auto text = std::make_unique<TextEntity>(QStringLiteral("图签文字"),
                                             Point2D(100.0, 50.0), 3.5, 0.0);
    // Middle：渲染与「中中」等价——用（居中，中部）模拟，落盘后应原样取回
    text->setHAlign(TextEntity::HAlign::Center);
    text->setVAlign(TextEntity::VAlign::Middle);
    text->setAlignmentPoint(Point2D(110.0, 52.0));
    text->setFontStyle(QStringLiteral("窄体"));
    const EntityId textId = document.addEntity(std::move(text));
    document.endBatchLoad();

    const QString dwgPath = m_dir.filePath(QStringLiteral("text-metrics.dwg"));
    const IoResult writeResult = CadFile::save(dwgPath, document, WriteOptions{});
    QVERIFY2(writeResult.ok, qPrintable(writeResult.error));

    Document loaded;
    const IoResult readResult = CadFile::load(dwgPath, loaded);
    QVERIFY2(readResult.ok, qPrintable(readResult.error));

    const TextStyle* roundTripped = loaded.textStyles().find(QStringLiteral("窄体"));
    QVERIFY2(roundTripped != nullptr, "文字样式应随 DWG 往返保留");
    QVERIFY2(std::abs(roundTripped->widthFactor() - 0.8) < 1e-9,
             "样式宽度因子应随 DWG 往返保留");

    const auto* roundTrippedText =
        dynamic_cast<const TextEntity*>(loaded.entity(textId));
    QVERIFY2(roundTrippedText != nullptr, "文字实体应随 DWG 往返保留");
    QVERIFY(roundTrippedText->hAlign() == TextEntity::HAlign::Center);
    QVERIFY(roundTrippedText->vAlign() == TextEntity::VAlign::Middle);
    QVERIFY2(roundTrippedText->alignmentPoint().distanceTo(Point2D(110.0, 52.0)) < 1e-9,
             "对齐点应随 DWG 往返保留");
}

void TestDwgCodec::mtextKeepsItsOwnAnchorAndLayoutOnDwgRoundTrip()
{
    // 多行文字必须是**独立图元**，不能降级成单行文字。三条理由各自都有回归价值：
    //   · 降级会丢掉附着点语义——MTEXT 中间行的垂直基准是「基线到小写高度的中点」，
    //     而 TEXT 的 72=1/73=2 用的是「基线到大写高度的中点」，两者差
    //     (capHeight − xHeight)/2 ≈ 0.13 倍字高，实测在浩辰/AutoCAD 里整条文字偏移，
    //     而本软件自读自写（两条路径用同一套换算）永远看不出来；
    //   · 参考栏宽（41）、书写方向（72）、行距样式（73）与行距系数（44）会一并丢失；
    //   · 内容里的内联格式码（\P、\f…）是给真实 CAD 读的，剥掉就再也回不去。
    // 因此这里逐字段盯住：类型、内容（逐字）、附着点、栏宽、字高、旋转、行距、样式、法向。
    Document document;
    document.beginBatchLoad();

    const QString content = QStringLiteral("第一段\\P{\\fSimSun|b0|i0|c134|p2;第二段}~尾");
    auto text = std::make_unique<MTextEntity>(content, Point2D(120.0, -35.0), 2.5);
    text->setAttachment(MTextEntity::Attachment::MiddleCenter);
    text->setReferenceWidth(12.5);
    text->setDirection(Vector2D(0.0, 1.0));                 // 旋转 90°
    text->setLineSpacingStyle(MTextEntity::LineSpacingStyle::Exact);
    text->setLineSpacingFactor(1.5);
    text->setFontStyle(QStringLiteral("Standard"));
    text->setExtents(6.0, 4.0);
    MTextEntity::Normal3D normal;
    normal.z = -1.0;                                        // 翻转法向也要原样带走
    text->setNormal(normal);
    const EntityId textId = document.addEntity(std::move(text));
    document.endBatchLoad();

    const QString dwgPath = m_dir.filePath(QStringLiteral("mtext.dwg"));
    const IoResult writeResult = CadFile::save(dwgPath, document, WriteOptions{});
    QVERIFY2(writeResult.ok, qPrintable(writeResult.error));

    Document loaded;
    const IoResult readResult = CadFile::load(dwgPath, loaded);
    QVERIFY2(readResult.ok, qPrintable(readResult.error));

    const Entity* roundTripped = loaded.entity(textId);
    QVERIFY2(roundTripped != nullptr, "多行文字实体应随 DWG 往返保留");
    QVERIFY2(roundTripped->type() == EntityType::MText,
             "多行文字不应被降级成单行文字（降级会丢掉附着点的垂直基准）");

    const auto* mtext = static_cast<const MTextEntity*>(roundTripped);
    QCOMPARE(mtext->content(), content);                    // 内容逐字保留（含内联格式码）
    QVERIFY(mtext->attachment() == MTextEntity::Attachment::MiddleCenter);
    QVERIFY(std::abs(mtext->referenceWidth() - 12.5) < 1e-9);
    QVERIFY(std::abs(mtext->height() - 2.5) < 1e-9);
    QVERIFY(std::abs(mtext->rotation() - kPi * 0.5) < 1e-9);
    QVERIFY(mtext->lineSpacingStyle() == MTextEntity::LineSpacingStyle::Exact);
    QVERIFY(std::abs(mtext->lineSpacingFactor() - 1.5) < 1e-9);
    QCOMPARE(mtext->fontStyle(), QStringLiteral("Standard"));
    QVERIFY2(mtext->normal().z < 0.0, "翻转的挤出方向应原样往返");
    QVERIFY(std::abs(mtext->insertionPoint().x - 120.0) < 1e-9);
    QVERIFY(std::abs(mtext->insertionPoint().y + 35.0) < 1e-9);
}

void TestDwgCodec::realSampleMtextSurvivesRoundTrip()
{
    // 真实图纸实样逐条比对：重存前后的多行文字必须一一对应。
    // 需要外部样本，未设置环境变量时自行跳过（与 test_libdxfrw 的实样用例同一口径）：
    //   LYCAD_MTEXT_SAMPLE=/path/to/file.dwg ctest -R test_dwg_codec
    const QByteArray sample = qgetenv("LYCAD_MTEXT_SAMPLE");
    if (sample.isEmpty()) QSKIP("未设置 LYCAD_MTEXT_SAMPLE，跳过真实图纸实样测试");
    const QString samplePath = QString::fromLocal8Bit(sample);
    if (!QFileInfo::exists(samplePath)) {
        QSKIP("LYCAD_MTEXT_SAMPLE 指向的文件不存在");
    }

    Document original;
    const IoResult loadResult = CadFile::load(samplePath, original);
    QVERIFY2(loadResult.ok, qPrintable(loadResult.error));

    // 源文件里的多行文字（降级成单行文字的实现在这里会得到 0 条）
    std::vector<EntityId> mtextIds;
    for (EntityId id : original.drawOrder()) {
        const Entity* entity = original.entity(id);
        if (entity && entity->type() == EntityType::MText) mtextIds.push_back(id);
    }
    QVERIFY2(!mtextIds.empty(), "实样里应能读出多行文字（MTEXT）");

    const QString dwgPath = m_dir.filePath(QStringLiteral("mtext-sample.dwg"));
    const IoResult writeResult = CadFile::save(dwgPath, original, WriteOptions{});
    QVERIFY2(writeResult.ok, qPrintable(writeResult.error));

    Document reloaded;
    const IoResult readResult = CadFile::load(dwgPath, reloaded);
    QVERIFY2(readResult.ok, qPrintable(readResult.error));
    QCOMPARE(reloaded.entityCount(), original.entityCount());

    for (EntityId id : mtextIds) {
        const auto* before = static_cast<const MTextEntity*>(original.entity(id));
        const Entity* after = reloaded.entity(id);
        QVERIFY2(after != nullptr, "重存后图元应仍在（id 稳定）");
        QVERIFY2(after->type() == EntityType::MText,
                 "重存后仍应是多行文字，不得降级成 TEXT");
        const auto* afterText = static_cast<const MTextEntity*>(after);
        QCOMPARE(afterText->content(), before->content());
        QVERIFY(afterText->attachment() == before->attachment());
        QVERIFY(std::abs(afterText->referenceWidth() - before->referenceWidth()) < 1e-9);
        QVERIFY(std::abs(afterText->height() - before->height()) < 1e-9);
        QVERIFY(std::abs(afterText->rotation() - before->rotation()) < 1e-9);
        QCOMPARE(afterText->fontStyle(), before->fontStyle());
    }
}

void TestDwgCodec::textMasqueradingAsDwgIsRejectedBeforeParsing()
{
    // DXF 内容套上 DWG 签名（被改扩展名、或转换/损坏后的残件）。
    // 解析器在这类不合法数据上会空转很久，必须在 512 字节预检处挡下，
    // 并给出能说明真实原因的错误——而不是让界面卡住两分钟。
    const QString path = m_dir.filePath(QStringLiteral("fake.dwg"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("AC1015");
        QByteArray body;
        for (int i = 0; i < 64; ++i) {
            body += "0\nSECTION\n2\nENTITIES\n0\nLINE\n";
        }
        QVERIFY(body.size() > 32);
        file.write(body);
    }

    DwgCodec dwg;
    Document document;
    const IoResult result = dwg.read(path, document);

    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(QStringLiteral("二进制")));
    QVERIFY(result.error.contains(QStringLiteral("DXF")));
    QCOMPARE(document.entityCount(), std::size_t(0));
}

void TestDwgCodec::brokenDwgIsReportedNotSwallowed()
{
    // 签名合法、内容也是二进制，但结构损坏：必须如实报错，
    // 不得静默成功，也不得把半成品留在文档里。
    const QString path = m_dir.filePath(QStringLiteral("broken.dwg"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("AC1018");
        file.write(QByteArray(512, '\0'));
    }

    DwgCodec dwg;
    Document document;
    const IoResult result = dwg.read(path, document);

    QVERIFY(!result.ok);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(document.entityCount(), std::size_t(0));

    // 版本识别不依赖解析成功，失败时也应一并告知
    QVERIFY(result.warnings.join(QLatin1Char('\n')).contains(QStringLiteral("AC1018")));
}

QTEST_MAIN(TestDwgCodec)
#include "test_dwg_codec.moc"
