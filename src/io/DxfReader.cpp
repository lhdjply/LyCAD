// =============================================================================
//  LyCAD - io/DxfReader.cpp
// =============================================================================
#include "io/DxfReader.h"

#include "core/ArcEntity.h"
#include "core/CircleEntity.h"
#include "core/Document.h"
#include "core/LineEntity.h"
#include "core/PointEntity.h"
#include "core/PolylineEntity.h"

#include <QFile>

#include <algorithm>

namespace lycad {

namespace {

/// 块引用递归展开的深度上限，防止自引用块造成无限递归
constexpr int kMaxBlockDepth = 16;

bool findGroup(const std::vector<DxfGroup>& groups, int code, DxfGroup& out)
{
    bool found = false;
    for (const DxfGroup& g : groups) {
        if (g.code == code) {
            out = g;
            found = true;              // 同码重复时取最后一个
        }
    }
    return found;
}

bool hasGroup(const std::vector<DxfGroup>& groups, int code)
{
    return std::any_of(groups.begin(), groups.end(),
                       [code](const DxfGroup& g) { return g.code == code; });
}

double doubleOf(const std::vector<DxfGroup>& groups, int code, double fallback = 0.0)
{
    DxfGroup g;
    if (findGroup(groups, code, g)) return g.real;
    return fallback;
}

int intOf(const std::vector<DxfGroup>& groups, int code, int fallback = 0)
{
    DxfGroup g;
    if (findGroup(groups, code, g)) return g.asInt();
    return fallback;
}

QString stringOf(const std::vector<DxfGroup>& groups, int code,
                 const QString& fallback = QString())
{
    DxfGroup g;
    if (findGroup(groups, code, g)) return g.text;
    return fallback;
}

} // namespace

// -----------------------------------------------------------------------------
//  构造
// -----------------------------------------------------------------------------
DxfReader::DxfReader() = default;

// -----------------------------------------------------------------------------
//  主入口
// -----------------------------------------------------------------------------
IoResult DxfReader::read(const QString& path, Document& document)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return IoResult::failure(QStringLiteral("无法打开文件：%1").arg(file.errorString()));
    }
    if (file.size() == 0) {
        return IoResult::failure(QStringLiteral("文件为空。"));
    }

    m_reader = std::make_unique<DxfGroupReader>(&file);

    QString openError;
    if (!m_reader->open(&openError)) {
        return IoResult::failure(openError.isEmpty()
                                     ? QStringLiteral("无法解析该文件的内容结构。")
                                     : openError);
    }

    m_binary = m_reader->isBinary();
    m_blocks.clear();
    m_blockIndex.clear();
    m_skippedTypes.clear();
    m_explodedInserts = 0;
    m_insertDepthViolations = 0;
    m_acadVersion.clear();
    m_insUnits = 0;
    m_headerCurrentLayer.clear();

    IoResult result;
    if (!parseDocument(document, &result)) {
        return IoResult::failure(result.error.isEmpty()
                                     ? QStringLiteral("DXF 解析失败。")
                                     : result.error);
    }

    collectWarnings(&result);
    result.ok = true;
    result.entityCount = static_cast<int>(document.entityCount());
    return result;
}

// -----------------------------------------------------------------------------
//  顶层区段循环
// -----------------------------------------------------------------------------
bool DxfReader::parseDocument(Document& document, IoResult* result)
{
    document.beginBatchLoad();
    document.layers().clear();

    bool sawEntities = false;
    bool sawEof = false;

    DxfGroup g;
    while (m_reader->next(g)) {
        if (g.code == 0 && g.text == QLatin1String("EOF")) {
            sawEof = true;
            break;
        }
        if (g.code != 0 || g.text != QLatin1String("SECTION")) continue;

        DxfGroup nameGroup;
        if (!m_reader->next(nameGroup)) break;
        if (nameGroup.code != 2) {
            if (!skipSection(result)) break;
            continue;
        }

        const QString section = nameGroup.text.toUpper();
        bool ok = true;
        if (section == QLatin1String("HEADER")) {
            ok = readHeaderSection(result);
        } else if (section == QLatin1String("TABLES")) {
            ok = readTablesSection(document, result);
        } else if (section == QLatin1String("BLOCKS")) {
            ok = readBlocksSection(result);
        } else if (section == QLatin1String("ENTITIES")) {
            sawEntities = true;
            ok = readEntitiesSection(document, result);
        } else {
            ok = skipSection(result);
        }
        if (!ok) break;
    }

    document.endBatchLoad();

    if (!sawEntities && !sawEof) {
        if (result) {
            result->error = QStringLiteral("文件中既无 ENTITIES 区段也无 EOF 标记，不是有效的 DXF 图纸。");
        }
        return false;
    }
    return true;
}

