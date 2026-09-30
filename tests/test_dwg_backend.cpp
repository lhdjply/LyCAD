// =============================================================================
//  LyCAD - tests/test_dwg_backend.cpp
//  DWG 后端适配链路的测试。
//
//  为什么这样测
//    DWG 为闭源二进制格式，本软件不自行解析，而是驱动外部转换器完成
//    「DWG ↔ DXF」的转换，再交由自带的 DXF 编解码器处理。这条链路无法用
//    真实的转换器做自动化测试（CI 环境通常没有 ODA/LibreDWG），但它的
//    逻辑——参数展开、临时目录编排、失败处理、能力上报——完全可以被验证。
//
//    做法：注册一个行为确定的「替代转换器」（一段复制文件的脚本），
//    通过自定义转换命令接进去，然后走完整的读取与写出流程。
//    这样测到的是真实的进程调用、真实的临时文件编排、真实的错误分支，
//    而不是被打桩的函数。
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
#include <QSettings>

using namespace lycad;

class TestDwgBackend : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void versionSignatureTable();
    void probeRecognizesDwgOnly();
    void withoutBackendReportsActionableError();
    void readThroughExternalConverter();
    void writeThroughExternalConverter();
    void converterFailureIsReportedNotSwallowed();

private:
    /// 生成一个可执行脚本，内容为把 $1 复制到 $2。
    /// stripSignature 为 true 时先丢弃前 6 字节，模拟真实的 DWG→DXF 转换
    /// （DWG 文件头带 AC10xx 签名，转换后的 DXF 不含该签名）。
    QString createCopyConverter(const QString& name, bool stripSignature);
    /// 生成一个必定失败的脚本
    QString createFailingConverter(const QString& name);

    void setCustomConverter(const QString& command);
    void clearCustomConverter();

    QTemporaryDir m_dir;
};

// -----------------------------------------------------------------------------
void TestDwgBackend::initTestCase()
{
    QVERIFY2(m_dir.isValid(), "无法创建临时目录");

    // 隔离配置：DwgCodec 通过默认构造的 QSettings 读取自定义转换命令，
    // 而 QSettings 由应用名决定存储位置。改掉应用名即可与用户真实配置隔离。
    QCoreApplication::setOrganizationName(QStringLiteral("LyCAD-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("LyCAD-Test"));

    clearCustomConverter();
    registerBuiltinCodecs();
}

void TestDwgBackend::cleanupTestCase()
{
    clearCustomConverter();
}

void TestDwgBackend::setCustomConverter(const QString& command)
{
    QSettings settings;
    settings.setValue(DwgCodec::customCommandSettingKey(), command);
    settings.sync();
}

void TestDwgBackend::clearCustomConverter()
{
    QSettings settings;
    settings.remove(DwgCodec::customCommandSettingKey());
    settings.sync();
}

QString TestDwgBackend::createCopyConverter(const QString& name, bool stripSignature)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return QString();

    // 参数模板 %%IN%% / %%OUT%% 由 DwgCodec 展开后作为 $1 / $2 传入
    if (stripSignature) {
        file.write("#!/bin/sh\nset -e\ntail -c +7 \"$1\" > \"$2\"\n");
    } else {
        file.write("#!/bin/sh\nset -e\ncp \"$1\" \"$2\"\n");
    }
    file.close();

    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                                | QFile::ReadGroup | QFile::ExeGroup
                                | QFile::ReadOther | QFile::ExeOther);
    return path;
}

QString TestDwgBackend::createFailingConverter(const QString& name)
{
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return QString();

    file.write("#!/bin/sh\necho '模拟转换器内部错误' >&2\nexit 3\n");
    file.close();

    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                                | QFile::ReadGroup | QFile::ExeGroup
                                | QFile::ReadOther | QFile::ExeOther);
    return path;
}

// -----------------------------------------------------------------------------
void TestDwgBackend::versionSignatureTable()
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

void TestDwgBackend::probeRecognizesDwgOnly()
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

    // 按扩展名路由必须命中 DWG 编解码器：
    // 即便当前缺少转换后端，也应返回该编解码器，
    // 这样调用方才能给出「缺少什么、如何安装」的具体说明，
    // 而不是一句「不支持的文件类型」。
    ICadCodec* dwgCodec = registry.codecForPath(dwgPath, false);
    QVERIFY(dwgCodec != nullptr);
    QCOMPARE(dwgCodec->name(), QStringLiteral("DWG"));

    QCOMPARE(registry.codecForPath(dxfPath, false)->name(), QStringLiteral("DXF"));

    // 格式已知但不可用时，canLoad 应为 false，同时能取到原因
    if (!dwgCodec->canRead()) {
        QVERIFY(!CadFile::canLoad(dwgPath));
        QVERIFY(!CadFile::capabilityNoteFor(dwgPath).isEmpty());
    }

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

