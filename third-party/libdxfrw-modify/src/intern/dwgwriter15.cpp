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
//!  dwgwriter15.cpp —— DWG **R2000 (AC1015)** 的**文件容器**。
//!
//!  与读取端 intern/dwgreader15 对称：数字 15 指 DWG 内部版本号
//!  （= AC1015 = AutoCAD 2000），不是年份。
//!
//!  分层（与读取端一致）
//!    · dwgW（声明与实现均在 ../libdwgrw.h / ../libdwgrw.cpp）—— 对外接口
//!      与全部**与版本无关**的写出逻辑；版本分派见 libdwgrw.cpp::dwgW::write()。
//!    · 本模块 —— 仅 AC1015 的**线性节容器**：版本头、6 条节定位记录、
//!      AuxHeader、缩略图、header/classes 节、对象流、对象映射、第二头部、
//!      R14 模板节，以及各处 CRC 回填。
//!    · ../intern/dwgwriter.h —— 与版本无关的位级原语（dwgBitWriter / CRC /
//!      哨兵），各版本容器共用。
//!
//!  节载荷（header 变量位流、classes 位流、对象流）由 dwgW::buildSections()
//!  统一构建（见 ../libdwgrw.cpp），本文件只负责摆放与校验。
//!
//!  新增目标版本：仿照本文件与 dwgwriter18.cpp 建立 dwgwriterNN.cpp/h，
//!  在 libdwgrw.cpp 的 dwgW::write() 分派处追加分支即可。
//!
#include "../libdwgrw.h"
#include "dwgwriter15.h"
#include "dwgwriter.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <vector>

namespace {

// 节哨兵类别（见 dwgSentinelBytes）
constexpr int kSentHeaderFile = 0;      // 文件头结束哨兵（HEADER_END）
constexpr int kSentThumbnail = 1;       // THUMBNAIL_BEGIN
constexpr int kSentVariable = 3;        // header 数据节（VARIABLE_BEGIN）
constexpr int kSentClass = 5;           // classes 节（CLASS_BEGIN）
constexpr int kSent2ndHeader = 7;       // 第二头部（2NDHEADER_BEGIN）

} // namespace

namespace dwgWriter15 {

bool supports(DRW::Version ver) noexcept
{
    return ver == DRW::AC1015;
}

bool write(dwgW& writer, DRW_Interface* iface)
{
    dwgW::SectionData sd;
    if (!writer.buildSections(iface, DRW::AC1015, sd))
        return false;
    const std::vector<std::uint8_t>& headerData = sd.headerData;
    const std::vector<std::uint8_t>& classesData = sd.classesData;
    const std::uint16_t codepageId = sd.codepageId;

    // ---- 3. 组装文件 ---------------------------------------------------------------
    std::vector<std::uint8_t> out;
    out.reserve(4096 + sd.objects.size() * 32);
    auto putBytes = [&out](const std::uint8_t* p, std::size_t n) {
        out.insert(out.end(), p, p + n);
    };
    auto putSentinel = [&out, &putBytes](int kind, bool begin) {
        std::uint8_t s[16];
        dwgSentinelBytes(kind, begin, s);
        putBytes(s, 16);
    };
    auto putRL = [&out](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    };
    auto putRS = [&out](std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v & 0xFF));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    auto putRC = [&out](std::uint8_t v) { out.push_back(v); };
    auto patchRL = [&out](std::size_t pos, std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
            out[pos + static_cast<std::size_t>(i)] =
                static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    };
    auto putBitData = [&putBytes](const std::vector<std::uint8_t>& data) {
        putBytes(data.data(), data.size());
    };

    // 3.1 版本头（21 字节）
    const char* verStr = "AC1015";
    putBytes(reinterpret_cast<const std::uint8_t*>(verStr), 6);
    for (int i = 0; i < 5; ++i) putRC(0);
    putRC(0x0F);                            // 维护版本
    putRC(0x01);                            // zero_one_or_three
    const std::size_t thumbnailAddrPos = out.size();
    putRL(0);                               // 缩略图地址（回填）
    putRC(25);                              // 生成程序版本（ACAD 2000）
    putRC(0);                               // 维护版本
    putRS(codepageId);                      // 代码页

