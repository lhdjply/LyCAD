// =============================================================================
//  LyCAD - io/DwgCodec.cpp
// =============================================================================
#include "io/DwgCodec.h"

#include "core/Document.h"
#include "io/LibDxfRwAdapter.h"

#include <QFile>

#include <utility>
#include <vector>

namespace lycad {

namespace {

/// DWG 文件头的签名长度
constexpr int kSignatureLength = 6;

/// DWG 是二进制格式：文件头之后很快就会出现不可打印字节。
///
/// 这个检查用来挡住「套着 DWG 签名的文本文件」——被误改扩展名的 DXF、
/// 以及损坏后只剩下文本残件的文件都属于这一类。把它们直接交给解析器，
/// 会让它在不合法数据上长时间空转（实测一份伪装文件耗掉了近两分钟，
/// 界面会整个卡住）。这里只用读 512 字节的代价，把它们挡在解析之前。
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

    // 签名结构正确但版本号未知：仍视为 DWG，交由解析器判定
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

QString DwgCodec::writeFilter() const
{
    // 写出版本恒为 AC1015（见 write），过滤器名里点明，
    // 与「另存为」列表中逐版本的 DXF 条目保持同一套说法。
    return QStringLiteral("AutoCAD 2000 DWG 文件 (*.dwg)");
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

QString DwgCodec::capabilityNote() const
{
    return QStringLiteral(
        "DWG 读取：内置 libdxfrw 原生解析，支持 R14 ~ 2018（AC1012 ~ AC1032）。\n"
        "DWG 写出：内置 dwgW 原生写出 R2000（AC1015）。\n"
        "表格（ACAD_TABLE）：R2000 ~ 2018 读取均可从实体本体还原行列结构、"
        "单元格文字、合并与对齐（个别异常文件退回 *Tnn 几何块反推）；\n"
        "写出时几何完整保留且单元格文字随原生表格实体写出，AutoCAD 可直接"
        "编辑表格内容。两者均在进程内完成，无需外部程序。");
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

    // 内容预检：不是真的二进制内容就不必交给解析器（见 looksLikeBinaryDwg）。
    // 这里必须明确失败而不是试着解析——解析器在不合法数据上会空转很久。
    if (!looksLikeBinaryDwg(path)) {
        IoResult result = IoResult::failure(QStringLiteral(
            "该文件带有 DWG 签名，但内容不是 DWG 二进制数据，"
            "可能只是被改名为 .dwg 的 DXF，或损坏后剩下的文本残件。"));
        result.addWarning(versionNote);
        return result;
    }

    IoResult result = readDwgWithLibDxfRw(path, document);
    result.addWarning(versionNote);
    return result;
}

// -----------------------------------------------------------------------------
//  写出
// -----------------------------------------------------------------------------
IoResult DwgCodec::write(const QString& path, const Document& document,
                         const WriteOptions& options) const
{
    // 内置 dwgW 原生写出，目标版本固定 AC1015（AutoCAD 2000/R2000）：
    // 兼容面最广，AutoCAD 2000 及以上的版本都能直接打开。
    IoResult result = writeDwgWithLibDxfRw(path, document, options);
    if (result.ok) {
        result.addWarning(QStringLiteral("已由内置 dwgW 原生写出（AutoCAD 2000/R2000 格式）"));
    }
    return result;
}

} // namespace lycad
