// =============================================================================
//  LyCAD - io/LibDxfRwAdapter.cpp
//  内置 libdxfrw 的适配层实现。
//
//  ── 关于角度单位（这一节是实测定标的结果，改动前请先读） ────────────────────
//
//  libdxfrw 在「读 DXF」与「读 DWG」两条路径上，对同一字段的单位处理**并不一致**。
//  这一点无法从它对外宣称的接口看出，只能逐个字段核对源码并实测验证。核对结果：
//
//    字段                DXF 路径              DWG 路径        本适配器
//    ────────────────────────────────────────────────────────────────────
//    ARC 起止角          /ARAD（得弧度）        不换算          两者都是弧度，直接用
//    HATCH 边界弧角       同上                  不换算          两者都是弧度，直接用
//    INSERT 旋转         /ARAD（得弧度）        不换算          两者都是弧度，直接用
//    TEXT/MTEXT 旋转     不换算（度）           不换算          **单位不同，需按来源换算**
//    DIMENSION 角度      不换算（度）           ×ARAD（得度）    两者都是度，需转为弧度
//
//  实测依据（720 个真实 DWG 文件，共 29018 个弧角度样本）：
//    · 出现次数最多的取值是 3.141593(π) ×1234、4.712389(3π/2) ×1069、
//      1.570796(π/2) ×1003；而「接近整数度」的样本数为 **0** —— 说明 DWG 路径下
//      弧角度是弧度。
//    · 文字旋转在 DWG 中恰为 1.570796(π/2)，在 DXF 中则为 90.0 —— 两者单位不同。
//    · INSERT 旋转出现 6.021386，正好是 23π/12（即 345.000°）—— 弧度。
//
//  因此 makeText() 里那条按来源分支的换算是必需的，不是保险措施；删掉它会让
//  DWG 里的文字旋转全部错 57 倍，而 DXF 里的一切正常——这类缺陷很难在使用中归因。
//
//  ── 关于可见性 ────────────────────────────────────────────────────────────
//
//  libdxfrw-modify 已把两条路径的可见性按规范修正（DXF 按 60 组码取反，
//  DWG 把原本被丢弃的 invisibleFlag 赋给 visible，见该库 drw_entities.cpp），
//  这里按数据直接导入。
// =============================================================================
#include "io/LibDxfRwAdapter.h"

#include "core/AciPalette.h"
#include "core/ArcEntity.h"
#include "core/CircleEntity.h"
#include "core/DimensionEntity.h"
#include "core/Document.h"
#include "core/HatchEntity.h"
#include "core/Layer.h"
#include "core/Linetype.h"
#include "core/LineEntity.h"
#include "core/PointEntity.h"
#include "core/PolylineEntity.h"
#include "core/TextEntity.h"
#include "core/Types.h"

// libdxfrw（第三方）。这些头文件只在本文件出现，不外溢到其它翻译单元。
#include "drw_entities.h"
#include "drw_interface.h"
#include "drw_objects.h"
#include "libdwgr.h"
#include "libdxfrw.h"

#include <QFile>
#include <QHash>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace lycad {
namespace {

/// 块引用递归展开的深度上限（防止自引用块造成无限递归）
constexpr int kMaxBlockDepth = 16;

inline Point2D toPoint(const DRW_Coord& c) noexcept
{
    return Point2D(c.x, c.y);
}

/// 还原被 libdxfrw 破坏的 \U+XXXX 转义序列。
///
/// R12 的 DXF 没有 UTF-8 可言，非 ASCII 只能用 \U+XXXX 表达；libdxfrw 在处理
/// 这类序列时，会把前导的 '\\' 当成代码页里的字符解出来——在它使用的字符表里
/// 该位置正是 '¥'(U+00A5)，于是 "\U+65CB" 变成 "¥U+65CB"，中文全部退化成
/// 可见的转义文本。
///
/// 这里把两种形态都还原成真正的字符。它是补偿而非主路径：本项目的写出器在
/// R2000 及以上直接写 UTF-8（见 DxfGroupWriter::setUtf8Text），根本不会产生
/// 转义序列，只有读取他人生成的 R12 文件时才会用到这段逻辑。
QString repairUnicodeEscapes(const QString& text)
{
    if (!text.contains(QLatin1String("U+"))) return text;

    QString out;
    out.reserve(text.size());

    const int n = text.size();
    for (int i = 0; i < n;) {
        const QChar ch = text.at(i);
        const bool escaped = (ch == QLatin1Char('\\') || ch == QChar(0x00A5))
                             && i + 6 < n
                             && text.at(i + 1) == QLatin1Char('U')
                             && text.at(i + 2) == QLatin1Char('+');
        if (escaped) {
            bool ok = false;
            const uint code = text.mid(i + 3, 4).toUInt(&ok, 16);
            if (ok) {
                out.append(QChar(static_cast<ushort>(code)));
                i += 7;
                continue;
            }
        }
        out.append(ch);
        ++i;
    }
    return out;
}

/// libdxfrw 的字符串一律是 UTF-8（它内部用 drw_textcodec 做过编码转换）
inline QString toQString(const std::string& s)
{
    return repairUnicodeEscapes(
        QString::fromUtf8(s.c_str(), static_cast<int>(s.size())));
}

/// 是否为「容器块」——模型空间与图纸空间。它们不承载图形定义，其内容由读取器
/// 直接按实体流回调；而 *U / *D1 / *T1 这类匿名块是真正的块定义，必须收集。
bool isSpaceBlock(const QString& name)
{
    return name.startsWith(QLatin1String("*Model_Space"))
        || name.startsWith(QLatin1String("*Paper_Space"));
}

/// 逆时针归一到 (0, 2π]
inline double sweepCcw(double start, double end) noexcept
{
    double s = std::fmod(end - start, kTwoPi);
    if (s <= 0.0) s += kTwoPi;
    return s;
}

/// DRW::Version -> "AC1015" 形式的签名码
QString versionCode(DRW::Version v)
{
    switch (v) {
    case DRW::AC1006: return QStringLiteral("AC1006");
    case DRW::AC1009: return QStringLiteral("AC1009");
    case DRW::AC1012: return QStringLiteral("AC1012");
    case DRW::AC1014: return QStringLiteral("AC1014");
    case DRW::AC1015: return QStringLiteral("AC1015");
    case DRW::AC1018: return QStringLiteral("AC1018");
    case DRW::AC1021: return QStringLiteral("AC1021");
    case DRW::AC1024: return QStringLiteral("AC1024");
    case DRW::AC1027: return QStringLiteral("AC1027");
    case DRW::AC1032: return QStringLiteral("AC1032");
    default: break;
    }
    return QString();
}

// -----------------------------------------------------------------------------
//  Builder：把 libdxfrw 的回调装配成 LyCAD 文档
// -----------------------------------------------------------------------------
class Builder final : public DRW_Interface {
public:
    /// 读取来源。libdxfrw 对文字旋转的单位处理在两条路径下不同，据此换算。
    enum class Source { Dxf, Dwg };

    Builder(Document& document, Source source)
        : m_doc(document), m_source(source) {}

    /// 收尾：展开块引用、汇总未建模实体。必须在读取结束后调用。
    void finish();

    /// 未建模的实体类型 -> 个数
    const QHash<QString, int>& skippedTypes() const noexcept { return m_skipped; }

    /// 由块引用展开出的图元数
    int expandedCount() const noexcept { return m_expanded; }

    /// 文件头里的 $ACADVER（如 "AC1015"），未携带时为空
    QString acadVersion() const { return m_acadVersion; }

    // ---- 表与头 --------------------------------------------------------------
    void addHeader(const DRW_Header* data) override;
    void addLType(const DRW_LType&) override {}
    void addLayer(const DRW_Layer& data) override;
    void addDimStyle(const DRW_Dimstyle&) override {}
    void addVport(const DRW_Vport&) override {}
    void addTextStyle(const DRW_Textstyle&) override {}
    void addAppId(const DRW_AppId&) override {}
    /// 库不认识的实体类型（MLINE、MESH、ACAD_TABLE 等）经此回调计数上报，
    /// 不允许静默丢弃。
    void addUnknownEntity(const std::string& name) override
    {
        skip(toQString(name));
    }

