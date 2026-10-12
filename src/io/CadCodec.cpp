// =============================================================================
//  LyCAD - io/CadCodec.cpp
// =============================================================================
#include "io/CadCodec.h"

#include <QFileInfo>

#include <utility>
#include <vector>

namespace lycad {

// -----------------------------------------------------------------------------
//  WriteOptions
// -----------------------------------------------------------------------------
namespace {

/// 一档可写出的 DXF 目标版本
struct DxfVersionInfo {
    QString code;         ///< "AC1015"：写进文件头 $ACADVER 的值
    QString displayName;  ///< "AutoCAD 2000 (AC1015)"：消息与提示里的完整名字
    QString shortName;    ///< "AutoCAD 2000"：另存为的逐版本过滤器里的名字
};

/// 可写出的 DXF 目标版本，按版本从低到高排列。
///
/// 这张表是**唯一的版本清单**：界面过滤器、`DxfCodec::write` 的归一化、
/// 以及写出端的 DRW::Version 映射都以它为准，新增版本只需改这一处。
/// 版本代号与 libdxfrw 的 DRW::Version 一一对应（顺序也一致）。
///
/// **不含 R12 (AC1009)**：R12 没有 HATCH / SPLINE / ELLIPSE / XLINE / RAY，
/// 也没有 TrueType 与 MTEXT，写出时要把这些图元降级改写一遍。这个降级通道
/// 已经移除——**读取** R12 文件仍然支持（走 Builder，与写出无关），
/// 但本软件不再把图纸写成 R12。
const std::vector<DxfVersionInfo>& dxfVersionTable()
{
    static const std::vector<DxfVersionInfo> kTable = {
        { QStringLiteral("AC1015"), QStringLiteral("AutoCAD 2000 (AC1015)"),
          QStringLiteral("AutoCAD 2000") },
        { QStringLiteral("AC1018"), QStringLiteral("AutoCAD 2004 (AC1018)"),
          QStringLiteral("AutoCAD 2004") },
        { QStringLiteral("AC1021"), QStringLiteral("AutoCAD 2007 (AC1021)"),
          QStringLiteral("AutoCAD 2007") },
        { QStringLiteral("AC1024"), QStringLiteral("AutoCAD 2010 (AC1024)"),
          QStringLiteral("AutoCAD 2010") },
        { QStringLiteral("AC1027"), QStringLiteral("AutoCAD 2013 (AC1027)"),
          QStringLiteral("AutoCAD 2013") },
        { QStringLiteral("AC1032"), QStringLiteral("AutoCAD 2018 (AC1032)"),
          QStringLiteral("AutoCAD 2018") },
    };
    return kTable;
}

} // namespace

QString WriteOptions::versionDisplayName(const QString& version)
{
    for (const auto& entry : dxfVersionTable()) {
        if (entry.code.compare(version, Qt::CaseInsensitive) == 0) return entry.displayName;
    }
    return version;
}

QString WriteOptions::versionShortName(const QString& version)
{
    for (const auto& entry : dxfVersionTable()) {
        if (entry.code.compare(version, Qt::CaseInsensitive) == 0) return entry.shortName;
    }
    return version;
}

QStringList WriteOptions::availableDxfVersions()
{
    QStringList versions;
    versions.reserve(static_cast<qsizetype>(dxfVersionTable().size()));
    for (const auto& entry : dxfVersionTable()) versions << entry.code;
    return versions;
}

// -----------------------------------------------------------------------------
//  DWG 写出版本
//
//  与 DXF 分开成一路：DWG 的容器格式随版本变化（R2000 线性 / R2004 分页 +
//  LZ77），写出版本因此必须显式选择，不能跟着 DXF 的 version 走。
//  按版本从低到高排列，界面据此生成「另存为」的逐版本条目。
// -----------------------------------------------------------------------------
QStringList WriteOptions::availableDwgVersions()
{
    return { QStringLiteral("AC1015"), QStringLiteral("AC1018"), QStringLiteral("AC1021"),
             QStringLiteral("AC1024"), QStringLiteral("AC1027") };
}

QString WriteOptions::defaultDwgVersion()
{
    // AC1027（AutoCAD 2013 / R2013）：与 R2004 / R2010 同族容器（0x80 加密文件头 +
    // 页/段映射 + 分页），版本常量取 libredwg 版本表权威值（0x0B=0x7d / 0x11=0x1f）。
    // 较之 AC1024 更贴近现代 CAD 的默认保存版本。
    // 必须与 WriteOptions::dwgVersion 的初值保持一致。
    return QStringLiteral("AC1027");
}

QString WriteOptions::dwgVersionShortName(const QString& version)
{
    if (version.compare(QStringLiteral("AC1015"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("AutoCAD 2000");
    if (version.compare(QStringLiteral("AC1018"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("AutoCAD 2004");
    if (version.compare(QStringLiteral("AC1021"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("AutoCAD 2007");
    if (version.compare(QStringLiteral("AC1024"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("AutoCAD 2010");
    if (version.compare(QStringLiteral("AC1027"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("AutoCAD 2013");
    return version;
}

bool WriteOptions::isWritableDwgVersion(const QString& version)
{
    return availableDwgVersions().contains(version, Qt::CaseInsensitive);
}

// -----------------------------------------------------------------------------
//  IoResult
// -----------------------------------------------------------------------------
IoResult IoResult::success(int entityCount)
{
    IoResult r;
    r.ok = true;
    r.entityCount = entityCount;
    return r;
}

IoResult IoResult::failure(const QString& message)
{
    IoResult r;
    r.ok = false;
    r.error = message;
    return r;
}

void IoResult::addWarning(const QString& warning)
{
    if (!warning.isEmpty()) warnings.append(warning);
}

QString IoResult::summary() const
{
    if (!ok) return QStringLiteral("失败: %1").arg(error);
    QString s = QStringLiteral("成功读取 %1 个图元").arg(entityCount);
    if (hasWarnings()) s += QStringLiteral("，%1 条警告").arg(warnings.size());
    return s;
}

// -----------------------------------------------------------------------------
//  ICadCodec
// -----------------------------------------------------------------------------
ICadCodec::~ICadCodec() = default;

bool ICadCodec::extensionMatches(const QString& path, const QStringList& extensions) const
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix.isEmpty()) return false;
    return extensions.contains(suffix);
}

bool ICadCodec::hasReadExtension(const QString& path) const
{
    return extensionMatches(path, readExtensions());
}

bool ICadCodec::hasWriteExtension(const QString& path) const
{
    return extensionMatches(path, writeExtensions());
}

bool ICadCodec::canReadPath(const QString& path) const
{
    return canRead() && hasReadExtension(path);
}

bool ICadCodec::canWritePath(const QString& path) const
{
    return canWrite() && hasWriteExtension(path);
}

QString ICadCodec::readFilter() const
{
    QStringList patterns;
    for (const QString& ext : readExtensions()) patterns << QStringLiteral("*.%1").arg(ext);
    return QStringLiteral("%1 (%2)").arg(displayName(), patterns.join(QLatin1Char(' ')));
}

QString ICadCodec::writeFilter() const
{
    QStringList patterns;
    for (const QString& ext : writeExtensions()) patterns << QStringLiteral("*.%1").arg(ext);
    return QStringLiteral("%1 (%2)").arg(displayName(), patterns.join(QLatin1Char(' ')));
}

// -----------------------------------------------------------------------------
//  CodecRegistry
// -----------------------------------------------------------------------------
CodecRegistry& CodecRegistry::instance()
{
    static CodecRegistry registry;
    return registry;
}

void CodecRegistry::registerCodec(std::unique_ptr<ICadCodec> codec)
{
    if (!codec) return;

    // 同名注册视为替换，便于测试与运行时替换后端
    const QString name = codec->name();
    for (auto& existing : m_codecs) {
        if (existing->name() == name) {
            existing = std::move(codec);
            return;
        }
    }
    m_codecs.push_back(std::move(codec));
}

ICadCodec* CodecRegistry::codecForPath(const QString& path, bool forWriting) const
{
    // 第一轮：扩展名匹配且当前确实可用——正常路径走这里
    for (const auto& codec : m_codecs) {
        const bool extensionOk = forWriting ? codec->hasWriteExtension(path)
                                            : codec->hasReadExtension(path);
        if (!extensionOk) continue;

        const bool capable = forWriting ? codec->canWrite() : codec->canRead();
        if (capable) return codec.get();
    }

    // 第二轮：格式已知但能力不足（例如某个格式的解析器未编入）。
    // 这里仍返回该编解码器，好让调用方用 capabilityNote() 说明具体原因与解决办法。
    // 若在此处过滤掉，用户看到的将是一句没有信息量的「不支持的文件类型 .dwg」。
    for (const auto& codec : m_codecs) {
        const bool extensionOk = forWriting ? codec->hasWriteExtension(path)
                                            : codec->hasReadExtension(path);
        if (extensionOk) return codec.get();
    }

    return nullptr;
}

ICadCodec* CodecRegistry::codecByName(const QString& name) const
{
    for (const auto& codec : m_codecs) {
        if (codec->name().compare(name, Qt::CaseInsensitive) == 0) return codec.get();
    }
    return nullptr;
}

QStringList CodecRegistry::importFilters() const
{
    QStringList filters;
    QStringList allPatterns;

    for (const auto& codec : m_codecs) {
        if (!codec->canRead()) continue;
        filters << codec->readFilter();
        for (const QString& ext : codec->readExtensions()) {
            const QString pattern = QStringLiteral("*.%1").arg(ext);
            if (!allPatterns.contains(pattern)) allPatterns << pattern;
        }
    }

    if (!allPatterns.isEmpty()) {
        filters.prepend(QStringLiteral("所有支持的图形文件 (%1)").arg(allPatterns.join(QLatin1Char(' '))));
    }
    filters << QStringLiteral("所有文件 (*)");
    return filters;
}

QStringList CodecRegistry::exportFilters() const
{
    QStringList filters;
    for (const auto& codec : m_codecs) {
        if (codec->canWrite()) filters << codec->writeFilter();
    }
    filters << QStringLiteral("所有文件 (*)");
    return filters;
}

QString CodecRegistry::defaultImportFilter() const
{
    for (const auto& codec : m_codecs) {
        if (codec->canRead()) return codec->readFilter();
    }
    return QStringLiteral("所有文件 (*)");
}

QString CodecRegistry::defaultExportFilter() const
{
    for (const auto& codec : m_codecs) {
        if (codec->canWrite()) return codec->writeFilter();
    }
    return QStringLiteral("所有文件 (*)");
}

QStringList CodecRegistry::supportedReadExtensions() const
{
    QStringList out;
    for (const auto& codec : m_codecs) {
        if (!codec->canRead()) continue;
        for (const QString& ext : codec->readExtensions()) {
            if (!out.contains(ext)) out << ext;
        }
    }
    return out;
}

QStringList CodecRegistry::supportedWriteExtensions() const
{
    QStringList out;
    for (const auto& codec : m_codecs) {
        if (!codec->canWrite()) continue;
        for (const QString& ext : codec->writeExtensions()) {
            if (!out.contains(ext)) out << ext;
        }
    }
    return out;
}

QStringList CodecRegistry::knownReadExtensions() const
{
    QStringList out;
    for (const auto& codec : m_codecs) {
        for (const QString& ext : codec->readExtensions()) {
            if (!out.contains(ext)) out << ext;
        }
    }
    return out;
}

} // namespace lycad
