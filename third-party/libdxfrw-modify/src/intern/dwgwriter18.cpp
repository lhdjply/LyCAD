/******************************************************************************
**  libDXFrw - Library to read/write DXF files (ascii & binary)              **
**                                                                           **
**  Copyright (C) 2011-2015 José F. Soriano, rallazz@gmail.com               **
**                                                                           **
**  This library is free software, licensed under the terms of the GNU       **
**  General Public License as published by the Free Software Foundation,     **
**  either version 2 of the License, or (at your option) any later version.  **
**  You should have received a copy of the GNU General Public License        **
**  along with this program.  If not, see <http://www.gnu.org/licenses/>.    **
******************************************************************************/
//!
//!  dwgwriter18.cpp —— DWG **R2004 (AC1018)** 写出实现。
//!
//!  文件布局
//!    0x0000  0x100 字节文件头（版本串、预览/摘要地址…）
//!      0x80  0x6C 字节内层头（CRC-32 后与幻数序列异或）
//!      0xEC  0x14 字节幻数序列尾部
//!    0x0100  数据页（每页：32 字节 XOR 掩码页头 + LZ77 数据 + 0x20 对齐补长）
//!            → 段映射页（0x4163003b）→ 页映射页（0x41630e3b）
//!
//!  页地址由页映射页累计得出（读取端从 0x100 起按页尺寸累加），
//!  因此各页必须首尾相接、无间隙；页尺寸取 0x20 的倍数保证地址对齐。
//!
#include "dwgwriter18.h"

#include "dwgwriter.h"          // dwgBitWriter / dwgCalcCrc16 / dwgSentinelBytes
#include "../libdwgrw.h"        // dwgW / dwgW::SectionData / dwgW::buildSections

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

// -----------------------------------------------------------------------------
//  产品信息里的版本号
//
//  AcDb:AppInfo 段记录的是「谁写的、什么版本」，版本号不该在这里写字面量：
//  构建系统把本项目的版本以 LYCAD_VERSION 宏注入（唯一来源是仓库根的 VERSION
//  文件，见根 CMakeLists.txt）。libdxfrw 若被单独拿去编译则没有该宏，退回占位值，
//  以此保证本文件仍能独立编译。
//
//  宏的值必须是**字符串字面量**：下面按 C++ 的相邻字面量拼接使用。
// -----------------------------------------------------------------------------
#ifndef LYCAD_VERSION
#define LYCAD_VERSION "0.0.0"
#endif

namespace {

// -----------------------------------------------------------------------------
//  局部常量
// -----------------------------------------------------------------------------
constexpr std::size_t  kFileHeaderSize  = 0x100;    // 文件头预留
constexpr std::size_t  kInnerHeaderSize = 0x6C;     // 内层头长度
constexpr std::size_t  kDataPageHeader  = 32;       // 数据页页头长度
constexpr std::size_t  kSysPageHeader   = 20;       // 系统页页头长度
constexpr std::size_t  kMaxPageSize     = 0x7400;   // 单页最大解压尺寸
constexpr std::size_t  kAlign           = 0x20;     // 页对齐粒度
constexpr std::uint32_t kPageTypeData   = 0x4163043b;
constexpr std::uint32_t kPageTypeSecMap = 0x4163003b;
constexpr std::uint32_t kPageTypePageMap= 0x41630e3b;
constexpr std::uint32_t kMaskBase       = 0x4164536b;

/// AutoCAD 规范段号中的 Preview 段（用于在分段装配前为预览容器定址）
constexpr std::uint32_t kPreviewSectionId = 4;

/// 哨兵种类（dwgSentinelBytes 的 kind 参数，取值与 dwgwriter.cpp 一致）
constexpr int kSentThumbnailBegin = 1;
constexpr int kSentVariableBegin  = 3;
constexpr int kSentClassBegin     = 5;

/// R2004+ 段尾固定 8 字节 0 填充（header / classes 段结束哨兵之后）。
/// 真实 AutoCAD 与 ODA 写出器都会补；缺省虽不致读失败，但与规范不符。
constexpr int kSectionTrailerZeros = 8;

// -----------------------------------------------------------------------------
//  小端追加
// -----------------------------------------------------------------------------
void putU16(std::vector<std::uint8_t>& v, std::uint16_t x)
{
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
}
void putU32(std::vector<std::uint8_t>& v, std::uint32_t x)
{
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF));
}
void putU64(std::vector<std::uint8_t>& v, std::uint64_t x)
{
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF));
}
void putU32At(std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x)
{
    for (int i = 0; i < 4; ++i) v[off + i] = static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF);
}
void putU64At(std::vector<std::uint8_t>& v, std::size_t off, std::uint64_t x)
{
    for (int i = 0; i < 8; ++i) v[off + i] = static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF);
}
/// UMC：无符号模长字符（7 位一组，除末组外高位置 0x80）。
/// 对偶于 dwgBuffer::getUModularChar / dwgBitWriter::writeUMC。
/// R2010+ 的对象记录在 MS 尺寸后用它写「句柄流位数」。
void putUMC(std::vector<std::uint8_t>& v, std::uint32_t u)
{
    std::uint8_t g[6];
    int n = 0;
    do { g[n++] = static_cast<std::uint8_t>(u & 0x7Fu); u >>= 7; } while (u && n < 6);
    for (int i = 0; i < n; ++i)
        v.push_back(i < n - 1 ? static_cast<std::uint8_t>(g[i] | 0x80u) : g[i]);
}
void appendSentinel(std::vector<std::uint8_t>& v, int kind, bool begin)
{
    std::uint8_t s[16];
    dwgSentinelBytes(kind, begin, s);
    v.insert(v.end(), s, s + 16);
}

// -----------------------------------------------------------------------------
//  幻数序列（与 dwgReader18::genMagicNumber 同一 LCG）
// -----------------------------------------------------------------------------
std::vector<std::uint8_t> magicSequence(std::size_t n)
{
    std::vector<std::uint8_t> seq(n);
    std::uint32_t seed = 1;
    for (auto& b : seq) {
        seed = seed * 0x343FDu + 0x269EC3u;
        b = static_cast<std::uint8_t>((seed >> 16) & 0xFF);
    }
    return seq;
}

// -----------------------------------------------------------------------------
//  页校验（Adler-32 变体，与 dwgReader18::checksum 逐位一致）
// -----------------------------------------------------------------------------
std::uint32_t adler32(std::uint32_t seed, const std::uint8_t* data, std::size_t size)
{
    std::uint32_t sum1 = seed & 0xFFFF;
    std::uint32_t sum2 = (seed >> 16) & 0xFFFF;
    while (size != 0) {
        const std::uint32_t chunk = (0x15B0u < size) ? 0x15B0u : static_cast<std::uint32_t>(size);
        size -= chunk;
        for (std::uint32_t i = 0; i < chunk; ++i) {
            sum1 += *data++;
            sum2 += sum1;
        }
        sum1 %= 0xFFF1;
        sum2 %= 0xFFF1;
    }
    return (sum2 << 16) | (sum1 & 0xFFFF);
}

// -----------------------------------------------------------------------------
//  CRC-32/ISO-HDLC（内层头校验；读取端仅打印不校验）
// -----------------------------------------------------------------------------
std::uint32_t crc32(const std::uint8_t* data, std::size_t size)
{
    static std::uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i)
        c = (c >> 8) ^ table[(c ^ data[i]) & 0xFF];
    return ~c;
}