    // ---- 块 ------------------------------------------------------------------
    void addBlock(const DRW_Block& data) override;
    void setBlock(const int handle) override;
    void endBlock() override;

    // ---- 图元 ----------------------------------------------------------------
    void addPoint(const DRW_Point& data) override;
    void addLine(const DRW_Line& data) override;
    void addRay(const DRW_Ray&) override { skip(QStringLiteral("RAY")); }
    void addXline(const DRW_Xline&) override { skip(QStringLiteral("XLINE")); }
    void addArc(const DRW_Arc& data) override;
    void addCircle(const DRW_Circle& data) override;
    void addEllipse(const DRW_Ellipse&) override { skip(QStringLiteral("ELLIPSE")); }
    void addLWPolyline(const DRW_LWPolyline& data) override;
    void addPolyline(const DRW_Polyline& data) override;
    void addSpline(const DRW_Spline*) override { skip(QStringLiteral("SPLINE")); }
    void addKnot(const DRW_Entity&) override {}
    void addInsert(const DRW_Insert& data) override;
    void addTrace(const DRW_Trace& data) override;
    void add3dFace(const DRW_3Dface&) override { skip(QStringLiteral("3DFACE")); }
    void addSolid(const DRW_Solid& data) override;
    void addMText(const DRW_MText& data) override;
    void addText(const DRW_Text& data) override;

    // ---- 标注 ----------------------------------------------------------------
    void addDimAlign(const DRW_DimAligned* data) override;
    void addDimLinear(const DRW_DimLinear* data) override;
    void addDimRadial(const DRW_DimRadial* data) override;
    void addDimDiametric(const DRW_DimDiametric* data) override;
    void addDimAngular(const DRW_DimAngular*) override { skip(QStringLiteral("DIMANGULAR")); }
    void addDimAngular3P(const DRW_DimAngular3p*) override { skip(QStringLiteral("DIMANGULAR3P")); }
    void addDimOrdinate(const DRW_DimOrdinate*) override { skip(QStringLiteral("DIMORDINATE")); }

    // ---- 其余（未建模，计数后跳过） -------------------------------------------
    void addLeader(const DRW_Leader*) override { skip(QStringLiteral("LEADER")); }
    void addHatch(const DRW_Hatch* data) override;
    void addViewport(const DRW_Viewport&) override { skip(QStringLiteral("VIEWPORT")); }
    void addImage(const DRW_Image*) override { skip(QStringLiteral("IMAGE")); }
    void linkImage(const DRW_ImageDef*) override {}
    void addComment(const char*) override {}

    // ---- 写出接口：本适配器只用于读取，全部留空 --------------------------------
    void writeHeader(DRW_Header&) override {}
    void writeBlocks() override {}
    void writeBlockRecords() override {}
    void writeEntities() override {}
    void writeLTypes() override {}
    void writeLayers() override {}
    void writeTextstyles() override {}
    void writeVports() override {}
    void writeDimstyles() override {}
    void writeAppId() override {}

private:
    /// 一次待展开的块引用
    struct PendingInsert {
        QString blockName;
        Point2D point;
        double  scaleX = 1.0;
        double  scaleY = 1.0;
        double  rotation = 0.0;          ///< 弧度
        QString layer;
        int     colCount = 1;
        int     rowCount = 1;
        double  colSpace = 0.0;
        double  rowSpace = 0.0;
    };

    struct BlockDef {
        QString name;
        Point2D base;
        std::vector<std::unique_ptr<Entity>> entities;
        std::vector<PendingInsert> inserts;
    };

    /// 把图元放到正确的位置：块内 or 模型空间
    void place(std::unique_ptr<Entity> entity);
    void skip(const QString& type);

    /// 套用图层/颜色/线型/线宽等公共属性
    void applyCommon(Entity& entity, const DRW_Entity& source);

    std::unique_ptr<TextEntity> makeText(const DRW_Text& data);
    void applyHatchBoundary(const DRW_Hatch& data);

    void expand(const PendingInsert& insert, const Transform2D& outer,
                int depth, const QString& inheritedLayer);
    void expandAll();

    Document& m_doc;
    Source    m_source;

    std::map<QString, BlockDef> m_blocks;
    BlockDef* m_currentBlock = nullptr;
    std::map<duint32, QString> m_blockNameByHandle;

    std::vector<PendingInsert> m_modelInserts;

