// =============================================================================
//  LyCAD - io/DwgCodec.cpp
// =============================================================================
#include "io/DwgCodec.h"

#include "core/Document.h"
#include "io/DxfCodec.h"
#include "io/LibDxfRwAdapter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>

namespace lycad {

namespace {

/// DWG 文件头的签名长度
constexpr int kSignatureLength = 6;

/// 转换器调用的超时上限（毫秒）。大型图纸的转换可能耗时较长。
constexpr int kConverterTimeoutMs = 10 * 60 * 1000;

/// DWG 是二进制格式：文件头之后很快就会出现不可打印字节。
///
/// 这个检查用来挡住「套着 DWG 签名的文本文件」——转换器失败留下的残件、
/// 被误改扩展名的 DXF 都属于这一类。把它们直接交给解析器，会让它在不合法
/// 数据上长时间空转（实测一份伪装文件耗掉了近两分钟，界面会整个卡住）。
/// 这里只用读 512 字节的代价，把它们挡在解析之前。
bool looksLikeBinaryDwg(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;

    const QByteArray head = file.read(512);
    if (head.size() <= 32) return false;

    int odd = 0;
    for (int i = kSignatureLength; i < head.size(); ++i) {
        const uchar c = static_cast<uchar>(head.at(i));
        const bool printable =
            (c >= 0x20 && c <= 0x7E) || c == '\r' || c == '\n' || c == '\t';
        if (!printable) ++odd;
    }

    // 真 DWG 的头部以二进制数据为主；文本伪装文件则几乎全是可打印字符
    return odd * 100 >= (head.size() - kSignatureLength) * 10;
}

/// 版本签名 -> 可读名称
const std::vector<std::pair<QString, QString>>& versionTable()
{
    static const std::vector<std::pair<QString, QString>> kTable = {
        { QStringLiteral("AC1006"), QStringLiteral("AutoCAD R10") },
        { QStringLiteral("AC1009"), QStringLiteral("AutoCAD R11/R12") },
        { QStringLiteral("AC1012"), QStringLiteral("AutoCAD R13") },
        { QStringLiteral("AC1014"), QStringLiteral("AutoCAD R14") },
        { QStringLiteral("AC1015"), QStringLiteral("AutoCAD 2000/2000i/2002") },
        { QStringLiteral("AC1018"), QStringLiteral("AutoCAD 2004/2005/2006") },
        { QStringLiteral("AC1021"), QStringLiteral("AutoCAD 2007/2008/2009") },
        { QStringLiteral("AC1024"), QStringLiteral("AutoCAD 2010/2011/2012") },
        { QStringLiteral("AC1027"), QStringLiteral("AutoCAD 2013-2017") },
        { QStringLiteral("AC1032"), QStringLiteral("AutoCAD 2018 及更新") },
    };
    return kTable;
}

/// 在 PATH 中查找可执行文件
QString resolveExecutable(const QString& program)
{
    if (program.isEmpty()) return QString();

    const QFileInfo direct(program);
    if (direct.isAbsolute() && direct.isExecutable()) return direct.absoluteFilePath();

    return QStandardPaths::findExecutable(program);
}

} // namespace

// -----------------------------------------------------------------------------
//  版本识别
// -----------------------------------------------------------------------------
DwgVersionInfo dwgVersionFromSignature(const QByteArray& header)
{
    DwgVersionInfo info;
    if (header.size() < kSignatureLength) return info;

    const QString code = QString::fromLatin1(header.left(kSignatureLength));
    if (!code.startsWith(QLatin1String("AC"))) return info;

    info.code = code;
    for (const auto& entry : versionTable()) {
        if (entry.first == code) {
            info.displayName = entry.second;
            info.recognized = true;
            return info;
        }
    }

    // 签名结构正确但版本号未知：仍视为 DWG，交由转换器判定
    info.displayName = QStringLiteral("未知版本 (%1)").arg(code);
    return info;
}

DwgVersionInfo dwgVersionOfFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return DwgVersionInfo{};
    return dwgVersionFromSignature(file.read(kSignatureLength));
}

// -----------------------------------------------------------------------------
//  DwgCodec
// -----------------------------------------------------------------------------
DwgCodec::DwgCodec() = default;

QString DwgCodec::name() const
{
    return QStringLiteral("DWG");
}

QString DwgCodec::displayName() const
{
    return QStringLiteral("DWG 图形文件");
}

