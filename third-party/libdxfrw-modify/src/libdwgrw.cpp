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


#include "libdwgrw.h"
#include <fstream>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <ctime>
#include <utility>
#include "drw_header.h"
#include "intern/drw_dbg.h"
#include "intern/drw_textcodec.h"
#include "intern/dwgwriter.h"
#include "intern/dwgreader.h"
#include "intern/dwgreader15.h"
#include "intern/dwgreader18.h"
#include "intern/dwgreader21.h"
#include "intern/dwgreader24.h"
#include "intern/dwgreader27.h"
#include "intern/dwgreader32.h"
#include "intern/dwgwriter15.h"
#include "intern/dwgwriter18.h"
#include "intern/dwgwriter21.h"
#include "intern/dwgwriter24.h"
#include "intern/dwgwriter27.h"
#include "intern/dwgwriter32.h"

#define FIRSTHANDLE 48

/*enum sections {
    secUnknown,
    secHeader,
    secTables,
    secBlocks,
    secEntities,
    secObjects
};*/

dwgR::dwgR(const char* name){
    DRW_DBGSL(DRW_dbg::NONE);
    fileName = name;
    reader = NULL;
//    writer = NULL;
    applyExt = false;
    version = DRW::UNKNOWNV;
    error = DRW::BAD_NONE;
}

dwgR::~dwgR(){
    if (reader != NULL)
        delete reader;

}

void dwgR::setDebug(DRW::DBG_LEVEL lvl){
    switch (lvl){
    case DRW::DEBUG:
        DRW_DBGSL(DRW_dbg::DEBUG);
        break;
    default:
        DRW_DBGSL(DRW_dbg::NONE);
    }
}

/*reads metadata and loads image preview*/
bool dwgR::getPreview(){
    bool isOk = false;

    std::ifstream filestr;
    isOk = openFile(&filestr);
    if (!isOk)
        return false;

    isOk = reader->readMetaData();
    if (isOk) {
        isOk = reader->readPreview();
    } else
        error = DRW::BAD_READ_METADATA;

    filestr.close();
    if (reader != NULL) {
        delete reader;
        reader = NULL;
    }
    return isOk;
}

bool dwgR::testReader(){
    bool isOk = false;

    std::ifstream filestr;
    filestr.open (fileName.c_str(), std::ios_base::in | std::ios::binary);
    if (!filestr.is_open() || !filestr.good() ){
        error = DRW::BAD_OPEN;
        return isOk;
    }

    dwgBuffer fileBuf(&filestr);
    duint8 *tmpStrData = new duint8[fileBuf.size()];
    fileBuf.getBytes(tmpStrData, fileBuf.size());
    dwgBuffer dataBuf(tmpStrData, fileBuf.size());
    fileBuf.setPosition(0);
    DRW_DBG("\ndwgR::testReader filebuf size: ");DRW_DBG(fileBuf.size());
    DRW_DBG("\ndwgR::testReader dataBuf size: ");DRW_DBG(dataBuf.size());
    DRW_DBG("\n filebuf pos: ");DRW_DBG(fileBuf.getPosition());
    DRW_DBG("\n dataBuf pos: ");DRW_DBG(dataBuf.getPosition());
    DRW_DBG("\n filebuf bitpos: ");DRW_DBG(fileBuf.getBitPos());
    DRW_DBG("\n dataBuf bitpos: ");DRW_DBG(dataBuf.getBitPos());
    DRW_DBG("\n filebuf first byte : ");DRW_DBGH(fileBuf.getRawChar8());
    DRW_DBG("\n dataBuf  first byte : ");DRW_DBGH(dataBuf.getRawChar8());
    fileBuf.setBitPos(4);
    dataBuf.setBitPos(4);
    DRW_DBG("\n filebuf first byte : ");DRW_DBGH(fileBuf.getRawChar8());
    DRW_DBG("\n dataBuf  first byte : ");DRW_DBGH(dataBuf.getRawChar8());
    DRW_DBG("\n filebuf pos: ");DRW_DBG(fileBuf.getPosition());
    DRW_DBG("\n dataBuf pos: ");DRW_DBG(dataBuf.getPosition());
    DRW_DBG("\n filebuf bitpos: ");DRW_DBG(fileBuf.getBitPos());
    DRW_DBG("\n dataBuf bitpos: ");DRW_DBG(dataBuf.getBitPos());
    fileBuf.setBitPos(6);
    dataBuf.setBitPos(6);
    DRW_DBG("\n filebuf pos: ");DRW_DBG(fileBuf.getPosition());
    DRW_DBG("\n dataBuf pos: ");DRW_DBG(dataBuf.getPosition());
    DRW_DBG("\n filebuf bitpos: ");DRW_DBG(fileBuf.getBitPos());
    DRW_DBG("\n dataBuf bitpos: ");DRW_DBG(dataBuf.getBitPos());
    DRW_DBG("\n filebuf first byte : ");DRW_DBGH(fileBuf.getRawChar8());
    DRW_DBG("\n dataBuf  first byte : ");DRW_DBGH(dataBuf.getRawChar8());
    fileBuf.setBitPos(0);
    dataBuf.setBitPos(0);
    DRW_DBG("\n filebuf first byte : ");DRW_DBGH(fileBuf.getRawChar8());
    DRW_DBG("\n dataBuf  first byte : ");DRW_DBGH(dataBuf.getRawChar8());
    DRW_DBG("\n filebuf pos: ");DRW_DBG(fileBuf.getPosition());
    DRW_DBG("\n dataBuf pos: ");DRW_DBG(dataBuf.getPosition());
    DRW_DBG("\n filebuf bitpos: ");DRW_DBG(fileBuf.getBitPos());
    DRW_DBG("\n dataBuf bitpos: ");DRW_DBG(dataBuf.getBitPos());

    delete[]tmpStrData;
    filestr.close();
    DRW_DBG("\n\n");
    return isOk;
}

/*start reading dwg file header and, if can read it, continue reading all*/
bool dwgR::read(DRW_Interface *interface_, bool ext){
    bool isOk = false;
    applyExt = ext;
    iface = interface_;


//testReader();return false;

    std::ifstream filestr;
    isOk = openFile(&filestr);
    if (!isOk)
        return false;

    isOk = reader->readMetaData();
    if (isOk) {
        isOk = reader->readFileHeader();
        if (isOk) {
            isOk = processDwg();
        } else
            error = DRW::BAD_READ_FILE_HEADER;
    } else
        error = DRW::BAD_READ_METADATA;

    filestr.close();
    if (reader != NULL) {
        delete reader;
        reader = NULL;
    }

    return isOk;
}

/* Open the file and stores it in filestr, install the correct reader version.
 * If fail opening file, error are set as DRW::BAD_OPEN
 * If not are DWG or are unsupported version, error are set as DRW::BAD_VERSION
 * and closes filestr.
 * Return true on succeed or false on fail
*/
bool dwgR::openFile(std::ifstream *filestr){
    bool isOk = false;
    DRW_DBG("dwgR::read 1\n");
    filestr->open (fileName.c_str(), std::ios_base::in | std::ios::binary);
    if (!filestr->is_open() || !filestr->good() ){
        error = DRW::BAD_OPEN;
        return isOk;
    }

    char line[7];
    filestr->read (line, 6);
    line[6]='\0';
    DRW_DBG("dwgR::read 2\n");
    DRW_DBG("dwgR::read line version: ");
    DRW_DBG(line);
    DRW_DBG("\n");

    if (strcmp(line, "AC1006") == 0)
        version = DRW::AC1006;
    else if (strcmp(line, "AC1009") == 0) {
        version = DRW::AC1009;
//        reader = new dwgReader09(&filestr, this);
    }else if (strcmp(line, "AC1012") == 0){
        version = DRW::AC1012;
        reader = new dwgReader15(filestr, this);
    } else if (strcmp(line, "AC1014") == 0) {
        version = DRW::AC1014;
        reader = new dwgReader15(filestr, this);
    } else if (strcmp(line, "AC1015") == 0) {
        version = DRW::AC1015;
        reader = new dwgReader15(filestr, this);
    } else if (strcmp(line, "AC1018") == 0){
        version = DRW::AC1018;
        reader = new dwgReader18(filestr, this);
    } else if (strcmp(line, "AC1021") == 0) {
        version = DRW::AC1021;
        reader = new dwgReader21(filestr, this);
    } else if (strcmp(line, "AC1024") == 0) {
        version = DRW::AC1024;
        reader = new dwgReader24(filestr, this);
    } else if (strcmp(line, "AC1027") == 0) {
        version = DRW::AC1027;
        reader = new dwgReader27(filestr, this);
    } else if (strcmp(line, "AC1032") == 0) {
        version = DRW::AC1032;
        reader = new dwgReader32(filestr, this);
    } else
        version = DRW::UNKNOWNV;

    if (reader == NULL) {
        error = DRW::BAD_VERSION;
        filestr->close();
    } else
        isOk = true;

    return isOk;
}

/********* Reader Process *********/

bool dwgR::processDwg() {
    DRW_DBG("dwgR::processDwg() start processing dwg\n");
    bool ret;
    bool ret2;
    DRW_Header hdr;
    ret = reader->readDwgHeader(hdr);
    if (!ret) {
        error = DRW::BAD_READ_HEADER;
    }

    ret2 = reader->readDwgClasses();
    if (ret && !ret2) {
        error = DRW::BAD_READ_CLASSES;
        ret = ret2;
    }

    ret2 = reader->readDwgHandles();
    if (ret && !ret2) {
        error = DRW::BAD_READ_HANDLES;
        ret = ret2;
    }

    ret2 = reader->readDwgTables(hdr);
    if (ret && !ret2) {
        error = DRW::BAD_READ_TABLES;
        ret = ret2;
    }

    iface->addHeader(&hdr);

    for (std::map<duint32, DRW_LType*>::iterator it=reader->ltypemap.begin(); it!=reader->ltypemap.end(); ++it) {
        DRW_LType *lt = it->second;
        iface->addLType(const_cast<DRW_LType&>(*lt) );
    }
    for (std::map<duint32, DRW_Layer*>::iterator it=reader->layermap.begin(); it!=reader->layermap.end(); ++it) {
        DRW_Layer *ly = it->second;
        iface->addLayer(const_cast<DRW_Layer&>(*ly));
    }

    for (std::map<duint32, DRW_Textstyle*>::iterator it=reader->stylemap.begin(); it!=reader->stylemap.end(); ++it) {
        DRW_Textstyle *ly = it->second;
        iface->addTextStyle(const_cast<DRW_Textstyle&>(*ly));
    }

    for (std::map<duint32, DRW_Dimstyle*>::iterator it=reader->dimstylemap.begin(); it!=reader->dimstylemap.end(); ++it) {
        DRW_Dimstyle *ly = it->second;
        iface->addDimStyle(const_cast<DRW_Dimstyle&>(*ly));
    }

    for (std::map<duint32, DRW_Vport*>::iterator it=reader->vportmap.begin(); it!=reader->vportmap.end(); ++it) {
        DRW_Vport *ly = it->second;
        iface->addVport(const_cast<DRW_Vport&>(*ly));
    }

    for (std::map<duint32, DRW_AppId*>::iterator it=reader->appIdmap.begin(); it!=reader->appIdmap.end(); ++it) {
        DRW_AppId *ly = it->second;
        iface->addAppId(const_cast<DRW_AppId&>(*ly));
    }

    ret2 = reader->readDwgBlocks(*iface);
    if (ret && !ret2) {
        error = DRW::BAD_READ_BLOCKS;
        ret = ret2;
    }

    ret2 = reader->readDwgEntities(*iface);
    if (ret && !ret2) {
        error = DRW::BAD_READ_ENTITIES;
        ret = ret2;
    }

    ret2 = reader->readDwgObjects(*iface);
    if (ret && !ret2) {
        error = DRW::BAD_READ_OBJECTS;
        ret = ret2;
    }

    return ret;
}

// =============================================================================
//  dwgW：写出入口与**版本分派**
//
//  写出器与读取端 dwgR 完全对称：
//    · dwgW 的**主体实现**（本节以下的 Private / 编码器 / 各 writeXxx 回调 /
//      buildSections）都在本文件；
//    · 各版本的**文件容器**单独成文件，由这里按目标版本分派：
//        intern/dwgwriter15.cpp = AC1015（线性节）
//        intern/dwgwriter18.cpp = AC1018（分页 + LZ77）
//    · 与版本无关的位级原语在 intern/dwgwriter.h。
//  新增目标版本：在下面追加一个分支，并新建对应的 intern/dwgwriterNN.cpp/h。
// =============================================================================
bool dwgW::write(DRW_Interface* iface, DRW::Version ver)
{
    if (iface == nullptr)
        return false;

    if (dwgWriter15::supports(ver))
        return dwgWriter15::write(*this, iface);

    // AC1018（R2004）：分页 + LZ77 容器，节载荷与 AC15 共用。
    if (dwgWriter18::supports(ver))
        return dwgWriter18::write(*this, iface);

    // AC1021（R2007）：RS 纠错 + AC21 压缩流容器（intern/dwgwriter21.cpp）。
    if (dwgWriter21::supports(ver))
        return dwgWriter21::write(*this, iface);

    // AC1024（R2010）：容器与 AC1018（R2004）同族（0x80 加密文件头 +
    // 页/段映射 + 分页），差别只在版本标识与几个版本常量，故复用 dwgWriter18
    // 的摆放代码（intern/dwgwriter24.cpp）。注意**不是**复用 AC1021。
    if (dwgWriter24::supports(ver))
        return dwgWriter24::write(*this, iface);

    // AC1027（R2013）：容器与 AC1024 同族（libredwg 的 decode.c 对 R_2010b
    // 及以后一律走 decode_R2004），同样复用 dwgWriter18 的摆放代码
    // （intern/dwgwriter27.cpp）。
    if (dwgWriter27::supports(ver))
        return dwgWriter27::write(*this, iface);

    // AC1032（R2018）：容器同样与 R2010/R2013 同族（libredwg decode.c 的
    // `SINCE (R_2010b) → decode_R2004`），复用 dwgWriter18 的摆放代码
    // （intern/dwgwriter32.cpp）。
    if (dwgWriter32::supports(ver))
        return dwgWriter32::write(*this, iface);

    return false;                       // 暂不支持的目标版本
}

// =============================================================================
//  dwgW：DWG 原生写出器（与版本无关的主体实现）
//
//  与读取端对称：dwgR / dwgW 两个对外类都定义在本文件。各版本的**文件容器**
//  （AC1015 线性节, AC1018 分页 + LZ77）在 intern/dwgwriter15.cpp、
//  intern/dwgwriter18.cpp 中，只负责把这里构建的节载荷摆进文件。
//  与版本无关的位级原语（dwgBitWriter / CRC / 哨兵）见 intern/dwgwriter.h。
// =============================================================================
namespace {

// ---- R2000 固定对象类型 ---------------
enum : std::uint16_t {
    kText          = 0x01,
    kAttrib        = 0x02,  // 块参照上的属性值（独立对象，跟在 INSERT 之后）
    kAttdef        = 0x03,  // 块定义里的属性定义
    kBlock         = 0x04,
    kEndblk        = 0x05,
    kSeqEnd        = 0x06,  // 属性列表的结束标记
    kInsert        = 0x07,  // 块参照
    kMinsert       = 0x08,  // 块参照 + 阵列（列/行重复）
    kArc           = 0x11,
    kCircle        = 0x12,
    kLine          = 0x13,
    kDimOrdinate   = 0x14,  // 坐标标注（DIMENSION_ORDINATE）
    kDimLinear     = 0x15,
    kDimAligned    = 0x16,
    kDimRadial     = 0x19,
    kDimDiametric  = 0x1A,
    kPoint         = 0x1B,
    kSolid         = 0x1F,
    kEllipse       = 0x23,
    kSpline        = 0x24,
    kRay           = 0x28,
    kXline         = 0x29,
    kMText         = 0x2C,
    kBlockControl  = 0x30,
    kBlockHeader   = 0x31,
    kLayerControl  = 0x32,
    kLayer         = 0x33,
    kStyleControl  = 0x34,
    kStyle         = 0x35,
    kLTypeControl  = 0x38,
    kLType         = 0x39,
    kViewControl   = 0x3C,
    kVportControl  = 0x40,
    kVport         = 0x41,
    kAppIdControl  = 0x42,
    kAppId         = 0x43,
    kDimstyleControl = 0x44,
    kDimstyle      = 0x45,
    kLWPolyline    = 77,
    kHatch         = 78,
    kDictionary    = 0x2A,  // 42：命名对象字典（NOD 及其子字典）
    kMlineStyle    = 0x49,  // 73：多线样式（MLINESTYLE）
    kXrecord       = 0x4F   // 79：扩展记录（XRECORD）
};

/// 类号（≥500）：由 classes 节声明，对象头写类号而非固定类型号
constexpr std::uint16_t kClassDictWdf     = 500;   // AcDbDictionaryWithDefault
constexpr std::uint16_t kClassPlaceholder = 501;   // AcDbPlaceHolder
constexpr std::uint16_t kClassLayout      = 502;   // AcDbLayout
constexpr std::uint16_t kClassDictVar     = 503;   // AcDbDictionaryVar
constexpr std::uint16_t kClassArcDim      = 504;   // AcDbArcDimension（弧长标注）
constexpr std::uint16_t kClassTable       = 505;   // AcDbTable（表格 ACAD_TABLE）
constexpr std::uint16_t kClassLight       = 506;   // AcDbLight（光源 LIGHT，仅 R2007+）

constexpr std::uint8_t kHcAbsolute = 5;     // 句柄码 5：硬指针（绝对）
constexpr std::uint8_t kHcSoftPtr  = 4;     // 句柄码 4：软指针（绝对）——实体/控制对象属主
constexpr std::uint8_t kHcOwner    = 3;     // 句柄码 3：硬属主（控制对象 / XDicObj / NOD）
constexpr std::uint8_t kHcSoftOwn  = 2;     // 句柄码 2：软属主（表格条目）
constexpr std::uint8_t kHcData     = 0;     // 句柄码 0：数据句柄（句柄"值"，非对象引用）
constexpr double kDegToRad = 0.017453292519943295;

/// 文件头 0x13 处的代码页 id：39 = ANSI_936，30 = ANSI_1252
constexpr std::uint16_t kCodepageId936 = 39;
constexpr std::uint16_t kCodepageId1252 = 30;

bool isDefaultExtrusion(const DRW_Coord& c) noexcept
{
    return c.x == 0.0 && c.y == 0.0 && c.z == 1.0;
}


} // namespace

// =============================================================================
//  Private
// =============================================================================
struct dwgW::Private
{
    // ---- 对象缓冲 --------------------------------------------------------------
    struct Obj {
        std::uint32_t handle = 0;
        std::uint16_t type = 0;
        std::vector<std::uint8_t> body;   // 自 oType 起、句柄流止（含补位），无 CRC
        std::uint32_t offset = 0;         // MS 起始文件偏移（装配期填写）
        std::size_t prevBit = 0;          // prev_entity 数值部分在 body 内的位偏移（0=无）
        std::size_t nextBit = 0;          // next_entity 数值部分在 body 内的位偏移（0=无）
        std::uint32_t handleBits = 0;     // 句柄流位数（R2010+ 记录头要用，见 ObjectRecord）
    };

    /// 对象体构造：oType + bitsize 占位 + 数据 + 句柄流 + 回填 bitsize
    ///
    /// 2007+（AC1021）的对象体多一段**独立字符串区**：读取端（DRW_Entity /
    /// DRW_TableEntry::parseDwg 的 `version > AC1018` 分支）以 objSize-1 位为
    /// 「有字符串区」标志，其前 16 位是字符串**位数**，再往前才是字符串内容；
    /// 句柄流自 objSize 位起。故 2007+ 的装配顺序为
    ///     [数据][字符串区][u16 字符串位数][1 位标志][句柄流]
    /// 字符串内容由 writeT 按 codec 版本自动分流进 w 的字符串流（见
    /// dwgBitWriter::writeT / writeUCS），本处只负责摆放。
    struct ObjBuilder {
        /// 所属 Private。用来取目标版本：R2010+（AC1024）的对象头用**紧凑 otype
        /// 编码**且**不写 RL 尺寸字段**（读取端 DRW_Entity::parseDwg 的
        /// `version > DRW::AC1021` 分支用 `objSize = ms*8 - bs` 反推）。
        Private* owner = nullptr;
        explicit ObjBuilder(Private& o) : owner(&o) {}
        dwgBitWriter w;
        std::size_t rlPos = 0;
        std::size_t handleStart = 0;
        std::size_t prevBit = 0;          // prev_entity 数值位偏移（0=未写）
        std::size_t nextBit = 0;          // next_entity 数值位偏移（0=未写）
        /// 编码器**自己**已在句柄流之前写出「是否有字符串区」标志位（控制对象
        /// 即如此：读取端 DRW_ObjControl::parseDwg 显式 `buf->getBit()` 读它，
        /// 且不按 objSize 跳转）。置位后 finish() 不再补写，否则该处会多出
        /// 1 位，控制对象列出的条目句柄整体前移，读取端随即把表记录全部判为
        /// 「找不到」并在句柄循环里空转。
        bool trailingFlagWritten = false;

        /// 目标版本是否为 R2010+（对象头改用紧凑编码）。
        bool compactObjType() const noexcept
        {
            return owner != nullptr && owner->version > DRW::AC1021;
        }

        /// R2010+ 的对象类型号：2 位索引 + 变长（dwgBuffer::getObjType 的
        /// `version > DRW::AC1021` 分支之对偶）。
        void writeObjType24(std::uint16_t ot)
        {
            if (ot >= 0x01F0u && ot <= 0x02EFu) {
                w.writeBB(1);
                w.writeRC(static_cast<std::uint8_t>(ot - 0x01F0u));
            } else if (ot < 0x100u) {
                w.writeBB(0);
                w.writeRC(static_cast<std::uint8_t>(ot));
            } else {
                w.writeBB(2);
                w.writeRS(ot);
            }
        }

        void begin(std::uint16_t otype, std::uint32_t handle)
        {
            if (compactObjType()) {
                // R2010+：紧凑 otype，且**没有** RL 尺寸占位（finish 也不回填）。
                writeObjType24(otype);
                rlPos = 0;
            } else {
                w.writeBS(otype);
                rlPos = w.bitPosition();
                w.writeRL(0);
            }
            w.writeHandle(0, handle);
        }
        void beginHandles() { handleStart = w.bitPosition(); }
        /// 声明「标志位已由编码器自行写出」（见 trailingFlagWritten 说明）。
        void markTrailingFlagWritten() { trailingFlagWritten = true; }
        std::vector<std::uint8_t> finish()
        {
            std::size_t newHandleStart = handleStart;
            // 字符串区按字节对齐：读取端把「字符串区总位数」当**原始 16 位**
            // 读（getRawShort16），该字段必须起始于字节边界，字符串区长度本身
            // 也须是 8 的倍数，否则读出的文本整体偏移。
            dwgBitWriter* ss = const_cast<dwgBitWriter*>(w.stringStream());
            if (ss != nullptr) ss->padToByte();
            const std::size_t sinkBits = (ss != nullptr) ? ss->bitPosition() : 0;
            if (sinkBits > 0) {
                // 句柄流已写进 w：先按位暂存，插完字符串区再补回。
                // prevBit/nextBit 都落在数据区内（句柄流之前），不受影响。
                const std::size_t handleEnd = w.bitPosition();
                std::vector<bool> handles;
                handles.reserve(handleEnd - handleStart);
                for (std::size_t i = handleStart; i < handleEnd; ++i)
                    handles.push_back(w.bitAt(i));

                // 字符串区起点按字节对齐（中间不足一字节的位补 0）：读取端用
                // (objSize-17) 处的原始 16 位读字符串区长度，该字段必须起始于
                // 字节边界，且长度本身是 8 的倍数。数据区尾部的这些补位读取端
                // 不会读到——它读完数据区就 setPosition(objSize) 跳到句柄流。
                const std::size_t strAreaStart = (handleStart + 7) & ~static_cast<std::size_t>(7);
                w.setBitPosition(strAreaStart);
                w.appendBitsFrom(*ss);
                const std::size_t strBits = sinkBits;
                // 位数按「原始 16 位」写（读取端 getRawShort16），≥0x8000 时
                // 高 16 位另起一个字并置低位字的 0x8000 标志（读取端 -33 回退读它）。
                if (strBits >= 0x8000u) {
                    w.writeRS(static_cast<std::uint16_t>(strBits >> 15));
                    w.writeRS(static_cast<std::uint16_t>((strBits & 0x7FFFu) | 0x8000u));
                } else {
                    w.writeRS(static_cast<std::uint16_t>(strBits));
                }
                w.writeB(true);                        // 有字符串区
                newHandleStart = strAreaStart + strBits + 16 + 1;

                for (bool bit : handles) w.writeB(bit);
            } else if (ss != nullptr && !trailingFlagWritten) {
                // 无字符串区：**仅 2007+** 规格要求紧跟数据区写一个「无字符串区」标志位
                // （读取端在 objSize-1 位读它，objSize 即句柄流起点；参考实现
                // merge_three_stream 在无文本时执行 `main.write_bit(false)` 并把
                // total_bits 记作 main_size_bits + 1）。**漏写这一位会让句柄流整体
                // 前移 1 位**，读取端便把数据区最后一位当标志位：恰为 1 时会继续
                // 回退 16 位读出一个天文数字般的字符串区长度，随后整个对象的内容
                // （含其后的句柄）全部错位。实测 PLACEHOLDER / LAYOUT / 字典等
                // 无字符串对象首当其冲（libredwg 报 Invalid string stream
                // data_size ... > bitsize 与 Invalid LAYOUT.num_viewports）。
                //
                // 注意：2004 及更早（AC1018-）的对象是「双流」格式，数据区之后
                // 直接接句柄流，**没有**该标志位；在这里补一位会让所有实体整体
                // 错位（实测表现为图纸只剩个别图元）。
                const std::size_t handleEnd = w.bitPosition();
                std::vector<bool> handles;
                handles.reserve(handleEnd - handleStart);
                for (std::size_t i = handleStart; i < handleEnd; ++i)
                    handles.push_back(w.bitAt(i));
                w.setBitPosition(handleStart);
                w.writeB(false);                       // 无字符串区标志（2007+）
                newHandleStart = handleStart + 1;
                for (bool bit : handles) w.writeB(bit);
            }
            w.padToByte();
            const std::size_t endBit = w.bitPosition();
            // R2010+ 的对象头里**没有** RL 尺寸字段（读取端用 `bs` 反推：
            //   bitsize = size*8 - handlestream_size，
            // 见 libredwg dwg_decode_add_object），因此没有占位可回填；
            // 但要把「句柄流位数」交给容器写进记录头的那条 UMC。
            // 句柄流位于对象体末尾，故其位数 = 补齐后的总体位数 − 句柄流起点。
            if (owner != nullptr)
                owner->lastHandleBits = static_cast<std::uint32_t>(endBit - newHandleStart);
            if (!compactObjType()) {
                w.setBitPosition(rlPos);
                w.writeRL(static_cast<std::uint32_t>(newHandleStart));
                w.setBitPosition(endBit);
            }
            return w.takeData();
        }
    };

    /// INSERT 的属性句柄：写出 INSERT 时按条数一次性占下来，
    /// 之后写每条 ATTRIB / SEQEND 时依次取用。
    std::vector<std::uint32_t> attribHandles;
    std::uint32_t seqendHandle = 0;
    /// 当前正在写属性的那个 INSERT 的句柄：ATTRIB / SEQEND 的属主就是它
    std::uint32_t currentInsertHandle = 0;

    struct Blk {
        std::string name;
        std::uint32_t record = 0;
        std::uint32_t blockEnt = 0;
        std::uint32_t first = 0;
        std::uint32_t last = 0;
        std::uint32_t endblk = 0;
        bool space = false;
        bool encoded = false;
        std::vector<std::uint32_t> ents;  // 块内图元句柄（按编码顺序 = 链表顺序）
    };

    // ---- 状态 ------------------------------------------------------------------
    std::string fileName;
    DRW_Interface* iface = nullptr;
    DRW_TextCodec codec;
    std::uint32_t nextHandle = 0x30;
    std::vector<Obj> objects;
    /// ObjBuilder::finish() 算出的「句柄流位数」暂存位，由紧随其后的 pushObject
    /// 取走存入 Obj（R2010+ 的对象记录头要用它，见 ObjectRecord::handleBits）。
    std::uint32_t lastHandleBits = 0;