/// 使 32 + 压缩长度（再计 0x20 对齐补长）为 0x20 倍数所需的补长字节数
std::size_t compressionPadding(std::size_t compressedSize)
{
    return 0x1F - ((compressedSize + 0x1F) % 0x20);
}

/// 数据页页头 XOR 掩码：0x4164536b ^ 页地址，按 4 字节模式作用
void applyMask(std::uint8_t* buf, std::size_t size, std::uint64_t address)
{
    const std::uint32_t mask = kMaskBase ^ static_cast<std::uint32_t>(address);
    for (std::size_t i = 0; i + 4 <= size; i += 4) {
        buf[i]     ^= static_cast<std::uint8_t>(mask & 0xFF);
        buf[i + 1] ^= static_cast<std::uint8_t>((mask >> 8) & 0xFF);
        buf[i + 2] ^= static_cast<std::uint8_t>((mask >> 16) & 0xFF);
        buf[i + 3] ^= static_cast<std::uint8_t>((mask >> 24) & 0xFF);
    }
}

// -----------------------------------------------------------------------------
//  LZ77 压缩（AC18 变体）
//
//  与 dwgCompressor::decompress18 严格对偶。相对通用实现收紧了两处，
//  使输出必定落在读取端可解码的范围内：
//    · 回溯距离上限取 0x7FFF（读取端 long 分支只加 0x3FFF 偏置，
//      且不使用操作码的 0x08 位扩展，无法表达 >0x8000 的距离）；
//    · 长度为 3 的「长匹配」不编码（其操作码会与 0x11 终止符冲突），
//      退回字面量处理。
// -----------------------------------------------------------------------------
class Compressor
{
public:
    std::vector<std::uint8_t> compress(const std::uint8_t* src, std::size_t size)
    {
        m_block.assign(0x8000, -1);
        m_dest.clear();
        m_src = src;
        m_size = static_cast<int>(size);

        int currOffset = 0;
        int currPosition = 4;
        int compressionOffset = 0;
        int matchPos = 0;

        const int limit = std::max(0, m_size - 0x13);
        while (currPosition < limit) {
            int chunkOffset = 0, chunkMatchPos = 0;
            if (!findMatch(currPosition, &chunkOffset, &chunkMatchPos)) {
                ++currPosition;
                continue;
            }
            const int mask = currPosition - currOffset;
            if (compressionOffset != 0)
                writeMatch(matchPos, compressionOffset, mask);
            writeLiterals(currOffset, mask);
            currPosition += chunkOffset;
            currOffset = currPosition;
            compressionOffset = chunkOffset;
            matchPos = chunkMatchPos;
        }

        const int literalLength = m_size - currOffset;
        if (compressionOffset != 0)
            writeMatch(matchPos, compressionOffset, literalLength);
        writeLiterals(currOffset, literalLength);

        m_dest.push_back(0x11);
        m_dest.push_back(0x00);
        m_dest.push_back(0x00);
        return m_dest;
    }

private:
    void writeLen(int len)
    {
        while (len > 0xFF) { len -= 0xFF; m_dest.push_back(0x00); }
        m_dest.push_back(static_cast<std::uint8_t>(len));
    }
    void writeOpcode(std::uint8_t opcode, int value, int threshold)
    {
        if (value <= threshold)
            m_dest.push_back(static_cast<std::uint8_t>(opcode | (value - 2)));
        else { m_dest.push_back(opcode); writeLen(value - threshold); }
    }
    void writeLiterals(int start, int length)
    {
        if (length == 0) return;
        if (length > 3) writeOpcode(0x00, length - 1, 0x11);
        for (int i = 0; i < length; ++i) m_dest.push_back(m_src[start + i]);
    }
    void writeMatch(int matchPos, int compressionOffset, int mask)
    {
        std::uint8_t curr = 0, next = 0;
        if (compressionOffset >= 0x0F || matchPos > 0x400) {
            if (matchPos <= 0x4000) {
                --matchPos;
                writeOpcode(0x20, compressionOffset, 0x21);
            } else {
                matchPos -= 0x4000;
                writeOpcode(0x10, compressionOffset, 0x09);
            }
            curr = static_cast<std::uint8_t>((matchPos & 0xFF) << 2);
            next = static_cast<std::uint8_t>((matchPos >> 6) & 0xFF);
            if (mask < 4) curr = static_cast<std::uint8_t>(curr | mask);
        } else {
            --matchPos;
            curr = static_cast<std::uint8_t>(((compressionOffset + 1) << 4)
                                             | ((matchPos & 0x03) << 2));
            next = static_cast<std::uint8_t>((matchPos >> 2) & 0xFF);
            if (mask < 4) curr = static_cast<std::uint8_t>(curr | mask);
        }
        m_dest.push_back(curr);
        m_dest.push_back(next);
    }

    bool findMatch(int currPosition, int* outLen, int* outPos)
    {
        const std::int32_t v1 = static_cast<std::int32_t>(m_src[currPosition + 3]) << 6;
        const std::int32_t v2 = v1 ^ m_src[currPosition + 2];
        const std::int32_t v3 = (v2 << 5) ^ m_src[currPosition + 1];
        const std::int32_t v4 = (v3 << 5) ^ m_src[currPosition];
        int index = static_cast<int>((v4 + (v4 >> 5)) & 0x7FFF);

        std::int32_t value = m_block[index];
        std::int64_t dist = (value >= 0) ? (static_cast<std::int64_t>(currPosition) - value) : (1 << 30);
        if (value < 0 || dist > 0x7FFF) {
            m_block[index] = currPosition;
            return false;
        }
        if (dist > 0x400 && m_src[currPosition + 3] != m_src[value + 3]) {
            index = (index & 0x7FF) ^ 0x401F;
            value = m_block[index];
            dist = (value >= 0) ? (static_cast<std::int64_t>(currPosition) - value) : (1 << 30);
            if (value < 0 || dist > 0x7FFF
                || (dist > 0x400 && m_src[currPosition + 3] != m_src[value + 3])) {
                m_block[index] = currPosition;
                return false;
            }
        }
        if (m_src[currPosition] != m_src[value] || m_src[currPosition + 1] != m_src[value + 1]
            || m_src[currPosition + 2] != m_src[value + 2]) {
            m_block[index] = currPosition;
            return false;
        }

        int length = 3;
        int index2 = value + 3;
        int curr = currPosition + 3;
        while (curr < m_size && m_src[index2] == m_src[curr]) { ++length; ++index2; ++curr; }

        m_block[index] = currPosition;
        // 长度为 3 的长匹配（距离 > 0x4000）其操作码会与 0x11 终止符冲突，不编码
        if (dist > 0x4000 && length == 3)
            return false;
        *outLen = length;
        *outPos = static_cast<int>(dist);
        return true;
    }

    const std::uint8_t* m_src = nullptr;
    int m_size = 0;
    std::vector<std::int32_t> m_block;
    std::vector<std::uint8_t> m_dest;
};

