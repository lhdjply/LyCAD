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

#ifndef LIBDWGRW_H
#define LIBDWGRW_H

#include <string>
//#include <deque>
#include "drw_entities.h"
#include "drw_objects.h"
#include "drw_classes.h"
#include "drw_interface.h"

#include <cstdint>
#include <vector>

class dwgReader;
class dwgW;

// 各版本**文件容器**的入口（定义见 intern/dwgwriterNN.cpp，声明权威在各自的
// intern/dwgwriterNN.h）。dwgW::write() 按目标版本分派到这里，与读取端 dwgR
// 按版本创建 dwgReader15/18/… 的做法一致。
// 节载荷（header/classes/对象流）由 dwgW::buildSections() 统一构建后交给容器摆放。
namespace dwgWriter15 {
bool supports(DRW::Version ver) noexcept;
bool write(dwgW& writer, DRW_Interface* iface);   // AC1015 / R2000：线性节容器
}
namespace dwgWriter18 {
bool supports(DRW::Version ver) noexcept;
bool write(dwgW& writer, DRW_Interface* iface);   // AC1018 / R2004：分页 + LZ77 容器
}
namespace dwgWriter21 {
bool supports(DRW::Version ver) noexcept;
bool write(dwgW& writer, DRW_Interface* iface);   // AC1021 / R2007：RS + 分页容器
}

class dwgR {
public:
    dwgR(const char* name);
    ~dwgR();
    //read: return true if all ok
    bool read(DRW_Interface *interface_, bool ext);
    bool getPreview();
    DRW::Version getVersion(){return version;}
    DRW::error getError(){return error;}
bool testReader();
    void setDebug(DRW::DBG_LEVEL lvl);

private:
    bool openFile(std::ifstream *filestr);
    bool processDwg();
private:
    DRW::Version version;
    DRW::error error;
    std::string fileName;
    bool applyExt; /*apply extrusion in entities to conv in 2D?*/
    std::string codePage;
    DRW_Interface *iface;
    dwgReader *reader;

};

//------------------------------------------------------------------------------
//  dwgW：DWG 原生写出器（libdxfrw 的 DWG 写出半边）。
//
//  实现位置（与读取端 dwgR 完全对称：对外类在本层，版本实现单独成文件）
//    · **主体实现**（Private、各 writeXxx 回调、节构建 buildSections）全部位于
//      src/libdwgrw.cpp —— 与 dwgR 同置一处。
//    · 各版本的**文件容器**单独成文件，只负责把节载荷摆进文件、写哨兵与 CRC：
//        intern/dwgwriter15.cpp = AC1015 / R2000（线性节）
//        intern/dwgwriter18.cpp = AC1018 / R2004（分页 + LZ77）
//        intern/dwgwriter21.cpp = AC1021 / R2007（RS 纠错 + AC21 压缩流）
//      版本分派在 libdwgrw.cpp::dwgW::write() 中完成。
//    · intern/dwgwriter.h/.cpp 是**与版本无关的位级原语层**
//      （dwgBitWriter / CRC16 / 哨兵），由各版本容器共用。
//
//  设计要点
//    · 对外 API 与 dxfRW 的写出侧同构（writeLine / writeLayer / writeBlock…），
//      上游适配器（LibDxfRwAdapter 的 Exporter）无需修改即可在两者之间切换。
//    · 目标版本目前为 AC1015 / AC1018 / AC1021。
//    · 句柄由实现统一分配；表记录/块/实体间的引用（图层、线型、文字样式、
//      匿名块）按名字解析为句柄。
//    · 文件结构（版本头、节定位、header/classes/对象映射节、哨兵与 CRC）
//------------------------------------------------------------------------------
class DRW_Header;
class DRW_LType;
class DRW_Layer;
class DRW_Textstyle;
class DRW_Dimstyle;
class DRW_AppId;
class DRW_Vport;
class DRW_Block;
class DRW_Point;
class DRW_Line;
class DRW_Ray;
class DRW_Xline;
class DRW_Circle;
class DRW_Arc;
class DRW_Ellipse;
class DRW_Solid;
class DRW_LWPolyline;
class DRW_Spline;
class DRW_Hatch;
class DRW_Dimension;
class DRW_Text;
class DRW_MText;