    /// 预览图字节（[RL 图像字节数][图像字节…]，不含 BEGIN/END 哨兵）；
    /// 为空时写规范空缩略图（表示"本文件不含预览"）。
    std::vector<std::uint8_t> thumbnail;

    std::uint32_t ltypeCtrl = 0, layerCtrl = 0, styleCtrl = 0, vportCtrl = 0;
    std::uint32_t dimstyleCtrl = 0, appidCtrl = 0, blockCtrl = 0, viewCtrl = 0, ucsCtrl = 0;
    std::uint32_t vpEntHeaderCtrl = 0;      // R2000 必需（读取端按 0x46 查找）
    std::uint32_t ltypeByLayer = 0, ltypeByBlock = 0, ltypeContinuous = 0;
    std::uint32_t layer0 = 0, styleStandard = 0, vportActive = 0;
    std::uint32_t dimstyleStandard = 0, appidAcad = 0;
    std::uint16_t codepageId = kCodepageId936;      // EED 字符串的代码页标识
    DRW::Version version = DRW::AC1015;  // 写出目标版本：2004+ 的对象层有多处差异

    // 命名对象字典链（真实 AutoCAD 文件中必然存在；缺省会让严格读取器
    // 拿不到标准字典而判文件无效）
    std::uint32_t nod = 0;                  // 根字典 NOD
    std::uint32_t dictGroup = 0;            // ACAD_GROUP
    std::uint32_t dictLayout = 0;           // ACAD_LAYOUT
    std::uint32_t dictMline = 0;            // ACAD_MLINESTYLE
    std::uint32_t dictPlotSettings = 0;     // ACAD_PLOTSETTINGS
    std::uint32_t dictPlotStyleName = 0;    // ACAD_PLOTSTYLENAME（DICTIONARYWDFLT）
    std::uint32_t placeholder = 0;          // PLACEHOLDER（DICTIONARYWDFLT 的默认项）
    std::uint32_t layoutModel = 0;          // LAYOUT：模型空间
    std::uint32_t layoutPaper = 0;          // LAYOUT：图纸空间（Layout1）
    std::uint32_t layoutPaper2 = 0;         // LAYOUT：图纸空间（Layout2）
    std::uint32_t dictColor = 0;            // ACAD_COLOR（空字典）
    std::uint32_t dictMaterial = 0;         // ACAD_MATERIAL（空字典）
    std::uint32_t dictVar = 0;              // AcDbVariableDictionary
    std::uint32_t mlineStyle = 0;           // MLINESTYLE "STANDARD"
    std::uint32_t headerXrec = 0;           // 字典（NOD 的 ACDBHEADERROUNDTRIPXREC 条目）
    std::vector<std::uint32_t> dictVars;    // DICTIONARYVAR 句柄（按字典条目顺序）

    Blk mspace, pspace, pspace0;
    std::map<std::string, Blk> blocks;
    Blk* current = nullptr;

    std::map<std::string, std::uint32_t> layerMap, ltypeMap, styleMap, dimstyleMap;
    std::set<std::uint32_t> encodedHandles;     // 已编码的表记录句柄

    /// 标注参数（R2000 DIMSTYLE 记录需要完整字段集，取值来自 header 变量）
    /// 默认值必须与 LyCAD 标注引擎的实际布局参数一致（DimensionEntity.cpp）：
    /// 字高 3.5、箭头长 = 字高、界线间隙 0.18×字高、界线超出 0.30×字高、
    /// 文字置于尺寸线上方（dimtad=1 风格）。真实 CAD 打开时若按 DIMSTYLE
    /// 重算标注，声明的值就是它的布局依据——与块内几何不一致就会产生偏移。
    struct DimStyleData {
        double scale=1.0, asz=3.5, exo=0.63, dli=3.75, exe=1.05;
        double rnd=0.0, dle=0.0, tp=0.0, tm=0.0;
        double txt=3.5, cen=2.5, tsz=0.0, altf=0.03937007874015748;
        double lfac=1.0, tvp=0.0, tfac=1.0, gap=0.625, altrnd=0.0;
        int tol=0, lim=0, tih=0, toh=0, se1=0, se2=0;
        int tad=1, zin=0, azin=0;
        int alt=0, altd=4, tofl=0, sah=0, tix=0, soxd=0;
        int clrd=0, clre=0, clrt=0;
        int adec=0, dec=4, tdec=4, altu=2, alttd=4, aunit=0;
        int frac=0, lunit=2, dsep=46, tmove=0, just=0;
        int sd1=0, sd2=0, tolj=1, tzin=0, altz=0, alttz=0, upt=0, atfit=3;
        int lwd=31, lwe=31;
    };
    DimStyleData dimData;

    /// 图形范围（来自 header 的 $EXTMIN/$EXTMAX）。用于推导默认视口视图参数，
    /// 使文件在没有显式 VPORT 时也能以正确缩放打开。
    DRW_Coord extMin, extMax;
    bool hasExt = false;

    // ---- 句柄与查找 --------------------------------------------------------------
    std::uint32_t allocHandle() { return nextHandle++; }

    /// 名称查表（DWG 的符号名不区分大小写）。
    /// 表里登记的名字与调用方传入的取法可能不同：内建项用混合大小写
    /// （ByLayer / Continuous），而文档侧存的是大写规范名（BYLAYER /
    /// CONTINUOUS）——精确匹配会落空并静默回退到默认值（实测：图层线型
    /// 是 CONTINUOUS 时被写成 ByLayer）。故先精确查、再忽略大小写查。
    static std::uint32_t findName(const std::map<std::string, std::uint32_t>& m,
                                  const std::string& name)
    {
        auto it = m.find(name);
        if (it != m.end()) return it->second;
        for (const auto& kv : m) {
            if (kv.first.size() != name.size()) continue;
            bool eq = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
                if (std::toupper(static_cast<unsigned char>(kv.first[i]))
                    != std::toupper(static_cast<unsigned char>(name[i]))) { eq = false; break; }
            }
            if (eq) return kv.second;
        }
        return 0;                       // 0 = 未找到（调用方自行回退）
    }

    std::uint32_t layerHandle(const std::string& name) const
    {
        const std::uint32_t h = findName(layerMap, name);
        return h != 0 ? h : layer0;
    }
    std::uint32_t ltypeHandle(const std::string& name) const
    {
        const std::uint32_t h = findName(ltypeMap, name);
        return h != 0 ? h : ltypeByLayer;
    }
    std::uint32_t styleHandle(const std::string& name) const
    {
        const std::uint32_t h = findName(styleMap, name);
        return h != 0 ? h : styleStandard;
    }
    std::uint32_t dimstyleHandle(const std::string& name) const
    {
        const std::uint32_t h = findName(dimstyleMap, name);
        return h != 0 ? h : dimstyleStandard;
    }
    std::uint32_t blockRecordHandle(const std::string& name) const
    {
        auto it = blocks.find(name);
        return it != blocks.end() ? it->second.record : mspace.record;
    }
    /// 表格样式句柄（ACAD_TABLE 的 H342）。本写出器不单独写 TABLESTYLE 对象，
    /// 一律回退到句柄 0（"无"）——读取端把 0 视作使用默认 Standard 样式。
    std::uint32_t tableStyleHandle(const std::string& /*name*/) const
    {
        return 0;
    }

    // ---- 对象登记 --------------------------------------------------------------
    void pushObject(std::uint32_t handle, std::uint16_t type, std::vector<std::uint8_t> body,
                    std::size_t prevBit = 0, std::size_t nextBit = 0)
    {
        Obj o;
        o.handle = handle;
        o.type = type;
        o.body = std::move(body);
        o.prevBit = prevBit;
        o.nextBit = nextBit;
        o.handleBits = lastHandleBits;
        objects.push_back(std::move(o));
        encodedHandles.insert(handle);
    }

    Obj* findObject(std::uint32_t handle)
    {
        for (Obj& o : objects)
            if (o.handle == handle) return &o;
        return nullptr;
    }

    // ---- 公共写入片段 -------------------------------------------------------------
    /// 2007+ 的对象体是「三段流」（数据 / 字符串 / 句柄）：ObjBuilder::finish 必须
    /// 知道当前是否 2007+，才能决定要不要补写「无字符串区」标志位；而 ObjBuilder 是
    /// Private 的嵌套类型、拿不到 version。故在 2007+ 的公共入口显式建立字符串流，
    /// finish 以它的存在性判定版本（2004 及更早不建立，双流装配保持原样不变）。
    void prepareStringStream(ObjBuilder& b) const
    {
        if (version > DRW::AC1018) b.w.ensureStringSink();
    }

    /// eed 为空时只写 BS 0 终止；否则写「BS 尺寸 + 应用句柄 + 数据」块。
    /// 注意 BS 尺寸只是句柄**之后**的数据长度—— libdxfrw 的读取端
    /// 都是先读句柄、再按尺寸读数据的。应用固定为 ACAD（码 5 硬指针）。
    void writeTableCommon(ObjBuilder& b, std::uint16_t otype, std::uint32_t handle,
                          const std::vector<std::uint8_t>* eed = nullptr)
    {
        b.begin(otype, handle);
        prepareStringStream(b);
        if (eed && !eed->empty()) {
            b.w.writeBS(static_cast<dint16>(eed->size()));
            b.w.writeHandle(kHcAbsolute, appidAcad);
            for (std::uint8_t byte : *eed) b.w.writeRC(byte);
            b.w.writeBS(0);                 // EED 终止
        } else {
            b.w.writeBS(0);                 // EED 长度（无扩展数据）
        }
        b.w.writeBL(0);                 // numReactors
        // 2004+：公共数据在此多一个「XDic 缺失标志」位（读取端 DRW_TableEntry::
        // parseDwg 的 `if (version > DRW::AC1015)`）。漏写会让其后句柄流错 1 位。
        // 写 1 = 无 XDicObj（句柄流不再写空 XDicObj 句柄，与真实文件一致）。
        if (version > DRW::AC1015) b.w.writeB(true);    // xDictFlag=1：无 XDicObj
        // 2013+：紧接着还有一个「has binary data」位（读取端 DRW_TableEntry::parseDwg
        // 的 `if (version > DRW::AC1024)`；libredwg common_entity_data.spec 的
        // `SINCE (R_2013) FIELD_B (has_ds_data, 0)`）。漏写会让其后数据整体错 1 位，
        // 表记录数读成 0、块记录全部找不到（实测 AC1027 停在 BAD_READ_TABLES）。
        if (version > DRW::AC1024) b.w.writeB(false);   // has binary data
    }

    /// 颜色字段：R2000 是 BS（ACI），R2004 起是 CMC —— BS 索引 + BL RGB + RC 标志
    /// （读取端一律走 buf->getCmColor(version)，2004+ 不再是纯 BS）。
    /// RGB 高字节是类型码：0xC0=ByLayer、0xC1=ByBlock、0xC3=ACI（低字节为索引），
    /// 见 dwgBuffer::getCmColor 的注释。标志字节 0 = 不带颜色名/色册名。
    void writeColor(dwgBitWriter& w, int aci)
    {
        if (version <= DRW::AC1015) {
            w.writeBS(static_cast<dint16>(aci));
            return;
        }
        std::uint32_t rgb;
        if (aci == 256)      rgb = 0xC0000000u;             // ByLayer
        else if (aci == 0)   rgb = 0xC1000000u;             // ByBlock
        else                 rgb = 0xC3000000u | (static_cast<std::uint32_t>(aci) & 0xFFu);
        w.writeBS(static_cast<dint16>(aci));
        w.writeBL(static_cast<dint32>(rgb));
        w.writeRC(0);
    }
    void writeColor(ObjBuilder& b, int aci) { writeColor(b.w, aci); }

    /// 表记录的公共句柄尾部。样例实测（LAYER 记录句柄流：
    /// [0] 码4 = 父控制对象、[1] 码3 = XDicObj）与规格 CONTROL_HANDLE_STREAM 一致。
    /// XDicObj 句柄：R2004+ 不写（xDictFlag=1，读取端不读它，真实 R2004 文件
    /// 亦无此句柄）；R2000 及以下必须写 —— 那些版本不写 xDictFlag 位，
    /// 读取端按默认值 0 判定 "XDicObj 存在" 而必然读取它，缺了会让其后
    /// 句柄流整体错位。
    void writeXdicObj(ObjBuilder& b)
    {
        if (version <= DRW::AC1015) b.w.writeHandle(kHcOwner, 0);
    }

    void writeTableHandleTail(ObjBuilder& b, std::uint32_t ctrlHandle)
    {
        b.w.writeHandle(kHcSoftPtr, ctrlHandle);    // ownerhandle：所属控制对象（码 4）
        writeXdicObj(b);
    }

    /// 命名对象（NOD 字典条目：MLINESTYLE / DICTIONARYVAR / XRECORD）的公共头。
    /// 数据段：EED + numReactors（BL）；句柄段：ownerhandle → reactors[n] → XDicObj。
    /// 这三类对象都会被所属命名字典记录一个 reactor，故 numReactors = 1。
    /// 注意 owner/reactors/xdicobj 属句柄段，
    /// 必须写在类型数据之后、并以 beginHandles() 标记句柄段起点。
    void writeNamedHeadStart(ObjBuilder& b, std::uint16_t otype, std::uint32_t handle)
    {
        b.begin(otype, handle);
        prepareStringStream(b);
        b.w.writeBS(0);                                     // EED 长度（无扩展数据）
        b.w.writeBL(1);                                     // numReactors = 1
        if (version > DRW::AC1015) b.w.writeB(true);        // xDictFlag=1：无 XDicObj（2004+）
        // 2013+：has_ds_data（libredwg common_object_handle_data.spec 的
        // `SINCE (R_2013) FIELD_B (has_ds_data, 0)`）。NOD 命名对象（MLINESTYLE /
        // DICTIONARYVAR / XRECORD）与表项一样要走这一位，否则其后对象字段整体错位。
        if (version > DRW::AC1024) b.w.writeB(false);       // has_ds_data = 0
    }
    void writeNamedHeadTail(ObjBuilder& b, std::uint32_t ownerDict)
    {
        b.w.writeHandle(kHcSoftPtr, ownerDict);             // ownerhandle（码 4）
        b.w.writeHandle(kHcSoftPtr, ownerDict);             // reactors[0]（码 4）
        writeXdicObj(b);
    }

    /// 实体公共数据（DRW_Entity::parseDwg common_entity_data.spec 的对偶）。
    /// entmode：2 = 模型空间（属主隐式 = *Model_Space）、1 = 图纸空间（隐式 =
    /// *Paper_Space）、0 = 属主句柄在句柄流中显式给出（块内实体）。
    /// AutoCAD 从不使用 0 表示空间实体，统一写 2/1 才能被严格读取器接受。
    void writeEntityCommon(ObjBuilder& b, std::uint16_t otype, const DRW_Entity& e,
                           std::uint32_t handle, int entmode)
    {
        b.begin(otype, handle);
        prepareStringStream(b);
        b.w.writeBS(0);                 // EED 长度（无扩展数据）
        b.w.writeB(false);              // preview_exists（无预览）
        b.w.writeBB(static_cast<std::uint8_t>(entmode));
        b.w.writeBS(0);                 // numReactors（0 与读取端的 BL 编码同形）
        // 2004+：xDictFlag 位（读取端 DRW_Entity::parseDwg 的 `version > AC1015`）。
        // 写 1 = 无 XDicObj：句柄流里就不再出现那个空 XDicObj 句柄，
        // 与真实 R2004 文件（浩辰）及其它第三方写出器的形态一致。
        if (version > DRW::AC1015) b.w.writeB(true);    // xDictFlag=1：无 XDicObj
        // nolinks 位只在 R13~R2002 存在（libredwg common_entity_data.spec 的
        // `VERSIONS (R_13b1, R_2002)`；读取端 `version < AC1018`）。原先多写的
        // `|| version > AC1024` 会让 AC1027 多写 1 位，与 has_ds_data 的位置冲突。
        if (version < DRW::AC1018)
            b.w.writeB(false);          // nolinks = 0：句柄流内含 prev/next 链
        // 2013+：has_ds_data（libredwg `SINCE (R_2013) FIELD_B (has_ds_data, 0)`）。
        if (version > DRW::AC1024)
            b.w.writeB(false);          // has_ds_data = 0（无 ACIS SAB 数据）
        // 颜色：**实体**走读取端 buf->getEnColor(version)，该函数对 AC1018+ 只读一个
        // BS（索引）；只有当 idx 高字节带 0x20（透明度）或 0x80（真彩）标志时才
        // 追加读 RL。即 ACI 颜色只占 2~18 位。
        // 注意与**表记录/头变量**区分：那两处读取端用 getCmColor（CMC = BS+BL+RC
        // = 44 位），必须用 writeColor；实体若误用 CMC，每个实体多写 42 位，
        // 其后字段（ltFlags/plotFlags/materialFlag…）全部错位，实体的
        // parseDwgEntHandle 随即越界（实测 good=0、ltFlags 读成 3）。
        b.w.writeBS(static_cast<dint16>(e.color));
        b.w.writeBD(e.ltypeScale);
        b.w.writeBB(0);                 // ltFlags：ByLayer
        b.w.writeBB(0);                 // plotFlags
        // 2007+：材质/阴影标志位（读取端 DRW_Entity::parseDwg 的 `version > AC1018`，
        // 位置在两个 plotFlags 之后、invisibleFlag 之前）。两者都写 0：读取端只在
        // materialFlag==3 / shadowFlag==3 时才到句柄流取材质/阴影句柄。
        // **漏写这 10 位会让其后全部实体字段整体前移**——HATCH 的渐变块首当其冲，
        // 读取端把错位后的位当 num_colors，读到天文数字后陷入死循环（实测）。
        if (version > DRW::AC1018) {
            b.w.writeBB(0);             // materialFlag
            b.w.writeRC(0);             // shadowFlag
        }
        // R2010+：材质/阴影之后还有 3 个**视觉样式**标志位（读取端
        // DRW_Entity::parseDwg 的 `version > DRW::AC1021`
        // 分支：2 位 full/face + 1 位 edge；libredwg common_entity_data.spec
        // 的 `SINCE (R_2010b)` 为 has_full/face/edge_visualstyle 三个 B）。
        // **漏写会让其后全部实体字段整体前移 3 位**：invisibleFlag 与线宽读错，
        // 实体特有字段（LWPOLYLINE 的顶点数等）读到天文数字——顶点循环随即
        // 变成事实上的死循环（实测 vertex num 读成 33554432，日志暴涨到 GB 级）。
        if (version > DRW::AC1021) {
            b.w.writeBB(0);             // has_full_visualstyle + has_face_visualstyle
            b.w.writeB(false);          // has_edge_visualstyle
        }
        b.w.writeBS(e.visible ? 0 : 1);
        b.w.writeRC(static_cast<std::uint8_t>(DRW_LW_Conv::lineWidth2dwgInt(e.lWeight)));
    }

    /// 由属主句柄推导 entmode（AutoCAD 约定）。
    int entmodeFor(std::uint32_t owner) const
    {
        if (owner != 0 && owner == mspace.record) return 2;   // *Model_Space
        if (owner != 0 && owner == pspace.record) return 1;   // *Paper_Space
        return 0;                                             // 块内实体：显式属主
    }

    /// 厚度/挤出（R2000 风格的可省略编码）
    void writeDimensionCommon(ObjBuilder& b, DRW_Dimension& ent)
    {
        // R2010+：标注数据最前面是一个 RC「class_version」（读取端
        // DRW_Dimension::parseDwg 的 `version > DRW::AC1021` 先 getRawChar8；
        // libredwg dwg.spec 的 COMMON_ENTITY_DIMENSION：
        //   `SINCE (R_2010b) FIELD_RC (class_version, 280); /* 0=r2010 */`）。
        // 写 0（= R2010）。漏写 1 字节会让其后全部标注字段前移一个字节，
        // 标注类型/测量值读成垃圾并判对象失败（实测 "Entity type 22 has failed"）。
        if (version > DRW::AC1021) b.w.writeRC(0);   // class_version = 0（R2010）
        // 拉伸方向这里是普通 3BD，没有公共实体数据那种「默认标志位」
        const DRW_Coord ext = ent.getExtrusion();
        b.w.write3BD(ext.x, ext.y, ext.z);
        const DRW_Coord tp = ent.getTextPoint();
        b.w.write2RD(tp.x, tp.y);           // 文字中点 (11)
        b.w.writeBD(tp.z);                  // 文字高程 (31)
        // flag1 位语义（与读取端对偶）：bit0=文字处于默认位置（读取端据此清除
        // DXF 70 的 bit7），bit1=引用几何块（读取端还原 bit5，R13 起恒置位）。
        // 低 3 位的标注子类型由对象类型决定，不在此字节内。
        b.w.writeRC(0x03);
        b.w.writeT(ent.getText(), codec);   // 用户文字 (1)
        b.w.writeBD(0.0);                   // 文字旋转 (53)
        b.w.writeBD(0.0);                   // 水平方向 (51)
        b.w.write3BD(1.0, 1.0, 1.0);        // 插入比例（AutoCAD 实测写 (1,1,1)）
        b.w.writeBD(0.0);                   // 插入旋转 (54)
        b.w.writeBS(static_cast<dint16>(ent.getAlign()));          // 文字对齐 (71)
        b.w.writeBS(static_cast<dint16>(ent.getTextLineStyle()));  // 行距样式 (72)
        b.w.writeBD(ent.getTextLineFactor());                      // 行距系数 (41)
        b.w.writeBD(ent.getMeasurement());                         // 实测值 (42)
        // 2007+：实测值与 clone 插入点之间还有 3 个标志位（读取端
        // DRW_Dimension::parseDwg 的 `if (version > DRW::AC1018)`：unk / flip1 /
        // flip2）。漏写会让其后全部标注字段前移 3 位，标注特有字段读到越界
        //（实测 DIMORDINATE 的 isGood() 变假、整张图判为损坏）。
        if (version > DRW::AC1018) {
            b.w.writeB(false);             // unk
            b.w.writeB(false);             // flip1
            b.w.writeB(false);             // flip2
        }
        b.w.write2RD(0.0, 0.0);             // clone 插入点 (12)
    }

    /// 厚度/挤出（R2000 风格的可省略编码）
    void writeThickness(ObjBuilder& b, double thickness)
    {
        if (thickness == 0.0) b.w.writeB(true);
        else { b.w.writeB(false); b.w.writeBD(thickness); }
    }
    void writeExtrusion(ObjBuilder& b, const DRW_Coord& ext)
    {
        if (isDefaultExtrusion(ext)) b.w.writeB(true);
        else { b.w.writeB(false); b.w.write3BD(ext.x, ext.y, ext.z); }
    }
    void writeThicknessExtrusion(ObjBuilder& b, double thickness, const DRW_Coord& ext)
    {
        writeThickness(b, thickness);
        writeExtrusion(b, ext);
    }

    /// TEXT / ATTRIB / ATTDEF 共有的文字数据段。
    ///
    /// 只写文字字段，**不含**属性专属尾部（标签/标志）与句柄流——两种属性实体
    /// 在这之后还要写自己的字段，所以必须拆出来复用，不能整体调 writeText。
    ///
    /// 注意挤出与厚度的顺序：读取端是 `getExtrusion()` 再 `getThickness()`，
    /// 这里就必须是挤出在前。两者都以「1 位 = 取默认值」编码，默认时写反看不出来
    ///（早先 writeText 就写反了，因为常见文字两者都取默认，一直没暴露）；
    /// 厚度或挤出非默认时写反，读回来就会张冠李戴。
    void writeTextBody(ObjBuilder& b, const DRW_Text& ent)
    {
        const bool hasAlignment = ent.alignH != DRW_Text::HLeft
                               || ent.alignV != DRW_Text::VBaseLine;
        std::uint8_t df = 0x01          // 省略高程（z=0）
                        | 0x04;         // 省略倾角（0）
        if (!hasAlignment) df |= 0x02;  // 省略对齐点（取插入点）
        if (ent.textgen == 0) df |= 0x20;
        if (!hasAlignment) df |= 0x40 | 0x80;

        b.w.writeRC(df);
        if (!(df & 0x01)) b.w.writeRD(ent.basePoint.z);
        b.w.write2RD(ent.basePoint.x, ent.basePoint.y);
        if (!(df & 0x02)) {
            b.w.writeDD(ent.secPoint.x, ent.basePoint.x);
            b.w.writeDD(ent.secPoint.y, ent.basePoint.y);
        }
        writeExtrusion(b, ent.extPoint);
        writeThickness(b, ent.thickness);
        // 倾角与旋转角：DRW_Text 的 angle/oblique 与 DXF 50/51 组码同单位（度），
        // 而 DWG 位流里一律存弧度——不换算的话，90° 的文字会被真实 CAD 解释为
        // 90 弧度（≈116.6° 取模），旋转整体错乱。
        if (!(df & 0x04)) b.w.writeRD(ent.oblique * kDegToRad);
        if (!(df & 0x08)) b.w.writeRD(ent.angle * kDegToRad);
        b.w.writeRD(ent.height);
        if (!(df & 0x10)) b.w.writeRD(ent.widthscale);
        b.w.writeT(ent.text, codec);
        if (!(df & 0x20)) b.w.writeBS(static_cast<dint16>(ent.textgen));
        if (!(df & 0x40)) b.w.writeBS(static_cast<dint16>(ent.alignH));
        if (!(df & 0x80)) b.w.writeBS(static_cast<dint16>(ent.alignV));
    }

    /// 取出为 ATTRIB 预分配的句柄（没预分配就现分配，见 dwgW::writeInsert）
    std::uint32_t takeAttribHandle()
    {
        if (attribHandles.empty()) return allocHandle();
        const std::uint32_t handle = attribHandles.front();
        attribHandles.erase(attribHandles.begin());
        return handle;
    }

    std::uint32_t takeSeqendHandle()
    {
        if (seqendHandle == 0) return allocHandle();
        const std::uint32_t handle = seqendHandle;
        seqendHandle = 0;
        return handle;
    }

    /// 实体句柄流。顺序依 libdxfrw `parseDwgEntHandle`（R2000 路径）与样例实测：
    ///   [owner 仅 entmode==0] → [numReactors × reactors] → [XDicObj] →
    ///   if(!nolinks){ prev_entity → next_entity } → [layer] → if(ltFlags==3)[linetype]
    /// prev/next 以定长占位写出，装配期按块内实体链回填（BLOCK/ENDBLK 保持 0）。
    /// dimStyleH/dimBlockH 为标注实体追加的样式/匿名块句柄（普通实体传 0）。
    static constexpr int kLinkBytes = 2;    // prev/next 句柄定长字节数（支持 ≤0xFFFF）

    void writeEntityHandles(ObjBuilder& b, std::uint32_t owner, const std::string& layerName,
                            int entmode, std::uint32_t dimStyleH = 0, std::uint32_t dimBlockH = 0)
    {
        b.beginHandles();
        if (entmode == 0) b.w.writeHandle(kHcSoftPtr, owner);
        writeXdicObj(b);                // 2004+ 无；R2000 及以下写入
        // prev/next 定长链只有 R2000- 才写：2004+ 读取端不读这两个句柄
        //（drw_entities.cpp 的 `if (version < DRW::AC1018)`），块内实体改由
        // 块记录的实体句柄表枚举（见 encodeBlockRecord）。
        if (version < DRW::AC1018) {
            b.prevBit = b.w.bitPosition() + 8;      // 跳过 (code<<4|size) 头部字节
            b.w.writeHandleFixed(kHcSoftPtr, 0, kLinkBytes);
            b.nextBit = b.w.bitPosition() + 8;
            b.w.writeHandleFixed(kHcSoftPtr, 0, kLinkBytes);
        }
        b.w.writeHandle(kHcAbsolute, layerHandle(layerName));
        if (dimStyleH) b.w.writeHandle(kHcAbsolute, dimStyleH);
        if (dimBlockH) b.w.writeHandle(kHcAbsolute, dimBlockH);
    }

    /// 实体写出主流程：公共数据 → 类型数据 → 句柄流 → 排队。
    /// fill 写类型数据；extraHandles 在公共句柄流之后追加句柄（标注实体用）。
    struct NoOp { void operator()(ObjBuilder&) const {} };
    /// 普通实体的句柄由内部按顺序分配；属性实体（ATTRIB/SEQEND）要用
    /// **预分配**的句柄 —— INSERT 在写出时就得填好 first_attrib / last_attrib /
    /// seqend 三个句柄，那时这些对象还没编码。
    ///
    /// ownerOverride 非 0 时改用它作属主：DWG 里 ATTRIB 与 SEQEND 的属主是**块参照
    /// 自己**（不是块记录）——实测 AutoCAD 2000 写的图纸正是如此（ATTRIB/SEQEND 的
    /// 330 都指向 INSERT 的句柄）。照「块内实体」那样写成块记录，别的实现会把属性
    /// 当成游离实体。
    ///
    /// linkToBlock 为假时**不**把该对象挂进当前块的实体链：属性是块参照的子实体，
    /// 靠 INSERT 的三个句柄索引，不参与块的 prev/next 链（AutoCAD 的写法亦然）。
    template<typename Fill, typename ExtraHandles = NoOp>
    std::uint32_t encodeEntity(std::uint16_t otype, const DRW_Entity& e, Fill fill,
                               ExtraHandles extraHandles = NoOp{}, std::uint32_t handle = 0,
                               std::uint32_t ownerOverride = 0, bool linkToBlock = true)
    {
        if (handle == 0) handle = allocHandle();
        Blk& blk = current ? *current : mspace;
        const std::uint32_t owner = ownerOverride != 0 ? ownerOverride : blk.record;
        const int entmode = entmodeFor(owner);
        ObjBuilder b(*this);
        writeEntityCommon(b, otype, e, handle, entmode);
        fill(b);
        writeEntityHandles(b, owner, e.layer, entmode);
        extraHandles(b);
        pushObject(handle, otype, b.finish(), b.prevBit, b.nextBit);
        if (linkToBlock) {
            if (blk.first == 0) blk.first = handle;
            blk.last = handle;
            blk.ents.push_back(handle);
        }
        return handle;
    }

    // ---- 初始化 / 收尾 -------------------------------------------------------------
    void initInfrastructure();
    void encodePendingDefaults();
    void closeCurrentBlock();
    void encodeBlockPair(Blk& blk);
};

