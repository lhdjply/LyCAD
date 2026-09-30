// =============================================================================
//  LyCAD - io/DxfWriter.h
//  DXF 写出器。
//
//  支持两个目标版本：
//    AC1009 (R12)  —— 默认值。结构最简、无句柄与字典区要求，
//                     被 AutoCAD / LibreCAD / ezdxf / Inkscape 等广泛读取，
//                     是互操作性最强的 DXF 形态。多段线以 POLYLINE+VERTEX 表达。
//    AC1015 (R2000) —— 现代格式，含句柄、拥有者引用、子类标记、BLOCK_RECORD
//                     表与 OBJECTS 字典区，多段线以 LWPOLYLINE 表达。
//
//  选择 R12 作为默认是刻意的工程取舍：R2000 对句柄-拥有者引用链与字典区的
//  完备性要求严格，任何一处缺失都可能被严格读取器判为非法；而 R12 无此负担。
// =============================================================================
#pragma once

#include "io/CadCodec.h"
#include "io/DxfGroup.h"

#include <QSet>
#include <QString>

#include <cstdint>
#include <vector>

class QIODevice;

namespace lycad {

class Document;
class Entity;
class Layer;
class LineEntity;
class CircleEntity;
class ArcEntity;
class PolylineEntity;
class PointEntity;
class TextEntity;
class HatchEntity;
class DimensionEntity;

class DxfWriter {
public:
    DxfWriter(QIODevice* device, WriteOptions options);

    IoResult write(const Document& document);

private:
    /// 尺寸标注的匿名几何块（*Dnn）。DXF 的 DIMENSION 实体只存定义点，
    /// 显示几何由读取方按公式重绘；为了让不具备重绘能力的旧软件也能显示，
    /// 导出时同时写出一个包含尺寸线/箭头/文字的匿名块。
    struct DimensionBlockInfo {
        QString name;               ///< 块名（*D1、*D2……）
        QString recordHandle;       ///< R2000 的 BLOCK_RECORD 句柄
        const DimensionEntity* dim = nullptr;
    };

    // ---- 区段 ----------------------------------------------------------------
    void openSection(const char* name);
    void closeSection();

    void writeHeader();
    void writeTables();
    void writeLinetypeTable();
    void writeLayerTable();
    void writeStyleTable();
    void writeAppIdTable();
    void writeBlockRecordTable();
    void writeBlocks();
    void writeEntities();
    void writeObjects();

    // ---- 图元 ----------------------------------------------------------------
    void writeEntity(const Entity& entity);
    void writeCommonEntityFields(const Entity& entity, const char* subclassMarker);
    void writeLine(const LineEntity& line);
    void writeCircle(const CircleEntity& circle);
    void writeArc(const ArcEntity& arc);
    void writePolyline(const PolylineEntity& polyline);
    void writePolylineR12(const PolylineEntity& polyline);
    void writePolylineR2000(const PolylineEntity& polyline);
    void writePoint(const PointEntity& point);
    void writeText(const TextEntity& text);
    void writeHatch(const HatchEntity& hatch);
    void writeDimension(const DimensionEntity& dim);

    // ---- 块 ------------------------------------------------------------------
    /// 写出 *Model_Space / *Paper_Space 两个必需的空块
    void writeSpaceBlock(const QString& name, const QString& ownerRecordHandle);
    /// 写出一个尺寸标注的匿名几何块（内容：尺寸线、界线、箭头、测量文字）
    void writeDimensionBlock(const DimensionBlockInfo& info);

    // ---- 辅助 ----------------------------------------------------------------
    bool isR2000() const;
    QString dxfVersion() const;

    /// 分配一个句柄（R2000 使用）
    QString nextHandle();

    /// 计算全图范围，用于 $EXTMIN / $EXTMAX
    Box2D computeExtents() const;

    /// 收集实际被引用的线型名（含 CONTINUOUS）
    QStringList referencedLinetypes() const;

    /// 图元颜色（-1 表示随层，不输出 62 组码）
    int entityColorIndex(const Entity& entity) const;

    /// 预扫描文档，为每个尺寸标注分配匿名块名与句柄
    void prescanDimensionBlocks();

    QIODevice* m_device = nullptr;
    DxfGroupWriter m_out;
    WriteOptions m_options;
    const Document* m_document = nullptr;

    quint64 m_handleSeed = 0x30;
    int m_entityCount = 0;
    /// R12 导出时被降级为边界多段线的填充图元数量
    int m_hatchDowngrades = 0;
    /// R2000 中 *Paper_Space 块记录的句柄，需在写出 BLOCKS 区时被引用
    QString m_paperSpaceRecordHandle;
    std::vector<DimensionBlockInfo> m_dimBlocks;
};

} // namespace lycad
