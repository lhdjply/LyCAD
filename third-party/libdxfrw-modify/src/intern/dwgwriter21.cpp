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
//!  dwgwriter21.cpp —— DWG **R2007 (AC1021)** 写出实现（RS + 分页容器）。
//!
//!  文件布局（逐层与读取端 dwgReader21 对偶，并与两份真实 R2007 文件
//!  ——OpenCADStudio 与浩辰 CAD 各写的一份——逐结构核对过）
//!    0x0000  0x80 字节元数据块：版本串、维护版本、预览/摘要/产品信息地址…
//!    0x0080  0x400 字节文件头页：
//!              [0, 0x2FD)  RS(255,239) × 3 交织编码的 717 字节前置块
//!              [0x2FD,0x3D8) 随机补长
//!              [0x3D8,0x400) 0x28 字节校验数据（两个 CRC-64 + 随机数 + 编码种子）
//!              前置块 = [检查序列 CRC][检查序列值][压缩数据 CRC][i32 长度][i32 0]
//!                       [0x110 字节压缩元数据]（块按 8 字节对齐后重复填满 717 字节）
//!    0x0480  数据页（各节按规范页尺寸切页，逐页 LZ77 + RS(255,251) 交织）
//!            → 段映射页（系统页）→ 段映射副本 → 页映射页 → 页映射副本
//!    末尾    文件头页副本（0x400 字节）
//!
//!  页地址由页映射页按「文件顺序累加」得出（读取端从 0x480 起累加各页尺寸），
//!  因此各页必须首尾相接、页尺寸取 0x20 的倍数（0x480 即数据区起点）。
//!
//!  与 R2004 分页容器的三点本质差别
//!    · 所有页都做 Reed-Solomon 纠错：系统页（页映射/段映射/文件头）用
//!      RS(255,239)（pp=0x96, tt=8；码字按 factor 交织），数据页用 RS(255,251)
//!      （pp=0xB8, tt=2）。实测真实 R2007 文件与本工程读取端使用的 RScodec
//!      逐位一致（参考实现注释里的「FCR=k」与 Rockliff 的「FCR=1」只是多项式
//!      数组方向约定不同，同一套码）。
//!    · 压缩流是 AC1021 变体（操作码高 4 位定指令类型，字面量按长度做字节重排），
//!      与 R2004 的 LZ77 不通用。本实现见下「压缩策略」。
//!    · 元数据/页 CRC 改用 CRC-64（正常式 + 镜像式两套），并有 CRC 随机编码器
//!      （LCG + MT 混合）产生种子字段。
//!
//!  压缩策略（与参考实现的差异，均以「读取端能按规范读回」为前提）
//!    · **只发字面量**的合法 AC1021 流：每段 ≤22 字节的字面量 run，run 之间用
//!      长度为 0 的 0x2_ 匹配操作码隔开（读取端的指令语法允许；参考实现同样
//!      承认字面量拷贝会写超出复制范围，故读取端留有溢出余量）。压缩比≈1，
//!      真正的匹配搜索压缩尚未实现——只影响体积，不影响可读性。
//!    · 字面量 run 长度避开 6 / 11：这两个长度的拷贝分支在实现里分别会
//!      多读一个字节 / 产生重复字节（缺 break 的直落），无法表达任意数据。
//!      11 字节的页用「8 字节 run + 尾随 3 字面量」表达。
//!
//!  参考实现：opencadcodec 的 file_headers/file_header_ac21.rs、dwg21_metadata.rs、
//!  crc.rs、reed_solomon.rs、section_definition.rs（仅作格式参考，本文件为独立
//!  实现，不引用其代码）。
//!
#include "dwgwriter21.h"

#include "dwgwriter.h"          // dwgCalcCrc16 / dwgSentinelBytes
#include "rscodec.h"            // RScodec（RS(255,239) / RS(255,251)）
#include "../libdwgrw.h"        // dwgW / dwgW::SectionData / buildSections

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

// -----------------------------------------------------------------------------
//  常量
// -----------------------------------------------------------------------------
constexpr std::size_t  kMetadataBlockSize = 0x80;    // 0x00 元数据块
constexpr std::size_t  kFileHeaderPageSize = 0x400;  // 0x80 文件头页
constexpr std::size_t  kReservedHeaderSize = 0x480;  // 数据区起点
constexpr std::size_t  kCheckDataSize = 0x28;        // 文件头页尾部校验数据
constexpr std::size_t  kRsDataInHeader = kFileHeaderPageSize - kCheckDataSize;  // 0x3D8
constexpr std::size_t  kFileHeaderRsFactor = 3;      // 文件头页 RS 交织份数
constexpr std::size_t  kAlign = 0x20;                // 页对齐粒度
constexpr std::size_t  kCrcBlock = 8;                // CRC 块对齐
constexpr std::size_t  kMinSystemPageSize = 0x400;   // 系统页最小尺寸
constexpr std::size_t  kRsN = 255;
constexpr std::size_t  kRsDataK = 251;               // 数据页 RS(255,251)
constexpr std::size_t  kRsSystemK = 239;             // 系统页 / 文件头 RS(255,239)
constexpr std::size_t  kMetadataSize = 0x110;        // 压缩元数据 34×u64
constexpr std::uint64_t kFileRandomSeed = 0;         // 元数据 RandomSeed（真实文件为 0）

// 段描述（规范 §5.2 段表）：哈希、规范页尺寸、编码（1=原文页，4=LZ77 页）。
// 顺序即段映射顺序（读取端按名字取用，不依赖顺序；真实文件即此序）。
struct SectionSpec {
    std::uint32_t id;          // ACAD 规范段号（与 R2004 相同的编号体系）
    const char*   name;
    std::uint64_t hash;
    std::uint32_t pageSize;
    bool          stored;      // true = encoding 1（原文 + RS 校验）
    bool          singlePage;  // true = 整段一页（预览图的绝对偏移要求数据连续）
};
const SectionSpec kSections[] = {
    {  8, "AcDb:FileDepList", 0x6C4205CAull, 0x100,  true,  false },
    { 11, "AcDb:AppInfo",     0x3FA0043Eull, 0x300,  true,  false },
    {  4, "AcDb:Preview",     0x40AA0473ull, 0x400,  true,  true  },
    {  2, "AcDb:SummaryInfo", 0x717A060Full, 0x80,   true,  false },
    {  3, "AcDb:RevHistory",  0x60A205B3ull, 0x1000, false, false },
    { 12, "AcDb:AcDbObjects", 0x674C05A9ull, 0xF800, false, false },
    {  5, "AcDb:ObjFreeSpace",0x77E2061Full, 0xF800, false, false },
    {  1, "AcDb:Template",    0x4A1404CEull, 0x400,  false, false },
    {  7, "AcDb:Handles",     0x3F6E0450ull, 0xF800, false, false },
    {  9, "AcDb:Classes",     0x3F54045Full, 0xF800, false, false },
    { 10, "AcDb:AuxHeader",   0x54F0050Aull, 0x800,  false, false },
    {  6, "AcDb:Header",      0x32B803D9ull, 0x800,  false, false },
};

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
void putU16At(std::vector<std::uint8_t>& v, std::size_t off, std::uint16_t x)
{
    for (int i = 0; i < 2; ++i) v[off + i] = static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF);
}
void putU32At(std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x)
{
    for (int i = 0; i < 4; ++i) v[off + i] = static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF);
}
void putU64At(std::vector<std::uint8_t>& v, std::size_t off, std::uint64_t x)
{
    for (int i = 0; i < 8; ++i) v[off + i] = static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF);
}
void appendSentinel(std::vector<std::uint8_t>& v, int kind, bool begin)
{
    std::uint8_t s[16];
    dwgSentinelBytes(kind, begin, s);
    v.insert(v.end(), s, s + 16);
}
std::size_t alignUp(std::size_t v, std::size_t boundary)
{
    return (v + boundary - 1) / boundary * boundary;
}