// =============================================================================
//  基础设施对象
// =============================================================================
namespace {

using Private = dwgW::Private;

/// 控制对象类别：决定 num_entries 的位宽与尾部附加句柄。
/// BLOCK 尾部附 model_space/paper_space，LTYPE 尾部附 byblock/bylayer。
enum class CtrlKind { Bl, Bs, Block, LType, Dimstyle };

/// 表项公共头里的 xref 标志。
///
/// **规格在 R2007+（AC1021）把三段式合并成了一个 BitShort**：
///   · 2004-（AC1015/AC1018）：B「referenced」+ BS「xrefindex」+ B「xref dependent」；
///   · 2007+（AC1021）        ：BS「is_xref_resolved」——只允许 0（非 xref 相关）
///                              或 256（0x100，已解析/依赖 xref）。
/// 读取端见 drw_objects.cpp 各表项 parseDwg 的 `version > DRW::AC1018` 分支，
/// 参考实现见 .cache/opencadcodec 的 write_xref_table_flags()/read_xref_table_flags()。
///
/// 若 2007+ 仍按 2004- 的三段式写，真实 CAD（AutoCAD/浩辰）会把这个位置当 BitShort
/// 解读：本实现写出的 `00` 前缀正好等于「后随 16 位短整型」，于是记录整体前移 2 字节，
/// 该表项及其后所有对象全部错位 —— 浩辰随即判「文件损坏」。
void writeXrefTableFlags(dwgBitWriter& w, DRW::Version ver)
{
    if (ver > DRW::AC1018) {        // 2007+
        w.writeBS(0);
    } else {                        // 2004-
        w.writeB(false);            // bit6（referenced）
        w.writeBS(0);               // xrefindex
        w.writeB(false);            // bit4（refx dependent）
    }
}

/// 控制对象。结构（R2000）：
///   num_entries               BL 或 BS（见 CtrlKind）
///   [Dimstyle] num_morehandles RC
///   ownerhandle(码4) + xdicobj(码3)        ← CONTROL_HANDLE_STREAM
///   entries[num_entries](码2)              ← HANDLE_VECTOR
///   [Block] model_space(码3) + paper_space(码3)
///   [LType] byblock(码3) + bylayer(码3)
/// 注意：num_entries 必须是真实条目数；模型/图纸空间块与 ByBlock/ByLayer 线型
/// 不计入条目，而在尾部单独给出（AutoCAD 与读取端均据此解析）。
void encodeControl(Private& d, std::uint16_t otype, std::uint32_t handle, CtrlKind kind,
                   const std::vector<std::uint32_t>& entries,
                   std::uint32_t argA = 0, std::uint32_t argB = 0)
{
    Private::ObjBuilder b(d);
    d.writeTableCommon(b, otype, handle);
    if (kind == CtrlKind::Bs || kind == CtrlKind::LType || kind == CtrlKind::Dimstyle)
        b.w.writeBS(static_cast<dint16>(entries.size()));
    else
        b.w.writeBL(static_cast<dint32>(entries.size()));
    if (kind == CtrlKind::Dimstyle)
        b.w.writeRC(0);                         // num_morehandles（R2000b 起）
    // 2007+：控制对象在条目句柄之前多一个「是否有字符串区」位（读取端
    // DRW_ObjControl::parseDwg 的 `if (version > DRW::AC1018) int stringBit =
    // buf->getBit();`，intern/dwgreader.cpp:1334）。控制对象本身没有字符串区，
    // 写 0。漏写这 1 位会让其后全部条目句柄错位，症状是「控制对象找到了、
    // 但它列出的表记录全 not found」。
    if (d.version > DRW::AC1018)
        b.w.writeB(false);                      // string bit = 0（无字符串区）
    b.markTrailingFlagWritten();                // 该位已写出，finish() 不再补
    b.beginHandles();
    b.w.writeHandle(kHcSoftPtr, 0);             // ownerhandle（控制对象无属主）
    d.writeXdicObj(b);
    for (std::uint32_t h : entries)
        b.w.writeHandle(kHcSoftOwn, h);         // 条目：软属主
    if (kind == CtrlKind::Block) {
        b.w.writeHandle(kHcOwner, argA);        // model_space
        b.w.writeHandle(kHcOwner, argB);        // paper_space
    } else if (kind == CtrlKind::LType) {
        b.w.writeHandle(kHcOwner, argA);        // byblock
        b.w.writeHandle(kHcOwner, argB);        // bylayer
    }
    d.pushObject(handle, otype, b.finish());
}

void encodeLType(Private& d, const DRW_LType& ent, std::uint32_t handle)
{
    Private::ObjBuilder b(d);
    d.writeTableCommon(b, kLType, handle);
    b.w.writeT(ent.name, d.codec);
    writeXrefTableFlags(b.w, d.version);
    b.w.writeT(ent.desc, d.codec);
    double length = ent.length;
    if (length <= 0.0) {
        length = 0.0;
        for (double p : ent.path) length += std::fabs(p);
    }
    b.w.writeBD(length);
    b.w.writeRC(65);                    // 'A'
    b.w.writeRC(static_cast<std::uint8_t>(ent.path.size()));
    for (double p : ent.path) {
        b.w.writeBD(p);
        b.w.writeBS(0);
        b.w.writeRD(0.0); b.w.writeRD(0.0);
        b.w.writeBD(0.0); b.w.writeBD(0.0);
        b.w.writeBS(0);
    }
    // 尾部字符串区（读取端 drw_objects.cpp:544）：2004- 是固定 256 字节；2007+
    // 改为「haveStrArea 时 512 字节」，而 haveStrArea 由上面每个 dash 项的 bs2 位
    //（恒写 0）决定 —— 故 2007+ 一个字节都不写。照旧写 256 字节会让读取端把这些
    // 字节误当句柄流数据。
    if (d.version < DRW::AC1021)
        for (int i = 0; i < 256; ++i) b.w.writeRC(0);
    b.beginHandles();
    for (std::size_t i = 0; i < ent.path.size(); ++i) {
        b.w.writeHandle(kHcAbsolute, 0);
    }
    d.writeTableHandleTail(b, d.ltypeCtrl);
    if (!ent.path.empty()) {
        b.w.writeHandle(kHcAbsolute, 0);
        b.w.writeHandle(kHcAbsolute, 0);
    }
    b.w.writeHandle(kHcAbsolute, 0);
    d.pushObject(handle, kLType, b.finish());
}

void encodeLayer(Private& d, const DRW_Layer& ent, std::uint32_t handle)
{
    Private::ObjBuilder b(d);
    d.writeTableCommon(b, kLayer, handle);
    b.w.writeT(ent.name, d.codec);
    writeXrefTableFlags(b.w, d.version);
    std::uint16_t flags = 0;
    if (!ent.plotF) flags |= 1u << 4;
    flags |= static_cast<std::uint16_t>(DRW_LW_Conv::lineWidth2dwgInt(ent.lWeight) & 0x1F) << 5;
    b.w.writeBS(static_cast<dint16>(flags));
    d.writeColor(b, ent.color);             // 颜色（2004+ 为 CMC）
    b.beginHandles();
    d.writeTableHandleTail(b, d.layerCtrl);
    b.w.writeHandle(kHcAbsolute, 0);    // xref
    b.w.writeHandle(kHcAbsolute, 0);    // plot style
    // 2007+：图层句柄流在 plot style 与 lineType 之间多一个材质句柄
    //（读取端 drw_objects.cpp:689，`if (version > DRW::AC1018)`）。漏写会让读取端
    // 把 lineType 句柄当材质、再越界读 lineType。
    if (d.version > DRW::AC1018)
        b.w.writeHandle(kHcAbsolute, 0); // material（2007+）
    b.w.writeHandle(kHcAbsolute, d.ltypeHandle(ent.lineType));
    // 2013+：图层句柄流在 lineType 之后再加一个「视觉样式」句柄
    //（读取端需对应读掉；libredwg dwg.spec:3464 的 `SINCE (R_2013b)
    //  FIELD_HANDLE (visualstyle, 5, 348)`）。漏写会让严格解析器（libredwg /
    //  CAD 内核）在此越界读，其后所有对象判 CRC/尺寸不符。
    if (d.version > DRW::AC1024)
        b.w.writeHandle(kHcAbsolute, 0); // visualstyle（2013+，无 = NULL）
    d.pushObject(handle, kLayer, b.finish());
}

/// UTF-8 → UTF-16LE 码元。用于两处：R2007+ 表记录的扩展数据（EED 字符串）
/// 与 R2007+ 表格单元格的值（AcDbCellValue）——两者在文件里都是 UTF-16LE 词流。
std::vector<std::uint16_t> utf8ToUtf16Units(const std::string& utf8)
{
    std::vector<std::uint16_t> units;
    units.reserve(utf8.size());
    for (std::size_t i = 0; i < utf8.size();) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        std::uint32_t cp = 0;
        std::size_t len = 1;
        if (c < 0x80) { cp = c; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; len = 4; }
        else { cp = 0xFFFD; len = 1; }
        if (i + len > utf8.size()) { cp = 0xFFFD; len = utf8.size() - i; }
        for (std::size_t k = 1; k < len; ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3Fu);
        i += len;
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            units.push_back(static_cast<std::uint16_t>(0xD800u + (cp >> 10)));
            units.push_back(static_cast<std::uint16_t>(0xDC00u + (cp & 0x3FFu)));
        } else {
            units.push_back(static_cast<std::uint16_t>(cp));
        }
    }
    return units;
}

void encodeStyle(Private& d, const DRW_Textstyle& ent, std::uint32_t handle)
{
    // TrueType 文字样式的字体族名不在 font/bigFont 字段（DXF 3/4 组码）里，
    // 只存在于扩展数据 EED（应用 ACAD + DXF 1000 族名 / 1071 族标志）。
    // 不写回 EED，重存的文件在 AutoCAD / 浩辰等 CAD 里会回退成默认 SHX 字体。
    std::vector<std::uint8_t> eed;
    bool inAcad = false;
    for (DRW_Variant* v : ent.extData) {
        if (v == nullptr) continue;
        if (v->code() == 1001) {
            inAcad = (v->type() == DRW_Variant::STRING && *v->content.s == "ACAD");
            continue;
        }
        if (!inAcad) continue;
        switch (v->code()) {
        case 1000: {    // 字符串
            if (v->type() != DRW_Variant::STRING) break;
            if (d.version > DRW::AC1018) {
                // 2007+：UTF-16LE 词流，长度字段是**16 位词数**（RS 小端）。
                // 老代码在这里写 pre-2007 的「RC 长度 + BE16 代码页 + 单字节文本」，
                // 且用 codec.fromUtf8 取字节 —— 而 DWG 路径的编解码器是
                // DRW_ConvUTF16，其 fromUtf8 至今是空实现（恒返回空串），于是这里
                // 写出的是 length=0 的空字符串。两项叠加的结果是 EED 里的字体族名
                // 丢失（AutoCAD/浩辰 AUDIT 判为数据不完整 → 打开时提示需要修复）。
                const std::vector<std::uint16_t> units = utf8ToUtf16Units(*v->content.s);
                const std::size_t n = std::min<std::size_t>(units.size(), 0xFFFFu);
                eed.push_back(0x00);
                eed.push_back(static_cast<std::uint8_t>(n & 0xFFu));
                eed.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFFu));
                for (std::size_t k = 0; k < n; ++k) {
                    eed.push_back(static_cast<std::uint8_t>(units[k] & 0xFFu));
                    eed.push_back(static_cast<std::uint8_t>((units[k] >> 8) & 0xFFu));
                }
            } else {        // R13-R2004：码 0 + RC 长度 + BE16 代码页 + 编码字节
                const std::string encoded = d.codec.fromUtf8(*v->content.s);
                const std::size_t len = std::min<std::size_t>(encoded.size(), 255);
                eed.push_back(0x00);
                eed.push_back(static_cast<std::uint8_t>(len));
                eed.push_back(static_cast<std::uint8_t>(d.codepageId >> 8));
                eed.push_back(static_cast<std::uint8_t>(d.codepageId & 0xFF));
                eed.insert(eed.end(), encoded.begin(), encoded.begin() + static_cast<long>(len));
            }
            break;
        }
        case 1070: {    // 16 位整数：码 70 + RS
            if (v->type() != DRW_Variant::INTEGER) break;
            const std::uint16_t val = static_cast<std::uint16_t>(v->content.i);
            eed.push_back(70);
            eed.push_back(static_cast<std::uint8_t>(val & 0xFF));
            eed.push_back(static_cast<std::uint8_t>(val >> 8));
            break;
        }
        case 1071: {    // 32 位整数：码 71 + RL（TrueType 字体族标志）
            if (v->type() != DRW_Variant::INTEGER) break;
            const std::uint32_t val = static_cast<std::uint32_t>(v->content.i);
            eed.push_back(71);
            for (int k = 0; k < 4; ++k)
                eed.push_back(static_cast<std::uint8_t>((val >> (8 * k)) & 0xFF));
            break;
        }
        case 1040: {    // 实数：码 40 + RD
            if (v->type() != DRW_Variant::DOUBLE) break;
            eed.push_back(40);
            const double val = v->content.d;
            std::uint64_t bits = 0;
            std::memcpy(&bits, &val, sizeof(bits));
            for (int k = 0; k < 8; ++k)
                eed.push_back(static_cast<std::uint8_t>((bits >> (8 * k)) & 0xFF));
            break;
        }
        default:
            break;
        }
    }

    Private::ObjBuilder b(d);
    d.writeTableCommon(b, kStyle, handle, eed.empty() ? nullptr : &eed);
    b.w.writeT(ent.name, d.codec);
    writeXrefTableFlags(b.w, d.version);
    b.w.writeB(false);                  // 竖排
    b.w.writeB(false);                  // 形文件
    b.w.writeBD(ent.height);
    b.w.writeBD(ent.width);
    b.w.writeBD(ent.oblique);
    b.w.writeRC(0);
    b.w.writeBD(ent.lastHeight);
    b.w.writeT(ent.font, d.codec);
    b.w.writeT(ent.bigFont, d.codec);
    b.beginHandles();
    d.writeTableHandleTail(b, d.styleCtrl);
    b.w.writeHandle(kHcAbsolute, 0);    // xref
    d.pushObject(handle, kStyle, b.finish());
}

void encodeVport(Private& d, const DRW_Vport& ent, std::uint32_t handle)
{
    Private::ObjBuilder b(d);
    d.writeTableCommon(b, kVport, handle);
    b.w.writeT(ent.name, d.codec);
    writeXrefTableFlags(b.w, d.version);
    b.w.writeBD(ent.height);
    b.w.writeBD(ent.ratio);
    b.w.write2RD(ent.center.x, ent.center.y);
    b.w.write3BD(ent.viewTarget.x, ent.viewTarget.y, ent.viewTarget.z);
    b.w.write3BD(ent.viewDir.x, ent.viewDir.y, ent.viewDir.z);
    b.w.writeBD(ent.twistAngle);
    b.w.writeBD(ent.lensHeight);
    b.w.writeBD(ent.frontClip);
    b.w.writeBD(ent.backClip);
    b.w.writeB(false); b.w.writeB(false); b.w.writeB(false); b.w.writeB(false);
    b.w.writeRC(0);                     // renderMode
    // 2007+：renderMode 之后多一段默认光照块（读取端 drw_objects.cpp:1071，
    // `if (version > DRW::AC1018)`）：1 位 + 1 字节类型 + 两个 BD + 环境色 CMC。
    if (d.version > DRW::AC1018) {
        b.w.writeB(true);               // use default lights
        b.w.writeRC(0);                 // default lighting type
        b.w.writeBD(1.0);               // brightness
        b.w.writeBD(1.0);               // contrast
        d.writeColor(b, 256);           // ambient color（CMC，ByLayer）
    }
    b.w.write2RD(ent.lowerLeft.x, ent.lowerLeft.y);
    b.w.write2RD(ent.UpperRight.x, ent.UpperRight.y);
    b.w.writeB(false);                  // UCSFOLLOW
    b.w.writeBS(100);                   // circleZoom
    b.w.writeB(true);                   // fastZoom
    b.w.writeB(true); b.w.writeB(true); // ucsIcon
    b.w.writeB(false);                  // grid
    b.w.write2RD(10.0, 10.0);
    b.w.writeB(false); b.w.writeB(false);
    b.w.writeBS(0);                     // snapIsopair
    b.w.writeBD(0.0);                   // snapAngle
    b.w.write2RD(0.0, 0.0);
    b.w.write2RD(10.0, 10.0);
    b.w.writeB(false);
    b.w.writeB(false);
    b.w.write3BD(0.0, 0.0, 0.0);
    b.w.write3BD(1.0, 0.0, 0.0);
    b.w.write3BD(0.0, 1.0, 0.0);
    b.w.writeBD(0.0);                   // UCS elevation
    b.w.writeBS(0);                     // UCS orthographic type
    // 2007+：再补栅格行为两个 BS（读取端 drw_objects.cpp:1112，
    // `if (version > DRW::AC1018)`）。
    if (d.version > DRW::AC1018) {
        b.w.writeBS(0);                 // gridBehavior
        b.w.writeBS(0);                 // gridMajor
    }
    b.beginHandles();
    d.writeTableHandleTail(b, d.vportCtrl);
    b.w.writeHandle(kHcAbsolute, 0);    // xref
    // 2007+：XRef 与 namedUCS 之间还有三个句柄（读取端 DRW_Vport::parseDwg 的
    // `if (version > DRW::AC1018)`：背景、视觉样式、阳光）。漏写会让其后所有
    // 句柄不足、读到缓冲末尾，vp->parseDwg 的 isGood() 变假（实测 good=0）。
    if (d.version > DRW::AC1018) {
        b.w.writeHandle(kHcAbsolute, 0);    // background（背景）
        b.w.writeHandle(kHcAbsolute, 0);    // visual style
        b.w.writeHandle(kHcAbsolute, 0);    // sun
    }
    b.w.writeHandle(kHcAbsolute, 0);    // named UCS
    b.w.writeHandle(kHcAbsolute, 0);    // base UCS
    d.pushObject(handle, kVport, b.finish());
}

/// 标注样式表记录：R2000 起记录体内含完整标注参数。libdxfrw 读取端
/// 只解析名称、其余跳过（`DRW_Dimstyle::parseDwg` 读完 name 就直接按 objSize
/// 跳到句柄流），因此记录内部的字段错位在自家往返里**完全看不出来**；
/// AutoCAD / 浩辰等按规格完整解析的宿主会一路读穿这条记录，随即判文件损坏。
///
/// 记录布局必须与**表头里的同名标注变量逐字段对齐**（两者共用同一套
/// DIMxxx 定义），见 writeHeaderVars 的同名段落与读取端 drw_header.cpp
/// 2067-2185（其中 2007+ 的分支由 `version > DRW::AC1018` 控制）：
///   · 公共表项头之后是 name(TV) 与 xref 标志位（与其它表项同形，未随版本变化）；
///   · DIMPOST / DIMAPOST / DIMSCALE…DIMTM；
///   · 2007+：DIMFXL(BD) / DIMJOGANG(BD) / DIMTFILL(BS) / DIMTFILLCLR(CMC)；
///   · DIMTOL…DIMSE2（位）、DIMTAD / DIMZIN / DIMAZIN；
///   · 2007+：DIMARCSYM(BS)；
///   · DIMTXT…DIMALTRND、DIMCLRD/RE/RT（**2004+ 为 CMC，不是 BS**）；
///   · DIMADEC…DIMATFIT、2007+ DIMFXLON（位）、DIMLWD / DIMLWE、末尾标志位；
///   · 句柄流：xref / DIMTXSTY / DIMLDRBLK / DIMBLK / DIMBLK1 / DIMBLK2，
///     2007+ 再补 DIMLTYPE / DIMLTEX1 / DIMLTEX2。
void encodeDimstyle(Private& d, const DRW_Dimstyle& ent, std::uint32_t handle)
{
    Private::ObjBuilder b(d);
    const Private::DimStyleData& v = d.dimData;
    d.writeTableCommon(b, kDimstyle, handle);
    b.w.writeT(ent.name, d.codec);
    writeXrefTableFlags(b.w, d.version);
    b.w.writeT(std::string(), d.codec); // DIMPOST
    b.w.writeT(std::string(), d.codec); // DIMAPOST
    b.w.writeBD(v.scale);  b.w.writeBD(v.asz);  b.w.writeBD(v.exo);
    b.w.writeBD(v.dli);    b.w.writeBD(v.exe);  b.w.writeBD(v.rnd);
    b.w.writeBD(v.dle);    b.w.writeBD(v.tp);   b.w.writeBD(v.tm);
    // 2007+：DIMTM 与 DIMTOL 之间多 DIMFXL / DIMJOGANG / DIMTFILL /
    // DIMTFILLCLR（读取端 drw_header.cpp:2072-2078，本文件 writeHeaderVars 同段）。
    if (d.version > DRW::AC1018) {
        b.w.writeBD(0.0);               // DIMFXL
        b.w.writeBD(0.0);               // DIMJOGANG
        b.w.writeBS(0);                 // DIMTFILL
        d.writeColor(b, 0);             // DIMTFILLCLR（CMC）
    }
    b.w.writeB(v.tol != 0); b.w.writeB(v.lim != 0);
    b.w.writeB(v.tih != 0); b.w.writeB(v.toh != 0);
    b.w.writeB(v.se1 != 0); b.w.writeB(v.se2 != 0);
    b.w.writeBS(static_cast<dint16>(v.tad));
    b.w.writeBS(static_cast<dint16>(v.zin));
    b.w.writeBS(static_cast<dint16>(v.azin));
    // 2007+：DIMAZIN 之后是 DIMARCSYM（读取端 drw_header.cpp:2092-2094）。
    if (d.version > DRW::AC1018)
        b.w.writeBS(0);                 // DIMARCSYM
    b.w.writeBD(v.txt);    b.w.writeBD(v.cen);  b.w.writeBD(v.tsz);
    b.w.writeBD(v.altf);   b.w.writeBD(v.lfac); b.w.writeBD(v.tvp);
    b.w.writeBD(v.tfac);   b.w.writeBD(v.gap);  b.w.writeBD(v.altrnd);
    b.w.writeB(v.alt != 0);
    b.w.writeBS(static_cast<dint16>(v.altd));
    b.w.writeB(v.tofl != 0); b.w.writeB(v.sah != 0);
    b.w.writeB(v.tix != 0);  b.w.writeB(v.soxd != 0);
    // 三个标注颜色：读取端一律走 getCmColor(version)（<AC1018 为 BS、
    // 2004+ 为 BS+BL+RC 的 CMC）。与 LAYER 记录一样改用 writeColor，
    // 否则 2004+ 每条记录少 42 位。
    d.writeColor(b, v.clrd);            // DIMCLRD
    d.writeColor(b, v.clre);            // DIMCLRE
    d.writeColor(b, v.clrt);            // DIMCLRT
    b.w.writeBS(static_cast<dint16>(v.adec));
    b.w.writeBS(static_cast<dint16>(v.dec));
    b.w.writeBS(static_cast<dint16>(v.tdec));
    b.w.writeBS(static_cast<dint16>(v.altu));
    b.w.writeBS(static_cast<dint16>(v.alttd));
    b.w.writeBS(static_cast<dint16>(v.aunit));
    b.w.writeBS(static_cast<dint16>(v.frac));
    b.w.writeBS(static_cast<dint16>(v.lunit));
    b.w.writeBS(static_cast<dint16>(v.dsep));
    b.w.writeBS(static_cast<dint16>(v.tmove));
    b.w.writeBS(static_cast<dint16>(v.just));
    b.w.writeB(v.sd1 != 0); b.w.writeB(v.sd2 != 0);
    b.w.writeBS(static_cast<dint16>(v.tolj));
    b.w.writeBS(static_cast<dint16>(v.tzin));
    b.w.writeBS(static_cast<dint16>(v.altz));
    b.w.writeBS(static_cast<dint16>(v.alttz));
    b.w.writeB(v.upt != 0);
    b.w.writeBS(static_cast<dint16>(v.atfit));
    // 2007+：DIMATFIT 之后是 DIMFXLON（读取端 drw_header.cpp:2142-2145）。
    if (d.version > DRW::AC1018)
        b.w.writeB(false);              // DIMFXLON
    // R2010+：DIMFXLON 与 DIMLWD 之间还有 5 个字段（libredwg dwg.spec:4419-4429
    // 的 `IF_FREE_OR_SINCE (R_2010b)`：
    //   B DIMTXTDIRECTION / BD DIMALTMZF / T DIMALTMZS / BD DIMMZF / T DIMMZS）。
    // 两个 T 走独立字符串流（writeT 自动分流），故主数据流里只多出 1 位 + 2 个 BD。
    // 漏写会让 DIMSTYLE 记录自该处起整体错位，严格解析器（浩辰 / libredwg）
    // 在 AUDIT 时判该记录损坏。
    if (d.version > DRW::AC1021) {
        b.w.writeB(false);                      // DIMTXTDIRECTION
        b.w.writeBD(0.0);                       // DIMALTMZF
        b.w.writeT(std::string(), d.codec);     // DIMALTMZS（字符串流）
        b.w.writeBD(0.0);                       // DIMMZF
        b.w.writeT(std::string(), d.codec);     // DIMMZS（字符串流）
    }
    b.w.writeBS(static_cast<dint16>(v.lwd));
    b.w.writeBS(static_cast<dint16>(v.lwe));
    b.w.writeB(false);                  // flag0
    b.beginHandles();
    d.writeTableHandleTail(b, d.dimstyleCtrl);
    b.w.writeHandle(kHcAbsolute, 0);    // xref
    b.w.writeHandle(kHcAbsolute, d.styleHandle("Standard"));  // DIMTXSTY
    b.w.writeHandle(kHcAbsolute, 0);    // DIMLDRBLK
    b.w.writeHandle(kHcAbsolute, 0);    // DIMBLK
    b.w.writeHandle(kHcAbsolute, 0);    // DIMBLK1
    b.w.writeHandle(kHcAbsolute, 0);    // DIMBLK2
    // 2007+：DIMBLK2 之后还有三个线型句柄（读取端 drw_header.cpp:2163-2174）
    // —— 漏写会让记录句柄流比规格少 3 条，严格读取器越界。
    if (d.version > DRW::AC1018) {
        b.w.writeHandle(kHcAbsolute, 0);    // DIMLTYPE
        b.w.writeHandle(kHcAbsolute, 0);    // DIMLTEX1
        b.w.writeHandle(kHcAbsolute, 0);    // DIMLTEX2
    }
    d.pushObject(handle, kDimstyle, b.finish());
}

