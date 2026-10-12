/******************************************************************************
**  libDXFrw - Library to read/write DXF files (ascii & binary)              **
**                                                                           **
**  与版本无关的位流写入层（dwgBitWriter / CRC16 / 节哨兵）。                **
**                                                                           **
**  本文件只提供二进制位级原语，不含任何版本相关逻辑；具体版本的写出           **
**  规则见同目录 dwgwriter15.cpp（AC1015 / R2000）。位编码对 R13~R2000         **
**  各版本一致，2007 及以后（AC1021+）改为按字节编码，需另写一层。            **
**                                                                           **
**  语义与读取端 dwgbuffer.cpp 的同名读取函数逐位对偶，字段顺序以读取端       **
**  解析代码（drw_header.cpp / drw_entities.cpp / drw_objects.cpp /           **
**  dwgreader.cpp）为准。      **
**                                                                           **
**  关键格式事实（实测 sample_2000.dwg 验证，勿凭记忆改动）：                 **
**    · 对象记录 = [MS size][BS oType][RL bitsize][数据+句柄流][补位][RS CRC]**
**      size 为 oType 起至句柄流结束（含补位）的字节数，不含 CRC；           **
**      CRC 种子 0xC0C1，覆盖范围从 MS 第一个字节起至 CRC 之前；             **
**      bitsize 为自 oType 首字节起至句柄流起点的位偏移。                    **
**    · T 字符串（R2000）：BS 长度（不含结尾 \0）+ 编码后字节。              **
**    · 节 CRC（header/classes）：种子 0xC0C1，覆盖 [数据区起点, CRC 之前)。**
**    · 文件头 CRC：种子 0xC0C1，覆盖 [0, CRC 位置)，小端存储。              **
******************************************************************************/
#ifndef DWGWRITER_H
#define DWGWRITER_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../drw_base.h"

class DRW_TextCodec;

/// DWG 位流写入器：在内存缓冲区上按 DWG 位编码规则追加数据。
class dwgBitWriter
{
public:
    dwgBitWriter() = default;

    /// 绝对位偏移（从缓冲区起点计）
    std::size_t bitPosition() const noexcept { return m_bitPos; }
    /// 当前字节位置（位对齐时等于字节数；否则向下取整）
    std::size_t bytePosition() const noexcept { return m_bitPos >> 3; }
    /// 当前位偏移（0~7）
    int bitOffset() const noexcept { return static_cast<int>(m_bitPos & 7); }

    /// 跳到指定位偏移（仅用于回填，只允许向前覆盖已写区域）
    void setBitPosition(std::size_t pos)
    {
        const std::size_t bytes = (pos >> 3) + ((pos & 7) ? 1u : 0u);
        if (buf.size() < bytes) buf.resize(bytes, 0);
        m_bitPos = pos;
    }

    // ---- 位级原语 -----------------------------------------------------------
    void writeB(bool bit) { writeBits(bit ? 1u : 0u, 1); }
    void writeBB(std::uint8_t two) { writeBits(two & 3u, 2); }
    void write3B(std::uint8_t three) { writeBits(three & 7u, 3); }

    /// RC：按位对齐写 1 字节
    void writeRC(std::uint8_t v) { writeBits(v, 8); }
    /// RS：小端 2 字节
    void writeRS(std::uint16_t v)
    {
        writeBits(v & 0xFFu, 8);
        writeBits((v >> 8) & 0xFFu, 8);
    }
    /// RL：小端 4 字节
    void writeRL(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            writeBits((v >> (8 * i)) & 0xFFu, 8);
    }
    /// RD：小端 8 字节 IEEE double
    void writeRD(double d)
    {
        std::uint8_t b[8];
        std::memcpy(b, &d, 8);
        for (int i = 0; i < 8; ++i)
            writeBits(b[i], 8);
    }