// -----------------------------------------------------------------------------
//  段与页
// -----------------------------------------------------------------------------
struct Section {
    std::uint32_t id = 0;                   ///< 规范段号（真实 R2004 文件的固定编号）
    std::string name;
    std::vector<std::uint8_t> payload;      ///< 未压缩节载荷
    std::vector<std::uint32_t> pageIds;     ///< 本节的页号（顺序）
    bool raw = false;                       ///< true = 页按原文存放（段记录 comp=1）
    std::uint32_t pageSize = kMaxPageSize;  ///< 单页解压尺寸（= 段记录的 maxSize）
    std::uint64_t declaredSize = 0;         ///< 非 0 时段记录的 size 用它（内容可无页）
};

/// 0x20 对齐向上取整
std::size_t alignUp(std::size_t v, std::size_t boundary)
{
    return (v + boundary - 1) / boundary * boundary;
}

struct PageInfo {
    std::uint32_t number = 0;
    std::size_t   size = 0;                 ///< 页总长（页头 + 数据 + 补长）
    std::uint64_t startOffset = 0;          ///< 本页数据在该段解压流中的起始偏移
    std::uint32_t compressedSize = 0;       ///< 本页压缩后字节数
    std::uint64_t address = 0;              ///< 本页在文件中的物理地址
};

// -----------------------------------------------------------------------------
//  元数据段（对照真实 R2004 文件逐字节核对过的固定结构）
// -----------------------------------------------------------------------------

/// AcDb:Template（段 1）：RS 描述串长度 + 描述 + RS $MEASUREMENT。
/// 无模板描述时整体 4 字节（0, measurement）——与真实文件一致。
std::vector<std::uint8_t> buildTemplatePayload(std::uint16_t measurement)
{
    std::vector<std::uint8_t> d;
    putU16(d, 0);                  // 模板描述长度 = 0
    putU16(d, measurement);        // 0 = 英制，1 = 公制
    return d;
}

/// AcDb:ObjFreeSpace（段 5，53 字节）：对象节空闲空间信息。
/// 结构：RL 0 + RL 近似对象数 + TDUPDATE(8) + RL 0 + RC 4 + 4 组 64 位定值。
std::vector<std::uint8_t> buildObjFreeSpacePayload(std::uint32_t handleCount,
                                                   std::uint32_t tdDay,
                                                   std::uint32_t tdMsec)
{
    std::vector<std::uint8_t> d;
    putU32(d, 0);                          // unknown = 0
    putU32(d, handleCount);                // 近似对象数（提示值，不影响读取）
    putU32(d, tdDay);                      // TDUPDATE.days（Julian 日）
    putU32(d, tdMsec);                     // TDUPDATE.ms
    putU32(d, 0);                          // 对象节偏移（分页格式恒 0）
    d.push_back(4);                        // 后随 4 组 64 位定值
    putU32(d, 0x00000032); putU32(d, 0);   // 0x32
    putU32(d, 0x00000064); putU32(d, 0);   // 0x64
    putU32(d, 0x00000200); putU32(d, 0);   // 0x200
    putU32(d, 0xFFFFFFFF); putU32(d, 0);   // 空闲块哨兵
    return d;
}

/// AcDb:AuxHeader（段 10）：版本/保存次数/时间戳/HANDSEED。
/// 结构与真实文件逐字节一致：0xFF 0x77 0x01 引导，两遍版本号，
/// 0x0005/0x0893 常量对，TDCREATE/TDUPDATE/HANDSEED 与保存计数。
/// 长度：AC1018~AC1027 = 119 字节；AC1032（R2018）再 +6 字节（见函数尾注）。
std::vector<std::uint8_t> buildAuxHeaderPayload(std::uint32_t tdDay,
                                                std::uint32_t tdMsec,
                                                std::uint32_t handSeed,
                                                std::uint16_t internalVer,
                                                bool r2018)
{
    const std::uint16_t kInternalVer = internalVer;
    constexpr std::uint32_t kNumSaves = 1;       // 保存次数（新建即 1）
    std::vector<std::uint8_t> d;
    d.push_back(0xFF); d.push_back(0x77); d.push_back(0x01);
    putU16(d, kInternalVer);               // DWG 内部版本
    putU16(d, 0);                          // 维护版本（须与文件头 0x0B 一致）
    putU32(d, kNumSaves);                  // 保存次数
    putU32(d, 0xFFFFFFFF);                 // -1
    putU16(d, static_cast<std::uint16_t>(kNumSaves));  // 保存次数 part1
    putU16(d, 0);                          // 保存次数 part2
    putU32(d, 0);
    putU16(d, kInternalVer); putU16(d, 0); // 版本号（重复）
    putU16(d, kInternalVer); putU16(d, 0);
    putU16(d, 0x0005); putU16(d, 0x0893);  // 固定常量对（×2）
    putU16(d, 0x0005); putU16(d, 0x0893);
    putU16(d, 0); putU16(d, 1);
    for (int i = 0; i < 5; ++i) putU32(d, 0);
    putU32(d, tdDay);                      // TDCREATE.days
    putU32(d, tdMsec);                     // TDCREATE.ms
    putU32(d, tdDay);                      // TDUPDATE.days
    putU32(d, tdMsec);                     // TDUPDATE.ms
    putU32(d, handSeed);                   // HANDSEED
    putU32(d, 0);                          // 教育版绘图戳
    putU16(d, 0);
    putU16(d, static_cast<std::uint16_t>(kNumSaves));
    for (int i = 0; i < 3; ++i) putU32(d, 0);
    putU32(d, kNumSaves);
    for (int i = 0; i < 4; ++i) putU32(d, 0);
    // R2018+：尾部再 3 个 RS（6 字节）——libredwg auxheader.spec 的
    //   SINCE (R_2018) { FIELD_VECTOR_INL (zero_18, RS, 3, 0); }
    // R2004~R2013 无这三个字段。漏写会让本段比规格少 6 字节，严格读取端按
    // 段长/后续哨兵校验即判文件损坏。
    if (r2018) {
        putU16(d, 0);
        putU16(d, 0);
        putU16(d, 0);
    }
    return d;
}

/// AcDb:SummaryInfo（段 2，58 字节，原文页）：图形属性。
/// 结构 = 7 个 u16 长度前缀的属性串（空串 = 长度 1 + NUL，即 `01 00 00`）+
/// 零补长——与真实文件/OpenCADStudio 逐字节一致（此处 7 个属性全部留空）。
std::vector<std::uint8_t> buildSummaryInfoPayload()
{
    std::vector<std::uint8_t> d(58, 0);
    for (int i = 0; i < 7; ++i) {
        d[i * 3 + 0] = 0x01;                    // 串长 = 1（仅 NUL）
        d[i * 3 + 1] = 0x00;
        d[i * 3 + 2] = 0x00;                    // NUL
    }
    return d;
}

/// AcDb:RevHistory（段 3，16 字节，LZ77 页）：修订历史（空）。
std::vector<std::uint8_t> buildRevHistoryPayload()
{
    std::vector<std::uint8_t> d(16, 0);
    putU32At(d, 8, 1);                          // 修订计数 = 1
    return d;
}

/// UTF-16LE 串（含 NUL）。
void appendUtf16(std::vector<std::uint8_t>& d, const std::string& text)
{
    putU16(d, static_cast<std::uint16_t>(text.size() + 1));   // 长度含 NUL
    for (char ch : text) {
        d.push_back(static_cast<std::uint8_t>(ch));
        d.push_back(0x00);
    }
    d.push_back(0x00);
    d.push_back(0x00);
}