void encodeAppId(Private& d, const DRW_AppId& ent, std::uint32_t handle)
{
    Private::ObjBuilder b(d);
    d.writeTableCommon(b, kAppId, handle);
    b.w.writeT(ent.name, d.codec);
    writeXrefTableFlags(b.w, d.version);
    b.w.writeRC(0);
    b.beginHandles();
    d.writeTableHandleTail(b, d.appidCtrl);
    b.w.writeHandle(kHcAbsolute, 0);    // xref
    d.pushObject(handle, kAppId, b.finish());
}

// =============================================================================
//  命名对象字典链（DICTIONARY / DICTIONARYWDFLT / PLACEHOLDER）
//
//  真实 AutoCAD 文件必然包含：根字典 NOD，下挂 ACAD_GROUP / ACAD_LAYOUT /
//  ACAD_MLINESTYLE / ACAD_PLOTSETTINGS / ACAD_PLOTSTYLENAME，
//  后者为 DICTIONARYWDFLT 并以 PLACEHOLDER 作为默认项。
//  字典的名字（TV）写在数据流，条目句柄写在句柄流，两者按索引对应。
// =============================================================================
namespace {

/// 字典条目：名字 → 对象句柄
using DictEntry = std::pair<std::string, std::uint32_t>;

/// 写字典的公共头部：numitems(BL) + cloning(BS 281) + is_hardowner(RC 280)
/// 以及数据流中的全部名字。
void writeDictHead(Private& d, Private::ObjBuilder& b,
                   const std::vector<DictEntry>& entries)
{
    b.w.writeBL(static_cast<dint32>(entries.size()));   // numitems
    b.w.writeBS(1);                                     // cloning（281）
    b.w.writeRC(0);                                     // is_hardowner（280）
    for (const DictEntry& e : entries)
        b.w.writeT(e.first, d.codec);                   // 名字写在数据流
}

/// 写句柄流中的字典条目（码 2 = 软属主）
void writeDictItems(Private::ObjBuilder& b, const std::vector<DictEntry>& entries)
{
    for (const DictEntry& e : entries)
        b.w.writeHandle(kHcSoftOwn, e.second);
}

/// DICTIONARY（固定类型 42）
void encodeDictionary(Private& d, std::uint32_t handle, std::uint32_t owner,
                      const std::vector<DictEntry>& entries)
{
    Private::ObjBuilder b(d);
    d.writeTableCommon(b, kDictionary, handle);
    writeDictHead(d, b, entries);
    b.beginHandles();
    b.w.writeHandle(kHcSoftPtr, owner);     // ownerhandle（NOD 的子字典属主为 NOD）
    d.writeXdicObj(b);
    writeDictItems(b, entries);
    d.pushObject(handle, kDictionary, b.finish());
}

/// DICTIONARYWDFLT（类 500）：多一个 defaultid 句柄
void encodeDictionaryWdf(Private& d, std::uint32_t handle, std::uint32_t owner,
                         const std::vector<DictEntry>& entries, std::uint32_t defaultId)
{
    Private::ObjBuilder b(d);
    d.writeTableCommon(b, kClassDictWdf, handle);
    writeDictHead(d, b, entries);
    b.beginHandles();
    b.w.writeHandle(kHcSoftPtr, owner);
    d.writeXdicObj(b);
    writeDictItems(b, entries);
    b.w.writeHandle(kHcAbsolute, defaultId);    // defaultid（码 5）
    d.pushObject(handle, kClassDictWdf, b.finish());
}

/// PLACEHOLDER（类 501）：仅公共数据，无自有字段
void encodePlaceholder(Private& d, std::uint32_t handle, std::uint32_t owner)
{
    Private::ObjBuilder b(d);
    d.writeTableCommon(b, kClassPlaceholder, handle);
    b.beginHandles();
    b.w.writeHandle(kHcSoftPtr, owner);
    d.writeXdicObj(b);
    d.pushObject(handle, kClassPlaceholder, b.finish());
}

/// MLINESTYLE：多线样式。
/// 生成标准样式 "STANDARD"：两条平行线（偏移 ±0.5）、角度 90°、ByLayer 颜色。
void encodeMlineStyle(Private& d, std::uint32_t handle, std::uint32_t ownerDict,
                      const std::string& name)
{
    Private::ObjBuilder b(d);
    d.writeNamedHeadStart(b, kMlineStyle, handle);
    b.w.writeT(name, d.codec);                          // name（2）
    b.w.writeT(std::string(), d.codec);                 // description（0）
    b.w.writeBS(0);                                     // flag（70）
    // fill_color 是 **CMC**（libredwg dwg.spec:4537 的 `FIELD_CMC (fill_color, 62)`），
    // 不是纯 BS —— 漏写其后的 BL/RC 会让 start_angle 等字段整体前移，
    // 严格解析器（libredwg / CAD 内核）随即报 CMC 非法。原先写作 writeBS(256)。
    d.writeColor(b, 256);                               // fill_color（CMC 62）= ByLayer
    const double halfPi = 1.5707963267948966;
    b.w.writeBD(halfPi);                                // start_angle（51）= 90°
    b.w.writeBD(halfPi);                                // end_angle（52）= 90°
    // num_lines = 0：STANDARD 样式在 AutoCAD / 浩辰里就是无自定线（实测浩辰产物同为 0），
    // 且 lines[].lt_index 属 R2018 字段，R2013 写出会多出数据。
    b.w.writeRC(0);                                     // num_lines（71）
    b.beginHandles();
    d.writeNamedHeadTail(b, ownerDict);
    d.pushObject(handle, kMlineStyle, b.finish());
}

/// DICTIONARYVAR：AcDbVariableDictionary 的条目（schema + 字符串值）。
void encodeDictVar(Private& d, std::uint32_t handle, std::uint32_t ownerDict,
                   const std::string& value)
{
    Private::ObjBuilder b(d);
    d.writeNamedHeadStart(b, kClassDictVar, handle);
    b.w.writeRC(0);                                     // schema（280）
    b.w.writeT(value, d.codec);                         // strvalue（1）
    b.beginHandles();
    d.writeNamedHeadTail(b, ownerDict);
    d.pushObject(handle, kClassDictVar, b.finish());
}

/// XRECORD：扩展记录。生成空 xdata 的最小合法形式
/// （xdata_size=0、cloning=1、无 objid 句柄）——用于 NOD 的
/// ACDBHEADERROUNDTRIPXREC 条目。
void encodeXrecord(Private& d, std::uint32_t handle, std::uint32_t ownerDict)
{
    Private::ObjBuilder b(d);
    d.writeNamedHeadStart(b, kXrecord, handle);
    b.w.writeBL(0);                                     // xdata_size
    b.w.writeBS(1);                                     // cloning（280）
    b.w.writeBL(0);                                     // num_objid_handles
    b.beginHandles();
    d.writeNamedHeadTail(b, ownerDict);
    d.pushObject(handle, kXrecord, b.finish());
}

/// LAYOUT（类 502）：图纸布局。字段序列依样例实测的解码顺序。
/// 打印设置内联在本对象内；blockHeader 指向该布局对应的块记录。
void encodeLayout(Private& d, std::uint32_t handle, std::uint32_t owner,
                  std::uint32_t blockHeader, const std::string& name)
{
    Private::ObjBuilder b(d);
    d.writeTableCommon(b, kClassLayout, handle);
    // ---- 打印设置（内联） ----
    b.w.writeT(std::string(), d.codec);                     // printer_cfg_file
    b.w.writeT("none_device", d.codec);                     // paper_size
    b.w.writeBS(0x2eb0);                                    // plot_flags（BSx 即 BS）
    b.w.writeBD(6.35);                                      // left_margin
    b.w.writeBD(19.05);                                     // bottom_margin
    b.w.writeBD(6.35001);                                   // right_margin
    b.w.writeBD(19.05);                                     // top_margin
    b.w.writeBD(215.9);                                     // paper_width
    b.w.writeBD(279.4);                                     // paper_height
    b.w.writeT("ANSI_A_(8.50_x_11.00_Inches)", d.codec);    // canonical_media_name
    b.w.write2BD(0.0, 0.0);                                 // plot_origin
    b.w.writeBS(0);                                         // plot_paper_unit
    b.w.writeBS(1);                                         // plot_rotation_mode
    b.w.writeBS(0);                                         // plot_type
    b.w.write2BD(0.0, 0.0);                                 // plot_window_ll
    b.w.write2BD(0.0, 0.0);                                 // plot_window_ur
    // plotview_name(T)：按规范仅 **R2002 及更早**（libredwg dwg.spec 的
    // `UNTIL (R_2002) FIELD_T (plotview_name)`）。R2004 起该字段改为句柄
    // plotsettings.plotview（见下方句柄流）。AC1018 若在此多写一个空串，
    // 主流会多出 2 位，其后 paper_units/drawing_units/… 与布局本体整体移位，
    // 严格读取器随即报 bit_read_* 越界与 num_viewports 荒谬值。
    // AC1021 即便多写也只落在**独立字符串区**里（主流不受影响），故此前只在
    // AC1018 上暴露。
    if (d.version <= DRW::AC1015)
        b.w.writeT(std::string(), d.codec);                 // plotview_name（仅 2002-）
    b.w.writeBD(1.0);                                       // paper_units
    b.w.writeBD(78.4365);                                   // drawing_units
    b.w.writeT(std::string(), d.codec);                     // stylesheet
    b.w.writeBS(0);                                         // std_scale_type
    b.w.writeBD(0.0127492);                                 // std_scale_factor
    b.w.write2BD(0.0, 0.0);                                 // paper_image_origin
    // R2004（AC1018）起打印设置尾部多三项 shadeplot 参数（规范 SINCE R_2004a）。
    // 漏写会让其后整段（布局本体 + 句柄流）前移，读取端随之全部错位。
    if (d.version > DRW::AC1015) {
        b.w.writeBS(0);                                     // shadeplot_type
        b.w.writeBS(0);                                     // shadeplot_reslevel
        b.w.writeBS(0);                                     // shadeplot_customdpi
    }
    // ---- 布局本体 ----
    b.w.writeT(name, d.codec);                              // layout_name
    b.w.writeBS(0);                                         // tab_order
    b.w.writeBS(1);                                         // layout_flags（BSx）
    b.w.write3BD(0.0, 0.0, 0.0);                            // INSBASE
    b.w.write2RD(0.0, 0.0);                                 // LIMMIN
    b.w.write2RD(12.0, 9.0);                                // LIMMAX
    b.w.write3BD(0.0, 0.0, 0.0);                            // UCSORG
    b.w.write3BD(1.0, 0.0, 0.0);                            // UCSXDIR
    b.w.write3BD(0.0, 1.0, 0.0);                            // UCSYDIR
    b.w.writeBD(0.0);                                       // ucs_elevation
    b.w.writeBS(0);                                         // UCSORTHOVIEW
    b.w.write3BD(0.0, 0.0, 0.0);                            // EXTMIN
    b.w.write3BD(0.0, 0.0, 0.0);                            // EXTMAX
    // R2004+：布局本体末尾多一个视图口数（规范 SINCE R_2004a）。
    if (d.version > DRW::AC1015) b.w.writeBL(0);            // num_viewports
    b.beginHandles();
    b.w.writeHandle(kHcSoftPtr, owner);                     // ownerhandle = ACAD_LAYOUT 字典
    d.writeXdicObj(b);
    // 与上面 plotview_name 对应：R2004+ 的 plotview 是句柄（按规范顺序排在实体本体
    // 句柄之前）；R2007+（AC1021）再补一个 shadeplot 句柄（SINCE R_2007a）。
    if (d.version > DRW::AC1015) b.w.writeHandle(kHcSoftPtr, 0);   // plotsettings.plotview
    if (d.version > DRW::AC1018) b.w.writeHandle(kHcSoftPtr, 0);   // plotsettings.shadeplot
    b.w.writeHandle(kHcSoftPtr, blockHeader);               // block_header（H 330）
    b.w.writeHandle(kHcSoftPtr, 0);                         // active_viewport（H 331）
    b.w.writeHandle(kHcAbsolute, 0);                        // base_ucs（H 346）
    b.w.writeHandle(kHcAbsolute, 0);                        // named_ucs（H 345）
    d.pushObject(handle, kClassLayout, b.finish());
}

} // namespace

/// BLOCK / ENDBLK 实体
void encodeBlockEntity(Private& d, Private::Blk& blk, bool isEnd)
{
    Private::ObjBuilder b(d);
    DRW_Point attrs;                    // 仅借具体实体类型携带公共属性
    attrs.layer = "0";
    attrs.color = 0;                    // ByBlock
    attrs.lineType = "ByLayer";
    attrs.lWeight = DRW_LW_Conv::widthDefault;
    const std::uint32_t handle = isEnd ? blk.endblk : blk.blockEnt;
    const std::uint16_t otype = isEnd ? kEndblk : kBlock;
    // 与 AutoCAD 实测一致：*Model_Space 的 BLOCK/ENDBLK 用 entmode 2（属主隐式 =
    // BLOCK_RECORD_MSPACE）、*Paper_Space 用 1，其余块用 0 + 显式属主句柄。
    // 读取端据"未读到 owner"（parentHandle 保持 0）识别空间块，无需写空属主。
    const std::uint32_t owner = blk.record;
    const int entmode = d.entmodeFor(owner);
    d.writeEntityCommon(b, otype, attrs, handle, entmode);
    if (!isEnd)
        b.w.writeT(blk.name, d.codec);
    d.writeEntityHandles(b, owner, "0", entmode);
    d.pushObject(handle, otype, b.finish());
}

void encodeBlockRecord(Private& d, Private::Blk& blk)
{
    Private::ObjBuilder b(d);
    d.writeTableCommon(b, kBlockHeader, blk.record);
    b.w.writeT(blk.name, d.codec);
    writeXrefTableFlags(b.w, d.version);
    b.w.writeB(blk.name.size() > 1 && blk.name[0] == '*'); // 匿名块
    b.w.writeB(false);                  // 含 ATTDEF
    b.w.writeB(false);                  // Xref
    b.w.writeB(false);                  // 覆盖式 Xref
    b.w.writeB(false);                  // 已加载 Xref
    // 2004+：其后多一个 BL「块内实体数」，句柄流里再逐一给出这些实体
    //（读取端 DRW_BlockRecord::parseDwg 的 `if (version > DRW::AC1015)`）。
    if (d.version > DRW::AC1015)
        b.w.writeBL(static_cast<dint32>(blk.ents.size()));
    b.w.write3BD(0.0, 0.0, 0.0);        // 基点
    b.w.writeT(std::string(), d.codec); // Xref 路径
    b.w.writeRC(0);                     // 插入计数终止（读取端按 0 截止）
    b.w.writeT(std::string(), d.codec); // 块说明
    b.w.writeBL(0);                     // 预览数据
    // 2007+：预览数据之后还有三个字段（读取端 DRW_Block_Record::parseDwg 的
    // `if (version > DRW::AC1018)`）：插入单位 BS、可否分解位 B、块缩放 RC。
    // 漏写会让其后 setPosition(objSize) 之前的游标错位，块记录 isGood() 变假。
    if (d.version > DRW::AC1018) {
        b.w.writeBS(0);                 // insUnits
        b.w.writeB(false);              // canExplode
        b.w.writeRC(0);                 // bkScaling
    }
    b.beginHandles();
    d.writeTableHandleTail(b, d.blockCtrl);
    b.w.writeHandle(kHcAbsolute, 0);            // Null handle
    b.w.writeHandle(kHcOwner, blk.blockEnt);    // BLOCK 实体（码 3）
    // 实体链首尾：空间块同样必须指向真实图元（样例 *Model_Space 为 4.1.8D/4.1.92）。
    // 置 0 会让读取器认为块内无图元 —— 文件能打开但图形区空白。
    if (d.version > DRW::AC1015) {                // 2004+：逐一列出块内实体
        // 句柄码为 3（硬属主）：实测真实 R2004 文件与其它第三方写出器
        // 在此处均用码 3，用码 4 会被严格读取器判为引用类型不符。
        for (std::uint32_t h : blk.ents)
            b.w.writeHandle(kHcOwner, h);
    } else {
        b.w.writeHandle(kHcSoftPtr, blk.first); // first_entity（码 4）
        b.w.writeHandle(kHcSoftPtr, blk.last);  // last_entity（码 4）
    }
    b.w.writeHandle(kHcOwner, blk.endblk);      // ENDBLK 实体（码 3）
    // 布局对象：模型/图纸空间块各引用自己的 LAYOUT（样例 *Model_Space.layout = LAYOUT
    // "Model"、*Paper_Space.layout = LAYOUT "Layout1"），其余块为空。
    std::uint32_t layout = 0;
    if (blk.space) {
        if (blk.name == "*Model_Space") layout = d.layoutModel;
        else if (blk.name == "*Paper_Space") layout = d.layoutPaper;
        else if (blk.name == "*Paper_Space0") layout = d.layoutPaper2;
    }
    b.w.writeHandle(kHcAbsolute, layout);       // 布局（码 5）
    d.pushObject(blk.record, kBlockHeader, b.finish());
}

} // namespace

// =============================================================================
//  基础设施初始化与收尾
// =============================================================================
void dwgW::Private::initInfrastructure()
{
    // 控制对象
    ltypeCtrl = allocHandle();
    layerCtrl = allocHandle();
    styleCtrl = allocHandle();
    vportCtrl = allocHandle();
    dimstyleCtrl = allocHandle();
    appidCtrl = allocHandle();
    blockCtrl = allocHandle();
    viewCtrl = allocHandle();
    ucsCtrl = allocHandle();
    vpEntHeaderCtrl = allocHandle();        // 视口实体头控制（空对象，类型 0x46）

    // 默认表记录（句柄先占；若回调未覆盖，收尾时按默认值编码）
    ltypeByLayer = allocHandle();
    ltypeByBlock = allocHandle();
    ltypeContinuous = allocHandle();
    layer0 = allocHandle();
    styleStandard = allocHandle();
    vportActive = allocHandle();
    dimstyleStandard = allocHandle();
    appidAcad = allocHandle();

    layerMap["0"] = layer0;
    styleMap["Standard"] = styleStandard;
    ltypeMap["ByLayer"] = ltypeByLayer;
    ltypeMap["ByBlock"] = ltypeByBlock;
    ltypeMap["Continuous"] = ltypeContinuous;

    // 模型/图纸空间块
    mspace.name = "*Model_Space";
    mspace.space = true;
    mspace.record = allocHandle();
    mspace.blockEnt = allocHandle();
    pspace.name = "*Paper_Space";
    pspace.space = true;
    pspace.record = allocHandle();
    pspace.blockEnt = allocHandle();
    pspace0.name = "*Paper_Space0";     // 第二个图纸布局（LAYOUT "Layout2"）的块
    pspace0.space = true;
    pspace0.record = allocHandle();
    pspace0.blockEnt = allocHandle();

    // 命名对象字典链与 LAYOUT 的句柄必须在块记录编码之前确定：BLOCK_HEADER.layout
    // 会引用对应的 LAYOUT 对象（样例 *Model_Space.layout = LAYOUT "Model"）。
    nod = allocHandle();
    dictGroup = allocHandle();
    dictLayout = allocHandle();
    dictMline = allocHandle();
    dictPlotSettings = allocHandle();
    dictPlotStyleName = allocHandle();
    placeholder = allocHandle();
    layoutModel = allocHandle();
    layoutPaper = allocHandle();
    layoutPaper2 = allocHandle();

    // 其余标准命名对象（NOD 条目；样例 NOD 共 9 项）
    dictColor = allocHandle();          // ACAD_COLOR
    dictMaterial = allocHandle();       // ACAD_MATERIAL
    mlineStyle = allocHandle();         // MLINESTYLE "STANDARD"
    dictVar = allocHandle();            // AcDbVariableDictionary
    headerXrec = allocHandle();         // 字典（ACDBHEADERROUNDTRIPXREC）
    dictVars.assign(4, 0);              // DIMASSOC / HIDETEXT / LIGHTINGUNITS / PROJECTNAME
    for (std::uint32_t& h : dictVars) h = allocHandle();
}

void dwgW::Private::encodeBlockPair(Blk& blk)
{
    if (blk.endblk == 0) blk.endblk = allocHandle();
    if (!blk.encoded) {
        encodeBlockEntity(*this, blk, false);
        encodeBlockEntity(*this, blk, true);
        encodeBlockRecord(*this, blk);
        blk.encoded = true;
    }
}

void dwgW::Private::closeCurrentBlock()
{
    if (!current) return;
    encodeBlockPair(*current);
    current = nullptr;
}

/// 回调未覆盖的默认表记录，按标准值编码。
void dwgW::Private::encodePendingDefaults()
{
    auto missing = [this](std::uint32_t h) { return encodedHandles.count(h) == 0; };

    if (missing(ltypeByLayer)) {
        DRW_LType lt;
        lt.name = "ByLayer";
        lt.desc = "Solid line";
        encodeLType(*this, lt, ltypeByLayer);
    }
    if (missing(ltypeByBlock)) {
        DRW_LType lt;
        lt.name = "ByBlock";
        lt.desc = "";
        encodeLType(*this, lt, ltypeByBlock);
    }
    if (missing(ltypeContinuous)) {
        DRW_LType lt;
        lt.name = "Continuous";
        lt.desc = "Solid line";
        encodeLType(*this, lt, ltypeContinuous);
    }
    if (missing(layer0)) {
        DRW_Layer la;
        la.name = "0";
        la.color = 7;
        la.lineType = "Continuous";
        la.plotF = true;
        la.lWeight = DRW_LW_Conv::widthDefault;
        encodeLayer(*this, la, layer0);
    }
    if (missing(styleStandard)) {
        DRW_Textstyle st;
        st.name = "Standard";
        st.font = "txt";
        st.bigFont = "";
        st.height = 0.0;
        st.width = 1.0;
        encodeStyle(*this, st, styleStandard);
    }
    if (missing(vportActive)) {
        DRW_Vport vp;
        vp.name = "*ACTIVE";
        vp.ratio = 1.6;
        if (hasExt) {
            // 视图中心取包围盒中心、高度取最大跨度（留约 1.1% 边距）。
            // 与 AutoCAD 打开文件时的初始视图一致，避免打开后缩放异常。
            vp.center.x = (extMin.x + extMax.x) * 0.5;
            vp.center.y = (extMin.y + extMax.y) * 0.5;
            const double w = extMax.x - extMin.x;
            const double h = extMax.y - extMin.y;
            double span = (w > h) ? w : h;
            if (span <= 0.0) span = 1.0;
            vp.height = span * 1.011;
        } else {
            vp.height = 9.0;
        }
        encodeVport(*this, vp, vportActive);
    }
    if (missing(dimstyleStandard)) {
        DRW_Dimstyle ds;
        ds.name = "Standard";
        encodeDimstyle(*this, ds, dimstyleStandard);
    }
    if (missing(appidAcad)) {
        DRW_AppId ai;
        ai.name = "ACAD";
        encodeAppId(*this, ai, appidAcad);
    }

    // ---- 命名对象字典链 --------------------------------------------------------
    // 根字典 NOD + 标准子字典 + PLACEHOLDER（AutoCAD 文件中必然存在）
    // 句柄已在 initInfrastructure() 中预分配（BLOCK_HEADER.layout 需提前引用）。
    encodeLayout(*this, layoutPaper, dictLayout, pspace.record, "Layout1");
    encodeLayout(*this, layoutPaper2, dictLayout, pspace0.record, "Layout2");
    encodeLayout(*this, layoutModel, dictLayout, mspace.record, "Model");
    encodePlaceholder(*this, placeholder, dictPlotStyleName);
    encodeMlineStyle(*this, mlineStyle, dictMline, "STANDARD");
    encodeDictionaryWdf(*this, dictPlotStyleName, nod,
                        { { "Normal", placeholder } }, placeholder);
    encodeDictionary(*this, dictGroup, nod, {});
    encodeDictionary(*this, dictLayout, nod,
                     { { "Layout1", layoutPaper },
                       { "Layout2", layoutPaper2 },
                       { "Model", layoutModel } });
    encodeDictionary(*this, dictMline, nod, { { "STANDARD", mlineStyle } });
    encodeDictionary(*this, dictPlotSettings, nod, {});
    encodeDictionary(*this, dictColor, nod, {});
    encodeDictionary(*this, dictMaterial, nod, {});
    // AcDbVariableDictionary 的变量。前三个与样例一致（DIMASSOC/HIDETEXT/
    // PROJECTNAME），第四个 LIGHTINGUNITS 是本写出器额外声明的：
    //
    // DWG 里 LIGHT 对象**是否携带光度扩展块**由图纸的 LIGHTINGUNITS 决定
    // （libredwg dwg2.spec 的 `DWG_ENTITY (LIGHT)` → DECODER 段即按
    //  `LIGHTINGUNITS == "2"` 置 is_photometric，AutoCAD 亦然）。本写出器给光源
    // 写的是**标准光照**字段、不写光度块；若图纸按默认值被当成「光度单位」，
    // 严格的读取端会照光度布局继续读，把光源对象后面的字节当光度字段 ——
    // 光源解析崩掉后在图形区就看不见了。显式写 "0"（标准光照）与本写出器的
    // 光源布局自洽，也与源图（老格式、标准光照）一致。
    encodeDictVar(*this, dictVars[0], dictVar, "2");     // DIMASSOC
    encodeDictVar(*this, dictVars[1], dictVar, "1");     // HIDETEXT
    encodeDictVar(*this, dictVars[2], dictVar, "0");     // LIGHTINGUNITS = 标准光照
    encodeDictVar(*this, dictVars[3], dictVar, "");      // PROJECTNAME
    encodeDictionary(*this, dictVar, nod,
                     { { "DIMASSOC", dictVars[0] },
                       { "HIDETEXT", dictVars[1] },
                       { "LIGHTINGUNITS", dictVars[2] },
                       { "PROJECTNAME", dictVars[3] } });
    // 样例中 NOD 的 "ACDBHEADERROUNDTRIPXREC" 条目指向的是 DICTIONARY（非 XRECORD），
    // 类型必须一致，否则严格读取器判定文件损坏。此处按样例写空字典。
    encodeDictionary(*this, headerXrec, nod, {});
    encodeDictionary(*this, nod, 0,
                     { { "ACAD_COLOR", dictColor },
                       { "ACAD_GROUP", dictGroup },
                       { "ACAD_LAYOUT", dictLayout },
                       { "ACAD_MATERIAL", dictMaterial },
                       { "ACAD_MLINESTYLE", dictMline },
                       { "ACAD_PLOTSETTINGS", dictPlotSettings },
                       { "ACAD_PLOTSTYLENAME", dictPlotStyleName },
                       { "ACDBHEADERROUNDTRIPXREC", headerXrec },
                       { "AcDbVariableDictionary", dictVar } });

    // 控制对象（此时成员句柄表已齐）。
    // 条目 = 真实表格记录；模型/图纸空间块与 ByBlock/ByLayer 线型作为尾部字段单独给出。
    std::vector<std::uint32_t> ltypes;
    for (const auto& [name, h] : ltypeMap)
        if (h != ltypeByLayer && h != ltypeByBlock) ltypes.push_back(h);
    encodeControl(*this, kLTypeControl, ltypeCtrl, CtrlKind::LType, ltypes,
                  ltypeByBlock, ltypeByLayer);

    std::vector<std::uint32_t> layers;
    for (const auto& [name, h] : layerMap) layers.push_back(h);
    encodeControl(*this, kLayerControl, layerCtrl, CtrlKind::Bl, layers);

    std::vector<std::uint32_t> styles;
    for (const auto& [name, h] : styleMap) styles.push_back(h);
    encodeControl(*this, kStyleControl, styleCtrl, CtrlKind::Bl, styles);

    std::vector<std::uint32_t> vports;
    vports.push_back(vportActive);
    encodeControl(*this, kVportControl, vportCtrl, CtrlKind::Bs, vports);

    std::vector<std::uint32_t> dimstyles;
    dimstyles.push_back(dimstyleStandard);
    encodeControl(*this, kDimstyleControl, dimstyleCtrl, CtrlKind::Dimstyle, dimstyles);

    std::vector<std::uint32_t> appids;
    appids.push_back(appidAcad);
    encodeControl(*this, kAppIdControl, appidCtrl, CtrlKind::Bs, appids);

    std::vector<std::uint32_t> blkHeaders;
    // *Paper_Space0（第二个图纸布局块）必须登记在条目列表中，否则其块记录将成为
    // 无人引用的孤儿对象 —— 容错读取器不报错，但 ODA 内核会判定文件损坏。
    blkHeaders.push_back(pspace0.record);
    for (const auto& [name, blk] : blocks) blkHeaders.push_back(blk.record);
    encodeControl(*this, kBlockControl, blockCtrl, CtrlKind::Block, blkHeaders,
                  mspace.record, pspace.record);

    encodeControl(*this, kViewControl, viewCtrl, CtrlKind::Bl, {});
    encodeControl(*this, 0x3Eu /*UCS control*/, ucsCtrl, CtrlKind::Bs, {});
    // VX_CONTROL（0x46，视口实体头控制）是 **R13~R2000** 的对象
    //（libredwg header_variables.spec:541-543 的
    //  `VERSIONS (R_13b1, R_2000) { FIELD_HANDLE (VX_CONTROL_OBJECT, 3, 0); }`，
    //  dwg.h 亦注 `VX_CONTROL_OBJECT /*!< r11-r2000 code 3 */`）。R2004+ 不写出——
    //  浩辰 / AutoCAD 的 R2004+ 文件里都没有它，多写会让严格 AUDIT 判出多余对象。
    if (version <= DRW::AC1015)
        encodeControl(*this, 0x46 /*视口实体头控制*/, vpEntHeaderCtrl, CtrlKind::Bs, {});
}