QStringList DwgCodec::readExtensions() const
{
    return { QStringLiteral("dwg") };
}

QStringList DwgCodec::writeExtensions() const
{
    return { QStringLiteral("dwg") };
}

QString DwgCodec::customCommandSettingKey()
{
    return QStringLiteral("dwg/converterCommand");
}

bool DwgCodec::probe(QIODevice& device) const
{
    if (!device.isOpen() && !device.open(QIODevice::ReadOnly)) return false;

    const qint64 originalPos = device.pos();
    const QByteArray head = device.read(kSignatureLength);
    if (!device.isSequential()) device.seek(originalPos);

    return dwgVersionFromSignature(head).recognized
        || (head.size() == kSignatureLength && head.startsWith("AC10"));
}

// -----------------------------------------------------------------------------
//  后端探测
// -----------------------------------------------------------------------------
void DwgCodec::detectBackends() const
{
    if (m_detected) return;
    m_detected = true;
    m_backends.clear();

    // 1) 用户自定义命令优先级最高：显式配置代表用户的明确意图
    if (Backend custom = customBackend(); custom.found) {
        custom.displayName = QStringLiteral("自定义转换命令");
        m_backends.push_back(std::move(custom));
    }

    // 2) LibreDWG：dwg2dxf / dxf2dwg
    {
        Backend backend;
        backend.displayName = QStringLiteral("LibreDWG");
        backend.odaStyle = false;

        const QString dwg2dxf = resolveExecutable(QStringLiteral("dwg2dxf"));
        const QString dxf2dwg = resolveExecutable(QStringLiteral("dxf2dwg"));

        if (!dwg2dxf.isEmpty()) {
            backend.program = QStringLiteral("dwg2dxf");
            backend.resolvedPath = dwg2dxf;
            backend.readArgs = { QStringLiteral("-y"), QStringLiteral("-o"), QStringLiteral("%OUT%"),
                                 QStringLiteral("%IN%") };
            backend.found = true;
        }
        if (!dxf2dwg.isEmpty()) {
            if (!backend.found) {
                backend.program = QStringLiteral("dxf2dwg");
                backend.resolvedPath = dxf2dwg;
                backend.found = true;
            }
            backend.writeArgs = { QStringLiteral("-y"), QStringLiteral("-o"), QStringLiteral("%OUT%"),
                                  QStringLiteral("%IN%") };
        }
        if (backend.found) {
            if (backend.readArgs.isEmpty()) {
                backend.note = QStringLiteral("仅检测到 dxf2dwg，DWG 写出可用、读取不可用。");
            }
            m_backends.push_back(std::move(backend));
        }
    }

    // 3) ODA File Converter：目录进目录出
    {
        const QString oda = resolveExecutable(QStringLiteral("ODAFileConverter"));
        if (!oda.isEmpty()) {
            Backend backend;
            backend.displayName = QStringLiteral("ODA File Converter");
            backend.program = QStringLiteral("ODAFileConverter");
            backend.resolvedPath = oda;
            backend.odaStyle = true;
            backend.found = true;
            // 实际参数在 runConverter 中按目录结构展开
            m_backends.push_back(std::move(backend));
        }
    }

    // 按「读能力优先」排序，保证读取尽量走最可靠的后端
    std::stable_sort(m_backends.begin(), m_backends.end(),
                     [](const Backend& a, const Backend& b) {
                         const bool aRead = a.odaStyle || !a.readArgs.isEmpty();
                         const bool bRead = b.odaStyle || !b.readArgs.isEmpty();
                         return aRead && !bRead;
                     });
}

DwgCodec::Backend DwgCodec::customBackend()
{
    Backend backend;

    // 使用默认构造的 QSettings，它会取用应用级的组织名/应用名。
    // 相比把 "LyCAD"/"LyCAD" 写死在此处，这样既保持了运行时的同一份配置，
    // 又允许测试通过修改应用名来隔离配置，不会污染用户的真实设置。
    QSettings settings;
    const QString command = settings.value(customCommandSettingKey()).toString().trimmed();
    if (command.isEmpty()) return backend;

    const QStringList parts = QProcess::splitCommand(command);
    if (parts.isEmpty()) return backend;

    const QString program = parts.first();
    const QString resolved = resolveExecutable(program);
    if (resolved.isEmpty()) {
        backend.displayName = QStringLiteral("自定义转换命令");
        backend.note = QStringLiteral("设置中的自定义转换命令 \"%1\" 不存在或不可执行。").arg(program);
        return backend;
    }

    backend.program = program;
    backend.resolvedPath = resolved;
    backend.readArgs = parts.mid(1);
    backend.writeArgs = parts.mid(1);      // 同一模板，方向由参数中的占位符决定
    backend.found = true;
    return backend;
}