bool DxfReader::skipSection(IoResult* result)
{
    Q_UNUSED(result);
    DxfGroup g;
    while (m_reader->next(g)) {
        if (g.code == 0 && g.text == QLatin1String("ENDSEC")) return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
//  HEADER
// -----------------------------------------------------------------------------
bool DxfReader::readHeaderSection(IoResult* result)
{
    Q_UNUSED(result);

    DxfGroup g;
    while (m_reader->next(g)) {
        if (g.code == 0 && g.text == QLatin1String("ENDSEC")) return true;
        if (g.code != 9) continue;                  // 只处理「变量名 + 值」结构中的名字

        const QString name = g.text;
        DxfGroup value;
        if (!m_reader->next(value)) return true;

        if (value.code == 0) {
            if (value.text == QLatin1String("ENDSEC")) return true;
            // 该变量没有值组，把控制组交还主循环
            m_reader->pushBack(value);
            continue;
        }

        if (name == QLatin1String("$ACADVER")) {
            m_acadVersion = value.text.trimmed();
        } else if (name == QLatin1String("$INSUNITS")) {
            m_insUnits = value.asInt();
        } else if (name == QLatin1String("$CLAYER")) {
            m_headerCurrentLayer = value.text;
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
//  TABLES
// -----------------------------------------------------------------------------
bool DxfReader::readTablesSection(Document& document, IoResult* result)
{
    DxfGroup g;
    QString currentTable;

    while (m_reader->next(g)) {
        if (g.code == 0 && g.text == QLatin1String("ENDSEC")) return true;

        if (g.code == 0 && g.text == QLatin1String("TABLE")) {
            DxfGroup nameGroup;
            if (m_reader->next(nameGroup) && nameGroup.code == 2) {
                currentTable = nameGroup.text.toUpper();
            } else {
                m_reader->pushBack(nameGroup);
                currentTable.clear();
            }
            continue;
        }

        if (g.code == 0 && g.text == QLatin1String("ENDTAB")) {
            currentTable.clear();
            continue;
        }

        if (currentTable == QLatin1String("LAYER")
            && g.code == 0 && g.text == QLatin1String("LAYER")) {
            if (!applyLayerRecord(document, collectUntilNextRecord(), result)) return false;
            continue;
        }
        // 其余符号表（LTYPE/STYLE/VIEW/UCS/VPORT/DIMSTYLE/APPID/BLOCK_RECORD）
        // 不参与几何模型，交由下一轮循环自然跳过
    }
    return true;
}

std::vector<DxfGroup> DxfReader::collectUntilNextRecord()
{
    std::vector<DxfGroup> groups;
    DxfGroup g;
    while (m_reader->next(g)) {
        if (g.code == 0) {
            m_reader->pushBack(g);      // 控制组交还上层状态机
            break;
        }
        groups.push_back(g);
    }
    return groups;
}

bool DxfReader::applyLayerRecord(Document& document, const std::vector<DxfGroup>& groups,
                                 IoResult* result)
{
    const QString name = stringOf(groups, 2);
    if (name.isEmpty()) {
        if (result) result->addWarning(QStringLiteral("跳过了一个缺少名称的 LAYER 记录。"));
        return true;
    }
    if (document.layers().contains(name)) {
        return true;                    // 重复定义以首次为准
    }

    Layer layer(name);
    layer.setFlagsFromDxf(intOf(groups, 70, 0));

    // 图层关闭在 DXF 中以颜色取负表达
    const int rawColor = intOf(groups, 62, 7);
    layer.setVisible(rawColor >= 0);
    const int absColor = std::abs(rawColor);
    layer.setColorIndex((absColor >= 1 && absColor <= 255) ? absColor : 7);

    QString linetype = stringOf(groups, 6, QStringLiteral("CONTINUOUS")).trimmed();
    if (linetype.isEmpty()) linetype = QStringLiteral("CONTINUOUS");
    layer.setLinetype(linetype);

    layer.setLineweightDxf(intOf(groups, 370, -3));

    document.layers().ensure(name) = layer;
    return true;
}

// -----------------------------------------------------------------------------
//  BLOCKS
// -----------------------------------------------------------------------------
bool DxfReader::readBlocksSection(IoResult* result)
{
    Q_UNUSED(result);

    DxfGroup g;
    while (m_reader->next(g)) {
        if (g.code == 0 && g.text == QLatin1String("ENDSEC")) return true;
        if (g.code != 0 || g.text != QLatin1String("BLOCK")) continue;

        BlockDefinition block;
        DxfGroup header;
        bool haveName = false;
        while (m_reader->next(header)) {
            if (header.code == 0) {
                m_reader->pushBack(header);
                break;
            }
            if (header.code == 2 && !haveName) {
                block.name = header.text;
                haveName = true;
            }
            if (header.code == 10) block.basePoint.x = header.real;
            if (header.code == 20) block.basePoint.y = header.real;
        }

        if (block.name.isEmpty()) {
            skipUntilBlockEnd();        // 无名块无法被引用，仅需保持流同步
            continue;
        }

        if (!readBlockBody(block, result)) return false;

        if (m_blockIndex.contains(block.name)) {
            continue;                   // 重名块保留首次定义
        }
        m_blockIndex.insert(block.name, m_blocks.size());
        m_blocks.push_back(std::move(block));
    }
    return true;
}

void DxfReader::skipUntilBlockEnd()
{
    DxfGroup g;
    while (m_reader->next(g)) {
        if (g.code == 0 && g.text == QLatin1String("ENDBLK")) return;
    }
}

bool DxfReader::readBlockBody(BlockDefinition& block, IoResult* result)
{
    DxfGroup g;
    while (m_reader->next(g)) {
        if (g.code != 0) continue;
        if (g.text == QLatin1String("ENDBLK")) return true;

        if (g.text == QLatin1String("POLYLINE")) {
            for (auto& e : readLegacyPolyline(result)) {
                block.entities.push_back(std::move(e));
            }
            continue;
        }

        if (g.text == QLatin1String("INSERT")) {
            // 块内引用不立即展开：此处仅记录参数，待全部块就位后在插入点统一处理
            const std::vector<DxfGroup> groups = collectUntilNextRecord();

            BlockInsertRef ref;
            ref.targetBlock = stringOf(groups, 2);
            ref.insertPoint = Point2D(doubleOf(groups, 10), doubleOf(groups, 20));
            ref.scaleX      = doubleOf(groups, 41, 1.0);
            ref.scaleY      = doubleOf(groups, 42, 1.0);
            ref.rotation    = degToRad(doubleOf(groups, 50, 0.0));
            ref.layer       = stringOf(groups, 8, QStringLiteral("0"));

            if (!ref.targetBlock.isEmpty()) {
                block.nestedInserts.push_back(std::move(ref));
            }
            continue;
        }

        if (g.text == QLatin1String("ATTDEF") || g.text == QLatin1String("ATTRIB")
            || g.text == QLatin1String("SEQEND")) {
            collectUntilNextRecord();
            continue;
        }

        for (auto& e : readSimpleEntity(g.text, result)) {
            block.entities.push_back(std::move(e));
        }
    }
    return false;                       // 文件在块定义中途结束
}

const DxfReader::BlockDefinition* DxfReader::findBlock(const QString& name) const
{
    const auto it = m_blockIndex.constFind(name);
    if (it == m_blockIndex.constEnd()) return nullptr;
    return &m_blocks[it.value()];
}

// -----------------------------------------------------------------------------
//  ENTITIES
// -----------------------------------------------------------------------------
bool DxfReader::readEntitiesSection(Document& document, IoResult* result)
{
    DxfGroup g;
    while (m_reader->next(g)) {
        if (g.code != 0) continue;
        if (g.text == QLatin1String("ENDSEC")) return true;

        std::vector<std::unique_ptr<Entity>> produced;

        if (g.text == QLatin1String("POLYLINE")) {
            produced = readLegacyPolyline(result);
        } else if (g.text == QLatin1String("INSERT")) {
            produced = readInsert(result);
        } else if (g.text == QLatin1String("SEQEND")) {
            collectUntilNextRecord();
            continue;
        } else {
            produced = readSimpleEntity(g.text, result);
        }

        for (auto& entity : produced) {
            if (!entity) continue;
            // 容错：图元可能引用了 LAYER 表中不存在的图层
            document.layers().ensure(entity->layerName());
            document.addEntity(std::move(entity));
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
//  图元解析
// -----------------------------------------------------------------------------
void DxfReader::applyCommonProperties(const std::vector<DxfGroup>& groups, Entity& entity)
{
    const QString layerName = stringOf(groups, 8, QStringLiteral("0")).trimmed();
    entity.setLayerName(layerName.isEmpty() ? QStringLiteral("0") : layerName);

    // 62：256 = 随层，0 = 随块，1-255 = 具体索引
    if (hasGroup(groups, 62)) {
        const int color = intOf(groups, 62, 256);
        if (color >= 1 && color <= 255) {
            entity.setColorIndex(color);
        } else {
            entity.setColorByLayer();
        }
    } else {
        entity.setColorByLayer();
    }

    const QString linetype = stringOf(groups, 6).trimmed();
    if (!linetype.isEmpty()
        && linetype.compare(QLatin1String("ByLayer"), Qt::CaseInsensitive) != 0
        && linetype.compare(QLatin1String("ByBlock"), Qt::CaseInsensitive) != 0) {
        entity.setLinetypeName(linetype);
    }

    if (hasGroup(groups, 370)) {
        entity.setLineweightDxf(intOf(groups, 370, -1));
    }

    // 60：1 表示不可见
    entity.setVisible(intOf(groups, 60, 0) == 0);
}

std::vector<std::unique_ptr<Entity>> DxfReader::readSimpleEntity(const QString& type,
                                                                 IoResult* result)
{
    return buildEntities(type, collectUntilNextRecord(), result);
}

std::vector<std::unique_ptr<Entity>> DxfReader::buildEntities(const QString& type,
                                                              const std::vector<DxfGroup>& groups,
                                                              IoResult* result)
{
    std::vector<std::unique_ptr<Entity>> out;
    const QString upper = type.toUpper();

    if (upper == QLatin1String("LINE")) {
        auto e = std::make_unique<LineEntity>(Point2D(doubleOf(groups, 10), doubleOf(groups, 20)),
                                              Point2D(doubleOf(groups, 11), doubleOf(groups, 21)));
        applyCommonProperties(groups, *e);
        out.push_back(std::move(e));
        return out;
    }

    if (upper == QLatin1String("CIRCLE")) {
        const double radius = doubleOf(groups, 40);
        if (radius <= kGeomTol) {
            m_skippedTypes[QStringLiteral("CIRCLE(半径为零)")] =
                m_skippedTypes.value(QStringLiteral("CIRCLE(半径为零)"), 0) + 1;
            return out;
        }
        auto e = std::make_unique<CircleEntity>(Point2D(doubleOf(groups, 10), doubleOf(groups, 20)),
                                                radius);
        applyCommonProperties(groups, *e);
        out.push_back(std::move(e));
        return out;
    }

    if (upper == QLatin1String("ARC")) {
        const double radius = doubleOf(groups, 40);
        if (radius <= kGeomTol) {
            m_skippedTypes[QStringLiteral("ARC(半径为零)")] =
                m_skippedTypes.value(QStringLiteral("ARC(半径为零)"), 0) + 1;
            return out;
        }
        // DXF 的 50/51 为「度」且逆时针为正，与内部弧度制约定方向一致，仅需换算单位
        auto e = std::make_unique<ArcEntity>(Point2D(doubleOf(groups, 10), doubleOf(groups, 20)),
                                             radius,
                                             degToRad(doubleOf(groups, 50)),
                                             degToRad(doubleOf(groups, 51)));
        applyCommonProperties(groups, *e);
        out.push_back(std::move(e));
        return out;
    }

    if (upper == QLatin1String("LWPOLYLINE")) {
        return parseLwPolyline(groups, result);
    }

    if (upper == QLatin1String("POINT")) {
        auto e = std::make_unique<PointEntity>(Point2D(doubleOf(groups, 10), doubleOf(groups, 20)));
        applyCommonProperties(groups, *e);
        out.push_back(std::move(e));
        return out;
    }

    if (upper == QLatin1String("SOLID") || upper == QLatin1String("TRACE")) {
        // 填充实体不做面填充，退化为轮廓闭合多段线，保证几何信息不丢失
        const Point2D p0(doubleOf(groups, 10), doubleOf(groups, 20));
        const Point2D p1(doubleOf(groups, 11), doubleOf(groups, 21));
        const Point2D p2(doubleOf(groups, 12), doubleOf(groups, 22));
        const Point2D p3(doubleOf(groups, 13), doubleOf(groups, 23));

        auto polyline = std::make_unique<PolylineEntity>(
            std::vector<Point2D>{ p0, p1, p3, p2 }, true);
        applyCommonProperties(groups, *polyline);
        out.push_back(std::move(polyline));

        const QString key = QStringLiteral("%1(退化为多段线)").arg(upper);
        m_skippedTypes[key] = m_skippedTypes.value(key, 0) + 1;
        return out;
    }

    m_skippedTypes[upper] = m_skippedTypes.value(upper, 0) + 1;
    return out;
}

std::vector<std::unique_ptr<Entity>> DxfReader::parseLwPolyline(const std::vector<DxfGroup>& groups,
                                                                IoResult* result)
{
    Q_UNUSED(result);

    std::vector<std::unique_ptr<Entity>> out;

    std::vector<PolylineEntity::Vertex> vertices;
    vertices.reserve(16);

    double x = 0.0;
    bool haveX = false;

    for (const DxfGroup& g : groups) {
        switch (g.code) {
        case 10:
            x = g.real;
            haveX = true;
            break;
        case 20:
            if (!haveX) break;
            vertices.push_back(PolylineEntity::Vertex{ Point2D(x, g.real), 0.0 });
            haveX = false;
            break;
        case 42:
            // 凸度归属于前一个顶点（即上一段的起点）
            if (!vertices.empty()) vertices.back().bulge = g.real;
            break;
        default:
            break;
        }
    }

    if (vertices.size() >= 2) {
        const bool closed = (intOf(groups, 70, 0) & 1) != 0;
        auto polyline = std::make_unique<PolylineEntity>(std::move(vertices), closed);
        applyCommonProperties(groups, *polyline);
        out.push_back(std::move(polyline));
        return out;
    }

    if (vertices.size() == 1) {
        // 单顶点多段线在几何上等价于点
        auto point = std::make_unique<PointEntity>(vertices.front().point);
        applyCommonProperties(groups, *point);
        out.push_back(std::move(point));
        m_skippedTypes[QStringLiteral("LWPOLYLINE(单顶点转为 POINT)")] =
            m_skippedTypes.value(QStringLiteral("LWPOLYLINE(单顶点转为 POINT)"), 0) + 1;
        return out;
    }

    m_skippedTypes[QStringLiteral("LWPOLYLINE(无有效顶点)")] =
        m_skippedTypes.value(QStringLiteral("LWPOLYLINE(无有效顶点)"), 0) + 1;
    return out;
}

std::vector<std::unique_ptr<Entity>> DxfReader::readLegacyPolyline(IoResult* result)
{
    std::vector<std::unique_ptr<Entity>> out;

    const std::vector<DxfGroup> header = collectUntilNextRecord();
    const int flags = intOf(header, 70, 0);

    // bit 16 = 三维多边形网格，bit 64 = 多面网格：顶点语义与二维多段线不同
    if ((flags & 16) != 0 || (flags & 64) != 0) {
        consumeVertexSequence();
        m_skippedTypes[QStringLiteral("POLYLINE(网格)")] =
            m_skippedTypes.value(QStringLiteral("POLYLINE(网格)"), 0) + 1;
        return out;
    }

    const bool closed = (flags & 1) != 0;

    std::vector<PolylineEntity::Vertex> vertices;
    DxfGroup g;
    while (m_reader->next(g)) {
        if (g.code != 0) continue;

        if (g.text == QLatin1String("SEQEND")) {
            collectUntilNextRecord();
            break;
        }
        if (g.text != QLatin1String("VERTEX")) {
            m_reader->pushBack(g);
            break;
        }

        const std::vector<DxfGroup> vertexGroups = collectUntilNextRecord();
        const int vertexFlags = intOf(vertexGroups, 70, 0);

        // bit 16/32 = 网格顶点，bit 128 = 样条框架控制点：均不属于二维多段线顶点
        if ((vertexFlags & (16 | 32 | 128)) != 0) continue;

        PolylineEntity::Vertex v;
        v.point = Point2D(doubleOf(vertexGroups, 10), doubleOf(vertexGroups, 20));
        v.bulge = doubleOf(vertexGroups, 42, 0.0);
        vertices.push_back(v);
    }

    if (vertices.size() < 2) {
        if (result) result->addWarning(QStringLiteral("跳过了一个顶点数不足的 POLYLINE。"));
        m_skippedTypes[QStringLiteral("POLYLINE(顶点不足)")] =
            m_skippedTypes.value(QStringLiteral("POLYLINE(顶点不足)"), 0) + 1;
        return out;
    }

    auto polyline = std::make_unique<PolylineEntity>(std::move(vertices), closed);
    applyCommonProperties(header, *polyline);
    out.push_back(std::move(polyline));
    return out;
}

void DxfReader::consumeVertexSequence()
{
    DxfGroup g;
    while (m_reader->next(g)) {
        if (g.code != 0) continue;
        if (g.text == QLatin1String("SEQEND")) {
            collectUntilNextRecord();
            return;
        }
        if (g.text == QLatin1String("VERTEX")) {
            collectUntilNextRecord();
            continue;
        }
        m_reader->pushBack(g);
        return;
    }
}

std::vector<std::unique_ptr<Entity>> DxfReader::readInsert(IoResult* result)
{
    std::vector<std::unique_ptr<Entity>> out;

    const std::vector<DxfGroup> groups = collectUntilNextRecord();

    // INSERT 之后可能跟随 ATTRIB 序列，需一并消费
    DxfGroup g;
    while (m_reader->next(g)) {
        if (g.code != 0) continue;
        if (g.text == QLatin1String("ATTRIB")) {
            collectUntilNextRecord();
            continue;
        }
        if (g.text == QLatin1String("SEQEND")) {
            collectUntilNextRecord();
            break;
        }
        m_reader->pushBack(g);
        break;
    }

    const QString blockName = stringOf(groups, 2);
    if (blockName.isEmpty()) {
        if (result) result->addWarning(QStringLiteral("跳过一个未指明块名的 INSERT。"));
        return out;
    }
    if (blockName.startsWith(QLatin1Char('*'))) {
        return out;                     // *Model_Space / *Paper_Space 等内部块不含图形内容
    }

    const BlockDefinition* block = findBlock(blockName);
    if (!block) {
        const QString key = QStringLiteral("INSERT(未定义块 %1)").arg(blockName);
        m_skippedTypes[key] = m_skippedTypes.value(key, 0) + 1;
        return out;
    }

    const Point2D insertPoint(doubleOf(groups, 10), doubleOf(groups, 20));
    const double sx = doubleOf(groups, 41, 1.0);
    const double sy = doubleOf(groups, 42, 1.0);
    const double rotation = degToRad(doubleOf(groups, 50, 0.0));
    const QString insertLayer = stringOf(groups, 8, QStringLiteral("0"));

    // 块内坐标 -> 世界坐标：
    //   先平移到块基点，再做缩放、旋转，最后平移到插入点
    const Transform2D transform = Transform2D::translation(insertPoint.x, insertPoint.y)
                                * Transform2D::rotation(rotation)
                                * Transform2D::scaling(sx == 0.0 ? 1.0 : sx,
                                                       sy == 0.0 ? 1.0 : sy)
                                * Transform2D::translation(-block->basePoint.x, -block->basePoint.y);

    explodeBlock(blockName, transform, insertLayer, 0, out, result);
    ++m_explodedInserts;
    return out;
}

void DxfReader::explodeBlock(const QString& blockName,
                             const Transform2D& transform,
                             const QString& insertLayer,
                             int depth,
                             std::vector<std::unique_ptr<Entity>>& out,
                             IoResult* result)
{
    if (depth >= kMaxBlockDepth) {
        ++m_insertDepthViolations;
        return;
    }

    const BlockDefinition* block = findBlock(blockName);
    if (!block) return;

    for (const auto& source : block->entities) {
        if (!source) continue;

        std::unique_ptr<Entity> clone = source->clone();

        // 块内位于 "0" 层的图元继承 INSERT 所在图层——DXF 的标准语义
        if (clone->layerName() == QLatin1String("0") && !insertLayer.isEmpty()) {
            clone->setLayerName(insertLayer);
        }

        clone->transform(transform);
        out.push_back(std::move(clone));
    }

    // 递归展开块内的嵌套引用
    for (const BlockInsertRef& ref : block->nestedInserts) {
        const BlockDefinition* nested = findBlock(ref.targetBlock);
        if (!nested) {
            const QString key = QStringLiteral("INSERT(未定义块 %1)").arg(ref.targetBlock);
            m_skippedTypes[key] = m_skippedTypes.value(key, 0) + 1;
            continue;
        }

        const Transform2D local = Transform2D::translation(ref.insertPoint.x, ref.insertPoint.y)
                                * Transform2D::rotation(ref.rotation)
                                * Transform2D::scaling(ref.scaleX == 0.0 ? 1.0 : ref.scaleX,
                                                       ref.scaleY == 0.0 ? 1.0 : ref.scaleY)
                                * Transform2D::translation(-nested->basePoint.x, -nested->basePoint.y);

        const QString layer = ref.layer.isEmpty() ? insertLayer : ref.layer;
        explodeBlock(ref.targetBlock, transform * local, layer, depth + 1, out, result);
    }
}

// -----------------------------------------------------------------------------
//  警告汇总
// -----------------------------------------------------------------------------
void DxfReader::collectWarnings(IoResult* result)
{
    if (!result) return;

    // 未支持实体的类型较多时逐条列出会淹没关键信息，这里按数量降序取前 8 类
    struct Entry { QString type; int count; };
    std::vector<Entry> entries;
    entries.reserve(static_cast<std::size_t>(m_skippedTypes.size()));
    for (auto it = m_skippedTypes.constBegin(); it != m_skippedTypes.constEnd(); ++it) {
        entries.push_back(Entry{ it.key(), it.value() });
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.count > b.count; });

    const std::size_t limit = std::min<std::size_t>(entries.size(), 8);
    int total = 0;
    for (const Entry& e : entries) total += e.count;

    for (std::size_t i = 0; i < limit; ++i) {
        result->addWarning(QStringLiteral("跳过 %1 个 %2").arg(entries[i].count).arg(entries[i].type));
    }
    if (entries.size() > limit) {
        result->addWarning(QStringLiteral("另有 %1 类未列举的实体共 %2 个")
                               .arg(entries.size() - limit)
                               .arg(total - [&] {
                                   int shown = 0;
                                   for (std::size_t i = 0; i < limit; ++i) shown += entries[i].count;
                                   return shown;
                               }()));
    }

    if (m_explodedInserts > 0) {
        result->addWarning(QStringLiteral("已展开 %1 个块引用（INSERT），块内图元转为独立实体")
                               .arg(m_explodedInserts));
    }
    if (m_insertDepthViolations > 0) {
        result->addWarning(QStringLiteral("%1 处块引用嵌套超过 %2 层，已停止展开")
                               .arg(m_insertDepthViolations).arg(kMaxBlockDepth));
    }
    if (!m_acadVersion.isEmpty()) {
        result->addWarning(QStringLiteral("源文件版本 %1").arg(m_acadVersion));
    }
}

} // namespace lycad