// =============================================================================
//  表记录写出（dwgW 公有接口）
// =============================================================================
dwgW::dwgW(const char* name)
    : d(new Private)
{
    d->fileName = name;
}

dwgW::~dwgW()
{
    delete d;
}

void dwgW::setThumbnail(std::vector<std::uint8_t> data)
{
    d->thumbnail = std::move(data);
}

bool dwgW::writeLineType(DRW_LType* ent)
{
    std::string upper = ent->name;
    for (char& c : upper)
        c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    if (upper == "BYLAYER" || upper == "BYBLOCK" || upper == "CONTINUOUS")
        return true;                    // 内置默认线型已预留句柄，收尾时编码
    if (Private::findName(d->ltypeMap, ent->name) != 0) return true;
    const std::uint32_t h = d->allocHandle();
    d->ltypeMap[ent->name] = h;
    encodeLType(*d, *ent, h);
    return true;
}

bool dwgW::writeLayer(DRW_Layer* ent)
{
    auto it = d->layerMap.find(ent->name);
    if (it != d->layerMap.end()) {
        if (d->encodedHandles.count(it->second)) return true;
        encodeLayer(*d, *ent, it->second);      // 复用预留句柄编码真实数据
        return true;
    }
    const std::uint32_t h = d->allocHandle();
    d->layerMap[ent->name] = h;
    encodeLayer(*d, *ent, h);
    return true;
}

bool dwgW::writeTextstyle(DRW_Textstyle* ent)
{
    auto it = d->styleMap.find(ent->name);
    if (it != d->styleMap.end()) {
        if (d->encodedHandles.count(it->second)) return true;
        encodeStyle(*d, *ent, it->second);
        return true;
    }
    const std::uint32_t h = d->allocHandle();
    d->styleMap[ent->name] = h;
    encodeStyle(*d, *ent, h);
    return true;
}

bool dwgW::writeDimstyle(DRW_Dimstyle* ent)
{
    if (ent->name == "Standard") {
        if (d->encodedHandles.count(d->dimstyleStandard)) return true;
        encodeDimstyle(*d, *ent, d->dimstyleStandard);
        return true;
    }
    if (d->dimstyleMap.count(ent->name)) return true;
    const std::uint32_t h = d->allocHandle();
    d->dimstyleMap[ent->name] = h;
    encodeDimstyle(*d, *ent, h);
    return true;
}

bool dwgW::writeAppId(DRW_AppId* ent)
{
    if (ent->name == "ACAD") {
        if (d->encodedHandles.count(d->appidAcad)) return true;
        encodeAppId(*d, *ent, d->appidAcad);
        return true;
    }
    if (d->encodedHandles.count(d->appidAcad) && false) return true;
    const std::uint32_t h = d->allocHandle();
    encodeAppId(*d, *ent, h);
    return true;
}

bool dwgW::writeVport(DRW_Vport* ent)
{
    // VPORT_CONTROL 的条目列表使用预分配的 vportActive 句柄，记录必须复用同一
    // 句柄，否则控制表指向一个不存在的对象（悬空引用）：读取端据此找不到 VPORT，
    // vportmap 为空 → addVport 不回调，视图参数丢失（打开时缩放异常）。
    // 名称大小写兼容：AutoCAD 写 *Active，内部默认值为 *ACTIVE。
    if (ent->name == "*ACTIVE" || ent->name == "*Active") {
        if (d->encodedHandles.count(d->vportActive)) return true;
        encodeVport(*d, *ent, d->vportActive);
        return true;
    }
    const std::uint32_t h = d->allocHandle();
    encodeVport(*d, *ent, h);
    return true;
}

bool dwgW::writeBlockRecord(std::string name)
{
    if (d->blocks.count(name)) return true;
    Private::Blk blk;
    blk.name = name;
    blk.record = d->allocHandle();
    d->blocks[name] = blk;
    return true;
}

bool dwgW::writeBlock(DRW_Block* bk)
{
    d->closeCurrentBlock();
    auto it = d->blocks.find(bk->name);
    if (it == d->blocks.end()) {
        writeBlockRecord(bk->name);
        it = d->blocks.find(bk->name);
    }
    it->second.blockEnt = d->allocHandle();
    d->current = &it->second;
    return true;
}

// =============================================================================
//  实体写出（dwgW 公有接口）
// =============================================================================
bool dwgW::writePoint(DRW_Point* ent)
{
    d->encodeEntity(kPoint, *ent, [&](Private::ObjBuilder& b) {
        b.w.write3BD(ent->basePoint.x, ent->basePoint.y, ent->basePoint.z);
        d->writeThicknessExtrusion(b, ent->thickness, ent->extPoint);
        b.w.writeBD(0.0);               // x_axis
    });
    return true;
}

bool dwgW::writeLine(DRW_Line* ent)
{
    d->encodeEntity(kLine, *ent, [&](Private::ObjBuilder& b) {
        const bool zIsZero = ent->basePoint.z == 0.0 && ent->secPoint.z == 0.0;
        b.w.writeB(zIsZero);
        b.w.writeRD(ent->basePoint.x);
        b.w.writeDD(ent->secPoint.x, ent->basePoint.x);
        b.w.writeRD(ent->basePoint.y);
        b.w.writeDD(ent->secPoint.y, ent->basePoint.y);
        if (!zIsZero) {
            b.w.writeRD(ent->basePoint.z);
            b.w.writeDD(ent->secPoint.z, ent->basePoint.z);
        }
        d->writeThicknessExtrusion(b, ent->thickness, ent->extPoint);
    });
    return true;
}

// 无界直线（RAY 0x28 / XLINE 0x29）：类型数据 = 基点 3BD + 方向 3BD。
// 严格对应读取端 DRW_Ray::parseDwg（common data → 3BD → 3BD → 句柄流），
// 这两个实体在 R2000 里没有厚度与挤出字段，多写一个字节都会让后续数据错位。
// 方向按单位向量写出（与 DXF 的 11 组码语义一致），由调用方保证。
bool dwgW::writeRay(DRW_Ray* ent)
{
    d->encodeEntity(kRay, *ent, [&](Private::ObjBuilder& b) {
        b.w.write3BD(ent->basePoint.x, ent->basePoint.y, ent->basePoint.z);
        b.w.write3BD(ent->secPoint.x, ent->secPoint.y, ent->secPoint.z);
    });
    return true;
}

bool dwgW::writeXline(DRW_Xline* ent)
{
    d->encodeEntity(kXline, *ent, [&](Private::ObjBuilder& b) {
        b.w.write3BD(ent->basePoint.x, ent->basePoint.y, ent->basePoint.z);
        b.w.write3BD(ent->secPoint.x, ent->secPoint.y, ent->secPoint.z);
    });
    return true;
}

// 块参照（INSERT 0x07 / MINSERT 0x08）。
//
// 字段序严格对应读取端 DRW_Insert::parseDwg 的 R2000 分支：
//   插入点 3BD → 比例（2 位标志 + 若干 RD/DD）→ 旋转 BD → 挤出 3BD
//   → hasAttrib 位 →（MINSERT 时）列数 BS / 行数 BS / 列距 BD / 行距 BD
// 句柄流在公共实体句柄之后追加 **BLOCK RECORD 硬指针**（读取端用它反查块名）。
//
// hasAttrib 恒写 false：本写出器不生成 ATTRIB 对象。置位却给不出 ATTRIB/SEQEND
// 句柄会让读取端按「后面还有两个句柄」继续读，整个对象的句柄流随之错位。
bool dwgW::writeInsert(DRW_Insert* ent)
{
    const bool isMinsert = ent->colcount > 1 || ent->rowcount > 1;
    const std::uint32_t blockH = d->blockRecordHandle(ent->name);

    const bool hasAttribs = ent->hasAttribs && ent->attribCount > 0;
    const std::uint32_t insertHandle = d->allocHandle();
    d->currentInsertHandle = insertHandle;

    d->attribHandles.clear();
    d->seqendHandle = 0;
    if (hasAttribs) {
        d->attribHandles.reserve(static_cast<std::size_t>(ent->attribCount));
        for (int i = 0; i < ent->attribCount; ++i) d->attribHandles.push_back(d->allocHandle());
        d->seqendHandle = d->allocHandle();
    }

    d->encodeEntity(isMinsert ? kMinsert : kInsert, *ent, [&](Private::ObjBuilder& b) {
        b.w.write3BD(ent->basePoint.x, ent->basePoint.y, ent->basePoint.z);

        const bool defaultX = ent->xscale == 1.0;
        const bool defaultY = ent->yscale == 1.0;
        const bool defaultZ = ent->zscale == 1.0;
        const bool uniform  = ent->xscale == ent->yscale && ent->yscale == ent->zscale;
        if (defaultX && defaultY && defaultZ) {
            b.w.writeBB(3);                        // 三个比例全为默认值 1
        } else if (uniform) {
            b.w.writeBB(2);                        // x = y = z
            b.w.writeRD(ent->xscale);
        } else if (defaultX) {
            b.w.writeBB(1);                        // x 默认 1，y/z 用带默认值的编码
            b.w.writeDD(ent->yscale, 1.0);
            b.w.writeDD(ent->zscale, 1.0);
        } else {
            b.w.writeBB(0);                        // 三个比例各自编码
            b.w.writeRD(ent->xscale);
            b.w.writeDD(ent->yscale, ent->xscale);
            b.w.writeDD(ent->zscale, ent->xscale);
        }

        b.w.writeBD(ent->angle);
        // 挤出方向：R2000 的 INSERT 这里是普通 3BD，没有公共实体数据那种
        // 「默认标志位」——读取端正是按 getExtrusion(false) 读的。
        b.w.write3BD(0.0, 0.0, 1.0);
        b.w.writeB(hasAttribs);                    // has_attribs（对应 DXF 66）
        // 2004+：其后多一个 BL「属性对象数」，句柄流里逐一给出这些 ATTRIB
        //（读取端 DRW_Insert::parseDwg 的 `if (hasAttrib && version > AC1015)`）。
        // 读取端按 `for (duint8 i = 0; i < objCount; ++i)` 循环——计数错误会让
        // duint8 计数器回绕、读取端死循环。
        if (hasAttribs && d->version > DRW::AC1015)
            b.w.writeBL(static_cast<dint32>(d->attribHandles.size()));

        if (isMinsert) {
            b.w.writeBS(static_cast<dint16>(ent->colcount));
            b.w.writeBS(static_cast<dint16>(ent->rowcount));
            b.w.writeBD(ent->colspace);
            b.w.writeBD(ent->rowspace);
        }
    }, [&](Private::ObjBuilder& b) {
        b.w.writeHandle(kHcAbsolute, blockH);      // H 5 BLOCK RECORD
        if (hasAttribs) {
            // R2000-：first_attrib 软指针(4) → last_attrib 软指针(4)；
            // 2004+：逐一列出各 ATTRIB 句柄（读取端 `for (i < objCount)`）。
            // 两者之后都是 seqend 硬属主(3)。缺项/多写都会让句柄流整体错位。
            if (d->version > DRW::AC1015) {
                for (std::uint32_t h : d->attribHandles)
                    b.w.writeHandle(kHcSoftPtr, h);
            } else {
                b.w.writeHandle(kHcSoftPtr, d->attribHandles.front());
                b.w.writeHandle(kHcSoftPtr, d->attribHandles.back());
            }
            b.w.writeHandle(kHcOwner, d->seqendHandle);
        }
    }, insertHandle);
    return true;
}

// ATTRIB / ATTDEF / SEQEND
//
//   [common entity data]
//   dataflags RC → 高程 RD → 插入点 2RD → 对齐点 2DD → 挤出 BE → 厚度 BT
//   → 倾角 RD → 旋转 RD → 字高 RD → 宽度因子 RD → 文字 T
//   → 生成标志 BS → 水平对齐 BS → 垂直对齐 BS
//   → 【属性专属】标签 T → 字段长度 BS → 标志 RC →（ATTDEF 再跟一个）提示 T
//   [common entity handle data] → 文字样式句柄 H5
//
// 两处最容易写错：标志是 **RC（1 字节）不是 BS**，且它前面还有一个字段长度；
// 对齐点写在挤出/厚度**之前**（与 TEXT 的字段序不同）。
bool dwgW::writeAttrib(DRW_Attrib* ent)
{
    d->encodeEntity(kAttrib, *ent, [&](Private::ObjBuilder& b) {
        d->writeTextBody(b, *ent);
        b.w.writeT(ent->tag, d->codec);              // 2 组码：标签
        b.w.writeBS(0);                              // 73：字段长度，不建模
        b.w.writeRC(static_cast<std::uint8_t>(ent->flags & 0xFF));
    }, [&](Private::ObjBuilder& b) {
        b.w.writeHandle(kHcAbsolute, d->styleHandle(ent->style));
    }, d->takeAttribHandle(), d->currentInsertHandle, false);
    return true;
}

bool dwgW::writeAttdef(DRW_Attdef* ent)
{
    d->encodeEntity(kAttdef, *ent, [&](Private::ObjBuilder& b) {
        d->writeTextBody(b, *ent);
        b.w.writeT(ent->tag, d->codec);
        b.w.writeBS(0);                              // 73：字段长度，不建模
        b.w.writeRC(static_cast<std::uint8_t>(ent->flags & 0xFF));
        b.w.writeT(ent->prompt, d->codec);           // 3 组码：提示
    }, [&](Private::ObjBuilder& b) {
        b.w.writeHandle(kHcAbsolute, d->styleHandle(ent->style));
    });
    return true;
}

// SEQEND：没有类型数据，只有公共实体数据 + 公共句柄流。
// 它没有自己的句柄可分配——句柄早在写 INSERT 时就已经占下了。
bool dwgW::writeSeqEnd(DRW_Entity* ent)
{
    const std::uint32_t handle = d->takeSeqendHandle();
    d->encodeEntity(kSeqEnd, *ent, [](Private::ObjBuilder&) {}, Private::NoOp{}, handle,
                    d->currentInsertHandle, false);
    return true;
}

bool dwgW::writeCircle(DRW_Circle* ent)
{
    d->encodeEntity(kCircle, *ent, [&](Private::ObjBuilder& b) {
        b.w.write3BD(ent->basePoint.x, ent->basePoint.y, ent->basePoint.z);
        b.w.writeBD(ent->radious);
        d->writeThicknessExtrusion(b, ent->thickness, ent->extPoint);
    });
    return true;
}

bool dwgW::writeArc(DRW_Arc* ent)
{
    d->encodeEntity(kArc, *ent, [&](Private::ObjBuilder& b) {
        b.w.write3BD(ent->basePoint.x, ent->basePoint.y, ent->basePoint.z);
        b.w.writeBD(ent->radious);
        d->writeThicknessExtrusion(b, ent->thickness, ent->extPoint);
        b.w.writeBD(ent->staangle);
        b.w.writeBD(ent->endangle);
    });
    return true;
}

bool dwgW::writeEllipse(DRW_Ellipse* ent)
{
    d->encodeEntity(kEllipse, *ent, [&](Private::ObjBuilder& b) {
        b.w.write3BD(ent->basePoint.x, ent->basePoint.y, ent->basePoint.z);
        b.w.write3BD(ent->secPoint.x, ent->secPoint.y, ent->secPoint.z);
        b.w.write3BD(ent->extPoint.x, ent->extPoint.y, ent->extPoint.z);
        b.w.writeBD(ent->ratio);
        b.w.writeBD(ent->staparam);
        b.w.writeBD(ent->endparam);
    });
    return true;
}

bool dwgW::writeSolid(DRW_Solid* ent)
{
    d->encodeEntity(kSolid, *ent, [&](Private::ObjBuilder& b) {
        d->writeThickness(b, ent->thickness);
        b.w.writeBD(ent->basePoint.z);
        b.w.write2RD(ent->basePoint.x, ent->basePoint.y);
        b.w.write2RD(ent->secPoint.x, ent->secPoint.y);
        b.w.write2RD(ent->thirdPoint.x, ent->thirdPoint.y);
        b.w.write2RD(ent->fourPoint.x, ent->fourPoint.y);
        d->writeExtrusion(b, ent->extPoint);
    });
    return true;
}

bool dwgW::writeLWPolyline(DRW_LWPolyline* ent)
{
    d->encodeEntity(kLWPolyline, *ent, [&](Private::ObjBuilder& b) {
        bool hasBulge = false, hasWidth = false;
        for (const DRW_Vertex2D* v : ent->vertlist) {
            if (v->bulge != 0.0) hasBulge = true;
            if (v->stawidth != 0.0 || v->endwidth != 0.0) hasWidth = true;
        }
        std::uint16_t flags = 0;
        if (ent->flags & 1) flags |= 512;       // 闭合：DWG 中为 bit 512
        if (hasBulge) flags |= 16;
        if (hasWidth) flags |= 32;
        if (ent->width != 0.0) flags |= 4;

        b.w.writeBS(static_cast<dint16>(flags));
        if (flags & 4) b.w.writeBD(ent->width);
        b.w.writeBL(static_cast<dint32>(ent->vertlist.size()));
        // 凸度/宽度按顶点索引配对：列表覆盖到
        // 最后一个非零项为止，中间无值的顶点写 0.0。
        std::uint32_t bulgeCount = 0, widthCount = 0;
        if (flags & 16) {
            for (std::size_t i = 0; i < ent->vertlist.size(); ++i)
                if (ent->vertlist[i]->bulge != 0.0) bulgeCount = static_cast<std::uint32_t>(i) + 1;
            b.w.writeBL(static_cast<dint32>(bulgeCount));
        }
        if (flags & 32) {
            for (std::size_t i = 0; i < ent->vertlist.size(); ++i)
                if (ent->vertlist[i]->stawidth != 0.0 || ent->vertlist[i]->endwidth != 0.0)
                    widthCount = static_cast<std::uint32_t>(i) + 1;
            b.w.writeBL(static_cast<dint32>(widthCount));
        }
        if (!ent->vertlist.empty()) {
            auto it = ent->vertlist.begin();
            b.w.write2RD((*it)->x, (*it)->y);
            const DRW_Vertex2D* prev = *it;
            for (++it; it != ent->vertlist.end(); ++it) {
                b.w.writeDD((*it)->x, prev->x);
                b.w.writeDD((*it)->y, prev->y);
                prev = *it;
            }
            if (flags & 16)
                for (std::uint32_t i = 0; i < bulgeCount; ++i)
                    b.w.writeBD(ent->vertlist[i]->bulge);
            if (flags & 32)
                for (std::uint32_t i = 0; i < widthCount; ++i) {
                    b.w.writeBD(ent->vertlist[i]->stawidth);
                    b.w.writeBD(ent->vertlist[i]->endwidth);
                }
        }
    });
    return true;
}

bool dwgW::writePolyline(DRW_Polyline* ent)
{
    // R2000 目标：把 2D 多段线转成 LWPOLYLINE 编码（顶点/宽度/凸度一一对应）。
    // 3D 多段线（bit 8）与多面网格（bit 64）不被本写出器支持，直接拒绝。
    if (ent->flags & 8 || ent->flags & 64) return false;
    DRW_LWPolyline lw;
    lw.flags = ent->flags & 1;          // 闭合位
    lw.width = ent->defstawidth;
    lw.layer = ent->layer;
    lw.lineType = ent->lineType;
    lw.color = ent->color;
    lw.lWeight = ent->lWeight;
    lw.ltypeScale = ent->ltypeScale;
    for (DRW_Vertex* v : ent->vertlist) {
        lw.addVertex(DRW_Vertex2D(v->basePoint.x, v->basePoint.y, v->bulge));
        DRW_Vertex2D* lv = lw.vertlist.back();
        lv->stawidth = v->stawidth;
        lv->endwidth = v->endwidth;
    }
    return writeLWPolyline(&lw);
}

bool dwgW::writeSpline(DRW_Spline* ent)
{
    d->encodeEntity(kSpline, *ent, [&](Private::ObjBuilder& b) {
        b.w.writeBL(1);                 // scenario 1（控制点表示）
        // R2013+：scenario 之后、degree 之前还有两个 BL —— splineflags 与
        // knotparam（读取端 DRW_Spline::parseDwg 的 `if (version > DRW::AC1024)`，
        //  libredwg dwg.spec 的 `SINCE (R_2013b) FIELD_BL (splineflags) /
        //  FIELD_BL (knotparam)`）。漏写会让 degree 起点前移 2 个 BL，
        // 控制点数 / 节点数全错。
        if (d->version > DRW::AC1024) {
            b.w.writeBL(0);             // splineflags（bit0 = 1 表示 scenario 2）
            b.w.writeBL(0);             // knotparam
        }
        b.w.writeBL(static_cast<dint32>(ent->degree));
        b.w.writeB(false);              // 有理
        b.w.writeB((ent->flags & 1) != 0);
        b.w.writeB(false);              // 周期
        b.w.writeBD(ent->tolknot);
        b.w.writeBD(ent->tolcontrol);
        b.w.writeBL(static_cast<dint32>(ent->knotslist.size()));
        b.w.writeBL(static_cast<dint32>(ent->controllist.size()));
        b.w.writeB(false);              // 带权重
        for (double k : ent->knotslist) b.w.writeBD(k);
        for (const DRW_Coord* c : ent->controllist)
            b.w.write3BD(c->x, c->y, c->z);
    });
    return true;
}

bool dwgW::writeHatch(DRW_Hatch* ent)
{
    d->encodeEntity(kHatch, *ent, [&](Private::ObjBuilder& b) {
        // 2004+：数据段开头多一整块**渐变填充字段**（读取端 DRW_Hatch::parseDwg
        // 的 `if (version > DRW::AC1015)`，位置在读 basePoint.z **之前**）。
        // 本写出器只产生普通填充，故全写 0 / 空串，但字段一个都不能少——
        // 漏写会让读取端把我们的 basePoint.z 当成 is_gradient，其后 num_colors /
        // 种子点数解析成天文数字（实测：自家读取端直接死循环，第三方判文件损坏）。
        if (d->version > DRW::AC1015) {
            b.w.writeBL(0);                         // is_gradient
            b.w.writeBL(0);                         // reserved
            b.w.writeBD(0.0);                       // gradient_angle
            b.w.writeBD(0.0);                       // gradient_shift
            b.w.writeBL(0);                         // single_color_grad
            b.w.writeBD(0.0);                       // gradient_tint
            b.w.writeBL(0);                         // num_colors
            b.w.writeT(std::string(), d->codec);    // gradient_name
        }
        b.w.writeBD(ent->basePoint.z);  // 高程
        b.w.write3BD(ent->extPoint.x, ent->extPoint.y, ent->extPoint.z);
        b.w.writeT(ent->name, d->codec);
        b.w.writeB(ent->solid != 0);
        b.w.writeB(ent->associative != 0);
        b.w.writeBL(static_cast<dint32>(ent->looplist.size()));
        for (DRW_HatchLoop* loop : ent->looplist) {
            // 本写出器只生成多段线边界（type 1|2 = 外部环 + 多段线路径）
            const std::uint32_t loopType = loop->type ? loop->type : 3u;
            b.w.writeBL(static_cast<dint32>(loopType));
            if (loopType & 2) {
                DRW_LWPolyline* pline = nullptr;
                for (DRW_Entity* e : loop->objlist) {
                    if ((pline = dynamic_cast<DRW_LWPolyline*>(e)) != nullptr) break;
                }
                bool bulged = false;
                if (pline)
                    for (const DRW_Vertex2D* v : pline->vertlist)
                        if (v->bulge != 0.0) { bulged = true; break; }
                b.w.writeB(bulged);
                b.w.writeB(pline && (pline->flags & 1));
                b.w.writeBL(pline ? static_cast<dint32>(pline->vertlist.size()) : 0);
                if (pline)
                    for (const DRW_Vertex2D* v : pline->vertlist) {
                        b.w.writeRD(v->x);
                        b.w.writeRD(v->y);
                        if (bulged) b.w.writeBD(v->bulge);
                    }
            }
            b.w.writeBL(0);             // 边界引用句柄数（非关联填充）
        }
        b.w.writeBS(static_cast<dint16>(ent->hstyle));
        b.w.writeBS(static_cast<dint16>(ent->hpattern));
        if (!ent->solid) {
            b.w.writeBD(ent->angle);
            b.w.writeBD(ent->scale);
            b.w.writeB(false);          // 双重图案
            b.w.writeBS(static_cast<dint16>(ent->deflines));
            for (const DRW_HatchDefLine& def : ent->deflist) {
                b.w.writeBD(def.angle);
                b.w.writeBD(def.base.x);
                b.w.writeBD(def.base.y);
                b.w.writeBD(def.offset.x);
                b.w.writeBD(def.offset.y);
                b.w.writeBS(static_cast<dint16>(def.dashes.size()));
                for (double dash : def.dashes) b.w.writeBD(dash);
            }
        }
        b.w.writeBL(0);                 // 种子点数
    });
    return true;
}

// 弧长标注（ARC_DIMENSION）——DWG 里的类对象，对象头写的是类号 kClassArcDim。
//
//   定义点 3BD → 界线 1 3BD → 界线 2 3BD → 弧心 3BD → is_partial 位
//   → 起参数 BD → 止参数 BD → has_leader 位 → 引线 1 3BD → 引线 2 3BD
// 句柄流与其它标注一致：dimstyle 硬指针 + 匿名几何块硬指针。
//
// 注意 DIMENSION 的公共数据（DRW_Dimension::parseDwg 那一段）由 writeDimension
// 的公共流程负责不了——这里整段自己写：先公共实体数据（encodeEntity 已含），
// 再写 DIMENSION 公共段，最后才是弧长专属字段。
bool dwgW::writeArcDimension(DRW_DimArc* ent)
{
    const std::uint32_t styleH = d->dimstyleHandle(ent->getStyle());
    const std::uint32_t blockH = d->blockRecordHandle(ent->getName());

    d->encodeEntity(kClassArcDim, *ent, [&](Private::ObjBuilder& b) {
        // ---- DIMENSION 公共段（与其它标注共用同一份实现）----
        d->writeDimensionCommon(b, *ent);

        // ---- 弧长专属字段 ----
        const DRW_Coord def = ent->getDefPoint();
        b.w.write3BD(def.x, def.y, def.z);
        b.w.write3BD(ent->xline1Pt.x, ent->xline1Pt.y, ent->xline1Pt.z);
        b.w.write3BD(ent->xline2Pt.x, ent->xline2Pt.y, ent->xline2Pt.z);
        b.w.write3BD(ent->centerPt.x, ent->centerPt.y, ent->centerPt.z);
        b.w.writeB(ent->subFlags != 0);     // is_partial
        b.w.writeBD(ent->arcStartParam);    // 弧度
        b.w.writeBD(ent->arcEndParam);      // 弧度
        b.w.writeB(ent->hasLeader != 0);
        b.w.write3BD(ent->leader1Pt.x, ent->leader1Pt.y, ent->leader1Pt.z);
        b.w.write3BD(ent->leader2Pt.x, ent->leader2Pt.y, ent->leader2Pt.z);
    }, [&](Private::ObjBuilder& b) {
        b.w.writeHandle(kHcAbsolute, styleH);
        b.w.writeHandle(kHcAbsolute, blockH);
    });
    return true;
}