/// AcDb:AppInfo（段 11，原文页 ×0x80）：产品信息（UTF-16LE 串表）。
/// 结构对照真实文件逐段核对：u32 版本(2) + 名单串 + 若干 u16 长度前缀的
/// UTF-16LE 串（间以固定零填充）。读取端按长度逐一跳读，串内容不参与校验。
std::vector<std::uint8_t> buildAppInfoPayload()
{
    std::vector<std::uint8_t> d;
    putU32(d, 2);                               // 格式版本
    appendUtf16(d, "AppInfoDataList");
    appendUtf16(d, std::string());              // 空字段（长度 3，全 NUL）
    for (int i = 0; i < 12; ++i) d.push_back(0x00);
    appendUtf16(d, LYCAD_VERSION);              // 版本
    for (int i = 0; i < 16; ++i) d.push_back(0x00);
    appendUtf16(d, "Generated by LyCAD (libdxfrw dwgW)");
    for (int i = 0; i < 16; ++i) d.push_back(0x00);
    appendUtf16(d,
        "<ProductInformation name=\"LyCAD\" build_version=\"" LYCAD_VERSION
        "\" registry_version=\"" LYCAD_VERSION
        "\" install_id_string=\"LyCAD\" registry_localeID=\"1033\"/>");
    return d;
}

/// 生成 1×1 白色 24 位 DIB（BITMAPINFOHEADER + 一行像素，行按 4 字节对齐）。
/// R2004 的 AcDb:Preview 缩略图是 **DIB**（无 14 字节 BITMAPFILEHEADER），
/// 描述符 code = 2；PNG 自 R2007 起才允许。
std::vector<std::uint8_t> buildPreviewDib()
{
    std::vector<std::uint8_t> d;
    putU32(d, 40);                          // biSize = sizeof(BITMAPINFOHEADER)
    putU32(d, 1);                           // biWidth
    putU32(d, 1);                           // biHeight
    putU16(d, 1);                           // biPlanes
    putU16(d, 24);                          // biBitCount
    putU32(d, 0);                           // biCompression = BI_RGB
    putU32(d, 4);                           // biSizeImage（1 行 → 4 字节对齐）
    putU32(d, 0); putU32(d, 0);             // biXPelsPerMeter / biYPelsPerMeter
    putU32(d, 0); putU32(d, 0);             // biClrUsed / biClrImportant
    d.push_back(0xFF); d.push_back(0xFF); d.push_back(0xFF); d.push_back(0x00);  // 白 + 对齐
    return d;
}

/// AcDb:Preview（R2004）：由 THUMBNAIL 哨兵界定的**容器**，而非裸图像：
///   [BEGIN 16B][RL overall][RC 图片数] 图片数×{RC code, RL 绝对起始, RL 长度}
///   [图片数据][END 16B]
/// BMP 图片前固定一段 80 字节保留头（code = 1），其后才是 code = 2 的 DIB。
/// 描述符里的起始位置是**绝对文件偏移**，故 base 必须等于容器所在页载荷的地址
///（= 页地址 + 0x20）；地址必须在压缩/校验之前确定，事后回填会破坏页 adler 校验。
std::vector<std::uint8_t> buildPreviewContainer(std::uint64_t base,
                                                const std::vector<std::uint8_t>& dib)
{
    constexpr std::uint32_t kReservedHeaderLen = 80;   // code=1 的保留头长度
    const std::size_t count   = 2;                     // 保留头 + DIB
    const std::size_t descLen = 9 * count;
    const std::size_t dataRel = 16 + 4 + 1 + descLen;  // 图片数据相对容器起点的偏移
    const std::uint32_t headerStart = static_cast<std::uint32_t>(base + dataRel);
    const std::uint32_t imageStart  = headerStart + kReservedHeaderLen;
    const std::uint32_t overall = static_cast<std::uint32_t>(
        1 + descLen + kReservedHeaderLen + dib.size());

    std::vector<std::uint8_t> v;
    appendSentinel(v, kSentThumbnailBegin, true);
    putU32(v, overall);
    v.push_back(static_cast<std::uint8_t>(count));
    v.push_back(1);                                    // code = 1：保留头
    putU32(v, headerStart);
    putU32(v, kReservedHeaderLen);
    v.push_back(2);                                    // code = 2：BMP(DIB)
    putU32(v, imageStart);
    putU32(v, static_cast<std::uint32_t>(dib.size()));
    v.insert(v.end(), kReservedHeaderLen, 0x00);
    v.insert(v.end(), dib.begin(), dib.end());
    appendSentinel(v, kSentThumbnailBegin, false);
    return v;
}

/// 把一段未压缩内容切成页写入 out，返回各页页号与总长。
/// 空载荷不写任何页（真实 R2004 文件中空节就是 0 页记录，如 RevHistory）。
/// raw 模式（段记录 comp=1）：页数据不经 LZ77，按原文整页存放（零补长），
/// 页尺寸 = pageSize——SummaryInfo/AppInfo/Preview 等元数据段即如此存放。
void writeDataPages(std::vector<std::uint8_t>& out,
                    std::vector<PageInfo>& pages,
                    const std::vector<std::uint8_t>& payload,
                    std::uint32_t sectionNumber,
                    std::vector<std::uint32_t>& outPageIds,
                    bool raw = false,
                    std::uint32_t pageSize = kMaxPageSize)
{
    const std::size_t total = payload.size();
    if (total == 0) return;
    const std::size_t chunk = raw ? pageSize : kMaxPageSize;
    const std::size_t fullPages = total / chunk;
    std::size_t offset = 0;

    for (std::size_t i = 0; i <= fullPages; ++i) {
        const std::size_t chunkLen = std::min(chunk, total - offset);
        if (chunkLen == 0 && i == fullPages && fullPages > 0) break;

        std::vector<std::uint8_t> page(chunk, 0);
        if (chunkLen > 0)
            std::memcpy(page.data(), payload.data() + offset, chunkLen);

        std::vector<std::uint8_t> compData;
        if (raw) {
            compData = std::move(page);                 // 原文整页（含零补长）
        } else {
            Compressor comp;
            compData = comp.compress(page.data(), chunk);
        }

        const std::uint64_t address = out.size();
        const std::uint32_t cSize = static_cast<std::uint32_t>(compData.size());

        std::vector<std::uint8_t> hdr(kDataPageHeader, 0);
        const std::size_t cSizeOff = 8, offLo = 16, hSum = 24, dSum = 28;
        // 0x0C 记录**本页物理尺寸**（页头 + 压缩数据 + 0x20 对齐补长）。
        // AutoCAD 与 ODA 写出器都写这个值；此前写的是页最大解压尺寸
        //（0x7400），与两个可正常打开的文件都不一致。
        const std::uint32_t pageSizeField = static_cast<std::uint32_t>(
            kDataPageHeader + cSize + compressionPadding(cSize));
        hdr[0] = static_cast<std::uint8_t>(kPageTypeData & 0xFF);
        hdr[1] = static_cast<std::uint8_t>((kPageTypeData >> 8) & 0xFF);
        hdr[2] = static_cast<std::uint8_t>((kPageTypeData >> 16) & 0xFF);
        hdr[3] = static_cast<std::uint8_t>((kPageTypeData >> 24) & 0xFF);
        for (int b = 0; b < 4; ++b) {
            hdr[4 + b] = static_cast<std::uint8_t>((sectionNumber >> (8 * b)) & 0xFF);
            hdr[cSizeOff + b] = static_cast<std::uint8_t>((cSize >> (8 * b)) & 0xFF);
            hdr[12 + b] = static_cast<std::uint8_t>((pageSizeField >> (8 * b)) & 0xFF);
            hdr[offLo + b] = static_cast<std::uint8_t>((offset >> (8 * b)) & 0xFF);
        }
        for (int b = 0; b < 4; ++b) hdr[offLo + 4 + b] = 0;   // startOffset 高 32 位

        const std::uint32_t dataSum = adler32(0, compData.data(), compData.size());
        for (int b = 0; b < 4; ++b)
            hdr[dSum + b] = static_cast<std::uint8_t>((dataSum >> (8 * b)) & 0xFF);
        const std::uint32_t headSum = adler32(dataSum, hdr.data(), hdr.size());
        for (int b = 0; b < 4; ++b)
            hdr[hSum + b] = static_cast<std::uint8_t>((headSum >> (8 * b)) & 0xFF);

        applyMask(hdr.data(), hdr.size(), address);

        out.insert(out.end(), hdr.begin(), hdr.end());
        out.insert(out.end(), compData.begin(), compData.end());

        const std::size_t pad = compressionPadding(cSize);
        if (pad > 0) {
            const std::vector<std::uint8_t> magic = magicSequence(256);
            out.insert(out.end(), magic.begin(), magic.begin() + static_cast<std::ptrdiff_t>(pad));
        }

        const std::uint32_t pageNumber = static_cast<std::uint32_t>(pages.size());
        outPageIds.push_back(pageNumber);
        pages.push_back(PageInfo{ pageNumber, out.size() - static_cast<std::size_t>(address),
                                  static_cast<std::uint64_t>(offset), cSize, address });

        offset += chunkLen;
        if (offset >= total) break;
    }
}

