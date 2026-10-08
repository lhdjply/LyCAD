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
//!  dwgwriter15.cpp —— DWG **R2000 (AC1015)** 写出实现（dwgW 的全部成员）。
//!
//!  命名与读取端对称：数字 15 指 DWG 的**内部版本号**（= AC1015 = AutoCAD 2000），
//!  与 intern/dwgreader15.cpp 一一对应。若将来增加其它目标版本，请仿照读取端
//!  （dwgreader18 / 21 / 24 / 27 / 32）新增 dwgwriter18.cpp 等，并在
//!  libdwgrw.cpp 的 dwgW::write() 中按版本分派。
//!
//!  位级原语（位流、CRC、哨兵）在 intern/dwgwriter.h —— 该层与版本无关，
//!  由各版本写出器共用，不要在其中加入版本相关逻辑。
//!
//!  对象/节的二进制布局以读取端（dwgreader15 / dwgreader.cpp /
//!  drw_header.cpp / drw_entities.cpp / drw_objects.cpp）的解析顺序为唯一
//!  权威来源逐字段对偶实现；关键结构（对象 CRC 范围、节 CRC 范围、T 字符串
//!  长度约定、第二头部布局、classes 与对象区之间的 0x0DCA 字段）已用 libredwg
//!  测试样例 sample_2000.dwg 实测确认。改动任何写出顺序前，请先核对读取端
//!  对应的 parseDwg 函数，再对照样例文件验证。
//!
#include "../libdwgrw.h"
#include "dwgwriter15.h"

#include <cmath>
#include <cctype>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <set>
#include <ctime>

#include "../drw_entities.h"
#include "../drw_header.h"
#include "../drw_interface.h"
#include "../drw_objects.h"
#include "drw_textcodec.h"
#include "dwgwriter.h"

namespace {

// ---- R2000 固定对象类型（与 libredwg dwg.h / 读取端分支一致） ---------------
enum : std::uint16_t {
    kText          = 0x01,
    kBlock         = 0x04,
    kEndblk        = 0x05,
    kArc           = 0x11,
    kCircle        = 0x12,
    kLine          = 0x13,
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

// 节哨兵类别（见 dwgSentinelBytes）
constexpr int kSentHeaderFile = 0;      // 文件头结束哨兵（HEADER_END）
constexpr int kSentThumbnail = 1;       // THUMBNAIL_BEGIN
constexpr int kSentVariable = 3;        // header 数据节（VARIABLE_BEGIN）
constexpr int kSentClass = 5;           // classes 节（CLASS_BEGIN）
constexpr int kSent2ndHeader = 7;       // 第二头部（2NDHEADER_BEGIN）

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
    };

    /// 对象体构造：oType + bitsize 占位 + 数据 + 句柄流 + 回填 bitsize
    struct ObjBuilder {
        dwgBitWriter w;
        std::size_t rlPos = 0;
        std::size_t handleStart = 0;
        std::size_t prevBit = 0;          // prev_entity 数值位偏移（0=未写）
        std::size_t nextBit = 0;          // next_entity 数值位偏移（0=未写）

        void begin(std::uint16_t otype, std::uint32_t handle)
        {
            w.writeBS(otype);
            rlPos = w.bitPosition();
            w.writeRL(0);
            w.writeHandle(0, handle);
        }
        void beginHandles() { handleStart = w.bitPosition(); }
        std::vector<std::uint8_t> finish()
        {
            w.padToByte();
            const std::size_t endBit = w.bitPosition();
            w.setBitPosition(rlPos);
            w.writeRL(static_cast<std::uint32_t>(handleStart));
            w.setBitPosition(endBit);
            return w.takeData();
        }
    };

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

    /// 预览图字节（[RL 图像字节数][图像字节…]，不含 BEGIN/END 哨兵）；
    /// 为空时写规范空缩略图（表示"本文件不含预览"）。
    std::vector<std::uint8_t> thumbnail;

    std::uint32_t ltypeCtrl = 0, layerCtrl = 0, styleCtrl = 0, vportCtrl = 0;
    std::uint32_t dimstyleCtrl = 0, appidCtrl = 0, blockCtrl = 0, viewCtrl = 0, ucsCtrl = 0;
    std::uint32_t vpEntHeaderCtrl = 0;      // R2000 必需（读取端按 0x46 查找）
    std::uint32_t ltypeByLayer = 0, ltypeByBlock = 0, ltypeContinuous = 0;
    std::uint32_t layer0 = 0, styleStandard = 0, vportActive = 0;
    std::uint32_t dimstyleStandard = 0, appidAcad = 0;

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
    struct DimStyleData {
        double scale=1.0, asz=2.5, exo=0.625, dli=3.75, exe=1.25;
        double rnd=0.0, dle=0.0, tp=0.0, tm=0.0;
        double txt=2.5, cen=2.5, tsz=0.0, altf=0.03937007874015748;
        double lfac=1.0, tvp=0.0, tfac=1.0, gap=0.625, altrnd=0.0;
        int tol=0, lim=0, tih=0, toh=0, se1=0, se2=0;
        int tad=0, zin=0, azin=0;
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