    // 3.2 节定位记录占位（6 节）
    putRL(6);
    const std::size_t locatorPos = out.size();
    for (int i = 0; i < 6; ++i) { putRC(static_cast<std::uint8_t>(i)); putRL(0); putRL(0); }
    const std::size_t fileCrcPos = out.size();
    putRS(0);                               // 文件头 CRC（回填）
    putSentinel(kSentHeaderFile, true);

    // 3.3 AuxHeader（R2000 六节文件的第五节 对六节文件会无条件
    // 解析该节，缺省 (0,0) 会在 0 地址读出垃圾并中止后续解码）。
    const std::uint32_t auxAddr = static_cast<std::uint32_t>(out.size());
    {
        auto putAuxRS = [&putRS](std::uint16_t v) { putRS(v); };
        auto putAuxRL = [&putRL](std::uint32_t v) { putRL(v); };
        putRC(0xFF); putRC(0x77); putRC(0x01);  // aux_intro
        putAuxRS(25);                           // dwg_version（ACAD 2000）
        putAuxRS(0);                            // maint_version
        putAuxRL(1);                            // numsaves
        putAuxRL(0xFFFFFFFFu);                  // minus_1
        putAuxRS(1);                            // numsaves_1
        putAuxRS(1);                            // numsaves_2
        putAuxRL(0);                            // zero
        putAuxRS(25); putAuxRS(0);              // dwg/maint version 1
        putAuxRS(25); putAuxRS(0);              // dwg/maint version 2
        putAuxRS(4); putAuxRS(0x0565);          // unknown_6rs
        putAuxRS(5); putAuxRS(0x0893);
        putAuxRS(0); putAuxRS(1);
        putAuxRL(0); putAuxRL(0); putAuxRL(0);  // unknown_5rl
        putAuxRL(256); putAuxRL(393218);
        putAuxRL(0); putAuxRL(0);               // TDCREATE（day, msec）
        putAuxRL(0); putAuxRL(0);               // TDUPDATE（day, msec）
        putAuxRL(sd.nextHandle); putAuxRL(0);   // HANDSEED（RLL）
        putAuxRS(0);                            // zero_1
        putAuxRS(1);                            // numsaves_3
        putAuxRL(0); putAuxRL(0); putAuxRL(0);  // zero_2..4
        putAuxRL(1);                            // numsaves_4
        putAuxRL(0); putAuxRL(0);               // zero_5..6
        for (int i = 0; i < 8; ++i) putRC(0);   // 尾部保留（样例文件亦为 0）
    }
    const std::uint32_t auxSize = static_cast<std::uint32_t>(out.size()) - auxAddr;

    // 3.4 缩略图（预览图）
    // 预览区内容为 [RL 图像字节数][图像字节…]，由 BEGIN/END 哨兵界定。
    const std::uint32_t thumbnailAddr = static_cast<std::uint32_t>(out.size());
    putSentinel(kSentThumbnail, true);
    if (!sd.thumbnail.empty()) {
        putBytes(sd.thumbnail.data(), sd.thumbnail.size());
    } else {
        putRL(5);
        putRC(0);                               // 图片数 0
    }
    putSentinel(kSentThumbnail, false);

    // 3.4 header 数据节
    const std::uint32_t headerAddr = static_cast<std::uint32_t>(out.size());
    putSentinel(kSentVariable, true);
    const std::size_t headerRlPos = out.size();
    putRL(static_cast<std::uint32_t>(headerData.size()));
    putBitData(headerData);
    const std::uint16_t headerCrc = dwgCalcCrc16(
        0xC0C1, out.data() + headerRlPos, out.size() - headerRlPos);
    putRS(headerCrc);
    putSentinel(kSentVariable, false);
    const std::uint32_t headerSize = static_cast<std::uint32_t>(out.size()) - headerAddr;

    // 3.5 classes 节
    const std::uint32_t classesAddr = static_cast<std::uint32_t>(out.size());
    putSentinel(kSentClass, true);
    const std::size_t classesRlPos = out.size();
    putRL(static_cast<std::uint32_t>(classesData.size()));
    putBitData(classesData);
    const std::uint16_t classesCrc = dwgCalcCrc16(
        0xC0C1, out.data() + classesRlPos, out.size() - classesRlPos);
    putRS(classesCrc);
    putSentinel(kSentClass, false);
    const std::uint32_t classesSize = static_cast<std::uint32_t>(out.size()) - classesAddr;
    // classes 节结束哨兵之后、对象数据之前，必须写一个固定常量 RL=0x0DCA
    // 注意：它不计入 classes 节的声明尺寸（尺寸在上一步已定），但会占用文件空间，
    // 对象数据的起始偏移因此后移 4 字节。缺失会让严格读取器解析对象数据整体错位。
    putRL(0x0DCA);