    QHash<QString, int> m_skipped;
    QString m_acadVersion;
    int m_missingBlocks = 0;
    int m_depthExceeded = 0;
    int m_expanded = 0;
};

// -----------------------------------------------------------------------------
//  公共属性
// -----------------------------------------------------------------------------
void Builder::applyCommon(Entity& entity, const DRW_Entity& source)
{
    const QString layerName = toQString(source.layer);
    entity.setLayerName(layerName);

    // 实体引用的图层可能没有对应的 LAYER 记录（极简 DXF、被截断的文件）：
    // 按引用补齐图层，否则该图层在图层面板里凭空消失。
    // LayerTable::ensure 的注释写明它就是为这种导入场景准备的。
    if (!layerName.isEmpty()) m_doc.layers().ensure(layerName);

    // ---- 颜色 ----------------------------------------------------------------
    // 24 位真彩色（420 组码）优先：本软件内部只有 ACI 索引，因此取最接近的索引。
    if (source.color24 >= 0) {
        const int rgb = source.color24;
        entity.setColorIndex(aci::fromColor(QColor((rgb >> 16) & 0xFF,
                                                   (rgb >> 8) & 0xFF,
                                                   rgb & 0xFF)));
    } else if (source.color > 0 && source.color < aci::kByLayer) {
        entity.setColorIndex(source.color);
    } else {
        // 256 = 随层；0 = 随块，本软件无此概念，按随层处理
        entity.setColorByLayer();
    }

    // ---- 线型 ----------------------------------------------------------------
    const QString linetype = toQString(source.lineType);
    if (linetype.isEmpty() || linetype.compare(QLatin1String("BYLAYER"), Qt::CaseInsensitive) == 0) {
        entity.setLinetypeName(QString());       // 空 = 随层
    } else {
        entity.setLinetypeName(linetype);
    }

    // ---- 线宽 ----------------------------------------------------------------
    entity.setLineweightDxf(DRW_LW_Conv::lineWidth2dxfInt(source.lWeight));

    // ---- 可见性 --------------------------------------------------------------
    // libdxfrw-modify 已让 visible 在 DXF 与 DWG 两条路径下都符合语义
    // （见该库 drw_entities.cpp 中的说明），这里按数据直接导入。
    entity.setVisible(source.visible);
}

void Builder::place(std::unique_ptr<Entity> entity)
{
    if (!entity) return;

    if (m_currentBlock) {
        m_currentBlock->entities.push_back(std::move(entity));
    } else {
        m_doc.addEntity(std::move(entity));
    }
}

void Builder::skip(const QString& type)
{
    m_skipped[type] = m_skipped.value(type, 0) + 1;
}

// -----------------------------------------------------------------------------
//  表
// -----------------------------------------------------------------------------
void Builder::addHeader(const DRW_Header* data)
{
    // 目前只关心 $ACADVER（用于在导入警告中报告源文件版本）。
    // 注意：varMap 里的值指针归 DRW_Header 所有，这里只读不存指针。
    if (!data) return;
    const auto it = data->vars.find("$ACADVER");
    if (it != data->vars.end() && it->second
        && it->second->type() == DRW_Variant::STRING && it->second->content.s) {
        m_acadVersion = toQString(*it->second->content.s).trimmed();
    }
}

void Builder::addLayer(const DRW_Layer& data)
{
    const QString name = toQString(data.name).trimmed();
    if (name.isEmpty()) return;

    // ensure 对已存在的图层返回既有引用（内置的 "0" 层就属于这种情况）
    Layer& layer = m_doc.layers().ensure(name);

    // DXF 用负数颜色表示「图层关闭」
    int aci = data.color;
    bool visible = true;
    if (aci < 0) {
        aci = -aci;
        visible = false;
    }
    if (aci >= 1 && aci <= 255) layer.setColorIndex(aci);
    layer.setVisible(visible);

    const QString linetype = toQString(data.lineType);
    if (!linetype.isEmpty()) layer.setLinetype(linetype);

    layer.setLineweightDxf(DRW_LW_Conv::lineWidth2dxfInt(data.lWeight));
    layer.setFlagsFromDxf(data.flags);
}

// -----------------------------------------------------------------------------
//  块
// -----------------------------------------------------------------------------
void Builder::addBlock(const DRW_Block& data)
{
    const QString name = toQString(data.name);
    if (name.isEmpty()) return;

    // 模型空间 / 图纸空间是容器，不收集；其余块（含 *U、*D1 等匿名块）正常收集。
    // 尺寸标注的几何块、EDA 工具导出的匿名块都属于后者，丢掉它们会让图元消失。
    if (isSpaceBlock(name)) {
        m_currentBlock = nullptr;
        m_blockNameByHandle[data.handle] = name;
        return;
    }

    BlockDef& block = m_blocks[name];
    block.name = name;
    block.base = toPoint(data.basePoint);
    m_currentBlock = &block;
    m_blockNameByHandle[data.handle] = name;
}

void Builder::setBlock(const int handle)
{
    // DWG 路径用句柄切换当前块；句柄未知或为 0 表示回到模型空间。
    const auto it = m_blockNameByHandle.find(handle);
    if (it == m_blockNameByHandle.end() || isSpaceBlock(it->second)) {
        m_currentBlock = nullptr;
        return;
    }

    const auto block = m_blocks.find(it->second);
    m_currentBlock = (block == m_blocks.end()) ? nullptr : &block->second;
}

void Builder::endBlock()
{
    m_currentBlock = nullptr;
}

void Builder::addInsert(const DRW_Insert& data)
{
    PendingInsert insert;
    insert.blockName = toQString(data.name);
    insert.point     = toPoint(data.basePoint);
    insert.scaleX    = (data.xscale == 0.0) ? 1.0 : data.xscale;
    insert.scaleY    = (data.yscale == 0.0) ? 1.0 : data.yscale;
    insert.rotation  = data.angle;            // 两条路径都是弧度
    insert.layer     = toQString(data.layer);
    insert.colCount  = std::max(1, data.colcount);
    insert.rowCount  = std::max(1, data.rowcount);
    insert.colSpace  = data.colspace;
    insert.rowSpace  = data.rowspace;

    if (insert.blockName.isEmpty()) return;

    if (m_currentBlock) m_currentBlock->inserts.push_back(insert);
    else                m_modelInserts.push_back(insert);
}

// -----------------------------------------------------------------------------
//  基础图元
// -----------------------------------------------------------------------------
void Builder::addPoint(const DRW_Point& data)
{
    auto entity = std::make_unique<PointEntity>(toPoint(data.basePoint));
    applyCommon(*entity, data);
    place(std::move(entity));
}

void Builder::addLine(const DRW_Line& data)
{
    const Point2D a = toPoint(data.basePoint);
    const Point2D b = toPoint(data.secPoint);
    if (a.distanceTo(b) <= kGeomTol) {
        skip(QStringLiteral("LINE(零长度)"));
        return;
    }

    auto entity = std::make_unique<LineEntity>(a, b);
    applyCommon(*entity, data);
    place(std::move(entity));
}

void Builder::addCircle(const DRW_Circle& data)
{
    if (data.radious <= kGeomTol) {
        skip(QStringLiteral("CIRCLE(半径为零)"));
        return;
    }

    auto entity = std::make_unique<CircleEntity>(toPoint(data.basePoint), data.radious);
    applyCommon(*entity, data);
    place(std::move(entity));
}

void Builder::addArc(const DRW_Arc& data)
{
    if (data.radious <= kGeomTol) {
        skip(QStringLiteral("ARC(半径为零)"));
        return;
    }

    // 起止角在两条路径下都是弧度（实测定标，见文件头）
    auto entity = std::make_unique<ArcEntity>(toPoint(data.basePoint), data.radious,
                                              data.staangle, data.endangle);
    applyCommon(*entity, data);
    place(std::move(entity));
}

// -----------------------------------------------------------------------------
//  多段线
// -----------------------------------------------------------------------------
void Builder::addLWPolyline(const DRW_LWPolyline& data)
{
    std::vector<PolylineEntity::Vertex> vertices;
    vertices.reserve(data.vertlist.size());
    for (const DRW_Vertex2D* v : data.vertlist) {
        if (!v) continue;
        PolylineEntity::Vertex vertex;
        vertex.point      = Point2D(v->x, v->y);
        vertex.bulge      = v->bulge;
        vertex.startWidth = v->stawidth;
        vertex.endWidth   = v->endwidth;
        vertices.push_back(vertex);
    }

    if (vertices.size() < 2) {
        skip(QStringLiteral("LWPOLYLINE(顶点不足)"));
        return;
    }

    auto entity = std::make_unique<PolylineEntity>(std::move(vertices), (data.flags & 1) != 0);
    if (data.width > kGeomTol) entity->setConstantWidth(data.width);
    applyCommon(*entity, data);
    place(std::move(entity));
}

void Builder::addPolyline(const DRW_Polyline& data)
{
    // 70 组码的类型位：8 = 三维多段线，16 = 三维网格，64 = 多面网格。
    // 本软件只建模二维多段线，其余如实计数跳过。
    const int flags = data.flags;
    if ((flags & (8 | 16 | 64)) != 0) {
        skip(QStringLiteral("POLYLINE(三维/网格)"));
        return;
    }

    std::vector<PolylineEntity::Vertex> vertices;
    vertices.reserve(data.vertlist.size());
    for (const DRW_Vertex* v : data.vertlist) {
        if (!v) continue;
        PolylineEntity::Vertex vertex;
        vertex.point      = toPoint(v->basePoint);
        vertex.bulge      = v->bulge;
        vertex.startWidth = v->stawidth;
        vertex.endWidth   = v->endwidth;
        vertices.push_back(vertex);
    }

    if (vertices.size() < 2) {
        skip(QStringLiteral("POLYLINE(顶点不足)"));
        return;
    }

    auto entity = std::make_unique<PolylineEntity>(std::move(vertices), (flags & 1) != 0);
    if (data.defstawidth > kGeomTol) entity->setConstantWidth(data.defstawidth);
    applyCommon(*entity, data);
    place(std::move(entity));
}

void Builder::addSolid(const DRW_Solid& data)
{
    // SOLID 的四个角点按 10/11/12/13 给出，但顺序是「1、2、4、3」——
    // 要交换后两个才是多边形绕行顺序。
    std::vector<Point2D> points = {
        toPoint(data.basePoint),
        toPoint(data.secPoint),
        toPoint(data.fourPoint),
        toPoint(data.thirdPoint),
    };

    auto entity = std::make_unique<PolylineEntity>(points, true);
    applyCommon(*entity, data);
    place(std::move(entity));
}

void Builder::addTrace(const DRW_Trace& data)
{
    std::vector<Point2D> points = {
        toPoint(data.basePoint),
        toPoint(data.secPoint),
        toPoint(data.fourPoint),
        toPoint(data.thirdPoint),
    };

    auto entity = std::make_unique<PolylineEntity>(points, true);
    applyCommon(*entity, data);
    place(std::move(entity));
}

// -----------------------------------------------------------------------------
//  文字
// -----------------------------------------------------------------------------
std::unique_ptr<TextEntity> Builder::makeText(const DRW_Text& data)
{
    // 单位换算：DXF 路径下 libdxfrw 原样返回 50 组码（度），DWG 路径下是弧度。
    const double rotation = (m_source == Source::Dxf) ? degToRad(data.angle)
                                                      : data.angle;

    const double height = (data.height > kGeomTol) ? data.height : 3.5;
    auto entity = std::make_unique<TextEntity>(toQString(data.text),
                                               toPoint(data.basePoint),
                                               height, rotation);

    // 对齐：libdxfrw 的枚举与 DXF 72/73 一致；Aligned(3)/Middle(4)/Fit(5)
    // 只在垂直对齐为基线时出现，这里分别映射到最接近的语义。
    switch (data.alignH) {
    case DRW_Text::HCenter:
    case DRW_Text::HMiddle:
        entity->setHAlign(TextEntity::HAlign::Center);
        break;
    case DRW_Text::HRight:
        entity->setHAlign(TextEntity::HAlign::Right);
        break;
    default:
        entity->setHAlign(TextEntity::HAlign::Left);
        break;
    }

    switch (data.alignV) {
    case DRW_Text::VBottom: entity->setVAlign(TextEntity::VAlign::Bottom); break;
    case DRW_Text::VMiddle: entity->setVAlign(TextEntity::VAlign::Middle); break;
    case DRW_Text::VTop:    entity->setVAlign(TextEntity::VAlign::Top);    break;
    default:                entity->setVAlign(TextEntity::VAlign::Baseline); break;
    }

    // 第二对齐点（11/21）：缺省时回落至插入点，避免对齐文字跑到原点
    Point2D alignmentPoint = toPoint(data.secPoint);
    if (alignmentPoint.distanceTo(Point2D(0.0, 0.0)) <= kGeomTol
        && entity->position().distanceTo(Point2D(0.0, 0.0)) > kGeomTol) {
        alignmentPoint = entity->position();
    }
    entity->setAlignmentPoint(alignmentPoint);

    // 71 生成标志：bit2 = 水平镜像（背面文字），bit4 = 上下翻转
    entity->setBackwards((data.textgen & 2) != 0);
    entity->setUpsideDown((data.textgen & 4) != 0);

    if (std::abs(data.widthscale) > kGeomTol
        && std::abs(std::abs(data.widthscale) - 1.0) > 1e-9) {
        entity->setWidthFactor(data.widthscale);
    }

    const QString style = toQString(data.style);
    if (!style.isEmpty() && style.compare(QLatin1String("STANDARD"), Qt::CaseInsensitive) != 0) {
        entity->setFontStyle(style);
    }

    return entity;
}

void Builder::addText(const DRW_Text& data)
{
    auto entity = makeText(data);
    applyCommon(*entity, data);
    place(std::move(entity));
}

void Builder::addMText(const DRW_MText& data)
{
    // 多行文字未建模，但内容仍值得保留：取其中的文字并按单行导入。
    // 这里如实计数，让用户知道它被降级处理过。
    auto entity = makeText(data);
    if (entity) {
        applyCommon(*entity, data);
        place(std::move(entity));
        skip(QStringLiteral("MTEXT(按单行文字导入)"));
    }
}

// -----------------------------------------------------------------------------
//  填充
// -----------------------------------------------------------------------------
void Builder::applyHatchBoundary(const DRW_Hatch& data)
{
    // 取第一个有效边界环。多环（带孤岛）的填充暂取外环。
    std::vector<Point2D> boundary;
    std::vector<double> bulges;
    bool found = false;

    for (const DRW_HatchLoop* loop : data.looplist) {
        if (!loop) continue;
        boundary.clear();
        bulges.clear();

        const bool polylinePath = (loop->type & 2) != 0;
        if (polylinePath) {
            for (const DRW_Entity* edge : loop->objlist) {
                const auto* poly = dynamic_cast<const DRW_LWPolyline*>(edge);
                if (!poly) continue;
                for (const DRW_Vertex2D* v : poly->vertlist) {
                    if (!v) continue;
                    boundary.push_back(Point2D(v->x, v->y));
                    bulges.push_back(v->bulge);
                }
            }
        } else {
            // 由直线/圆弧边拼成的环：串成带凸度的顶点序列
            for (const DRW_Entity* edge : loop->objlist) {
                if (const auto* arc = dynamic_cast<const DRW_Arc*>(edge)) {
                    if (arc->radious <= kGeomTol) continue;
                    double sweep = sweepCcw(arc->staangle, arc->endangle);
                    if (arc->isccw == 0) sweep -= kTwoPi;      // 顺时针边
                    const Point2D center = toPoint(arc->basePoint);
                    boundary.push_back(Point2D(center.x + arc->radious * std::cos(arc->staangle),
                                               center.y + arc->radious * std::sin(arc->staangle)));
                    bulges.push_back(std::tan(sweep / 4.0));
                } else if (const auto* line = dynamic_cast<const DRW_Line*>(edge)) {
                    boundary.push_back(toPoint(line->basePoint));
                    bulges.push_back(0.0);
                }
            }
        }

        // 「2 顶点 + 凸度」是整圆的表达（EDA 焊盘的常见形态），同样算有效边界
        const bool hasBulge = std::any_of(bulges.begin(), bulges.end(),
                                          [](double b) { return std::abs(b) > kGeomTol; });
        if (boundary.size() >= 3 || (boundary.size() == 2 && hasBulge)) {
            found = true;
            break;
        }
    }

    if (!found) {
        skip(QStringLiteral("HATCH(无可解析边界)"));
        return;
    }

    auto entity = std::make_unique<HatchEntity>(std::move(boundary), std::move(bulges));
    entity->setPatternName(data.solid ? QStringLiteral("SOLID") : toQString(data.name));
    if (data.scale > kGeomTol) entity->setPatternScale(data.scale);
    entity->setPatternAngle(degToRad(data.angle));      // 图案角在两条路径下都是度
    applyCommon(*entity, data);
    place(std::move(entity));
}

void Builder::addHatch(const DRW_Hatch* data)
{
    if (!data) return;
    applyHatchBoundary(*data);
}

// -----------------------------------------------------------------------------
//  标注
// -----------------------------------------------------------------------------
void Builder::addDimAlign(const DRW_DimAligned* data)
{
    if (!data) return;

    auto entity = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Aligned);
    entity->setFirstPoint(toPoint(data->getDef1Point()));
    entity->setSecondPoint(toPoint(data->getDef2Point()));
    entity->setDimLinePoint(toPoint(data->getDimPoint()));

