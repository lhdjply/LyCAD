// =============================================================================
//  LyCAD - io/DxfCodec.cpp
// =============================================================================
#include "io/DxfCodec.h"

#include "io/LibDxfRwAdapter.h"

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
    // 读取走内置 libdxfrw（与 DWG 同一套解析器、同一个适配层）。
    // ASCII 与二进制 DXF 都由它自动识别；此前由自研 DxfReader 承担，
    // 现已删除——交叉校验的基准改为 ezdxf（见 tools/ 下的校验脚本）。
    return readDxfWithLibDxfRw(path, document);
}

IoResult DxfCodec::write(const QString& path, const Document& document,
                         const WriteOptions& options) const
{
    WriteOptions effective = options;

    // 版本归一化：只接受 WriteOptions::availableDxfVersions() 里的目标版本，
    // 其余（含空串）一律落到清单首项 R12，绝不写出版本不明的文件
    const QStringList supported = WriteOptions::availableDxfVersions();
    if (!supported.contains(effective.version, Qt::CaseInsensitive)) {
        effective.version = supported.front();
    }

    return writeDxfWithLibDxfRw(path, document, effective);
}

} // namespace lycad
