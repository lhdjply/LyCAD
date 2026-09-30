// =============================================================================
//  LyCAD - io/DxfWriter.cpp
// =============================================================================
#include "io/DxfWriter.h"

#include "core/ArcEntity.h"
#include "core/CircleEntity.h"
#include "core/DimensionEntity.h"
#include "core/Document.h"
#include "core/HatchEntity.h"
#include "core/LineEntity.h"
#include "core/Linetype.h"
#include "core/PointEntity.h"
#include "core/PolylineEntity.h"
#include "core/TextEntity.h"

#include <QIODevice>
#include <QSet>

#include <cmath>

namespace lycad {

namespace {

// ---- R2000 结构中的固定句柄（与 AutoCAD 输出保持一致，便于比对与互操作） ----
constexpr quint64 kHandleBlockRecordTable = 0x01;
constexpr quint64 kHandleLayerTable       = 0x02;
constexpr quint64 kHandleStyleTable       = 0x03;
constexpr quint64 kHandleLtypeTable       = 0x05;
constexpr quint64 kHandleAppIdTable       = 0x09;
constexpr quint64 kHandleRootDictionary   = 0x0C;
constexpr quint64 kHandleGroupDictionary  = 0x0D;
constexpr quint64 kHandleMlineStyleDict   = 0x11;
constexpr quint64 kHandleStandardMline    = 0x12;
constexpr quint64 kHandlePlotStyleDict    = 0x13;
constexpr quint64 kHandlePlotPlaceholder  = 0x17;
constexpr quint64 kHandleModelSpaceRecord = 0x1F;
constexpr quint64 kHandleContinuousLtype  = 0x14;
constexpr quint64 kHandleStandardStyle    = 0x15;
constexpr quint64 kHandleAcadAppId        = 0x16;

QString handleText(quint64 handle)
{
    return QString::number(handle, 16).toUpper();
}

} // namespace

DxfWriter::DxfWriter(QIODevice* device, WriteOptions options)
    : m_device(device)
    , m_out(device)
    , m_options(std::move(options))
{
}

bool DxfWriter::isR2000() const
{
    return m_options.version.compare(QLatin1String("AC1009"), Qt::CaseInsensitive) != 0;
}

QString DxfWriter::dxfVersion() const
{
    return isR2000() ? QStringLiteral("AC1015") : QStringLiteral("AC1009");
}

QString DxfWriter::nextHandle()
{
    return handleText(m_handleSeed++);
}

void DxfWriter::prescanDimensionBlocks()
{
    m_dimBlocks.clear();
    int index = 0;
    for (EntityId id : m_document->drawOrder()) {
        const Entity* entity = m_document->entity(id);
        if (!entity || entity->type() != EntityType::Dimension) continue;

        DimensionBlockInfo info;
        info.name = QStringLiteral("*D%1").arg(++index);
        info.recordHandle = nextHandle();       // R2000 的 BLOCK_RECORD 句柄
        info.dim = static_cast<const DimensionEntity*>(entity);
        m_dimBlocks.push_back(std::move(info));
    }
}

void DxfWriter::openSection(const char* name)
{
    m_out.write(0, QStringLiteral("SECTION"));
    m_out.write(2, QString::fromLatin1(name));
}

void DxfWriter::closeSection()
{
    m_out.write(0, QStringLiteral("ENDSEC"));
}

// -----------------------------------------------------------------------------
//  主入口
// -----------------------------------------------------------------------------
IoResult DxfWriter::write(const Document& document)
{
    if (!m_device) return IoResult::failure(QStringLiteral("输出设备为空。"));
    if (!m_device->isOpen() && !m_device->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return IoResult::failure(QStringLiteral("无法打开文件用于写入：%1").arg(m_device->errorString()));
    }

    m_document = &document;
    m_handleSeed = 0x30;
    m_hatchDowngrades = 0;

    // 预扫描尺寸标注：BLOCK_RECORD 表与 BLOCKS 区都需要提前知道
    // 每个标注对应的匿名块名，因此必须先于 TABLES 区完成
    prescanDimensionBlocks();

    m_out.writeComment(QStringLiteral("LyCAD 生成的 DXF 文件 (目标版本 %1)").arg(dxfVersion()));

    writeHeader();
    writeTables();
    // 两个版本都写出 BLOCKS 区：R2000 结构上必需；R12 虽可省略，
    // 但尺寸标注的匿名几何块在 R12 下同样需要载体
    writeBlocks();
    writeEntities();
    if (isR2000()) writeObjects();

    m_out.write(0, QStringLiteral("EOF"));

    if (!m_out.flush()) {
        return IoResult::failure(QStringLiteral("写入文件时出错：%1").arg(m_device->errorString()));
    }

    IoResult result = IoResult::success(document.entityCount());
    result.addWarning(QStringLiteral("目标版本 %1").arg(dxfVersion()));
    if (m_hatchDowngrades > 0) {
        result.addWarning(QStringLiteral("%1 个填充图元在 R12 中降级为闭合多段线（R12 不支持 HATCH 实体）")
                              .arg(m_hatchDowngrades));
    }
    return result;
}

// -----------------------------------------------------------------------------
//  HEADER
// -----------------------------------------------------------------------------
void DxfWriter::writeHeader()
{
    const Box2D extents = computeExtents();

    openSection("HEADER");

    m_out.write(9, QStringLiteral("$ACADVER"));
    m_out.write(1, dxfVersion());

    if (isR2000()) {
        m_out.write(9, QStringLiteral("$HANDSEED"));
        m_out.write(5, handleText(m_handleSeed + 1));
    }

    m_out.write(9, QStringLiteral("$DWGCODEPAGE"));
    m_out.write(3, QStringLiteral("ANSI_1252"));

    m_out.write(9, QStringLiteral("$INSUNITS"));
    m_out.write(70, 4);                       // 4 = 毫米

    m_out.write(9, QStringLiteral("$MEASUREMENT"));
    m_out.write(70, 1);                       // 1 = 公制

    m_out.write(9, QStringLiteral("$LTSCALE"));
    m_out.write(40, 1.0);

    // 以下三个变量为 R2000 引入，R12 文件中不写，避免向旧读取器暴露未知变量
    if (isR2000()) {
        m_out.write(9, QStringLiteral("$CELTSCALE"));
        m_out.write(40, 1.0);

        m_out.write(9, QStringLiteral("$CELTSTYLE"));
        m_out.write(70, 0);

        m_out.write(9, QStringLiteral("$CELWEIGHT"));
        m_out.write(370, -1);
    }

    m_out.write(9, QStringLiteral("$CLAYER"));
    m_out.write(8, m_document->layers().currentLayerName());

    m_out.write(9, QStringLiteral("$CECOLOR"));
    m_out.write(62, 256);                     // ByLayer

    m_out.write(9, QStringLiteral("$CELTYPE"));
    m_out.write(6, QStringLiteral("ByLayer"));

    m_out.write(9, QStringLiteral("$PDMODE"));
    m_out.write(70, 0);

    m_out.write(9, QStringLiteral("$PDSIZE"));
    m_out.write(40, 0.0);

    m_out.write(9, QStringLiteral("$LUNITS"));
    m_out.write(70, 2);                       // 2 = 十进制

    m_out.write(9, QStringLiteral("$LUPREC"));
    m_out.write(70, 4);

    m_out.write(9, QStringLiteral("$AUNITS"));
    m_out.write(70, 0);

    m_out.write(9, QStringLiteral("$AUPREC"));
    m_out.write(70, 0);

    m_out.write(9, QStringLiteral("$TEXTSIZE"));
    m_out.write(40, 3.5);

    m_out.write(9, QStringLiteral("$EXTMIN"));
    m_out.writePoint(10, extents.isValid() ? extents.minCorner() : Point2D(0.0, 0.0));

    m_out.write(9, QStringLiteral("$EXTMAX"));
    m_out.writePoint(10, extents.isValid() ? extents.maxCorner() : Point2D(420.0, 297.0));

    m_out.write(9, QStringLiteral("$LIMMIN"));
    m_out.writePoint(10, Point2D(0.0, 0.0));

    m_out.write(9, QStringLiteral("$LIMMAX"));
    m_out.writePoint(10, extents.isValid() ? extents.maxCorner() : Point2D(420.0, 297.0));

    m_out.write(9, QStringLiteral("$ORTHOMODE"));
    m_out.write(70, 0);

    m_out.write(9, QStringLiteral("$ATTMODE"));
    m_out.write(70, 1);

    closeSection();
}

// -----------------------------------------------------------------------------
//  TABLES
// -----------------------------------------------------------------------------
void DxfWriter::writeTables()
{
    openSection("TABLES");
    if (isR2000()) writeBlockRecordTable();
    writeLinetypeTable();
    writeLayerTable();
    writeStyleTable();
    writeAppIdTable();
    closeSection();
}

QStringList DxfWriter::referencedLinetypes() const
{
    QSet<QString> used;
    used.insert(QStringLiteral("CONTINUOUS"));

    for (const Layer& layer : m_document->layers().all()) {
        const QString name = layer.linetype().trimmed().toUpper();
        if (!name.isEmpty()) used.insert(name);
    }
    for (const auto& pair : m_document->drawOrder()) {
        if (const Entity* e = m_document->entity(pair)) {
            const QString name = e->linetypeName().trimmed().toUpper();
            if (!name.isEmpty()) used.insert(name);
        }
    }

    // 只保留内置定义中确实存在的线型，未定义的引用在 DXF 中会引发读取警告
    QStringList out;
    for (const LinetypePattern& p : linetypes::standard()) {
        if (used.contains(p.name.toUpper())) out << p.name;
    }
    if (out.isEmpty()) out << QStringLiteral("CONTINUOUS");
    return out;
}

void DxfWriter::writeLinetypeTable()
{
    const QStringList used = referencedLinetypes();

    m_out.write(0, QStringLiteral("TABLE"));
    m_out.write(2, QStringLiteral("LTYPE"));
    if (isR2000()) {
        m_out.write(5, handleText(kHandleLtypeTable));
        m_out.write(330, QStringLiteral("0"));
        m_out.write(100, QStringLiteral("AcDbSymbolTable"));
    }
    m_out.write(70, static_cast<int>(used.size()));

    for (const QString& name : used) {
        const LinetypePattern pattern = linetypes::resolve(name);

        m_out.write(0, QStringLiteral("LTYPE"));
        if (isR2000()) {
            m_out.write(5, name == QLatin1String("CONTINUOUS") ? handleText(kHandleContinuousLtype)
                                                               : nextHandle());
            m_out.write(330, handleText(kHandleLtypeTable));
            m_out.write(100, QStringLiteral("AcDbSymbolTableRecord"));
            m_out.write(100, QStringLiteral("AcDbLinetypeTableRecord"));
        }
        m_out.write(2, pattern.name);
        m_out.write(70, 0);
        m_out.write(3, pattern.description);
        m_out.write(72, 65);                                  // 65 = 两端对齐
        m_out.write(73, static_cast<int>(pattern.pattern.size()));
        m_out.write(40, pattern.cycleLength());
        for (double segment : pattern.pattern) {
            m_out.write(49, segment);
        }
    }

    m_out.write(0, QStringLiteral("ENDTAB"));
}

void DxfWriter::writeLayerTable()
{
    m_out.write(0, QStringLiteral("TABLE"));
    m_out.write(2, QStringLiteral("LAYER"));
    if (isR2000()) {
        m_out.write(5, handleText(kHandleLayerTable));
        m_out.write(330, QStringLiteral("0"));
        m_out.write(100, QStringLiteral("AcDbSymbolTable"));
    }
    m_out.write(70, static_cast<int>(m_document->layers().count()));

    for (const Layer& layer : m_document->layers().all()) {
        // 统一取内置线型的规范拼写；未定义的名称回退为 CONTINUOUS，
        // 因为写出器无法为未知线型生成 LTYPE 定义，硬写名称会让读取端报错。
        const QString linetypeName = linetypes::resolve(layer.linetype()).name;

        m_out.write(0, QStringLiteral("LAYER"));
        if (isR2000()) {
            m_out.write(5, nextHandle());
            m_out.write(330, handleText(kHandleLayerTable));
            m_out.write(100, QStringLiteral("AcDbSymbolTableRecord"));
            m_out.write(100, QStringLiteral("AcDbLayerTableRecord"));
        }
        m_out.write(2, layer.name());
        m_out.write(70, layer.flagsForDxf());

        // 图层关闭在 DXF 中以「颜色取负」表达
        int color = layer.colorIndex();
        if (!layer.isVisible()) color = -std::abs(color);
        m_out.write(62, color);

        m_out.write(6, linetypeName);

        if (isR2000()) {
            m_out.write(370, layer.lineweightDxf() < 0 ? -3 : layer.lineweightDxf());
            m_out.write(390, handleText(kHandlePlotPlaceholder));
        }
    }

    m_out.write(0, QStringLiteral("ENDTAB"));
}

void DxfWriter::writeStyleTable()
{
    m_out.write(0, QStringLiteral("TABLE"));
    m_out.write(2, QStringLiteral("STYLE"));
    if (isR2000()) {
        m_out.write(5, handleText(kHandleStyleTable));
        m_out.write(330, QStringLiteral("0"));
        m_out.write(100, QStringLiteral("AcDbSymbolTable"));
    }
    m_out.write(70, 1);

    m_out.write(0, QStringLiteral("STYLE"));
    if (isR2000()) {
        m_out.write(5, handleText(kHandleStandardStyle));
        m_out.write(330, handleText(kHandleStyleTable));
        m_out.write(100, QStringLiteral("AcDbSymbolTableRecord"));
        m_out.write(100, QStringLiteral("AcDbTextStyleTableRecord"));
    }
    m_out.write(2, QStringLiteral("STANDARD"));
    m_out.write(70, 0);
    m_out.write(40, 0.0);
    m_out.write(41, 1.0);
    m_out.write(50, 0.0);
    m_out.write(71, 0);
    m_out.write(42, 2.5);
    m_out.write(3, QStringLiteral("txt"));
    m_out.write(4, QString());

    m_out.write(0, QStringLiteral("ENDTAB"));
}

void DxfWriter::writeAppIdTable()
{
    m_out.write(0, QStringLiteral("TABLE"));
    m_out.write(2, QStringLiteral("APPID"));
    if (isR2000()) {
        m_out.write(5, handleText(kHandleAppIdTable));
        m_out.write(330, QStringLiteral("0"));
        m_out.write(100, QStringLiteral("AcDbSymbolTable"));
    }
    m_out.write(70, 1);

    m_out.write(0, QStringLiteral("APPID"));
    if (isR2000()) {
        m_out.write(5, handleText(kHandleAcadAppId));
        m_out.write(330, handleText(kHandleAppIdTable));
        m_out.write(100, QStringLiteral("AcDbSymbolTableRecord"));
        m_out.write(100, QStringLiteral("AcDbRegAppTableRecord"));
    }
    m_out.write(2, QStringLiteral("ACAD"));
    m_out.write(70, 0);

    m_out.write(0, QStringLiteral("ENDTAB"));
}

void DxfWriter::writeBlockRecordTable()
{
    m_out.write(0, QStringLiteral("TABLE"));
    m_out.write(2, QStringLiteral("BLOCK_RECORD"));
    m_out.write(5, handleText(kHandleBlockRecordTable));
    m_out.write(330, QStringLiteral("0"));
    m_out.write(100, QStringLiteral("AcDbSymbolTable"));
    m_out.write(70, static_cast<int>(2 + m_dimBlocks.size()));

    // 图纸空间与模型空间两个必需的块记录
    m_out.write(0, QStringLiteral("BLOCK_RECORD"));
    m_out.write(5, handleText(kHandleModelSpaceRecord));
    m_out.write(330, handleText(kHandleBlockRecordTable));
    m_out.write(100, QStringLiteral("AcDbSymbolTableRecord"));
    m_out.write(100, QStringLiteral("AcDbBlockTableRecord"));
    m_out.write(2, QStringLiteral("*Model_Space"));
    m_out.write(70, 0);

    m_out.write(0, QStringLiteral("BLOCK_RECORD"));
    m_paperSpaceRecordHandle = nextHandle();
    m_out.write(5, m_paperSpaceRecordHandle);
    m_out.write(330, handleText(kHandleBlockRecordTable));
    m_out.write(100, QStringLiteral("AcDbSymbolTableRecord"));
    m_out.write(100, QStringLiteral("AcDbBlockTableRecord"));
    m_out.write(2, QStringLiteral("*Paper_Space"));
    m_out.write(70, 0);

    // 尺寸标注的匿名几何块记录
    for (const DimensionBlockInfo& info : m_dimBlocks) {
        m_out.write(0, QStringLiteral("BLOCK_RECORD"));
        m_out.write(5, info.recordHandle);
        m_out.write(330, handleText(kHandleBlockRecordTable));
        m_out.write(100, QStringLiteral("AcDbSymbolTableRecord"));
        m_out.write(100, QStringLiteral("AcDbBlockTableRecord"));
        m_out.write(2, info.name);
        m_out.write(70, 0);
    }

    m_out.write(0, QStringLiteral("ENDTAB"));
}

void DxfWriter::writeSpaceBlock(const QString& name, const QString& ownerRecordHandle)
{
    m_out.write(0, QStringLiteral("BLOCK"));
    if (isR2000()) {
        m_out.write(5, nextHandle());
        m_out.write(330, ownerRecordHandle);
        m_out.write(100, QStringLiteral("AcDbEntity"));
    }
    m_out.write(8, QStringLiteral("0"));
    if (isR2000()) {
        m_out.write(100, QStringLiteral("AcDbBlockBegin"));
    }
    m_out.write(2, name);
    m_out.write(70, 0);
    m_out.writePoint(10, Point2D(0.0, 0.0));
    m_out.write(3, name);
    m_out.write(1, QString());
    m_out.write(0, QStringLiteral("ENDBLK"));
    if (isR2000()) {
        m_out.write(5, nextHandle());
        m_out.write(330, ownerRecordHandle);
        m_out.write(100, QStringLiteral("AcDbEntity"));
        m_out.write(8, QStringLiteral("0"));
        m_out.write(100, QStringLiteral("AcDbBlockEnd"));
    }
}

void DxfWriter::writeBlocks()
{
    openSection("BLOCKS");

    // *Model_Space / *Paper_Space 块定义：实体本身写在 ENTITIES 区
    writeSpaceBlock(QStringLiteral("*Model_Space"), handleText(kHandleModelSpaceRecord));
    writeSpaceBlock(QStringLiteral("*Paper_Space"), m_paperSpaceRecordHandle);

    // 尺寸标注的匿名几何块
    for (const DimensionBlockInfo& info : m_dimBlocks) {
        writeDimensionBlock(info);
    }

    closeSection();
}

void DxfWriter::writeDimensionBlock(const DimensionBlockInfo& info)
{
    const DimensionEntity& dim = *info.dim;
    const DimensionEntity::Geometry geo = dim.geometry();

    // ---- 块头 ----
    m_out.write(0, QStringLiteral("BLOCK"));
    if (isR2000()) {
        m_out.write(5, nextHandle());
        m_out.write(330, info.recordHandle);
        m_out.write(100, QStringLiteral("AcDbEntity"));
    }
    m_out.write(8, QStringLiteral("0"));
    if (isR2000()) {
        m_out.write(100, QStringLiteral("AcDbBlockBegin"));
    }
    m_out.write(2, info.name);
    m_out.write(70, 1);                       // bit 1 = 匿名块
    m_out.writePoint(10, Point2D(0.0, 0.0));  // 块基点即世界原点
    m_out.write(3, info.name);
    m_out.write(1, QString());

    // ---- 块内容（约定：图层 0 + 颜色 ByBlock，随 DIMENSION 实体着色） ----
    auto beginBlockEntity = [this, &info](const char* subclassMarker) {
        m_out.write(0, QStringLiteral("%1").arg(QLatin1String(subclassMarker)));
        if (isR2000()) {
            m_out.write(5, nextHandle());
            m_out.write(330, info.recordHandle);
            m_out.write(100, QStringLiteral("AcDbEntity"));
        }
        m_out.write(8, QStringLiteral("0"));
        m_out.write(62, 0);                   // ByBlock
        if (isR2000()) {
            m_out.write(100, QString::fromLatin1(subclassMarker));
        }
    };

    for (const auto& seg : geo.segments) {
        beginBlockEntity("LINE");
        if (isR2000()) m_out.write(100, QStringLiteral("AcDbLine"));
        m_out.writePoint(10, seg.first);
        m_out.writePoint(11, seg.second);
    }

    for (const DimensionEntity::Arrow& arrow : geo.arrows) {
        beginBlockEntity("SOLID");
        if (isR2000()) m_out.write(100, QStringLiteral("AcDbTrace"));
        // SOLID 的顶点顺序有历史怪癖：第 3、4 个顶点相互对调。
        // 三角形以「重复第 3 点」的方式补成四点，顺序问题随之消解。
        m_out.writePoint(10, arrow.tip);
        m_out.writePoint(11, arrow.base1);
        m_out.writePoint(12, arrow.base2);
        m_out.writePoint(13, arrow.base2);
    }

    if (!geo.text.isEmpty()) {
        beginBlockEntity("TEXT");
        if (isR2000()) m_out.write(100, QStringLiteral("AcDbText"));
        m_out.write(1, dim.displayText(/*dxfStyle=*/true));
        m_out.writePoint(10, geo.textAnchor);
        m_out.write(40, geo.textHeight);
        m_out.write(50, radToDeg(geo.textAngle));
        m_out.write(72, 1);                   // 水平居中
        m_out.writePoint(11, geo.textAnchor);
        m_out.write(73, 2);                   // 垂直居中
    }

    // ---- 块尾 ----
    m_out.write(0, QStringLiteral("ENDBLK"));
    if (isR2000()) {
        m_out.write(5, nextHandle());
        m_out.write(330, info.recordHandle);
        m_out.write(100, QStringLiteral("AcDbEntity"));
        m_out.write(8, QStringLiteral("0"));
        m_out.write(100, QStringLiteral("AcDbBlockEnd"));
    }
}

// -----------------------------------------------------------------------------
//  ENTITIES
// -----------------------------------------------------------------------------
void DxfWriter::writeEntities()
{
    openSection("ENTITIES");

    m_entityCount = 0;
    for (EntityId id : m_document->drawOrder()) {
        const Entity* entity = m_document->entity(id);
        if (!entity) continue;
        writeEntity(*entity);
        ++m_entityCount;
    }

    closeSection();
}

void DxfWriter::writeCommonEntityFields(const Entity& entity, const char* subclassMarker)
{
    m_out.write(8, entity.layerName());

    if (const int color = entityColorIndex(entity); color >= 0) {
        m_out.write(62, color);
    }
    if (!entity.isLinetypeByLayer()) {
        m_out.write(6, linetypes::resolve(entity.linetypeName()).name);
    }

    // 60 组码：1 表示不可见。丢失该标志会让被隐藏的图元在下次打开时重现。
    if (!entity.isVisible()) {
        m_out.write(60, 1);
    }

    if (isR2000()) {
        m_out.write(370, entity.lineweightDxf());
        m_out.write(100, QString::fromLatin1(subclassMarker));
    }
}

int DxfWriter::entityColorIndex(const Entity& entity) const
{
    return entity.isColorByLayer() ? -1 : entity.colorIndex();
}

void DxfWriter::writeEntity(const Entity& entity)
{
    switch (entity.type()) {
    case EntityType::Line:
        writeLine(static_cast<const LineEntity&>(entity));
        break;
    case EntityType::Circle:
        writeCircle(static_cast<const CircleEntity&>(entity));
        break;
    case EntityType::Arc:
        writeArc(static_cast<const ArcEntity&>(entity));
        break;
    case EntityType::Polyline:
        writePolyline(static_cast<const PolylineEntity&>(entity));
        break;
    case EntityType::Point:
        writePoint(static_cast<const PointEntity&>(entity));
        break;
    case EntityType::Text:
        writeText(static_cast<const TextEntity&>(entity));
        break;
    case EntityType::Hatch:
        writeHatch(static_cast<const HatchEntity&>(entity));
        break;
    case EntityType::Dimension:
        writeDimension(static_cast<const DimensionEntity&>(entity));
        break;
    }
}

void DxfWriter::writeLine(const LineEntity& line)
{
    m_out.write(0, QStringLiteral("LINE"));
    if (isR2000()) {
        m_out.write(5, nextHandle());
        m_out.write(330, handleText(kHandleModelSpaceRecord));
        m_out.write(100, QStringLiteral("AcDbEntity"));
    }
    writeCommonEntityFields(line, "AcDbLine");
    m_out.writePoint(10, line.start());
    m_out.writePoint(11, line.end());
}

void DxfWriter::writeCircle(const CircleEntity& circle)
{
    m_out.write(0, QStringLiteral("CIRCLE"));
    if (isR2000()) {
        m_out.write(5, nextHandle());
        m_out.write(330, handleText(kHandleModelSpaceRecord));
        m_out.write(100, QStringLiteral("AcDbEntity"));
    }
    writeCommonEntityFields(circle, "AcDbCircle");
    m_out.writePoint(10, circle.center());
    m_out.write(40, circle.radius());
}

void DxfWriter::writeArc(const ArcEntity& arc)
{
    m_out.write(0, QStringLiteral("ARC"));
    if (isR2000()) {
        m_out.write(5, nextHandle());
        m_out.write(330, handleText(kHandleModelSpaceRecord));
        m_out.write(100, QStringLiteral("AcDbEntity"));
    }
    writeCommonEntityFields(arc, "AcDbCircle");
    m_out.writePoint(10, arc.center());
    m_out.write(40, arc.radius());
    if (isR2000()) m_out.write(100, QStringLiteral("AcDbArc"));
    // DXF 以度存储、逆时针为正，与内部弧度制约定仅差单位换算
    m_out.write(50, radToDeg(arc.startAngle()));
    m_out.write(51, radToDeg(arc.endAngle()));
}

void DxfWriter::writePolyline(const PolylineEntity& polyline)
{
    if (polyline.vertexCount() < 2) {
        // 顶点不足无法构成多段线：降级为点，避免产生非法实体
        m_out.write(0, QStringLiteral("POINT"));
        if (isR2000()) {
            m_out.write(5, nextHandle());
            m_out.write(330, handleText(kHandleModelSpaceRecord));
            m_out.write(100, QStringLiteral("AcDbEntity"));
        }
        writeCommonEntityFields(polyline, "AcDbPoint");
        if (polyline.vertexCount() == 1) {
            m_out.writePoint(10, polyline.vertex(0).point);
        } else {
            m_out.writePoint(10, Point2D(0.0, 0.0));
        }
        return;
    }

    if (isR2000()) {
        writePolylineR2000(polyline);
    } else {
        writePolylineR12(polyline);
    }
}

void DxfWriter::writePolylineR12(const PolylineEntity& polyline)
{
    const int flags = polyline.isClosed() ? 1 : 0;

    m_out.write(0, QStringLiteral("POLYLINE"));
    m_out.write(8, polyline.layerName());
    if (const int color = entityColorIndex(polyline); color >= 0) m_out.write(62, color);
    if (!polyline.isLinetypeByLayer()) {
        m_out.write(6, linetypes::resolve(polyline.linetypeName()).name);
    }
    if (!polyline.isVisible()) m_out.write(60, 1);
    m_out.write(66, 1);                       // 后续跟随 VERTEX 实体
    m_out.write(70, flags);
    if (polyline.constantWidth() > kEpsilon) {
        m_out.write(40, polyline.constantWidth());   // 默认起点宽度
        m_out.write(41, polyline.constantWidth());   // 默认终点宽度
    }
    m_out.writePoint(10, Point2D(0.0, 0.0));  // R12 要求给出一个占位坐标

    for (const PolylineEntity::Vertex& v : polyline.vertices()) {
        m_out.write(0, QStringLiteral("VERTEX"));
        m_out.write(8, polyline.layerName());
        m_out.writePoint(10, v.point);
        if (std::abs(v.bulge) > kEpsilon) m_out.write(42, v.bulge);
        if (polyline.constantWidth() <= kEpsilon) {
            if (v.startWidth > kEpsilon) m_out.write(40, v.startWidth);
            if (v.endWidth > kEpsilon) m_out.write(41, v.endWidth);
        }
        m_out.write(70, 0);
    }

    m_out.write(0, QStringLiteral("SEQEND"));
    m_out.write(8, polyline.layerName());
}

void DxfWriter::writePolylineR2000(const PolylineEntity& polyline)
{
    m_out.write(0, QStringLiteral("LWPOLYLINE"));
    m_out.write(5, nextHandle());
    m_out.write(330, handleText(kHandleModelSpaceRecord));
    m_out.write(100, QStringLiteral("AcDbEntity"));
    writeCommonEntityFields(polyline, "AcDbPolyline");

    m_out.write(90, static_cast<int>(polyline.vertexCount()));
    m_out.write(70, polyline.isClosed() ? 1 : 0);
    if (polyline.constantWidth() > kEpsilon) {
        m_out.write(43, polyline.constantWidth());   // 恒定宽度
    }

    for (const PolylineEntity::Vertex& v : polyline.vertices()) {
        m_out.write(10, v.point.x);
        m_out.write(20, v.point.y);
        if (std::abs(v.bulge) > kEpsilon) m_out.write(42, v.bulge);
        // 恒定宽度优先：设置了 43 后逐顶点宽度不再有意义
        if (polyline.constantWidth() <= kEpsilon) {
            if (v.startWidth > kEpsilon) m_out.write(40, v.startWidth);
            if (v.endWidth > kEpsilon) m_out.write(41, v.endWidth);
        }
    }
}

void DxfWriter::writePoint(const PointEntity& point)
{
    m_out.write(0, QStringLiteral("POINT"));
    if (isR2000()) {
        m_out.write(5, nextHandle());
        m_out.write(330, handleText(kHandleModelSpaceRecord));
        m_out.write(100, QStringLiteral("AcDbEntity"));
    }
    writeCommonEntityFields(point, "AcDbPoint");
    m_out.writePoint(10, point.position());
}

void DxfWriter::writeText(const TextEntity& text)
{
    if (text.displayString().isEmpty()) {
        // 空文字不是合法的 DXF 实体，跳过而非产出坏文件
        return;
    }

    m_out.write(0, QStringLiteral("TEXT"));
    if (isR2000()) {
        m_out.write(5, nextHandle());
        m_out.write(330, handleText(kHandleModelSpaceRecord));
        m_out.write(100, QStringLiteral("AcDbEntity"));
    }
    writeCommonEntityFields(text, "AcDbText");

    m_out.write(1, text.displayString());
    m_out.writePoint(10, text.position());
    m_out.write(40, text.height());
    m_out.write(50, radToDeg(text.rotation()));

    if (text.usesAlignmentPoint()) {
        // 非左/基线对齐时，DXF 以 72/73 + 第二对齐点（11/21）定位
        m_out.write(72, static_cast<int>(text.hAlign()));
        m_out.writePoint(11, text.alignmentPoint());
        m_out.write(73, static_cast<int>(text.vAlign()));
    }

    // 71 生成标志：bit2 = 水平镜像（背面文字），bit4 = 垂直镜像
    int generationFlags = 0;
    if (text.isBackwards()) generationFlags |= 2;
    if (text.isUpsideDown()) generationFlags |= 4;
    if (generationFlags != 0) m_out.write(71, generationFlags);

    // 41 宽度因子
    if (std::abs(text.widthFactor() - 1.0) > kEpsilon) {
        m_out.write(41, text.widthFactor());
    }
}

void DxfWriter::writeHatch(const HatchEntity& hatch)
{
    // HATCH 实体为 R14 引入，R12 没有对应的表达方式；
    // 降级写出边界多段线以保证几何信息不丢失
    if (!isR2000()) {
        if (hatch.vertexCount() >= 2) {
            PolylineEntity boundary(hatch.boundary(), true);
            boundary.setLayerName(hatch.layerName());
            boundary.setColorIndex(hatch.colorIndex());
            boundary.setLineweightDxf(hatch.lineweightDxf());
            boundary.setVisible(hatch.isVisible());
            writePolylineR12(boundary);
        }
        m_hatchDowngrades += 1;
        return;
    }

    if (hatch.vertexCount() < 3) return;      // 无法构成闭合边界

    m_out.write(0, QStringLiteral("HATCH"));
    m_out.write(5, nextHandle());
    m_out.write(330, handleText(kHandleModelSpaceRecord));
    m_out.write(100, QStringLiteral("AcDbEntity"));
    writeCommonEntityFields(hatch, "AcDbHatch");

    // 定义点（标高）
    m_out.writePoint(10, Point2D(0.0, 0.0));
    m_out.write(210, 0.0);
    m_out.write(220, 0.0);
    m_out.write(230, 1.0);

    m_out.write(2, hatch.patternName().isEmpty() ? QStringLiteral("SOLID")
                                                 : hatch.patternName());
    m_out.write(70, hatch.isSolid() ? 1 : 0);
    m_out.write(71, 0);                       // 非关联

    // ---- 边界路径：单个外部多段线环（可含凸度） ----
    const bool boundaryHasBulge = hatch.bulges().empty()
        ? false
        : std::any_of(hatch.bulges().begin(), hatch.bulges().end(),
                      [](double b) { return std::abs(b) > kEpsilon; });

    m_out.write(91, 1);                       // 环数
    m_out.write(92, 5);                       // bit0 = 多段线路径, bit2 = 外部环
    m_out.write(72, boundaryHasBulge ? 1 : 0);
    m_out.write(73, 1);                       // 闭合
    m_out.write(93, static_cast<int>(hatch.vertexCount()));
    for (std::size_t i = 0; i < hatch.vertexCount(); ++i) {
        const Point2D& p = hatch.vertex(i);
        m_out.write(10, p.x);
        m_out.write(20, p.y);
        if (boundaryHasBulge && std::abs(hatch.vertexBulge(i)) > kEpsilon) {
            m_out.write(42, hatch.vertexBulge(i));
        }
    }
    m_out.write(97, 0);                       // 种子点数

    // ---- 图案定义 ----
    if (!hatch.isSolid()) {
        m_out.write(75, 0);                   // 边界样式
        m_out.write(76, 1);                   // 图案类型：预定义
        m_out.write(52, radToDeg(hatch.patternAngle()));
        m_out.write(41, hatch.patternScale());

        // 一条图案定义线：方向 45°（ANSI31 基准），间距随比例缩放
        const double spacing = hatch.patternSpacing();
        const double theta = HatchEntity::kAnsi31BaseAngle;
        m_out.write(78, 1);                   // 图案定义线数
        m_out.write(53, radToDeg(theta));     // 该线与 X 轴夹角
        m_out.write(43, 0.0);
        m_out.write(44, 0.0);
        m_out.write(45, -spacing * std::sin(theta));   // 相邻线偏移向量
        m_out.write(46,  spacing * std::cos(theta));
        m_out.write(79, 0);                   // 无虚线段（连续实线）
    }
}

void DxfWriter::writeDimension(const DimensionEntity& dim)
{
    // 匿名块名在 prescanDimensionBlocks 中按文档顺序分配
    const QString blockName = [this, id = dim.id()]() -> QString {
        for (const DimensionBlockInfo& info : m_dimBlocks) {
            if (info.dim->id() == id) return info.name;
        }
        return QString();
    }();

    m_out.write(0, QStringLiteral("DIMENSION"));
    if (isR2000()) {
        m_out.write(5, nextHandle());
        m_out.write(330, handleText(kHandleModelSpaceRecord));
        m_out.write(100, QStringLiteral("AcDbEntity"));
    }
    writeCommonEntityFields(dim, "AcDbDimension");

    const DimensionEntity::Geometry geo = dim.geometry();

    // ---- AcDbDimension 子类的公共数据 ----
    if (!blockName.isEmpty()) m_out.write(2, blockName);

    switch (dim.dimType()) {
    case DimensionEntity::DimType::Linear:
    case DimensionEntity::DimType::Aligned:
        m_out.writePoint(10, dim.dimLinePoint());       // 尺寸线位置
        break;
    case DimensionEntity::DimType::Radial:
        m_out.writePoint(10, dim.firstPoint());         // 圆心
        break;
    case DimensionEntity::DimType::Diametric:
        m_out.writePoint(10, dim.firstPoint());         // 圆上点之一
        break;
    }
    m_out.writePoint(11, geo.textAnchor);               // 文字中点

    // bit 32 表示「几何块引用已在文件中」，读取端无需自行重绘
    m_out.write(70, static_cast<int>(dim.dimType()) | 32);

    if (!dim.textOverride().isEmpty() && dim.textOverride() != QLatin1String("<>")) {
        m_out.write(1, dim.textOverride());
    }
    m_out.write(3, QStringLiteral("Standard"));         // 标注样式名
    m_out.write(42, dim.measurement());                 // 实测值（只读字段）

    // ---- 类型专属子类数据 ----
    if (isR2000()) {
        if (dim.dimType() == DimensionEntity::DimType::Radial) {
            m_out.write(100, QStringLiteral("AcDbRadialDimension"));
            m_out.writePoint(15, dim.secondPoint());    // 圆上定义点
        } else if (dim.dimType() == DimensionEntity::DimType::Diametric) {
            m_out.write(100, QStringLiteral("AcDbDiametricDimension"));
            m_out.writePoint(15, dim.secondPoint());    // 对径点
        } else {
            m_out.write(100, QStringLiteral("AcDbAlignedDimension"));
            m_out.writePoint(13, dim.firstPoint());     // 第一界线原点
            m_out.writePoint(14, dim.secondPoint());    // 第二界线原点
            if (dim.dimType() == DimensionEntity::DimType::Linear) {
                m_out.write(100, QStringLiteral("AcDbRotatedDimension"));
                m_out.write(50, radToDeg(dim.rotation()));
            }
        }
    } else {
        // R12 无子类标记，组码顺序不敏感；按常见惯例紧随其后写出
        switch (dim.dimType()) {
        case DimensionEntity::DimType::Linear:
        case DimensionEntity::DimType::Aligned:
            m_out.writePoint(13, dim.firstPoint());
            m_out.writePoint(14, dim.secondPoint());
            if (dim.dimType() == DimensionEntity::DimType::Linear) {
                m_out.write(50, radToDeg(dim.rotation()));
            }
            break;
        case DimensionEntity::DimType::Radial:
        case DimensionEntity::DimType::Diametric:
            m_out.writePoint(15, dim.secondPoint());
            break;
        }
    }
}

// -----------------------------------------------------------------------------
//  OBJECTS（仅 R2000）
// -----------------------------------------------------------------------------
void DxfWriter::writeObjects()
{
    openSection("OBJECTS");

    // 根命名对象字典
    m_out.write(0, QStringLiteral("DICTIONARY"));
    m_out.write(5, handleText(kHandleRootDictionary));
    m_out.write(330, QStringLiteral("0"));
    m_out.write(100, QStringLiteral("AcDbDictionary"));
    m_out.write(281, 1);
    m_out.write(3, QStringLiteral("ACAD_GROUP"));
    m_out.write(350, handleText(kHandleGroupDictionary));
    m_out.write(3, QStringLiteral("ACAD_MLINESTYLE"));
    m_out.write(350, handleText(kHandleMlineStyleDict));
    m_out.write(3, QStringLiteral("ACAD_PLOTSTYLENAME"));
    m_out.write(350, handleText(kHandlePlotStyleDict));

    // 编组字典（空）
    m_out.write(0, QStringLiteral("DICTIONARY"));
    m_out.write(5, handleText(kHandleGroupDictionary));
    m_out.write(330, handleText(kHandleRootDictionary));
    m_out.write(100, QStringLiteral("AcDbDictionary"));
    m_out.write(281, 1);

    // 多线样式字典
    m_out.write(0, QStringLiteral("DICTIONARY"));
    m_out.write(5, handleText(kHandleMlineStyleDict));
    m_out.write(330, handleText(kHandleRootDictionary));
    m_out.write(100, QStringLiteral("AcDbDictionary"));
    m_out.write(281, 1);
    m_out.write(3, QStringLiteral("Standard"));
    m_out.write(350, handleText(kHandleStandardMline));

    m_out.write(0, QStringLiteral("MLINESTYLE"));
    m_out.write(5, handleText(kHandleStandardMline));
    m_out.write(330, handleText(kHandleMlineStyleDict));
    m_out.write(100, QStringLiteral("AcDbMlineStyle"));
    m_out.write(2, QStringLiteral("Standard"));
    m_out.write(70, 0);
    m_out.write(3, QString());
    m_out.write(62, 256);
    m_out.write(51, 90.0);
    m_out.write(52, 90.0);
    m_out.write(71, 2);
    m_out.write(49, 0.5);
    m_out.write(62, 256);
    m_out.write(6, QStringLiteral("BYLAYER"));
    m_out.write(49, -0.5);
    m_out.write(62, 256);
    m_out.write(6, QStringLiteral("BYLAYER"));

    // 打印样式字典 + 占位对象（图层 390 组码指向此处）
    m_out.write(0, QStringLiteral("DICTIONARY"));
    m_out.write(5, handleText(kHandlePlotStyleDict));
    m_out.write(330, handleText(kHandleRootDictionary));
    m_out.write(100, QStringLiteral("AcDbDictionary"));
    m_out.write(281, 1);
    m_out.write(3, QStringLiteral("Normal"));
    m_out.write(350, handleText(kHandlePlotPlaceholder));

    m_out.write(0, QStringLiteral("ACDBPLACEHOLDER"));
    m_out.write(5, handleText(kHandlePlotPlaceholder));
    m_out.write(330, handleText(kHandlePlotStyleDict));
    m_out.write(100, QStringLiteral("AcDbPlaceHolder"));

    closeSection();
}

// -----------------------------------------------------------------------------
//  辅助
// -----------------------------------------------------------------------------
Box2D DxfWriter::computeExtents() const
{
    Box2D box;
    for (EntityId id : m_document->drawOrder()) {
        if (const Entity* e = m_document->entity(id)) {
            if (!e->isDisplayable(m_document->layers())) continue;
            box.expand(e->boundingBox());
        }
    }
    return box;
}

} // namespace lycad