    const QString override = toQString(data->getText()).trimmed();
    if (!override.isEmpty() && override != QLatin1String("<>")) entity->setTextOverride(override);

    if (entity->measurement() <= kGeomTol && override.isEmpty()) {
        skip(QStringLiteral("DIMALIGNED(测量值为零)"));
        return;
    }
    applyCommon(*entity, *data);
    place(std::move(entity));
}

void Builder::addDimLinear(const DRW_DimLinear* data)
{
    if (!data) return;

    auto entity = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Linear);
    entity->setFirstPoint(toPoint(data->getDef1Point()));
    entity->setSecondPoint(toPoint(data->getDef2Point()));
    entity->setDimLinePoint(toPoint(data->getDimPoint()));
    // DIMENSION 的 50 组码在两条路径下都是度（DWG 侧由库内 ×ARAD 换来）
    entity->setRotation(degToRad(data->getAngle()));

    const QString override = toQString(data->getText()).trimmed();
    if (!override.isEmpty() && override != QLatin1String("<>")) entity->setTextOverride(override);

    if (entity->measurement() <= kGeomTol && override.isEmpty()) {
        skip(QStringLiteral("DIMLINEAR(测量值为零)"));
        return;
    }
    applyCommon(*entity, *data);
    place(std::move(entity));
}

void Builder::addDimRadial(const DRW_DimRadial* data)
{
    if (!data) return;

    auto entity = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Radial);
    entity->setFirstPoint(toPoint(data->getCenterPoint()));
    entity->setSecondPoint(toPoint(data->getDiameterPoint()));

    const Point2D textPoint = toPoint(data->getTextPoint());
    if (textPoint.distanceTo(Point2D(0.0, 0.0)) <= kGeomTol) {
        const Vector2D u = (entity->secondPoint() - entity->firstPoint())
                               .normalizedOr(Vector2D(1.0, 0.0));
        entity->setDimLinePoint(entity->secondPoint() + u * (entity->textHeight() * 2.0));
    } else {
        entity->setDimLinePoint(textPoint);
    }

    const QString override = toQString(data->getText()).trimmed();
    if (!override.isEmpty() && override != QLatin1String("<>")) entity->setTextOverride(override);

    applyCommon(*entity, *data);
    place(std::move(entity));
}

void Builder::addDimDiametric(const DRW_DimDiametric* data)
{
    if (!data) return;

    auto entity = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Diametric);
    entity->setFirstPoint(toPoint(data->getDiameter2Point()));
    entity->setSecondPoint(toPoint(data->getDiameter1Point()));

    const Point2D textPoint = toPoint(data->getTextPoint());
    if (textPoint.distanceTo(Point2D(0.0, 0.0)) <= kGeomTol) {
        const Point2D a = entity->firstPoint();
        const Point2D b = entity->secondPoint();
        entity->setDimLinePoint(Point2D((a.x + b.x) * 0.5, (a.y + b.y) * 0.5));
    } else {
        entity->setDimLinePoint(textPoint);
    }

    const QString override = toQString(data->getText()).trimmed();
    if (!override.isEmpty() && override != QLatin1String("<>")) entity->setTextOverride(override);

    applyCommon(*entity, *data);
    place(std::move(entity));
}