/// 把一个系统页（段映射 / 页映射）写到 out
void writeSystemPage(std::vector<std::uint8_t>& out,
                     std::vector<PageInfo>& pages,
                     std::uint32_t pageType,
                     const std::vector<std::uint8_t>& content,
                     std::vector<std::uint32_t>* outPageId)
{
    Compressor comp;
    const std::vector<std::uint8_t> compData = comp.compress(content.data(), content.size());

    const std::uint64_t address = out.size();
    const std::uint32_t cSize = static_cast<std::uint32_t>(compData.size());
    const std::uint32_t uSize = static_cast<std::uint32_t>(content.size());

    std::vector<std::uint8_t> hdr(kSysPageHeader, 0);
    for (int b = 0; b < 4; ++b) {
        hdr[0 + b]  = static_cast<std::uint8_t>((pageType >> (8 * b)) & 0xFF);
        hdr[4 + b]  = static_cast<std::uint8_t>((uSize >> (8 * b)) & 0xFF);
        hdr[8 + b]  = static_cast<std::uint8_t>((cSize >> (8 * b)) & 0xFF);
    }
    // 压缩类型：2 = LZ77。u32 字段，高 3 字节必须为 0——此前误写成
    // 4 个字节各填 2（u32 = 0x02020202），严格校验该字段的读取端
    //（OpenCADStudio/浩辰）会把页数据当未压缩内容解析，整文件判损坏。
    hdr[12] = 2;
    std::uint32_t sum = adler32(0, hdr.data(), hdr.size());
    sum = adler32(sum, compData.data(), compData.size());
    for (int b = 0; b < 4; ++b)
        hdr[16 + b] = static_cast<std::uint8_t>((sum >> (8 * b)) & 0xFF);

    out.insert(out.end(), hdr.begin(), hdr.end());
    out.insert(out.end(), compData.begin(), compData.end());

    // 页分配尺寸与数据页同规：align32(0x20 + 压缩长)。真实 R2004 文件的
    // 页映射页即如此分配（如 132 字节压缩数据 → 0xC0 页），虽其数据自 0x14
    // 偏移起读。补长字节为幻数序列（与数据页一致）。
    const std::size_t pageSize = alignUp(kDataPageHeader + cSize, kAlign);
    if (pageSize > kSysPageHeader + cSize) {
        const std::vector<std::uint8_t> magic = magicSequence(256);
        const std::size_t pad = pageSize - kSysPageHeader - cSize;
        out.insert(out.end(), magic.begin(), magic.begin() + static_cast<std::ptrdiff_t>(pad));
    }

    const std::uint32_t pageNumber = static_cast<std::uint32_t>(pages.size());
    if (outPageId != nullptr) outPageId->push_back(pageNumber);
    pages.push_back(PageInfo{ pageNumber, out.size() - static_cast<std::size_t>(address),
                              0, cSize, address });   // 系统页无解压偏移（整页即内容）
}

} // namespace

