// =============================================================================
//  LyCAD - io/DxfGroup.h
//  DXF 组码（group code）读写底层。
//
//  DXF 文件本质是一串「组码 / 值」对。ASCII 形式每对占两行；二进制形式则是
//  1 字节组码 + 按组码区间决定宽度的紧凑载荷，文件头带 22 字节哨兵。
//
//  本模块只负责「把字节流翻译成组码序列」与反向操作，不理解任何实体语义，
//  因此可被 DXF 读写器之外的场景（如文件头探测、内容统计）复用。
// =============================================================================
#pragma once

#include "core/Types.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <vector>

class QIODevice;

namespace lycad {

// -----------------------------------------------------------------------------
//  组码值类型
// -----------------------------------------------------------------------------
enum class DxfValueKind {
    String,    ///< 字符串（组码 0-9, 100, 102, 105, 300-369, 390-399, ...）
    Real,      ///< 双精度浮点（组码 10-59, 110-149, 210-239, 460-469, 1010-1059）
    Integer,   ///< 16/32 位整数（组码 60-79, 90-99, 170-179, 270-289, ...）
    Int64,     ///< 64 位整数（组码 160-169）
    Boolean    ///< 布尔（组码 290-299）
};

/// 组码对应的值类型
DxfValueKind dxfValueKind(int code);

/// 整数组码的位宽（1/2/4/8 字节），非整数组码返回 0。二进制解析用。
int dxfIntegerWidth(int code);

// -----------------------------------------------------------------------------
//  字符串转义
//
//  R12/R2000 的字符串按代码页编码，直接写 UTF-8 会导致中文注释与文字乱码。
//  DXF 为此定义了 \U+XXXX 转义。这里统一把非 ASCII 字符转为该转义形式，
//  使文件在任意代码页下都能被正确还原（AutoCAD、ezdxf、LibreCAD 均支持）。
// -----------------------------------------------------------------------------
QString encodeDxfString(const QString& text);
QString decodeDxfString(const QString& text);

// -----------------------------------------------------------------------------
//  单个组
// -----------------------------------------------------------------------------
struct DxfGroup {
    int     code = 0;
    QString text;            ///< String 类组码的原始内容
    double  real = 0.0;      ///< Real 类组码
    qint64  integer = 0;     ///< Integer / Int64 / Boolean 类组码
    bool    valid = false;   ///< 是否为有效读入的组

    int  asInt() const { return static_cast<int>(integer); }
    qint64 asInt64() const { return integer; }
    double asDouble() const { return real; }
    bool asBool() const { return integer != 0; }
};

// -----------------------------------------------------------------------------
//  DxfGroupReader
// -----------------------------------------------------------------------------
class DxfGroupReader {
public:
    /// device 需已打开且可读。生命周期由调用方保证。
    explicit DxfGroupReader(QIODevice* device);

    /// 读取文件头，判定 ASCII / 二进制并做基本校验
    bool open(QString* error = nullptr);

    bool isBinary() const noexcept { return m_binary; }

    /// 读取下一组。返回 false 表示已到流末尾或发生错误。
    bool next(DxfGroup& group, QString* error = nullptr);

    /// 回退一组（仅支持回退一组，供状态机做前瞻判断）
    void pushBack(const DxfGroup& group);

    /// 已读取的组序号（从 1 开始），用于错误定位
    int groupIndex() const noexcept { return m_groupIndex; }

    /// 当前行号（仅 ASCII 模式有意义）
    quint64 lineNumber() const noexcept { return m_line; }

private:
    bool nextAscii(DxfGroup& group, QString* error);
    bool nextBinary(DxfGroup& group, QString* error);
    bool readLine(QByteArray& out);

    QIODevice* m_device = nullptr;
    bool m_binary = false;
    bool m_opened = false;

    bool     m_hasPushedBack = false;
    DxfGroup m_pushedBack;

    int     m_groupIndex = 0;
    quint64 m_line = 1;

    // ASCII 逐行读取缓冲
    QByteArray m_lineBuffer;
};

// -----------------------------------------------------------------------------
//  DxfGroupWriter
// -----------------------------------------------------------------------------
class DxfGroupWriter {
public:
    explicit DxfGroupWriter(QIODevice* device);
    ~DxfGroupWriter();

    void write(int code, const QString& value);
    void write(int code, int value);
    void write(int code, qint64 value);
    void write(int code, double value);
    void write(int code, bool value);

    /// 写入一个坐标点：主码、主码+10、主码+20（即 10/20/30 形式）
    void writePoint(int xCode, const Point2D& point);

    /// 999 注释行
    void writeComment(const QString& text);

    /// 写入一个只含组码与名称的表头片段（如 0/SECTION, 2/HEADER）
    void writePair(int code, const QString& value) { write(code, value); }

    /// 将内部缓冲刷出到设备。析构时自动调用。
    bool flush();

    qint64 bytesWritten() const noexcept { return m_bytesWritten; }
    int groupCount() const noexcept { return m_groupCount; }

private:
    void appendLine(const QByteArray& line);

    QIODevice* m_device = nullptr;
    QByteArray m_buffer;
    qint64 m_bytesWritten = 0;
    int m_groupCount = 0;
    bool m_flushed = false;
};

} // namespace lycad