void TestDwgBackend::withoutBackendReportsActionableError()
{
    clearCustomConverter();

    DwgCodec dwg;
    if (dwg.canRead()) {
        QSKIP("本机存在可用的 DWG 转换后端，跳过「无后端」分支的验证");
    }

    // 能力说明必须给出可执行的下一步，而不是一句「不支持」
    const QString note = dwg.capabilityNote();
    QVERIFY(note.contains(QStringLiteral("LibreDWG")));
    QVERIFY(note.contains(QStringLiteral("ODA")));
    QVERIFY(note.contains(QStringLiteral("%IN%")));
    QVERIFY(note.contains(QStringLiteral("DXF")));

    // 探测 DWG 的版本识别不依赖后端，必须始终可用
    const QString path = m_dir.filePath(QStringLiteral("nobackend.dwg"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("AC1018");
        file.write(QByteArray(256, '\0'));
    }

    Document document;
    const IoResult result = dwg.read(path, document);
    QVERIFY(!result.ok);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(document.entityCount(), std::size_t(0));
}

void TestDwgBackend::readThroughExternalConverter()
{
    const QString converter = createCopyConverter(QStringLiteral("fake_dwg2dxf.sh"), true);
    QVERIFY(!converter.isEmpty());
    setCustomConverter(QStringLiteral("%1 %IN% %OUT%").arg(converter));

    DwgCodec dwg;
    QVERIFY2(dwg.canRead(), "配置了自定义转换命令后应上报可读");
    QVERIFY(dwg.availableBackendNames().contains(QStringLiteral("自定义转换命令")));

    // 先造一份合法的 DXF 作为「转换结果」
    const QString dxfPath = m_dir.filePath(QStringLiteral("payload.dxf"));
    {
        Document source;
        source.beginBatchLoad();
        source.layers().ensure(QStringLiteral("测试层")).setColorIndex(2);
        source.addEntity(std::make_unique<LineEntity>(Point2D(0.0, 0.0), Point2D(30.0, 40.0)));
        source.addEntity(std::make_unique<CircleEntity>(Point2D(5.0, 5.0), 2.5));
        source.endBatchLoad();

        const IoResult writeResult = CadFile::save(dxfPath, source, WriteOptions{});
        QVERIFY2(writeResult.ok, qPrintable(writeResult.error));
    }

    // 伪装成 DWG：带 DWG 文件头签名 + DXF 内容。
    // 替代转换器只做文件复制，因此 LyCAD 最终读到的就是上面那份 DXF 内容。
    const QString dwgPath = m_dir.filePath(QStringLiteral("converted.dwg"));
    {
        QFile source(dxfPath);
        QFile target(dwgPath);
        QVERIFY(source.open(QIODevice::ReadOnly));
        QVERIFY(target.open(QIODevice::WriteOnly));
        target.write("AC1015");
        target.write(source.readAll());
    }

    Document document;
    const IoResult result = dwg.read(dwgPath, document);

    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(document.entityCount(), std::size_t(2));
    QCOMPARE(document.layers().count(), std::size_t(2));      // 0 层 + 测试层

    const Entity* first = document.entity(document.drawOrder().at(0));
    QCOMPARE(first->type(), EntityType::Line);
    QVERIFY(std::abs(first->length() - 50.0) < 1e-9);          // 3-4-5 直角三角形

    // 转换后端与源文件版本应出现在提示中，便于用户追溯
    const QString joined = result.warnings.join(QLatin1Char('\n'));
    QVERIFY(joined.contains(QStringLiteral("AC1015")));
    QVERIFY(joined.contains(QStringLiteral("自定义转换命令")));
}

void TestDwgBackend::writeThroughExternalConverter()
{
    const QString converter = createCopyConverter(QStringLiteral("fake_dxf2dwg.sh"), false);
    QVERIFY(!converter.isEmpty());
    setCustomConverter(QStringLiteral("%1 %IN% %OUT%").arg(converter));

    DwgCodec dwg;
    QVERIFY2(dwg.canWrite(), "配置了自定义转换命令后应上报可写");

    Document document;
    document.beginBatchLoad();
    document.addEntity(std::make_unique<LineEntity>(Point2D(0.0, 0.0), Point2D(10.0, 0.0)));
    document.endBatchLoad();

    const QString dwgPath = m_dir.filePath(QStringLiteral("out.dwg"));
    const IoResult result = dwg.write(dwgPath, document, WriteOptions{});

    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(QFileInfo::exists(dwgPath));
    QVERIFY(QFileInfo(dwgPath).size() > 0);

    // 中间 DXF 必须以 R2000 生成：这是兼容性最好的中间格式
    QFile file(dwgPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray content = file.readAll();
    QVERIFY(content.contains("AC1015"));
    QVERIFY(content.contains("ENTITIES"));
    QVERIFY(content.contains("LINE"));
}

void TestDwgBackend::converterFailureIsReportedNotSwallowed()
{
    const QString converter = createFailingConverter(QStringLiteral("failing_converter.sh"));
    QVERIFY(!converter.isEmpty());
    setCustomConverter(QStringLiteral("%1 %IN% %OUT%").arg(converter));

    DwgCodec dwg;
    QVERIFY(dwg.canRead());

    const QString path = m_dir.filePath(QStringLiteral("will_fail.dwg"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("AC1015");
        file.write(QByteArray(256, '\0'));
    }

    Document document;
    const IoResult result = dwg.read(path, document);

    // 转换器失败必须如实上报，且不得把半成品留在文档里
    QVERIFY(!result.ok);
    QVERIFY(result.error.contains(QStringLiteral("错误码")));
    QVERIFY(result.error.contains(QStringLiteral("自定义转换命令")));
    QCOMPARE(document.entityCount(), std::size_t(0));

    clearCustomConverter();
}

QTEST_MAIN(TestDwgBackend)
#include "test_dwg_backend.moc"
