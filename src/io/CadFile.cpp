// =============================================================================
//  LyCAD - io/CadFile.cpp
// =============================================================================
#include "io/CadFile.h"

#include "core/Document.h"
#include "io/DwgCodec.h"
#include "io/DxfCodec.h"

#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

namespace lycad {

void registerBuiltinCodecs()
{
    static QMutex mutex;
    static bool registered = false;

    QMutexLocker locker(&mutex);
    if (registered) return;

    CodecRegistry& registry = CodecRegistry::instance();
    registry.registerCodec(std::make_unique<DxfCodec>());
    registry.registerCodec(std::make_unique<DwgCodec>());

    registered = true;
}

namespace CadFile {

IoResult load(const QString& path, Document& document)
{
    registerBuiltinCodecs();

    const QFileInfo info(path);
    if (!info.exists()) {
        return IoResult::failure(QStringLiteral("文件不存在：%1").arg(path));
    }
    if (!info.isFile()) {
        return IoResult::failure(QStringLiteral("路径不是普通文件：%1").arg(path));
    }
    if (!info.isReadable()) {
        return IoResult::failure(QStringLiteral("文件不可读（请检查权限）：%1").arg(path));
    }

    ICadCodec* codec = CodecRegistry::instance().codecForPath(path, false);
    if (!codec) {
        return IoResult::failure(
            QStringLiteral("不支持的文件类型 \"%1\"。当前可读取：%2")
                .arg(info.suffix(),
                     CodecRegistry::instance().supportedReadExtensions().join(QStringLiteral(", "))));
    }
    if (!codec->canRead()) {
        return IoResult::failure(codec->capabilityNote());
    }

    // 装载前先重置文档，避免上一次的内容与新文件混合
    document.clear(true);
    document.beginBatchLoad();

    IoResult result = codec->read(path, document);

    document.endBatchLoad();

    if (!result.ok) {
        // 失败时不留半成品：把文档恢复为空白
        document.clear(true);
        return result;
    }

    document.setFilePath(path);
    document.markClean();
    return result;
}

IoResult save(const QString& path, const Document& document, const WriteOptions& options)
{
    registerBuiltinCodecs();

    const QFileInfo info(path);
    if (info.exists() && !info.isWritable()) {
        return IoResult::failure(QStringLiteral("目标文件不可写（请检查权限）：%1").arg(path));
    }
    const QFileInfo dirInfo(info.absolutePath());
    if (!dirInfo.exists() || !dirInfo.isDir()) {
        return IoResult::failure(QStringLiteral("目标目录不存在：%1").arg(info.absolutePath()));
    }

    ICadCodec* codec = CodecRegistry::instance().codecForPath(path, true);
    if (!codec) {
        return IoResult::failure(
            QStringLiteral("不支持写出的文件类型 \"%1\"。当前可写出：%2")
                .arg(info.suffix(),
                     CodecRegistry::instance().supportedWriteExtensions().join(QStringLiteral(", "))));
    }
    if (!codec->canWrite()) {
        return IoResult::failure(codec->capabilityNote());
    }

    return codec->write(path, document, options);
}

bool canLoad(const QString& path)
{
    registerBuiltinCodecs();
    ICadCodec* codec = CodecRegistry::instance().codecForPath(path, false);
    return codec != nullptr && codec->canRead();
}

bool canSave(const QString& path)
{
    registerBuiltinCodecs();
    ICadCodec* codec = CodecRegistry::instance().codecForPath(path, true);
    return codec != nullptr && codec->canWrite();
}

QString capabilityNoteFor(const QString& path)
{
    registerBuiltinCodecs();
    ICadCodec* codec = CodecRegistry::instance().codecForPath(path, false);
    if (!codec) return QString();

    const QString note = codec->capabilityNote();
    return note.isEmpty() ? QString() : QStringLiteral("%1：%2").arg(codec->displayName(), note);
}

} // namespace CadFile

} // namespace lycad