// -----------------------------------------------------------------------------
//  块引用展开
// -----------------------------------------------------------------------------
void Builder::expand(const PendingInsert& insert, const Transform2D& outer,
                     int depth, const QString& inheritedLayer)
{
    if (depth > kMaxBlockDepth) {
        ++m_depthExceeded;
        return;
    }

    const auto it = m_blocks.find(insert.blockName);
    if (it == m_blocks.end()) {
        ++m_missingBlocks;
        return;
    }

    // 块内位于 "0" 层的图元继承 INSERT 所在图层（DXF 标准语义）
    const QString insertLayer =
        ((insert.layer.isEmpty() || insert.layer == QLatin1String("0")) && !inheritedLayer.isEmpty())
            ? inheritedLayer
            : insert.layer;

    const BlockDef& block = it->second;

    for (int row = 0; row < insert.rowCount; ++row) {
        for (int col = 0; col < insert.colCount; ++col) {
            const Point2D offset(col * insert.colSpace, row * insert.rowSpace);

            // 块内坐标 -> 世界坐标：平移到块基点、缩放、旋转，最后平移到插入点
            const Transform2D local =
                Transform2D::translation(insert.point.x + offset.x,
                                         insert.point.y + offset.y)
                * Transform2D::rotation(insert.rotation)
                * Transform2D::scaling(insert.scaleX, insert.scaleY)
                * Transform2D::translation(-block.base.x, -block.base.y);
            const Transform2D combined = outer * local;

            for (const std::unique_ptr<Entity>& source : block.entities) {
                auto copy = source->clone();
                if (copy->layerName() == QLatin1String("0") && !insertLayer.isEmpty()) {
                    copy->setLayerName(insertLayer);
                }
                copy->transform(combined);
                m_doc.addEntity(std::move(copy));
                ++m_expanded;
            }

            for (const PendingInsert& nested : block.inserts) {
                expand(nested, combined, depth + 1, insertLayer);
            }
        }
    }
}

void Builder::expandAll()
{
    for (const PendingInsert& insert : m_modelInserts) {
        expand(insert, Transform2D::identity(), 0, QString());
    }
    m_modelInserts.clear();
}

// -----------------------------------------------------------------------------
//  收尾
// -----------------------------------------------------------------------------
void Builder::finish()
{
    // 必须等全部读完再展开：块定义可能在引用之后才出现（DWG 的对象流不保证顺序）
    expandAll();

    if (m_missingBlocks > 0) {
        m_skipped[QStringLiteral("INSERT(引用的块未定义)")] = m_missingBlocks;
    }
    if (m_depthExceeded > 0) {
        m_skipped[QStringLiteral("INSERT(嵌套过深，已截断)")] = m_depthExceeded;
    }
}

} // namespace

// -----------------------------------------------------------------------------
//  对外入口
// -----------------------------------------------------------------------------
bool libDxfRwAvailable()
{
    // 源码已随项目编入（third-party/libdxfrw-modify），编到这个文件即代表可用。
    return true;
}

namespace {

/// 读取完成后的统一收尾：告警汇总、批处理结束
// -----------------------------------------------------------------------------
//  EDA 图层启发式配色
// -----------------------------------------------------------------------------
namespace {

/// 常见 EDA（Altium / KiCad / 立创 EDA 等）图层的行业惯例配色。
///
/// 不少 EDA 软件导出 DXF 时**不写图层颜色**（LAYER 记录无 62 组码），
/// 实体全部随层——在任何 CAD 中都会显示为单色，多板层叠在一起完全不可读。
/// 此处按图层名关键词给出约定俗成的配色（与主流 EDA 工具的 2D 视图一致）：
/// 只在图层颜色为默认值时生效，绝不覆盖显式颜色，并在导入警告中明确告知。
struct EdaLayerColor {
    const char* keyword;    ///< 图层名关键词（小写匹配）
    int         aci;        ///< AutoCAD 颜色索引
};

const std::vector<EdaLayerColor>& edaLayerColorTable()
{
    static const std::vector<EdaLayerColor> kTable = {
        { "top-layer",          1 },     ///< 顶层走线：红
        { "bottom-layer",       5 },     ///< 底层走线：蓝
        { "bottom-silkscreen",  3 },     ///< 底面丝印：绿（与顶面丝印分色，见 cmp 对比）
        { "silkscreen",         2 },     ///< 顶面/未指明丝印：黄
        { "solder-mask",        3 },     ///< 阻焊：绿
        { "paste-mask",         8 },     ///< 焊膏：灰
        { "board-outline",      6 },     ///< 板框：洋红
        { "3d-shell-outline",   6 },
        { "assembly",           4 },     ///< 装配层：青
        { "multi-layer",        8 },     ///< 多层（焊盘）：灰
        { "hole",               7 },     ///< 孔：白/黑
        { "document",           8 },
        { "mechanical",         8 },
        { "drill",              8 },
        { "ratline",            8 },
    };
    return kTable;
}

/// 命中 EDA 图层名时返回惯例配色；否则返回 0（未命中）
int heuristicEdaColor(const QString& layerName)
{
    const QString lower = layerName.toLower();
    for (const EdaLayerColor& entry : edaLayerColorTable()) {
        if (lower.contains(QLatin1String(entry.keyword))) return entry.aci;
    }
    return 0;
}

void applyEdaLayerColorHeuristic(Document& document, IoResult* result)
{
    // 判定条件从严：至少 3 个图层命中 EDA 命名，才认定这是一份
    // 未携带颜色的 EDA 导出文件，避免对普通图纸过度干预
    std::vector<std::pair<Layer*, int>> pending;
    int matched = 0;
    int colored = 0;

    for (Layer& layer : document.layers().all()) {
        if (const int aci = heuristicEdaColor(layer.name()); aci > 0) {
            ++matched;
            if (layer.colorIndex() != 7) {
                ++colored;      // 已有显式颜色，不覆盖
                continue;
            }
            pending.emplace_back(&layer, aci);
        }
    }

    if (matched < 3 || pending.empty()) return;

    for (auto& [layer, aci] : pending) layer->setColorIndex(aci);

    if (result) {
        result->addWarning(QStringLiteral("原文件未包含图层颜色，已按常见 EDA 图层名自动配色（%1 个图层）；可在图层面板中修改")
                               .arg(pending.size()));
    }
}

} // namespace

IoResult buildResult(Document& document, Builder& builder, int expanded,
                     const QString& versionNote)
{
    IoResult result = IoResult::success(document.entityCount());

    if (!versionNote.isEmpty()) result.addWarning(versionNote);
    if (expanded > 0) {
        result.addWarning(QStringLiteral("块引用已展开为独立图元（本软件不建模块定义，"
                                         "共 %1 个图元）").arg(expanded));
    }

    // 未建模的实体按类型计数上报，绝不静默丢弃
    for (auto it = builder.skippedTypes().constBegin();
         it != builder.skippedTypes().constEnd(); ++it) {
        result.addWarning(QStringLiteral("跳过 %1 个：%2").arg(it.value()).arg(it.key()));
    }
    return result;
}

} // namespace

IoResult readDwgWithLibDxfRw(const QString& path, Document& document)
{
    const QByteArray native = QFile::encodeName(path);

    document.clear(true);
    document.beginBatchLoad();

    dwgR reader(native.constData());
    Builder builder(document, Builder::Source::Dwg);
    const bool ok = reader.read(&builder, false);
    builder.finish();

    document.endBatchLoad();

    if (!ok) {
        const QString code = versionCode(reader.getVersion());
        return IoResult::failure(
            QStringLiteral("DWG 解析失败%1。该文件可能版本过旧或已损坏；"
                           "若确认版本在支持范围内（R14 ~ 2018），请反馈该文件以便排查。")
                .arg(code.isEmpty() ? QString()
                                    : QStringLiteral("（识别到版本 %1）").arg(code)));
    }

    // 版本信息不在这里报：调用方（DwgCodec）会给出带可读名称的版本说明，
    // 两处都报会让同一条信息在界面上出现两次。
    IoResult result = buildResult(document, builder, builder.expandedCount(), QString());
    document.markClean();

    // 图层可能被批量改写，统一通知一次
    document.notifyLayersChanged();
    return result;
}

