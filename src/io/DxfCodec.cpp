// =============================================================================
//  LyCAD - io/DxfCodec.cpp
// =============================================================================
#include "io/DxfCodec.h"

#include "io/DxfReader.h"
#include "io/DxfWriter.h"

#include <QFile>
#include <QFileInfo>

namespace lycad {

QString DxfCodec::name() const
{
    return QStringLiteral("DXF");
}

QString DxfCodec::displayName() const
{
    return QStringLiteral("DXF 图形交换文件");
}

QStringList DxfCodec::readExtensions() const
{
    return { QStringLiteral("dxf") };
}

QStringList DxfCodec::writeExtensions() const
{
    return { QStringLiteral("dxf") };
}

bool DxfCodec::probe(QIODevice& device) const
{
    if (!device.isOpen() && !device.open(QIODevice::ReadOnly)) return false;

    const qint64 originalPos = device.pos();
    const QByteArray head = device.read(1024);
    if (device.isSequential() == false) device.seek(originalPos);

    if (head.isEmpty()) return false;

    // 二进制 DXF 有固定的 22 字节哨兵
    static const QByteArray kBinarySentinel("AutoCAD Binary DXF\r\n\x1a\x00");
    if (head.startsWith(kBinarySentinel)) return true;

    // ASCII DXF：首个组码应为 0，紧随其后的值多为 SECTION（也可能先出现 999 注释）
    const QList<QByteArray> lines = head.split('\n');
    if (lines.size() < 2) return false;

    bool codeOk = false;
    const int firstCode = lines.at(0).trimmed().toInt(&codeOk);
    if (!codeOk || firstCode < 0 || firstCode > 1071) return false;

    const QByteArray firstValue = lines.at(1).trimmed().toUpper();
    if (firstValue == "SECTION") return true;

    // 兼容以注释开头的文件
    return head.contains("SECTION") || head.contains("ENTITIES");
}

IoResult DxfCodec::read(const QString& path, Document& document) const
{
    DxfReader reader;
    return reader.read(path, document);
}

IoResult DxfCodec::write(const QString& path, const Document& document,
                         const WriteOptions& options) const
{
    WriteOptions effective = options;

    // 版本归一化：仅接受本写出器实现的两个目标版本
    const QStringList supported = WriteOptions::availableDxfVersions();
    if (!supported.contains(effective.version, Qt::CaseInsensitive)) {
        effective.version = supported.front();
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return IoResult::failure(QStringLiteral("无法写入文件：%1").arg(file.errorString()));
    }

    DxfWriter writer(&file, effective);
    IoResult result = writer.write(document);

    if (!file.flush()) {
        return IoResult::failure(QStringLiteral("刷新文件缓冲失败：%1").arg(file.errorString()));
    }
    file.close();

    return result;
}

} // namespace lycad