// -----------------------------------------------------------------------------
//  CRC-64（ECMA-182 的两套用法）
//
//  正常式（MSB 先）：用于文件头元数据 CRC、压缩数据 CRC、检查数据 CRC；
//  镜像式（LSB 先，反射）：用于系统页 / 数据页 CRC。
//  两者都先按规范 §5.12 做「每 8 字节内 16 位字倒序」的重排，再由
//  UpdateSeed1/2 依据数据长度推导初值。
// -----------------------------------------------------------------------------
const std::uint64_t* crc64TableNormal()
{
    static std::uint64_t table[256];
    static bool init = false;
    if (!init) {
        const std::uint64_t poly = 0x42F0E1EBA9EA3693ull;
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint64_t c = static_cast<std::uint64_t>(i) << 56;
            for (int k = 0; k < 8; ++k)
                c = (c & (1ull << 63)) ? ((c << 1) ^ poly) : (c << 1);
            table[i] = c;
        }
        init = true;
    }
    return table;
}
const std::uint64_t* crc64TableReflected()
{
    static std::uint64_t table[256];
    static bool init = false;
    if (!init) {
        // 镜像式多项式：**不是** 0x42F0E1EBA9EA3693 的位反（0xC96C5795D7870F42 是
        // CRC-64/XZ 那套），DWG 用的是 0x95AC9329AC4BC9B5。二者算出的表完全不同
        //（表[1] 分别为 0x7AD870C830358979 与 0xB32E4CBE03A75F6F），用错会让
        // 每一页的 CRC、页映像/段映像 CRC、文件头校验数据里的镜像 CRC 全部错值。
        // 本读取器不校验这些值，所以自家往返看不出问题；AutoCAD/浩辰会校验，
        // 表现就是「文件损坏」。
        const std::uint64_t poly = 0x95AC9329AC4BC9B5ull;
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint64_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? ((c >> 1) ^ poly) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    return table;
}
std::uint64_t crc64Normal(std::uint64_t seed, const std::uint8_t* d, std::size_t n)
{
    const std::uint64_t* t = crc64TableNormal();
    std::uint64_t crc = seed;
    for (std::size_t i = 0; i < n; ++i)
        crc = t[(d[i] ^ (crc >> 56)) & 0xFF] ^ (crc << 8);
    return crc;
}
std::uint64_t crc64Reflected(std::uint64_t seed, const std::uint8_t* d, std::size_t n)
{
    const std::uint64_t* t = crc64TableReflected();
    std::uint64_t crc = seed;
    for (std::size_t i = 0; i < n; ++i)
        crc = (crc >> 8) ^ t[(crc ^ d[i]) & 0xFF];
    return crc;
}
/// 规范 §5.12 的字节重排：整 8 字节块按 [6,7,4,5,2,3,0,1]，尾部 4 字节按 [2,3,0,1]
/// 其余顺序。
void reorderForCrc(const std::uint8_t* d, std::size_t n, std::vector<std::uint8_t>& out)
{
    out.clear();
    out.reserve(n);
    const std::size_t full = n / 8;
    for (std::size_t b = 0; b < full; ++b) {
        const std::uint8_t* p = d + b * 8;
        out.insert(out.end(), { p[6], p[7], p[4], p[5], p[2], p[3], p[0], p[1] });
    }
    const std::uint8_t* r = d + full * 8;
    switch (n % 8) {
    case 0: break;
    case 1: out.push_back(r[0]); break;
    case 2: out.insert(out.end(), { r[0], r[1] }); break;
    case 3: out.insert(out.end(), { r[0], r[1], r[2] }); break;
    case 4: out.insert(out.end(), { r[2], r[3], r[0], r[1] }); break;
    case 5: out.insert(out.end(), { r[2], r[3], r[0], r[1], r[4] }); break;
    case 6: out.insert(out.end(), { r[2], r[3], r[0], r[1], r[4], r[5] }); break;
    default: out.insert(out.end(), { r[2], r[3], r[0], r[1], r[4], r[5], r[6] }); break;
    }
}
/// ODA UpdateSeed2：文件头 / 压缩数据 CRC 的动态初值（结尾取反）
std::uint64_t updateSeed2(std::uint64_t seed, std::uint32_t len)
{
    const std::uint64_t l = len;
    std::uint64_t s = (seed + l) * 0x343FDull + 0x269EC3ull;
    s = s * 0x1000343FDull + (l + 0x269EC3ull);
    return ~s;
}
/// ODA UpdateSeed1：系统页 / 数据页 CRC 的动态初值（结尾取反）
std::uint64_t updateSeed1(std::uint64_t seed, std::uint32_t len)
{
    const std::uint64_t l = len;
    std::uint64_t s = (seed + l) * 0x343FDull + 0x269EC3ull;
    s |= s * (0x343FDull << 32) + (0x269EC3ull << 32);
    return ~s;
}
std::uint64_t ac21NormalCrc64(std::uint64_t seed, std::uint32_t len,
                              const std::uint8_t* d, std::size_t n)
{
    std::vector<std::uint8_t> r;
    reorderForCrc(d, n, r);
    return ~crc64Normal(updateSeed2(seed, len), r.data(), r.size());
}
std::uint64_t ac21NormalCrc64Seed1(std::uint64_t seed, std::uint32_t len,
                                   const std::uint8_t* d, std::size_t n)
{
    std::vector<std::uint8_t> r;
    reorderForCrc(d, n, r);
    return ~crc64Normal(updateSeed1(seed, len), r.data(), r.size());
}
std::uint64_t ac21MirroredCrc64(std::uint64_t seed, std::uint32_t len,
                                const std::uint8_t* d, std::size_t n)
{
    std::vector<std::uint8_t> r;
    reorderForCrc(d, n, r);
    return crc64Reflected(updateSeed1(seed, len), r.data(), r.size());
}
std::uint64_t checkDataNormalCrc64(std::uint64_t random2, const std::uint8_t* d, std::size_t n)
{
    std::vector<std::uint8_t> r;
    reorderForCrc(d, n, r);
    return ~crc64Normal(~random2, r.data(), r.size());
}
std::uint64_t checkDataMirroredCrc64(std::uint64_t random1, const std::uint8_t* d, std::size_t n)
{
    std::vector<std::uint8_t> r;
    reorderForCrc(d, n, r);
    return crc64Reflected(~random1, r.data(), r.size());
}
/// 文件头元数据 CRC-64（规范 §5.2.1.2）：0x108 处的 CRC 字段先清零
std::uint64_t headerCrc64(const std::vector<std::uint8_t>& meta)
{
    std::vector<std::uint8_t> buf(meta.begin(), meta.begin() + kMetadataSize);
    std::fill(buf.begin() + 0x108, buf.begin() + 0x110, 0);
    std::vector<std::uint8_t> r;
    reorderForCrc(buf.data(), buf.size(), r);
    return ~crc64Normal(updateSeed2(0, static_cast<std::uint32_t>(kMetadataSize)),
                        r.data(), r.size());
}
/// 页校验（规范 §5.4.1）：修改版 Adler-32，8 字节块内按 [6,7,4,5,2,3,0,1] 取字节
std::uint32_t pageChecksum(std::uint32_t seed, const std::uint8_t* d, std::size_t n)
{
    const std::uint64_t s64 = (static_cast<std::uint64_t>(seed) + n) * 0x343FDull + 0x269EC3ull;
    std::uint32_t sum1 = static_cast<std::uint32_t>(s64 & 0xFFFF);
    std::uint32_t sum2 = static_cast<std::uint32_t>((s64 >> 16) & 0xFFFF);
    constexpr std::uint32_t kChunk = 0x15B0;
    constexpr std::uint32_t kMod = 0xFFF1;
    std::size_t off = 0;
    while (off < n) {
        const std::size_t end = std::min(off + kChunk, n);
        const std::uint8_t* c = d + off;
        const std::size_t len = end - off;
        std::size_t i = 0;
        static const int kOrder[8] = { 6, 7, 4, 5, 2, 3, 0, 1 };
        while (i + 8 <= len) {
            for (int k = 0; k < 8; ++k) { sum1 += c[i + kOrder[k]]; sum2 += sum1; }
            i += 8;
        }
        const std::size_t rem = len - i;
        const std::uint8_t* r = c + i;
        if (rem >= 4) {
            static const int kOrder4[4] = { 2, 3, 0, 1 };
            for (int k = 0; k < 4; ++k) { sum1 += r[kOrder4[k]]; sum2 += sum1; }
            for (std::size_t j = 4; j < rem; ++j) { sum1 += r[j]; sum2 += sum1; }
        } else {
            for (std::size_t j = 0; j < rem; ++j) { sum1 += r[j]; sum2 += sum1; }
        }
        sum1 %= kMod;
        sum2 %= kMod;
        off = end;
    }
    return (sum2 << 16) | (sum1 & 0xFFFF);
}

// -----------------------------------------------------------------------------
//  CRC 随机编码器（规范 §5.11）
//
//  LCG 初始化表 0/1，其后按 MT 初始化 2..623；InitPadding 先消费 128 项；
//  Encode 把一个 10 位值按固定位位置散布进随机 64 位值中（其余位保留随机性），
//  FillRandom 用填充表产生「看起来随机」的补长字节。
//  文件头元数据的 pages_map_crc_seed / sections_map_crc_seed / crc_seed_encoded
//  与 0x28 校验数据都由它按固定调用次序产生——次序错一位，值就全不一样。
// -----------------------------------------------------------------------------
class CrcRandomEncoder
{
public:
    explicit CrcRandomEncoder(std::uint64_t seed)
    {
        m_table.resize(0x270);
        m_table[0] = static_cast<std::uint32_t>(seed) * 0x343FDu + 0x269EC3u;
        m_table[1] = static_cast<std::uint32_t>(seed >> 32) * 0x343FDu + 0x269EC3u;
        std::uint32_t value = m_table[1];
        for (std::size_t i = 2; i < 0x270; ++i) {
            value = ((value >> 30) ^ value) * 0x6C078965u + static_cast<std::uint32_t>(i);
            m_table[i] = value;
        }
        m_padding.resize(0x80);
        for (std::size_t i = 0; i < 0x80; ++i) {
            updateIndex();
            m_padding[i] = m_table[m_index];
            ++m_index;
        }
    }
    std::uint64_t nextU64()
    {
        m_index += 2;
        updateIndex();
        const std::uint64_t lo = m_table[m_index];
        const std::uint64_t hi = m_table[m_index + 1 < 0x270 ? m_index + 1 : 0];
        return lo | (hi << 32);
    }
    std::uint64_t encode(std::uint32_t value)
    {
        const std::uint64_t random = nextU64();
        std::uint32_t lo = static_cast<std::uint32_t>(random) & 0xDF7DF7DFu;
        std::uint32_t hi = static_cast<std::uint32_t>(random >> 32) & 0xF7DF7DF7u;
        if (value & 0x200) lo |= 0x20;
        if (value & 0x100) lo |= 0x800;
        if (value & 0x080) lo |= 0x20000;
        if (value & 0x040) lo |= 0x800000;
        if (value & 0x020) lo |= 0x20000000;
        if (value & 0x010) hi |= 0x08;
        if (value & 0x008) hi |= 0x200;
        if (value & 0x004) hi |= 0x8000;
        if (value & 0x002) hi |= 0x200000;
        if (value & 0x001) hi |= 0x8000000;
        return static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
    }
    std::uint64_t encodeSeed(std::uint64_t seed) { return encode(static_cast<std::uint32_t>(seed)); }
    void fillRandom(std::uint8_t* out, std::size_t n)
    {
        std::size_t pad = 0;
        std::size_t i = 0;
        while (i + 4 <= n) {
            std::uint32_t v;
            if (pad < m_padding.size()) {
                v = m_padding[pad++];
            } else {
                ++m_index;
                updateIndex();
                v = m_table[m_index];
            }
            for (int k = 0; k < 4; ++k) out[i + k] = static_cast<std::uint8_t>((v >> (8 * k)) & 0xFF);
            i += 4;
        }
        while (i < n) {
            std::uint32_t v = (pad < m_padding.size()) ? m_padding[pad++] : 0u;
            out[i++] = static_cast<std::uint8_t>(v & 0xFF);
        }
    }

private:
    void updateIndex()
    {
        if (m_index < 0x270) return;
        for (std::size_t i = 0; i < 0x270; ++i) {
            const std::uint32_t y = (m_table[i] & 0x80000000u)
                | (m_table[(i + 1) % 0x270] & 0x7FFFFFFFu);
            std::uint32_t v = m_table[(i + 0x18D) % 0x270] ^ (y >> 1);
            if (y & 1) v ^= 0x9908B0DFu;
            m_table[i] = v;
        }
        m_index = 0;
    }
    std::vector<std::uint32_t> m_table;
    std::vector<std::uint32_t> m_padding;
    std::size_t m_index = 0;
};

// -----------------------------------------------------------------------------
//  Reed-Solomon 交织编码
//
//  与读取端的 dwgRSCodec::decode239I / decode251I 严格对偶：文件里第
//  (i + j*factor) 字节是第 i 个码字的第 j 个符号（j<K 为数据，其余为校验）。
// -----------------------------------------------------------------------------
std::vector<std::uint8_t> rsEncodeInterleaved(const std::vector<std::uint8_t>& data,
                                              std::size_t factor, std::size_t k,
                                              unsigned pp, int tt)
{
    RScodec rsc(pp, 8, tt);
    const std::size_t parity = kRsN - k;
    std::vector<std::uint8_t> out(factor * kRsN, 0);
    std::vector<unsigned char> block(k, 0), par(parity, 0);
    for (std::size_t b = 0; b < factor; ++b) {
        std::fill(block.begin(), block.end(), 0);
        const std::size_t src = b * k;
        for (std::size_t i = 0; i < k && src + i < data.size(); ++i)
            block[i] = data[src + i];
        std::fill(par.begin(), par.end(), 0);
        rsc.encode(block.data(), par.data());
        for (std::size_t j = 0; j < k; ++j)
            out[b + j * factor] = block[j];
        for (std::size_t j = 0; j < parity; ++j)
            out[b + (k + j) * factor] = par[j];
    }
    return out;
}

/// encoding=1（原文页）的 RS 布局：前 align8(len) 字节是**连续**的原文
///（预览图的绝对偏移因此有效），其后逐符号逐块追加校验。
/// 数据不足 factor*k 的部分由 CRC 随机编码器按文件的 RandomSeed 填充（不落盘）。
std::vector<std::uint8_t> rsEncodeStored(const std::vector<std::uint8_t>& data,
                                         std::uint64_t randomSeed)
{
    const std::size_t aligned = alignUp(data.size(), kCrcBlock);
    const std::size_t factor = (aligned + kRsDataK - 1) / kRsDataK;
    std::vector<std::uint8_t> padded(factor * kRsDataK, 0);
    std::copy(data.begin(), data.end(), padded.begin());
    // 填充约定（与规范 §5.4/§5.13.1 及参考实现一致）：
    //   [data.len(), align8(len)) —— **补零**，这段会落盘（页内前 align8(len) 字节）；
    //   [align8(len), factor*251) —— 由 RandomSeed 派生的随机填充，**不落盘**，
    //                               只参与最后一个码字的校验字节计算。
    // 两者若互换，最后一个码字的校验字节与 AutoCAD 的重算结果不符。
    if (aligned < padded.size()) {
        CrcRandomEncoder rng(randomSeed);
        rng.fillRandom(padded.data() + aligned, padded.size() - aligned);
    }
    RScodec rsc(0xB8, 8, 2);
    std::vector<std::uint8_t> out(padded.begin(), padded.begin() + aligned);
    std::vector<unsigned char> block(kRsDataK, 0), par(kRsN - kRsDataK, 0);
    for (std::size_t b = 0; b < factor; ++b) {
        std::copy(padded.begin() + b * kRsDataK,
                  padded.begin() + (b + 1) * kRsDataK, block.begin());
        std::fill(par.begin(), par.end(), 0);
        rsc.encode(block.data(), par.data());
        // 校验按「符号优先、块次之」排列（读取端 encoding=1 页的布局）
        for (std::size_t j = 0; j < par.size(); ++j)
            out.push_back(par[j]);
    }
    return out;
}

// -----------------------------------------------------------------------------
//  AC1021 LZ77 的「只发字面量」编码
//
//  语法（读取端 dwgUtil.cpp::decompress21 / readInstructions21 / litLength21）：
//    [字面量 opcode 0x00..0x0E] → 长度 = opcode + 8（8..22）
//    [0x20 00 00 00]            → 长度 0 的 0x2_ 匹配（分隔用；尾随字面量 0）
//    [0x20 00 00 0r] + r 字节   → 长度 0 的匹配 + r 个尾随字面量（1..7）
//    首字节高 4 位为 2 时，读取端走「起始特例」：跳过 2 字节后取 3 位长度（1..7）
//
//  字面量在流内按长度重排（读取端 copyCompBytes21 的长度分支）：
//  out[d] = in[perm[L][d]]，故写入时按 perm 反查落位。长度 6 的分支会多读一个
//  字节（见文件头注释），故该情形要备足 L+1 个字节并把 payload[5] 落到下标 6。
// -----------------------------------------------------------------------------
const std::vector<int>& literalPerm(int len)
{
    static std::vector<std::vector<int>> table;
    if (table.empty()) {
        table.resize(23);
        auto seq = [](std::initializer_list<int> l) { return std::vector<int>(l); };
        table[1]  = seq({0});
        table[2]  = seq({1,0});
        table[3]  = seq({2,1,0});
        table[4]  = seq({0,1,2,3});
        table[5]  = seq({4,0,1,2,3});
        // 注意 case 6 的末字节读的是 six+0（不是 six+6）：写成后者会把该字节
        // 留 0，解出的数据少一个字节，页校验和随之不符（浩辰会判「文件损坏」）。
        table[6]  = seq({5,1,2,3,4,0});
        table[7]  = seq({6,5,1,2,3,4,0});
        table[8]  = seq({0,1,2,3,4,5,6,7});
        table[9]  = seq({8,0,1,2,3,4,5,6,7});
        table[10] = seq({9,1,2,3,4,5,6,7,8,0});
        table[11] = seq({10,9,1,2,3,4,5,6,7,8,0});          // 本写出器不使用
        table[12] = seq({8,9,10,11,0,1,2,3,4,5,6,7});
        table[13] = seq({12,8,9,10,11,0,1,2,3,4,5,6,7});
        table[14] = seq({13,9,10,11,12,1,2,3,4,5,6,7,8,0});
        table[15] = seq({14,13,9,10,11,12,1,2,3,4,5,6,7,8,0});
        table[16] = seq({8,9,10,11,12,13,14,15,0,1,2,3,4,5,6,7});
        table[17] = seq({9,10,11,12,13,14,15,16,8,0,1,2,3,4,5,6,7});
        table[18] = seq({17,9,10,11,12,13,14,15,16,1,2,3,4,5,6,7,8,0});
        table[19] = seq({18,17,16,8,9,10,11,12,13,14,15,0,1,2,3,4,5,6,7});
        table[20] = seq({16,17,18,19,8,9,10,11,12,13,14,15,0,1,2,3,4,5,6,7});
        table[21] = seq({20,16,17,18,19,8,9,10,11,12,13,14,15,0,1,2,3,4,5,6,7});
        table[22] = seq({21,20,16,17,18,19,8,9,10,11,12,13,14,15,0,1,2,3,4,5,6,7});
    }
    return table[static_cast<std::size_t>(len)];
}

/// 把 payload 的一段（长度 len ∈ 1..22，len ≠ 11 或为整段）按重排写入流
void emitLiteralRun(std::vector<std::uint8_t>& out, const std::uint8_t* data,
                    std::size_t len, bool withOpcode)
{
    if (withOpcode) {
        out.push_back(static_cast<std::uint8_t>(len - 8));   // 0x00..0x0E
    }
    const std::size_t base = out.size();
    out.resize(base + len, 0);
    const std::vector<int>& p = literalPerm(static_cast<int>(len));
    for (std::size_t d = 0; d < len; ++d)
        out[base + static_cast<std::size_t>(p[d])] = data[d];
}

/// 零长度「分隔」匹配（t=2 形式，4 字节）。它自身不拷贝任何字节，但把控制权交给
/// 其后的 nextLit ∈ 0..6 个字面量；nextLit == 0 时，接下来的字节会被内层循环读到，
/// 若其高 4 位为 0（即下面「独立字面量条目」的长度字节）就退出内层，转由外层按
/// litLength21 读该字节 —— 于是**两个相邻的字面量条目**必须靠它连接。
/// （读取端语义见 dwgCompressor::decompress21 的 t==2 分支与内层循环。）
void emitSeparator(std::vector<std::uint8_t>& out, std::size_t nextLit)
{
    out.push_back(0x20);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(static_cast<std::uint8_t>(nextLit & 0x07));
}

/// 匹配条目（t=1 形式，3 字节）：copyLen ∈ 3..18、offset ∈ 1..8192。
///   字节0 = 0x10 | (copyLen - 3)
///   字节1 = (offset-1) & 0xFF
///   字节2 = (((offset-1) >> 8) << 3) | nextLit
/// 读取端 `so = ((字节2 & 0xF8) << 5) + 1 + 字节1`，即 (字节2>>3)*256 + 字节1 + 1；
/// 字节2 的低 3 位留给**紧随其后**的字面量个数（0 表示后面是另一个条目）。
/// 返回字节2 的下标，便于随后回填 nextLit。
/// 注：刻意不用 t=0/t=2/t=15 三种形式——读取端在「匹配之后」读到高 4 位为 0 或 15
/// 的字节时语义不同（0 表示退出内层、15 会被 &=15 改写），高 4 位为 1 则始终安全。
std::size_t emitMatch(std::vector<std::uint8_t>& out, std::size_t copyLen,
                      std::size_t offset, std::size_t nextLit)
{
    const std::size_t o = offset - 1;
    out.push_back(static_cast<std::uint8_t>(0x10u | (copyLen - 3)));
    out.push_back(static_cast<std::uint8_t>(o & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(((o >> 8) << 3) | (nextLit & 0x07u)));
    return out.size() - 1;
}

/// 把一段字面量写进流。
/// `anchored` 表示上一条目是匹配（其 nextLit 为 0），此时首条字面量条目可直接跟在
/// 匹配之后；否则要先补一条分隔匹配（注意：流**最开始**不能用分隔匹配——读取端的
/// 起始特例会把 0x20 自身当作长度字节，长度会算成 0x28）。故首条字面量传 true。
/// 允许的独立条目长度：8..10、12..22；1..6 用分隔匹配带出；7 与 11 在读取端
/// copyCompBytes21 的分支有缺陷（case 7 缺 break、case 11 缺 dix++），拆分规避。
void emitLiteralSequence(std::vector<std::uint8_t>& out, const std::uint8_t* data,
                         std::size_t len, bool anchored)
{
    std::size_t off = 0;
    bool needSep = !anchored;
    while (off < len) {
        const std::size_t left = len - off;
        if (left > 22) {
            if (needSep) emitSeparator(out, 0);
            emitLiteralRun(out, data + off, 22, /*withOpcode=*/true);
            off += 22;
            needSep = true;
        } else if (left >= 12 || (left >= 8 && left <= 10)) {
            if (needSep) emitSeparator(out, 0);
            emitLiteralRun(out, data + off, left, /*withOpcode=*/true);
            off = len;
        } else if (left >= 1 && left <= 6) {
            emitSeparator(out, left);
            emitLiteralRun(out, data + off, left, /*withOpcode=*/false);
            off = len;
        } else if (left == 7) {
            emitSeparator(out, 1); emitLiteralRun(out, data + off, 1, false);
            emitSeparator(out, 6); emitLiteralRun(out, data + off + 1, 6, false);
            off = len;
        } else {                                   // left == 11
            emitSeparator(out, 6); emitLiteralRun(out, data + off, 6, false);
            emitSeparator(out, 5); emitLiteralRun(out, data + off + 6, 5, false);
            off = len;
        }
    }
}

/// AC1021（R2007）压缩流编码器：贪心 LZ77（窗口 8192、匹配长度 3..18）。
///
/// 这是**必需的**，不是优化：真实 R2007 文件（AutoCAD / 浩辰 CAD / OpenCADStudio
/// 导出）的页与文件头元数据块都是**真压缩**流；浩辰 CAD 只认压缩形式，遇到
/// 「未压缩」标记（文件头元数据块的 compr_len 为负）会直接判「无法打开此图形 /
/// 文件损坏」。此前本函数只发字面量（输出恒大于原文），调用方只能退回未压缩，
/// 于是所有容器页都带上负长度标记 → 浩辰一律拒绝。
std::vector<std::uint8_t> ac21Encode(const std::vector<std::uint8_t>& src)
{
    std::vector<std::uint8_t> out;
    const std::size_t n = src.size();
    if (n == 0)
        return out;
    if (n < 8) {
        // 短于 8 字节无法凑出「起始字面量 ≥ 8」：沿用起始特例
        // （0x20 + 2 字节 + 3 位长度 + 重排数据）。长度 7 会触发读取端缺陷，
        // 但本路径的输入仅 1..7 字节，且调用方在压缩不划算时仍会退回原文。
        out.push_back(0x20); out.push_back(0x00); out.push_back(0x00);
        out.push_back(static_cast<std::uint8_t>(n & 0x07));
        emitLiteralRun(out, src.data(), n, /*withOpcode=*/false);
        return out;
    }

    constexpr std::size_t kMinLen = 3;
    constexpr std::size_t kMaxLen = 18;            // t=1 形式的长度上限
    constexpr std::size_t kMaxOff = 8192;          // t=1 形式的偏移上限
    constexpr std::size_t kHashBits = 14;
    constexpr std::size_t kHashSize = 1u << kHashBits;
    constexpr int kMaxChain = 48;

    // ---- 1. 分词：贪心 LZ77 ----
    struct Tok { bool isMatch; std::size_t a; std::size_t b; };   // 匹配:len,off / 字面量:起,长
    std::vector<Tok> toks;
    std::vector<std::int32_t> head(kHashSize, -1);
    std::vector<std::int32_t> prev(n, -1);
    auto hashAt = [&src](std::size_t p) {
        return ((static_cast<std::size_t>(src[p]) << 10) ^
                (static_cast<std::size_t>(src[p + 1]) << 5) ^
                 static_cast<std::size_t>(src[p + 2])) & (kHashSize - 1);
    };

    std::size_t litStart = 0;
    std::size_t pos = 0;
    while (pos < n) {
        std::size_t bestLen = 0, bestOff = 0;
        // 位置 < 8 时不出匹配：起始字面量条目至少要 8 字节（读取端 litLength21
        // 的最小长度就是 8），否则流首无法表达。
        if (pos >= 8 && pos + kMinLen <= n) {
            const std::size_t maxLen = std::min<std::size_t>(kMaxLen, n - pos);
            std::int32_t cand = head[hashAt(pos)];
            int guard = 0;
            for (; cand >= 0 && guard < kMaxChain; cand = prev[static_cast<std::size_t>(cand)], ++guard) {
                const std::size_t c = static_cast<std::size_t>(cand);
                if (pos - c > kMaxOff) break;
                std::size_t l = 0;
                while (l < maxLen && src[c + l] == src[pos + l]) ++l;
                if (l > bestLen) { bestLen = l; bestOff = pos - c; if (l >= maxLen) break; }
            }
        }
        if (pos + kMinLen <= n) {
            const std::size_t h = hashAt(pos);
            prev[pos] = head[h];
            head[h] = static_cast<std::int32_t>(pos);
        }
        if (bestLen >= kMinLen) {
            if (pos > litStart) toks.push_back(Tok{false, litStart, pos - litStart});
            toks.push_back(Tok{true, bestLen, bestOff});
            pos += bestLen;
            litStart = pos;
        } else {
            ++pos;
        }
    }
    if (litStart < n) toks.push_back(Tok{false, litStart, n - litStart});

    // ---- 2. 发射 ----
    bool prevIsMatch = false;
    std::size_t matchTailByte = 0;
    bool firstLiteral = true;
    for (const Tok& t : toks) {
        if (t.isMatch) {
            matchTailByte = emitMatch(out, t.a, t.b, 0);
            prevIsMatch = true;
        } else if (t.b > 0) {
            if (prevIsMatch && t.b <= 6) {
                // 紧随匹配的短字面量直接塞进匹配末字节的低 3 位（省一个条目）
                out[matchTailByte] = static_cast<std::uint8_t>(out[matchTailByte] | t.b);
                emitLiteralRun(out, src.data() + t.a, t.b, /*withOpcode=*/false);
            } else {
                emitLiteralSequence(out, src.data() + t.a, t.b,
                                    /*anchored=*/prevIsMatch || firstLiteral);
            }
            prevIsMatch = false;
            firstLiteral = false;
        }
    }
    return out;
}

// -----------------------------------------------------------------------------
//  页与段
// -----------------------------------------------------------------------------
struct PageRec {
    std::uint64_t size = 0;                 ///< 该页在文件里的字节数（0x20 对齐）
    std::uint64_t id = 0;
    std::uint64_t address = 0;              ///< 页在文件中的起始地址
};
struct SectionPageRec {
    std::uint64_t dataOffset = 0;           ///< 本页数据在该段解压流中的起始偏移
    std::uint64_t pageSize = 0;             ///< 段的规范页尺寸（段映射字段）
    std::uint64_t pageId = 0;
    std::uint64_t uSize = 0;                ///< 本页解压后字节数
    std::uint64_t cSize = 0;                ///< 本页压缩后字节数（原文页 = uSize）
    std::uint64_t checksum = 0;             ///< 解压数据的页校验
    std::uint64_t crc = 0;                  ///< 压缩（原文）数据的 CRC-64
};
struct Section {
    SectionSpec spec{};                     ///< 规范参数
    std::vector<std::uint8_t> payload;
    std::vector<SectionPageRec> pages;
};

/// 系统页编码结果
struct SystemPageResult {
    std::uint64_t pageId = 0;
    std::uint64_t address = 0;
    std::uint64_t compressedSize = 0;
    std::uint64_t uncompressedSize = 0;
    std::uint64_t crcCompressed = 0;
    std::uint64_t crcUncompressed = 0;
    std::uint64_t correctionFactor = 0;
};

/// 系统页尺寸：按解压尺寸给一个够放下「原文 + 校验」两次的页
std::size_t systemPageTargetSize(std::size_t dataSize)
{
    const std::size_t aligned = alignUp(dataSize, kCrcBlock);
    std::size_t size = ((aligned * 2) + kRsSystemK - 1) / kRsSystemK * kRsN;
    if (size < kMinSystemPageSize) size = kMinSystemPageSize;
    return alignUp(size, kAlign);
}

/// 页写入器：负责把已编码的页字节摆进输出、登记页映射，并给出该页的地址
class PageWriter
{
public:
    explicit PageWriter(std::vector<std::uint8_t>& out, std::vector<PageRec>& pages)
        : m_out(out), m_pages(pages) {}

    /// 登记一页已编码数据（内部做 0x20 对齐补零）
    std::uint64_t addPage(const std::vector<std::uint8_t>& bytes, std::uint64_t nextId)
    {
        const std::size_t size = alignUp(bytes.size(), kAlign);
        PageRec rec;
        rec.size = size;
        rec.id = nextId;
        rec.address = m_out.size();
        m_out.insert(m_out.end(), bytes.begin(), bytes.end());
        m_out.resize(rec.address + size, 0);            // 补长
        m_pages.push_back(rec);
        return rec.address;
    }
    void skip(std::size_t n) { m_out.resize(m_out.size() + n, 0); }

private:
    std::vector<std::uint8_t>& m_out;
    std::vector<PageRec>& m_pages;
};

} // namespace

// =============================================================================
//  dwgWriter21
// =============================================================================
namespace dwgWriter21 {

bool supports(DRW::Version ver) noexcept
{
    return ver == DRW::AC1021;
}

bool write(dwgW& writer, DRW_Interface* iface)
{
    dwgW::SectionData sd;
    if (!writer.buildSections(iface, DRW::AC1021, sd))
        return false;

    // ---- 1. 组节载荷 ------------------------------------------------------------
    // 头部与类节在 buildSections 里已按 2007 形态装配好（[RL bitSize] 开头的
    // 完整数据区），这里只补哨兵 / RL size / CRC16 / 结束哨兵（与 AC1018 同构）。
    constexpr int kSentThumbnailBegin = 1;
    constexpr int kSentVariableBegin  = 3;
    constexpr int kSentClassBegin     = 5;
    constexpr int kSectionTrailerZeros = 8;

    auto wrapSection = [](std::vector<std::uint8_t>& out, int sentinelKind,
                          const std::vector<std::uint8_t>& area) {
        appendSentinel(out, sentinelKind, true);
        putU32(out, static_cast<std::uint32_t>(area.size()));
        out.insert(out.end(), area.begin(), area.end());
        const std::uint16_t crc = dwgCalcCrc16(0xC0C1, out.data() + 16, out.size() - 16);
        putU16(out, crc);
        appendSentinel(out, sentinelKind, false);
        out.insert(out.end(), kSectionTrailerZeros, 0x00);
    };

    std::vector<std::uint8_t> headerPayload;
    wrapSection(headerPayload, kSentVariableBegin, sd.headerData);
    std::vector<std::uint8_t> classesPayload;
    wrapSection(classesPayload, kSentClassBegin, sd.classesData);

    // 对象流：MS 尺寸 + 体 + CRC16（与 AC1015/AC1018 完全一致）
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
        objectsPayload.insert(objectsPayload.end(), o.body.begin(), o.body.end());
        const std::uint16_t crc = dwgCalcCrc16(0xC0C1,
            objectsPayload.data() + offset, objectsPayload.size() - offset);
        putU16(objectsPayload, crc);
    }

    // 对象映射（HANDLES 段）：与 AC1015/AC1018 同构，偏移为段内偏移
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
        const std::uint8_t term[2] = { 0x00, 0x02 };
        const std::uint16_t termCrc = dwgCalcCrc16(0xC0C1, term, 2);
        handlesPayload.push_back(term[0]);
        handlesPayload.push_back(term[1]);
        handlesPayload.push_back(static_cast<std::uint8_t>(termCrc >> 8));
        handlesPayload.push_back(static_cast<std::uint8_t>(termCrc & 0xFF));
    }

    // 元数据段载荷（与 AC1018 同构；Template/ObjFreeSpace/AuxHeader 为压缩页）
    std::vector<std::uint8_t> previewDib;      // 预览 DIB（下方按真实页地址重建容器）
    std::vector<std::uint8_t> templatePayload;
    putU16(templatePayload, 0);                // 模板描述长度 = 0
    putU16(templatePayload, sd.measurement);   // 0 = 英制，1 = 公制

    std::vector<std::uint8_t> objFreeSpacePayload;
    putU32(objFreeSpacePayload, 0);
    putU32(objFreeSpacePayload, static_cast<std::uint32_t>(sd.objects.size()));
    putU32(objFreeSpacePayload, sd.tdDay);
    putU32(objFreeSpacePayload, sd.tdMsec);
    putU32(objFreeSpacePayload, 0);
    objFreeSpacePayload.push_back(4);
    putU32(objFreeSpacePayload, 0x00000032); putU32(objFreeSpacePayload, 0);
    putU32(objFreeSpacePayload, 0x00000064); putU32(objFreeSpacePayload, 0);
    putU32(objFreeSpacePayload, 0x00000200); putU32(objFreeSpacePayload, 0);
    putU32(objFreeSpacePayload, 0xFFFFFFFF); putU32(objFreeSpacePayload, 0);

    std::vector<std::uint8_t> auxHeaderPayload;
    {
        constexpr std::uint16_t kInternalVer = 27;     // AC1021 的内部版本号
        // 维护版本（文件头 0x0B 处的同值，AC1021 为 0x19）：真实文件在 AuxHeader
        // 的三处「(主版本, 维护版本)」对里都写它。原先写 0 与真实文件不一致。
        constexpr std::uint16_t kMaintenanceVer = 0x19;
        constexpr std::uint32_t kNumSaves = 1;
        auxHeaderPayload.push_back(0xFF); auxHeaderPayload.push_back(0x77);
        auxHeaderPayload.push_back(0x01);
        putU16(auxHeaderPayload, kInternalVer);
        putU16(auxHeaderPayload, kMaintenanceVer);
        putU32(auxHeaderPayload, kNumSaves);
        putU32(auxHeaderPayload, 0xFFFFFFFF);
        putU16(auxHeaderPayload, static_cast<std::uint16_t>(kNumSaves));
        putU16(auxHeaderPayload, 0);
        putU32(auxHeaderPayload, 0);
        putU16(auxHeaderPayload, kInternalVer); putU16(auxHeaderPayload, kMaintenanceVer);
        putU16(auxHeaderPayload, kInternalVer); putU16(auxHeaderPayload, kMaintenanceVer);
        putU16(auxHeaderPayload, 0x0005); putU16(auxHeaderPayload, 0x0893);
        putU16(auxHeaderPayload, 0x0005); putU16(auxHeaderPayload, 0x0893);
        putU16(auxHeaderPayload, 0); putU16(auxHeaderPayload, 1);
        for (int i = 0; i < 5; ++i) putU32(auxHeaderPayload, 0);
        putU32(auxHeaderPayload, sd.tdDay);
        putU32(auxHeaderPayload, sd.tdMsec);
        putU32(auxHeaderPayload, sd.tdDay);
        putU32(auxHeaderPayload, sd.tdMsec);
        putU32(auxHeaderPayload, sd.nextHandle);
        putU32(auxHeaderPayload, 0);
        putU16(auxHeaderPayload, 0);
        putU16(auxHeaderPayload, static_cast<std::uint16_t>(kNumSaves));
        for (int i = 0; i < 3; ++i) putU32(auxHeaderPayload, 0);
        putU32(auxHeaderPayload, kNumSaves);
        for (int i = 0; i < 4; ++i) putU32(auxHeaderPayload, 0);
    }

    // AcDb:SummaryInfo（DWGPROPS）。字段序列按 libredwg 的 summaryinfo.spec
    // 与真实 AC1021 文件（浩辰 CAD 导出，80 字节）逐字节核对：
    //   8 × T16（RS 码元数 + 相应 UTF-16LE）
    //   → TDINDWG / TDCREATE / TDUPDATE（各 8 字节：低 4 字节日、高 4 字节毫秒）
    //   → num_props(RS) → [props…] → unknown1(RL) → unknown2(RL)
    // 原实现是 `summaryInfoPayload[i * 3] = 0x01`——按 3 字节步长铺 0x01，
    // 产出完全错位的字节流，严格读取器解析该节立刻失败（libredwg 实测报
    // `Invalid .props`，num_props 被读成天文数字）。本实现不采集 DWGPROPS
    // 元数据，8 个字符串统一写空串（真实文件把空串写作「长度 1 + U+0000」）。
    std::vector<std::uint8_t> summaryInfoPayload;
    auto putT16Empty = [&summaryInfoPayload]() {
        putU16(summaryInfoPayload, 1);               // 长度 = 1 个码元
        summaryInfoPayload.push_back(0x00);          // U+0000
        summaryInfoPayload.push_back(0x00);
    };
    for (int i = 0; i < 8; ++i) putT16Empty();       // TITLE…HYPERLINKBASE
    auto putTimer = [&summaryInfoPayload](std::uint32_t day, std::uint32_t msec) {
        putU32(summaryInfoPayload, day);             // 低 4 字节：儒略日
        putU32(summaryInfoPayload, msec);            // 高 4 字节：当日毫秒
    };
    putTimer(0, 0);                                  // TDINDWG（累计编辑时间）
    putTimer(sd.tdDay, sd.tdMsec);                   // TDCREATE
    putTimer(sd.tdDay, sd.tdMsec);                   // TDUPDATE
    putU16(summaryInfoPayload, 0);                   // num_props（无自定义属性）
    putU32(summaryInfoPayload, 0);                   // unknown1
    putU32(summaryInfoPayload, 0);                   // unknown2

    std::vector<std::uint8_t> revHistoryPayload(16, 0);
    putU32At(revHistoryPayload, 8, 1);

    std::vector<std::uint8_t> appInfoPayload;
    auto appendUtf16 = [](std::vector<std::uint8_t>& d, const std::string& text) {
        putU16(d, static_cast<std::uint16_t>(text.size() + 1));
        for (char ch : text) { d.push_back(static_cast<std::uint8_t>(ch)); d.push_back(0x00); }
        d.push_back(0x00); d.push_back(0x00);
    };
    putU32(appInfoPayload, 2);
    appendUtf16(appInfoPayload, "AppInfoDataList");
    appendUtf16(appInfoPayload, std::string());
    for (int i = 0; i < 12; ++i) appInfoPayload.push_back(0x00);
    appendUtf16(appInfoPayload, "0.1.0");
    for (int i = 0; i < 16; ++i) appInfoPayload.push_back(0x00);
    appendUtf16(appInfoPayload, "Generated by LyCAD (libdxfrw dwgW)");
    for (int i = 0; i < 16; ++i) appInfoPayload.push_back(0x00);
    appendUtf16(appInfoPayload,
        "<ProductInformation name=\"LyCAD\" build_version=\"0.1.0\""
        " registry_version=\"0.1.0\" install_id_string=\"LyCAD\""
        " registry_localeID=\"1033\"/>");

    // 预览：1×1 白色 24 位 DIB；R2007 的预览容器仍用 DIB（code = 2）
    previewDib.clear();
    putU32(previewDib, 40);
    putU32(previewDib, 1);
    putU32(previewDib, 1);
    putU16(previewDib, 1);
    putU16(previewDib, 24);
    putU32(previewDib, 0);
    putU32(previewDib, 4);
    putU32(previewDib, 0); putU32(previewDib, 0);
    putU32(previewDib, 0); putU32(previewDib, 0);
    previewDib.push_back(0xFF); previewDib.push_back(0xFF);
    previewDib.push_back(0xFF); previewDib.push_back(0x00);

    /// 预览容器：描述符里的图片起始位置是**绝对文件偏移**。原文页（encoding=1）
    /// 的前 align8(len) 字节在文件里连续，故 base 取该页在文件中的地址即可。
    auto buildPreviewContainer = [](std::uint64_t base,
                                    const std::vector<std::uint8_t>& dib) {
        constexpr std::uint32_t kReservedHeaderLen = 80;
        const std::size_t count = 2;
        const std::size_t descLen = 9 * count;
        const std::size_t dataRel = 16 + 4 + 1 + descLen;
        const std::uint32_t headerStart = static_cast<std::uint32_t>(base + dataRel);
        const std::uint32_t imageStart  = headerStart + kReservedHeaderLen;
        const std::uint32_t overall = static_cast<std::uint32_t>(
            1 + descLen + kReservedHeaderLen + dib.size());
        std::vector<std::uint8_t> v;
        appendSentinel(v, kSentThumbnailBegin, true);
        putU32(v, overall);
        v.push_back(static_cast<std::uint8_t>(count));
        v.push_back(1);
        putU32(v, headerStart);
        putU32(v, kReservedHeaderLen);
        v.push_back(2);
        putU32(v, imageStart);
        putU32(v, static_cast<std::uint32_t>(dib.size()));
        v.insert(v.end(), kReservedHeaderLen, 0x00);
        v.insert(v.end(), dib.begin(), dib.end());
        appendSentinel(v, kSentThumbnailBegin, false);
        return v;
    };

    // ---- 2. 组节表（顺序 = 段映射顺序 = 落盘顺序）--------------------------------
    std::vector<Section> sections;
    for (const SectionSpec& spec : kSections) {
        Section s;
        s.spec = spec;
        if (std::strcmp(spec.name, "AcDb:FileDepList") == 0) {
            // 文件依赖表：本实现不记录依赖，写「0 条」的最小合法载荷
            // （4 字节条数 + 4 字节保留，全 0）——与 OpenCADStudio 导出的、
            // 已被浩辰 CAD 正常打开的文件逐字节一致。
            // **不要**留成空段：0 页的段记录会被严格读取器当作表结构非法
            // （libredwg 亦报 `Invalid num_pages 0, skip` 并跳过该节）。
            s.payload.assign(8, 0x00);
        } else if (std::strcmp(spec.name, "AcDb:AppInfo") == 0) {
            s.payload = std::move(appInfoPayload);
        } else if (std::strcmp(spec.name, "AcDb:Preview") == 0) {
            s.payload = buildPreviewContainer(0, previewDib);   // 地址下方重算
        } else if (std::strcmp(spec.name, "AcDb:SummaryInfo") == 0) {
            s.payload = std::move(summaryInfoPayload);
        } else if (std::strcmp(spec.name, "AcDb:RevHistory") == 0) {
            s.payload = std::move(revHistoryPayload);
        } else if (std::strcmp(spec.name, "AcDb:AcDbObjects") == 0) {
            s.payload = std::move(objectsPayload);
        } else if (std::strcmp(spec.name, "AcDb:ObjFreeSpace") == 0) {
            s.payload = std::move(objFreeSpacePayload);
        } else if (std::strcmp(spec.name, "AcDb:Template") == 0) {
            s.payload = std::move(templatePayload);
        } else if (std::strcmp(spec.name, "AcDb:Handles") == 0) {
            s.payload = std::move(handlesPayload);
        } else if (std::strcmp(spec.name, "AcDb:Classes") == 0) {
            s.payload = std::move(classesPayload);
        } else if (std::strcmp(spec.name, "AcDb:AuxHeader") == 0) {
            s.payload = std::move(auxHeaderPayload);
        } else if (std::strcmp(spec.name, "AcDb:Header") == 0) {
            s.payload = std::move(headerPayload);
        }
        sections.push_back(std::move(s));
    }

    // ---- 3. 落盘 ----------------------------------------------------------------
    std::vector<std::uint8_t> out(kReservedHeaderSize, 0);   // 0x0000-0x047F 预留
    std::vector<PageRec> pages;
    PageWriter pw(out, pages);
    std::uint64_t nextPageId = 1;

    // ---- 3.1 数据页（按段顺序）--------------------------------------------------
    auto encodeOnePage = [&](const std::uint8_t* data, std::size_t len, bool stored,
                             std::uint64_t& cSize, std::uint64_t& crc) {
        std::vector<std::uint8_t> raw(data, data + len);
        if (stored) {
            cSize = len;
            crc = ac21MirroredCrc64(0, static_cast<std::uint32_t>(len), data, len);
            return rsEncodeStored(raw, kFileRandomSeed);
        }
        const std::vector<std::uint8_t> comp = ac21Encode(raw);
        // 本压缩器只发字面量（见 ac21Encode 说明），输出恒不小于原文。规范与
        // AutoCAD / 浩辰 / 参考实现都以「compSize < uSize」判定该页是否压缩：
        // 若把更大的「压缩流」写出去并声明 compSize > uSize，读取端会按原文
        // 直接取用这段数据 —— 页内容乱码，页映像/段映像页更会直接找不到段映射
        // 页（libredwg 报 "Failed to find sections page map N"），随即判文件损坏。
        // 故压缩不划算时退回原文存储（compSize == uSize == len），与文件头
        // 元数据处的处理保持一致。
        const bool useComp = comp.size() < raw.size();
        const std::vector<std::uint8_t>& body = useComp ? comp : raw;
        cSize = body.size();
        crc = ac21MirroredCrc64(0, static_cast<std::uint32_t>(body.size()),
                                body.data(), body.size());
        const std::size_t factor = (alignUp(body.size(), kCrcBlock) + kRsDataK - 1) / kRsDataK;
        return rsEncodeInterleaved(body, factor, kRsDataK, 0xB8, 2);
    };

    auto chunkOf = [](const Section& s) {
        return s.spec.singlePage ? s.payload.size() : static_cast<std::size_t>(s.spec.pageSize);
    };

    auto writeSectionPages = [&](Section& s) {
        const std::size_t total = s.payload.size();
        if (total == 0) return;                            // 空段：登记 0 页
        const std::size_t chunk = chunkOf(s);
        std::size_t off = 0;
        while (off < total) {
            const std::size_t len = std::min(chunk, total - off);
            std::uint64_t cSize = 0, crc = 0;
            const std::vector<std::uint8_t> page =
                encodeOnePage(s.payload.data() + off, len, s.spec.stored, cSize, crc);
            SectionPageRec rec;
            rec.dataOffset = off;
            rec.pageSize = s.spec.pageSize;
            rec.pageId = nextPageId;
            rec.uSize = len;
            rec.cSize = cSize;
            rec.checksum = pageChecksum(0, s.payload.data() + off, len);
            rec.crc = crc;
            s.pages.push_back(rec);
            pw.addPage(page, nextPageId);
            ++nextPageId;
            off += len;
        }
    };

    // 预览页的容器里记录图片的**绝对文件偏移**：先按当前偏移试算页地址，再重建容器。
    // 原文页的前 align8(len) 字节在文件里连续，故 base 取该页地址即可。
    {
        Section& prev = sections[2];                       // AcDb:Preview
        std::size_t addr = out.size();
        for (std::size_t i = 0; i < 2; ++i) {              // 预览之前的段
            const Section& s = sections[i];
            std::size_t o = 0;
            while (o < s.payload.size()) {
                const std::size_t l = std::min(chunkOf(s), s.payload.size() - o);
                std::uint64_t cs = 0, cr = 0;
                const std::vector<std::uint8_t> pg =
                    encodeOnePage(s.payload.data() + o, l, s.spec.stored, cs, cr);
                addr = alignUp(addr + pg.size(), kAlign);
                o += l;
            }
        }
        prev.payload = buildPreviewContainer(addr, previewDib);
    }

    for (Section& s : sections)
        writeSectionPages(s);

    // ---- 3.2 段映射页 + 副本（系统页）-------------------------------------------
    std::vector<std::uint8_t> sectionMap;
    {
        // 注意：AC1021 的段映射**没有**前导条数（读取端 dwgReader21::readFileHeader
        // 从位置 0 起逐条解析，直到缓冲区结束）。写成 AC1018 那种「条数 + 记录」的
        // 形式会让整张表错位 8 字节，段名长度被读成垃圾值。
        for (const Section& s : sections) {
            putU64(sectionMap, s.payload.size());          // 段解压数据长度（= size）
            putU64(sectionMap, s.spec.pageSize);           // 规范页尺寸
            putU64(sectionMap, 0);                         // 加密标志（本实现不做异或混淆）
            putU64(sectionMap, s.spec.hash);
            const std::size_t nameChars = std::strlen(s.spec.name);
            putU64(sectionMap, (nameChars + 1) * 2);       // 段名长度（字节，含 NUL）
            putU64(sectionMap, 0);
            putU64(sectionMap, s.spec.stored ? 1 : 4);     // 编码：1 原文 / 4 LZ77
            putU64(sectionMap, s.pages.size());
            for (const char* p = s.spec.name; *p; ++p) {
                sectionMap.push_back(static_cast<std::uint8_t>(*p));
                sectionMap.push_back(0x00);                // UTF-16LE
            }
            sectionMap.push_back(0x00); sectionMap.push_back(0x00);
            for (const SectionPageRec& pr : s.pages) {
                putU64(sectionMap, pr.dataOffset);
                putU64(sectionMap, pr.pageSize);
                putU64(sectionMap, pr.pageId);
                putU64(sectionMap, pr.uSize);
                putU64(sectionMap, pr.cSize);
                putU64(sectionMap, pr.checksum);
                putU64(sectionMap, pr.crc);
            }
        }
        // 段映射必须以一条**空记录**收尾：meta[20]（sections_amount）声明的是
        // 记录**条数**（含这条空记录）。两份真实 R2007 文件（浩辰 CAD 与
        // AutoCAD 导出的 AC1021）都写满声明条数——「12 个节 + 1 条空记录」。
        // 少写这一条时严格读取器（浩辰）按声明条数逐条读，读到最后一条会落到
        // 段映射缓冲之外，读到垃圾长度/页数随即判「文件损坏」；宽松实现
        //（libredwg）只是把它们 skip 掉，因此此前一直未被发现。
        // 空记录内容与真实文件一致：dataSize=0、maxSize=0xF800、enc=4、pages=0。
        putU64(sectionMap, 0);
        putU64(sectionMap, 0xF800);
        putU64(sectionMap, 0);
        putU64(sectionMap, 0);
        putU64(sectionMap, 0);
        putU64(sectionMap, 0);
        putU64(sectionMap, 4);
        putU64(sectionMap, 0);
    }

    /// 系统页：LZ77 + RS(255,239) 交织。targetOverride 非 0 时用指定页尺寸
    ///（页映射页必须先按试算尺寸编码，使页映射里登记的自尺寸与实际一致）。
    auto writeSystemPage = [&](const std::vector<std::uint8_t>& data,
                               std::size_t targetOverride,
                               std::uint64_t pageIdOverride = 0) -> SystemPageResult {
        SystemPageResult r;
        r.uncompressedSize = data.size();
        r.crcUncompressed = ac21MirroredCrc64(0, static_cast<std::uint32_t>(data.size()),
                                              data.data(), data.size());
        const std::vector<std::uint8_t> comp = ac21Encode(data);
        // 同 encodeOnePage：压缩不划算时退回原文存储，令 compressedSize ==
        // uncompressedSize，读取端才会按「未压缩」取用原文（而不是把压缩流
        // 当原文直接取用 → 页映像/段映像页内容乱码 → 判文件损坏）。
        const bool useComp = comp.size() < data.size();
        const std::vector<std::uint8_t>& body = useComp ? comp : data;
        r.compressedSize = body.size();
        r.crcCompressed = ac21MirroredCrc64(0, static_cast<std::uint32_t>(body.size()),
                                            body.data(), body.size());
        const std::size_t target = targetOverride != 0 ? targetOverride
                                                       : systemPageTargetSize(data.size());
        const std::size_t aligned = alignUp(body.size(), kCrcBlock);
        std::uint64_t cf = 1;
        if (aligned > 0) {
            cf = (target / kRsN * kRsSystemK) / aligned;   // 尽量填满目标页
            if (cf < 2) cf = 2;
        }
        const std::size_t total = aligned * static_cast<std::size_t>(cf);
        const std::size_t factor = (total + kRsSystemK - 1) / kRsSystemK;
        std::vector<std::uint8_t> padded(factor * kRsSystemK, 0);
        if (aligned > 0) {
            for (std::size_t c = 0; c + aligned <= total; c += aligned)
                std::copy(body.begin(), body.end(), padded.begin() + c);
        }
        std::vector<std::uint8_t> page =
            rsEncodeInterleaved(padded, factor, kRsSystemK, 0x96, 8);
        page.resize(std::max(alignUp(page.size(), kAlign), target), 0);
        r.correctionFactor = cf;
        r.pageId = pageIdOverride != 0 ? pageIdOverride : nextPageId;
        r.address = pw.addPage(page, r.pageId);
        if (pageIdOverride == 0) ++nextPageId;
        else if (r.pageId >= nextPageId) nextPageId = r.pageId + 1;
        return r;
    };

    const SystemPageResult secMap1 = writeSystemPage(sectionMap, 0);
    const SystemPageResult secMap2 = writeSystemPage(sectionMap, 0);

    // ---- 3.3 页映射页 + 副本（登记自身，最后落盘）--------------------------------
    const std::uint64_t pageMapId = nextPageId;
    const std::uint64_t pageMap2Id = nextPageId + 1;
    // 记录数 = 已有页 + 本页映像页 + 其副本；meta[12]（pages_amount）声明的正是
    // 这个条数（pagesmap_usize == 记录数 × 16 也与之一致）。
    const std::size_t pmTarget = systemPageTargetSize((pages.size() + 2) * 16);
    std::vector<std::uint8_t> pageMap;
    for (const PageRec& p : pages) {
        putU64(pageMap, p.size);
        putU64(pageMap, p.id);
    }
    const std::size_t selfSizeSlot = pageMap.size();
    putU64(pageMap, 0);                                    // 自身页长（占位）
    putU64(pageMap, pageMapId);
    putU64(pageMap, 0);                                    // 副本页长（占位）
    putU64(pageMap, pageMap2Id);
    // 注意：**不要**再补一条 (size=0,id=0) 的终止记录。真实 R2007 文件（浩辰 CAD
    // 导出的 AC1021 实测）的页映像恰好 = pages_amount 条，没有终止记录；多写一条
    // 会让 pagesmap_usize/16 比 pages_amount 大 1，严格读取器据此认为表结构不一致。
    // （原先的终止记录是对规范的误读，段映射那条才是真正需要的收尾空记录。）
    {
        // 页映射页的实际尺寸只取决于压缩后的长度，条目本身恒 16 字节，故先试算
        const std::vector<std::uint8_t> probe = ac21Encode(pageMap);
        const std::size_t aligned = alignUp(probe.size(), kCrcBlock);
        std::uint64_t cf = (pmTarget / kRsN * kRsSystemK) / (aligned ? aligned : 1);
        if (cf < 2) cf = 2;
        const std::size_t factor = (aligned * cf + kRsSystemK - 1) / kRsSystemK;
        const std::size_t size = alignUp(std::max(factor * kRsN, pmTarget), kAlign);
        putU64At(pageMap, selfSizeSlot, size);
        putU64At(pageMap, selfSizeSlot + 16, size);
    }
    const SystemPageResult pm1 = writeSystemPage(pageMap, pmTarget, pageMapId);
    const SystemPageResult pm2 = writeSystemPage(pageMap, pmTarget, pageMap2Id);

    // ---- 4. 元数据（34 × u64）---------------------------------------------------
    const std::uint64_t header2Address = out.size();        // 文件头页副本紧随页映射
    std::vector<std::uint64_t> meta(34, 0);
    meta[0]  = 0x70;                                        // header size
    meta[1]  = header2Address + kFileHeaderPageSize;        // file size
    meta[2]  = pm1.crcCompressed;
    meta[3]  = pm1.correctionFactor;
    meta[5]  = pm2.address - kReservedHeaderSize;           // map2 offset（第 2 份副本）
    meta[6]  = pm2.pageId;
    meta[7]  = pm1.address - kReservedHeaderSize;           // pages map offset
    meta[8]  = pm1.pageId;
    meta[9]  = header2Address - kReservedHeaderSize;        // 文件头副本偏移
    meta[10] = pm1.compressedSize;
    meta[11] = pm1.uncompressedSize;
    meta[12] = pages.size();                                // 页总数（含页映射页）
    meta[13] = nextPageId - 1;                              // 最大页号
    meta[14] = 0x20;
    meta[15] = 0x40;
    meta[16] = pm1.crcUncompressed;
    meta[17] = 0xF800;
    meta[18] = 4;
    meta[19] = 1;
    meta[20] = sections.size() + 1;                         // 段数 + 1（规范约定）
    meta[21] = secMap1.crcUncompressed;
    meta[22] = secMap1.compressedSize;
    meta[23] = secMap2.pageId;
    meta[24] = secMap1.pageId;
    meta[25] = secMap1.uncompressedSize;
    meta[26] = secMap1.crcCompressed;
    meta[27] = secMap1.correctionFactor;
    meta[29] = 0x60100;                                     // stream version
    meta[30] = 0;                                           // crc seed
    meta[32] = kFileRandomSeed;                             // random seed

    auto metaBytes = [](const std::vector<std::uint64_t>& m) {
        std::vector<std::uint8_t> v;
        v.reserve(kMetadataSize);
        for (std::uint64_t x : m) putU64(v, x);
        return v;
    };

    /// 构建 0x400 文件头页（同时把 RNG 派生的种子与 header CRC 写回元数据）
    auto buildHeaderPage = [&](std::vector<std::uint64_t>& m) {
        CrcRandomEncoder rng(kFileRandomSeed);
        // ① RNG 派生字段（调用次序不可改：两个地图种子 → 两个随机数 → 编码种子）
        m[28] = rng.encodeSeed(m[30]);                      // sections map crc seed
        m[4]  = rng.encodeSeed(m[30]);                      // pages map crc seed
        const std::uint64_t random1 = rng.nextU64();
        const std::uint64_t random2 = rng.nextU64();
        const std::uint64_t encodedSeed = rng.encodeSeed(m[30]);
        auto rotl = [](std::uint64_t value, std::uint64_t control) {
            const unsigned sh = static_cast<unsigned>(control & 0x1F);
            return sh ? ((value << sh) | (value >> (64 - sh))) : value;
        };
        std::uint8_t cbytes[64];
        auto packLe = [&cbytes](const std::uint64_t* v) {
            for (int i = 0; i < 8; ++i)
                for (int k = 0; k < 8; ++k)
                    cbytes[i * 8 + k] = static_cast<std::uint8_t>((v[i] >> (8 * k)) & 0xFF);
        };
        std::uint64_t a[8];
        a[0] = rotl(random1, random2);
        a[1] = rotl(a[0], a[0]);
        a[2] = rotl(random2, a[1]);
        a[3] = rotl(a[2], a[2]);
        a[4] = rotl(random1, a[3]);
        a[5] = rotl(a[4], a[4]);
        a[6] = rotl(a[5], a[5]);
        a[7] = rotl(a[6], a[6]);
        packLe(a);
        const std::uint64_t normalCrc = checkDataNormalCrc64(random2, cbytes, 64);
        std::uint64_t b[8];
        b[0] = rotl(random1, random2);
        b[1] = rotl(normalCrc, b[0]);
        b[2] = rotl(random2, b[1]);
        b[3] = rotl(normalCrc, b[2]);
        b[4] = rotl(random1, b[3]);
        b[5] = rotl(normalCrc, b[4]);
        b[6] = rotl(random2, b[5]);
        b[7] = rotl(b[6], b[6]);
        packLe(b);
        const std::uint64_t mirCrc = checkDataMirroredCrc64(random1, cbytes, 64);
        m[31] = rng.encodeSeed(m[30]);                      // crc seed encoded

        // ② 文件头 CRC-64（覆盖含上述字段的整套元数据，其 0x108 字段内部清零）
        m[33] = headerCrc64(metaBytes(m));
        const std::vector<std::uint8_t> mb = metaBytes(m);
        // ③ 元数据压缩：本实现只发字面量（恒不小于原文），故按原文存放，
        //    长度字段取负 —— 读取端 `if (fileHdrCompLength < 0)` 即按原文取用。
        const std::vector<std::uint8_t> comp = ac21Encode(mb);
        const bool useComp = comp.size() < mb.size();
        const std::vector<std::uint8_t>& body = useComp ? comp : mb;
        const std::int32_t comprLen = useComp ? static_cast<std::int32_t>(body.size())
                                              : -static_cast<std::int32_t>(body.size());
        const std::uint64_t comprCrc = ac21NormalCrc64(
            0, static_cast<std::uint32_t>(body.size()), body.data(), body.size());

        // ④ 检查序列
        const std::uint64_t seqVal1 = rng.nextU64();
        const std::uint64_t seqVal2 = rotl(seqVal1, seqVal1);
        std::uint8_t seqBytes[16];
        for (int k = 0; k < 8; ++k) {
            seqBytes[k] = static_cast<std::uint8_t>((seqVal1 >> (8 * k)) & 0xFF);
            seqBytes[8 + k] = static_cast<std::uint8_t>((seqVal2 >> (8 * k)) & 0xFF);
        }
        const std::uint64_t seqCrc = ac21NormalCrc64Seed1(0, 16, seqBytes, 16);

        // ⑤ 前置块（3×u64 + 长度 + 长度2 + 数据），8 字节对齐后重复填满 717 字节
        std::vector<std::uint8_t> block;
        putU64(block, seqCrc);
        putU64(block, seqVal1);
        putU64(block, comprCrc);
        putU32(block, static_cast<std::uint32_t>(comprLen));
        putU32(block, 0);
        block.insert(block.end(), body.begin(), body.end());
        block.resize(alignUp(block.size(), kCrcBlock), 0);
        std::vector<std::uint8_t> preRs(kFileHeaderRsFactor * kRsSystemK, 0);
        const std::size_t copies = block.empty() ? 0 : preRs.size() / block.size();
        for (std::size_t i = 0; i < copies; ++i)
            std::copy(block.begin(), block.end(), preRs.begin() + i * block.size());
        if (copies * block.size() < preRs.size())
            rng.fillRandom(preRs.data() + copies * block.size(),
                           preRs.size() - copies * block.size());

        // ⑥ RS 编码 + 随机补长 + 尾部 0x28 校验数据
        const std::vector<std::uint8_t> rs = rsEncodeInterleaved(
            preRs, kFileHeaderRsFactor, kRsSystemK, 0x96, 8);
        std::vector<std::uint8_t> page(kFileHeaderPageSize, 0);
        const std::size_t copyLen = std::min(rs.size(), kRsDataInHeader);
        std::copy(rs.begin(), rs.begin() + copyLen, page.begin());
        if (copyLen < kRsDataInHeader)
            rng.fillRandom(page.data() + copyLen, kRsDataInHeader - copyLen);
        std::size_t cd = kRsDataInHeader;
        auto putCd = [&page, &cd](std::uint64_t v) {
            for (int k = 0; k < 8; ++k)
                page[cd + k] = static_cast<std::uint8_t>((v >> (8 * k)) & 0xFF);
            cd += 8;
        };
        putCd(normalCrc);
        putCd(mirCrc);
        putCd(random1);
        putCd(random2);
        putCd(encodedSeed);
        return page;
    };

    const std::vector<std::uint8_t> headerPage = buildHeaderPage(meta);
    std::copy(headerPage.begin(), headerPage.end(), out.begin() + kMetadataBlockSize);
    out.resize(header2Address + kFileHeaderPageSize, 0);
    std::copy(headerPage.begin(), headerPage.end(), out.begin() + header2Address);

    // ---- 5. 0x00 元数据块 -------------------------------------------------------
    {
        auto firstPageAddr = [&](const char* name) -> std::uint32_t {
            for (const Section& s : sections) {
                if (std::strcmp(s.spec.name, name) != 0 || s.pages.empty()) continue;
                const std::uint64_t id = s.pages.front().pageId;
                for (const PageRec& p : pages)
                    if (p.id == id) return static_cast<std::uint32_t>(p.address);
            }
            return 0;
        };
        std::vector<std::uint8_t> m(kMetadataBlockSize, 0);
        std::memcpy(m.data(), "AC1021", 6);
        m[0x0B] = 0x19;                                     // 维护版本（AutoCAD 2007）
        m[0x0C] = 0x03;
        putU32At(m, 0x0D, firstPageAddr("AcDb:Preview"));
        m[0x11] = 0x21;                                     // 写出版本（AC1021）
        m[0x12] = 0x19;
        putU16At(m, 0x13, sd.codepageId);
        putU32At(m, 0x18, 0);                               // 安全标志
        putU32At(m, 0x1C, 0);
        putU32At(m, 0x20, firstPageAddr("AcDb:SummaryInfo"));
        putU32At(m, 0x24, 0);                               // VBA 地址（无）
        putU32At(m, 0x28, 0x80);
        putU32At(m, 0x2C, firstPageAddr("AcDb:AppInfo"));
        std::copy(m.begin(), m.end(), out.begin());
    }

    // ---- 6. 落盘 ----------------------------------------------------------------
    std::ofstream file(sd.fileName.c_str(),
                       std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
    if (!file.is_open())
        return false;
    file.write(reinterpret_cast<const char*>(out.data()),
               static_cast<std::streamsize>(out.size()));
    const bool ok = file.good();
    file.close();
    return ok;
}

} // namespace dwgWriter21