    // 3.6 对象数据区（按句柄分配顺序）
    for (dwgW::ObjectRecord& o : sd.objects) {
        o.offset = static_cast<std::uint32_t>(out.size());
        // MS：对象体字节数（oType 起至句柄流止，不含 CRC）
        const std::uint32_t bodySize = static_cast<std::uint32_t>(o.body.size());
        if (bodySize > 0x7FFFu) {
            putRS(static_cast<std::uint16_t>((bodySize & 0x7FFFu) | 0x8000u));
            putRS(static_cast<std::uint16_t>(bodySize >> 15));
        } else {
            putRS(static_cast<std::uint16_t>(bodySize));
        }
        putBitData(o.body);
        const std::uint16_t objCrc = dwgCalcCrc16(0xC0C1, out.data() + o.offset,
                                                  out.size() - o.offset);
        putRS(objCrc);
    }

    // 3.7 对象映射（按句柄升序打包——映射内为 UMC/MC 增量编码，句柄必须
    // 递增；对象物理顺序与此无关。每节 ≤2030 字节数据，末尾补终止节）
    const std::uint32_t mapAddr = static_cast<std::uint32_t>(out.size());
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> entries;   // (句柄, 偏移)
        for (const dwgW::ObjectRecord& o : sd.objects)
            entries.emplace_back(o.handle, o.offset);
        std::sort(entries.begin(), entries.end());