QStringList DwgCodec::availableBackendNames() const
{
    detectBackends();

    QStringList out;
    // 内置解析器排在最前：它是进程内的，不需要任何外部程序
    if (libDxfRwAvailable()) out << QStringLiteral("内置 libdxfrw（原生读取）");
    for (const Backend& b : m_backends) {
        if (b.found) out << b.displayName;
    }
    return out;
}

const DwgCodec::Backend* DwgCodec::backendForRead() const
{
    detectBackends();
    for (const Backend& b : m_backends) {
        if (b.found && (b.odaStyle || !b.readArgs.isEmpty())) return &b;
    }
    return nullptr;
}

const DwgCodec::Backend* DwgCodec::backendForWrite() const
{
    detectBackends();
    for (const Backend& b : m_backends) {
        if (b.found && (b.odaStyle || !b.writeArgs.isEmpty())) return &b;
    }
    return nullptr;
}

bool DwgCodec::canRead() const
{
    // 内置 libdxfrw 已随程序编入，因此读取能力不依赖任何外部程序
    return libDxfRwAvailable() || backendForRead() != nullptr;
}

bool DwgCodec::canWrite() const
{
    return backendForWrite() != nullptr;
}

QString DwgCodec::capabilityNote() const
{
    detectBackends();

    if (libDxfRwAvailable() && canWrite()) {
        return QStringLiteral(
                   "DWG 读取：内置 libdxfrw 原生解析，无需外部程序。\n"
                   "DWG 写出：经 %1 转换为 DXF 后再输出——libdxfrw 只读 DWG，不提供写出能力。")
            .arg(availableBackendNames().join(QStringLiteral("、")));
    }

    if (libDxfRwAvailable()) {
        return QStringLiteral(
            "DWG 导入可直接使用：内置 libdxfrw 原生解析，不需要外部程序。\n"
            "DWG 导出需要一个外部转换器（libdxfrw 只读 DWG，本身不具备写出能力）。"
            "请任选其一安装：\n"
            "  · LibreDWG  —— 提供 dwg2dxf / dxf2dwg 命令\n"
            "  · ODA File Converter —— Autodesk 官方免费转换工具\n"
            "  · 或在 设置 → DWG 转换命令 中填写自定义转换命令模板（含 %IN% 与 %OUT% 占位符）\n"
            "在安装之前，DXF 的导入导出完全可用。");
    }

    return QStringLiteral(
        "当前既没有内置 libdxfrw，也没有可用的 DWG 转换后端，因此 DWG 导入导出均不可用。\n"
        "请任选其一安装：\n"
        "  · LibreDWG  —— 提供 dwg2dxf / dxf2dwg 命令\n"
        "  · ODA File Converter —— Autodesk 官方免费转换工具\n"
        "  · 或在 设置 → DWG 转换命令 中填写自定义转换命令模板（含 %IN% 与 %OUT% 占位符）\n"
        "在未安装前，DXF 的导入导出完全可用。");
}