// =============================================================================
//  dwgWriter18
// =============================================================================
namespace dwgWriter18 {

bool supports(DRW::Version ver) noexcept
{
    return ver == DRW::AC1018;
}

bool write(dwgW& writer, DRW_Interface* iface)
{
    return writeAs(writer, iface, DRW::AC1018);
}

bool writeAs(dwgW& writer, DRW_Interface* iface, DRW::Version ver)
{
    // R2010（AC1024）/ R2013（AC1027）与 R2004（AC1018）的文件容器**同属一族**：
    // 0x80 处的加密文件头（"AcFssFcAJMB"）、页映射 / 段映射、分页数据都一致。
    // libredwg 的 decode.c 也是把 R2010 及以后交给 decode_R2004（注释写着
    // "for 2004 and 2010+"），而 R2007 走的是另一套 decode_R2007。
    // 故这里只需按版本取标识与常量。
    const bool r2010 = (ver > DRW::AC1018);        // 2010+（含 2013/2018）
    const bool r2013 = (ver >= DRW::AC1027);       // 2013+
    const bool r2018 = (ver >= DRW::AC1032);       // 2018+
    const char* const versionStr = r2018 ? "AC1032"
                                : r2013 ? "AC1027"
                                : r2010 ? "AC1024" : "AC1018";
    // 内部版本号：与 libredwg 版本表的 dwg_version 一致（R2004 = 0x19 = 25、
    // R2010 = 0x1d = 29、R2013 = 0x1f = 31、R2018 = 0x21 = 33）。
    // R2004 沿用本实现既有的 25。
    const std::uint16_t internalVer = r2018 ? 33 : r2013 ? 31 : r2010 ? 29 : 25;

    dwgW::SectionData sd;
    if (!writer.buildSections(iface, ver, sd))
        return false;

    // ---- 1. 组节载荷 ------------------------------------------------------------
    // 各节先构建载荷，再按真实 R2004 文件的段记录顺序装配（页也按此顺序落盘）。

    std::vector<std::uint8_t> headerPayload;
    {   // HEADER：哨兵 + RL 尺寸 + 变量位流 + CRC16 + 结束哨兵 + R2004+ 尾 8 字节 0
        // 结束哨兵不可省：读取端 parseDwg 末尾在 2004+ 分支会用
        // `buf->moveBitPos(-128)` 回退 16 字节定位头节结束哨兵。
        appendSentinel(headerPayload, kSentVariableBegin, true);
        putU32(headerPayload, static_cast<std::uint32_t>(sd.headerData.size()));
        headerPayload.insert(headerPayload.end(), sd.headerData.begin(), sd.headerData.end());
        const std::uint16_t crc = dwgCalcCrc16(
            0xC0C1, headerPayload.data() + 16, headerPayload.size() - 16);
        putU16(headerPayload, crc);
        appendSentinel(headerPayload, kSentVariableBegin, false);
        headerPayload.insert(headerPayload.end(), kSectionTrailerZeros, 0x00);
    }
    std::vector<std::uint8_t> classesPayload;
    {   // CLASSES：哨兵 + RL 尺寸 + [BS 最大类号, RC, RC, Bit] + 类记录 + CRC16 + 哨兵 + 尾 8 字节
        // 先把「RL 之后的整个类数据区」编码进临时缓冲：RL 的语义是**数据区**长度，
        // 必须包含那 5 字节（33 位）前导。此前 RL 只算类记录长度，比实际少 5 字节；
        // 按 RL 切片再解析的读取端（ODA 内核 / 浩辰）会把最后一条类记录截断，
        // 越界后结束哨兵位置整体偏移，进而判定文件损坏。
        std::vector<std::uint8_t> classBody;
        if (r2010) {
            // R2007+ 的 Classes 节前导由 buildSections 一并编码在 sd.classesData 里：
            //   [RL hsize][RL bitsize][BS maxClassNum][RC][RC][B][类记录][字符串区][RS][B]
            // 容器只需在它前面再补一个 RL size。次序为
            //   size → hsize → bitsize → BS → RC → RC → B → 类记录
            // —— 读取端（dwgReader18::readDwgClasses 的 2010+/2007+ 分支）与
            // libredwg（decode.c 的 read_2004_section_classes）都是这个次序。
            // 若照 AC1018 那样在这里再补一遍 [BS][RC][RC][B]，次序会变成
            //   size → BS → RC → RC → B → hsize → bitsize
            // 读取端于是把 bitsize 当成 maxClassNum、把类记录当 bitsize，类表整体
            // 错位（实测直接 BAD_READ_CLASSES）。
            classBody = sd.classesData;
        } else {
            dwgBitWriter w;
            w.writeBS(static_cast<dint16>(sd.maxClassNum));
            w.writeRC(0);
            w.writeRC(0);
            w.writeB(false);
            for (std::uint8_t b : sd.classesData) w.writeRC(b);
            w.padToByte();
            classBody = w.data();
        }
        appendSentinel(classesPayload, kSentClassBegin, true);
        putU32(classesPayload, static_cast<std::uint32_t>(classBody.size()));
        classesPayload.insert(classesPayload.end(), classBody.begin(), classBody.end());
        const std::uint16_t crc = dwgCalcCrc16(0xC0C1, classesPayload.data() + 16,
                                               classesPayload.size() - 16);
        putU16(classesPayload, crc);
        appendSentinel(classesPayload, kSentClassBegin, false);
        classesPayload.insert(classesPayload.end(), kSectionTrailerZeros, 0x00);
    }

    // OBJECTS：对象记录序列，偏移相对本段起点。
    //   R2004（AC1018）：[MS 尺寸][对象体][CRC16]
    //   R2010（AC1024）：[MS 尺寸][UMC 句柄流位数][对象体][CRC16]
    //                     —— 读取端在 MS 之后还要读一个 UMC（dwgreader.cpp 的
    //                     `if (version > DRW::AC1021) bs = dbuf->getUModularChar();`，
    //                     libredwg 同：`obj->handlestream_size = bit_read_UMC()`），
    //                     少了这 1~4 字节，对象类型/内容整体前移一位字节，
    //                     表控制对象会被读成随机的 oType（实测 "Not LineType
    //                     control object, found oType 12545 instead 0x38"）。
    std::vector<std::uint8_t> objectsPayload;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> objectEntries;   // (句柄, 段内偏移)
    for (const dwgW::ObjectRecord& o : sd.objects) {
        const std::uint32_t offset = static_cast<std::uint32_t>(objectsPayload.size());
        objectEntries.emplace_back(o.handle, offset);
        const std::uint32_t bodySize = static_cast<std::uint32_t>(o.body.size());
        if (bodySize > 0x7FFFu) {
            putU16(objectsPayload, static_cast<std::uint16_t>((bodySize & 0x7FFFu) | 0x8000u));
            putU16(objectsPayload, static_cast<std::uint16_t>(bodySize >> 15));
        } else {
            putU16(objectsPayload, static_cast<std::uint16_t>(bodySize));
        }
        if (r2010) putUMC(objectsPayload, o.handleBits);
        objectsPayload.insert(objectsPayload.end(), o.body.begin(), o.body.end());
        const std::uint16_t crc = dwgCalcCrc16(0xC0C1,
            objectsPayload.data() + offset, objectsPayload.size() - offset);
        putU16(objectsPayload, crc);
    }

    // HANDLES：对象映射（与 AC15 同构；偏移为本段内偏移）
    std::vector<std::uint8_t> handlesPayload;
    {
        std::sort(objectEntries.begin(), objectEntries.end());
        std::size_t i = 0;
        while (i < objectEntries.size()) {
            std::uint32_t lastHandle = 0, lastOffset = 0;
            std::vector<std::uint8_t> sec;
            while (i < objectEntries.size()) {
                dwgBitWriter w;
                w.writeUMC(objectEntries[i].first - lastHandle);
                w.writeMC(static_cast<dint32>(objectEntries[i].second)
                          - static_cast<dint32>(lastOffset));
                const std::vector<std::uint8_t>& bytes = w.data();
                if (!sec.empty() && sec.size() + bytes.size() > 2030) break;
                sec.insert(sec.end(), bytes.begin(), bytes.end());
                lastHandle = objectEntries[i].first;
                lastOffset = objectEntries[i].second;
                ++i;
                if (sec.size() > 2030 - 32) break;
            }
            const std::uint16_t secSize = static_cast<std::uint16_t>(2 + sec.size());
            std::vector<std::uint8_t> framed;
            framed.push_back(static_cast<std::uint8_t>(secSize >> 8));
            framed.push_back(static_cast<std::uint8_t>(secSize & 0xFF));
            framed.insert(framed.end(), sec.begin(), sec.end());
            const std::uint16_t crc = dwgCalcCrc16(0xC0C1, framed.data(), framed.size());
            framed.push_back(static_cast<std::uint8_t>(crc >> 8));
            framed.push_back(static_cast<std::uint8_t>(crc & 0xFF));
            handlesPayload.insert(handlesPayload.end(), framed.begin(), framed.end());
        }
        // 终止节（size = 2）
        const std::uint8_t term[2] = { 0x00, 0x02 };
        const std::uint16_t termCrc = dwgCalcCrc16(0xC0C1, term, 2);
        handlesPayload.push_back(term[0]);
        handlesPayload.push_back(term[1]);
        handlesPayload.push_back(static_cast<std::uint8_t>(termCrc >> 8));
        handlesPayload.push_back(static_cast<std::uint8_t>(termCrc & 0xFF));
    }

    // 按真实 R2004 文件的段记录顺序装配（段号是 AutoCAD 的规范编号，
    // 读取端可能按号取义——顺序编号会让第三方识别成完全不同的节）。
    // 元数据段（SummaryInfo/Preview/AppInfo/RevHistory）按真实文件的形态
    // 给出内容：SummaryInfo/AppInfo/Preview 原文页（comp=1），RevHistory 压缩页。
    // 缩略图图像数据：R2004 的预览容器把图片按 code 分类，BMP 一支为
    // **DIB**（含 BITMAPINFOHEADER、不含 14 字节 BITMAPFILEHEADER）。
    // 调用方若经 dwgW::setThumbnail 提供了字节，即按 DIB 解释（AC15 的
    // 「哨兵内裸字节」形态在 R2004 不成立，容器必须带格式码）；否则给一张
    // 1×1 白色 DIB。
    const std::vector<std::uint8_t> previewDib =
        sd.thumbnail.empty() ? buildPreviewDib() : sd.thumbnail;

    std::vector<Section> sections;
    auto addSection = [&sections](std::uint32_t id, const char* name,
                                  std::vector<std::uint8_t> payload) {
        Section s;
        s.id = id;
        s.name = name;
        s.payload = std::move(payload);
        sections.push_back(std::move(s));
        return &sections.back();
    };
    addSection(0,  "",                  {});                                        // 空占位
    {   // FileDepList（字体依赖）：真实文件也常见「有声明、无页」的空记录
        Section& s = *addSection(8, "AcDb:FileDepList", {});
        s.declaredSize = 8;
        s.raw = true;
        s.pageSize = 0x80;
    }
    {   // AppInfo（产品信息，XML 串表，原文页 0x80）
        Section& s = *addSection(11, "AcDb:AppInfo", buildAppInfoPayload());
        s.raw = true;
        s.pageSize = 0x80;
    }
    {   // Preview（缩略图容器，原文单页）：描述符含**绝对**文件偏移，
        // 载荷先以基址 0 占位，待下方试算出页载荷地址后重建。
        Section& s = *addSection(4, "AcDb:Preview", buildPreviewContainer(0, previewDib));
        s.raw = true;
        s.pageSize = static_cast<std::uint32_t>(alignUp(s.payload.size(), kAlign));
    }
    {   // SummaryInfo（图形属性，原文页 0x100）
        Section& s = *addSection(2, "AcDb:SummaryInfo", buildSummaryInfoPayload());
        s.raw = true;
        s.pageSize = 0x100;
    }
    addSection(3,  "AcDb:RevHistory",   buildRevHistoryPayload());
    addSection(12, "AcDb:AcDbObjects",  std::move(objectsPayload));
    addSection(5,  "AcDb:ObjFreeSpace", buildObjFreeSpacePayload(sd.nextHandle, sd.tdDay, sd.tdMsec));
    addSection(1,  "AcDb:Template",     buildTemplatePayload(sd.measurement));
    addSection(7,  "AcDb:Handles",      std::move(handlesPayload));
    addSection(9,  "AcDb:Classes",      std::move(classesPayload));
    addSection(10, "AcDb:AuxHeader",    buildAuxHeaderPayload(sd.tdDay, sd.tdMsec, sd.nextHandle, internalVer, r2018));
    addSection(6,  "AcDb:Header",       std::move(headerPayload));

    // ---- 1.5 为预览容器定址并重建其载荷 ------------------------------------------
    // 预览容器的图片描述符记录的是**绝对**文件偏移，而数据页头里的 adler 校验
    // 覆盖页的最终字节——地址必须在写页之前确定，事后回填会让校验失配。
    // 预览之前的段（空段 / 原文页 / 压缩页）都不受预览影响，用同一套分页规则
    // 试算一次即可得到与正式装配完全一致的地址（同一份载荷、同一个压缩器，
    // 且容器长度与描述符里的地址取值无关，故两次装配的页几何完全相同）。
    {
        std::vector<std::uint8_t> probe(kFileHeaderSize, 0);
        std::vector<PageInfo> probePages;
        probePages.push_back(PageInfo{ 0, 0 });
        for (Section& s : sections) {
            if (s.id == kPreviewSectionId) {
                const std::uint64_t base =
                    static_cast<std::uint64_t>(probe.size()) + kDataPageHeader;
                s.payload = buildPreviewContainer(base, previewDib);
                s.pageSize = static_cast<std::uint32_t>(alignUp(s.payload.size(), kAlign));
                break;
            }
            std::vector<std::uint32_t> probeIds;
            writeDataPages(probe, probePages, s.payload, s.id, probeIds, s.raw, s.pageSize);
        }
    }

    // ---- 2. 写数据页 ------------------------------------------------------------
    std::vector<std::uint8_t> out(kFileHeaderSize, 0);
    std::vector<PageInfo> pages;               // 1-based（下标 0 占位）
    pages.push_back(PageInfo{ 0, 0 });

    for (Section& s : sections) {
        writeDataPages(out, pages, s.payload, s.id, s.pageIds, s.raw, s.pageSize);
    }
    const std::size_t dataPageCount = pages.size() - 1;

    // 外层头寻址字段：指向各段首页数据的起点（页头之后 0x20）
    std::uint32_t previewAddr = 0, summaryInfoAddr = 0, appInfoAddr = 0;
    auto firstPageDataAddr = [&pages](const Section& s) -> std::uint32_t {
        if (s.pageIds.empty()) return 0;
        return static_cast<std::uint32_t>(pages[s.pageIds.front()].address) + 0x20;
    };
    for (const Section& s : sections) {
        if (s.name == "AcDb:Preview")     previewAddr     = firstPageDataAddr(s);
        else if (s.name == "AcDb:SummaryInfo") summaryInfoAddr = firstPageDataAddr(s);
        else if (s.name == "AcDb:AppInfo")     appInfoAddr     = firstPageDataAddr(s);
    }

    // ---- 3. 段映射页（0x4163003b） ----------------------------------------------
    const std::uint32_t sectionMapId = static_cast<std::uint32_t>(dataPageCount) + 1;
    const std::uint32_t pageMapId    = static_cast<std::uint32_t>(dataPageCount) + 2;
    std::uint64_t pageMapAddress = 0;
    {
        std::vector<std::uint8_t> sm;
        putU32(sm, static_cast<std::uint32_t>(sections.size()));
        putU32(sm, 2);
        putU32(sm, static_cast<std::uint32_t>(kMaxPageSize));
        putU32(sm, 0);
        putU32(sm, static_cast<std::uint32_t>(sections.size()));
        for (const Section& s : sections) {
            const std::uint64_t declared = s.declaredSize != 0
                ? s.declaredSize : static_cast<std::uint64_t>(s.payload.size());
            putU64(sm, declared);
            putU32(sm, static_cast<std::uint32_t>(s.pageIds.size()));
            putU32(sm, s.pageSize);              // maxSize = 本段页解压尺寸
            putU32(sm, 1);
            putU32(sm, s.raw ? 1 : 2);           // compressed code（1 = 原文，2 = LZ77）
            putU32(sm, s.id);                    // 规范段号
            putU32(sm, 0);                       // encrypted
            std::uint8_t nameBuf[64];
            std::memset(nameBuf, 0, sizeof(nameBuf));
            const std::size_t n = std::min<std::size_t>(63, s.name.size());
            std::memcpy(nameBuf, s.name.data(), n);
            sm.insert(sm.end(), nameBuf, nameBuf + 64);
            for (std::uint32_t pn : s.pageIds) {
                const PageInfo& pi = pages[pn];
                putU32(sm, pn);
                putU32(sm, pi.compressedSize);            // 数据尺寸（压缩后）
                // 解压起始偏移：读取端 parseDataPage 用它把本页解压到
                // objData 的对应位置（`objData + pi.startOffset`），缺省 0 会让
                // 所有页互相覆盖、只剩最后一页的内容。
                putU64(sm, pi.startOffset);
            }
        }
        writeSystemPage(out, pages, kPageTypeSecMap, sm, nullptr);
    }

    // ---- 4. 页映射页（0x41630e3b） ----------------------------------------------
    // 真实文件的页映射页把「自身」也登记为最后一个条目。页尺寸只依赖
    // 压缩后的字节数，条目本身恒 8 字节，故先用占位尺寸压缩求页长，再回填。
    {
        pageMapAddress = static_cast<std::uint64_t>(out.size());
        std::vector<std::uint8_t> pm;
        for (std::size_t i = 1; i < pages.size(); ++i) {
            putU32(pm, pages[i].number);
            putU32(pm, static_cast<std::uint32_t>(pages[i].size));
        }
        putU32(pm, pageMapId);
        putU32(pm, 0);                           // 自身页长占位
        {
            Compressor probe;
            const std::size_t cSize = probe.compress(pm.data(), pm.size()).size();
            const std::size_t selfSize = alignUp(kDataPageHeader + cSize, kAlign);
            putU32At(pm, pm.size() - 4, static_cast<std::uint32_t>(selfSize));
        }
        writeSystemPage(out, pages, kPageTypePageMap, pm, nullptr);
    }

    const std::uint32_t lastPageId = static_cast<std::uint32_t>(pages.size() - 1);
    // 所有页尺寸之和 = 页映射页末址 - 0x100（真实文件即如此记录 lastPageEnd）
    const std::uint64_t lastPageEnd =
        static_cast<std::uint64_t>(out.size()) - kFileHeaderSize;
    // 文件尾的第二头部（0x6C 字节内层头副本）紧跟页映射页之后
    const std::uint64_t secondHeaderAddress = static_cast<std::uint64_t>(out.size());

    // ---- 5. 内层头（0x80）与文件头 -------------------------------------------------
    std::vector<std::uint8_t> inner(kInnerHeaderSize, 0);
    std::memcpy(inner.data(), "AcFssFcAJMB", 11);
    putU32At(inner, 0x0C, 0);
    putU32At(inner, 0x10, static_cast<std::uint32_t>(kInnerHeaderSize));
    putU32At(inner, 0x14, 4);
    putU32At(inner, 0x18, 0);
    putU32At(inner, 0x1C, 0);
    putU32At(inner, 0x20, 0);
    putU32At(inner, 0x24, 1);
    putU32At(inner, 0x28, lastPageId);
    putU64At(inner, 0x2C, lastPageEnd);             // 各页尺寸之和（相对 0x100 的末址）
    putU64At(inner, 0x34, secondHeaderAddress);     // 文件尾第二头部地址
    putU32At(inner, 0x3C, 0);                       // gap amount
    putU32At(inner, 0x40, lastPageId);              // section page amount（页总数）
    putU32At(inner, 0x44, 0x20);
    putU32At(inner, 0x48, 0x80);
    putU32At(inner, 0x4C, 0x40);
    putU32At(inner, 0x50, pageMapId);               // Section Page Map Id
    putU64At(inner, 0x54, pageMapAddress - kFileHeaderSize);
    putU32At(inner, 0x5C, sectionMapId);
    putU32At(inner, 0x60, lastPageId);              // 页映射条目数（含页映射页自身）
    putU32At(inner, 0x64, 0);
    putU32At(inner, 0x68, 0);
    putU32At(inner, 0x68, crc32(inner.data(), inner.size()));

    const std::vector<std::uint8_t> magic = magicSequence(256);
    for (std::size_t i = 0; i < inner.size(); ++i)
        inner[i] ^= magic[i % magic.size()];

    // 文件头 0..0x80
    {
        std::vector<std::uint8_t> meta;
        meta.reserve(kFileHeaderSize - kInnerHeaderSize);
        meta.insert(meta.end(), reinterpret_cast<const std::uint8_t*>(versionStr),
                    reinterpret_cast<const std::uint8_t*>(versionStr) + 6);
        for (int i = 0; i < 5; ++i) meta.push_back(0x00);
        // 维护版本（0x0B）与写出版本（0x11/0x12）。AC1018 沿用本实现既有取值；
        // AC1024 / AC1027 / AC1032 取 libredwg 版本表的权威值
        //（maint_rel_version / dwg_version）：
        //     R2010 → 0x6d / 0x1d      R2013 → 0x7d / 0x1f      R2018 → 0x1d / 0x21
        // 该表同时被 libredwg 的 header.spec 用来解读文件头。
        // 注意 R2018 的 maint_rel_version 回落到 0x1d（仍 > 3，满足
        // 「2010+ 高位尺寸字段」的判定条件）。
        meta.push_back(r2018 ? 0x1d : r2013 ? 0x7d : (r2010 ? 0x6d : 0x00));   // maintenance version
        meta.push_back(0x03);                       // byte 0x0C
        putU32(meta, previewAddr);                  // 预览图数据地址（页头后 0x20）
        meta.push_back(r2018 ? 0x21 : r2013 ? 0x1f : (r2010 ? 0x1d : 0x21));   // app dwg version
        meta.push_back(r2018 ? 0x21 : r2013 ? 0x1f : (r2010 ? 0x1d : 0x00));   // app maintenance version
        putU16(meta, sd.codepageId);                // codepage
        for (int i = 0; i < 3; ++i) meta.push_back(0x00);
        putU32(meta, 0);                            // security flags
        putU32(meta, 0);                            // unknown
        putU32(meta, summaryInfoAddr);              // 图形属性数据地址
        putU32(meta, 0);                            // VBA address
        putU32(meta, 0x80);                         // 0x28 常量
        putU32(meta, appInfoAddr);                  // 产品信息数据地址
        while (meta.size() < kFileHeaderSize - kInnerHeaderSize - 20)
            meta.push_back(0x00);                   // 0x30 起 80 字节 0
        std::copy(meta.begin(), meta.end(), out.begin());

        std::copy(inner.begin(), inner.end(), out.begin() + 0x80);
        for (int i = 0; i < 20; ++i)
            out[0xEC + i] = magic[236 + i];
    }

    // ---- 6. 第二头部（0x6C 字节内层头副本，真实 R2004 文件必有） -------------------
    // 地址已在内层头 0x34 处登记；内容为加密后的内层头逐字节副本。
    out.resize(static_cast<std::size_t>(secondHeaderAddress) + kInnerHeaderSize, 0);
    std::memcpy(out.data() + secondHeaderAddress, inner.data(), kInnerHeaderSize);

    // ---- 7. 落盘 ----------------------------------------------------------------
    std::ofstream file(sd.fileName.c_str(), std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
    if (!file.is_open()) return false;
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    const bool ok = file.good();
    file.close();
    return ok;
}

} // namespace dwgWriter18