        std::size_t i = 0;
        while (i < entries.size()) {
            // 每页重置增量
            std::uint32_t lastHandle = 0, lastOffset = 0;
            // 试探组包：单节结构与数据量
            std::vector<std::uint8_t> sec;      // 不含 BE16 尺寸字段
            std::size_t n = 0;
            while (i < entries.size()) {
                std::vector<std::uint8_t> probe(sec);
                dwgBitWriter w;
                w.writeUMC(entries[i].first - lastHandle);
                w.writeMC(static_cast<dint32>(entries[i].second) - static_cast<dint32>(lastOffset));
                const std::vector<std::uint8_t>& bytes = w.data();
                if (probe.size() + bytes.size() > 2030 && n > 0) break;
                probe.insert(probe.end(), bytes.begin(), bytes.end());
                sec = std::move(probe);
                lastHandle = entries[i].first;
                lastOffset = entries[i].second;
                ++i;
                ++n;
                if (sec.size() > 2030 - 32) break;  // 留余量给后续条目
            }
            const std::uint16_t secSize = static_cast<std::uint16_t>(2 + sec.size());
            std::vector<std::uint8_t> framed;
            framed.push_back(static_cast<std::uint8_t>(secSize >> 8));
            framed.push_back(static_cast<std::uint8_t>(secSize & 0xFF));
            framed.insert(framed.end(), sec.begin(), sec.end());
            const std::uint16_t crc = dwgCalcCrc16(0xC0C1, framed.data(), framed.size());
            framed.push_back(static_cast<std::uint8_t>(crc >> 8));  // 大端
            framed.push_back(static_cast<std::uint8_t>(crc & 0xFF));
            putBytes(framed.data(), framed.size());
        }
        // 终止节（size=2，仅尺寸+CRC）
        putRC(0); putRC(2);
        std::uint8_t term[2] = { 0x00, 0x02 };
        const std::uint16_t termCrc = dwgCalcCrc16(0xC0C1, term, 2);
        putRC(static_cast<std::uint8_t>(termCrc >> 8));
        putRC(static_cast<std::uint8_t>(termCrc & 0xFF));
    }
    const std::uint32_t mapSize = static_cast<std::uint32_t>(out.size()) - mapAddr;

    // 3.8 第二头部
    const std::uint32_t addr2 = static_cast<std::uint32_t>(out.size());
    putSentinel(kSent2ndHeader, true);
    const std::size_t rl2Pos = out.size();
    putRL(0);                               // size 占位
    std::size_t crc2DataEnd = 0;
    {
        dwgBitWriter w;
        w.writeBL(addr2);                   // 自身地址
        const char v11[11] = { 'A', 'C', '1', '0', '1', '5', 0, 0, 0, 0, 0 };
        for (char c : v11) w.writeRC(static_cast<std::uint8_t>(c));
        w.writeRC(0x0F);
        w.writeRC(1);
        w.writeBS(25);                      // dwg_versions
        w.writeRS(codepageId);
        w.writeBS(6);
        const std::uint32_t secs[6][3] = {
            { 0, headerAddr, headerSize }, { 1, classesAddr, classesSize },
            { 2, mapAddr, mapSize }, { 3, 0, 0 }, { 4, 0, 0 },
            { 5, auxAddr, auxSize } };
        for (int i = 0; i < 6; ++i) {
            w.writeRC(static_cast<std::uint8_t>(secs[i][0]));
            w.writeBL(secs[i][1]);
            w.writeBL(secs[i][2]);
        }
        w.writeBS(14);
        // 第二头部的 14 个句柄：
        // 0=HANDSEED 1..9=各控制对象 10=VX控制 11=命名对象字典 12/13=字典
        // 本写出器不生成命名字典，11..13 留空（0）。
        const std::uint32_t handles2[14] = {
            sd.nextHandle, sd.blockCtrl, sd.layerCtrl, sd.styleCtrl, sd.ltypeCtrl,
            sd.viewCtrl, sd.ucsCtrl, sd.vportCtrl, sd.appidCtrl, sd.dimstyleCtrl,
            sd.vpEntHeaderCtrl, sd.nod, sd.dictMline, sd.dictGroup };
        for (int i = 0; i < 14; ++i) {
            std::uint8_t nb = 0;
            std::uint32_t t = handles2[i];
            while (t) { ++nb; t >>= 8; }
            w.writeRC(nb);
            w.writeRC(static_cast<std::uint8_t>(i));
            for (int k = nb - 1; k >= 0; --k)
                w.writeRC(static_cast<std::uint8_t>((handles2[i] >> (8 * k)) & 0xFF));
        }
        w.padToByte();
        crc2DataEnd = out.size() + w.data().size();
        putBitData(w.data());
        putRS(0);                               // CRC 占位
        for (int i = 0; i < 8; ++i) putRC(0);   // junk_r14
    }
    const std::size_t endSentinel2Pos = out.size();
    putSentinel(kSent2ndHeader, false);
    // 先回填 size（CRC 覆盖该字段），再计算并回填 CRC
    patchRL(rl2Pos, static_cast<std::uint32_t>(endSentinel2Pos) - static_cast<std::uint32_t>(rl2Pos));
    {
        std::uint16_t crc2 = dwgCalcCrc16(0xC0C1, out.data() + rl2Pos, crc2DataEnd - rl2Pos);
        out[crc2DataEnd] = static_cast<std::uint8_t>(crc2 & 0xFF);
        out[crc2DataEnd + 1] = static_cast<std::uint8_t>(crc2 >> 8);
    }

    // 3.9 R14 模板节
    const std::uint32_t templateAddr = static_cast<std::uint32_t>(out.size());
    putRC(0); putRC(0); putRC(1); putRC(0);

    // ---- 4. 回填与文件头 CRC -------------------------------------------------------
    patchRL(thumbnailAddrPos, thumbnailAddr);
    for (int i = 0; i < 6; ++i) {
        const std::size_t recPos = locatorPos + static_cast<std::size_t>(i) * 9;
        std::uint32_t addr = 0, size = 0;
        switch (i) {
        case 0: addr = headerAddr; size = headerSize; break;
        case 1: addr = classesAddr; size = classesSize; break;
        case 2: addr = mapAddr; size = mapSize; break;
        case 4: addr = templateAddr; size = 4; break;
        case 5: addr = auxAddr; size = auxSize; break;
        default: break;
        }
        patchRL(recPos + 1, addr);
        patchRL(recPos + 5, size);
    }
    const std::uint16_t fileCrc = dwgCalcCrc16(0xC0C1, out.data(), fileCrcPos);
    out[fileCrcPos] = static_cast<std::uint8_t>(fileCrc & 0xFF);
    out[fileCrcPos + 1] = static_cast<std::uint8_t>(fileCrc >> 8);

    // ---- 5. 落盘 -------------------------------------------------------------------
    std::ofstream file(sd.fileName.c_str(), std::ios_base::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;
    file.write(reinterpret_cast<const char*>(out.data()),
               static_cast<std::streamsize>(out.size()));
    const bool ok = file.good();
    file.close();
    return ok;
}

} // namespace dwgWriter15