// -----------------------------------------------------------------------------
//  读取
// -----------------------------------------------------------------------------
IoResult DwgCodec::read(const QString& path, Document& document) const
{
    const DwgVersionInfo version = dwgVersionOfFile(path);
    if (!version.recognized && version.code.isEmpty()) {
        return IoResult::failure(QStringLiteral("该文件不具备 DWG 的文件头签名，可能不是 DWG 文件。"));
    }

    const QString versionNote =
        version.recognized
            ? QStringLiteral("源文件版本：%1 [%2]").arg(version.displayName, version.code)
            : QStringLiteral("源文件版本未能识别（签名 %1）").arg(version.code);

    // ---- 首选：内置 libdxfrw 原生解析 ----------------------------------------
    // 进程内完成，不依赖外部程序、不产生中间文件，也不受某个转换器版本支持范围的限制。
    // 但要先过一道内容检查：不是真的二进制内容就不必交给解析器（见 looksLikeBinaryDwg）。
    if (libDxfRwAvailable() && looksLikeBinaryDwg(path)) {
        IoResult native = readDwgWithLibDxfRw(path, document);
        if (native.ok) {
            native.addWarning(versionNote);
            return native;
        }

        // ---- 回退：外部转换器 -------------------------------------------------
        // 保留原生解析的失败原因。若该文件恰好是转换器能处理而我们读不了的，
        // 用户应当知道真相，而不是只看到一句「转换失败」。
        const Backend* fallback = backendForRead();
        if (!fallback) return native;

        const QString nativeError = native.error;
        // readViaDxf 会重新装载文档，原生解析留下的半份内容不会残留
        IoResult converted = readViaDxf(path, document, *fallback);
        if (converted.ok) {
            converted.addWarning(
                QStringLiteral("内置解析器未能读取该文件（%1），已改用 %2 转换为 DXF 后导入。")
                    .arg(nativeError, fallback->displayName));
        }
        converted.addWarning(versionNote);
        return converted;
    }

    // ---- 未编入内置解析器：只能走外部转换器 -----------------------------------
    const Backend* backend = backendForRead();
    if (!backend) {
        IoResult result = IoResult::failure(capabilityNote());
        result.addWarning(versionNote);
        return result;
    }

    IoResult result = readViaDxf(path, document, *backend);
    result.addWarning(QStringLiteral("%1，经 %2 转换为 DXF 后导入")
                          .arg(versionNote, backend->displayName));
    return result;
}

IoResult DwgCodec::readViaDxf(const QString& path, Document& document,
                              const Backend& backend) const
{
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        return IoResult::failure(QStringLiteral("无法创建临时目录：%1").arg(tempDir.errorString()));
    }

    const QString dxfPath = QDir(tempDir.path()).filePath(QStringLiteral("converted.dxf"));

    const IoResult conversion = runConverter(backend, true, path, dxfPath);
    if (!conversion.ok) return conversion;

    if (!QFile::exists(dxfPath)) {
        return IoResult::failure(
            QStringLiteral("%1 未生成中间 DXF 文件，转换可能失败。").arg(backend.displayName));
    }

    DxfCodec dxf;
    IoResult result = dxf.read(dxfPath, document);
    if (!result.ok) {
        return IoResult::failure(
            QStringLiteral("中间 DXF 解析失败：%1（转换后端 %2）").arg(result.error, backend.displayName));
    }
    return result;
}

// -----------------------------------------------------------------------------
//  写出
// -----------------------------------------------------------------------------
IoResult DwgCodec::write(const QString& path, const Document& document,
                         const WriteOptions& options) const
{
    const Backend* backend = backendForWrite();
    if (!backend) {
        return IoResult::failure(capabilityNote());
    }
    return writeViaDxf(path, document, options, *backend);
}

IoResult DwgCodec::writeViaDxf(const QString& path, const Document& document,
                               const WriteOptions& options, const Backend& backend) const
{
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        return IoResult::failure(QStringLiteral("无法创建临时目录：%1").arg(tempDir.errorString()));
    }

    const QString dxfPath = QDir(tempDir.path()).filePath(QStringLiteral("intermediate.dxf"));

    // 中间文件一律用 R2000，兼容性最好，转换器对其支持也最完整
    WriteOptions intermediate = options;
    intermediate.version = QStringLiteral("AC1015");

    DxfCodec dxf;
    IoResult writeResult = dxf.write(dxfPath, document, intermediate);
    if (!writeResult.ok) {
        return IoResult::failure(QStringLiteral("生成中间 DXF 失败：%1").arg(writeResult.error));
    }

    const IoResult conversion = runConverter(backend, false, dxfPath, path);
    if (!conversion.ok) return conversion;

    if (!QFile::exists(path)) {
        return IoResult::failure(
            QStringLiteral("%1 未生成目标 DWG 文件，转换可能失败。").arg(backend.displayName));
    }

    IoResult result = IoResult::success(static_cast<int>(document.entityCount()));
    result.addWarning(QStringLiteral("已经 %1 转换为 DWG").arg(backend.displayName));
    for (const QString& w : writeResult.warnings) result.addWarning(w);
    return result;
}