IoResult readDxfWithLibDxfRw(const QString& path, Document& document)
{
    const QByteArray native = QFile::encodeName(path);

    document.clear(true);
    document.beginBatchLoad();

    dxfRW reader(native.constData());
    Builder builder(document, Builder::Source::Dxf);
    const bool ok = reader.read(&builder, false);
    builder.finish();

    document.endBatchLoad();

    if (!ok) {
        return IoResult::failure(QStringLiteral("DXF 解析失败（libdxfrw）。"));
    }

    IoResult result = buildResult(document, builder, builder.expandedCount(),
                                  QStringLiteral("由 libdxfrw 读取"));
    if (!builder.acadVersion().isEmpty()) {
        result.addWarning(QStringLiteral("源文件版本 %1").arg(builder.acadVersion()));
    }
    // EDA 导出的图纸往往不带图层颜色，按图层名启发式补色
    applyEdaLayerColorHeuristic(document, &result);
    document.markClean();
    document.notifyLayersChanged();
    return result;
}

// =============================================================================
//  DXF 写出（libdxfrw-modify）
//
//  写出协议：实现 DRW_Interface 的 write* 回调，dxfRW 负责组码编排、
//  区段顺序、句柄分配与 BLOCK_RECORD/OBJECTS 字典区。
//  图元映射的保真决策与此前自研写出器一致，差异点：
//    · 文字编码：不再写 \U+XXXX 转义，改为声明 ANSI_936 代码页由库转 GBK
//      （R12/R2000 均实测可被 ezdxf 正确读回）；
//    · 实测值 42（标注）不再写出，由读取端按定义点重推导。
// =============================================================================
namespace {

constexpr double kRadToDeg = 57.29577951308232;

/// 把 LyCAD 实体的公共属性搬到 DRW_Entity（图层/颜色/线型/线宽）
void applyCommonWrite(DRW_Entity& target, const Entity& source)
{
    target.layer = source.layerName().toStdString();
    target.color = source.isColorByLayer() ? 256 : source.colorIndex();
    // 62=256/6=ByLayer 与「不写该组码」语义相同，这里统一显式写出
    target.lineType = source.isLinetypeByLayer()
                          ? QStringLiteral("ByLayer").toStdString()
                          : linetypes::resolve(source.linetypeName()).name.toStdString();
    const int lw = source.lineweightDxf();
    target.lWeight = DRW_LW_Conv::dxfInt2lineWidth(lw < 0 ? -3 : lw);
    target.visible = source.isVisible();
}

void applyCoord(DRW_Coord& coord, const Point2D& p)
{
    coord.x = p.x;
    coord.y = p.y;
    coord.z = 0.0;
}

DRW_Coord toCoord(const Point2D& p)
{
    return DRW_Coord(p.x, p.y, 0.0);
}

DRW_Text::HAlign toDrwHAlign(TextEntity::HAlign align)
{
    switch (align) {
    case TextEntity::HAlign::Center: return DRW_Text::HCenter;
    case TextEntity::HAlign::Right:  return DRW_Text::HRight;
    case TextEntity::HAlign::Left:   break;
    }
    return DRW_Text::HLeft;
}

DRW_Text::VAlign toDrwVAlign(TextEntity::VAlign align)
{
    switch (align) {
    case TextEntity::VAlign::Bottom: return DRW_Text::VBottom;
    case TextEntity::VAlign::Middle: return DRW_Text::VMiddle;
    case TextEntity::VAlign::Top:    return DRW_Text::VTop;
    case TextEntity::VAlign::Baseline: break;
    }
    return DRW_Text::VBaseLine;
}

/// DXF 导出器：把 Document 翻译成 dxfRW 的写出回调
class Exporter final : public DRW_Interface {
public:
    Exporter(const Document& document, bool r2000)
        : m_doc(document), m_r2000(r2000) {}

    void setDxfRw(dxfRW* rw) { m_rw = rw; }

    /// 预扫描：为每个尺寸标注分配匿名块名（*D1、*D2……）
    void prescanDimensionBlocks()
    {
        int index = 0;
        for (EntityId id : m_doc.drawOrder()) {
            const Entity* entity = m_doc.entity(id);
            if (!entity || entity->type() != EntityType::Dimension) continue;
            m_dimBlocks.emplace(
                entity->id(),
                QStringLiteral("*D%1").arg(++index).toStdString());
        }
    }

    int hatchDowngrades() const noexcept { return m_hatchDowngrades; }

    // ---- 读侧接口（导出不使用） ----------------------------------------------
    void addHeader(const DRW_Header*) override {}
    void addLType(const DRW_LType&) override {}
    void addLayer(const DRW_Layer&) override {}
    void addDimStyle(const DRW_Dimstyle&) override {}
    void addVport(const DRW_Vport&) override {}
    void addTextStyle(const DRW_Textstyle&) override {}
    void addAppId(const DRW_AppId&) override {}
    void addBlock(const DRW_Block&) override {}
    void setBlock(const int) override {}
    void endBlock() override {}
    void addPoint(const DRW_Point&) override {}
    void addLine(const DRW_Line&) override {}
    void addRay(const DRW_Ray&) override {}
    void addXline(const DRW_Xline&) override {}
    void addArc(const DRW_Arc&) override {}
    void addCircle(const DRW_Circle&) override {}
    void addEllipse(const DRW_Ellipse&) override {}
    void addLWPolyline(const DRW_LWPolyline&) override {}
    void addPolyline(const DRW_Polyline&) override {}
    void addSpline(const DRW_Spline*) override {}
    void addKnot(const DRW_Entity&) override {}
    void addInsert(const DRW_Insert&) override {}
    void addTrace(const DRW_Trace&) override {}
    void add3dFace(const DRW_3Dface&) override {}
    void addSolid(const DRW_Solid&) override {}
    void addMText(const DRW_MText&) override {}
    void addText(const DRW_Text&) override {}
    void addDimAlign(const DRW_DimAligned*) override {}
    void addDimLinear(const DRW_DimLinear*) override {}
    void addDimRadial(const DRW_DimRadial*) override {}
    void addDimDiametric(const DRW_DimDiametric*) override {}
    void addDimAngular(const DRW_DimAngular*) override {}
    void addDimAngular3P(const DRW_DimAngular3p*) override {}
    void addDimOrdinate(const DRW_DimOrdinate*) override {}
    void addLeader(const DRW_Leader*) override {}
    void addHatch(const DRW_Hatch*) override {}
    void addViewport(const DRW_Viewport&) override {}
    void addImage(const DRW_Image*) override {}
    void linkImage(const DRW_ImageDef*) override {}
    void addComment(const char*) override {}

    // ---- 写侧 ----------------------------------------------------------------
    void writeHeader(DRW_Header& header) override
    {
        // $ACADVER/$HANDSEED 由库负责；$DWGCODEPAGE 决定写出编码，
        // 声明 ANSI_936 后库会把 UTF-8 字符串转成 GBK（简中环境的标准做法）。
        header.addStr("$DWGCODEPAGE", "ANSI_936", 3);

        header.addStr("$CLAYER", m_doc.layers().currentLayerName().toStdString(), 8);
        header.addInt("$CECOLOR", 256, 62);
        header.addStr("$CELTYPE", "ByLayer", 6);
        header.addDouble("$LTSCALE", 1.0, 40);
        header.addInt("$INSUNITS", 4, 70);        // 4 = 毫米
        header.addInt("$MEASUREMENT", 1, 70);     // 1 = 公制
        header.addInt("$LUNITS", 2, 70);          // 2 = 十进制
        header.addInt("$LUPREC", 4, 70);
        header.addInt("$AUNITS", 0, 70);
        header.addInt("$AUPREC", 0, 70);
        header.addInt("$PDMODE", 0, 70);
        header.addDouble("$PDSIZE", 0.0, 40);
        header.addDouble("$TEXTSIZE", 3.5, 40);
        header.addInt("$ORTHOMODE", 0, 70);
        header.addInt("$ATTMODE", 1, 70);

        // 全图范围（空图回退到 A4 横向尺寸）
        Box2D box;
        for (EntityId id : m_doc.drawOrder()) {
            if (const Entity* e = m_doc.entity(id)) {
                if (!e->isDisplayable(m_doc.layers())) continue;
                box.expand(e->boundingBox());
            }
        }
        const Point2D extMin = box.isValid() ? box.minCorner() : Point2D(0.0, 0.0);
        const Point2D extMax = box.isValid() ? box.maxCorner() : Point2D(420.0, 297.0);
        DRW_Coord minC; applyCoord(minC, extMin);
        DRW_Coord maxC; applyCoord(maxC, extMax);
        header.addCoord("$EXTMIN", minC, 10);
        header.addCoord("$EXTMAX", maxC, 10);
        header.addCoord("$LIMMIN", DRW_Coord(), 10);
        header.addCoord("$LIMMAX", maxC, 10);
    }