// 光源（LIGHT）：DWG 里的类对象，对象头写的是类号 kClassLight（= 506）。
//
// 位布局严格照读取端 DRW_Light::parseDwg（后者又照 libredwg dwg2.spec 的
// DWG_ENTITY (LIGHT)）：BL class_version → T name → BL type → B status
// → CMC light_color → B plot_glyph → BD intensity → 3BD position → 3BD target
// → BL attenuation_type → B use_attenuation_limits → BD 衰减界限
// → BD 聚光角/衰退角 → B cast_shadows → BL shadow_type → BS 阴影贴图
// → RC 阴影柔化。之后由框架接公共实体句柄段（COMMON_ENTITY_HANDLE_DATA）。
//
// 这里刻意**不写**光度扩展块（LIGHTINGUNITS = 2 的图纸才有，本实现未建模），
// 写出的光源一律是非光度的 —— 与读取端一致，两边成对。
bool dwgW::writeLight(DRW_Light* ent)
{
    d->encodeEntity(kClassLight, *ent, [&](Private::ObjBuilder& b) {
        b.w.writeBL(ent->classVersion);                 // class_version
        b.w.writeT(ent->name, d->codec);                // name（2007+ 进字符串流）
        b.w.writeBL(ent->type);                         // type
        b.w.writeB(ent->status);                        // status
        d->writeColor(b, ent->lightColor);              // light_color（CMC）
        b.w.writeB(ent->plotGlyph);                     // plot_glyph
        b.w.writeBD(ent->intensity);                    // intensity
        b.w.write3BD(ent->position.x, ent->position.y, ent->position.z);
        b.w.write3BD(ent->target.x, ent->target.y, ent->target.z);
        b.w.writeBL(ent->attenuationType);              // attenuation_type
        b.w.writeB(ent->useAttenuationLimits);          // use_attenuation_limits
        b.w.writeBD(ent->attenuationStartLimit);        // attenuation_start_limit
        b.w.writeBD(ent->attenuationEndLimit);          // attenuation_end_limit
        b.w.writeBD(ent->hotspotAngle);                 // hotspot_angle
        b.w.writeBD(ent->falloffAngle);                 // falloff_angle
        b.w.writeB(ent->castShadows);                   // cast_shadows
        b.w.writeBL(ent->shadowType);                   // shadow_type
        b.w.writeBS(ent->shadowMapSize);                // shadow_map_size
        b.w.writeRC(static_cast<std::uint8_t>(ent->shadowSoftness & 0xFF));  // shadow_map_softness
    });
    return true;
}


/// AC1021（R2007+）表格单元格的「值」（AcDbCellValue）。
///
/// R2007 起单元格文字**不再内联**在数据流里（内联只到 R2004），而是由本结构
/// 承载 —— 读取端 `read_table_cell_data` 的 R2007+ 分支注释即
/// "unknown then the cell value (holds the text for R2007 files)"。
///
/// 布局（与读取端 read_cad_value_with_schema 逐字段对应）：
///     flags        BL    0
///     code         BL    4 = kString（有文字）/ 0 = kUnknown（空格）
///     —— code==4：BL 字节数（UTF-16 + 双字节 NUL）→ UTF-16LE 字节 + 0x0000
///        code==0：BL 数值体（kUnknown 与 kLong 共用同一体，读端 `0 | 1 =>`）
///     unit_type    BL    0（无单位）
///     format       TV    空（走字符串流）
///     formatted    TV    空（unit != 12 时存在）
///
/// 漏写这一段时，浩辰按 R2007+ 读格尾的 flag/值，会把原本内联的文字字节当
/// 结构字段解析 —— 表格网格仍能显示，但**单元格文字丢失**（双击文字即消失）。
void writeTableCellValue(dwgBitWriter& w, const std::string& utf8, DRW_TextCodec& codec)
{
    w.writeBL(0);                              // flags
    if (utf8.empty()) {
        w.writeBL(0);                          // code = kUnknown
        w.writeBL(0);                          // 数值体（code 0|1 共用）
    } else {
        w.writeBL(4);                          // code = kString
        const std::vector<std::uint16_t> units = utf8ToUtf16Units(utf8);
        w.writeBL(static_cast<dint32>(units.size() * 2 + 2));   // 含末尾 NUL
        for (std::uint16_t u : units) {
            w.writeRC(static_cast<std::uint8_t>(u & 0xFFu));    // UTF-16LE
            w.writeRC(static_cast<std::uint8_t>((u >> 8) & 0xFFu));
        }
        w.writeRC(0); w.writeRC(0);            // 末尾 UTF-16 NUL
    }
    w.writeBL(0);                              // unit_type_code = 0
    w.writeT(std::string(), codec);            // format
    w.writeT(std::string(), codec);            // formatted_value（unit != 12）
}

// 表格（ACAD_TABLE）——DWG 里的类对象，对象头写的是类号 kClassTable。
//
// 字段序 100% 依据对 example.dwg（对象 278 / handle 4F2 / 类号 528）的实测解码
//
//   [AcDbBlockReference]
//     ins_pt        3BD    (10)  插入点
//     scale_flag    BB     (41)  3 => (1,1,1)；2 => x==y==z；1 => x 默认；0 => 各自
//     scale         3DD    (41)  随 flag
//     rotation      BD     (50)  弧度
//     extrusion     3BD    (210) 通常 (0,0,1)
//     has_attribs   B      (66)
//     block_header  H5     (2)   *Tnn 几何块记录
//     tablestyle    H342   (342) 表样式
//   [AcDbTable]
//     flag_for_table_value BS (90)  实测 22
//     horiz_direction      3BD (11) 实测 (1,0,0)
//     num_cols             BL  (92)
//     num_rows             BL  (91)
//     col_widths[numCols]  BD×n (142)
//     row_heights[numRows] BD×n (141)
//     cells[numRows*numCols] 逐格（行优先）：
//         cell_type        BS      1 = 文字，0 = 空
//         edge_flags       RC      0
//         merged           B       0
//         auto_fit         B       0
//         merge_width      BL      跨列数（合并区左上角格 >1）
//         merge_height     BL      跨行数（同上）
//         rotation         BD      0
//         value_handle     H(句柄流) 空（文字内联）
//         text             TV      2000/2004 内联文字（BS 长度 + 代码页字节）
//         has_style_override B     0
//     has_style_override   B       0（表级样式替代）
//     has_border_colors    B       0
//     has_border_lweights  B       0
//     has_border_visibility B      0
//
// 单元格数组编码已按 opencadcodec 的 write_table / write_table_cell_data
// 逆向补全：文字内联在数据流（TV），每个单元格在句柄流里各占一个空句柄
// （顺序：tablestyle 之后、行优先）。表级替代写 4 个 0 bit。这样写出的
// 表格在 AutoCAD 与本软件读回时都能拿到真实行列结构与单元格文字。
bool dwgW::writeTable(DRW_Table* ent)
{
    const std::uint32_t blockH = d->blockRecordHandle(ent->name);
    const std::uint32_t styleH = d->tableStyleHandle(ent->tableStyle);

    // 合并区：左上角格写跨距，其余格不写。按 (row, col) 索引。
    // 另收集被合并覆盖的格：浩辰/AutoCAD 的写法是覆盖格置 merged=1
    //（左上角格 merged=0、只带跨距），编辑器靠它判断格的归属。
    std::map<std::pair<int, int>, const DRW_Table::Merge*> byMerge;
    std::set<std::pair<int, int>> mergedCovered;
    for (const DRW_Table::Merge& merge : ent->merges) {
        byMerge.emplace(std::make_pair(merge.row, merge.col), &merge);
        for (int r = merge.row; r < merge.row + merge.rowSpan; ++r) {
            for (int c = merge.col; c < merge.col + merge.colSpan; ++c) {
                if (r == merge.row && c == merge.col) continue;
                mergedCovered.insert(std::make_pair(r, c));
            }
        }
    }

    // 非空文字按 (row, col) 索引。
    std::map<std::pair<int, int>, const DRW_Table::Cell*> byCell;
    for (const DRW_Table::Cell& cell : ent->cells) {
        if (!cell.text.empty())
            byCell.emplace(std::make_pair(cell.row, cell.col), &cell);
    }

    const int numRows = std::max(0, ent->numRows);
    const int numCols = std::max(0, ent->numCols);

    d->encodeEntity(kClassTable, *ent, [&](Private::ObjBuilder& b) {
        // ---- AcDbBlockReference 段 ----
        b.w.write3BD(ent->basePoint.x, ent->basePoint.y, ent->basePoint.z);

        const bool defaultX = ent->xscale == 1.0;
        const bool defaultY = ent->yscale == 1.0;
        const bool defaultZ = ent->zscale == 1.0;
        const bool uniform  = ent->xscale == ent->yscale && ent->yscale == ent->zscale;
        if (defaultX && defaultY && defaultZ) {
            b.w.writeBB(3);                        // 三个比例全为默认值 1
        } else if (uniform) {
            b.w.writeBB(2);                        // x = y = z
            b.w.writeRD(ent->xscale);
        } else if (defaultX) {
            b.w.writeBB(1);                        // x 默认 1，y/z 用带默认值的编码
            b.w.writeDD(ent->yscale, 1.0);
            b.w.writeDD(ent->zscale, 1.0);
        } else {
            b.w.writeBB(0);                        // 三个比例各自编码
            b.w.writeRD(ent->xscale);
            b.w.writeDD(ent->yscale, ent->xscale);
            b.w.writeDD(ent->zscale, ent->xscale);
        }

        b.w.writeBD(ent->angle);                   // 旋转（弧度）
        b.w.write3BD(0.0, 0.0, 1.0);               // 挤出
        b.w.writeB(false);                         // has_attribs（表格从不带 ATTRIB）
        // ★ 此处**不得**再写「所拥有对象数」BL。
        //   该字段的语义是「本实体拥有的对象数（ATTRIB… + SEQEND）」，与
        //   INSERT 一样只在 has_attribs 为真时存在（见 writeInsert 的
        //   `if (hasAttribs && version > AC1015)`，以及读取端 DRW_Insert::parseDwg）。
        //   表格的 has_attribs 恒为 false，写出这个 BL 会让其后所有字段整体位移
        //   3 位：严格读取端（浩辰 / ODA）把 num_cols / num_rows 读成天文数字，
        //   随后按该数字循环建格，表现为打开图纸长时间卡死。

        // ============ 分岔：R2010 起改用 AcDbLinkedTableData(TABLECONTENT) ============
        // R2000~R2007 是「扁平行列网格」；R2010 起整个内容换成 LinkedTableData——
        // 列/行各带样式与自定义数据、每格是「若干内容 + 样式 + 几何」的容器。
        // 字段序与读取端 DRW_Table::parseDwg 的 `version >= AC1024` 分支逐字段对偶，
        // 并已与参考实现（.cache/opencadcodec：write_table 的 r2010 分支、
        // write_table_content、write_table_cell_r2010）交叉核对。
        // 若仍按扁平格式写出，读取端会把它当 TABLECONTENT 解，行列与文字全丢
        //（实测：往返后图层读成 0、内容为空）。
        if (d->version >= DRW::AC1024) {
            // ---- 内容之前的「表头」三件套 ----
            // 注意：其中的「未知句柄」**不能**在这里写。fill() 阶段调用 writeHandle
            // 会落进**数据流**（句柄段起点由 writeEntityHandles → beginHandles 才确定，
            // 那已经是 fill() 之后），而读取端是从**句柄流**读它
            //（drw_entities.cpp 的 `hbuf.getHandle()`）。位置错了会让数据流凭空多出
            // 8 位：读取端把紧随其后的 BL 当成「码 01 + RC」多读 8 位，恰好又与那
            // 8 位抵消，数值看起来对、实则整体错位 —— 一旦表格内容变化就会崩塌。
            // 故该句柄统一在下面的 extraHandles 里按次序写出。
            b.w.writeRC(0);                     // 未知字节
            b.w.writeBL(0);                     // 未知长整数
            // 紧随的未知字段：R2010 是**单个 bit**（默认必须为真，写 0 会被严格
            // 读取端判为新表非法）；R2013+ 起换成 **BL**
            //（读取端 drw_entities.cpp 的
            //  `if (version >= DRW::AC1027) buf->getBitLong(); else buf->getBit();`）。
            // 类型写错会让其后的表内容整体错位（行列数读成垃圾）。
            if (d->version >= DRW::AC1027)
                b.w.writeBL(0);                 // R2013+：BL
            else
                b.w.writeB(true);               // R2010：bit，默认真

            // ---- 无样式编码（本实现不携带单元格/内容样式模型）----
            // 无样式：cellStyle = [BL 样式类型=0][BS data_flags=0]，
            // 读取端的 readCellStyle 在 dataFlags==0 时立即返回。
            auto writeCellStyle = [&]() {
                b.w.writeBL(0);                 // 样式类型
                b.w.writeBS(0);                 // data_flags = 0 → 无样式体
            };
            // 内容格式（readContentFormat 的对偶）
            auto writeContentFormat = [&](int alignment) {
                b.w.writeBL(0);                         // 替代标志
                b.w.writeBL(0);                         // 属性标志
                b.w.writeBL(0);                         // 值数据类型
                b.w.writeBL(0);                         // 值单位类型
                b.w.writeT(std::string(), d->codec);    // 值格式串
                b.w.writeBD(0.0);                       // 旋转
                b.w.writeBD(1.0);                       // 缩放
                b.w.writeBL(alignment);                 // 对齐 1..9
                d->writeColor(b.w, 256);                // 内容颜色（CMC，ByLayer）
                b.w.writeHandle(kHcAbsolute, 0);        // 文字样式（走句柄流）
                b.w.writeBD(0.18);                      // 文字高
            };
            // 单元格（write_table_cell_r2010 的对偶）
            auto writeCell = [&](int r, int c) {
                const auto cit = byCell.find(std::make_pair(r, c));
                const std::string text =
                    (cit != byCell.end()) ? cit->second->text : std::string();
                b.w.writeBL(0);                         // state
                b.w.writeT(std::string(), d->codec);    // 提示
                b.w.writeBL(0);                         // custom data
                b.w.writeBL(0);                         // 自定义项数 = 0
                b.w.writeBL(0);                         // has_linked_data = 0
                b.w.writeBL(1);                         // 内容数 = 1
                b.w.writeBL(1);                         // ct = 1（值）
                writeTableCellValue(b.w, text, d->codec);// AcDbCellValue（空格写空值）
                b.w.writeBL(0);                         // 属性数 = 0
                b.w.writeBS(0);                         // has_format = 0
                writeCellStyle();                       // 单元格样式（无）
                b.w.writeBL(0);                         // style id
                b.w.writeBL(0);                         // has_geometry = 0
            };

            // ---- AcDbLinkedTableData ----
            // 表名/描述写空：表的身份由 *Tnn 块记录承载，匿名表的
            // AcDbLinkedTableData.name 在真实文件里就是空的（读取端也只读不取）。
            b.w.writeT(std::string(), d->codec);        // 表名
            b.w.writeT(std::string(), d->codec);        // 描述

            b.w.writeBL(numCols);
            for (int c = 0; c < numCols; ++c) {
                b.w.writeT(std::string(), d->codec);    // 列名
                b.w.writeBL(0);                         // custom data
                b.w.writeBL(0);                         // 自定义项数 = 0
                writeCellStyle();                       // 列样式（无）
                b.w.writeBL(0);                         // style id
                const double cw = (c < static_cast<int>(ent->colWidths.size()))
                                      ? ent->colWidths[static_cast<std::size_t>(c)]
                                      : 63.5;
                b.w.writeBD(cw);                        // 列宽
            }

            b.w.writeBL(numRows);
            for (int r = 0; r < numRows; ++r) {
                b.w.writeBL(numCols);                   // 本行单元格数
                for (int c = 0; c < numCols; ++c) writeCell(r, c);
                b.w.writeBL(0);                         // custom data
                b.w.writeBL(0);                         // 自定义项数 = 0
                writeCellStyle();                       // 行样式（无）
                // style id：1=标题行 / 2=表头行 / 3=数据行。本实现的行模型
                // 没有「标题/表头」语义（表格样式里也未必定义了那两种行样式），
                // 一律按数据行写出，避免读回后被套上标题/表头的格式。
                b.w.writeBL(3);                         // style id = 3（数据行）
                const double rh = (r < static_cast<int>(ent->rowHeights.size()))
                                      ? ent->rowHeights[static_cast<std::size_t>(r)]
                                      : 9.0;
                b.w.writeBD(rh);                        // 行高
            }

            b.w.writeBL(0);                             // 字段句柄数 = 0
            writeCellStyle();                           // 基样式（无）

            b.w.writeBL(static_cast<dint32>(ent->merges.size()));
            for (const DRW_Table::Merge& mv : ent->merges) {
                const int bottom = mv.row + std::max(1, mv.rowSpan) - 1;
                const int right = mv.col + std::max(1, mv.colSpan) - 1;
                b.w.writeBL(mv.row);                    // top
                b.w.writeBL(mv.col);                    // left
                b.w.writeBL(bottom);                    // bottom
                b.w.writeBL(right);                     // right
            }

            b.w.writeHandle(kHcAbsolute, styleH);       // 表样式（句柄流，内容末尾）

            // ---- 内容之后的尾段 ----
            b.w.writeBS(0);                             // 未知短整型
            b.w.write3BD(1.0, 0.0, 0.0);                // 水平方向
            b.w.writeBL(0);                             // 无断裂数据
            b.w.writeBL(0);                             // 无断裂区间
        } else {

        // ---- AcDbTable 段（R2000 ~ R2007 扁平格式） ----
        b.w.writeBS(static_cast<dint16>(22));      // flag_for_table_value（实测值）
        b.w.write3BD(1.0, 0.0, 0.0);               // horiz_direction
        b.w.writeBL(numCols);
        b.w.writeBL(numRows);
        for (int c = 0; c < numCols; ++c) {
            const double wv = (c < static_cast<int>(ent->colWidths.size()))
                                  ? ent->colWidths[static_cast<std::size_t>(c)]
                                  : 63.5;
            b.w.writeBD(wv);
        }
        for (int r = 0; r < numRows; ++r) {
            const double hv = (r < static_cast<int>(ent->rowHeights.size()))
                                  ? ent->rowHeights[static_cast<std::size_t>(r)]
                                  : 9.0;
            b.w.writeBD(hv);
        }

        // 单元格：行优先逐格。合并跨距写在左上角格上（越界收拢到表内）。
        for (int r = 0; r < numRows; ++r) {
            for (int c = 0; c < numCols; ++c) {
                int colSpan = 1, rowSpan = 1;
                const auto mit = byMerge.find(std::make_pair(r, c));
                if (mit != byMerge.end()) {
                    colSpan = std::max(1, std::min(mit->second->colSpan, numCols - c));
                    rowSpan = std::max(1, std::min(mit->second->rowSpan, numRows - r));
                }
                const auto cit = byCell.find(std::make_pair(r, c));
                const bool hasText = cit != byCell.end();

                b.w.writeBS(hasText ? 1 : 0);      // 单元格类型：1 = 文字
                b.w.writeRC(0);                    // 边框标志
                b.w.writeB(mergedCovered.count(std::make_pair(r, c)) != 0);   // merged
                b.w.writeB(false);                 // auto fit
                b.w.writeBL(colSpan);              // 跨列数
                b.w.writeBL(rowSpan);              // 跨行数
                b.w.writeBD(0.0);                  // 旋转
                // 单元格文字：只有 R2004 及更早**内联**在数据流里。R2007+ 的
                // 文字改由格尾的 cad value 承载（见下），此处不得再写 —— 多写
                // 会让浩辰把文字字节当结构字段解析，文字随即丢失。
                if (hasText && d->version <= DRW::AC1018)
                    b.w.writeT(cit->second->text, d->codec);   // TV：内联文字
                b.w.writeB(false);                 // 无单元格样式替代
                // R2007+：每格（含空格）追加 [cell.flag BL] + cad value
                if (d->version > DRW::AC1018) {
                    b.w.writeBL(0);                // cell.flag
                    writeTableCellValue(b.w, hasText ? cit->second->text : std::string(),
                                        d->codec);
                }
            }
        }

        // 表级替代：4 个 bit 全 0
        b.w.writeB(false);
        b.w.writeB(false);
        b.w.writeB(false);
        b.w.writeB(false);

        }   // end else（R2000 ~ R2007 扁平格式）
    }, [&](Private::ObjBuilder& b) {
        // 句柄顺序与读取端一致：
        //   2000~2007：block_header(H5) → tablestyle(H342) → 每格一个空值句柄（行优先）
        //   2010+    ：block_header(H5) → 未知句柄 → tablestyle（挂在内容末尾）
        //              —— 单元格的值在 R2010 是**内联**的，不再占句柄。
        b.w.writeHandle(kHcAbsolute, blockH);
        if (d->version >= DRW::AC1024) {
            b.w.writeHandle(kHcAbsolute, 0);          // 内容开头的未知句柄
            b.w.writeHandle(kHcAbsolute, styleH);     // 表样式（内容末尾）
            return;
        }
        b.w.writeHandle(kHcAbsolute, styleH);
        for (int i = 0; i < numRows * numCols; ++i)
            b.w.writeHandle(kHcAbsolute, 0);
    });
    return true;
}

bool dwgW::writeText(DRW_Text* ent)
{
    d->encodeEntity(kText, *ent, [&](Private::ObjBuilder& b) {
        d->writeTextBody(b, *ent);
    }, [&](Private::ObjBuilder& b) {
        b.w.writeHandle(kHcAbsolute, d->styleHandle(ent->style));
    });
    return true;
}

bool dwgW::writeMText(DRW_MText* ent)
{
    d->encodeEntity(kMText, *ent, [&](Private::ObjBuilder& b) {
        b.w.write3BD(ent->basePoint.x, ent->basePoint.y, ent->basePoint.z);
        b.w.write3BD(ent->extPoint.x, ent->extPoint.y, ent->extPoint.z);
        // MTEXT 的旋转只存在 X 轴方向向量（11 组码）中：由角度反取向量
        const double rad = ent->angle * kDegToRad;
        b.w.write3BD(std::cos(rad), std::sin(rad), 0.0);
        b.w.writeBD(ent->widthscale);   // 矩形宽度（0 = 不折行）
        // 2007+：矩形宽度与字高之间还有一个 BD（读取端 DRW_MText::parseDwg 的
        // `if (version > DRW::AC1018)`，上游标注 @todo、读取后丢弃）。漏写会让
        // 其后全部字段（附着点 71、栏宽 41、行距…）前移一个 BD —— 症状是多行文字
        // 的附着点在往返后丢失。
        if (d->version > DRW::AC1018)
            b.w.writeBD(0.0);           // 未公开字段（矩形高度），读取端忽略
        b.w.writeBD(ent->height);
        dint16 attachment = static_cast<dint16>(ent->textgen);
        if (attachment < 1) attachment = 1;
        b.w.writeBS(attachment);
        // 绘制方向（DXF 72）与行距样式（DXF 73）从字段写出：MTEXT 的这两个组码由
        // DRW_MText 的 alignH / alignV 承载（与读取端同义）。硬编码 1 会把竖排
        // （TopToBottom）与「固定行距」在往返中悄悄改成默认值。
        dint16 direction = static_cast<dint16>(ent->alignH);
        if (direction < 1) direction = 1;               // 1 = 从左到右
        b.w.writeBS(direction);
        b.w.writeBD(0.0);               // 范围高
        b.w.writeBD(0.0);               // 范围宽
        b.w.writeT(ent->text, d->codec);
        dint16 spacingStyle = static_cast<dint16>(ent->alignV);
        if (spacingStyle < 1) spacingStyle = 1;         // 1 = 至少
        b.w.writeBS(spacingStyle);
        b.w.writeBD(ent->interlin);
        b.w.writeB(false);
        // 2004+：其后多一个 BL「背景填充标志」（读取端 DRW_MText::parseDwg 的
        // `if (version > DRW::AC1015)`）。0 = 无背景填充，其后不跟任何字段。
        if (d->version > DRW::AC1015)
            b.w.writeBL(0);
        // 2018+：再一个 B「非注释性」标志（读取端 DRW_MText::parseDwg 的
        // `version >= AC1032` 分支；libredwg dwg.spec 的
        // `SINCE (R_2018) FIELD_B (is_not_annotative, 0)`）。为真时其后跟一整块
        // AnnotScaleObject 字段；本实现恒写 0（普通多行文字）。
        // 漏写会让 MTEXT 的公共句柄段整体前移 1 位，文字样式句柄随即读错。
        if (d->version >= DRW::AC1032)
            b.w.writeB(false);          // is_not_annotative = 0
    }, [&](Private::ObjBuilder& b) {
        b.w.writeHandle(kHcAbsolute, d->styleHandle(ent->style));
    });
    return true;
}

bool dwgW::writeDimension(DRW_Dimension* ent)
{
    // 标注的专属字段分散在各子类且基类访问器为 protected：
    // 逐子类取出后再走统一的公共编码流程。
    DRW_Coord def1, def2, circlePoint;
    DRW_Coord ordinateFeature, ordinateLeader;
    bool ordinateXType = false;
    double angle = 0.0, oblique = 0.0, leaderLength = 0.0;
    std::uint16_t otype = 0;
    if (auto* lin = dynamic_cast<DRW_DimLinear*>(ent); lin != nullptr) {
        otype = kDimLinear;
        def1 = lin->getDef1Point();
        def2 = lin->getDef2Point();
        angle = lin->getAngle();            // 度（DXF 约定），DWG 需弧度
        oblique = lin->getOblique();
    } else if (auto* ali = dynamic_cast<DRW_DimAligned*>(ent); ali != nullptr) {
        otype = kDimAligned;
        def1 = ali->getDef1Point();
        def2 = ali->getDef2Point();
        oblique = 0.0;
    } else if (auto* rad = dynamic_cast<DRW_DimRadial*>(ent); rad != nullptr) {
        otype = kDimRadial;
        circlePoint = rad->getDiameterPoint();
        leaderLength = rad->getLeaderLength();
    } else if (auto* dia = dynamic_cast<DRW_DimDiametric*>(ent); dia != nullptr) {
        otype = kDimDiametric;
        circlePoint = dia->getDiameter1Point();
        leaderLength = dia->getLeaderLength();
    } else if (auto* ord = dynamic_cast<DRW_DimOrdinate*>(ent); ord != nullptr) {
        otype = kDimOrdinate;
        ordinateFeature = ord->getFirstLine();
        ordinateLeader = ord->getSecondLine();
        ordinateXType = (ord->type & 64) != 0;
    } else {
        return false;                       // 其余标注类型暂不支持 DWG 原生写出
    }
    const DRW_Coord defPoint = ent->getDefPoint();

    // 标注引用的是 DIMSTYLE 表记录（不是文字样式表），styleMap 查出来会指错对象。
    const std::uint32_t styleH = d->dimstyleHandle(ent->getStyle());
    const std::uint32_t blockH = d->blockRecordHandle(ent->getName());

    d->encodeEntity(otype, *ent, [&](Private::ObjBuilder& b) {
        // R2000 标注专属数据，字段序与真实 AutoCAD 文件逐字节对齐
        d->writeDimensionCommon(b, *ent);
        switch (otype) {
        case kDimLinear:
            b.w.write3BD(def1.x, def1.y, def1.z);
            b.w.write3BD(def2.x, def2.y, def2.z);
            b.w.write3BD(defPoint.x, defPoint.y, defPoint.z);
            b.w.writeBD(oblique);
            b.w.writeBD(angle * kDegToRad);     // DWG 存弧度
            break;
        case kDimAligned:
            b.w.write3BD(def1.x, def1.y, def1.z);
            b.w.write3BD(def2.x, def2.y, def2.z);
            b.w.write3BD(defPoint.x, defPoint.y, defPoint.z);
            b.w.writeBD(oblique);
            break;
        case kDimRadial:
            b.w.write3BD(defPoint.x, defPoint.y, defPoint.z);
            b.w.write3BD(circlePoint.x, circlePoint.y, circlePoint.z);
            b.w.writeBD(leaderLength);
            break;
        case kDimDiametric:
            b.w.write3BD(circlePoint.x, circlePoint.y, circlePoint.z);
            b.w.write3BD(defPoint.x, defPoint.y, defPoint.z);
            b.w.writeBD(leaderLength);
            break;
        case kDimOrdinate:
            b.w.write3BD(defPoint.x, defPoint.y, defPoint.z);
            b.w.write3BD(ordinateFeature.x, ordinateFeature.y, ordinateFeature.z);
            b.w.write3BD(ordinateLeader.x, ordinateLeader.y, ordinateLeader.z);
            b.w.writeRC(ordinateXType ? 1 : 0);
            break;
        default: break;
        }
    }, [&](Private::ObjBuilder& b) {
        b.w.writeHandle(kHcAbsolute, styleH);
        b.w.writeHandle(kHcAbsolute, blockH);
    });
    return true;
}

