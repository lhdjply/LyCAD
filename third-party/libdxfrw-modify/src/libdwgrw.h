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

// 版本写出实现（定义于 intern/dwgwriter15.cpp，声明见 intern/dwgwriter15.h）。
// dwgW::write() 在这里做版本分派；新增版本时在此追加对应的 dwgWriterNN。
namespace dwgWriter15 {
bool supports(DRW::Version ver) noexcept;
bool write(dwgW& writer, DRW_Interface* iface);
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
//  实现位置
//    · 本头文件只声明接口；**全部实现位于 intern/dwgwriter15.cpp**，
//      与读取端的命名对称（15 = DWG 内部版本号 = AC1015 = AutoCAD 2000，
//      对应 intern/dwgreader15.cpp）。
//    · intern/dwgwriter.h/.cpp 是**与版本无关的位级原语层**
//      （dwgBitWriter / CRC16 / 哨兵），由各版本写出器共用。
//
//  设计要点
//    · 对外 API 与 dxfRW 的写出侧同构（writeLine / writeLayer / writeBlock…），
//      上游适配器（LibDxfRwAdapter 的 Exporter）无需修改即可在两者之间切换。
//    · 目标版本目前仅 AC1015。
//    · 句柄由实现统一分配；表记录/块/实体间的引用（图层、线型、文字样式、
//      匿名块）按名字解析为句柄。
//    · 文件结构（版本头、节定位、header/classes/对象映射节、哨兵与 CRC）
//      依据读取端 dwgreader15/dwgreader.cpp 的解析契约实现，并对照
//      libredwg encode.c 与实测样例文件（sample_2000.dwg）交叉验证。
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
    bool writeArc(DRW_Arc* ent);
    bool writeEllipse(DRW_Ellipse* ent);
    bool writeSolid(DRW_Solid* ent);
    bool writeLWPolyline(DRW_LWPolyline* ent);
    /// 兼容入口：R2000 目标下 2D 多段线统一编码为 LWPOLYLINE
    ///（POLYLINE/VERTEX 是 R12 时代的对象链）。3D 多段线不被本写出器支持。
    bool writePolyline(DRW_Polyline* ent);
    bool writeSpline(DRW_Spline* ent);
    bool writeHatch(DRW_Hatch* ent);
    bool writeDimension(DRW_Dimension* ent);
    bool writeText(DRW_Text* ent);
    bool writeMText(DRW_MText* ent);

private:
    dwgW(const dwgW& other) = delete;
    dwgW& operator=(const dwgW& other) = delete;

public:
    /// 实现细节（intern/dwgwriter15.cpp 内的辅助函数需要直接引用）
    struct Private;

private:
    /// 版本写出实现需要访问内部状态（d）
    friend bool dwgWriter15::write(dwgW& writer, DRW_Interface* iface);

    Private* d;
};

#endif // LIBDWGRW_H