class dwgW {
public:
    explicit dwgW(const char* name);
    ~dwgW();

    /// 写出整个文件。目标版本仅支持 AC1015，其余返回失败。
    bool write(DRW_Interface* iface, DRW::Version ver);

    /// 设置预览图。data 为预览区原始字节（[RL 图像字节数][图像字节…]，
    /// 不含 BEGIN/END 哨兵）——把既有 DWG 的预览区原样传入即可携带预览。
    /// 未调用时写规范空缩略图（表示本文件不含预览图）。
    void setThumbnail(std::vector<std::uint8_t> data);

    // ---- 以下方法与 dxfRW 同名同义，供适配器复用写出回调 -------------------
    bool writeLineType(DRW_LType* ent);
    bool writeLayer(DRW_Layer* ent);
    bool writeTextstyle(DRW_Textstyle* ent);
    bool writeDimstyle(DRW_Dimstyle* ent);
    bool writeAppId(DRW_AppId* ent);
    bool writeVport(DRW_Vport* ent);
    bool writeBlockRecord(std::string name);
    bool writeBlock(DRW_Block* bk);

    bool writePoint(DRW_Point* ent);
    bool writeLine(DRW_Line* ent);
    /// 无界直线：RAY 自基点单向延伸，XLINE 双向延伸。
    /// 类型数据只有「基点 + 方向」两个 3BD，与读取端 DRW_Ray::parseDwg 的
    /// 消费顺序严格对应（无厚度/挤出字段）。
    bool writeRay(DRW_Ray* ent);
    bool writeXline(DRW_Xline* ent);
    bool writeCircle(DRW_Circle* ent);
    /// 块参照：块内与模型空间通用。写出时引用 BLOCK_RECORD 句柄，
    /// 由预扫描阶段登记的块记录解析得到。
    bool writeInsert(DRW_Insert* ent);
    bool writeArc(DRW_Arc* ent);
    bool writeEllipse(DRW_Ellipse* ent);
    bool writeSolid(DRW_Solid* ent);
    bool writeLWPolyline(DRW_LWPolyline* ent);
    /// 兼容入口：R2000 目标下 2D 多段线统一编码为 LWPOLYLINE
    ///（POLYLINE/VERTEX 是 R12 时代的对象链）。3D 多段线不被本写出器支持。
    bool writePolyline(DRW_Polyline* ent);
    bool writeSpline(DRW_Spline* ent);
    bool writeHatch(DRW_Hatch* ent);
    /// 表格：DWG 里是类对象（CLASSES 段的 ACAD_TABLE，类号随段内顺序而定）。
    /// 对象头写类号；专属字段 = AcDbBlockReference 段 + AcDbTable 段。
    /// 几何（网格线/单元文字）仍存在 *Tnn 匿名块里，由本实体按名字引用。
    bool writeTable(DRW_Table* ent);
    bool writeDimension(DRW_Dimension* ent);
    /// 弧长标注：DWG 里是类对象（CLASSES 段的 ARC_DIMENSION），
    /// 对象头写类号而非固定类型号。
    bool writeArcDimension(DRW_DimArc* ent);
    /// 光源：DWG 里是类对象（CLASSES 段的 LIGHT / AcDbLight，所属模块 SCENEOE），
    /// 对象头写类号。本体没有几何，只有那一组属性。**仅 R2007+ 有**这个类，
    /// 更低版本的文件里不登记、也不该写出。
    bool writeLight(DRW_Light* ent);
    bool writeText(DRW_Text* ent);
    /// 块属性：ATTRIB（块参照上的值）与 ATTDEF（块定义里的定义）。
    /// 两者与 TEXT 共用同一段文字数据，尾部再多「标签 + 字段长度 + 标志(+提示)」。
    bool writeAttrib(DRW_Attrib* ent);
    bool writeAttdef(DRW_Attdef* ent);
    /// 属性列表的结束标记。句柄在写 INSERT 时就已一并占下。
    bool writeSeqEnd(DRW_Entity* ent);
    bool writeMText(DRW_MText* ent);