// =============================================================================
//  header 数据节（DRW_Header::parseDwg 读取顺序的对偶，AC1015 分支）
//
//  单一位流：变量数据与句柄按读取端 encounter 顺序交错出现。所有取值优先
//  来自适配器填写的 DRW_Header 变量（$XXX），未提供时用 AutoCAD 默认值。
// =============================================================================
namespace {

// DRW_Header 的取值访问器为 private，这里直接走公开的 vars 映射（取出即删，
// 与 getStr/getDouble 的消费语义一致）。
/// 取出并移除 header 变量（消费语义与 DRW_Header::getXxx 一致）。
/// 键名兼容带/不带 '$' 前缀：libdxfrw 解析 DWG 时存储的键不带 '$'（如 "EXTMIN"），
/// 而适配器按 DXF 惯例填的是带 '$' 的键（"$EXTMIN"）。两种都必须能取到。
DRW_Variant* takeHdrVar(DRW_Header& h, const char* key)
{
    auto it = h.vars.find(key);
    if (it == h.vars.end() && key != nullptr && key[0] == '$')
        it = h.vars.find(std::string(key + 1));
    if (it == h.vars.end()) return nullptr;
    DRW_Variant* v = it->second;
    h.vars.erase(it);
    return v;
}

std::string hdrStr(DRW_Header& h, const char* key, const char* def)
{
    DRW_Variant* v = takeHdrVar(h, key);
    if (v == nullptr) return std::string(def);
    std::string s = (v->type() == DRW_Variant::STRING && v->content.s)
                        ? *v->content.s : std::string(def);
    delete v;
    return s;
}

int hdrInt(DRW_Header& h, const char* key, int def)
{
    DRW_Variant* v = takeHdrVar(h, key);
    if (v == nullptr) return def;
    int r = (v->type() == DRW_Variant::INTEGER) ? static_cast<int>(v->content.i) : def;
    delete v;
    return r;
}

double hdrDouble(DRW_Header& h, const char* key, double def)
{
    DRW_Variant* v = takeHdrVar(h, key);
    if (v == nullptr) return def;
    double r = (v->type() == DRW_Variant::DOUBLE) ? v->content.d : def;
    delete v;
    return r;
}

DRW_Coord hdrCoord(DRW_Header& h, const char* key, double dx, double dy, double dz)
{
    DRW_Variant* v = takeHdrVar(h, key);
    DRW_Coord c(dx, dy, dz);
    if (v != nullptr) {
        if (v->type() == DRW_Variant::COORD && v->content.v) c = *v->content.v;
        delete v;
    }
    return c;
}

constexpr double kDegToRad2 = 0.017453292519943295;

void writeHeaderVars(dwgBitWriter& w, DRW_Header& hdr, dwgW::Private& d,
                     DRW::Version ver)
{
    // ---- 2007+（AC1021）的流分离 ------------------------------------------
    // 读取端（drw_header.cpp:1789-1795）在 2007+ 用**两个独立游标**读本段：
    //   · 变量流 buf —— 自节内第 20 字节起（[16 字节哨兵][RL size] 之后）；
    //   · 句柄流 hBbuf —— 定位到节内 `160 + bitSize` 位，bitSize 是本段第一个 RL。
    // 故 2007+ 必须写成
    //     [RL bitSize][变量位流][字符串区][u16 字符串位数][1 位标志][句柄流]
    // 其中变量流里只留 HANDSEED 一个句柄（读取端用 buf->getHandle() 读它，
    // 其余句柄全在句柄流）。字符串区由 writeT 自动分流（见 dwgBitWriter::writeT）。
    dwgBitWriter hdlStream;                     // 句柄流（仅 2007+ 使用）
    std::size_t bitSizeSlot = 0;
    // R2010（AC1024，且 maint_version > 3）在 size 与 bitsize 之间还有一个
    // **bitsize_hi**(RL)。读取端 drw_header.cpp:1776-1782 会按
    // `version > AC1021 && mv > 3` 读它并把句柄流基点从 160 挪到 192；
    // 漏写这 4 字节会让 hBbuf 整体前移 32 位，读出的表控制句柄全是垃圾
    // （症状：LineType/Layer/Style… 全部 "control not found"）。
    // 取值与 libredwg 一致：占位置 0（encode.c 只回填 bitsize，bitsize_hi 恒 0）。
    const bool hasHSize = (ver > DRW::AC1021);
    if (ver > DRW::AC1018) {
        if (hasHSize) w.writeRL(0);              // bitsize_hi 占位
        bitSizeSlot = w.bitPosition();           // 此后即 RL bitsize
        w.writeRL(0);                            // 占位，末尾回填
        w.setHandleSink(&hdlStream);
    }
    // R2013+：变量流在 RL bitSize 之后还有一个 BLL REQUIREDVERSIONS
    // （读取端 drw_header.cpp:1797-1800 的 `if (version > DRW::AC1024)`；
    //  libredwg header_variables.spec 的 `SINCE (R_2013b) FIELD_BLL`）。
    // 漏写会让其后所有变量整体前移 3 位，变量名与值全错位（症状：$ACADVER /
    // 图层等读成垃圾，表控制对象随之找不到）。本实现不携带该信息，写 0。
    if (ver > DRW::AC1024)
        w.writeBLL(0);                           // REQUIREDVERSIONS
    // 颜色字段：R2000 是 BS（ACI），R2004 起是 CMC —— BS 索引 + BL RGB + RC 标志
    // （读取端 drw_header.cpp 一律走 buf->getCmColor(version)，2004+ 不再是纯 BS；
    //   漏写 BL/RC 会让其后整个句柄流错位，表控制对象全部找不到）。
    // RGB 高字节是类型码：0xC0=ByLayer、0xC1=ByBlock、0xC3=ACI（低字节为索引），
    // 见 dwgBuffer::getCmColor 的注释。标志字节 0 = 不带颜色名/色册名。
    // ---- 未命名前导字段 -----------------------------------------------------
    for (int i = 0; i < 4; ++i) w.writeBD(0.0);         // unknown doubles
    for (int i = 0; i < 4; ++i) w.writeT(std::string(), d.codec);
    w.writeBL(0); w.writeBL(0);                         // unknown longs
    // 当前视口句柄：仅 R2000 及更早（< AC1018）写出。R2004 起读取端
    // （drw_header.cpp parseDwg 的 `if (version < DRW::AC1018)`）不再读它，
    // 多写一个句柄会让其后所有字段整体错位。
    if (ver < DRW::AC1018)
        w.writeHandle(kHcAbsolute, 0);                  // 当前视口（无）

    // ---- 位标志（20 位）------------------------------------------------------
    w.writeB(true);                                     // DIMASO
    w.writeB(true);                                     // DIMSHO
    w.writeB(false);                                    // PLINEGEN
    w.writeB(hdrInt(hdr, "$ORTHOMODE", 0) != 0);        // ORTHOMODE
    w.writeB(true);                                     // REGENMODE
    w.writeB(true);                                     // FILLMODE
    w.writeB(false);                                    // QTEXTMODE
    w.writeB(true);                                     // PSLTSCALE
    w.writeB(false);                                    // LIMCHECK
    // R2004 起在 LIMCHECK 之后多一个未命名位（parseDwg 的
    // `if (version > DRW::AC1015)`），紧邻 USRTIMER 之前。
    if (ver > DRW::AC1015)
        w.writeB(false);                                // Undocumented（2004+）
    w.writeB(true);                                     // USRTIMER
    w.writeB(false);                                    // SKPOLY
    w.writeB(false);                                    // ANGDIR
    w.writeB(false);                                    // SPLFRAME
    w.writeB(false);                                    // MIRRTEXT
    w.writeB(true);                                     // WORLDVIEW
    w.writeB(true);                                     // TILEMODE
    w.writeB(false);                                    // PLIMCHECK
    w.writeB(true);                                     // VISRETAIN
    w.writeB(false);                                    // DISPSILH
    w.writeB(false);                                    // PELLIPSE

    // ---- 8 个 BS --------------------------------------------------------------
    w.writeBS(static_cast<dint16>(hdrInt(hdr, "$PROXYGRAPHICS", 1)));  // PROXIGRAPHICS
    w.writeBS(3020);                                    // TREEDEPTH
    w.writeBS(static_cast<dint16>(hdrInt(hdr, "$LUNITS", 2)));
    w.writeBS(static_cast<dint16>(hdrInt(hdr, "$LUPREC", 4)));
    w.writeBS(static_cast<dint16>(hdrInt(hdr, "$AUNITS", 0)));
    w.writeBS(static_cast<dint16>(hdrInt(hdr, "$AUPREC", 0)));
    w.writeBS(static_cast<dint16>(hdrInt(hdr, "$ATTMODE", 1)));
    w.writeBS(static_cast<dint16>(hdrInt(hdr, "$PDMODE", 0)));

    // ---- 19 个 BS --------------------------------------------------------------
    // 2004+：USERI1 之前多 3 个 BitLong（读取端 drw_header.cpp parseDwg 的
    // `if (version > DRW::AC1015)`，紧跟 PICKSTYLE 之后、USERI1 之前）。
    if (ver > DRW::AC1015) { w.writeBL(0); w.writeBL(0); w.writeBL(0); }
    w.writeBS(0); w.writeBS(0); w.writeBS(0); w.writeBS(0); w.writeBS(0); // USERI1-5
    w.writeBS(8);                                       // SPLINESEGS
    w.writeBS(6); w.writeBS(6);                         // SURFU, SURFV
    w.writeBS(6);                                       // SURFTYPE
    w.writeBS(6); w.writeBS(6);                         // SURFTAB1, SURFTAB2
    w.writeBS(6);                                       // SPLINETYPE
    w.writeBS(3);                                       // SHADEDGE
    w.writeBS(70);                                      // SHADEDIF
    w.writeBS(0);                                       // UNITMODE
    w.writeBS(64);                                      // MAXACTVP
    w.writeBS(4);                                       // ISOLINES
    w.writeBS(0);                                       // CMLJUST
    w.writeBS(50);                                      // TEXTQLTY

    // ---- 21 个 BD ---------------------------------------------------------------
    w.writeBD(hdrDouble(hdr, "$LTSCALE", 1.0));
    w.writeBD(hdrDouble(hdr, "$TEXTSIZE", 3.5));
    w.writeBD(1.0);                                     // TRACEWID
    w.writeBD(hdrDouble(hdr, "$SKETCHINC", 0.1));       // SKETCHINC
    w.writeBD(0.0);                                     // FILLETRAD
    w.writeBD(0.0);                                     // THICKNESS
    w.writeBD(0.0);                                     // ANGBASE
    w.writeBD(hdrDouble(hdr, "$PDSIZE", 0.0));
    w.writeBD(0.0);                                     // PLINEWID
    for (int i = 0; i < 5; ++i) w.writeBD(0.0);         // USERR1-5
    for (int i = 0; i < 4; ++i) w.writeBD(0.0);         // CHAMFERA-D
    w.writeBD(0.5);                                     // FACETRES
    w.writeBD(hdrDouble(hdr, "$CMLSCALE", 1.0));        // CMLSCALE
    w.writeBD(1.0);                                     // CELTSCALE

    w.writeT(hdrStr(hdr, "$MENU", ""), d.codec);        // MENU

    // ---- 时间变量（4 组 × [day/days, msec]）-------------------------------------
    {
        const std::time_t now = std::time(nullptr);
        const double jd = static_cast<double>(now) / 86400.0 + 2440587.5;
        const std::uint32_t jdDay = static_cast<std::uint32_t>(jd);
        const std::uint32_t jdMsec = static_cast<std::uint32_t>(
            (jd - static_cast<double>(jdDay)) * 86400000.0);
        w.writeBL(static_cast<dint32>(jdDay));   // TDUCREATE.days
        w.writeBL(static_cast<dint32>(jdMsec));  // TDUCREATE.ms
        w.writeBL(static_cast<dint32>(jdDay));   // TDUUPDATE.days
        w.writeBL(static_cast<dint32>(jdMsec));  // TDUUPDATE.ms
        // 2004+：此处多 3 个 BitLong（读取端 drw_header.cpp parseDwg 的
        // `if (version > DRW::AC1015)`，位于 TDUPDATE 之后）。
        if (ver > DRW::AC1015) { w.writeBL(0); w.writeBL(0); w.writeBL(0); }
        w.writeBL(0); w.writeBL(0);              // TDINDWG
        w.writeBL(0); w.writeBL(0);              // TDUSRTIMER
    }

    d.writeColor(w, hdrInt(hdr, "$CECOLOR", 256));      // CECOLOR（2004+ 为 CMC）

    // ---- HANDSEED + 顶部句柄 ----------------------------------------------------
    // HANDSEED 在规格中是 FIELD_DATAHANDLE（header_variables.spec:299），
    // 句柄码必须为 0——它存的是"下一个空闲句柄值"，并不存在对应对象。
    // 若误写成码 5（软指针），严格读取器会把它当对象引用解析失败，判文件损坏。
    w.writeMainHandle(kHcData, d.nextHandle);           // HANDSEED：2007+ 仍留在数据流
    w.writeHandle(kHcAbsolute, d.layerHandle(hdrStr(hdr, "$CLAYER", "0")));
    w.writeHandle(kHcAbsolute, d.styleHandle(hdrStr(hdr, "$TEXTSTYLE", "Standard")));
    w.writeHandle(kHcAbsolute, d.ltypeHandle(hdrStr(hdr, "$CELTYPE", "ByLayer")));
    // 2007+：CELTYPE 与 DIMSTYLE 之间多一个 CMATERIAL 句柄（读取端
    // drw_header.cpp:1971，`if (version > DRW::AC1018)`）。**漏写会让其后
    // 所有句柄（含 14 个 CONTROL）整体前移一个**，症状是控制对象全部
    // "control not found"、整张图打不开。
    if (ver > DRW::AC1018)
        w.writeHandle(kHcAbsolute, 0);                  // CMATERIAL（2007+，无材质）
    w.writeHandle(kHcAbsolute, d.dimstyleHandle(hdrStr(hdr, "$DIMSTYLE", "Standard")));
    w.writeHandle(kHcAbsolute, 0);                      // CMLSTYLE（无多线样式）

    w.writeBD(0.0);                                     // PSVPSCALE
    w.write3BD(0.0, 0.0, 0.0);                          // PINSBASE
    w.write3BD(0.0, 0.0, 0.0);                          // PEXTMIN
    w.write3BD(0.0, 0.0, 0.0);                          // PEXTMAX
    // 图纸空间界限（AutoCAD 默认 12×9）。
    const DRW_Coord plimMin = hdrCoord(hdr, "$PLIMMIN", 0.0, 0.0, 0.0);
    const DRW_Coord plimMax = hdrCoord(hdr, "$PLIMMAX", 12.0, 9.0, 0.0);
    w.write2RD(plimMin.x, plimMin.y);                   // PLIMMIN
    w.write2RD(plimMax.x, plimMax.y);                   // PLIMMAX
    w.writeBD(0.0);                                     // PELEVATION
    w.write3BD(0.0, 0.0, 0.0);                          // PUCSORG
    w.write3BD(1.0, 0.0, 0.0);                          // PUCSXDIR
    w.write3BD(0.0, 1.0, 0.0);                          // PUCSYDIR
    w.writeHandle(kHcAbsolute, 0);                      // PUCSNAME
    w.writeHandle(kHcAbsolute, 0);                      // PUCSORTHOREF
    w.writeBS(0);                                       // PUCSORTHOVIEW
    w.writeHandle(kHcAbsolute, 0);                      // PUCSBASE
    for (int i = 0; i < 6; ++i) w.write3BD(0.0, 0.0, 0.0); // PUCSORGTOP..BACK

    w.write3BD(0.0, 0.0, 0.0);                          // INSBASE
    // 图形范围：已在 write() 开头从 header 取出（默认 VPORT 推导也需要它）。
    w.write3BD(d.extMin.x, d.extMin.y, d.extMin.z);     // EXTMIN
    w.write3BD(d.extMax.x, d.extMax.y, d.extMax.z);     // EXTMAX
    const DRW_Coord limMin = hdrCoord(hdr, "$LIMMIN", 0.0, 0.0, 0.0);
    const DRW_Coord limMax = hdrCoord(hdr, "$LIMMAX", 12.0, 9.0, 0.0);
    w.write2RD(limMin.x, limMin.y);                     // LIMMIN
    w.write2RD(limMax.x, limMax.y);                     // LIMMAX
    w.writeBD(0.0);                                     // ELEVATION
    w.write3BD(0.0, 0.0, 0.0);                          // UCSORG
    w.write3BD(1.0, 0.0, 0.0);                          // UCSXDIR
    w.write3BD(0.0, 1.0, 0.0);                          // UCSYDIR
    w.writeHandle(kHcAbsolute, 0);                      // UCSNAME
    w.writeHandle(kHcAbsolute, 0);                      // UCSORTHOREF
    w.writeBS(0);                                       // UCSORTHOVIEW
    w.writeHandle(kHcAbsolute, 0);                      // UCSBASE
    for (int i = 0; i < 6; ++i) w.write3BD(0.0, 0.0, 0.0); // UCSORGTOP..BACK
    // R2000（<AC1021）在此处有两个标注后缀字符串（读取端 drw_header.cpp
    // 2028-2031 行），缺一个位就错位整个句柄流。
    w.writeT(std::string(), d.codec);                   // DIMPOST
    w.writeT(std::string(), d.codec);                   // DIMAPOST
    // R2010+：字符串流里紧跟 DIMAPOST 还有两个标注后缀串（读取端
    // drw_header.cpp:2364-2367；libredwg header_variables.spec 的
    // `SINCE (R_2010b) { FIELD_T (DIMALTMZS, 1); FIELD_T (DIMMZS, 1); }`）。
    // 漏写会让其后全部字符串（HYPERLINKBASE…PROJECTNAME）整体前移。
    if (ver > DRW::AC1021) {
        w.writeT(std::string(), d.codec);               // DIMALTMZS
        w.writeT(std::string(), d.codec);               // DIMMZS
    }

    // ---- 标注变量（第一组）------------------------------------------------------
    // 默认值与 DimStyleData（即 LyCAD 标注引擎布局）保持一致，两处必须同步：
    // DIMTAD=1 对应引擎「文字置于尺寸线上方」的实际布局，字高/箭头=3.5。
    w.writeBD(hdrDouble(hdr, "$DIMSCALE", 1.0));
    w.writeBD(hdrDouble(hdr, "$DIMASZ", 3.5));
    w.writeBD(hdrDouble(hdr, "$DIMEXO", 0.63));
    w.writeBD(hdrDouble(hdr, "$DIMDLI", 3.75));
    w.writeBD(hdrDouble(hdr, "$DIMEXE", 1.05));
    w.writeBD(0.0);                                     // DIMRND
    w.writeBD(0.0);                                     // DIMDLE
    w.writeBD(0.0);                                     // DIMTP
    w.writeBD(0.0);                                     // DIMTM
    // 2007+：读取端 drw_header.cpp:2075-2079（`version > AC1018`）在 DIMTM 与
    // DIMTOL 之间读 DIMFXL/DIMJOGANG/DIMTFILL/DIMTFILLCLR；漏写会让其后全部
    // 标注变量与句柄流整体错位。
    if (ver > DRW::AC1018) {
        w.writeBD(0.0);                                 // DIMFXL
        w.writeBD(0.0);                                 // DIMJOGANG
        w.writeBS(0);                                   // DIMTFILL
        d.writeColor(w, 0);                             // DIMTFILLCLR（CMC）
    }
    w.writeB(false);                                    // DIMTOL
    w.writeB(false);                                    // DIMLIM
    w.writeB(false);                                    // DIMTIH
    w.writeB(false);                                    // DIMTOH
    w.writeB(false);                                    // DIMSE1
    w.writeB(false);                                    // DIMSE2
    w.writeBS(1);                                       // DIMTAD
    w.writeBS(0);                                       // DIMZIN
    w.writeBS(0);                                       // DIMAZIN
    if (ver > DRW::AC1018)
        w.writeBS(0);                                   // DIMARCSYM（2007+，读取端 2093）
    w.writeBD(hdrDouble(hdr, "$DIMTXT", 3.5));
    w.writeBD(hdrDouble(hdr, "$DIMCEN", 2.5));
    w.writeBD(0.0);                                     // DIMTSZ
    w.writeBD(0.03937007874015748);                     // DIMALTF
    w.writeBD(1.0);                                     // DIMLFAC
    w.writeBD(0.0);                                     // DIMTVP
    w.writeBD(1.0);                                     // DIMTFAC
    w.writeBD(0.625);                                   // DIMGAP
    w.writeBD(0.0);                                     // DIMALTRND
    w.writeB(false);                                    // DIMALT
    w.writeBS(4);                                       // DIMALTD
    w.writeB(false);                                    // DIMTOFL
    w.writeB(false);                                    // DIMSAH
    w.writeB(false);                                    // DIMTIX
    w.writeB(false);                                    // DIMSOXD

    d.writeColor(w, 0);                                 // DIMCLRD（2004+ 为 CMC）
    d.writeColor(w, 0);                                 // DIMCLRE
    d.writeColor(w, 0);                                 // DIMCLRT
    w.writeBS(0);                                       // DIMADEC
    w.writeBS(static_cast<dint16>(hdrInt(hdr, "$DIMDEC", 4)));
    w.writeBS(4);                                       // DIMTDEC
    w.writeBS(2);                                       // DIMALTU
    w.writeBS(4);                                       // DIMALTTD
    w.writeBS(0);                                       // DIMAUNIT
    w.writeBS(0);                                       // DIMFAC
    w.writeBS(2);                                       // DIMLUNIT
    w.writeBS(46);                                      // DIMDSEP '.'
    w.writeBS(0);                                       // DIMTMOVE
    w.writeBS(0);                                       // DIMJUST
    w.writeB(false);                                    // DIMSD1
    w.writeB(false);                                    // DIMSD2
    w.writeBS(1);                                       // DIMTOLJ
    w.writeBS(0);                                       // DIMTZIN
    w.writeBS(0);                                       // DIMALTZ
    w.writeBS(0);                                       // DIMALTTZ
    w.writeB(false);                                    // DIMUPT
    w.writeBS(3);                                       // DIMATFIT
    // 2007+：DIMFXLON（读取端 2143-2145）。
    if (ver > DRW::AC1018)
        w.writeB(false);                                // DIMFXLON
    // R2010+：DIMFXLON 之后还有三个字段（读取端 drw_header.cpp:2146-2150 的
    // `version > AC1021` 分支；libredwg header_variables.spec 的
    // `SINCE (R_2010b)`：`B DIMTXTDIRECTION / BD DIMALTMZF / BD DIMMZF`）。
    // **漏写这 3 个字段会让其后整个变量流前移**，读取端从此把后续每个字段都
    // 读错位——libredwg 会在文件头变量区报一串 CMC/TU/BD 越界（实测 9~10 条），
    // 浩辰的 AUDIT 也因此判「需修复」。取值与浩辰/AutoCAD 一致：全为 0。
    if (ver > DRW::AC1021) {
        w.writeB(false);                                // DIMTXTDIRECTION
        w.writeBD(0.0);                                 // DIMALTMZF
        w.writeBD(0.0);                                 // DIMMZF
    }

    w.writeHandle(kHcAbsolute, d.styleHandle(hdrStr(hdr, "$DIMTXSTY", "Standard")));
    w.writeHandle(kHcAbsolute, 0);                      // DIMLDRBLK
    w.writeHandle(kHcAbsolute, 0);                      // DIMBLK
    w.writeHandle(kHcAbsolute, 0);                      // DIMBLK1
    w.writeHandle(kHcAbsolute, 0);                      // DIMBLK2
    // 2007+：3 个线型句柄（读取端 2163-2170，在 DIMBLK2 与 DIMLWD 之间）
    if (ver > DRW::AC1018) {
        w.writeHandle(kHcAbsolute, 0);                  // DIMLTYPE
        w.writeHandle(kHcAbsolute, 0);                  // DIMLTEX1
        w.writeHandle(kHcAbsolute, 0);                  // DIMLTEX2
    }
    w.writeBS(31);                                      // DIMLWD（默认）
    w.writeBS(31);                                      // DIMLWE（默认）

    // ---- 控制对象句柄 -------------------------------------------------------------
    // 规格（header_variables.spec:532-542）规定各 *_CONTROL_OBJECT 用句柄码 3
    // （硬属主），而非普通对象引用的码 5。
    w.writeHandle(kHcOwner, d.blockCtrl);
    w.writeHandle(kHcOwner, d.layerCtrl);
    w.writeHandle(kHcOwner, d.styleCtrl);
    w.writeHandle(kHcOwner, d.ltypeCtrl);
    w.writeHandle(kHcOwner, d.viewCtrl);
    w.writeHandle(kHcOwner, d.ucsCtrl);
    w.writeHandle(kHcOwner, d.vportCtrl);
    w.writeHandle(kHcOwner, d.appidCtrl);
    w.writeHandle(kHcOwner, d.dimstyleCtrl);
    // 视口实体头控制：仅 R2000 及更早（< AC1018）写出。读取端
    // （drw_header.cpp parseDwg 的 `if (version < DRW::AC1018)`）在 2004+
    // 不再读它，多写会让其后字段整体错位——控制对象随之全部找不到。
    if (ver < DRW::AC1018)
        w.writeHandle(kHcOwner, d.vpEntHeaderCtrl);     // 视口实体头控制

    // ---- 命名字典 --------------------------------------------------------------
    w.writeHandle(kHcAbsolute, d.dictGroup);            // ACAD_GROUP
    w.writeHandle(kHcAbsolute, d.dictMline);            // ACAD_MLINESTYLE
    w.writeHandle(kHcOwner, d.nod);                     // DICTIONARY_NAMED_OBJECT（码 3）

    w.writeBS(1);                                       // TSTACKALIGN
    w.writeBS(70);                                      // TSTACKSIZE
    w.writeT(std::string(), d.codec);                   // HYPERLINKBASE
    w.writeT(std::string(), d.codec);                   // STYLESHEET
    w.writeHandle(kHcAbsolute, d.dictLayout);           // 字典：LAYOUTS
    w.writeHandle(kHcAbsolute, d.dictPlotSettings);     // 字典：PLOTSETTINGS
    w.writeHandle(kHcAbsolute, d.dictPlotStyleName);    // 字典：PLOTSTYLES
    // 2004+：其后多 2 个字典句柄（MATERIALS、COLORS，读取端
    // drw_header.cpp parseDwg 的 `if (version > DRW::AC1015)`）。
    // 本写出器不生成这两个字典，按空句柄写出。
    if (ver > DRW::AC1015) {
        w.writeHandle(kHcAbsolute, d.dictMaterial);     // DICT MATERIALS
        w.writeHandle(kHcAbsolute, d.dictColor);        // DICT COLORS
    }
    // 2007+：再一个字典句柄（读取端 2234-2237，`version > AC1018`）
    if (ver > DRW::AC1018)
        w.writeHandle(kHcAbsolute, 0);                  // DICT VISUALSTYLE
    // 2013+：**再一个**未命名字典句柄（libredwg header_variables.spec:577-580 的
    // `SINCE (R_2013b) FIELD_HANDLE (unknown_20, 5, 0)`，疑为 DICTIONARY_LIGHTLIST）。
    // 漏写会让其后的**全部头变量整体前移一个句柄**：libredwg 把 FLAGS 的低位当句柄读，
    // 我方 JSON 里 `unknown_20` 读出 0x44、`CMLSTYLE`/`CMATERIAL` 等后续句柄全错位；
    // 浩辰的 AUDIT 正是据此判文件损坏（实测：补上后头变量逐项与本对照文件一致）。
    if (ver > DRW::AC1024)
        w.writeHandle(kHcAbsolute, 0);                  // DICTIONARY_LIGHTLIST（2013+）

    // FLAGS：
    //   低 5 位 = CELWEIGHT 的 DWG 编码（29 = ByLayer）
    //   位 0x200  = LWDISPLAY（置位表示"不显示线宽"；AutoCAD 文件通常置位）
    //   位 0x800  = EXTNAMES（支持扩展符号名，即名称可超过 31 字符）
    //   位 0x2000 = PSTYLEMODE（使用颜色相关打印样式，AutoCAD 2000 起默认）
    {
        const int celWeight = hdrInt(hdr, "$CELWEIGHT", 29);
        const std::uint32_t headerFlags = 0x2000u | 0x800u | 0x200u
            | (static_cast<std::uint32_t>(celWeight) & 0x1Fu);
        w.writeBL(static_cast<dint32>(headerFlags));
    }
    w.writeBS(static_cast<dint16>(hdrInt(hdr, "$INSUNITS", 4)));
    w.writeBS(0);                                       // CEPSNTYPE（非 3：无句柄）
    w.writeT(std::string(), d.codec);                   // FINGERPRINTGUID
    w.writeT(std::string(), d.codec);                   // VERSIONGUID
    // 2004+：其后多 6 个 RawChar8 变量（SORTENTS / INDEXCTL / HIDETEXT /
    // XCLIPFRAME / DIMASSOC / HALOGAP，读取端 parseDwg 的 `if (version > DRW::AC1015)`）。
    if (ver > DRW::AC1015) {
        for (int i = 0; i < 6; ++i) w.writeRC(0);
        // 2004+ 还多 5 个变量（读取端 parseDwg 同一分支）：两个 BitShort、
        // 两个 RC 与 PROJECTNAME 字符串。**此前漏写整块**——读取端（含浩辰）
        // 会把这 5 个变量的位置当成其后的空间块/线型句柄来读，句柄流从
        // BLOCK PAPER_SPACE 起全部错位，图纸被判损坏。取值与真实文件一致
        //（257 = AutoCAD 的"随对象"色值约定，OBSCUREDLTYPE/INTERSECTIONDISPLAY
        // 默认 0，PROJECTNAME 空串）。
        w.writeBS(static_cast<dint16>(hdrInt(hdr, "$OBSCUREDCOLOR", 257)));
        w.writeBS(static_cast<dint16>(hdrInt(hdr, "$INTERSECTIONCOLOR", 257)));
        w.writeRC(0);                                       // OBSCUREDLTYPE
        w.writeRC(0);                                       // INTERSECTIONDISPLAY
        w.writeT(hdrStr(hdr, "$PROJECTNAME", ""), d.codec);
    }

    // ---- 空间块与默认线型句柄 -----------------------------------------------------
    w.writeHandle(kHcAbsolute, d.pspace.record);
    w.writeHandle(kHcAbsolute, d.mspace.record);
    w.writeHandle(kHcAbsolute, d.ltypeByLayer);
    w.writeHandle(kHcAbsolute, d.ltypeByBlock);
    w.writeHandle(kHcAbsolute, d.ltypeContinuous);

    // ---- 2007+ 变量块（读取端 drw_header.cpp:2281-2319）----------------------
    // 整块位于 LTYPE CONTINUOUS 句柄之后、R14+ 的 4 个 BS 之前；其中
    // INTERFEREOBJVS/INTERFEREVPVS/DRAGVS 是句柄（走句柄流），INTERFERECOLOR 是
    // CMC。漏写这一整块会让其后 4 个 BS 与控制对象句柄流整体错位。
    // R2007+ 新增的头变量：**取值必须用 AutoCAD 的默认值，不能用 0**。
    //
    // 这一段的取值来自 libredwg `header_variables.spec` 的
    // `SINCE (R_2007a) { IF_ENCODE_FROM_EARLIER { ... } }` —— 那个分支正是
    // AutoCAD 在把 R2004- 的图纸升级到 R2007+ 时为这些新变量填入的默认值
    // （与本仓 sample 目录里 AutoCAccess 系 CAD 写出的 DXF 逐项吻合：
    //  $STEPSPERSEC=2、$DWFFRAME=2、$TIMEZONE=-8000、$PSOLWIDTH=5、
    //  $LIGHTGLYPHDISPLAY=1、$TILEMODELIGHTSYNCH=1、$SOLIDHIST=1、$SHOWHIST=1 …）。
    //
    // 早期实现整段写 0，其中 **LIGHTGLYPHDISPLAY=0 会让 AutoCAD / 浩辰不再绘制
    // 光源标记**（光源对象仍在图里、仍能渲染，但屏幕上没有任何可见标记）——
    // 表现为「文件能打开，却看不到光源」。其余变量的 0 值同样与真实文件不符
    // （如 TIMEZONE=0 会让「太阳/地理位置」跑到格林尼治）。
    if (ver > DRW::AC1018) {
        const double halfPi = 1.5707963267948966;
        w.writeB(false);                                // CAMERADISPLAY
        w.writeBL(0); w.writeBL(0);                     // 未知长整型 ×2
        w.writeBD(0.0);                                 // 未知 double
        w.writeBD(2.0);                                 // STEPSPERSEC
        w.writeBD(50.0);                                // STEPSIZE
        w.writeBD(2.0);                                 // 3DDWFPREC
        w.writeBD(50.0);                                // LENSLENGTH
        w.writeBD(0.0);                                 // CAMERAHEIGHT
        w.writeRC(1);                                   // SOLIDHIST
        w.writeRC(1);                                   // SHOWHIST
        w.writeBD(5.0);                                 // PSOLWIDTH
        w.writeBD(80.0);                                // PSOLHEIGHT
        w.writeBD(halfPi);                              // LOFTANG1
        w.writeBD(halfPi);                              // LOFTANG2
        w.writeBD(0.0);                                 // LOFTMAG1
        w.writeBD(0.0);                                 // LOFTMAG2
        w.writeBS(7);                                   // LOFTPARAM
        w.writeRC(1);                                   // LOFTNORMALS
        w.writeBD(1.0);                                 // LATITUDE
        w.writeBD(1.0);                                 // LONGITUDE
        w.writeBD(0.0);                                 // NORTHDIRECTION
        w.writeBL(-8000);                               // TIMEZONE
        w.writeRC(1);                                   // LIGHTGLYPHDISPLAY（1=显示光源标记）
        w.writeRC(1);                                   // TILEMODELIGHTSYNCH
        w.writeRC(2);                                   // DWFFRAME
        w.writeRC(0);                                   // DGNFRAME
        w.writeB(true);                                 // REALWORLDSCALE
        d.writeColor(w, 1);                             // INTERFERECOLOR（CMC，红）
        w.writeHandle(kHcAbsolute, 0);                  // INTERFEREOBJVS
        w.writeHandle(kHcAbsolute, 0);                  // INTERFEREVPVS
        w.writeHandle(kHcAbsolute, 0);                  // DRAGVS
        w.writeRC(0);                                   // CSHADOW
        w.writeBD(0.0);                                 // SHADOWPLANELOCATION
    }

    for (int i = 0; i < 4; ++i) w.writeBS(0);           // R14+ 未知短整型

    // ---- 2007+：字符串区 + 句柄流 ------------------------------------------
    // 布局：[变量位流][字符串区][u16 位数][1 位标志][句柄流]；
    // 读取端 2334-2352 用 `endBitPos-1` 的标志位与前 16 位的位数反推字符串起点。
    if (ver > DRW::AC1018) {
        w.setHandleSink(nullptr);                       // 句柄流已写完
        // 变量流与字符串区都按字节对齐：读取端用 (160+bitSize)-17 处的
        // **原始 16 位**读字符串区长度，该字段必须落在字节边界（getRawShort16
        // 在非对齐时按另一套大端逻辑解读，字符串起点会整体偏移）。
        w.padToByte();
        dwgBitWriter* hss = const_cast<dwgBitWriter*>(w.stringStream());
        if (hss != nullptr) hss->padToByte();
        const std::size_t strBits = (hss != nullptr) ? hss->bitPosition() : 0;
        if (strBits > 0) {
            w.appendBitsFrom(*hss);
            if (strBits >= 0x8000u) {                   // 位数按原始 16 位写（getRawShort16）
                w.writeRS(static_cast<std::uint16_t>(strBits >> 15));
                w.writeRS(static_cast<std::uint16_t>((strBits & 0x7FFFu) | 0x8000u));
            } else {
                w.writeRS(static_cast<std::uint16_t>(strBits));
            }
            w.writeB(true);                             // 有字符串区（读取端 2337）
        } else {
            w.writeB(false);
        }
        // bitSize = **句柄流起点**（相对 bitSize 字段，即节内 160 位处）：
        // 读取端 drw_header.cpp:1792-1794 直接 setPosition((160+bitSize)>>3) 定位
        // hBbuf，并把 (160+bitSize)-1 当字符串区标志位、(160+bitSize)-17 当 u16
        // 位数——写成句柄流【末尾】会让 hBbuf 定位到句柄流之后，读出一串垃圾
        //（症状：所有 CONTROL 句柄错、控制对象全部 not found）。
        // R2010 有 bitsize_hi 时基点再 +32（读取端的 `endBitPos += 32`），
        // 故此处要把 handleStartBit 里属于 bitsize_hi 的那 32 位扣掉，才是 bitsize。
        const std::size_t handleStartBit = w.bitPosition();
        w.appendBitsFrom(hdlStream);                    // 句柄流紧随其后
        const std::size_t endBit = w.bitPosition();
        w.setBitPosition(bitSizeSlot);
        w.writeRL(static_cast<std::uint32_t>(
            hasHSize ? (handleStartBit - 32u) : handleStartBit));
        w.setBitPosition(endBit);
    }
}

/// 写一条类记录（R2000 classes 节）
///
/// appName 是**所属模块名**：AutoCAD 自己的类用 "ObjectDBX Classes"，
/// 而光源来自 SCENEOE 模块（实测 example.dxf 的 CLASS 记录 3 组码即 "SCENEOE"）。
/// 写成别的名字会让严格解析端把它当代理类，光源属性读不出来。
void writeOneClass(dwgBitWriter& w, dwgW::Private& d, std::uint16_t num,
                   const char* cppName, const char* dxfName, DRW::Version ver,
                   std::uint16_t itemClassId = 499,
                   const char* appName = "ObjectDBX Classes")
{
    w.writeBS(static_cast<dint16>(num));                // 类号（500 起）
    w.writeBS(0);                                       // 版本/代理标志
    w.writeT(appName, d.codec);                         // 应用名（所属模块）
    w.writeT(cppName, d.codec);                         // C++ 类名
    w.writeT(dxfName, d.codec);                         // DXF 记录名
    w.writeB(false);                                    // is_zombie
    // item_class_id：499（0x1F3）= 非图元对象，498（0x1F2）= 图元。
    // 规格按该位区分「实体」与「普通对象」——二者公共数据布局不同（实体多
    // entmode/属主等字段）。弧长标注 / 表格本质是图元，若写成 499，读取端会
    // 按普通对象解析，公共数据随即错位（实测 libredwg 报
    // `num_reactors: 93 > AVAIL_BITS(hdl_dat)`，对象内容整段失效）。
    w.writeBS(static_cast<dint16>(itemClassId));        // item_class_id
    // 2004+ 的类记录尾部多 5 个 BitLong（读取端 drw_classes.cpp 的
    // `if (version > DRW::AC1015)`）：实例计数、DWG 版本、维护版本与两个未知值。
    if (ver > DRW::AC1015) {
        w.writeBL(0);                                   // instanceCount
        w.writeBL(25);                                  // dwgVersion（ACAD 2004）
        w.writeBL(0);                                   // maintenance version
        w.writeBL(0);                                   // unknown 1
        w.writeBL(0);                                   // unknown 2
    }
}

/// classes 节数据：登记本写出器会用到的类对象（DICTIONARYWDFLT、PLACEHOLDER、LAYOUT）。
void writeClassesData(dwgBitWriter& w, dwgW::Private& d, std::uint16_t& nextClassNum,
                      DRW::Version ver)
{
    writeOneClass(w, d, nextClassNum++, "AcDbDictionaryWithDefault", "ACDBDICTIONARYWDFLT", ver);
    writeOneClass(w, d, nextClassNum++, "AcDbPlaceHolder", "ACDBPLACEHOLDER", ver);
    writeOneClass(w, d, nextClassNum++, "AcDbLayout", "LAYOUT", ver);
    writeOneClass(w, d, nextClassNum++, "AcDbDictionaryVar", "DICTIONARYVAR", ver);
    // 弧长标注（ARC_DIMENSION）在 DWG 里是类对象，没有固定类型号：
    // 实体写出时用的就是**这里的类号**（kClassArcDim），两处必须一致。
    // 它是**图元**，item_class_id 必须写 498（0x1F2），否则读取端按普通对象
    // 解析，公共数据错位。
    writeOneClass(w, d, nextClassNum++, "AcDbArcDimension", "ARC_DIMENSION", ver, 498);
    // 表格（ACAD_TABLE）同样是类对象：实体写出时用这里的类号 kClassTable。
    // 同样是图元 → item_class_id = 498。
    writeOneClass(w, d, nextClassNum++, "AcDbTable", "ACAD_TABLE", ver, 498);
    // 光源（LIGHT）也是类对象，且 **R2007 起才有**：低版本不登记这个类，
    // 那些版本的 classes 节因此与本条之前**逐字节一致**（回归不变量）。
    // 所属模块是 SCENEOE（AutoCAD 实测如此），不是 ObjectDBX Classes。
    if (ver >= DRW::AC1021) {
        writeOneClass(w, d, nextClassNum++, "AcDbLight", "LIGHT", ver, 498, "SCENEOE");
    }
}

} // namespace