    /// BS：压缩 short
    void writeBS(dint16 v)
    {
        if (v == 0) { writeBB(2); }
        else if (v == 256) { writeBB(3); }
        else if (v > 0 && v < 256) { writeBB(1); writeRC(static_cast<std::uint8_t>(v)); }
        else { writeBB(0); writeRS(static_cast<std::uint16_t>(v)); }
    }
    /// BL：压缩 long
    void writeBL(dint32 v)
    {
        if (v == 0) { writeBB(2); }
        else if (v > 0 && v < 256) { writeBB(1); writeRC(static_cast<std::uint8_t>(v)); }
        else { writeBB(0); writeRL(static_cast<std::uint32_t>(v)); }
    }
    /// BLL：先 3 位「字节数」，再按**小端**写该长度的字节
    ///（dwgBuffer::getBitLongLong 之对偶，R2013+ 头部变量的 REQUIREDVERSIONS 用它）。
    void writeBLL(std::uint64_t v)
    {
        std::uint8_t n = 0;
        for (std::uint64_t t = v; t != 0; t >>= 8) ++n;
        write3B(n);
        for (std::uint8_t i = 0; i < n; ++i)
            writeRC(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
    /// BD：压缩 double
    void writeBD(double d)
    {
        if (d == 0.0) { writeBB(2); }
        else if (d == 1.0) { writeBB(1); }
        else { writeBB(0); writeRD(d); }
    }
    /// 3BD / 2RD / 2DD
    void write3BD(double x, double y, double z) { writeBD(x); writeBD(y); writeBD(z); }
    /// 2BD：两个完整 BD
    void write2BD(double x, double y) { writeBD(x); writeBD(y); }
    void write2RD(double x, double y) { writeRD(x); writeRD(y); }

    /// DD：带默认值的压缩 double（与读取端 dwgBuffer::getDefaultDouble 对偶）。
    ///
    /// 分支语义由**读取端**决定（它要同时兼容 AutoCAD 写出的真实文件，是基准）：
    ///   b=0：值与默认值相同，不写数据；
    ///   b=1：只写**低 4 字节**，高 4 字节取默认值；
    ///   b=2：只写 6 个字节（顺序 4,5,0,1,2,3），字节 6、7 取默认值；
    ///   b=3：完整 RD。
    /// 注意 b=1 传的是低 4 字节——不是「哪段不同传哪段」。此前实现按
    /// 「低 4 字节相同 → 传高 4 字节」处理，与读取端正好相反，导致几乎所有
    /// LINE / LWPOLYLINE 的坐标在读回时变成 1e-5 量级的垃圾值（实测几何全部
    /// 退化、若干线段因零长度被当作坏数据丢弃）。
    void writeDD(double value, double def)
    {
        std::uint8_t v[8], d[8];
        std::memcpy(v, &value, 8);
        std::memcpy(d, &def, 8);
        if (std::memcmp(v, d, 8) == 0) { writeBB(0); return; }
        if (std::memcmp(v + 4, d + 4, 4) == 0) {   // b=1：高 4 字节相同 → 传低 4 字节
            writeBB(1);
            for (int i = 0; i < 4; ++i) writeBits(v[i], 8);
            return;
        }
        if (std::memcmp(v + 6, d + 6, 2) == 0) {   // b=2：字节 6、7 相同 → 写 4,5,0,1,2,3
            writeBB(2);
            static const int order[6] = {4, 5, 0, 1, 2, 3};
            for (int i = 0; i < 6; ++i) writeBits(v[order[i]], 8);
            return;
        }
        writeBB(3);                                // b=3：完整 RD
        writeRD(value);
    }

    /// MC：带符号模长字符（7 位分组，末组 0x40 位为符号）
    void writeMC(dint32 v)
    {
        const bool neg = v < 0;
        std::uint32_t u = neg ? static_cast<std::uint32_t>(-(v + 1)) + 1u
                              : static_cast<std::uint32_t>(v);
        std::uint8_t g[6];
        int n = 0;
        do { g[n++] = static_cast<std::uint8_t>(u & 0x7Fu); u >>= 7; } while (u && n < 6);
        if (g[n - 1] & 0x40 && n < 6) g[n++] = 0;   // 末组 0x40 留给符号
        for (int i = 0; i < n; ++i) {
            std::uint8_t b = g[i];
            if (i < n - 1) b |= 0x80;
            else if (neg) b |= 0x40;
            writeRC(b);
        }
    }
    /// UMC：无符号模长字符
    void writeUMC(std::uint32_t u)
    {
        std::uint8_t g[6];
        int n = 0;
        do { g[n++] = static_cast<std::uint8_t>(u & 0x7Fu); u >>= 7; } while (u && n < 6);
        for (int i = 0; i < n; ++i)
            writeRC(i < n - 1 ? (g[i] | 0x80) : g[i]);
    }
    /// MS：模长短（15 位分组）
    void writeMS(std::uint32_t v)
    {
        if (v > 0x7FFFu) {
            writeRS(static_cast<std::uint16_t>((v & 0x7FFFu) | 0x8000u));
            writeRS(static_cast<std::uint16_t>(v >> 15));
        } else {
            writeRS(static_cast<std::uint16_t>(v));
        }
    }

    /// H：句柄。ref==0 时写 0 字节句柄（code 5）。
    void writeHandle(std::uint8_t code, std::uint32_t ref)
    {
        if (m_hdlSink != nullptr && m_mainHandle == 0) {
            m_hdlSink->writeHandle(code, ref);
            return;
        }
        if (m_mainHandle != 0) --m_mainHandle;
        std::uint8_t size = 0;
        if (ref != 0) {
            std::uint32_t t = ref;
            while (t) { ++size; t >>= 8; }
        }
        writeRC(static_cast<std::uint8_t>((code << 4) | size));
        for (int i = size - 1; i >= 0; --i)
            writeRC(static_cast<std::uint8_t>((ref >> (8 * i)) & 0xFFu));
    }

    /// 明确写进**数据流**的句柄（绕过句柄分流）。
    /// 2007+ 的 header 变量流里只有 HANDSEED 一个句柄留在数据流
    ///（读取端 drw_header.cpp 用 `buf->getHandle()` 读它），其余句柄全在独立句柄流。
    void writeMainHandle(std::uint8_t code, std::uint32_t ref)
    {
        // 只在这里 ++ ：writeHandle 走进主流分支时会自减，若此处再 -- 一次，
        // 计数会变成负数，之后 `m_mainHandle == 0` 永远不成立 —— 句柄流被整体
        // 旁路，全部句柄写进数据流（症状：句柄流为空、控制对象全部 not found）。
        ++m_mainHandle;
        writeHandle(code, ref);
    }

    /// 定长句柄：尾部字节数固定为 sizeBytes（高位补零）。
    /// 用于需要"等长回填"的字段（如实体 prev/next 链），装配期按位替换数值。
    void writeHandleFixed(std::uint8_t code, std::uint32_t ref, int sizeBytes)
    {
        if (m_hdlSink != nullptr) {
            m_hdlSink->writeHandleFixed(code, ref, sizeBytes);
            return;
        }
        writeRC(static_cast<std::uint8_t>((code << 4) | (sizeBytes & 0x0F)));
        for (int i = sizeBytes - 1; i >= 0; --i)
            writeRC(static_cast<std::uint8_t>((ref >> (8 * i)) & 0xFFu));
    }

    /// TV/T：R2000 文本 = BS 长度（不含结尾 \0）+ 按文件代码页编码的字节
    ///
    /// 2007+（AC1021 及以上）文本改为**独立字符串流**：读取端
    ///（drw_entities.cpp / drw_objects.cpp 的 `version > AC1018` 分支）用
    /// objSize-1 位上的标志把字符串缓冲区定位到对象末尾的字符串区，再按
    /// `getVariableText`（= `getUCSText`：BS 字符数 + UTF-16LE）逐个读。
    /// 因此 2007+ 下本函数把内容写进 setStringSink() 指定的字符串流
    ///（顺序即读取端的读取顺序），由装配方追加到数据流之后。
    void writeT(const std::string& utf8, DRW_TextCodec& codec);

    /// TU：BS 字符数 + UTF-16LE 码元（读取端 dwgBuffer::getUCSText 的对偶）。
    /// 长度为**字符数**（UTF-16 码元数），不是字节数——写成字节数会让读取端
    /// 按 2 倍长度去读，紧随其后的所有字段整体错位。
    void writeUCS(const std::string& utf8);

    /// 2007+ 的字符串流：writeT 依据 codec 版本自动分流到这里（懒创建）。
    /// 装配方用 stringBits() / stringStream() 把它追加到数据流之后。
    dwgBitWriter* ensureStringSink()
    {
        if (!m_ownedSink) m_ownedSink.reset(new dwgBitWriter());
        return m_ownedSink.get();
    }
    const dwgBitWriter* stringStream() const { return m_ownedSink.get(); }
    std::size_t stringBits() const { return m_ownedSink ? m_ownedSink->bitPosition() : 0; }

    /// 2007+ 的句柄分流目标：非空时 writeHandle / writeHandleFixed 写入该流。
    /// header 变量流在 2007+ 走「独立句柄流」，起点由 RL bitSize 给出
    ///（读取端 drw_header.cpp 把 hBbuf 定位到节内 160+bitSize 位）。
    void setHandleSink(dwgBitWriter* sink) { m_hdlSink = sink; }
    dwgBitWriter* handleSink() const { return m_hdlSink; }

    /// 按位追加另一个写入器已写好的内容（字符串流并入主流时使用）。
    void appendBitsFrom(const dwgBitWriter& other)
    {
        for (std::size_t i = 0; i < other.m_bitPos; ++i)
            writeB(other.bitAt(i));
    }

    /// 位区间追加（把已写好的句柄流搬到字符串区之后用）。
    void appendBitsRange(const dwgBitWriter& other, std::size_t from, std::size_t to)
    {
        for (std::size_t i = from; i < to; ++i)
            writeB(other.bitAt(i));
    }

    /// 读取指定位（越界返回 false）。
    bool bitAt(std::size_t pos) const
    {
        const std::size_t byteIdx = pos >> 3;
        if (byteIdx >= buf.size()) return false;
        return (buf[byteIdx] & (0x80 >> (pos & 7))) != 0;
    }

    /// 补 0 到字节边界
    void padToByte() { while (m_bitPos & 7) writeB(false); }

    /// 取已写内容（不改变游标）。调用前应 padToByte()。
    const std::vector<std::uint8_t>& data() const noexcept { return buf; }
    std::vector<std::uint8_t> takeData() noexcept { return std::move(buf); }

    /// CRC16（种子 dx，覆盖 [start, end) 字节范围，按当前缓冲区内容计算）
    std::uint16_t crc16(std::uint16_t dx, std::size_t start, std::size_t end) const;

private:
    void writeBits(std::uint32_t value, int nbits)
    {
        for (int i = nbits - 1; i >= 0; --i) {
            const std::size_t byteIdx = m_bitPos >> 3;
            if (buf.size() <= byteIdx) buf.resize(byteIdx + 1, 0);
            const std::uint8_t mask = static_cast<std::uint8_t>(0x80 >> (m_bitPos & 7));
            // 逐位**改写**（置位与清位都做）。只置位会让「回退覆盖」写失败：
            // 装配 2007+ 的字符串区时要先把游标退回数据区末尾重写，
            // 若旧位不被清除，新值与旧值按位或在一起（实测：写 32 读回 41632）。
            if ((value >> i) & 1u)
                buf[byteIdx] |= mask;
            else
                buf[byteIdx] = static_cast<std::uint8_t>(buf[byteIdx] & ~mask);
            ++m_bitPos;
        }
    }

    /// 2007+ 的字符串流（懒创建；writeT 按 codec 版本分流）
    std::unique_ptr<dwgBitWriter> m_ownedSink;
    /// 2007+ 的句柄分流目标（非空时 writeHandle/writeHandleFixed 都写它）
    dwgBitWriter* m_hdlSink = nullptr;
    /// >0 时把接下来的 N 次 writeHandle 强制写进主流（writeMainHandle 用）
    int m_mainHandle = 0;

private:
    std::vector<std::uint8_t> buf;
    std::size_t m_bitPos = 0;
};

/// CRC16 表
std::uint16_t dwgCalcCrc16(std::uint16_t seed, const std::uint8_t* data, std::size_t len);

/// 节哨兵（16 字节）。kind 取值见 dwgwriter.cpp。
void dwgSentinelBytes(int kind, bool begin, std::uint8_t out[16]);

#endif // DWGWRITER_H