// -----------------------------------------------------------------------------
//  调用转换器
// -----------------------------------------------------------------------------
namespace {

/// 统一的进程执行封装：启动、超时、退出码检查
IoResult runProcess(const QString& displayName, const QString& program,
                    const QStringList& args)
{
    QProcess process;
    process.setProgram(program);
    process.setArguments(args);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();

    if (!process.waitForStarted(15000)) {
        return IoResult::failure(
            QStringLiteral("无法启动 %1：%2").arg(displayName, process.errorString()));
    }
    if (!process.waitForFinished(kConverterTimeoutMs)) {
        process.kill();
        process.waitForFinished(3000);
        return IoResult::failure(
            QStringLiteral("%1 转换超时（超过 %2 分钟）。")
                .arg(displayName).arg(kConverterTimeoutMs / 60000));
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString log = QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed();
        return IoResult::failure(QStringLiteral("%1 返回错误码 %2。%3")
                                     .arg(displayName)
                                     .arg(process.exitCode())
                                     .arg(log.isEmpty() ? QString() : QStringLiteral("\n") + log));
    }
    return IoResult::success();
}

} // namespace

IoResult DwgCodec::runConverter(const Backend& backend, bool forReading,
                                const QString& input, const QString& output) const
{
    if (backend.odaStyle) {
        return runOdaConverter(backend, forReading, input, output);
    }

    // 通用命令行式后端：按 %IN% / %OUT% 占位符展开参数
    const QStringList& pattern = forReading ? backend.readArgs : backend.writeArgs;
    if (pattern.isEmpty()) {
        return IoResult::failure(QStringLiteral("%1 不具备%2能力。")
                                     .arg(backend.displayName,
                                          forReading ? QStringLiteral("读取")
                                                     : QStringLiteral("写出")));
    }

    QStringList args;
    args.reserve(pattern.size());
    for (const QString& token : pattern) {
        QString expanded = token;
        expanded.replace(QStringLiteral("%IN%"), input);
        expanded.replace(QStringLiteral("%OUT%"), output);
        args << expanded;
    }

    return runProcess(backend.displayName, backend.resolvedPath, args);
}

IoResult DwgCodec::runOdaConverter(const Backend& backend, bool forReading,
                                   const QString& input, const QString& output) const
{
    // ODA File Converter 的命令行接口是「目录进、目录出」：
    //   ODAFileConverter <输入目录> <输出目录> <输出版本> <输出类型> <递归子目录> <审计修复> [过滤器]
    // 因此需要先把源文件暂存到独立的输入目录，再从输出目录取回结果。
    QTemporaryDir sandbox;
    if (!sandbox.isValid()) {
        return IoResult::failure(QStringLiteral("无法创建 ODA 转换所需的临时目录。"));
    }

    const QFileInfo inputInfo(input);
    const QString inDir = QDir(sandbox.path()).filePath(QStringLiteral("in"));
    const QString outDir = QDir(sandbox.path()).filePath(QStringLiteral("out"));
    if (!QDir().mkpath(inDir) || !QDir().mkpath(outDir)) {
        return IoResult::failure(QStringLiteral("无法建立 ODA 转换的输入/输出目录。"));
    }

    const QString staged = QDir(inDir).filePath(inputInfo.fileName());
    if (!QFile::copy(input, staged)) {
        return IoResult::failure(QStringLiteral("无法把源文件暂存到转换目录：%1").arg(staged));
    }

    const QStringList args = {
        inDir,
        outDir,
        QStringLiteral("ACAD2018"),                       // 输出版本
        forReading ? QStringLiteral("DXF") : QStringLiteral("DWG"),
        QStringLiteral("0"),                              // 不递归子目录
        QStringLiteral("1"),                              // 转换前执行审计修复
        QStringLiteral("*")                               // 处理全部文件
    };

    const IoResult run = runProcess(backend.displayName, backend.resolvedPath, args);
    if (!run.ok) return run;

    const QString producedName = inputInfo.completeBaseName()
                               + (forReading ? QStringLiteral(".dxf") : QStringLiteral(".dwg"));
    const QString produced = QDir(outDir).filePath(producedName);
    if (!QFile::exists(produced)) {
        return IoResult::failure(
            QStringLiteral("%1 未在输出目录生成预期的 %2。").arg(backend.displayName, producedName));
    }

    if (QFile::exists(output) && !QFile::remove(output)) {
        return IoResult::failure(QStringLiteral("无法覆盖已存在的输出文件：%1").arg(output));
    }
    if (!QFile::copy(produced, output)) {
        return IoResult::failure(QStringLiteral("无法把转换结果复制到：%1").arg(output));
    }
    return IoResult::success();
}

} // namespace lycad