// =============================================================================
//  节数据构建（版本无关；供 AC1015 / AC1018 各版本文件容器复用）
// =============================================================================
bool dwgW::buildSections(DRW_Interface* iface, DRW::Version dwgVer,
                         SectionData& out)
{
    if (iface == nullptr)
        return false;
    d->iface = iface;
    d->version = dwgVer;

    // 文本编码：$DWGCODEPAGE 决定（默认简中 ANSI_936），文件头记录对应 id
    std::uint16_t codepageId = kCodepageId936;
    {
        // 文本编码：AC1021 起为 UTF-16，版本串决定 codec 的档位，而 codec 的档位
        // 又决定 writeT 是否把文本分流到**独立字符串流**（见 dwgBitWriter::writeT）。
        // 必须逐版本给出真名：AC1024 若落到 "AC1015"，codec 会退回代码页模式，
        // 文本内联进主数据流、字符串流为空，2007+ 的字符串区随即缺失
        // （实测：Classes 节 endbit 写成 0，读取端算出天文数字的 strDataSize，
        //  类名全丢 → BAD_READ_CLASSES）。DRW_TextCodec::setVersion 对
        // AC1021/AC1024/AC1027/AC1032 一视同仁，都归到 UTF-16 档。
        std::string verStr = (dwgVer == DRW::AC1032) ? "AC1032"
                          : (dwgVer == DRW::AC1027) ? "AC1027"
                          : (dwgVer == DRW::AC1024) ? "AC1024"
                          : (dwgVer == DRW::AC1021) ? "AC1021"
                          : (dwgVer == DRW::AC1018) ? "AC1018" : "AC1015";
        d->codec.setVersion(&verStr, false);
    }
    d->codec.setCodePage(std::string("ANSI_936"), false);

    // ---- 1. 收集内容 ------------------------------------------------------------
    DRW_Header header;
    d->initInfrastructure();
    iface->writeHeader(header);

    // 提前取出图形范围：若回调未提供 VPORT，默认视口的视图参数据此推导
    // （见 encodePendingDefaults）。此处消费后，writeHeaderVars 直接使用 d.extMin/extMax。
    d->extMin = hdrCoord(header, "$EXTMIN", 0.0, 0.0, 0.0);
    d->extMax = hdrCoord(header, "$EXTMAX", 0.0, 0.0, 0.0);
    d->hasExt = (d->extMax.x > d->extMin.x || d->extMax.y > d->extMin.y);

    const std::string dwgCodepage = hdrStr(header, "$DWGCODEPAGE", "ANSI_936");
    if (dwgCodepage != "ANSI_936") {
        d->codec.setCodePage(dwgCodepage, false);
        codepageId = kCodepageId1252;
    }
    d->codepageId = codepageId;

    // 提取标注参数（DIMSTYLE 记录需要完整字段；取自 $DIMxxx 变量或默认值）
    Private::DimStyleData& dv = d->dimData;
    dv.scale = hdrDouble(header, "$DIMSCALE", dv.scale);
    dv.asz   = hdrDouble(header, "$DIMASZ", dv.asz);
    dv.exo   = hdrDouble(header, "$DIMEXO", dv.exo);
    dv.dli   = hdrDouble(header, "$DIMDLI", dv.dli);
    dv.exe   = hdrDouble(header, "$DIMEXE", dv.exe);
    dv.rnd   = hdrDouble(header, "$DIMRND", dv.rnd);
    dv.dle   = hdrDouble(header, "$DIMDLE", dv.dle);
    dv.tp    = hdrDouble(header, "$DIMTP", dv.tp);
    dv.tm    = hdrDouble(header, "$DIMTM", dv.tm);
    dv.txt   = hdrDouble(header, "$DIMTXT", dv.txt);
    dv.cen   = hdrDouble(header, "$DIMCEN", dv.cen);
    dv.tsz   = hdrDouble(header, "$DIMTSZ", dv.tsz);
    dv.altf  = hdrDouble(header, "$DIMALTF", dv.altf);
    dv.lfac  = hdrDouble(header, "$DIMLFAC", dv.lfac);
    dv.tvp   = hdrDouble(header, "$DIMTVP", dv.tvp);
    dv.tfac  = hdrDouble(header, "$DIMTFAC", dv.tfac);
    dv.gap   = hdrDouble(header, "$DIMGAP", dv.gap);
    dv.altrnd= hdrDouble(header, "$DIMALTRND", dv.altrnd);
    dv.tol   = hdrInt(header, "$DIMTOL", dv.tol);
    dv.lim   = hdrInt(header, "$DIMLIM", dv.lim);
    dv.tih   = hdrInt(header, "$DIMTIH", dv.tih);
    dv.toh   = hdrInt(header, "$DIMTOH", dv.toh);
    dv.se1   = hdrInt(header, "$DIMSE1", dv.se1);
    dv.se2   = hdrInt(header, "$DIMSE2", dv.se2);
    dv.tad   = hdrInt(header, "$DIMTAD", dv.tad);
    dv.zin   = hdrInt(header, "$DIMZIN", dv.zin);
    dv.azin  = hdrInt(header, "$DIMAZIN", dv.azin);
    dv.alt   = hdrInt(header, "$DIMALT", dv.alt);
    dv.altd  = hdrInt(header, "$DIMALTD", dv.altd);
    dv.tofl  = hdrInt(header, "$DIMTOFL", dv.tofl);
    dv.sah   = hdrInt(header, "$DIMSAH", dv.sah);
    dv.tix   = hdrInt(header, "$DIMTIX", dv.tix);
    dv.soxd  = hdrInt(header, "$DIMSOXD", dv.soxd);
    dv.clrd  = hdrInt(header, "$DIMCLRD", dv.clrd);
    dv.clre  = hdrInt(header, "$DIMCLRE", dv.clre);
    dv.clrt  = hdrInt(header, "$DIMCLRT", dv.clrt);
    dv.adec  = hdrInt(header, "$DIMADEC", dv.adec);
    dv.dec   = hdrInt(header, "$DIMDEC", dv.dec);
    dv.tdec  = hdrInt(header, "$DIMTDEC", dv.tdec);
    dv.altu  = hdrInt(header, "$DIMALTU", dv.altu);
    dv.alttd = hdrInt(header, "$DIMALTTD", dv.alttd);
    dv.aunit = hdrInt(header, "$DIMAUNIT", dv.aunit);
    dv.frac  = hdrInt(header, "$DIMFRAC", dv.frac);
    dv.lunit = hdrInt(header, "$DIMLUNIT", dv.lunit);
    dv.dsep  = hdrInt(header, "$DIMDSEP", dv.dsep);
    dv.tmove = hdrInt(header, "$DIMTMOVE", dv.tmove);
    dv.just  = hdrInt(header, "$DIMJUST", dv.just);
    dv.sd1   = hdrInt(header, "$DIMSD1", dv.sd1);
    dv.sd2   = hdrInt(header, "$DIMSD2", dv.sd2);
    dv.tolj  = hdrInt(header, "$DIMTOLJ", dv.tolj);
    dv.tzin  = hdrInt(header, "$DIMTZIN", dv.tzin);
    dv.altz  = hdrInt(header, "$DIMALTZ", dv.altz);
    dv.alttz = hdrInt(header, "$DIMALTTZ", dv.alttz);
    dv.upt   = hdrInt(header, "$DIMUPT", dv.upt);
    dv.atfit = hdrInt(header, "$DIMATFIT", dv.atfit);
    dv.lwd   = hdrInt(header, "$DIMLWD", dv.lwd);
    dv.lwe   = hdrInt(header, "$DIMLWE", dv.lwe);

    iface->writeVports();
    iface->writeLTypes();
    iface->writeLayers();
    iface->writeTextstyles();
    iface->writeDimstyles();
    iface->writeAppId();
    iface->writeBlockRecords();
    iface->writeBlocks();
    d->closeCurrentBlock();                 // 收尾最后一个匿名块
    iface->writeEntities();                 // 实体进入模型空间
    d->encodeBlockPair(d->mspace);          // 模型空间 BLOCK/ENDBLK/记录
    d->encodeBlockPair(d->pspace);
    d->encodeBlockPair(d->pspace0);         // 第二个图纸空间布局块（Layout2）
    for (auto& [name, blk] : d->blocks)
        d->encodeBlockPair(blk);            // 尚未收尾的空匿名块
    d->encodePendingDefaults();             // 默认表记录 + 控制对象

    // ---- 回填块内实体链（prev_entity / next_entity）---------------------------------
    // 块内图元按编码顺序串成双向链表：链首 prev=0、链尾 next=0。
    // 字段以定长句柄写出，此处按位替换为等长数值（尺寸不变，故可在装配前统一回填）。
    auto patchLink = [](std::vector<std::uint8_t>& body, std::size_t bitBegin,
                        std::uint32_t value) {
        constexpr int kBits = Private::kLinkBytes * 8;
        if (bitBegin == 0) return;
        for (int i = 0; i < kBits; ++i) {
            const std::size_t p = bitBegin + static_cast<std::size_t>(i);
            if ((p >> 3) >= body.size()) return;
            const int bit = static_cast<int>((value >> (kBits - 1 - i)) & 1u);
            const std::uint8_t mask = static_cast<std::uint8_t>(1u << (7 - (p & 7)));
            body[p >> 3] = bit ? static_cast<std::uint8_t>(body[p >> 3] | mask)
                               : static_cast<std::uint8_t>(body[p >> 3] & ~mask);
        }
    };
    auto patchBlockLinks = [&](Private::Blk& blk) {
        for (std::size_t i = 0; i < blk.ents.size(); ++i) {
            Private::Obj* o = d->findObject(blk.ents[i]);
            if (o == nullptr) continue;
            const std::uint32_t prev = (i == 0) ? 0u : blk.ents[i - 1];
            const std::uint32_t next = (i + 1 == blk.ents.size()) ? 0u : blk.ents[i + 1];
            patchLink(o->body, o->prevBit, prev);
            patchLink(o->body, o->nextBit, next);
        }
    };
    patchBlockLinks(d->mspace);
    patchBlockLinks(d->pspace);
    for (auto& entry : d->blocks) patchBlockLinks(entry.second);

    // ---- 2. 编码各节数据 ----------------------------------------------------------
    {
        dwgBitWriter w;
        writeHeaderVars(w, header, *d, dwgVer);
        w.padToByte();
        out.headerData = w.takeData();
    }
    {
        dwgBitWriter w;
        std::uint16_t classNum = 500;
        writeClassesData(w, *d, classNum, dwgVer);
        out.maxClassNum = static_cast<std::uint16_t>(classNum - 1);
        if (dwgVer > DRW::AC1018) {
            // 2007+：类记录之后多一段**独立字符串区**（三个类名），读取端
            //（dwgReader21::readDwgClasses）用本段首个 RL bitSize 反推字符串起点
            //（strStartPos = bitSize + 159），再按 sBuf 逐个读字符串。故数据区为
            //   [RL bitSize][BS 最大类号][RC][RC][1 位][类记录][字符串区][u16 位数][1 位标志]
            dwgBitWriter body;
            body.writeBS(static_cast<dint16>(out.maxClassNum));
            body.writeRC(0);
            body.writeRC(0);
            body.writeB(true);
            body.appendBitsFrom(w);
            const std::size_t strBits = w.stringBits();
            if (strBits > 0) {
                body.appendBitsFrom(*w.stringStream());
                if (strBits >= 0x8000u) {
                    body.writeRS(static_cast<std::uint16_t>(strBits >> 15));
                    body.writeRS(static_cast<std::uint16_t>((strBits & 0x7FFFu) | 0x8000u));
                } else {
                    body.writeRS(static_cast<std::uint16_t>(strBits));
                }
                body.writeB(true);
            } else {
                body.writeB(false);
            }
            dwgBitWriter area;
            // R2010（maint_version > 3）：size 与 bitsize 之间还有第三个 RL
            // 「hsize」，值为 bitsize / 8（libredwg encode.c：
            // `hsize = (BITCODE_RL)(bitsize / 8)`；decode.c 的
            // `from_version >= R_2010 && maint_version > 3` 分支会读它）。
            // 漏写会让读取端把 bitsize 当 hsize、把 BS 最大类号当 bitsize，
            // 类表随即整体错位（实测 BAD_READ_CLASSES）。
            const bool hasHSize = dwgVer > DRW::AC1021;
            const std::size_t hsizeSlot = area.bitPosition();
            if (hasHSize) area.writeRL(0);        // hsize 占位
            const std::size_t slot = area.bitPosition();
            area.writeRL(0);                      // bitsize 占位
            area.appendBitsFrom(body);
            const std::size_t endBit = area.bitPosition();
            // bitsize 的语义（libredwg encode.c: encode_classes）：
            //   bitsize = 32（bitsize 字段自身）+ 该字段之后直到字符串区末位的位数
            // 也就是「从 bitsize 字段起算的位数」，**不含** 排在它前面的 hsize。
            // R2010 的 area 里 hsize 在 bitsize 之前，故要减掉那 32 位；AC1021
            // 没有 hsize，endBit 原值即对。读取端据此算字符串区起点
            // （libredwg section_string_stream / dwgReader18：strStartPos = bitsize + 191，
            //  指的是全节最后一位 endbit 的绝对位置），多算 32 位会让 endbit 读成 0、
            // 字符串区起点落到类记录里。
            const std::uint32_t bitsize =
                static_cast<std::uint32_t>(endBit - (hasHSize ? 32u : 0u));
            area.setBitPosition(slot);
            area.writeRL(bitsize);
            if (hasHSize) {
                area.setBitPosition(hsizeSlot);
                area.writeRL(bitsize / 8);
            }
            area.setBitPosition(endBit);
            area.padToByte();
            out.classesData = area.takeData();
        } else {
            w.padToByte();
            out.classesData = w.takeData();
        }
    }

    // ---- 3. 导出对象表（供各版本容器按自身偏移规则装配）-----------------------------
    out.objects.reserve(d->objects.size());
    for (const dwgW::Private::Obj& o : d->objects)
        out.objects.push_back(ObjectRecord{ o.handle, o.body, 0, o.handleBits });
    out.nextHandle = d->nextHandle;
    out.fileName = d->fileName;

    // 预览字节与控制对象句柄：文件头（缩略图地址）、AuxHeader、第二头部都要引用，
    // 随节载荷一并导出，使各版本容器无需访问 dwgW 内部状态即可独立装配。
    out.thumbnail = d->thumbnail;
    out.blockCtrl = d->blockCtrl;
    out.layerCtrl = d->layerCtrl;
    out.styleCtrl = d->styleCtrl;
    out.ltypeCtrl = d->ltypeCtrl;
    out.viewCtrl = d->viewCtrl;
    out.ucsCtrl = d->ucsCtrl;
    out.vportCtrl = d->vportCtrl;
    out.appidCtrl = d->appidCtrl;
    out.dimstyleCtrl = d->dimstyleCtrl;
    out.vpEntHeaderCtrl = d->vpEntHeaderCtrl;
    out.nod = d->nod;
    out.dictMline = d->dictMline;
    out.dictGroup = d->dictGroup;

    // AC18 元数据段所需杂项：$MEASUREMENT（Template）与当前 Julian 时间
    // （TDCREATE/TDUPDATE，AuxHeader 与 ObjFreeSpace 用；与 writeHeaderVars
    // 写出的 TDUUPDATE 同源，微小毫秒差不影响语义）。
    out.measurement = static_cast<std::uint16_t>(hdrInt(header, "$MEASUREMENT", 1));
    {
        const std::time_t now = std::time(nullptr);
        const double jd = static_cast<double>(now) / 86400.0 + 2440587.5;
        out.tdDay = static_cast<std::uint32_t>(jd);
        out.tdMsec = static_cast<std::uint32_t>(
            (jd - static_cast<double>(out.tdDay)) * 86400000.0);
    }

    out.codepageId = codepageId;
    return true;
}
