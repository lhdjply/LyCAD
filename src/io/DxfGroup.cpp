// =============================================================================
//  LyCAD - io/DxfGroup.cpp
// =============================================================================
#include "io/DxfGroup.h"

#include "core/Format.h"

#include <QIODevice>

#include <cstring>

namespace lycad {

namespace {

/// 二进制 DXF 的文件头哨兵（22 字节）
constexpr char kBinarySentinel[] = "AutoCAD Binary DXF\r\n\x1a\x00";
constexpr int  kBinarySentinelLength = 22;

constexpr int kBufferFlushThreshold = 1 << 16;   // 64 KiB

} // namespace

// -----------------------------------------------------------------------------
//  组码语义
// -----------------------------------------------------------------------------
DxfValueKind dxfValueKind(int code)
{
    if (code >= 0 && code <= 9) return DxfValueKind::String;
    if (code >= 10 && code <= 59) return DxfValueKind::Real;
    if (code >= 60 && code <= 79) return DxfValueKind::Integer;
    if (code >= 90 && code <= 99) return DxfValueKind::Integer;
    if (code == 100 || code == 102 || code == 105) return DxfValueKind::String;
    if (code >= 110 && code <= 149) return DxfValueKind::Real;
    if (code >= 160 && code <= 169) return DxfValueKind::Int64;
    if (code >= 170 && code <= 179) return DxfValueKind::Integer;
    if (code >= 210 && code <= 239) return DxfValueKind::Real;
    if (code >= 270 && code <= 289) return DxfValueKind::Integer;
    if (code >= 290 && code <= 299) return DxfValueKind::Boolean;
    if (code >= 300 && code <= 369) return DxfValueKind::String;
    if (code >= 370 && code <= 389) return DxfValueKind::Integer;
    if (code >= 390 && code <= 399) return DxfValueKind::String;
    if (code >= 400 && code <= 409) return DxfValueKind::Integer;
    if (code >= 410 && code <= 419) return DxfValueKind::String;
    if (code >= 420 && code <= 429) return DxfValueKind::Integer;
    if (code >= 430 && code <= 439) return DxfValueKind::String;
    if (code >= 440 && code <= 449) return DxfValueKind::Integer;
    if (code >= 450 && code <= 459) return DxfValueKind::Integer;
    if (code >= 460 && code <= 469) return DxfValueKind::Real;
    if (code >= 470 && code <= 481) return DxfValueKind::String;
    if (code == 999) return DxfValueKind::String;
    if (code >= 1000 && code <= 1009) return DxfValueKind::String;
    if (code >= 1010 && code <= 1059) return DxfValueKind::Real;
    if (code >= 1060 && code <= 1070) return DxfValueKind::Integer;
    if (code == 1071) return DxfValueKind::Integer;

    // 未知组码按字符串处理，保证读取过程不中断
    return DxfValueKind::String;
}

int dxfIntegerWidth(int code)
{
    if (code >= 60 && code <= 79) return 2;
    if (code >= 90 && code <= 99) return 4;
    if (code >= 160 && code <= 169) return 8;
    if (code >= 170 && code <= 179) return 2;
    if (code >= 270 && code <= 289) return 2;
    if (code >= 290 && code <= 299) return 1;
    if (code >= 370 && code <= 389) return 2;
    if (code >= 400 && code <= 409) return 2;
    if (code >= 420 && code <= 429) return 4;
    if (code >= 440 && code <= 449) return 4;
    if (code >= 450 && code <= 459) return 4;
    if (code >= 1060 && code <= 1070) return 2;
    if (code == 1071) return 4;
    return 0;
}

// -----------------------------------------------------------------------------
//  字符串转义
// -----------------------------------------------------------------------------
QString encodeDxfString(const QString& text)
{
    bool needsEscape = false;
    for (const QChar ch : text) {
        if (ch.unicode() > 0x7E || ch.unicode() < 0x20) {
            needsEscape = true;
            break;
        }
    }
    if (!needsEscape) return text;

    QString out;
    out.reserve(text.size() + 16);
    for (const QChar ch : text) {
        const ushort code = ch.unicode();
        if (code >= 0x20 && code <= 0x7E) {
            out.append(ch);
            continue;
        }
        // QChar 恒为 16 位，因此单个 \U+XXXX 足以表达任意一个 QChar。
        // 增补平面字符在 QString 中以 UTF-16 代理对存储，会被逐个转义为
        // 两个 \U+ 序列——这正是 AutoCAD 自身的写法，读取端可按同样规则重组。
        out.append(QStringLiteral("\\U+%1").arg(code, 4, 16, QLatin1Char('0')).toUpper());
    }
    return out;
}

QString decodeDxfString(const QString& text)
{
    if (!text.contains(QLatin1String("\\U+"))) return text;

    QString out;
    out.reserve(text.size());

    int i = 0;
    const int n = text.size();
    while (i < n) {
        if (text.at(i) == QLatin1Char('\\')
            && i + 6 < n
            && text.at(i + 1) == QLatin1Char('U')
            && text.at(i + 2) == QLatin1Char('+')) {
            bool ok = false;
            const ushort code = text.mid(i + 3, 4).toUShort(&ok, 16);
            if (ok) {
                out.append(QChar(code));
                i += 7;
                continue;
            }
        }
        out.append(text.at(i));
        ++i;
    }
    return out;
}

// -----------------------------------------------------------------------------
//  DxfGroupReader
// -----------------------------------------------------------------------------
DxfGroupReader::DxfGroupReader(QIODevice* device)
    : m_device(device)
{
}

bool DxfGroupReader::open(QString* error)
{
    if (!m_device || !m_device->isReadable()) {
        if (error) *error = QStringLiteral("设备不可读。");
        return false;
    }

    QByteArray header = m_device->read(kBinarySentinelLength);
    if (header.size() >= kBinarySentinelLength
        && std::memcmp(header.constData(), kBinarySentinel, kBinarySentinelLength) == 0) {
        m_binary = true;
    } else {
        m_binary = false;

        // 非二进制情形：把探测用的字节塞回行缓冲，交由 ASCII 解析处理
        m_lineBuffer = header;
        if (m_lineBuffer.endsWith('\n')) {
            // 探测恰好切在行尾，保持原样即可
        }
    }

    m_opened = true;
    return true;
}

bool DxfGroupReader::readLine(QByteArray& out)
{
    while (true) {
        const int idx = m_lineBuffer.indexOf('\n');
        if (idx >= 0) {
            out = m_lineBuffer.left(idx);
            m_lineBuffer.remove(0, idx + 1);
            if (out.endsWith('\r')) out.chop(1);
            return true;
        }

        if (!m_device->isOpen() || m_device->atEnd()) {
            if (m_lineBuffer.isEmpty()) return false;
            out = m_lineBuffer;
            m_lineBuffer.clear();
            if (out.endsWith('\r')) out.chop(1);
            return true;
        }

        const QByteArray chunk = m_device->read(1 << 16);
        if (chunk.isEmpty()) {
            if (m_lineBuffer.isEmpty()) return false;
            out = m_lineBuffer;
            m_lineBuffer.clear();
            return true;
        }
        m_lineBuffer.append(chunk);
    }
}

bool DxfGroupReader::nextAscii(DxfGroup& group, QString* error)
{
    QByteArray codeLine;
    if (!readLine(codeLine)) return false;

    // 跳过空行：部分工具会在文件末尾留下空行
    while (codeLine.trimmed().isEmpty()) {
        ++m_line;
        if (!readLine(codeLine)) return false;
    }

    bool ok = false;
    const int code = codeLine.trimmed().toInt(&ok);
    if (!ok) {
        if (error) {
            *error = QStringLiteral("第 %1 行：组码 \"%2\" 无法解析为整数。")
                         .arg(m_line)
                         .arg(QString::fromLatin1(codeLine.trimmed()));
        }
        return false;
    }

    QByteArray valueLine;
    if (!readLine(valueLine)) {
        if (error) {
            *error = QStringLiteral("第 %1 行：组码 %2 缺少对应的值行，文件可能被截断。")
                         .arg(m_line).arg(code);
        }
        return false;
    }

    m_line += 2;
    ++m_groupIndex;

    group = DxfGroup{};
    group.code = code;
    group.valid = true;

    // 值行不做过度的空白裁剪：图层名、文字内容中的首尾空格有语义。
    // 但制表符与孤立的行尾空格在多数写出器中只是填充，这里仅去除行尾空白。
    QByteArray trimmed = valueLine;
    while (trimmed.endsWith(' ') || trimmed.endsWith('\t')) trimmed.chop(1);

    switch (dxfValueKind(code)) {
    case DxfValueKind::Real:
        group.real = QString::fromLatin1(trimmed).trimmed().toDouble();
        group.text = QString::fromLatin1(trimmed).trimmed();
        break;
    case DxfValueKind::Integer:
    case DxfValueKind::Int64:
        group.integer = QString::fromLatin1(trimmed).trimmed().toLongLong();
        group.text = QString::fromLatin1(trimmed).trimmed();
        break;
    case DxfValueKind::Boolean:
        group.integer = (QString::fromLatin1(trimmed).trimmed().toInt() != 0) ? 1 : 0;
        group.text = QString::fromLatin1(trimmed).trimmed();
        break;
    case DxfValueKind::String:
        // DXF 的字符串按代码页编码。多数现代写出器输出 UTF-8/ASCII，
        // 这里先按 UTF-8 解码，失败再退回 Latin-1，随后处理 \U+XXXX 转义。
        {
            QString decoded = QString::fromUtf8(trimmed);
            if (decoded.contains(QChar(0xFFFD))) {
                decoded = QString::fromLatin1(trimmed);
            }
            group.text = decodeDxfString(decoded);
        }
        break;
    }

    ++m_line;
    return true;
}

bool DxfGroupReader::nextBinary(DxfGroup& group, QString* error)
{
    auto readExact = [&](int count, QByteArray& out) -> bool {
        out.clear();
        while (out.size() < count) {
            const QByteArray chunk = m_device->read(count - out.size());
            if (chunk.isEmpty()) return false;
            out.append(chunk);
        }
        return true;
    };

    QByteArray codeByte;
    if (!readExact(1, codeByte)) return false;

    const int code = static_cast<unsigned char>(codeByte.at(0));
    ++m_groupIndex;

    group = DxfGroup{};
    group.code = code;
    group.valid = true;

    const DxfValueKind kind = dxfValueKind(code);

    if (kind == DxfValueKind::String) {
        // NUL 结尾
        QByteArray raw;
        char c = 0;
        while (true) {
            QByteArray one;
            if (!readExact(1, one)) {
                if (error) *error = QStringLiteral("二进制 DXF 字符串未以 NUL 结尾，文件可能损坏。");
                return false;
            }
            c = one.at(0);
            if (c == '\0') break;
            raw.append(c);
        }
        QString decoded = QString::fromUtf8(raw);
        if (decoded.contains(QChar(0xFFFD))) decoded = QString::fromLatin1(raw);
        group.text = decodeDxfString(decoded);
        return true;
    }

    if (kind == DxfValueKind::Real) {
        QByteArray raw;
        if (!readExact(8, raw)) return false;
        double value = 0.0;
        std::memcpy(&value, raw.constData(), 8);   // DXF 二进制为小端，与主流平台一致
        group.real = value;
        return true;
    }

    const int width = dxfIntegerWidth(code);
    if (width == 0) {
        // 语义未知的整数组码：按 4 字节读取，避免流错位
        QByteArray raw;
        if (!readExact(4, raw)) return false;
        qint32 value = 0;
        std::memcpy(&value, raw.constData(), 4);
        group.integer = value;
        return true;
    }

    QByteArray raw;
    if (!readExact(width, raw)) return false;

    switch (width) {
    case 1:
        group.integer = static_cast<unsigned char>(raw.at(0));
        break;
    case 2: {
        qint16 value = 0;
        std::memcpy(&value, raw.constData(), 2);
        group.integer = value;
        break;
    }
    case 8: {
        qint64 value = 0;
        std::memcpy(&value, raw.constData(), 8);
        group.integer = value;
        break;
    }
    default: {
        qint32 value = 0;
        std::memcpy(&value, raw.constData(), 4);
        group.integer = value;
        break;
    }
    }
    return true;
}

bool DxfGroupReader::next(DxfGroup& group, QString* error)
{
    if (m_hasPushedBack) {
        group = m_pushedBack;
        m_hasPushedBack = false;
        return true;
    }
    if (!m_opened) {
        if (error) *error = QStringLiteral("读取器尚未初始化。");
        return false;
    }
    return m_binary ? nextBinary(group, error) : nextAscii(group, error);
}

void DxfGroupReader::pushBack(const DxfGroup& group)
{
    m_pushedBack = group;
    m_hasPushedBack = true;
}

// -----------------------------------------------------------------------------
//  DxfGroupWriter
// -----------------------------------------------------------------------------
DxfGroupWriter::DxfGroupWriter(QIODevice* device)
    : m_device(device)
{
}

DxfGroupWriter::~DxfGroupWriter()
{
    flush();
}

void DxfGroupWriter::appendLine(const QByteArray& line)
{
    m_buffer.append(line);
    m_buffer.append('\n');
    if (m_buffer.size() >= kBufferFlushThreshold) flush();
}

bool DxfGroupWriter::flush()
{
    if (!m_device) return false;
    if (m_buffer.isEmpty()) return true;

    const qint64 written = m_device->write(m_buffer);
    if (written != m_buffer.size()) return false;

    m_bytesWritten += written;
    m_buffer.clear();
    return true;
}

void DxfGroupWriter::write(int code, const QString& value)
{
    appendLine(QByteArray::number(code));
    appendLine(encodeDxfString(value).toUtf8());
    ++m_groupCount;
}

void DxfGroupWriter::write(int code, int value)
{
    appendLine(QByteArray::number(code));
    appendLine(QByteArray::number(value));
    ++m_groupCount;
}

void DxfGroupWriter::write(int code, qint64 value)
{
    appendLine(QByteArray::number(code));
    appendLine(QByteArray::number(value));
    ++m_groupCount;
}

void DxfGroupWriter::write(int code, double value)
{
    appendLine(QByteArray::number(code));
    appendLine(formatDxfDouble(value).toLatin1());
    ++m_groupCount;
}

void DxfGroupWriter::write(int code, bool value)
{
    write(code, value ? 1 : 0);
}

void DxfGroupWriter::writePoint(int xCode, const Point2D& point)
{
    write(xCode, point.x);
    write(xCode + 10, point.y);
    write(xCode + 20, 0.0);
}

void DxfGroupWriter::writeComment(const QString& text)
{
    write(999, text);
}

} // namespace lycad