    // ---- 与容器无关的节载荷（供各版本文件容器复用）---------------------------
    /// 一条待写出的对象记录（自 oType 起、句柄流止，含补位；不含 MS 尺寸与 CRC）。
    struct ObjectRecord {
        std::uint32_t handle = 0;
        std::vector<std::uint8_t> body;
        std::uint32_t offset = 0;               ///< MS 起始文件偏移（容器装配期填写）
        /// R2010+ 对象记录在 MS 尺寸之后还有一个 UMC「句柄流位数」
        ///（libredwg: `obj->handlestream_size`，不计入 obj->size），
        /// 读取端据此用 `obj->bitsize = size*8 - handlestream_size` 找句柄流。
        /// 2007 及更早为空记录格式（该字段不用）。
        std::uint32_t handleBits = 0;
    };

    /// 与容器无关的节载荷。
    /// 各版本容器的差别只在「节如何摆进文件」：AC1015 是哨兵界定的线性节，
    /// AC1018 是分页 + LZ77；节的**内容**（header 变量位流、classes 位流、
    /// 对象流、句柄流）完全一致，故集中由 buildSections() 构建一次。
    struct SectionData {
        /// header 变量位流（不含哨兵 / RL 尺寸）。
        /// AC1015/AC1018：变量流与内联句柄混在一条流里；
        /// AC1021：已按 2007 形态装配为 [RL bitSize][变量流][字符串区][u16 位数][标志][句柄流]。
        std::vector<std::uint8_t> headerData;
        /// classes 位流（不含哨兵 / RL 尺寸 / CRC）。
        /// AC1015/AC1018：仅类记录，容器再补前导；
        /// AC1021：已是完整数据区 [RL bitSize][BS 最大类号][RC][RC][位][类记录][字符串区][u16][标志]。
        std::vector<std::uint8_t> classesData;
        std::uint16_t codepageId = 0;            ///< $DWGCODEPAGE 对应的代码页标识
        std::uint16_t maxClassNum = 0;           ///< 最大类号（= 499 + 自定义类数）
        std::vector<ObjectRecord> objects;       ///< 对象表（prev/next 链已回填）
        std::uint32_t nextHandle = 0;            ///< HANDSEED（下一个空闲句柄）
        std::string   fileName;                  ///< 输出文件名
        /// ---- AC18 元数据段所需的杂项（AC15 容器不使用）----
        std::uint16_t measurement = 1;           ///< $MEASUREMENT（AcDb:Template：0=英制 1=公制）
        std::uint32_t tdDay = 0;                 ///< 当前时间的 Julian 日（TDCREATE/TDUPDATE）
        std::uint32_t tdMsec = 0;                ///< 当日毫秒
        /// ---- 容器装配所需的附加信息（文件头 / AuxHeader / 第二头部引用）----
        std::vector<std::uint8_t> thumbnail;     ///< 预览区原始字节（不含 BEGIN/END 哨兵）
        std::uint32_t ltypeCtrl = 0, layerCtrl = 0, styleCtrl = 0, vportCtrl = 0;
        std::uint32_t dimstyleCtrl = 0, appidCtrl = 0, blockCtrl = 0, viewCtrl = 0, ucsCtrl = 0;
        std::uint32_t vpEntHeaderCtrl = 0;       ///< 视口实体头控制（R2000 必需）
        std::uint32_t nod = 0;                   ///< 根字典 NOD
        std::uint32_t dictGroup = 0;             ///< ACAD_GROUP
        std::uint32_t dictMline = 0;             ///< ACAD_MLINESTYLE
    };

    /// 收集并编码全部节数据：调用 iface 的各 writeXxx 回调、分配句柄、把对象
    /// 写进内部对象表，并把容器装配所需的元信息一并导出到 out。成功返回 true。
    /// ver 决定 header 变量的版本形态（AC1015 与 AC1018 有两处差异）。
    /// 这是各版本容器共用的入口，不属于 dwgW 的常规对外使用面。
    bool buildSections(DRW_Interface* iface, DRW::Version dwgVer, SectionData& out);

private:
    dwgW(const dwgW& other) = delete;
    dwgW& operator=(const dwgW& other) = delete;

public:
    /// 实现细节（src/libdwgrw.cpp 内的辅助函数需要直接引用）
    struct Private;

private:
    Private* d;
};

#endif // LIBDWGRW_H