    void writeLTypes() override
    {
        // 库已自带 ByBlock/ByLayer/Continuous 三个必需项，
        // 这里只补文档实际引用的其它线型（按 resolve 后的规范名去重）。
        QSet<QString> referenced;
        const auto refer = [&](const QString& name) {
            if (name.isEmpty()
                || name.compare(QLatin1String("ByLayer"), Qt::CaseInsensitive) == 0
                || name.compare(QLatin1String("ByBlock"), Qt::CaseInsensitive) == 0
                || name.compare(QLatin1String("Continuous"), Qt::CaseInsensitive) == 0) {
                return;
            }
            referenced.insert(linetypes::resolve(name).name);
        };
        for (const Layer& layer : m_doc.layers().all()) refer(layer.linetype());
        for (EntityId id : m_doc.drawOrder()) {
            if (const Entity* e = m_doc.entity(id)) refer(e->linetypeName());
        }

        for (const QString& name : referenced) {
            const LinetypePattern pattern = linetypes::resolve(name);
            DRW_LType ltype;
            ltype.name = pattern.name.toStdString();
            ltype.desc = pattern.description.toStdString();
            ltype.size = static_cast<int>(pattern.pattern.size());
            ltype.length = pattern.cycleLength();
            ltype.path = pattern.pattern;
            m_rw->writeLineType(&ltype);
        }
    }

    void writeLayers() override
    {
        for (const Layer& layer : m_doc.layers().all()) {
            DRW_Layer record;
            record.name = layer.name().toStdString();
            record.flags = layer.flagsForDxf();
            // 图层关闭在 DXF 中以「颜色取负」表达
            int aci = layer.colorIndex();
            if (!layer.isVisible()) aci = -std::abs(aci);
            record.color = aci;
            record.lineType = linetypes::resolve(layer.linetype()).name.toStdString();
            const int lw = layer.lineweightDxf();
            record.lWeight = DRW_LW_Conv::dxfInt2lineWidth(lw < 0 ? -3 : lw);
            m_rw->writeLayer(&record);
        }
    }

    // 库会自动补 Standard 文字样式 / *ACTIVE 视口 / Standard 标注样式 / ACAD 应用程序
    void writeTextstyles() override {}
    void writeVports() override {}
    void writeDimstyles() override {}
    void writeAppId() override {}

    /// R2000 的 BLOCK_RECORD 表：每个匿名块都要在此登记，写出块体时才能对上句柄
    void writeBlockRecords() override
    {
        if (!m_r2000) return;
        for (const auto& [id, blockName] : m_dimBlocks) {
            Q_UNUSED(id);
            m_rw->writeBlockRecord(blockName);
        }
    }

    void writeBlocks() override
    {
        for (const auto& [id, blockName] : m_dimBlocks) {
            Q_UNUSED(id);
            writeDimensionBlock(blockName);
        }
    }

    void writeEntities() override
    {
        for (EntityId id : m_doc.drawOrder()) {
            const Entity* entity = m_doc.entity(id);
            if (!entity) continue;
            switch (entity->type()) {
            case EntityType::Line:      writeLine(*static_cast<const LineEntity*>(entity)); break;
            case EntityType::Circle:    writeCircle(*static_cast<const CircleEntity*>(entity)); break;
            case EntityType::Arc:       writeArc(*static_cast<const ArcEntity*>(entity)); break;
            case EntityType::Polyline:  writePolyline(*static_cast<const PolylineEntity*>(entity)); break;
            case EntityType::Point:     writePoint(*static_cast<const PointEntity*>(entity)); break;
            case EntityType::Text:      writeText(*static_cast<const TextEntity*>(entity)); break;
            case EntityType::Hatch:     writeHatch(*static_cast<const HatchEntity*>(entity)); break;
            case EntityType::Dimension: writeDimension(*static_cast<const DimensionEntity*>(entity)); break;
            default: break;
            }
        }
    }

private:
    void writeDimensionBlock(const std::string& name)
    {
        const DimensionEntity* dim = nullptr;
        for (const auto& [id, blockName] : m_dimBlocks) {
            if (blockName == name) {
                dim = static_cast<const DimensionEntity*>(m_doc.entity(id));
                break;
            }
        }
        if (!dim) return;

        DRW_Block block;
        block.name = name;
        block.flags = 1;                          // bit 1 = 匿名块
        applyCoord(block.basePoint, Point2D(0.0, 0.0));
        m_rw->writeBlock(&block);

        // 块内容约定：图层 0 + 颜色 ByBlock(0)，随 DIMENSION 实体着色
        const DimensionEntity::Geometry geo = dim->geometry();
        if (!geo.valid) return;

        auto beginBlockEntity = [&](DRW_Entity& entity) {
            entity.layer = "0";
            entity.color = 0;                     // ByBlock
            entity.lineType = "ByLayer";
            entity.lWeight = DRW_LW_Conv::dxfInt2lineWidth(-3);
        };

        for (const auto& seg : geo.segments) {
            DRW_Line line;
            beginBlockEntity(line);
            applyCoord(line.basePoint, seg.first);
            applyCoord(line.secPoint, seg.second);
            m_rw->writeLine(&line);
        }

        for (const DimensionEntity::Arrow& arrow : geo.arrows) {
            DRW_Solid solid;
            beginBlockEntity(solid);
            applyCoord(solid.basePoint, arrow.tip);
            applyCoord(solid.secPoint, arrow.base1);
            applyCoord(solid.thirdPoint, arrow.base2);
            // SOLID 的顶点顺序有历史怪癖：第 3、4 个顶点相互对调。
            // 三角形以「重复第 3 点」的方式补成四点，顺序问题随之消解。
            applyCoord(solid.fourPoint, arrow.base2);
            m_rw->writeSolid(&solid);
        }

        if (!geo.text.isEmpty()) {
            DRW_Text text;
            beginBlockEntity(text);
            text.text = dim->displayText(/*dxfStyle=*/true).toStdString();
            applyCoord(text.basePoint, geo.textAnchor);
            text.height = geo.textHeight;
            text.angle = geo.textAngle * kRadToDeg;
            text.alignH = DRW_Text::HCenter;
            text.alignV = DRW_Text::VMiddle;
            applyCoord(text.secPoint, geo.textAnchor);
            m_rw->writeText(&text);
        }
    }

    void writeLine(const LineEntity& line)
    {
        DRW_Line record;
        applyCommonWrite(record, line);
        applyCoord(record.basePoint, line.start());
        applyCoord(record.secPoint, line.end());
        m_rw->writeLine(&record);
    }

    void writeCircle(const CircleEntity& circle)
    {
        DRW_Circle record;
        applyCommonWrite(record, circle);
        applyCoord(record.basePoint, circle.center());
        record.radious = circle.radius();
        m_rw->writeCircle(&record);
    }

    void writeArc(const ArcEntity& arc)
    {
        DRW_Arc record;
        applyCommonWrite(record, arc);
        applyCoord(record.basePoint, arc.center());
        record.radious = arc.radius();
        record.staangle = arc.startAngle();       // 弧度直传（写出时库再转度）
        record.endangle = arc.endAngle();
        m_rw->writeArc(&record);
    }

    void writePoint(const PointEntity& point)
    {
        DRW_Point record;
        applyCommonWrite(record, point);
        applyCoord(record.basePoint, point.position());
        m_rw->writePoint(&record);
    }

