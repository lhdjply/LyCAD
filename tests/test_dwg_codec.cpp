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
#include "core/Document.h"
#include "core/LineEntity.h"
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
    void nativeReadWriteRoundTrip();
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
    QCOMPARE(content.left(6), QByteArray("AC1015"));
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
