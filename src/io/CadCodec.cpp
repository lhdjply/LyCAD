// =============================================================================
//  LyCAD - io/CadCodec.cpp
// =============================================================================
#include "io/CadCodec.h"

#include <QFileInfo>

namespace lycad {

// -----------------------------------------------------------------------------
//  WriteOptions
// -----------------------------------------------------------------------------
QString WriteOptions::versionDisplayName(const QString& version)
{
    if (version == QLatin1String("AC1009")) return QStringLiteral("AutoCAD R12 (AC1009)");
    if (version == QLatin1String("AC1015")) return QStringLiteral("AutoCAD 2000 (AC1015)");
    if (version == QLatin1String("AC1018")) return QStringLiteral("AutoCAD 2004 (AC1018)");
    return version;
}

QStringList WriteOptions::availableDxfVersions()
{
    return { QStringLiteral("AC1009"), QStringLiteral("AC1015") };
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

    // 第二轮：格式已知但能力不足（典型场景是未安装 DWG 转换后端）。
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