    void writePolyline(const PolylineEntity& polyline)
    {
        if (m_r2000) {
            DRW_LWPolyline record;
            applyCommonWrite(record, polyline);
            record.flags = polyline.isClosed() ? 1 : 0;
            record.width = polyline.constantWidth();
            for (const PolylineEntity::Vertex& v : polyline.vertices()) {
                DRW_Vertex2D* vertex = new DRW_Vertex2D(v.point.x, v.point.y, v.bulge);
                if (record.width <= kEpsilon) {
                    vertex->stawidth = v.startWidth;
                    vertex->endwidth = v.endWidth;
                }
                record.vertlist.push_back(vertex);
            }
            m_rw->writeLWPolyline(&record);
        } else {
            DRW_Polyline record;
            applyCommonWrite(record, polyline);
            record.flags = polyline.isClosed() ? 1 : 0;
            record.defstawidth = polyline.constantWidth();
            record.defendwidth = polyline.constantWidth();
            for (const PolylineEntity::Vertex& v : polyline.vertices()) {
                DRW_Vertex* vertex = new DRW_Vertex(v.point.x, v.point.y, 0.0, v.bulge);
                if (polyline.constantWidth() <= kEpsilon) {
                    vertex->stawidth = v.startWidth;
                    vertex->endwidth = v.endWidth;
                }
                record.vertlist.push_back(vertex);
            }
            m_rw->writePolyline(&record);
        }
    }

    void writeText(const TextEntity& text)
    {
        if (text.displayString().isEmpty()) {
            // 空文字不是合法的 DXF 实体，跳过而非产出坏文件
            return;
        }
        DRW_Text record;
        applyCommonWrite(record, text);
        applyCoord(record.basePoint, text.position());
        record.secPoint = record.basePoint;
        record.text = text.displayString().toStdString();
        record.height = text.height();
        record.angle = text.rotation() * kRadToDeg;
        record.widthscale = text.widthFactor();
        record.style = "Standard";
        record.textgen = (text.isBackwards() ? 2 : 0) | (text.isUpsideDown() ? 4 : 0);
        if (text.usesAlignmentPoint()) {
            record.alignH = toDrwHAlign(text.hAlign());
            record.alignV = toDrwVAlign(text.vAlign());
            applyCoord(record.secPoint, text.alignmentPoint());
        }
        m_rw->writeText(&record);
    }

    void writeHatch(const HatchEntity& hatch)
    {
        if (!m_r2000) {
            // HATCH 实体为 R14 引入：降级写出闭合边界多段线，保证几何信息不丢失
            if (hatch.vertexCount() >= 2) {
                DRW_Polyline record;
                applyCommonWrite(record, hatch);
                record.flags = 1;                     // 闭合
                for (std::size_t i = 0; i < hatch.vertexCount(); ++i) {
                    record.vertlist.push_back(new DRW_Vertex(
                        hatch.vertex(i).x, hatch.vertex(i).y, 0.0, hatch.vertexBulge(i)));
                }
                m_rw->writePolyline(&record);
            }
            ++m_hatchDowngrades;
            return;
        }
        if (hatch.vertexCount() < 3) return;      // 无法构成闭合边界

        DRW_Hatch record;
        applyCommonWrite(record, hatch);
        applyCoord(record.basePoint, Point2D(0.0, 0.0));
        record.extPoint = DRW_Coord(0.0, 0.0, 1.0);
        record.name = (hatch.patternName().isEmpty() ? QStringLiteral("SOLID")
                                                     : hatch.patternName()).toStdString();
        record.solid = hatch.isSolid() ? 1 : 0;
        record.associative = 0;
        record.hstyle = 0;
        record.hpattern = 1;                      // 预定义图案
        record.angle = hatch.patternAngle() * kRadToDeg;
        record.scale = hatch.patternScale();
        record.deflines = 0;                      // 预定义图案由读取端自带的定义生成

        DRW_HatchLoop* loop = new DRW_HatchLoop(1 | 2);   // 外部环 + 多段线路径
        DRW_LWPolyline* boundary = new DRW_LWPolyline;
        boundary->flags = 1;                      // 闭合
        for (std::size_t i = 0; i < hatch.vertexCount(); ++i) {
            boundary->addVertex(DRW_Vertex2D(hatch.vertex(i).x, hatch.vertex(i).y,
                                             hatch.vertexBulge(i)));
        }
        loop->objlist.push_back(boundary);
        record.appendLoop(loop);
        m_rw->writeHatch(&record);
    }

    void writeDimension(const DimensionEntity& dim)
    {
        const DimensionEntity::Geometry geo = dim.geometry();
        const auto blockIt = m_dimBlocks.find(dim.id());

        std::unique_ptr<DRW_Dimension> record;
        switch (dim.dimType()) {
        case DimensionEntity::DimType::Linear: {
            auto linear = std::make_unique<DRW_DimLinear>();
            linear->setAngle(dim.rotation() * kRadToDeg);            // DXF 50 为度
            linear->setDefPoint(toCoord(dim.dimLinePoint()));        // 尺寸线位置
            linear->setDef1Point(toCoord(dim.firstPoint()));         // 第一界线原点
            linear->setDef2Point(toCoord(dim.secondPoint()));        // 第二界线原点
            record = std::move(linear);
            break;
        }
        case DimensionEntity::DimType::Aligned: {
            auto aligned = std::make_unique<DRW_DimAligned>();
            aligned->setDefPoint(toCoord(dim.dimLinePoint()));
            aligned->setDef1Point(toCoord(dim.firstPoint()));
            aligned->setDef2Point(toCoord(dim.secondPoint()));
            record = std::move(aligned);
            break;
        }
        case DimensionEntity::DimType::Radial: {
            auto radial = std::make_unique<DRW_DimRadial>();
            radial->setDefPoint(toCoord(dim.firstPoint()));          // 圆心
            radial->setDiameterPoint(toCoord(dim.secondPoint()));    // 圆上定义点
            record = std::move(radial);
            break;
        }
        case DimensionEntity::DimType::Diametric: {
            auto diametric = std::make_unique<DRW_DimDiametric>();
            diametric->setDefPoint(toCoord(dim.firstPoint()));
            diametric->setDiameter1Point(toCoord(dim.secondPoint()));
            record = std::move(diametric);
            break;
        }
        default:
            return;
        }

        applyCommonWrite(*record, dim);
        if (blockIt != m_dimBlocks.end()) record->setName(blockIt->second);
        record->setTextPoint(toCoord(geo.textAnchor));
        // bit 32 表示「几何块引用已在文件中」，读取端无需自行重绘
        record->type = static_cast<int>(dim.dimType()) | 32;
        record->setMeasurement(dim.measurement());
        record->setStyle("Standard");
        if (!dim.textOverride().isEmpty() && dim.textOverride() != QLatin1String("<>")) {
            record->setText(dim.textOverride().toStdString());
        }
        m_rw->writeDimension(record.release());
    }

    const Document& m_doc;
    dxfRW* m_rw = nullptr;
    bool m_r2000 = false;
    std::map<EntityId, std::string> m_dimBlocks;
    int m_hatchDowngrades = 0;
};

} // namespace

IoResult writeDxfWithLibDxfRw(const QString& path, const Document& document,
                              const WriteOptions& options)
{
    // 版本归一化：仅接受本写出路径支持的两个目标版本
    const bool r2000 = options.version.compare(QLatin1String("AC1009"),
                                               Qt::CaseInsensitive) != 0;
    const DRW::Version version = r2000 ? DRW::AC1015 : DRW::AC1009;

    Exporter exporter(document, r2000);
    exporter.prescanDimensionBlocks();

    const QByteArray native = QFile::encodeName(path);
    dxfRW writer(native.constData());
    exporter.setDxfRw(&writer);

    const bool ok = writer.write(&exporter, version, /*bin=*/false);
    if (!ok) {
        return IoResult::failure(QStringLiteral("DXF 写出失败（libdxfrw）。"));
    }

    IoResult result = IoResult::success(document.entityCount());
    if (const int downgrades = exporter.hatchDowngrades(); downgrades > 0) {
        result.addWarning(QStringLiteral("%1 个填充图元在 R12 中降级为闭合多段线（R12 不支持 HATCH 实体）")
                              .arg(downgrades));
    }
    return result;
}

} // namespace lycad