    std::uint32_t layerHandle(const std::string& name) const
    {
        auto it = layerMap.find(name);
        return it != layerMap.end() ? it->second : layer0;
    }
    std::uint32_t ltypeHandle(const std::string& name) const
    {
        auto it = ltypeMap.find(name);
        return it != ltypeMap.end() ? it->second : ltypeByLayer;
    }
    std::uint32_t styleHandle(const std::string& name) const
    {
        auto it = styleMap.find(name);
        return it != styleMap.end() ? it->second : styleStandard;
    }
    std::uint32_t dimstyleHandle(const std::string& name) const
    {
        auto it = dimstyleMap.find(name);
        return it != dimstyleMap.end() ? it->second : dimstyleStandard;
    }
    std::uint32_t blockRecordHandle(const std::string& name) const
    {
        auto it = blocks.find(name);
        return it != blocks.end() ? it->second.record : mspace.record;
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
    void writeTableCommon(ObjBuilder& b, std::uint16_t otype, std::uint32_t handle)
    {
        b.begin(otype, handle);
        b.w.writeBS(0);                 // EED 长度（无扩展数据）
        b.w.writeBL(0);                 // numReactors
    }

    /// 表记录的公共句柄尾部。样例实测（LAYER 记录句柄流：
    /// [0] 码4 = 父控制对象、[1] 码3 = XDicObj）与规格 CONTROL_HANDLE_STREAM 一致。
    void writeTableHandleTail(ObjBuilder& b, std::uint32_t ctrlHandle)
    {
        b.w.writeHandle(kHcSoftPtr, ctrlHandle);    // ownerhandle：所属控制对象（码 4）
        b.w.writeHandle(kHcOwner, 0);               // XDicObj（码 3）
    }

    /// 命名对象（NOD 字典条目：MLINESTYLE / DICTIONARYVAR / XRECORD）的公共头。
    /// 数据段：EED + numReactors（BL）；句柄段：ownerhandle → reactors[n] → XDicObj。
    /// 这三类对象都会被所属命名字典记录一个 reactor，故 numReactors = 1。
    /// 注意 owner/reactors/xdicobj 属句柄段（libredwg 自 hdl_dat 读取 reactors），
    /// 必须写在类型数据之后、并以 beginHandles() 标记句柄段起点。
    void writeNamedHeadStart(ObjBuilder& b, std::uint16_t otype, std::uint32_t handle)
    {
        b.begin(otype, handle);
        b.w.writeBS(0);                                     // EED 长度（无扩展数据）
        b.w.writeBL(1);                                     // numReactors = 1
    }
    void writeNamedHeadTail(ObjBuilder& b, std::uint32_t ownerDict)
    {
        b.w.writeHandle(kHcSoftPtr, ownerDict);             // ownerhandle（码 4）
        b.w.writeHandle(kHcSoftPtr, ownerDict);             // reactors[0]（码 4）
        b.w.writeHandle(kHcOwner, 0);                       // XDicObj（码 3）
    }

    /// 实体公共数据（DRW_Entity::parseDwg / libredwg common_entity_data.spec 的对偶）。
    /// entmode：2 = 模型空间（属主隐式 = *Model_Space）、1 = 图纸空间（隐式 =
    /// *Paper_Space）、0 = 属主句柄在句柄流中显式给出（块内实体）。
    /// AutoCAD 从不使用 0 表示空间实体，统一写 2/1 才能被严格读取器接受。
    void writeEntityCommon(ObjBuilder& b, std::uint16_t otype, const DRW_Entity& e,
                           std::uint32_t handle, int entmode)
    {
        b.begin(otype, handle);
        b.w.writeBS(0);                 // EED 长度（无扩展数据）
        b.w.writeB(false);              // preview_exists（无预览）
        b.w.writeBB(static_cast<std::uint8_t>(entmode));
        b.w.writeBS(0);                 // numReactors（0 与读取端的 BL 编码同形）
        b.w.writeB(false);              // nolinks = 0：句柄流内含 prev/next 链
        b.w.writeBS(static_cast<dint16>(e.color));
        b.w.writeBD(e.ltypeScale);
        b.w.writeBB(0);                 // ltFlags：ByLayer
        b.w.writeBB(0);                 // plotFlags
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

    /// 实体句柄流。顺序依 libdxfrw `parseDwgEntHandle`（R2000 路径）与样例实测：
    ///   [owner 仅 entmode==0] → [numReactors × reactors] → [XDicObj] →
    ///   if(!nolinks){ prev_entity → next_entity } → [layer] → if(ltFlags==3)[linetype]
    /// 句柄码依 libredwg DWG_HDL_CODE：属主软指针(4)、XDicObj 硬属主(3)、图层硬指针(5)。
    /// prev/next 以定长占位写出，装配期按块内实体链回填（BLOCK/ENDBLK 保持 0）。
    /// dimStyleH/dimBlockH 为标注实体追加的样式/匿名块句柄（普通实体传 0）。
    static constexpr int kLinkBytes = 2;    // prev/next 句柄定长字节数（支持 ≤0xFFFF）

    void writeEntityHandles(ObjBuilder& b, std::uint32_t owner, const std::string& layerName,
                            int entmode, std::uint32_t dimStyleH = 0, std::uint32_t dimBlockH = 0)
    {
        b.beginHandles();
        if (entmode == 0) b.w.writeHandle(kHcSoftPtr, owner);
        b.w.writeHandle(kHcOwner, 0);               // XDicObj
        b.prevBit = b.w.bitPosition() + 8;          // 跳过 (code<<4|size) 头部字节
        b.w.writeHandleFixed(kHcSoftPtr, 0, kLinkBytes);
        b.nextBit = b.w.bitPosition() + 8;
        b.w.writeHandleFixed(kHcSoftPtr, 0, kLinkBytes);
        b.w.writeHandle(kHcAbsolute, layerHandle(layerName));
        if (dimStyleH) b.w.writeHandle(kHcAbsolute, dimStyleH);
        if (dimBlockH) b.w.writeHandle(kHcAbsolute, dimBlockH);
    }

    /// 实体写出主流程：公共数据 → 类型数据 → 句柄流 → 排队。
    /// fill 写类型数据；extraHandles 在公共句柄流之后追加句柄（标注实体用）。
    struct NoOp { void operator()(ObjBuilder&) const {} };
    template<typename Fill, typename ExtraHandles = NoOp>
    std::uint32_t encodeEntity(std::uint16_t otype, const DRW_Entity& e, Fill fill,
                               ExtraHandles extraHandles = NoOp{})
    {
        const std::uint32_t handle = allocHandle();
        Blk& blk = current ? *current : mspace;
        const std::uint32_t owner = blk.record;
        const int entmode = entmodeFor(owner);
        ObjBuilder b;
        writeEntityCommon(b, otype, e, handle, entmode);
        fill(b);
        writeEntityHandles(b, owner, e.layer, entmode);
        extraHandles(b);
        pushObject(handle, otype, b.finish(), b.prevBit, b.nextBit);
        if (blk.first == 0) blk.first = handle;
        blk.last = handle;
        blk.ents.push_back(handle);
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
/// 依 libredwg dwg.spec：BLOCK/LAYER/STYLE/VIEW 用 BL，其余用 BS；
/// BLOCK 尾部附 model_space/paper_space，LTYPE 尾部附 byblock/bylayer。
enum class CtrlKind { Bl, Bs, Block, LType, Dimstyle };

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
    Private::ObjBuilder b;
    d.writeTableCommon(b, otype, handle);
    if (kind == CtrlKind::Bs || kind == CtrlKind::LType || kind == CtrlKind::Dimstyle)
        b.w.writeBS(static_cast<dint16>(entries.size()));
    else
        b.w.writeBL(static_cast<dint32>(entries.size()));
    if (kind == CtrlKind::Dimstyle)
        b.w.writeRC(0);                         // num_morehandles（R2000b 起）
    b.beginHandles();
    b.w.writeHandle(kHcSoftPtr, 0);             // ownerhandle（控制对象无属主）
    b.w.writeHandle(kHcOwner, 0);               // XDicObj
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
    Private::ObjBuilder b;
    d.writeTableCommon(b, kLType, handle);
    b.w.writeT(ent.name, d.codec);
    b.w.writeB(false);
    b.w.writeBS(0);                     // xrefindex（R2000）
    b.w.writeB(false);
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
    // R2000（<AC1021）线型对象尾部有 256 字节字符串区，读取端无条件读取，
    // 缺失会导致缓冲区越界、整表解析判失败。
    for (int i = 0; i < 256; ++i) b.w.writeRC(0);
    b.beginHandles();
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
    Private::ObjBuilder b;
    d.writeTableCommon(b, kLayer, handle);
    b.w.writeT(ent.name, d.codec);
    b.w.writeB(false);
    b.w.writeBS(0);                     // xrefindex
    b.w.writeB(false);
    std::uint16_t flags = 0;
    if (!ent.plotF) flags |= 1u << 4;
    flags |= static_cast<std::uint16_t>(DRW_LW_Conv::lineWidth2dwgInt(ent.lWeight) & 0x1F) << 5;
    b.w.writeBS(static_cast<dint16>(flags));
    b.w.writeBS(static_cast<dint16>(ent.color));
    b.beginHandles();
    d.writeTableHandleTail(b, d.layerCtrl);
    b.w.writeHandle(kHcAbsolute, 0);    // xref
    b.w.writeHandle(kHcAbsolute, 0);    // plot style
    b.w.writeHandle(kHcAbsolute, d.ltypeHandle(ent.lineType));
    d.pushObject(handle, kLayer, b.finish());
}

void encodeStyle(Private& d, const DRW_Textstyle& ent, std::uint32_t handle)
{
    Private::ObjBuilder b;
    d.writeTableCommon(b, kStyle, handle);
    b.w.writeT(ent.name, d.codec);
    b.w.writeB(false);
    b.w.writeBS(0);                     // xrefindex
    b.w.writeB(false);
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
    Private::ObjBuilder b;
    d.writeTableCommon(b, kVport, handle);
    b.w.writeT(ent.name, d.codec);
    b.w.writeB(false);
    b.w.writeBS(0);                     // xrefindex
    b.w.writeB(false);
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
    b.w.writeBD(0.0);
    b.w.writeBS(0);
    b.beginHandles();
    d.writeTableHandleTail(b, d.vportCtrl);
    b.w.writeHandle(kHcAbsolute, 0);    // xref
    b.w.writeHandle(kHcAbsolute, 0);    // named UCS
    b.w.writeHandle(kHcAbsolute, 0);    // base UCS
    d.pushObject(handle, kVport, b.finish());
}

/// 标注样式表记录：R2000 起记录体内含完整标注参数（libredwg
/// dwg.spec DIMSTYLE 的 IF_FREE_OR_SINCE(R_2000b) 分支）。libdxfrw 读取端
/// 只解析名称、其余跳过，不受影响；AutoCAD 等真实 CAD 依赖这些字段。
void encodeDimstyle(Private& d, const DRW_Dimstyle& ent, std::uint32_t handle)
{
    Private::ObjBuilder b;
    const Private::DimStyleData& v = d.dimData;
    d.writeTableCommon(b, kDimstyle, handle);
    b.w.writeT(ent.name, d.codec);
    b.w.writeB(false);                  // bit6
    b.w.writeBS(0);                     // xrefindex
    b.w.writeB(false);                  // bit4
    b.w.writeT(std::string(), d.codec); // DIMPOST
    b.w.writeT(std::string(), d.codec); // DIMAPOST
    b.w.writeBD(v.scale);  b.w.writeBD(v.asz);  b.w.writeBD(v.exo);
    b.w.writeBD(v.dli);    b.w.writeBD(v.exe);  b.w.writeBD(v.rnd);
    b.w.writeBD(v.dle);    b.w.writeBD(v.tp);   b.w.writeBD(v.tm);
    b.w.writeB(v.tol != 0); b.w.writeB(v.lim != 0);
    b.w.writeB(v.tih != 0); b.w.writeB(v.toh != 0);
    b.w.writeB(v.se1 != 0); b.w.writeB(v.se2 != 0);
    b.w.writeBS(static_cast<dint16>(v.tad));
    b.w.writeBS(static_cast<dint16>(v.zin));
    b.w.writeBS(static_cast<dint16>(v.azin));
    b.w.writeBD(v.txt);    b.w.writeBD(v.cen);  b.w.writeBD(v.tsz);
    b.w.writeBD(v.altf);   b.w.writeBD(v.lfac); b.w.writeBD(v.tvp);
    b.w.writeBD(v.tfac);   b.w.writeBD(v.gap);  b.w.writeBD(v.altrnd);
    b.w.writeB(v.alt != 0);
    b.w.writeBS(static_cast<dint16>(v.altd));
    b.w.writeB(v.tofl != 0); b.w.writeB(v.sah != 0);
    b.w.writeB(v.tix != 0);  b.w.writeB(v.soxd != 0);
    b.w.writeBS(static_cast<dint16>(v.clrd));
    b.w.writeBS(static_cast<dint16>(v.clre));
    b.w.writeBS(static_cast<dint16>(v.clrt));
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
    d.pushObject(handle, kDimstyle, b.finish());
}

void encodeAppId(Private& d, const DRW_AppId& ent, std::uint32_t handle)
{
    Private::ObjBuilder b;
    d.writeTableCommon(b, kAppId, handle);
    b.w.writeT(ent.name, d.codec);
    b.w.writeB(false);
    b.w.writeBS(0);                     // xrefindex
    b.w.writeB(false);
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
    Private::ObjBuilder b;
    d.writeTableCommon(b, kDictionary, handle);
    writeDictHead(d, b, entries);
    b.beginHandles();
    b.w.writeHandle(kHcSoftPtr, owner);     // ownerhandle（NOD 的子字典属主为 NOD）
    b.w.writeHandle(kHcOwner, 0);           // xdicobjhandle
    writeDictItems(b, entries);
    d.pushObject(handle, kDictionary, b.finish());
}

/// DICTIONARYWDFLT（类 500）：多一个 defaultid 句柄
void encodeDictionaryWdf(Private& d, std::uint32_t handle, std::uint32_t owner,
                         const std::vector<DictEntry>& entries, std::uint32_t defaultId)
{
    Private::ObjBuilder b;
    d.writeTableCommon(b, kClassDictWdf, handle);
    writeDictHead(d, b, entries);
    b.beginHandles();
    b.w.writeHandle(kHcSoftPtr, owner);
    b.w.writeHandle(kHcOwner, 0);
    writeDictItems(b, entries);
    b.w.writeHandle(kHcAbsolute, defaultId);    // defaultid（码 5）
    d.pushObject(handle, kClassDictWdf, b.finish());
}

/// PLACEHOLDER（类 501）：仅公共数据，无自有字段
void encodePlaceholder(Private& d, std::uint32_t handle, std::uint32_t owner)
{
    Private::ObjBuilder b;
    d.writeTableCommon(b, kClassPlaceholder, handle);
    b.beginHandles();
    b.w.writeHandle(kHcSoftPtr, owner);
    b.w.writeHandle(kHcOwner, 0);
    d.pushObject(handle, kClassPlaceholder, b.finish());
}

/// MLINESTYLE：多线样式。字段依 libredwg dwg.spec（AcDbMlineStyle，样例实测同序）。
/// 生成标准样式 "STANDARD"：两条平行线（偏移 ±0.5）、角度 90°、ByLayer 颜色。
void encodeMlineStyle(Private& d, std::uint32_t handle, std::uint32_t ownerDict,
                      const std::string& name)
{
    Private::ObjBuilder b;
    d.writeNamedHeadStart(b, kMlineStyle, handle);
    b.w.writeT(name, d.codec);                          // name（2）
    b.w.writeT(std::string(), d.codec);                 // description（0）
    b.w.writeBS(0);                                     // flag（70）
    b.w.writeBS(256);                                   // fill_color（CMC.BS 62）= ByLayer
    const double halfPi = 1.5707963267948966;
    b.w.writeBD(halfPi);                                // start_angle（51）= 90°
    b.w.writeBD(halfPi);                                // end_angle（52）= 90°
    b.w.writeRC(2);                                     // num_lines（71）
    const double offsets[2] = { 0.5, -0.5 };
    for (int i = 0; i < 2; ++i) {
        b.w.writeBD(offsets[i]);                        // lines[i].offset（49）
        b.w.writeBS(256);                               // lines[i].color（CMC.BS 62）
        b.w.writeBS(32767);                             // lines[i].lt_index（BSd 6）
    }
    b.beginHandles();
    d.writeNamedHeadTail(b, ownerDict);
    d.pushObject(handle, kMlineStyle, b.finish());
}

/// DICTIONARYVAR：AcDbVariableDictionary 的条目（schema + 字符串值）。
void encodeDictVar(Private& d, std::uint32_t handle, std::uint32_t ownerDict,
                   const std::string& value)
{
    Private::ObjBuilder b;
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
    Private::ObjBuilder b;
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
    Private::ObjBuilder b;
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
    b.w.writeT(std::string(), d.codec);                     // plotview_name
    b.w.writeBD(1.0);                                       // paper_units
    b.w.writeBD(78.4365);                                   // drawing_units
    b.w.writeT(std::string(), d.codec);                     // stylesheet
    b.w.writeBS(0);                                         // std_scale_type
    b.w.writeBD(0.0127492);                                 // std_scale_factor
    b.w.write2BD(0.0, 0.0);                                 // paper_image_origin
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
    b.beginHandles();
    b.w.writeHandle(kHcSoftPtr, owner);                     // ownerhandle = ACAD_LAYOUT 字典
    b.w.writeHandle(kHcOwner, 0);                           // xdicobjhandle
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
    Private::ObjBuilder b;
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
    Private::ObjBuilder b;
    d.writeTableCommon(b, kBlockHeader, blk.record);
    b.w.writeT(blk.name, d.codec);
    b.w.writeB(false);
    b.w.writeBS(0);                     // xrefindex
    b.w.writeB(false);
    b.w.writeB(blk.name.size() > 1 && blk.name[0] == '*'); // 匿名块
    b.w.writeB(false);                  // 含 ATTDEF
    b.w.writeB(false);                  // Xref
    b.w.writeB(false);                  // 覆盖式 Xref
    b.w.writeB(false);                  // 已加载 Xref
    b.w.write3BD(0.0, 0.0, 0.0);        // 基点
    b.w.writeT(std::string(), d.codec); // Xref 路径
    b.w.writeRC(0);                     // 插入计数终止（读取端按 0 截止）
    b.w.writeT(std::string(), d.codec); // 块说明
    b.w.writeBL(0);                     // 预览数据
    b.beginHandles();
    d.writeTableHandleTail(b, d.blockCtrl);
    b.w.writeHandle(kHcAbsolute, 0);            // Null handle
    b.w.writeHandle(kHcOwner, blk.blockEnt);    // BLOCK 实体（码 3）
    // 实体链首尾：空间块同样必须指向真实图元（样例 *Model_Space 为 4.1.8D/4.1.92）。
    // 置 0 会让读取器认为块内无图元 —— 文件能打开但图形区空白。
    b.w.writeHandle(kHcSoftPtr, blk.first);     // first_entity（码 4）
    b.w.writeHandle(kHcSoftPtr, blk.last);      // last_entity（码 4）
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
    dictVars.assign(3, 0);              // DIMASSOC / HIDETEXT / PROJECTNAME
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
    // AcDbVariableDictionary 的三个变量（与样例一致：DIMASSOC/HIDETEXT/PROJECTNAME）
    encodeDictVar(*this, dictVars[0], dictVar, "2");
    encodeDictVar(*this, dictVars[1], dictVar, "1");
    encodeDictVar(*this, dictVars[2], dictVar, "");
    encodeDictionary(*this, dictVar, nod,
                     { { "DIMASSOC", dictVars[0] },
                       { "HIDETEXT", dictVars[1] },
                       { "PROJECTNAME", dictVars[2] } });
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
    if (d->ltypeMap.count(ent->name)) return true;
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
        // 凸度/宽度按顶点索引配对（libredwg 同款语义）：列表覆盖到
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

bool dwgW::writeText(DRW_Text* ent)
{
    d->encodeEntity(kText, *ent, [&](Private::ObjBuilder& b) {
        // DataFlags：按需省略字段。bit0 高程省略、bit2 倾角省略；
        // 对齐点/旋转/宽度因子/生成标志/对齐方式按记录内容写。
        const bool hasAlignment = ent->alignH != DRW_Text::HLeft
                               || ent->alignV != DRW_Text::VBaseLine;
        std::uint8_t df = 0x01          // 省略高程（z=0）
                        | 0x04;         // 省略倾角（0）
        if (!hasAlignment) df |= 0x02;  // 省略对齐点（取插入点）
        if (ent->textgen == 0) df |= 0x20;
        if (!hasAlignment) df |= 0x40 | 0x80;
        b.w.writeRC(df);
        if (!(df & 0x01)) b.w.writeRD(ent->basePoint.z);
        b.w.write2RD(ent->basePoint.x, ent->basePoint.y);
        if (!(df & 0x02)) {
            b.w.writeDD(ent->secPoint.x, ent->basePoint.x);
            b.w.writeDD(ent->secPoint.y, ent->basePoint.y);
        }
        d->writeThicknessExtrusion(b, ent->thickness, ent->extPoint);
        if (!(df & 0x04)) b.w.writeRD(ent->oblique);
        if (!(df & 0x08)) b.w.writeRD(ent->angle);
        b.w.writeRD(ent->height);
        if (!(df & 0x10)) b.w.writeRD(ent->widthscale);
        b.w.writeT(ent->text, d->codec);
        if (!(df & 0x20)) b.w.writeBS(static_cast<dint16>(ent->textgen));
        if (!(df & 0x40)) b.w.writeBS(static_cast<dint16>(ent->alignH));
        if (!(df & 0x80)) b.w.writeBS(static_cast<dint16>(ent->alignV));
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
        b.w.writeBD(ent->height);
        dint16 attachment = static_cast<dint16>(ent->textgen);
        if (attachment < 1) attachment = 1;
        b.w.writeBS(attachment);
        b.w.writeBS(1);                 // 绘制方向：从左到右
        b.w.writeBD(0.0);               // 范围高
        b.w.writeBD(0.0);               // 范围宽
        b.w.writeT(ent->text, d->codec);
        b.w.writeBS(1);                 // 行距样式：至少
        b.w.writeBD(ent->interlin);
        b.w.writeB(false);
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
    } else {
        return false;                       // 其余标注类型暂不支持 DWG 原生写出
    }
    const DRW_Coord defPoint = ent->getDefPoint();

    const std::uint32_t styleH = d->styleHandle(ent->getStyle());
    const std::uint32_t blockH = d->blockRecordHandle(ent->getName());

    d->encodeEntity(otype, *ent, [&](Private::ObjBuilder& b) {
        const DRW_Coord ext = ent->getExtrusion();
        if (isDefaultExtrusion(ext)) b.w.writeB(true);
        else { b.w.writeB(false); b.w.write3BD(ext.x, ext.y, ext.z); }
        for (int i = 0; i < 5; ++i) b.w.writeB(false);
        const DRW_Coord tp = ent->getTextPoint();
        b.w.writeRD(tp.x);
        b.w.writeRD(tp.y);
        b.w.writeBD(tp.z);
        // dimtype 字节：bit1 置位 → 读取端还原出 bit5（几何块存在）+ bit7；
        // 标注子类型由 oType 决定（低 3 位被读取端清除后按 oType 重组）。
        b.w.writeRC(0x02);
        b.w.writeT(ent->getText(), d->codec);
        b.w.writeBD(0.0);               // rot
        b.w.writeBD(0.0);               // hdir
        b.w.write3BD(0.0, 0.0, 0.0);    // inspoint
        b.w.writeBD(0.0);               // insRot（54）
        b.w.writeBS(0);                 // align
        b.w.writeBS(0);                 // linesty
        b.w.writeBD(1.0);               // linefactor
        b.w.writeBD(ent->getMeasurement());
        b.w.write2RD(0.0, 0.0);         // clonePoint
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

void writeHeaderVars(dwgBitWriter& w, DRW_Header& hdr, dwgW::Private& d)
{
    // ---- 未命名前导字段 -----------------------------------------------------
    for (int i = 0; i < 4; ++i) w.writeBD(0.0);         // unknown doubles
    for (int i = 0; i < 4; ++i) w.writeT(std::string(), d.codec);
    w.writeBL(0); w.writeBL(0);                         // unknown longs
    w.writeHandle(kHcAbsolute, 0);                      // 当前视口（无）

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
    // 顺序依 libredwg header_variables.spec：TDUCREATE / TDUUPDATE / TDINDWG /
    // TDUSRTIMER。前两者取当前时间（Julian 日 + 当日毫秒），后两者为累计编辑时间。
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
        w.writeBL(0); w.writeBL(0);              // TDINDWG
        w.writeBL(0); w.writeBL(0);              // TDUSRTIMER
    }

    w.writeBS(static_cast<dint16>(hdrInt(hdr, "$CECOLOR", 256)));

    // ---- HANDSEED + 顶部句柄 ----------------------------------------------------
    // HANDSEED 在规格中是 FIELD_DATAHANDLE（header_variables.spec:299），
    // 句柄码必须为 0——它存的是"下一个空闲句柄值"，并不存在对应对象。
    // 若误写成码 5（软指针），严格读取器会把它当对象引用解析失败，判文件损坏。
    w.writeHandle(kHcData, d.nextHandle);               // HANDSEED（数据句柄，码 0）
    w.writeHandle(kHcAbsolute, d.layerHandle(hdrStr(hdr, "$CLAYER", "0")));
    w.writeHandle(kHcAbsolute, d.styleHandle(hdrStr(hdr, "$TEXTSTYLE", "Standard")));
    w.writeHandle(kHcAbsolute, d.ltypeHandle(hdrStr(hdr, "$CELTYPE", "ByLayer")));
    w.writeHandle(kHcAbsolute, d.dimstyleHandle(hdrStr(hdr, "$DIMSTYLE", "Standard")));
    w.writeHandle(kHcAbsolute, 0);                      // CMLSTYLE（无多线样式）

    w.writeBD(0.0);                                     // PSVPSCALE
    w.write3BD(0.0, 0.0, 0.0);                          // PINSBASE
    w.write3BD(0.0, 0.0, 0.0);                          // PEXTMIN
    w.write3BD(0.0, 0.0, 0.0);                          // PEXTMAX
    // 图纸空间界限（AutoCAD 默认 12×9）。读取端与 libredwg 都会解析这两个值，
    // 硬编码 0 会与源文件的图纸空间设置不一致。
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

    // ---- 标注变量（第一组）------------------------------------------------------
    w.writeBD(hdrDouble(hdr, "$DIMSCALE", 1.0));
    w.writeBD(hdrDouble(hdr, "$DIMASZ", 2.5));
    w.writeBD(hdrDouble(hdr, "$DIMEXO", 0.625));
    w.writeBD(hdrDouble(hdr, "$DIMDLI", 3.75));
    w.writeBD(hdrDouble(hdr, "$DIMEXE", 1.25));
    w.writeBD(0.0);                                     // DIMRND
    w.writeBD(0.0);                                     // DIMDLE
    w.writeBD(0.0);                                     // DIMTP
    w.writeBD(0.0);                                     // DIMTM
    w.writeB(false);                                    // DIMTOL
    w.writeB(false);                                    // DIMLIM
    w.writeB(false);                                    // DIMTIH
    w.writeB(false);                                    // DIMTOH
    w.writeB(false);                                    // DIMSE1
    w.writeB(false);                                    // DIMSE2
    w.writeBS(0);                                       // DIMTAD
    w.writeBS(0);                                       // DIMZIN
    w.writeBS(0);                                       // DIMAZIN
    w.writeBD(hdrDouble(hdr, "$DIMTXT", 2.5));
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

    w.writeBS(0);                                       // DIMCLRD
    w.writeBS(0);                                       // DIMCLRE
    w.writeBS(0);                                       // DIMCLRT
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

    w.writeHandle(kHcAbsolute, d.styleHandle(hdrStr(hdr, "$DIMTXSTY", "Standard")));
    w.writeHandle(kHcAbsolute, 0);                      // DIMLDRBLK
    w.writeHandle(kHcAbsolute, 0);                      // DIMBLK
    w.writeHandle(kHcAbsolute, 0);                      // DIMBLK1
    w.writeHandle(kHcAbsolute, 0);                      // DIMBLK2
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
    w.writeHandle(kHcOwner, d.vpEntHeaderCtrl);         // 视口实体头控制

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

    // FLAGS（libredwg header_variables.spec 的 FIELD_BLx (FLAGS, 70)）：
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

    // ---- 空间块与默认线型句柄 -----------------------------------------------------
    w.writeHandle(kHcAbsolute, d.pspace.record);
    w.writeHandle(kHcAbsolute, d.mspace.record);
    w.writeHandle(kHcAbsolute, d.ltypeByLayer);
    w.writeHandle(kHcAbsolute, d.ltypeByBlock);
    w.writeHandle(kHcAbsolute, d.ltypeContinuous);

    for (int i = 0; i < 4; ++i) w.writeBS(0);           // R14+ 未知短整型
}

/// 写一条类记录（R2000 classes 节）
void writeOneClass(dwgBitWriter& w, dwgW::Private& d, std::uint16_t num,
                   const char* cppName, const char* dxfName)
{
    w.writeBS(static_cast<dint16>(num));                // 类号（500 起）
    w.writeBS(0);                                       // 版本/代理标志
    w.writeT("ObjectDBX Classes", d.codec);             // 应用名
    w.writeT(cppName, d.codec);                         // C++ 类名
    w.writeT(dxfName, d.codec);                         // DXF 记录名
    w.writeB(false);                                    // is_zombie
    w.writeBS(499);                                     // item_class_id（AutoCAD 固定值）
}

/// classes 节数据：登记本写出器会用到的类对象（DICTIONARYWDFLT、PLACEHOLDER、LAYOUT）。
void writeClassesData(dwgBitWriter& w, dwgW::Private& d, std::uint16_t& nextClassNum)
{
    writeOneClass(w, d, nextClassNum++, "AcDbDictionaryWithDefault", "ACDBDICTIONARYWDFLT");
    writeOneClass(w, d, nextClassNum++, "AcDbPlaceHolder", "ACDBPLACEHOLDER");
    writeOneClass(w, d, nextClassNum++, "AcDbLayout", "LAYOUT");
    writeOneClass(w, d, nextClassNum++, "AcDbDictionaryVar", "DICTIONARYVAR");
}

} // namespace

// =============================================================================
//  文件装配与写出
// =============================================================================
namespace dwgWriter15 {

bool supports(DRW::Version ver) noexcept
{
    return ver == DRW::AC1015;
}

bool write(dwgW& writer, DRW_Interface* iface)
{
    if (iface == nullptr)
        return false;
    writer.d->iface = iface;

    // 文本编码：$DWGCODEPAGE 决定（默认简中 ANSI_936），文件头记录对应 id
    std::uint16_t codepageId = kCodepageId936;
    {
        std::string ver = "AC1015";
        writer.d->codec.setVersion(&ver, false);
    }
    writer.d->codec.setCodePage(std::string("ANSI_936"), false);

    // ---- 1. 收集内容 ------------------------------------------------------------
    DRW_Header header;
    writer.d->initInfrastructure();
    iface->writeHeader(header);

    // 提前取出图形范围：若回调未提供 VPORT，默认视口的视图参数据此推导
    // （见 encodePendingDefaults）。此处消费后，writeHeaderVars 直接使用 d.extMin/extMax。
    writer.d->extMin = hdrCoord(header, "$EXTMIN", 0.0, 0.0, 0.0);
    writer.d->extMax = hdrCoord(header, "$EXTMAX", 0.0, 0.0, 0.0);
    writer.d->hasExt = (writer.d->extMax.x > writer.d->extMin.x || writer.d->extMax.y > writer.d->extMin.y);

    const std::string dwgCodepage = hdrStr(header, "$DWGCODEPAGE", "ANSI_936");
    if (dwgCodepage != "ANSI_936") {
        writer.d->codec.setCodePage(dwgCodepage, false);
        codepageId = kCodepageId1252;
    }

    // 提取标注参数（DIMSTYLE 记录需要完整字段；取自 $DIMxxx 变量或默认值）
    Private::DimStyleData& dv = writer.d->dimData;
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
    writer.d->closeCurrentBlock();                 // 收尾最后一个匿名块
    iface->writeEntities();                 // 实体进入模型空间
    writer.d->encodeBlockPair(writer.d->mspace);          // 模型空间 BLOCK/ENDBLK/记录
    writer.d->encodeBlockPair(writer.d->pspace);
    writer.d->encodeBlockPair(writer.d->pspace0);         // 第二个图纸空间布局块（Layout2）
    for (auto& [name, blk] : writer.d->blocks)
        writer.d->encodeBlockPair(blk);            // 尚未收尾的空匿名块
    writer.d->encodePendingDefaults();             // 默认表记录 + 控制对象

    // ---- 2. 编码各节数据 ----------------------------------------------------------
    std::vector<std::uint8_t> headerData, classesData;
    {
        dwgBitWriter w;
        writeHeaderVars(w, header, *writer.d);
        w.padToByte();
        headerData = w.takeData();
    }
    {
        dwgBitWriter w;
        std::uint16_t classNum = 500;
        writeClassesData(w, *writer.d, classNum);
        w.padToByte();
        classesData = w.takeData();
    }

    // ---- 3. 组装文件 ---------------------------------------------------------------
    std::vector<std::uint8_t> out;
    out.reserve(4096 + writer.d->objects.size() * 32);
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

    // 3.3 AuxHeader（R2000 六节文件的第五节；libredwg 对六节文件会无条件
    // 解析该节，缺省 (0,0) 会在 0 地址读出垃圾并中止后续解码）。
    // 布局依 libredwg auxheader.spec（R2000 分支，共 123 字节）。
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
        putAuxRL(writer.d->nextHandle); putAuxRL(0);   // HANDSEED（RLL）
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
    // 未设置预览时写 libredwg 编码端同款的规范空形式（RL 5 + RC 0）。
    const std::uint32_t thumbnailAddr = static_cast<std::uint32_t>(out.size());
    putSentinel(kSentThumbnail, true);
    if (!writer.d->thumbnail.empty()) {
        putBytes(writer.d->thumbnail.data(), writer.d->thumbnail.size());
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
    // （libredwg encode.c "Unknown bitlong inter class and object"；样例实测同值）。
    // 注意：它不计入 classes 节的声明尺寸（尺寸在上一步已定），但会占用文件空间，
    // 对象数据的起始偏移因此后移 4 字节。缺失会让严格读取器解析对象数据整体错位。
    putRL(0x0DCA);

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
            Private::Obj* o = writer.d->findObject(blk.ents[i]);
            if (o == nullptr) continue;
            const std::uint32_t prev = (i == 0) ? 0u : blk.ents[i - 1];
            const std::uint32_t next = (i + 1 == blk.ents.size()) ? 0u : blk.ents[i + 1];
            patchLink(o->body, o->prevBit, prev);
            patchLink(o->body, o->nextBit, next);
        }
    };
    patchBlockLinks(writer.d->mspace);
    patchBlockLinks(writer.d->pspace);
    for (auto& entry : writer.d->blocks) patchBlockLinks(entry.second);

    // 3.6 对象数据区（按句柄分配顺序）
    for (Private::Obj& o : writer.d->objects) {
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
        for (const Private::Obj& o : writer.d->objects)
            entries.emplace_back(o.handle, o.offset);
        std::sort(entries.begin(), entries.end());

        std::size_t i = 0;
        std::uint32_t lastHandle = 0, lastOffset = 0;
        while (i < entries.size()) {
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
        // 第二头部的 14 个句柄（顺序与 libredwg 名表一致）：
        // 0=HANDSEED 1..9=各控制对象 10=VX控制 11=命名对象字典 12/13=字典
        // 本写出器不生成命名字典，11..13 留空（0）。
        const std::uint32_t handles2[14] = {
            writer.d->nextHandle, writer.d->blockCtrl, writer.d->layerCtrl, writer.d->styleCtrl, writer.d->ltypeCtrl,
            writer.d->viewCtrl, writer.d->ucsCtrl, writer.d->vportCtrl, writer.d->appidCtrl, writer.d->dimstyleCtrl,
            writer.d->vpEntHeaderCtrl, writer.d->nod, writer.d->dictMline, writer.d->dictGroup };
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

    // 3.9 R14 模板节（4 字节，libredwg 对 4 号节同样会无条件解析）
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
    std::ofstream file(writer.d->fileName.c_str(), std::ios_base::out | std::ios::binary | std::ios::trunc);
    if (!file.is_open()) return false;
    file.write(reinterpret_cast<const char*>(out.data()),
               static_cast<std::streamsize>(out.size()));
    const bool ok = file.good();
    file.close();
    return ok;
}

} // namespace dwgWriter15
