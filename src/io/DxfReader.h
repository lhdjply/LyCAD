// =============================================================================
//  LyCAD - io/DxfReader.h
//  DXF 读入器。
//
//  覆盖范围
//    · 版本：AutoCAD R12 (AC1009) 至 AutoCAD 2018 (AC1032) 的 ASCII 与二进制 DXF。
//      高版本的新实体若不在支持列表内会被跳过并计入警告，不影响其余内容。
//    · 实体：POINT / LINE / CIRCLE / ARC / LWPOLYLINE / POLYLINE(二维) / SOLID / TRACE。
//    · 符号表：LAYER（名称、颜色、线型、线宽、开关/冻结/锁定标志）。
//    · 块与引用：BLOCKS 区被完整解析，INSERT 在导入时按插入点、比例、旋转角
//      展开为独立图元，并遵循「块内 0 层图元继承 INSERT 图层」的标准语义。
//
//  设计取舍
//    块引用选择「导入即展开」而非保留引用语义，是因为本软件当前不建模块定义。
//    这样做的代价是文件往返后块会被打散；收益是图形内容零丢失，且不引入一个
//    半成品的块引用体系。该行为会在导入结果中明确告知用户。
//
//  未建模的实体（TEXT/MTEXT/HATCH/DIMENSION/SPLINE/ELLIPSE/三维实体等）会按类型
//  汇总计数后跳过，不做静默丢弃。
// =============================================================================
#pragma once

#include "core/Entity.h"
#include "core/Types.h"
#include "io/CadCodec.h"
#include "io/DxfGroup.h"

#include <QHash>
#include <QString>

#include <cstddef>
#include <memory>
#include <vector>

namespace lycad {

class Document;
class PolylineEntity;

class DxfReader {
public:
    DxfReader();

    /// 读取 DXF 并装配到 document。调用方应在失败时丢弃该 document。
    IoResult read(const QString& path, Document& document);

private:
    /// 块定义内部的 INSERT 引用。
    ///
    /// 不在解析 BLOCKS 区的过程中立即展开：该区段中块的先后顺序并无保证，
    /// 被引用的块可能尚未出现。因此此处仅记录参数，待全部块就位后在插入点统一展开。
    struct BlockInsertRef {
        QString targetBlock;        ///< 被引用的块名
        Point2D insertPoint;
        double  scaleX = 1.0;
        double  scaleY = 1.0;
        double  rotation = 0.0;     ///< 弧度
        QString layer;
    };

    /// 块定义（位于 BLOCKS 区，坐标基于块基点）
    struct BlockDefinition {
        QString name;
        Point2D basePoint;
        std::vector<std::unique_ptr<Entity>> entities;
        std::vector<BlockInsertRef> nestedInserts;
    };

    // ---- 区段 ----
    bool parseDocument(Document& document, IoResult* result);
    bool readHeaderSection(IoResult* result);
    bool readTablesSection(Document& document, IoResult* result);
    bool readBlocksSection(IoResult* result);
    bool readEntitiesSection(Document& document, IoResult* result);
    bool skipSection(IoResult* result);

    // ---- 记录级工具 ----
    /// 消费当前记录的全部组码，直到遇到下一个 0 组（该组被推回流中）
    std::vector<DxfGroup> collectUntilNextRecord();
    bool applyLayerRecord(Document& document, const std::vector<DxfGroup>& groups,
                          IoResult* result);

    // ---- 块 ----
    bool readBlockBody(BlockDefinition& block, IoResult* result);
    void skipUntilBlockEnd();
    void explodeBlock(const QString& blockName,
                      const Transform2D& transform,
                      const QString& insertLayer,
                      int depth,
                      std::vector<std::unique_ptr<Entity>>& out,
                      IoResult* result);
    const BlockDefinition* findBlock(const QString& name) const;

    // ---- 图元 ----
    /// 读取一个不含子实体的图元（返回可能在语义上展开为多个图元）
    std::vector<std::unique_ptr<Entity>> readSimpleEntity(const QString& type, IoResult* result);
    std::vector<std::unique_ptr<Entity>> buildEntities(const QString& type,
                                                       const std::vector<DxfGroup>& groups,
                                                       IoResult* result);
    std::vector<std::unique_ptr<Entity>> parseLwPolyline(const std::vector<DxfGroup>& groups,
                                                         IoResult* result);
    std::vector<std::unique_ptr<Entity>> readLegacyPolyline(IoResult* result);
    std::vector<std::unique_ptr<Entity>> readInsert(IoResult* result);
    void consumeVertexSequence();

    static void applyCommonProperties(const std::vector<DxfGroup>& groups, Entity& entity);

    void collectWarnings(IoResult* result);

    std::unique_ptr<DxfGroupReader> m_reader;

    bool    m_binary = false;
    QString m_acadVersion;
    int     m_insUnits = 0;
    QString m_headerCurrentLayer;

    std::vector<BlockDefinition> m_blocks;
    QHash<QString, std::size_t>  m_blockIndex;

    QHash<QString, int> m_skippedTypes;
    int m_explodedInserts = 0;
    int m_insertDepthViolations = 0;
};

} // namespace lycad
