// =============================================================================
//  LyCAD - io/DxfWriter.cpp
// =============================================================================
#include "io/DxfWriter.h"

#include "core/ArcEntity.h"
#include "core/CircleEntity.h"
#include "core/Document.h"
#include "core/LineEntity.h"
#include "core/Linetype.h"
#include "core/PointEntity.h"
#include "core/PolylineEntity.h"

#include <QIODevice>
#include <QSet>

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

    m_out.writeComment(QStringLiteral("LyCAD 生成的 DXF 文件 (目标版本 %1)").arg(dxfVersion()));

    writeHeader();
    writeTables();
    if (isR2000()) writeBlocks();
    writeEntities();
    if (isR2000()) writeObjects();

    m_out.write(0, QStringLiteral("EOF"));

    if (!m_out.flush()) {
        return IoResult::failure(QStringLiteral("写入文件时出错：%1").arg(m_device->errorString()));
    }

    IoResult result = IoResult::success(document.entityCount());
    result.addWarning(QStringLiteral("目标版本 %1").arg(dxfVersion()));
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
    m_out.write(70, 2);

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

    m_out.write(0, QStringLiteral("ENDTAB"));
}

void DxfWriter::writeBlocks()
{
    openSection("BLOCKS");

    // *Model_Space 块定义：R2000 要求存在，实体本身写在 ENTITIES 区
    m_out.write(0, QStringLiteral("BLOCK"));
    m_out.write(5, nextHandle());
    m_out.write(330, handleText(kHandleModelSpaceRecord));
    m_out.write(100, QStringLiteral("AcDbEntity"));
    m_out.write(8, QStringLiteral("0"));
    m_out.write(100, QStringLiteral("AcDbBlockBegin"));
    m_out.write(2, QStringLiteral("*Model_Space"));
    m_out.write(70, 0);
    m_out.writePoint(10, Point2D(0.0, 0.0));
    m_out.write(3, QStringLiteral("*Model_Space"));
    m_out.write(1, QString());
    m_out.write(0, QStringLiteral("ENDBLK"));
    m_out.write(5, nextHandle());
    m_out.write(330, handleText(kHandleModelSpaceRecord));
    m_out.write(100, QStringLiteral("AcDbEntity"));
    m_out.write(8, QStringLiteral("0"));
    m_out.write(100, QStringLiteral("AcDbBlockEnd"));

    m_out.write(0, QStringLiteral("BLOCK"));
    m_out.write(5, nextHandle());
    m_out.write(330, m_paperSpaceRecordHandle);
    m_out.write(100, QStringLiteral("AcDbEntity"));
    m_out.write(8, QStringLiteral("0"));
    m_out.write(100, QStringLiteral("AcDbBlockBegin"));
    m_out.write(2, QStringLiteral("*Paper_Space"));
    m_out.write(70, 0);
    m_out.writePoint(10, Point2D(0.0, 0.0));
    m_out.write(3, QStringLiteral("*Paper_Space"));
    m_out.write(1, QString());
    m_out.write(0, QStringLiteral("ENDBLK"));
    m_out.write(5, nextHandle());
    m_out.write(330, m_paperSpaceRecordHandle);
    m_out.write(100, QStringLiteral("AcDbEntity"));
    m_out.write(8, QStringLiteral("0"));
    m_out.write(100, QStringLiteral("AcDbBlockEnd"));

    closeSection();
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
    m_out.writePoint(10, Point2D(0.0, 0.0));  // R12 要求给出一个占位坐标

    for (const PolylineEntity::Vertex& v : polyline.vertices()) {
        m_out.write(0, QStringLiteral("VERTEX"));
        m_out.write(8, polyline.layerName());
        m_out.writePoint(10, v.point);
        if (std::abs(v.bulge) > kEpsilon) m_out.write(42, v.bulge);
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
    m_out.write(43, 0.0);                     // 恒定宽度

    for (const PolylineEntity::Vertex& v : polyline.vertices()) {
        m_out.write(10, v.point.x);
        m_out.write(20, v.point.y);
        if (std::abs(v.bulge) > kEpsilon) m_out.write(42, v.bulge);
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
