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
//    TEXT 旋转          不换算（度）           不换算（弧度）   **单位不同，按来源换算**
//    MTEXT 旋转         取自 11 组码方向向量    同左            由方向向量算 atan2，不用 angle
//    DIMENSION 角度      不换算（度）           ×ARAD（得度）    两者都是度，需转为弧度
//
//  实测依据（720 个真实 DWG 文件，共 29018 个弧角度样本）：
//    · 出现次数最多的取值是 3.141593(π) ×1234、4.712389(3π/2) ×1069、
//      1.570796(π/2) ×1003；而「接近整数度」的样本数为 **0** —— 说明 DWG 路径下
//      弧角度是弧度。
//    · 文字旋转在 DWG 中恰为 1.570796(π/2)，在 DXF 中则为 90.0 —— 两者单位不同。
//    · INSERT 旋转出现 6.021386，正好是 23π/12（即 345.000°）—— 弧度。
//
//  因此 textRotationRadians() 里那条按来源分支的换算是必需的，不是保险措施；
//  删掉它会让 DWG 里的 TEXT 旋转全部错 57 倍，而 DXF 里的一切正常——这类缺陷很难
//  在使用中归因。MTEXT 走另一条路：它的旋转只写在 11 组码的 X 轴方向向量里，
//  与 50 组码的单位之争无关，两条路径都按方向向量算。注意 DWG 的 MTEXT 方向向量
//  依赖 libdxfrw 置位 haveXAxis——曾漏置，导致 DWG 里旋转过的 MTEXT 全部变水平
//  （见 addMText 内的说明）。
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
#include "core/Block.h"
#include "core/CircleEntity.h"
#include "core/DimensionEntity.h"
#include "core/Document.h"
#include "core/EllipseEntity.h"
#include "core/HatchEntity.h"
#include "core/InfiniteLineEntity.h"
#include "core/InsertEntity.h"
#include "core/Layer.h"
#include "core/Linetype.h"
#include "core/LineEntity.h"
#include "core/MTextEntity.h"
#include "core/PointEntity.h"
#include "core/PolylineEntity.h"
#include "core/RayEntity.h"
#include "core/SplineEntity.h"
#include "core/TableEntity.h"
#include "core/TextEntity.h"
#include "core/Types.h"
#include "core/XLineEntity.h"

// libdxfrw（第三方）。这些头文件只在本文件出现，不外溢到其它翻译单元。
#include "drw_entities.h"
#include "drw_interface.h"
#include "drw_objects.h"
#include "libdwgrw.h"
#include "libdxfrw.h"

#include <QDebug>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

namespace lycad {
namespace {

/// 图元坐标的合理上限（世界单位）。真实图纸——哪怕以毫米计的超大总图——都远小于
/// 它；而二进制格式里读错一个字节造出的天文数字（实测 1e36、1e101）会被挡住。
/// 见 Builder::place() 的坐标体检。
constexpr double kMaxCoordinate = 1e12;

inline Point2D toPoint(const DRW_Coord& c) noexcept
{
    return Point2D(c.x, c.y);
}

/// 方向向量（构造线/射线的 11 组码）：只取 X/Y，Z 分量在二维图元里没有意义
inline Vector2D toVector(const DRW_Coord& c) noexcept
{
    return Vector2D(c.x, c.y);
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

/// 是否为标注的匿名几何块（*D1、*D2……）。
///
/// 这类块只是「让不具备标注重绘能力的旧软件也能显示」的缓存几何；本软件按
/// DIMENSION 的定义点实时重推导标注几何（见 DimensionEntity::geometry），
/// 读进来既无用，写出时还会和自己重新生成的匿名块撞名。
bool isDimensionBlock(const QString& name)
{
    if (!name.startsWith(QLatin1Char('*'))) return false;
    if (name.size() < 3) return false;
    return name.at(1) == QLatin1Char('D') || name.at(1) == QLatin1Char('d');
}

/// 是否为表格的匿名几何块（*T1、*T2……）。
///
/// ACAD_TABLE 的可见几何（网格线、单元格文字）就存在这种块里，实体本身只是
/// 一个指向它的块参照。本软件建了原生 TableEntity，因此这类块的内容在读取期
/// 被解析成表格的行列结构，而不是当作普通块展开。
bool isTableBlock(const QString& name)
{
    if (!name.startsWith(QLatin1Char('*'))) return false;
    if (name.size() < 3) return false;
    return name.at(1) == QLatin1Char('T') || name.at(1) == QLatin1Char('t');
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

/// 「可写出版本」的签名码 -> DRW::Version；无法识别时返回 UNKNOWNV。
///
/// 刻意**不含 AC1009 (R12)**：R12 缺 HATCH/SPLINE/ELLIPSE/XLINE/RAY，
/// 也没有 TrueType 与 MTEXT，写出时得整体降级改写。那条降级通道已随
/// 「移除 R12 导出」一并删除，因此这里也不再把 AC1009 映射成可写目标——
/// 否则直接调用本层的人会写出一个「R12 文件里装着 R13+ 实体」的四不像。
/// 未知（含 AC1009）一律由调用方退回默认的可写版本。
DRW::Version writableVersionFromCode(const QString& code)
{
    static const std::pair<const char*, DRW::Version> kTable[] = {
        { "AC1015", DRW::AC1015 }, { "AC1018", DRW::AC1018 },
        { "AC1021", DRW::AC1021 }, { "AC1024", DRW::AC1024 },
        { "AC1027", DRW::AC1027 }, { "AC1032", DRW::AC1032 },
    };
    for (const auto& entry : kTable) {
        if (code.compare(QLatin1String(entry.first), Qt::CaseInsensitive) == 0) {
            return entry.second;
        }
    }
    return DRW::UNKNOWNV;
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

    /// 降级保留的实体类型 -> 个数
    const QHash<QString, int>& downgradedTypes() const noexcept { return m_downgraded; }

    /// 读入的块定义个数（供导入提示显示）
    std::size_t blockCount() const noexcept { return m_resolvedBlocks.size(); }

    /// 文件头里的 $ACADVER（如 "AC1015"），未携带时为空
    QString acadVersion() const { return m_acadVersion; }

    // ---- 表与头 --------------------------------------------------------------
    void addHeader(const DRW_Header* data) override;
    void addLType(const DRW_LType&) override {}
    void addLayer(const DRW_Layer& data) override;
    void addDimStyle(const DRW_Dimstyle&) override {}
    void addVport(const DRW_Vport&) override {}
    void addTextStyle(const DRW_Textstyle& data) override;
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
    /// 属性定义（ATTDEF）：只出现在块定义里，进块定义的属性表而不是图元表
    void addAttdef(const DRW_Attdef& data) override;
    /// 属性值（ATTRIB）：紧跟在 INSERT 之后，挂到刚读到的那个块参照上
    void addAttrib(const DRW_Attrib& data) override;

    // ---- 图元 ----------------------------------------------------------------
    void addPoint(const DRW_Point& data) override;
    void addLine(const DRW_Line& data) override;
    void addRay(const DRW_Ray& data) override;
    void addXline(const DRW_Xline& data) override;
    void addArc(const DRW_Arc& data) override;
    void addCircle(const DRW_Circle& data) override;
    void addEllipse(const DRW_Ellipse& data) override;
    void addLWPolyline(const DRW_LWPolyline& data) override;
    void addPolyline(const DRW_Polyline& data) override;
    void addSpline(const DRW_Spline* data) override;
    void addKnot(const DRW_Entity&) override {}
    void addInsert(const DRW_Insert& data) override;
    /// ACAD_TABLE：几何在其引用的 *Tnn 块里，本体的 AcDbTable 段落带完整
    /// 行列结构（行列数、行列高宽、单元格文字、合并）。优先用本体数据还原，
    /// 本体数据缺失时再退回几何块反推。
    void addTable(const DRW_Table& data) override { noteTable(data); }
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
    void addDimOrdinate(const DRW_DimOrdinate* data) override;
    void addDimArc(const DRW_DimArc* data) override;

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
    /// 一个块定义在读取期暂存的形态。
    ///
    /// 块引用（INSERT）此刻还不能变成 InsertEntity——它要指向的块定义可能出现在
    /// 文件更靠后的位置（DWG 的对象流完全不保证顺序）。因此定义先攒起来，
    /// 等全部读完（finish）再统一解析成 BlockDefinition + InsertEntity。
    struct BlockDef {
        QString name;
        Point2D base;
        int     flags = 0;
        /// 这是表格的匿名几何块（*Tnn）：收尾时按行列结构还原成 TableEntity，
        /// 而不是注册为普通块定义。
        bool    isTableGeometry = false;
        std::vector<std::unique_ptr<Entity>> entities;
        std::vector<std::unique_ptr<InsertEntity>> inserts;
        std::vector<AttributeDefinition> attributeDefs;
    };

    /// 把图元放到正确的位置：块内 or 模型空间
    void place(std::unique_ptr<Entity> entity);
    /// 记录一个块引用（先建 InsertEntity，块定义留到 finish 解析）
    void noteInsert(const DRW_Insert& data, const DRW_Entity& source);
    /// 记录一个 ACAD_TABLE。几何在其引用的 *Tnn 匿名块里，留到 finish 解析。
    void noteTable(const DRW_Table& data);
    /// 记录一个指向表格几何块（*Tnn）的普通块参照——本软件写出的表格走这条路。
    void noteTableRef(const DRW_Insert& data);
    /// 取一个尚未被认领的表格几何块（DWG/R2000 表格本体无法解析时的配对兜底）。
    const BlockDef* takeUnclaimedTableGeometry();
    void skip(const QString& type);
    /// 降级保留：实体换了形式但内容没丢（如 MTEXT 按单行文字导入），
    /// 与「跳过」分开计数——否则用户会以为文字被丢掉了。
    void downgrade(const QString& what);

    /// 套用图层/颜色/线型/线宽等公共属性
    void applyCommon(Entity& entity, const DRW_Entity& source);

    /// TEXT 的旋转角（弧度）。
    ///
    /// libdxfrw 两条读取路径对同一个 `angle` 字段用的单位不同：DXF 路径原样返回
    /// 50 组码（度），DWG 路径存的是弧度（见本文件头部实测定标）。这是整个文件里
    /// 唯一需要按来源分单位的地方。
    static double textRotationRadians(const DRW_Text& data, Source source)
    {
        return (source == Source::Dxf) ? degToRad(data.angle) : data.angle;
    }

    /// rotationRadians 由调用方给出（弧度）——MTEXT 的旋转不是从 50 组码来的，
    /// 而是从 11 组码的 X 轴方向向量算出来的，必须由调用方自己决定。
    std::unique_ptr<TextEntity> makeText(const DRW_Text& data, double rotationRadians);
    void applyHatchBoundary(const DRW_Hatch& data);

    /// 读取收尾：把暂存的块定义装配进文档、解析块引用、汇总警告。
    /// 把若干暂存的块引用接到文档（或某个块定义）上；解析不到定义的按缺失计数。
    void attachInserts(std::vector<std::unique_ptr<InsertEntity>>&& inserts,
                       const std::shared_ptr<BlockDefinition>& owner);
    std::shared_ptr<BlockDefinition> resolveBlock(const QString& name);
    /// 把 DWG 路径攒下的 ATTRIB 按「属主 + 标签」回填到对应块参照上
    void attachPendingAttribs();

    Document& m_doc;
    Source    m_source;

    std::map<QString, BlockDef> m_blocks;         ///< 归一化名 -> 定义
    std::map<QString, std::shared_ptr<BlockDefinition>> m_resolvedBlocks;
    /// 正在装配的块定义链（归一化名）。用于**掐断循环块引用**：A 里插 B、
    /// B 里又插 A 时，InsertEntity 会持有对方的 shared_ptr，而对方又拥有这个
    /// 块参照——引用计数永远降不到 0，整块定义连同它的图元一起泄漏。
    /// 现实中的 CAD 会拒绝建立循环引用，但手改过或损坏的文件里存在这种结构。
    std::vector<QString> m_resolveChain;
    std::map<duint32, QString> m_blockNameByHandle;
    BlockDef* m_currentBlock = nullptr;           ///< 正在读取的块（模型空间为 null）
    /// 正在读取一个**被忽略**的块（*Dnn 标注几何块）。它的图元要就地丢弃，
    /// 不能因为「没有 currentBlock」而掉进模型空间——那会让标注的显示几何
    /// 在图纸里多出一整套（实测某个 4 个标注的样图凭空多出 20 个图元）。
    bool m_discardBlock = false;

    std::vector<std::unique_ptr<InsertEntity>> m_modelInserts;
    /// 刚读到的块参照：紧随其后的 ATTRIB 都挂在它上面（DXF 路径）
    InsertEntity* m_lastInsert = nullptr;

    /// 待解析的表格。两种来源：
    ///   1) 真·ACAD_TABLE 实体：本体自带完整行列结构（m_native* 有值），
    ///      几何块仅作显示缓存，还原时以本体为准；
    ///   2) 指向 *Tnn 几何块的普通块参照（本软件写出的表格）：本体无数据，
    ///      只能从几何块反推行列结构。
    struct TableRef {
        QString blockName;
        Point2D insertion;
        double  rotation = 0.0;
        double  scaleX = 1.0;
        double  scaleY = 1.0;
        QString layer;

        // ---- 真·ACAD_TABLE 本体携带的行列结构（几何反推路径下为空）----
        bool    hasNativeData = false;
        int     nativeRows = 0;
        int     nativeCols = 0;
        std::vector<double> nativeRowHeights;
        std::vector<double> nativeColWidths;
        std::vector<std::tuple<int, int, QString, int>> nativeCells;
        std::vector<TableMergeRange> nativeMerges;
        QString nativeTableStyle;
    };
    std::vector<TableRef> m_tables;
    /// 已被某个表格认领的几何块名（归一化键）——配对兜底时避免一块被两表抢用。
    std::set<QString> m_claimedTableGeometry;

    /// 收尾：把 m_tables 里记录的表格引用，按各自的 *Tnn 几何块还原成 TableEntity
    void finishTables();
    /// 用 ACAD_TABLE 本体携带的行列结构建表（优先路径）
    std::unique_ptr<TableEntity> buildTableFromNative(const TableRef& ref) const;
    /// 从几何块（网格线 + 文字）反推表格的行列结构
    std::unique_ptr<TableEntity> buildTableFromGeometry(const TableRef& ref,
                                                        const BlockDef& def) const;
    /// 本体结构有效但 cells 为空（DWG 写出端编码未完全逆向）时，
    /// 从配对的 *Tnn 几何块把单元格文字按格归位补回，并反推合并区
    void mergeTableCellTexts(TableEntity& table, const TableRef& ref,
                             const BlockDef& def) const;

    /// DWG 路径的 ATTRIB 是**独立对象**，不跟在 INSERT 后面，只能靠「同一个属主
    /// （块记录）+ 标签命中该块的属性定义」回填。读取期块定义还没装配好（属性
    /// 定义在 finish 才解析），所以先把它们攒起来，收尾时按标签逐条配对。
    struct PendingAttrib {
        Attribute attribute;
        duint32   owner = 0;          ///< 所属块记录句柄
    };
    std::vector<PendingAttrib> m_pendingAttribs;
    /// 属主句柄 -> 该空间里的块参照（按读取顺序）
    std::map<duint32, std::vector<InsertEntity*>> m_insertsByOwner;

    QHash<QString, int> m_skipped;
    QHash<QString, int> m_downgraded;
    QString m_acadVersion;
    int m_missingBlocks = 0;
    int m_cyclicBlocks = 0;
    int m_orphanAttribs = 0;
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
    } else if (source.color == aci::kByBlock) {
        // 0 = 随块：块内图元取块参照的颜色。必须如实保留，
        // 否则块定义一旦被不同颜色的块参照引用，整块颜色就都错了。
        entity.setColorByBlock();
    } else {
        entity.setColorByLayer();     // 256 = 随层
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
    // 只有紧跟在 INSERT 之后的 ATTRIB 属于那个块参照。这里清掉「刚读到的块参照」，
    // 于是中间夹了任何别的实体时，后面的 ATTRIB 不会被错挂到上一个块参照上。
    m_lastInsert = nullptr;
    if (!entity) return;

    // 坐标体检：二进制格式里读错一个字节就能造出 1e100 这种坐标，混进来会把
    // 全图范围撑到天文数字——用户看到的又是「打开是空白」（缩放范围失效），
    // 拾取与包围盒判断也跟着失真。实测某 2007 版 DWG 有 2 条样条各一个控制点
    // 如此，几何已不可用。这里挡在入口：非有限值、或超出任何真实图纸量级的一律
    // 跳过并计数上报，不静默丢弃（与 README 的约定一致）。
    //
    // 无界图元（构造线、射线）的范围是无穷，包围盒必然是无效盒；这类图元改以
    // 其定义点做同一套体检，否则它们会被下面这条判据整批当作坏数据丢掉。
    Box2D box = entity->boundingBox();
    if (!box.isValid()) {
        const std::optional<Point2D> defined = entity->definitionPoint();
        if (!defined) {
            skip(QStringLiteral("坐标超出范围（文件数据异常，已跳过）"));
            return;
        }
        box = Box2D::aroundPoint(*defined, 0.0);
    }

    const auto outOfRange = [](double v) {
        return !std::isfinite(v) || std::abs(v) > kMaxCoordinate;
    };
    if (outOfRange(box.minX) || outOfRange(box.minY)
        || outOfRange(box.maxX) || outOfRange(box.maxY)) {
        skip(QStringLiteral("坐标超出范围（文件数据异常，已跳过）"));
        return;
    }

    if (m_discardBlock) return;      // 被忽略的块（*Dnn）：内容一概不留
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

void Builder::downgrade(const QString& what)
{
    m_downgraded[what] = m_downgraded.value(what, 0) + 1;
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

void Builder::addTextStyle(const DRW_Textstyle& data)
{
    const QString name = toQString(data.name).trimmed();
    if (name.isEmpty()) return;

    // 整条替换而不是逐字段覆盖：表里预置的 Standard 带中文兜底字体，
    // 逐字段覆盖会让「文件只写了 TrueType 族名（3 组码为空）」的样式
    // 白白继承那份兜底字体，导出后字体就不是原文件的了。
    TextStyle imported(name);
    imported.setFontFile(toQString(data.font));
    imported.setBigFontFile(toQString(data.bigFont));

    // 度量字段（DXF 40/41/50）必须随样式走：真实 CAD 用**样式级**宽度因子
    // 压缩字形（中文图纸的 Standard 常是 0.8），丢掉它，重存的文件在
    // 浩辰/AutoCAD 里所有文字都会按 1.0 重排，图面上文字整体错位。
    // 文件里 41 为 0 表示「未设置」，按默认 1.0 归一。
    imported.setWidthFactor(data.width > 0.0 ? data.width : 1.0);
    imported.setFixedHeight(data.height);
    imported.setOblique(data.oblique);

    // TrueType 字体：AutoCAD 把「从字体列表里选中的族名」写进样式的扩展数据
    // （1001 <应用名> / 1000 <族名> / 1071 <族标志>），此时 3/4 组码都是空的——
    // 字体信息**只**存在于扩展数据里。中文图纸的 Standard 几乎都是这种形态，
    // 不读它，导出的汉字就没有字形（CAD 会退回不含汉字的 txt.shx，整片 "?"）。
    //
    // 应用名不统一（AutoCAD 写 ACAD，也有写 _APP0 的），所以不认应用名，
    // 只取**最后一个应用块**里的 1000：注记样式的扩展数据里还夹着一条
    // AcadAnnotative / "AnnotativeData"，取最后一个块才不会把标记当成字体。
    // 字体族标志（1071）被 libdxfrw 解析进了 fontFamily 字段，不在 extData 里。
    QString blockFontName;
    for (DRW_Variant* variant : data.extData) {
        if (!variant) continue;
        if (variant->code() == 1001) {
            blockFontName.clear();
        } else if (variant->code() == 1000 && variant->type() == DRW_Variant::STRING) {
            blockFontName = toQString(*variant->content.s);
        }
    }
    if (!blockFontName.isEmpty()) imported.setTtfName(blockFontName);
    imported.setTtfFlags(data.fontFamily);
    // DWG 路径下 1071（字体族标志）经由 EED 以扩展数据变体带回，
    // fontFamily 字段只反映 DXF 直读的情形
    if (data.fontFamily == 0) {
        for (DRW_Variant* variant : data.extData) {
            if (variant && variant->code() == 1071) {
                imported.setTtfFlags(variant->content.i);
                break;
            }
        }
    }

    if (!imported.hasFontInfo()) {
        // 文件明确给了这条样式，却没给任何字体（既无 3/4 组码，也无 TrueType
        // 扩展数据）：用中文安全字体兜底，让汉字至少画得出来。
        imported.setFontFile(TextStyleTable::cjkSafeStandard().fontFile());
        imported.setBigFontFile(TextStyleTable::cjkSafeStandard().bigFontFile());
    }

    m_doc.textStyles().ensure(name) = imported;
}

// -----------------------------------------------------------------------------
//  块
//
//  块定义**不展开**：定义原样收集进 BlockDef，块引用建成 InsertEntity 并指向
//  对应定义（引用语义）。这样文件往返后 BLOCKS 段与 INSERT 都还在原处，
//  分解（EXPLODE）由用户在需要时显式触发。
// -----------------------------------------------------------------------------
void Builder::addBlock(const DRW_Block& data)
{
    const QString name = toQString(data.name);
    if (name.isEmpty()) return;

    // 模型空间 / 图纸空间是容器，不是块定义；它们的内容由读取器直接按实体流回调。
    // 其余块（含 *U、*D1 等匿名块）都是真正的定义。
    if (isSpaceBlock(name)) {
        m_currentBlock = nullptr;
        m_discardBlock = false;
        m_blockNameByHandle[data.handle] = name;
        return;
    }

    // 尺寸标注的匿名几何块（*Dnn）仅作显示用途——本软件按定义点重推导标注几何，
    // 收下它们只会在写出时与重新生成的匿名块重名，其内容因此整块丢弃。
    if (isDimensionBlock(name)) {
        m_currentBlock = nullptr;
        m_discardBlock = true;
        m_blockNameByHandle[data.handle] = name;
        return;
    }

    const QString key = BlockTable::normalizedKey(name);
    BlockDef& block = m_blocks[key];
    block.name = name;
    block.base = toPoint(data.basePoint);
    block.flags = data.flags;
    // 表格的匿名几何块（*Tnn）：内容要读（用来还原行列结构），但它不是用户
    // 意义上的块定义——收尾时会被转成 TableEntity，不进文档的块表。
    block.isTableGeometry = isTableBlock(name);
    m_currentBlock = &block;
    m_discardBlock = false;
    m_blockNameByHandle[data.handle] = name;
}

void Builder::setBlock(const int handle)
{
    // DWG 路径用句柄切换当前块；句柄未知或为 0 表示回到模型空间。
    const auto it = m_blockNameByHandle.find(handle);
    if (it == m_blockNameByHandle.end() || isSpaceBlock(it->second)
        || isDimensionBlock(it->second)) {
        m_currentBlock = nullptr;
        m_discardBlock = (it != m_blockNameByHandle.end()) && isDimensionBlock(it->second);
        return;
    }

    const auto block = m_blocks.find(BlockTable::normalizedKey(it->second));
    m_currentBlock = (block == m_blocks.end()) ? nullptr : &block->second;
    m_discardBlock = false;
}

void Builder::endBlock()
{
    m_currentBlock = nullptr;
    m_discardBlock = false;
}

void Builder::addAttdef(const DRW_Attdef& data)
{
    // ATTDEF 只在块定义里有意义：它是块参照上待填的文字占位符，本身不作为
    // 几何绘制。混进图元列表会让每次插入都多画一遍占位文字。
    if (m_discardBlock) return;      // 被忽略的块：内容一概不留
    if (!m_currentBlock) {
        skip(QStringLiteral("ATTDEF(块定义之外)"));
        return;
    }

    AttributeDefinition definition;
    definition.tag = toQString(data.tag).trimmed();
    definition.prompt = toQString(data.prompt);
    definition.defaultValue = toQString(data.text);
    definition.position = toPoint(data.basePoint);
    definition.height = data.height;
    definition.rotation = textRotationRadians(data, m_source);
    definition.style = toQString(data.style);
    definition.flags = data.flags;
    if (definition.tag.isEmpty()) return;

    m_currentBlock->attributeDefs.push_back(definition);
}

void Builder::addAttrib(const DRW_Attrib& data)
{
    Attribute attribute;
    attribute.tag = toQString(data.tag).trimmed();
    attribute.value = toQString(data.text);
    attribute.position = toPoint(data.basePoint);
    attribute.height = data.height;
    attribute.rotation = textRotationRadians(data, m_source);
    attribute.style = toQString(data.style);
    attribute.flags = data.flags;
    attribute.layer = toQString(data.layer);
    if (attribute.tag.isEmpty()) return;

    // DXF：ATTRIB 紧跟在它的 INSERT 之后（libdxfrw 的 processInsert 会把随后的
    // ATTRIB 读完再返回），直接挂到刚读到的那个块参照上。
    if (m_lastInsert) {
        m_lastInsert->attributes().push_back(attribute);
        return;
    }

    // DWG：ATTRIB 是独立对象，与 INSERT 之间没有顺序关系，只能收尾时按属主配对。
    if (m_source == Source::Dwg) {
        m_pendingAttribs.push_back(PendingAttrib{ attribute, data.parentHandle });
        return;
    }

    skip(QStringLiteral("ATTRIB(前无块参照)"));
}

void Builder::addInsert(const DRW_Insert& data)
{
    noteInsert(data, data);
}

void Builder::noteInsert(const DRW_Insert& data, const DRW_Entity& source)
{
    const QString name = toQString(data.name);
    if (name.isEmpty()) return;

    // 指向表格匿名几何块（*Tnn）的块参照。真·ACAD_TABLE 实体走 addTable
    // （见 processTable），会同时给出几何块与本体行列结构；但老版本 AutoCAD
    // 或经第三方工具处理过的文件，可能只剩这个块参照——此时退回几何反推。
    if (isTableBlock(name)) {
        noteTableRef(data);
        return;
    }

    // 插入点的坐标体检。块内图元在各自读入时已经查过，这里只需盯住插入点：
    // 它把整块平移到哪里，决定了这块内容会不会落到天文数字上去。
    const Point2D point = toPoint(data.basePoint);
    if (!std::isfinite(point.x) || !std::isfinite(point.y)
        || std::abs(point.x) > kMaxCoordinate || std::abs(point.y) > kMaxCoordinate) {
        skip(QStringLiteral("INSERT(坐标超出范围)"));
        return;
    }

    auto insert = std::make_unique<InsertEntity>();
    insert->setBlockName(name);
    insert->setInsertionPoint(point);
    insert->setScale((data.xscale == 0.0) ? 1.0 : data.xscale,
                     (data.yscale == 0.0) ? 1.0 : data.yscale,
                     (data.zscale == 0.0) ? 1.0 : data.zscale);
    insert->setRotation(data.angle);            // 两条路径都是弧度
    insert->setColumnCount(data.colcount);
    insert->setRowCount(data.rowcount);
    insert->setColumnSpacing(data.colspace);
    insert->setRowSpacing(data.rowspace);
    applyCommon(*insert, source);

    if (m_discardBlock) {            // 被忽略的块：内容一概不留
        m_lastInsert = nullptr;
        return;
    }

    m_lastInsert = insert.get();
    if (m_source == Source::Dwg) {
        m_insertsByOwner[source.parentHandle].push_back(insert.get());
    }

    if (m_currentBlock) m_currentBlock->inserts.push_back(std::move(insert));
    else                m_modelInserts.push_back(std::move(insert));
}

void Builder::noteTableRef(const DRW_Insert& data)
{
    const QString name = toQString(data.name);
    if (name.isEmpty()) return;

    const Point2D point = toPoint(data.basePoint);
    if (!std::isfinite(point.x) || !std::isfinite(point.y)
        || std::abs(point.x) > kMaxCoordinate || std::abs(point.y) > kMaxCoordinate) {
        skip(QStringLiteral("ACAD_TABLE(坐标超出范围)"));
        return;
    }

    TableRef ref;
    ref.blockName = name;
    ref.insertion = point;
    ref.rotation = data.angle;                    // 两条路径都是弧度
    ref.scaleX = (data.xscale == 0.0) ? 1.0 : data.xscale;
    ref.scaleY = (data.yscale == 0.0) ? 1.0 : data.yscale;
    ref.layer = toQString(data.layer);
    if (!m_discardBlock) m_tables.push_back(ref);
}

void Builder::noteTable(const DRW_Table& data)
{
    // 注意：不能因为块名称为空就丢弃。DWG（R2000/AC1015）里表格本体是不透明二进制，
    // 块头句柄解不出来 → name 为空；但表格实体本身有效，后续可由 finishTables 与
    // 相应的 *Tnn 几何块配对还原。DXF 路径的 name 一定是齐的，不影响。
    const QString name = toQString(data.name);

    const Point2D point = toPoint(data.basePoint);
    if (!std::isfinite(point.x) || !std::isfinite(point.y)
        || std::abs(point.x) > kMaxCoordinate || std::abs(point.y) > kMaxCoordinate) {
        skip(QStringLiteral("ACAD_TABLE(坐标超出范围)"));
        return;
    }

    TableRef ref;
    ref.blockName = name;
    ref.insertion = point;
    ref.rotation = data.angle;                    // 两条路径都是弧度
    ref.scaleX = (data.xscale == 0.0) ? 1.0 : data.xscale;
    ref.scaleY = (data.yscale == 0.0) ? 1.0 : data.yscale;
    ref.layer = toQString(data.layer);

    // 本体自带行列结构：行列数 > 0 才认为有效（有些工具写出的是空壳）。
    if (data.numRows > 0 && data.numCols > 0) {
        ref.hasNativeData = true;
        ref.nativeRows = data.numRows;
        ref.nativeCols = data.numCols;
        ref.nativeRowHeights = data.rowHeights;
        ref.nativeColWidths = data.colWidths;
        ref.nativeTableStyle = toQString(data.tableStyle);
        for (const DRW_Table::Cell& cell : data.cells) {
            ref.nativeCells.emplace_back(cell.row, cell.col, toQString(cell.text), cell.align);
        }
        for (const DRW_Table::Merge& merge : data.merges) {
            TableMergeRange range;
            range.row = merge.row;
            range.col = merge.col;
            range.rowSpan = merge.rowSpan;
            range.colSpan = merge.colSpan;
            ref.nativeMerges.push_back(range);
        }
    }

    if (!m_discardBlock) m_tables.push_back(ref);
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

void Builder::addRay(const DRW_Ray& data)
{
    // RAY：基点（10 组码）+ 单位方向（11 组码），只朝方向一侧延伸。
    // 方向退化为零时图元画不出来也选不中，按坏数据计数上报。
    if (toVector(data.secPoint).length() <= kGeomTol) {
        skip(QStringLiteral("RAY(方向为零)"));
        return;
    }

    auto entity = std::make_unique<RayEntity>(toPoint(data.basePoint), toVector(data.secPoint));
    applyCommon(*entity, data);
    place(std::move(entity));
}

void Builder::addXline(const DRW_Xline& data)
{
    // XLINE：与 RAY 同构，区别只在「双向延伸」（由图元类型决定）。
    if (toVector(data.secPoint).length() <= kGeomTol) {
        skip(QStringLiteral("XLINE(方向为零)"));
        return;
    }

    auto entity = std::make_unique<XLineEntity>(toPoint(data.basePoint), toVector(data.secPoint));
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
// MTEXT 的内联格式码（\P 段落、\f 字体、\H 字高…）与「附着点 + 参考矩形」的
// 排版语义都由核心图元 MTextEntity 负责解析与还原（core/MTextEntity.cpp）：
//   · stripInlineCodes()        剥码取可见文本（显示 / 编辑用）
//   · paragraphJustifications() 段落对齐码（\pxqc; 等）优先于附着点隐含的横向对齐
//   · inlineFontNameOf()        内容里声明的内联字体
// 导入不再「剥码 + 另建样式 + 降级成单行文字」，内容与排版字段整体交给模型保真往返。

/// 建立单行文字图元。旋转角由调用方按**弧度**给出——libdxfrw 的两条读取路径
/// 对角度用的单位不同（见本文件头部说明），换算是调用方的事，这里只认弧度。
std::unique_ptr<TextEntity> Builder::makeText(const DRW_Text& data, double rotationRadians)
{
    const double height = (data.height > kGeomTol) ? data.height : 3.5;
    auto entity = std::make_unique<TextEntity>(toQString(data.text),
                                               toPoint(data.basePoint),
                                               height, rotationRadians);

    // 对齐：libdxfrw 的枚举与 DXF 72/73 一致；Aligned(3)/Middle(4)/Fit(5)
    // 只在垂直对齐为基线时出现，这里分别映射到最接近的语义。
    // 先垂直后水平：Middle(4) 要在垂直档**之后**同时置 Center 与 Middle——
    // DXF 规范明确 Middle 渲染与「中中」完全等价，仅 72/73 的存法不同；
    // 若只改水平档，写回会变成（居中，基线），文字整体抬高约半个字高
    // （实测某图纸 52 条图签文字因此错位）。
    switch (data.alignV) {
    case DRW_Text::VBottom: entity->setVAlign(TextEntity::VAlign::Bottom); break;
    case DRW_Text::VMiddle: entity->setVAlign(TextEntity::VAlign::Middle); break;
    case DRW_Text::VTop:    entity->setVAlign(TextEntity::VAlign::Top);    break;
    default:                entity->setVAlign(TextEntity::VAlign::Baseline); break;
    }

    switch (data.alignH) {
    case DRW_Text::HCenter:
        entity->setHAlign(TextEntity::HAlign::Center);
        break;
    case DRW_Text::HMiddle:
        entity->setHAlign(TextEntity::HAlign::Center);
        entity->setVAlign(TextEntity::VAlign::Middle);
        break;
    case DRW_Text::HRight:
        entity->setHAlign(TextEntity::HAlign::Right);
        break;
    default:
        entity->setHAlign(TextEntity::HAlign::Left);
        break;
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

// -----------------------------------------------------------------------------
//  椭圆 / 样条
// -----------------------------------------------------------------------------
void Builder::addEllipse(const DRW_Ellipse& data)
{
    // 11 组码是「主轴端点相对中心」的向量；参数以主轴方向为零点（弧度）
    auto entity = std::make_unique<EllipseEntity>(
        toPoint(data.basePoint), Point2D(data.secPoint.x, data.secPoint.y),
        data.ratio, data.staparam, data.endparam);
    applyCommon(*entity, data);
    place(std::move(entity));
}

void Builder::addSpline(const DRW_Spline* data)
{
    if (!data) return;

    auto entity = std::make_unique<SplineEntity>();
    entity->setDegree(data->degree);
    entity->setClosed((data->flags & 1) != 0);    // bit1 = 闭合

    std::vector<Point2D> control;
    control.reserve(data->controllist.size());
    for (const DRW_Coord* c : data->controllist) {
        if (c) control.push_back(toPoint(*c));
    }
    entity->setControlPoints(std::move(control));
    entity->setKnots(data->knotslist);

    std::vector<Point2D> fits;
    fits.reserve(data->fitlist.size());
    for (const DRW_Coord* c : data->fitlist) {
        if (c) fits.push_back(toPoint(*c));
    }
    entity->setFitPoints(std::move(fits));

    applyCommon(*entity, *data);
    place(std::move(entity));
}

void Builder::addText(const DRW_Text& data)
{
    auto entity = makeText(data, textRotationRadians(data, m_source));
    applyCommon(*entity, data);
    place(std::move(entity));
}

void Builder::addMText(const DRW_MText& data)
{
    // 多行文字**不再降级**成单行文字：MTEXT 与 TEXT 的定位语义不同（MTEXT 由
    // 附着点 + 参考矩形宽度定位，中间行的垂直基准是基线到小写高度的中点；TEXT 的
    // 72=1/73=2 用的是基线到大写高度的中点），降级会让文件在 AutoCAD / 浩辰里
    // 重新打开时每条文字整体偏移约 0.13 倍字高。这里按 MTEXT 自身语义建独立图元。
    //
    // 组码与库字段的对应（与 DXF 一致，不能照搬 TEXT 的语义）：
    //   · 10      插入点            -> data.basePoint
    //   · 11      X 轴方向向量      -> data.secPoint（旋转的唯一来源）
    //   · 40      字高              -> data.height
    //   · 41      参考矩形宽度      -> data.widthscale
    //   · 71      附着点(1~9)       -> data.textgen
    //   · 72      书写方向          -> data.alignH
    //   · 73      行距样式          -> data.alignV
    //   · 44      行距系数          -> data.interlin
    //   · 210     挤出方向          -> data.extPoint
    //   · 1/3     内容（原始字符串，含内联格式码）-> data.text
    //   · 7       文字样式名        -> data.style
    auto entity = std::make_unique<MTextEntity>(toQString(data.text),
                                                toPoint(data.basePoint),
                                                (data.height > kGeomTol) ? data.height : 3.5);

    // 旋转：MTEXT 的旋转写在 11 组码的 X 轴方向向量里——50 组码在 MTEXT 中几乎不出现，
    // 只有极少数生成器会直接给角度。因此只要有方向向量就按它算 atan2。
    //
    // 这里曾经踩过一个坑：libdxfrw-modify 原实现在 DWG 路径上没置位 haveXAxis，
    // hasXAxisDirection() 恒为 false，于是 DWG 里的 MTEXT 一律退回 angle——而 DWG
    // 路径的 angle 永远是 0，旋转就整体丢了。实测一张 2018 版 DWG（AC1032）：
    // 272 条文字里 202 条旋转 90° 的 MTEXT 被画成水平，同一张图的 DXF 全部正确。
    // 库那边已修正，这里同时保留方向向量的优先级。
    double rotationRad = textRotationRadians(data, m_source);
    if (data.hasXAxisDirection()
        && std::hypot(data.secPoint.x, data.secPoint.y) > kGeomTol) {
        rotationRad = std::atan2(data.secPoint.y, data.secPoint.x);
    }
    entity->setDirection(Vector2D(std::cos(rotationRad), std::sin(rotationRad)));

    // 附着点（71）：库把 1~9 读进了 textgen，越界值按左上处理
    entity->setAttachment(MTextEntity::attachmentFromRowColumn(
        (std::clamp(data.textgen, 1, 9) - 1) / 3,
        (std::clamp(data.textgen, 1, 9) - 1) % 3));

    // 参考矩形宽度（41）：真实文件里的**编辑时折行宽度**（显示不据此重排，见 MTextEntity）；
    // 负值表示"无限制"，统一归一为 0
    const double rectWidth = std::abs(data.widthscale);
    entity->setReferenceWidth(rectWidth > kGeomTol ? rectWidth : 0.0);

    // 书写方向（72）与行距样式（73）：MTEXT 的这两个组码被库读进了 alignH/alignV
    switch (static_cast<int>(data.alignH)) {
    case 3:  entity->setDrawingDirection(MTextEntity::DrawingDirection::TopToBottom); break;
    case 5:  entity->setDrawingDirection(MTextEntity::DrawingDirection::ByStyle); break;
    default: entity->setDrawingDirection(MTextEntity::DrawingDirection::LeftToRight); break;
    }
    switch (static_cast<int>(data.alignV)) {
    case 2:  entity->setLineSpacingStyle(MTextEntity::LineSpacingStyle::Exact); break;
    default: entity->setLineSpacingStyle(MTextEntity::LineSpacingStyle::AtLeast); break;
    }
    entity->setLineSpacingFactor(data.interlin);

    // 挤出方向（210）：只做原样往返（几何按文字落在 XY 平面计算）
    MTextEntity::Normal3D normal;
    normal.x = data.extPoint.x;
    normal.y = data.extPoint.y;
    normal.z = data.extPoint.z;
    entity->setNormal(normal);

    const QString style = toQString(data.style);
    if (!style.isEmpty()) entity->setFontStyle(style);

    // 内联字体码（\fSimSun|b0|i0|c134|p2;）原样留在内容里——真实 CAD 打开时按它取字，
    // 不再像降级时代那样"剥码 + 另建一条文字样式"（那会改掉文件里的内容）。
    applyCommon(*entity, data);
    place(std::move(entity));
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

    // 图案定义线（53/43/44/45/46/49）原样带走：导出时必须写回，
    // 只给图案名不给定义的话，别的 CAD（实测 GstarCAD）会把填充画成一片乱纹。
    std::vector<HatchEntity::PatternLine> patternLines;
    patternLines.reserve(data.deflist.size());
    const double readScale = data.scale > kGeomTol ? data.scale : 1.0;
    for (const DRW_HatchDefLine& src : data.deflist) {
        HatchEntity::PatternLine line;
        line.angle  = src.angle;                        // libdxfrw 内部统一用弧度
        line.base   = Point2D(src.base.x, src.base.y);
        // 偏移换算成「已经乘过比例的最终间距」：DXF 的图案定义是乘比例之前的，
        // DWG 的则是最终值（实测本图 45° 定义偏移 1.5875 = 内置 ANSI31 间距
        // 3.175 × 比例 0.5）。统一口径后，显示与写出才能对得上。
        const bool fromDxf = (m_source == Source::Dxf);
        line.offset = fromDxf ? Point2D(src.offset.x * readScale, src.offset.y * readScale)
                              : Point2D(src.offset.x, src.offset.y);
        line.dashes = src.dashes;
        patternLines.push_back(std::move(line));
    }
    entity->setPatternLines(std::move(patternLines));

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

void Builder::addDimOrdinate(const DRW_DimOrdinate* data)
{
    if (!data) return;

    auto entity = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Ordinate);

    // 坐标标注的定义点：13/23 是被测点，14/24 是引线端点，10/20 是**坐标基准**
    //（量的是被测点相对基准在某一轴上的坐标，不是两点距离）。
    entity->setFirstPoint(toPoint(data->getFirstLine()));
    entity->setSecondPoint(toPoint(data->getSecondLine()));
    entity->setDimLinePoint(toPoint(data->getOriginPoint()));

    // X 型还是 Y 型（量 X 坐标还是 Y 坐标）。这个标志位在两条读取路径上
    // **不在同一处**：DXF 用 70 的 bit6（值 64），DWG 经 libdxfrw 解码后落在
    // bit7（128）上，且 bit7 还会被「文字是否在默认位置」那一位干扰。
    // 因此这里以文件里给出的**实测值**（42 组码）为准：哪个轴的坐标与之吻合
    // 就是哪一型；42 缺失时才退回按位判断。
    const double measured = data->getMeasurement();
    const double axisX = entity->firstPoint().x - entity->dimLinePoint().x;
    const double axisY = entity->firstPoint().y - entity->dimLinePoint().y;
    const bool xType = (std::abs(measured) > kGeomTol)
                           ? (std::abs(measured - axisX) <= std::abs(measured - axisY))
                           : (data->type & 64) != 0;
    entity->setOrdinateXType(xType);

    const Point2D textPoint = toPoint(data->getTextPoint());
    if (textPoint.distanceTo(Point2D(0.0, 0.0)) > kGeomTol
        || entity->firstPoint().distanceTo(textPoint) > kGeomTol) {
        entity->setTextPoint(textPoint);
    }

    const QString override = toQString(data->getText()).trimmed();
    if (!override.isEmpty() && override != QLatin1String("<>")) entity->setTextOverride(override);

    applyCommon(*entity, *data);
    place(std::move(entity));
}

void Builder::addDimArc(const DRW_DimArc* data)
{
    if (!data) return;

    // 弧长标注的定义点：13/14 是**被测弧**的两端（也就是尺寸界线的原点），
    // 15 是弧心，40/41 是起止参数（弧度），10 是尺寸弧经过的点。
    // 被测弧半径由「弧心 -> 13 点」推出，尺寸弧半径由「弧心 -> 10 点」推出，
    // 两者不是一回事（实测某图：被测弧 806.6，尺寸弧 391.3）。
    auto entity = std::make_unique<DimensionEntity>(DimensionEntity::DimType::ArcLength);
    entity->setArcCenter(toPoint(data->getCenterPoint()));
    entity->setFirstPoint(toPoint(data->getXLine1Point()));
    entity->setSecondPoint(toPoint(data->getXLine2Point()));
    entity->setDimLinePoint(toPoint(data->getDefPoint()));
    entity->setArcAngles(data->getArcStartParam(), data->getArcEndParam());

    const Point2D textPoint = toPoint(data->getTextPoint());
    if (textPoint.distanceTo(Point2D(0.0, 0.0)) > kGeomTol
        || entity->dimLinePoint().distanceTo(textPoint) > kGeomTol) {
        entity->setTextPoint(textPoint);
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
//  块定义装配
//
//  读取期只把定义暂存在 BlockDef 里，块引用是「没有定义的 InsertEntity」。
//  这里一次性把它们接上：块引用可以出现在定义之前（DWG 的对象流完全不保证
//  顺序），也可以嵌套，所以必须等全部读完再解析。
// -----------------------------------------------------------------------------
std::shared_ptr<BlockDefinition> Builder::resolveBlock(const QString& name)
{
    const QString key = BlockTable::normalizedKey(name);
    const auto it = m_blocks.find(key);
    if (it == m_blocks.end()) return nullptr;

    // 同一个定义只装配一次：块内的 InsertEntity 必须共享同一份定义对象，
    // 否则「改一处定义、所有引用同步」的语义就没了。
    const auto cached = m_resolvedBlocks.find(key);
    if (cached != m_resolvedBlocks.end()) return cached->second;

    BlockDef& def = it->second;
    auto block = std::make_shared<BlockDefinition>(def.name);
    block->setBasePoint(def.base);
    block->setFlags(def.flags);
    for (const AttributeDefinition& attributeDef : def.attributeDefs) {
        block->addAttributeDefinition(attributeDef);
    }
    for (std::unique_ptr<Entity>& entity : def.entities) {
        block->addEntity(std::move(entity));
    }
    def.entities.clear();

    // 先登记再递归：自引用块（A 里插 A）会在这里再次查到自己，
    // 拿到的是同一个定义对象，因而不会无限递归地新建定义。
    m_resolvedBlocks.emplace(key, block);

    std::vector<std::unique_ptr<InsertEntity>> nested = std::move(def.inserts);
    def.inserts.clear();

    // 装配期间把「当前正在装配的定义」记进链上：链上已有的名字再被引用就是
    // 循环，必须断开（见 m_resolveChain 的说明）。
    m_resolveChain.push_back(key);
    attachInserts(std::move(nested), block);
    m_resolveChain.pop_back();
    return block;
}

void Builder::attachInserts(std::vector<std::unique_ptr<InsertEntity>>&& inserts,
                            const std::shared_ptr<BlockDefinition>& owner)
{
    for (std::unique_ptr<InsertEntity>& insert : inserts) {
        if (!insert) continue;

        // 循环引用：目标块正在装配链上。断开而不是连上——连上会让两者互相
        // 持有 shared_ptr，谁都不会被释放。
        const QString target = BlockTable::normalizedKey(insert->blockName());
        if (!target.isEmpty()
            && std::find(m_resolveChain.begin(), m_resolveChain.end(), target)
                   != m_resolveChain.end()) {
            ++m_cyclicBlocks;
            continue;
        }

        std::shared_ptr<BlockDefinition> block = resolveBlock(insert->blockName());
        if (!block) {
            // 引用的块未定义：写回去会变成悬空引用（AutoCAD 打开时报警），
            // 因此不保留这个块参照，只在导入警告里如实计数。
            ++m_missingBlocks;
            continue;
        }

        insert->setBlock(block);       // 顺带按属性定义补齐缺失的 ATTRIB
        if (owner) owner->addEntity(std::move(insert));
        else       m_doc.addEntity(std::move(insert));
    }
}

void Builder::attachPendingAttribs()
{
    // 同一个属主下、同一个标签的第 k 条 ATTRIB，配给第 k 个「块定义里有这个
    // 标签」的块参照。单块参照（最常见）时就是全部给它；多个同块参照时按
    // 读取顺序依次铺开，不会出现「所有属性都堆到第一个块参照上」。
    std::map<duint32, std::map<QString, std::size_t>> assigned;

    for (const PendingAttrib& pending : m_pendingAttribs) {
        const auto ownerIt = m_insertsByOwner.find(pending.owner);
        if (ownerIt == m_insertsByOwner.end()) {
            ++m_orphanAttribs;
            continue;
        }

        const QString tag = pending.attribute.tag;
        std::size_t& index = assigned[pending.owner][BlockTable::normalizedKey(tag)];

        bool placed = false;
        std::size_t seen = 0;
        for (InsertEntity* insert : ownerIt->second) {
            if (!insert || !insert->hasBlock()) continue;
            if (!insert->block()->findAttributeDefinition(tag)) continue;
            if (seen++ < index) continue;

            insert->setAttribute(pending.attribute);
            ++index;
            placed = true;
            break;
        }

        if (!placed) ++m_orphanAttribs;
    }

    m_pendingAttribs.clear();
    m_insertsByOwner.clear();
}

// -----------------------------------------------------------------------------
//  表格还原
//
//  两条路径，按可信度排序：
//    1) 真·ACAD_TABLE 本体自带行列结构（91/92/141/142/1）——直接采用，
//       这是权威数据，不受几何块里文字摆放方式的影响。
//    2) 只有指向 *Tnn 几何块的块参照（本软件写出的表格、或被第三方工具
//       剥掉本体的文件）——从几何反推：横线/竖线的坐标集合即行/列边界，
//       块内文字按所在格归位。
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
//       块内文字按所在格归位。
// -----------------------------------------------------------------------------
//! DWG/R2000 兜底：取一个还没被别的表格认领的表格几何块。
//!
//! R2000 的 ACAD_TABLE 本体是一段不透明的二进制数据，
//! 块头句柄解不出来。但文件结构保证：每张表恰有一个 *Tnn 几何块，且该块的
//! BLKREFS 反应器直指表格句柄。两个集合各取未被认领的一个即可配对。
const Builder::BlockDef* Builder::takeUnclaimedTableGeometry()
{
    for (const auto& [key, def] : m_blocks) {
        if (!def.isTableGeometry) continue;
        if (m_claimedTableGeometry.count(key)) continue;
        m_claimedTableGeometry.insert(key);
        return &def;
    }
    return nullptr;
}

void Builder::finishTables()
{
    if (m_tables.empty()) return;

    // 几何块按归一化名索引，便于 O(1) 查引用。
    std::map<QString, const BlockDef*> byKey;
    for (const auto& [key, def] : m_blocks) {
        if (def.isTableGeometry) byKey[key] = &def;
    }
    for (TableRef& ref : m_tables) {
        std::unique_ptr<TableEntity> table = buildTableFromNative(ref);

        // 本体结构有效但 cells 数组为空：DWG 的 cells 编码尚未完全逆向
        // 而完整的单元格文字烘在 *Tnn 匿名几何块里。此时结构仍以本体为准，
        // 再从几何块把文字按格归位补回来——不算降级，行列尺寸、合并、图层
        // 全部保持本体值，也不触发「按几何块反推」警告。
        if (table && ref.nativeCells.empty()) {
            const BlockDef* paired = nullptr;
            const auto named = byKey.find(BlockTable::normalizedKey(ref.blockName));
            if (named != byKey.end() && named->second) {
                paired = named->second;
                m_claimedTableGeometry.insert(named->first);
            } else {
                // DWG(R2000) 里表格本体的块头句柄解不出来 → blockName 为空，
                // 退回「各取未被认领的一个」配对（每个表格恰对应一个 *Tnn 块）。
                paired = takeUnclaimedTableGeometry();
            }
            if (paired) mergeTableCellTexts(*table, ref, *paired);
        }

        // 块名缺失/失配：DWG（R2000/AC1015）里 ACAD_TABLE 的本体是一段不透明的
        // 二进制块，块头句柄解不出来；此时退回「与几何块配对」。
        // 依据：每个表格实体恰对应一个 *Tnn 几何块，且该块的 BLKREFS 反应器
        // 直指表格句柄（DXF 侧可见）。两个集合各取未被认领的那一个即可。
        if (!table) {
            const auto it = byKey.find(BlockTable::normalizedKey(ref.blockName));
            if (it != byKey.end() && it->second) {
                m_claimedTableGeometry.insert(it->first);
                table = buildTableFromGeometry(ref, *it->second);
                if (table) downgrade(QStringLiteral("ACAD_TABLE(按几何块反推行列)"));
            } else {
                const BlockDef* paired = takeUnclaimedTableGeometry();
                if (paired) {
                    table = buildTableFromGeometry(ref, *paired);
                    if (table) downgrade(QStringLiteral("ACAD_TABLE(按几何块反推行列)"));
                }
            }
        }

        if (!table) {
            skip(QStringLiteral("ACAD_TABLE(无法还原)"));
            continue;
        }

        if (!ref.layer.isEmpty() && ref.layer != QLatin1String("0")) {
            // 仅当本体图层可信时覆盖；几何反推路径已在内部回退到块内图层。
            if (table->layerName().isEmpty() || table->layerName() == QLatin1String("0"))
                table->setLayerName(ref.layer);
        }
        m_doc.addEntity(std::move(table));
    }

    m_tables.clear();
}

//! 用 ACAD_TABLE 本体携带的行列结构建表。无本体数据时返回 nullptr。
std::unique_ptr<TableEntity> Builder::buildTableFromNative(const TableRef& ref) const
{
    if (!ref.hasNativeData) return nullptr;

    const std::size_t rows = static_cast<std::size_t>(ref.nativeRows);
    const std::size_t cols = static_cast<std::size_t>(ref.nativeCols);

    auto table = std::make_unique<TableEntity>(rows, cols);
    table->setInsertionPoint(ref.insertion);
    table->setRotation(ref.rotation);

    // 行列尺寸：本体给出的是局部尺寸（未随块参照缩放），需乘上块参照缩放。
    const double sx = std::abs(ref.scaleX);
    const double sy = std::abs(ref.scaleY);
    for (std::size_t r = 0; r < rows && r < ref.nativeRowHeights.size(); ++r) {
        table->setRowHeight(r, ref.nativeRowHeights[r] * sy);
    }
    for (std::size_t c = 0; c < cols && c < ref.nativeColWidths.size(); ++c) {
        table->setColumnWidth(c, ref.nativeColWidths[c] * sx);
    }

    // DWG 原生路径给出的对齐方式是 1~9 的九宫格编号
    //（1=左上 … 5=居中 … 9=右下），越界/缺省归居中。
    auto mapCellAlign = [](int align) {
        switch (align) {
        case 1: return TableCellAlign::TopLeft;
        case 2: return TableCellAlign::TopCenter;
        case 3: return TableCellAlign::TopRight;
        case 4: return TableCellAlign::MiddleLeft;
        case 6: return TableCellAlign::MiddleRight;
        case 7: return TableCellAlign::BottomLeft;
        case 8: return TableCellAlign::BottomCenter;
        case 9: return TableCellAlign::BottomRight;
        default: return TableCellAlign::MiddleCenter;
        }
    };

    for (const auto& [row, col, text, align] : ref.nativeCells) {
        if (row < 0 || col < 0) continue;
        if (static_cast<std::size_t>(row) >= rows || static_cast<std::size_t>(col) >= cols) continue;
        const auto r = static_cast<std::size_t>(row);
        const auto c = static_cast<std::size_t>(col);
        TableCell cell = table->cell(r, c);
        cell.text = text;
        cell.align = mapCellAlign(align);
        table->setCell(r, c, cell);
    }
    for (const TableMergeRange& merge : ref.nativeMerges) {
        table->addMerge(merge);
    }
    return table;
}

std::unique_ptr<TableEntity> Builder::buildTableFromGeometry(const TableRef& ref,
                                                             const BlockDef& def) const
{
    // 1) 收集块内所有直线段（LINE / LWPOLYLINE 段的直线化结果）作为网格线。
    //    这里只认真正水平/垂直的线段——表格的网格线正是如此；单元格文字
    //    的包围盒不参与，因为它不是线。
    std::vector<std::pair<Point2D, Point2D>> segments;
    segments.reserve(def.entities.size());

    // 文字：记录基线点与内容，稍后按所在单元格归位
    struct BlockText {
        QString text;
        Point2D anchor;
        double  height = 0.0;
    };
    std::vector<BlockText> texts;
    // 几何块内首个非空图层名——表格本体图层不可信（DWG 兜底）时用它。
    QString geometryLayer;

    for (const std::unique_ptr<Entity>& entity : def.entities) {
        if (!entity) continue;
        if (geometryLayer.isEmpty() && !entity->layerName().isEmpty())
            geometryLayer = entity->layerName();
        switch (entity->type()) {
        case EntityType::Line: {
            const auto* line = static_cast<const LineEntity*>(entity.get());
            segments.emplace_back(line->start(), line->end());
            break;
        }
        case EntityType::Polyline: {
            const auto* poly = static_cast<const PolylineEntity*>(entity.get());
            const std::vector<Point2D> pts = poly->flatten(kGeomTol);
            for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
                segments.emplace_back(pts[i], pts[i + 1]);
            }
            if (poly->isClosed() && pts.size() >= 3) {
                segments.emplace_back(pts.back(), pts.front());
            }
            break;
        }
        case EntityType::Text: {
            const auto* text = static_cast<const TextEntity*>(entity.get());
            texts.push_back(BlockText{ text->displayString(), text->position(), text->height() });
            break;
        }
        default:
            break;
        }
    }

    if (segments.empty()) return nullptr;

    // 2) 水平线 -> 行边界 Y 集合；竖线 -> 列边界 X 集合。坐标相同的线合并。
    constexpr double kAxisTol = 1e-3;
    auto nearlyEqual = [](double a, double b) { return std::abs(a - b) <= kAxisTol; };

    std::vector<double> rowYs;      // Y 坐标（自上而下递减）
    std::vector<double> colXs;      // X 坐标（自左而右递增）
    auto insertUnique = [&](std::vector<double>& v, double value) {
        for (double existing : v) {
            if (nearlyEqual(existing, value)) return;
        }
        v.push_back(value);
    };

    for (const auto& [a, b] : segments) {
        if (nearlyEqual(a.y, b.y)) {
            insertUnique(rowYs, a.y);              // 水平线
        } else if (nearlyEqual(a.x, b.x)) {
            insertUnique(colXs, a.x);              // 竖线
        }
    }

    std::sort(rowYs.begin(), rowYs.end(), std::greater<double>());   // 上大下小
    std::sort(colXs.begin(), colXs.end());                           // 左小右大

    if (rowYs.size() < 2 || colXs.size() < 2) return nullptr;

    const std::size_t rows = rowYs.size() - 1;
    const std::size_t cols = colXs.size() - 1;

    // 3) 组装 TableEntity。局部坐标：插入点即左上角（0,0），Y 向下为负。
    auto table = std::make_unique<TableEntity>();

    // 插入点与旋转的取舍：
    //   · DXF / 写出的表格引用：ref 里的插入点、旋转是可信的（与块参照字段一致）。
    //   · DWG(R2000) 兜底配对：表格本体是不可解析的不透明块，ref 里的数值是错位
    //     解出来的垃圾（可能是 1e80 量级）。此时改由几何自身定位——左上角即插入点，
    //     旋转取 0（块内几何已按图纸坐标系摆放）。
    const bool refTrustworthy =
        std::isfinite(ref.insertion.x) && std::isfinite(ref.insertion.y)
        && std::abs(ref.insertion.x) <= kMaxCoordinate
        && std::abs(ref.insertion.y) <= kMaxCoordinate
        && std::abs(ref.rotation) <= 2.0 * M_PI + 1e-6
        && std::abs(ref.scaleX) > 1e-9 && std::abs(ref.scaleX) < 1e6
        && std::abs(ref.scaleY) > 1e-9 && std::abs(ref.scaleY) < 1e6;

    if (refTrustworthy) {
        table->setInsertionPoint(ref.insertion);
        table->setRotation(ref.rotation);
    } else {
        // 几何块基点 + 网格左上角 → 世界坐标插入点
        const double sx = 1.0;
        table->setInsertionPoint(Point2D{ def.base.x + colXs.front() * sx,
                                          def.base.y + rowYs.front() * sx });
        table->setRotation(0.0);
    }

    // 几何块基点可能不是 0（块基点 -> 局部坐标偏移）。块内坐标需减去基点，
    // 再按插入点的参考系摆放。这里把块基点折算进局部坐标。
    const double baseX = def.base.x;
    const double baseY = def.base.y;

    const double topY = rowYs.front();
    const double leftX = colXs.front();

    table->reset(rows, cols, 1.0, 1.0);
    const double scaleX = refTrustworthy ? std::abs(ref.scaleX) : 1.0;
    const double scaleY = refTrustworthy ? std::abs(ref.scaleY) : 1.0;
    for (std::size_t r = 0; r < rows; ++r) {
        const double h = std::abs(rowYs[r] - rowYs[r + 1]);
        table->setRowHeight(r, h > kGeomTol ? h * scaleY : 1.0);
    }
    for (std::size_t c = 0; c < cols; ++c) {
        const double w = std::abs(colXs[c + 1] - colXs[c]);
        table->setColumnWidth(c, w > kGeomTol ? w * scaleX : 1.0);
    }

    // 4) 文字归位：按基线点落在哪个单元格（考虑对齐产生的水平偏移，
    //    这里用中心点而非锚点判断——表格文字通常居中，锚点会在格中）。
    for (const BlockText& text : texts) {
        if (text.text.isEmpty()) continue;

        const double lx = text.anchor.x - baseX;
        const double ly = text.anchor.y - baseY;

        // 找到所属行（ly 落在 [rowYs[r+1], rowYs[r]]）
        int row = -1;
        for (std::size_t r = 0; r < rows; ++r) {
            const double upper = rowYs[r] - baseY;
            const double lower = rowYs[r + 1] - baseY;
            if (ly <= upper + kAxisTol && ly >= lower - kAxisTol) {
                row = static_cast<int>(r);
                break;
            }
        }
        if (row < 0) {
            // 基线可能略低于单元格底（VBaseLine），按单元格高度向上回退一行
            for (std::size_t r = 0; r < rows; ++r) {
                const double upper = rowYs[r] - baseY;
                const double lower = rowYs[r + 1] - baseY;
                if (ly <= upper + text.height + kAxisTol && ly >= lower - kAxisTol) {
                    row = static_cast<int>(r);
                    break;
                }
            }
        }

        int col = -1;
        for (std::size_t c = 0; c < cols; ++c) {
            const double left = colXs[c] - baseX;
            const double right = colXs[c + 1] - baseX;
            if (lx >= left - kAxisTol && lx <= right + kAxisTol) {
                col = static_cast<int>(c);
                break;
            }
        }

        if (row < 0 || col < 0) continue;

        TableCell existing = table->cell(static_cast<std::size_t>(row),
                                        static_cast<std::size_t>(col));
        if (!existing.text.isEmpty()) {
            existing.text += QLatin1Char('\n') + text.text;   // 多行堆叠
        } else {
            existing.text = text.text;
        }
        existing.align = TableCellAlign::MiddleCenter;
        table->setCell(static_cast<std::size_t>(row), static_cast<std::size_t>(col), existing);
    }

    Q_UNUSED(topY);
    Q_UNUSED(leftX);
    // 本体图层不可信（DWG 兜底配对）时，退回几何块内实体的图层。
    if (!refTrustworthy && !geometryLayer.isEmpty())
        table->setLayerName(geometryLayer);
    return table;
}

//! 本体结构有效但 cells 为空时的补全：*Tnn 匿名几何块里除了网格线，
//! 还烘着每个非空单元格的 TEXT（写出端与 AutoCAD 原生写法一致）。块基点
//! 取原点，块内坐标即表格局部坐标，与 rowBoundaries()/columnBoundaries()
//! 同一参考系——锚点减去块基点（再按块参照缩放折算）后，行按 Y 区间
//! （第 0 行在最上方）、列按 X 区间归属。只补文字，不动结构：已有内容的
//! 格用 \n 堆叠，对齐一律置 MiddleCenter（基线锚点反推不出原始对齐）。
//!
//! 合并区同样从几何块反推：写出约定「合并区内部的网格线段不画」，因此
//! 某条内部竖线在行带上找不到覆盖段 → 相邻两格横向连通；内部横线缺失 →
//! 纵向连通。连通块恰为实心矩形时登记为合并区（addMerge 自拒越界/重叠，
//! 畸形几何最多被丢弃，不会污染结构）。本体已带 merges 时跳过反推。
void Builder::mergeTableCellTexts(TableEntity& table, const TableRef& ref,
                                  const BlockDef& def) const
{
    // 块参照缩放异常时按 1 处理（LyCAD 写出的表格比例恒为 1）
    const double sx = (std::isfinite(ref.scaleX) && std::abs(ref.scaleX) > 1e-9
                       && std::abs(ref.scaleX) < 1e6) ? std::abs(ref.scaleX) : 1.0;
    const double sy = (std::isfinite(ref.scaleY) && std::abs(ref.scaleY) > 1e-9
                       && std::abs(ref.scaleY) < 1e6) ? std::abs(ref.scaleY) : 1.0;

    const std::size_t rows = table.rowCount();
    const std::size_t cols = table.columnCount();
    const std::vector<double> rowYs = table.rowBoundaries();     // 上 0，向下为负
    const std::vector<double> colXs = table.columnBoundaries();  // 左 0，向右为正
    if (rowYs.size() != rows + 1 || colXs.size() != cols + 1) return;

    const double baseX = def.base.x;
    const double baseY = def.base.y;

    constexpr double kAxisTol = 1e-3;

    // 块内内容：轴对齐线段（网格线）与文字锚点，均折算到表格局部参考系
    // （块内坐标 - 块基点，再乘块参照缩放）。
    struct BlockSeg { double x1, y1, x2, y2; };
    struct BlockText { QString text; double x, y, height; };
    std::vector<BlockSeg> segments;
    std::vector<BlockText> texts;

    auto pushSeg = [&](const Point2D& a, const Point2D& b) {
        segments.push_back({ (a.x - baseX) * sx, (a.y - baseY) * sy,
                             (b.x - baseX) * sx, (b.y - baseY) * sy });
    };

    for (const std::unique_ptr<Entity>& entity : def.entities) {
        if (!entity) continue;
        switch (entity->type()) {
        case EntityType::Line: {
            const auto* line = static_cast<const LineEntity*>(entity.get());
            pushSeg(line->start(), line->end());
            break;
        }
        case EntityType::Polyline: {
            const auto* poly = static_cast<const PolylineEntity*>(entity.get());
            const std::vector<Point2D> pts = poly->flatten(kGeomTol);
            for (std::size_t i = 0; i + 1 < pts.size(); ++i)
                pushSeg(pts[i], pts[i + 1]);
            if (poly->isClosed() && pts.size() >= 3)
                pushSeg(pts.back(), pts.front());
            break;
        }
        case EntityType::Text: {
            const auto* text = static_cast<const TextEntity*>(entity.get());
            if (!text->displayString().isEmpty()) {
                texts.push_back({ text->displayString(),
                                  (text->position().x - baseX) * sx,
                                  (text->position().y - baseY) * sy,
                                  text->height() * sy });
            }
            break;
        }
        default:
            break;
        }
    }

    // ---- 合并区反推（本体没给 merges 才做） --------------------------------
    if (ref.nativeMerges.empty() && !segments.empty()) {
        // 竖直边界 c 在行带 [rowYs[r+1], rowYs[r]] 上是否有线段覆盖
        auto vBoundaryPresent = [&](std::size_t r, std::size_t c) {
            const double x = colXs[c];
            const double yTop = rowYs[r];
            const double yBot = rowYs[r + 1];
            for (const BlockSeg& s : segments) {
                if (std::abs(s.x1 - x) > kAxisTol || std::abs(s.x2 - x) > kAxisTol)
                    continue;
                const double lo = std::min(s.y1, s.y2);
                const double hi = std::max(s.y1, s.y2);
                if (lo <= yBot + kAxisTol && hi >= yTop - kAxisTol) return true;
            }
            return false;
        };
        // 水平边界 r 在列带 [colXs[c], colXs[c+1]] 上是否有线段覆盖
        auto hBoundaryPresent = [&](std::size_t r, std::size_t c) {
            const double y = rowYs[r];
            const double xL = colXs[c];
            const double xR = colXs[c + 1];
            for (const BlockSeg& s : segments) {
                if (std::abs(s.y1 - y) > kAxisTol || std::abs(s.y2 - y) > kAxisTol)
                    continue;
                const double lo = std::min(s.x1, s.x2);
                const double hi = std::max(s.x1, s.x2);
                if (lo <= xL + kAxisTol && hi >= xR - kAxisTol) return true;
            }
            return false;
        };

        // 并查集：相邻两格之间的内部边界缺失即连通
        std::vector<int> parent(rows * cols);
        std::iota(parent.begin(), parent.end(), 0);
        auto find = [&parent](int x) {
            while (parent[x] != x) x = parent[x] = parent[parent[x]];
            return x;
        };
        auto unite = [&find, &parent](int a, int b) { parent[find(a)] = find(b); };

        for (std::size_t r = 0; r < rows; ++r) {
            for (std::size_t c = 0; c + 1 < cols; ++c) {
                if (vBoundaryPresent(r, c + 1)) continue;
                unite(static_cast<int>(r * cols + c), static_cast<int>(r * cols + c + 1));
            }
        }
        for (std::size_t r = 0; r + 1 < rows; ++r) {
            for (std::size_t c = 0; c < cols; ++c) {
                if (hBoundaryPresent(r + 1, c)) continue;
                unite(static_cast<int>(r * cols + c), static_cast<int>((r + 1) * cols + c));
            }
        }

        // 连通块恰为实心矩形（格子数 == 外包矩形面积）才登记为合并区
        struct Comp { int minR, maxR, minC, maxC, count; };
        std::map<int, Comp> comps;
        for (std::size_t r = 0; r < rows; ++r) {
            for (std::size_t c = 0; c < cols; ++c) {
                const int root = find(static_cast<int>(r * cols + c));
                const auto it = comps.find(root);
                if (it == comps.end()) {
                    comps.emplace(root, Comp{ static_cast<int>(r), static_cast<int>(r),
                                              static_cast<int>(c), static_cast<int>(c), 1 });
                } else {
                    Comp& comp = it->second;
                    comp.minR = std::min(comp.minR, static_cast<int>(r));
                    comp.maxR = std::max(comp.maxR, static_cast<int>(r));
                    comp.minC = std::min(comp.minC, static_cast<int>(c));
                    comp.maxC = std::max(comp.maxC, static_cast<int>(c));
                    ++comp.count;
                }
            }
        }
        for (const auto& [root, comp] : comps) {
            Q_UNUSED(root);
            if (comp.count <= 1) continue;
            const int rowSpan = comp.maxR - comp.minR + 1;
            const int colSpan = comp.maxC - comp.minC + 1;
            if (comp.count != rowSpan * colSpan) continue;   // 非矩形：放弃
            TableMergeRange range;
            range.row = comp.minR;
            range.col = comp.minC;
            range.rowSpan = rowSpan;
            range.colSpan = colSpan;
            table.addMerge(range);
        }
    }

    // ---- 文字按格归位 -------------------------------------------------------
    for (const BlockText& text : texts) {
        const double lx = text.x;
        const double ly = text.y;
        const double th = text.height;   // 基线回退用（已在缩放参考系）

        // 行：ly ∈ [rowYs[r+1], rowYs[r]]（Y 向下为负、第 0 行在最上方）
        int row = -1;
        for (std::size_t r = 0; r < rows; ++r) {
            if (ly <= rowYs[r] + kAxisTol && ly >= rowYs[r + 1] - kAxisTol) {
                row = static_cast<int>(r);
                break;
            }
        }
        if (row < 0) {
            // 基线可能贴着格底（VBaseLine），按文字高度向上回退一行
            for (std::size_t r = 0; r < rows; ++r) {
                if (ly <= rowYs[r] + th + kAxisTol && ly >= rowYs[r + 1] - kAxisTol) {
                    row = static_cast<int>(r);
                    break;
                }
            }
        }

        // 列：lx ∈ [colXs[c], colXs[c+1]]
        int col = -1;
        for (std::size_t c = 0; c < cols; ++c) {
            if (lx >= colXs[c] - kAxisTol && lx <= colXs[c + 1] + kAxisTol) {
                col = static_cast<int>(c);
                break;
            }
        }

        if (row < 0 || col < 0) continue;

        TableCell existing = table.cell(static_cast<std::size_t>(row),
                                        static_cast<std::size_t>(col));
        if (!existing.text.isEmpty())
            existing.text += QLatin1Char('\n') + text.text;   // 同格多行堆叠
        else
            existing.text = text.text;
        existing.align = TableCellAlign::MiddleCenter;
        table.setCell(static_cast<std::size_t>(row), static_cast<std::size_t>(col), existing);
    }
}

// -----------------------------------------------------------------------------
//  收尾
// -----------------------------------------------------------------------------
void Builder::finish()
{
    // 块定义与块引用可能相互穿插（DWG 的对象流完全不保证顺序），
    // 因此必须等全部读完再统一解析。
    for (auto& [key, def] : m_blocks) {
        Q_UNUSED(key);
        // 表格的匿名几何块不是用户意义上的块定义：它的内容用来还原表格，
        // 不能注册进文档块表，否则每次打开都会多出一个 *Tnn 块。
        if (def.isTableGeometry) continue;
        const std::shared_ptr<BlockDefinition> block = resolveBlock(def.name);
        if (block) m_doc.blocks().add(block);
    }

    attachInserts(std::move(m_modelInserts), nullptr);
    m_modelInserts.clear();

    // 表格必须在块定义装配完之后还原：几何块可能出现在引用它之后。
    finishTables();

    // 块引用都装配好之后，DWG 路径攒下的 ATTRIB 才有定义可对齐
    attachPendingAttribs();

    if (m_orphanAttribs > 0) {
        m_skipped[QStringLiteral("ATTRIB(找不到所属块参照)")] = m_orphanAttribs;
    }
    if (m_missingBlocks > 0) {
        m_skipped[QStringLiteral("INSERT(引用的块未定义)")] = m_missingBlocks;
    }
    if (m_cyclicBlocks > 0) {
        m_skipped[QStringLiteral("INSERT(循环引用，已断开)")] = m_cyclicBlocks;
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

IoResult buildResult(Document& document, Builder& builder,
                     const QString& versionNote)
{
    IoResult result = IoResult::success(document.entityCount());

    if (!versionNote.isEmpty()) result.addWarning(versionNote);
    if (builder.blockCount() > 0) {
        result.addWarning(QStringLiteral("保留块定义 %1 个（块参照不再展开，可用 分解(EXPLODE) 打散）")
                              .arg(builder.blockCount()));
    }

    // 降级保留的（换了形式但内容在）与真正跳过的分开报，避免误导
    for (auto it = builder.downgradedTypes().constBegin();
         it != builder.downgradedTypes().constEnd(); ++it) {
        result.addWarning(QStringLiteral("降级 %1 个：%2").arg(it.value()).arg(it.key()));
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
    // 开发者开关：DWG 的字段序无法从接口看出，只能与同一张图的 DXF 逐字段对照
    // 来定标（新增实体类型时就是这么做的）。置 LYCAD_DWG_DEBUG=1 打开逐字段输出。
    if (qEnvironmentVariableIsSet("LYCAD_DWG_DEBUG")) reader.setDebug(DRW::DEBUG);
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
    IoResult result = buildResult(document, builder, QString());
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

    IoResult result = buildResult(document, builder, QStringLiteral("由 libdxfrw 读取"));
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
//    · 文字编码：不再写 \U+XXXX 转义，2000/2004 声明 ANSI_936 由库转 GBK，
//      2007 及以后直接写 UTF-8（各档均实测可被 ezdxf 正确读回）；
//    · 实测值 42（标注）不再写出，由读取端按定义点重推导。
// =============================================================================
namespace {

constexpr double kRadToDeg = 57.29577951308232;

/// 退化图形（单点、零长度）的兜底尺度：视高为 0 的视口等于没有视图
constexpr double kMinExtent = 1e-3;

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

/// 方向向量的写出形式（构造线/射线的 11 组码）
DRW_Coord toCoord(const Vector2D& v)
{
    return DRW_Coord(v.x, v.y, 0.0);
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

/// DXF/DWG 导出器：把 Document 翻译成 libdxfrw 写出回调。
///
/// 写出端以模板参数注入：DXF 走 dxfRW，DWG 走 dwgW——两者暴露同一套
/// writeXxx 方法（dwgW 是 dxfRW 写出侧的同构实现），Exporter 无需感知差异。
template <typename RW>
class ExporterBase final : public DRW_Interface {
public:
    ExporterBase(const Document& document, DRW::Version version)
        : m_doc(document)
        , m_version(version)
        , m_utf8Text(version >= DRW::AC1021)   // 2007 及以后：DXF 文本一律 UTF-8
    {}

    void setDxfRw(RW* rw) { m_rw = rw; }

    /// 预扫描：把文档实际引用到的块定义收集起来，并给每个定一个**文件内唯一**的名字。
    ///
    /// 必须遍历 referencedBlocks 而不是 blocks().all()：块参照在非相似变换下会把
    /// 几何烘焙进一份不在块表里的私有定义，只遍历表会把那些内容整块丢掉。
    /// 名字唯一化也不能改块定义本身（文档是 const 的，改了还会影响其它引用），
    /// 因此在写出侧维护一张「定义 -> 文件名」的映射。
    void prescanBlocks()
    {
        QSet<QString> used;
        const std::vector<std::shared_ptr<BlockDefinition>> blocks = m_doc.referencedBlocks();
        for (const std::shared_ptr<BlockDefinition>& block : blocks) {
            if (!block) continue;

            QString name = block->name().trimmed();
            if (name.isEmpty()) name = QStringLiteral("*U%1").arg(m_blockOrder.size() + 1);

            if (used.contains(BlockTable::normalizedKey(name))) {
                for (int i = 2;; ++i) {
                    const QString candidate = name + QString::number(i);
                    if (!used.contains(BlockTable::normalizedKey(candidate))) {
                        name = candidate;
                        break;
                    }
                }
            }

            used.insert(BlockTable::normalizedKey(name));
            m_blockWriteNames.emplace(block.get(), name.toStdString());
            m_blockOrder.push_back(block);
        }
    }

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

    /// 预扫描：为每个表格分配匿名几何块名（*T1、*T2……）。
    ///
    /// 表格以「插入点 + 旋转 + 行列尺寸 + 单元格文字」表达，DXF 侧没有等价
    /// 的可写实体（libdxfrw 不支持 ACAD_TABLE 写出）。因此走 AutoCAD 自己的
    /// 老路子：把网格线与单元格文字烘焙进一个匿名块，实体本体写成块参照。
    /// 这样本软件读回时按块参照还原（见 processTable），AutoCAD 也能正常显示。
    void prescanTableBlocks()
    {
        int index = 0;
        for (EntityId id : m_doc.drawOrder()) {
            const Entity* entity = m_doc.entity(id);
            if (!entity || entity->type() != EntityType::Table) continue;
            m_tableBlocks.emplace(
                entity->id(),
                QStringLiteral("*T%1").arg(++index).toStdString());
        }
    }

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
    void addAttdef(const DRW_Attdef&) override {}
    void addAttrib(const DRW_Attrib&) override {}
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
    void addDimArc(const DRW_DimArc*) override {}
    void addLeader(const DRW_Leader*) override {}
    void addHatch(const DRW_Hatch*) override {}
    void addViewport(const DRW_Viewport&) override {}
    void addImage(const DRW_Image*) override {}
    void linkImage(const DRW_ImageDef*) override {}
    void addComment(const char*) override {}

    // ---- 写侧 ----------------------------------------------------------------
    void writeHeader(DRW_Header& header) override
    {
        // $ACADVER/$HANDSEED 由库负责；$DWGCODEPAGE 决定写出编码。
        //
        //   · 2000 ~ 2004：文本按代码页存放。声明 ANSI_936（简中）后库会把
        //     UTF-8 字符串转成 GBK，这是简中环境的标准做法。
        //   · 2007 及以后：DXF 文本一律 UTF-8，$DWGCODEPAGE 不再参与解码。
        //     库此时不做任何编码转换，但若仍声明 ANSI_936，会误导按声明
        //     解码的读取器（本软件与 AutoCAD 都按版本判定，不受影响）。
        //     按惯例声明 ANSI_1252——AutoCAD 写 2007+ DXF 时正是这么写的。
        header.addStr("$DWGCODEPAGE", m_utf8Text ? "ANSI_1252" : "ANSI_936", 3);

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
        const Box2D box = drawingExtents();
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

    /// 文字样式表：把文档里的样式**原样写回**。
    ///
    /// 字体信息（主字体 3、大字体 4，以及 AutoCAD 记在 ACAD 扩展数据里的
    /// TrueType 族名）由导入时收集，图元只带样式名——所以这一步落下，
    /// 图元的 7 组码就成了悬空引用，CAD 只能退回默认的 `txt.shx`：那是没有
    /// 汉字字形的 SHX 字体，汉字会整片渲染成 `?`（多行文字走另一条取字路径，
    /// 于是同一张图里 MTEXT 正常、TEXT 全是问号，极易被误判成编码问题）。
    ///
    /// 文档没提供字体信息的样式（例如纯新建的图纸，或文件里只写了样式名）
    /// 一律补上「txt + gbcbig.shx」：两者都随 AutoCAD / GstarCAD 分发，
    /// 不依赖操作系统装了哪款中文字体，对全部可写版本都有效
    ///（大字体这条路不挑版本，只需目标 CAD 装了这两款随它分发的字体）。
    void writeTextstyles() override
    {
        const TextStyleTable& table = m_doc.textStyles();

        std::vector<QString> written;
        const auto writeStyle = [&](const QString& name) {
            const TextStyle fallback = TextStyleTable::cjkSafeStandard();
            const TextStyle* style = table.find(name);

            QString font = style ? style->fontFile() : QString();
            QString bigFont = style ? style->bigFontFile() : QString();
            const QString ttfName = style ? style->ttfName() : QString();

            // 需要兜底中文字体的情形：文档里根本没有这条样式，或它没带任何
            // 字体信息（3/4 组码为空、也没有 TrueType 族名）。
            const bool needsFallback = font.isEmpty() && bigFont.isEmpty() && ttfName.isEmpty();
            if (needsFallback) {
                font = fallback.fontFile();
                bigFont = fallback.bigFontFile();
            }

            DRW_Textstyle record;
            record.name = (style ? style->name() : name).toStdString();
            record.font = font.toStdString();
            record.bigFont = bigFont.toStdString();
            // 度量字段（DXF 40/41/50）与字体同样属于样式本体：宽度因子不写回，
            // 浩辰/AutoCAD 会按 1.0 重排全部文字（见 addTextStyle 里的说明）。
            // DRW_Textstyle 的默认值恰是 height=0 / width=1 / oblique=0，
            // 文档没有这条样式时保持默认即可。
            record.height = style ? style->fixedHeight() : 0.0;
            record.width = style ? style->widthFactor() : 1.0;
            record.oblique = style ? style->oblique() : 0.0;
            if (style && style->ttfName().isEmpty()) {
                // 只有字体族标志、没有族名（DWG 路径常见）：写在记录本体上
                record.fontFamily = style->ttfFlags();
            }
            // 3 组码为空、字体只存在于扩展数据时，必须把扩展数据一起写回
            // （libdxfrw-modify 已让写出侧支持表记录的扩展数据）。
            if (!ttfName.isEmpty()) {
                record.extData.push_back(new DRW_Variant(1001, std::string("ACAD")));
                record.extData.push_back(new DRW_Variant(1000, ttfName.toStdString()));
                if (style->ttfFlags() != 0) {
                    record.extData.push_back(new DRW_Variant(1071, style->ttfFlags()));
                }
            }
            m_rw->writeTextstyle(&record);
        };

        for (const TextStyle& style : table.all()) {
            writeStyle(style.name());
            written.push_back(style.name());
        }

        // 图元引用了表里没有的样式（删过样式表、或文件本身不自洽）：
        // 补一条同名的安全样式，避免图元挂在不存在的样式上。
        for (EntityId id : m_doc.drawOrder()) {
            const Entity* entity = m_doc.entity(id);
            if (!entity) continue;
            QString name;
            if (entity->type() == EntityType::Text) {
                name = static_cast<const TextEntity*>(entity)->fontStyle();
            } else if (entity->type() == EntityType::MText) {
                name = static_cast<const MTextEntity*>(entity)->fontStyle();
            } else {
                continue;
            }
            if (name.isEmpty()) continue;
            const bool already = std::any_of(
                written.begin(), written.end(),
                [&](const QString& n) { return n.compare(name, Qt::CaseInsensitive) == 0; });
            if (!already) {
                writeStyle(name);
                written.push_back(name);
            }
        }
    }

    // 库会自动补 Standard 标注样式 / ACAD 应用程序
    void writeDimstyles() override {}
    void writeAppId() override {}

    /// `*ACTIVE` 视口——它决定 CAD 打开本文件时**第一眼看到哪里**。
    ///
    /// 这一段不能省：`writeVports()` 为空时，libdxfrw 会自己补一个内置默认视口
    /// （视图中心 (0.651828, -0.16)、视高 5.13732），那是一个贴在原点的极小窗口。
    /// AutoCAD 系软件（含 GstarCAD / 浩辰CAD）打开图形时会直接采用 `*ACTIVE`
    /// 里记录的视图，于是画面停在原点附近，而图元通常远在视野之外——用户看到
    /// 的就是「打开是空白的」（实测：别人的文件能正常显示，往往只是因为它
    /// 的 `*ACTIVE` 视图恰好落在图形上）。
    /// 因此这里按图元范围写一个能装下全图的视图。
    void writeVports() override
    {
        const Box2D box = drawingExtents();
        const Point2D min = box.isValid() ? box.minCorner() : Point2D(0.0, 0.0);
        const Point2D max = box.isValid() ? box.maxCorner() : Point2D(420.0, 297.0);

        // 视口按 3:2 的横向绘图区估算；视高同时保证横竖两个方向都装得下整张图，
        // 再留 6% 边距——图形贴边和「空白」只有一线之隔，宁松勿紧。
        constexpr double kAspect = 1.5;
        constexpr double kMargin = 1.06;
        const double width = std::max(max.x - min.x, kMinExtent);
        const double height = std::max(max.y - min.y, kMinExtent);

        DRW_Vport viewport;
        viewport.name = "*ACTIVE";
        viewport.center.x = (min.x + max.x) / 2.0;
        viewport.center.y = (min.y + max.y) / 2.0;
        viewport.height = std::max(height, width / kAspect) * kMargin;
        viewport.ratio = kAspect;
        m_rw->writeVport(&viewport);
    }

    /// R2000 及以上的 BLOCK_RECORD 表：每个块都要在此登记，写出块体时才能对上句柄。
    /// 顺序必须与 writeBlocks 完全一致（库按写入次序分配句柄，块体写出时按名字查表）。
    void writeBlockRecords() override
    {
        for (const std::shared_ptr<BlockDefinition>& block : m_blockOrder) {
            m_rw->writeBlockRecord(blockNameOf(block));
        }
        for (const auto& [id, blockName] : m_dimBlocks) {
            Q_UNUSED(id);
            m_rw->writeBlockRecord(blockName);
        }
        for (const auto& [id, blockName] : m_tableBlocks) {
            Q_UNUSED(id);
            m_rw->writeBlockRecord(blockName);
        }
    }

    void writeBlocks() override
    {
        for (const std::shared_ptr<BlockDefinition>& block : m_blockOrder) {
            writeBlockDefinition(block);
        }
        for (const auto& [id, blockName] : m_dimBlocks) {
            Q_UNUSED(id);
            writeDimensionBlock(blockName);
        }
        for (const auto& [id, blockName] : m_tableBlocks) {
            Q_UNUSED(id);
            writeTableBlock(blockName);
        }
    }

    void writeEntities() override
    {
        for (EntityId id : m_doc.drawOrder()) {
            const Entity* entity = m_doc.entity(id);
            if (!entity) continue;
            writeEntity(*entity);
        }
    }

private:
    /// 单个图元写出——模型空间与块内**共用同一条分发**。
    /// 分开写两份 switch 意味着「块内的标注/多段线写法与模型空间不同」这种
    /// 缺陷不会有任何编译错误提示，只会体现在文件里。
    void writeEntity(const Entity& entity)
    {
        switch (entity.type()) {
        case EntityType::Line:      writeLine(static_cast<const LineEntity&>(entity)); break;
        case EntityType::Circle:    writeCircle(static_cast<const CircleEntity&>(entity)); break;
        case EntityType::Arc:       writeArc(static_cast<const ArcEntity&>(entity)); break;
        case EntityType::Polyline:  writePolyline(static_cast<const PolylineEntity&>(entity)); break;
        case EntityType::Point:     writePoint(static_cast<const PointEntity&>(entity)); break;
        case EntityType::Text:      writeText(static_cast<const TextEntity&>(entity)); break;
        case EntityType::MText:     writeMText(static_cast<const MTextEntity&>(entity)); break;
        case EntityType::Hatch:     writeHatch(static_cast<const HatchEntity&>(entity)); break;
        case EntityType::Dimension: writeDimension(static_cast<const DimensionEntity&>(entity)); break;
        case EntityType::Spline:    writeSplineCurve(static_cast<const SplineEntity&>(entity)); break;
        case EntityType::Ellipse:   writeEllipseCurve(static_cast<const EllipseEntity&>(entity)); break;
        case EntityType::XLine:     writeXLine(static_cast<const XLineEntity&>(entity)); break;
        case EntityType::Ray:       writeRay(static_cast<const RayEntity&>(entity)); break;
        case EntityType::Insert:    writeInsert(static_cast<const InsertEntity&>(entity)); break;
        case EntityType::Table:     writeTable(static_cast<const TableEntity&>(entity)); break;
        }
    }

    void writeBlockDefinition(const std::shared_ptr<BlockDefinition>& block)
    {
        if (!block) return;

        DRW_Block record;
        record.name = blockNameOf(block);
        record.flags = block->flags();
        applyCoord(record.basePoint, block->basePoint());
        m_rw->writeBlock(&record);

        // 属性定义（ATTDEF）不是几何，但它是块定义的一部分：不写回去，块参照
        // 就再也不知道自己该有哪些属性，往返一次属性块就退化成普通块。
        {
            for (const AttributeDefinition& definition : block->attributeDefinitions()) {
                DRW_Attdef record;
                record.layer = "0";
                record.color = 0;                 // ByBlock：随块参照着色
                record.lineType = "ByLayer";
                record.lWeight = DRW_LW_Conv::dxfInt2lineWidth(-3);
                record.visible = !definition.isInvisible();
                applyCoord(record.basePoint, definition.position);
                applyCoord(record.secPoint, definition.position);
                record.text = definition.defaultValue.toStdString();
                record.prompt = definition.prompt.toStdString();
                record.tag = definition.tag.toStdString();
                record.flags = definition.flags;
                record.height = definition.height;
                record.angle = definition.rotation * kRadToDeg;   // DRW_Text 的 50 组码按度
                record.style = (definition.style.isEmpty() ? QStringLiteral("Standard")
                                                           : definition.style)
                                   .toStdString();
                m_rw->writeAttdef(&record);
            }
        }

        // 块内图元按原图层/颜色写出——**不做**图层继承换算：继承是块参照在
        // 展开时的事（InsertEntity::collectInto），文件里要保留 0 层的原始归属。
        for (const std::unique_ptr<Entity>& entity : block->entities()) {
            if (entity) writeEntity(*entity);
        }
    }

    /// 块定义 -> 写出用的名字。名字在预扫描时定好并做过唯一化，
    /// 这里不能临时用 block->name()：两份匿名副本可能恰好同名。
    const std::string& blockNameOf(const std::shared_ptr<BlockDefinition>& block) const
    {
        static const std::string kEmpty;
        if (!block) return kEmpty;
        const auto it = m_blockWriteNames.find(block.get());
        return it == m_blockWriteNames.end() ? kEmpty : it->second;
    }

    /// 块参照（INSERT）。带属性时还要写出 66=1、随后若干 ATTRIB 与 SEQEND，
    /// 否则 AutoCAD 会把属性文字整段丢掉。
    void writeInsert(const InsertEntity& insert)
    {
        const std::string name = blockNameOf(insert.block());
        if (name.empty()) return;      // 块未定义：写出去就是悬空引用

        const bool hasAttributes = !insert.attributes().empty();

        DRW_Insert record;
        applyCommonWrite(record, insert);
        record.name = name;
        record.hasAttribs = hasAttributes;
        // DWG 的 INSERT 必须在写出时就填好 first_attrib / last_attrib / seqend 三个
        // 句柄，句柄得按条数提前分配——所以条数要在写 INSERT 之前交出去。
        record.attribCount = static_cast<int>(insert.attributes().size());
        applyCoord(record.basePoint, insert.insertionPoint());
        record.xscale = insert.scaleX();
        record.yscale = insert.scaleY();
        record.zscale = insert.scaleZ();
        // DRW_Insert::angle 的约定与其它实体不同：**弧度**。
        // 读侧 parseCode 收 50 组码后除以 ARAD 得弧度，写侧 dxfRW::writeInsert
        // 再乘 ARAD 还原成度——转换在库里做完了，这里若再乘一次 kRadToDeg，
        // 往返一次旋转就变成「度当弧度用」（实测 311.34° 变成 198.69°）。
        record.angle = insert.rotation();
        record.colcount = insert.columnCount();
        record.rowcount = insert.rowCount();
        record.colspace = insert.columnSpacing();
        record.rowspace = insert.rowSpacing();
        m_rw->writeInsert(&record);

        if (!hasAttributes) return;

        // 属性条数必须与 attribCount 严格一致（DWG 的句柄是照这个数预分配的，
        // 少写一条就会留下一个悬空句柄），因此空值也照写。
        for (const Attribute& attribute : insert.attributes()) {
            DRW_Attrib attrib;
            attrib.layer = (attribute.layer.isEmpty() ? insert.layerName() : attribute.layer)
                               .toStdString();
            attrib.color = insert.isColorByLayer() ? 256 : insert.colorIndex();
            attrib.lineType = "ByLayer";
            attrib.lWeight = DRW_LW_Conv::dxfInt2lineWidth(-3);
            attrib.visible = !attribute.isInvisible();
            applyCoord(attrib.basePoint, attribute.position);
            applyCoord(attrib.secPoint, attribute.position);
            attrib.text = attribute.value.toStdString();
            attrib.height = attribute.height;
            attrib.angle = attribute.rotation * kRadToDeg;
            attrib.style = (attribute.style.isEmpty() ? QStringLiteral("Standard")
                                                      : attribute.style)
                               .toStdString();
            attrib.tag = attribute.tag.toStdString();
            attrib.flags = attribute.flags;
            m_rw->writeAttrib(&attrib);
        }
        m_rw->writeSeqEnd(&record);
    }

    /// 表格（ACAD_TABLE）。
    ///
    /// 两条写出策略：
    ///   · DXF：写出**真正的 ACAD_TABLE 实体**（AcDbBlockReference 子类），
    ///     本体带完整行列结构与单元格文字，同时引用预先烘焙好的 *Tnn 几何块
    ///     （旧软件靠它显示）。AutoCAD 打开即为可编辑的表格对象。
    ///   · DWG：同样写出原生 ACAD_TABLE 类实体（dwgW::writeTable 已按实测位
    ///     布局实现 AcDbBlockReference + AcDbTable 两段，单元格文字随本体
    ///     逐格写出，并在 CLASSES 段登记 AcDbTable）。几何（网格线 + 单元格
    ///     文字）仍完整保留在 *Tnn 匿名块里，由本实体的 block_header(H2) 引用
    ///     —— 与 AutoCAD 的原生写法一致。
    ///
    /// ★ 单元格与合并区两条支路都必须填进 record：DWG 分支若漏填，写出的
    ///   表格本体全是空格（cell type=0）——显示端靠 *Tnn 块照常显示，但
    ///   双击进入表格编辑器后内容模型为空，退出即把文字抹掉
    ///   （实测浩辰 CAD 就是这样丢字的）。
    void writeTable(const TableEntity& table)
    {
        const auto it = m_tableBlocks.find(table.id());
        if (it == m_tableBlocks.end()) return;   // 未预扫描：写出去就是悬空引用

        DRW_Table record;
        applyCommonWrite(record, table);
        record.name = it->second;
        record.tableStyle = "Standard";
        applyCoord(record.basePoint, table.insertionPoint());
        record.xscale = 1.0;
        record.yscale = 1.0;
        record.zscale = 1.0;
        record.angle = table.rotation();      // 弧度（DXF 写出时库再乘 ARAD 转度）

        record.numRows = static_cast<int>(table.rowCount());
        record.numCols = static_cast<int>(table.columnCount());
        record.rowHeights = table.rowHeights();
        record.colWidths = table.columnWidths();
        for (const auto& [row, col, cell] : table.nonEmptyCells()) {
            DRW_Table::Cell record2;
            record2.row = row;
            record2.col = col;
            record2.text = cell.text.toStdString();
            record.cells.push_back(record2);
        }
        for (const TableMergeRange& merge : table.merges()) {
            DRW_Table::Merge m;
            m.row = merge.row;
            m.col = merge.col;
            m.rowSpan = merge.rowSpan;
            m.colSpan = merge.colSpan;
            record.merges.push_back(m);
        }
        m_rw->writeTable(&record);
    }

private:
    /// 表格的匿名几何块：网格线（LINE）+ 单元格文字（TEXT），图层 0、颜色 ByBlock，
    /// 与标注块同一套约定——随表格实体着色。
    void writeTableBlock(const std::string& name)
    {
        const TableEntity* table = nullptr;
        for (const auto& [id, blockName] : m_tableBlocks) {
            if (blockName == name) {
                table = static_cast<const TableEntity*>(m_doc.entity(id));
                break;
            }
        }
        if (!table) return;

        DRW_Block block;
        block.name = name;
        block.flags = 1;                          // bit 1 = 匿名块
        applyCoord(block.basePoint, Point2D(0.0, 0.0));
        m_rw->writeBlock(&block);

        auto beginBlockEntity = [&](DRW_Entity& entity) {
            entity.layer = "0";
            entity.color = 0;                     // ByBlock：随表格实体着色
            entity.lineType = "ByLayer";
            entity.lWeight = DRW_LW_Conv::dxfInt2lineWidth(-3);
        };

        // 网格线：块内坐标即表格局部坐标（插入点在实体上，块基点取原点）
        for (const auto& seg : table->gridLinesLocal()) {
            DRW_Line line;
            beginBlockEntity(line);
            applyCoord(line.basePoint, seg.first);
            applyCoord(line.secPoint, seg.second);
            m_rw->writeLine(&line);
        }

        // 单元格文字：按对齐方式给出基线锚点，与渲染期的定位保持一致
        const std::vector<double> rows = table->rowBoundaries();
        const std::vector<double> cols = table->columnBoundaries();
        for (const auto& [row, col, cell] : table->nonEmptyCells()) {
            if (cell.text.isEmpty()) continue;
            if (row < 0 || col < 0) continue;
            const std::size_t r = static_cast<std::size_t>(row);
            const std::size_t c = static_cast<std::size_t>(col);
            if (r + 1 >= rows.size() || c + 1 >= cols.size()) continue;

            const double top = rows[r];
            const double bottom = rows[r + 1];
            const double left = cols[c];
            const double right = cols[c + 1];
            const double midY = (top + bottom) * 0.5;

            const double boxH = std::abs(top - bottom);
            const double boxW = std::abs(right - left);
            // 文字高度：单元格高度的 60%，再按最长行文字宽度收敛，避免溢出格子
            const QStringList textLines = cell.text.split(QLatin1Char('\n'));
            double height = boxH * 0.6;
            const int longest = [&] {
                int n = 1;
                for (const QString& l : textLines) n = std::max(n, static_cast<int>(l.size()));
                return n;
            }();
            if (longest > 0 && boxW > 0.0) {
                // 粗估：平均字宽约 0.6 倍字高
                height = std::min(height, boxW / (0.6 * static_cast<double>(longest)));
            }
            if (height <= kGeomTol) continue;

            // 对齐 -> 水平锚点与基线 Y（块内局部坐标，Y 向下为负）
            const bool toLeft = cell.align == TableCellAlign::TopLeft
                || cell.align == TableCellAlign::MiddleLeft
                || cell.align == TableCellAlign::BottomLeft;
            const bool toRight = cell.align == TableCellAlign::TopRight
                || cell.align == TableCellAlign::MiddleRight
                || cell.align == TableCellAlign::BottomRight;
            const bool toTop = cell.align == TableCellAlign::TopLeft
                || cell.align == TableCellAlign::TopCenter
                || cell.align == TableCellAlign::TopRight;
            const bool toBottom = cell.align == TableCellAlign::BottomLeft
                || cell.align == TableCellAlign::BottomCenter
                || cell.align == TableCellAlign::BottomRight;

            const double anchorX = toLeft ? left + boxW * 0.05 : (toRight ? right - boxW * 0.05 : (left + right) * 0.5);
            double baselineY = toTop ? top - height
                                     : (toBottom ? bottom : midY - height * 0.5);

            for (int i = 0; i < textLines.size(); ++i) {
                if (textLines[i].isEmpty()) continue;
                DRW_Text text;
                beginBlockEntity(text);
                text.text = textLines[i].toStdString();
                text.height = height;
                text.angle = 0.0;
                text.widthscale = 1.0;
                text.style = "Standard";
                applyCoord(text.basePoint, Point2D(anchorX, baselineY));
                applyCoord(text.secPoint, Point2D(anchorX, baselineY));
                if (toLeft || toRight || (cell.align != TableCellAlign::MiddleCenter
                                          && cell.align != TableCellAlign::TopCenter
                                          && cell.align != TableCellAlign::BottomCenter)) {
                    // 非左对齐（含居中/右对齐）必须给对齐点，否则 TEXT 会忽略 alignH
                    text.alignH = toRight ? DRW_Text::HRight
                                          : (toLeft ? DRW_Text::HLeft : DRW_Text::HCenter);
                    text.alignV = DRW_Text::VBaseLine;
                    applyCoord(text.secPoint, Point2D(anchorX, baselineY));
                }
                m_rw->writeText(&text);
                baselineY -= height * 1.2;        // 多行向下串行（块内 Y 轴向上为正）
            }
        }
    }

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
        // 一律走 LWPOLYLINE（R2000 起）：顶点与凸度压在一个实体里，
        // 宽度写 43 组码。R12 的 POLYLINE/VERTEX 形态已随 R12 导出一起移除。
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
    }

    /// 多行文字的 MTEXT 附着点（1~9）：行 Top=1 / Middle=4 / Bottom=7，
    /// 列 Left=+0 / Center=+1 / Right=+2。与读侧 addMText 的映射互逆。
    static int mtextAttachment(const TextEntity& text)
    {
        const int row = text.vAlign() == TextEntity::VAlign::Top    ? 1
                        : text.vAlign() == TextEntity::VAlign::Middle ? 4
                                                                      : 7;
        const int col = text.hAlign() == TextEntity::HAlign::Center ? 1
                        : text.hAlign() == TextEntity::HAlign::Right  ? 2
                                                                      : 0;
        return row + col;
    }

    void writeText(const TextEntity& text)
    {
        const QString content = text.displayString();
        if (content.isEmpty()) {
            // 空文字不是合法的 DXF 实体，跳过而非产出坏文件
            return;
        }

        // 样式名回写（DXF 7）：字体的全部信息都在 STYLE 表里，图元只写引用，
        // 由 writeTextstyles() 保证这条样式确实存在于同一文件。
        // 导入时 Standard 会被清空（那就是默认样式），这里落回 Standard。
        const std::string styleName =
            (text.fontStyle().isEmpty() ? QStringLiteral("Standard") : text.fontStyle())
                .toStdString();

        // 多行文字（读侧 MTEXT 降级留下的 \n）写回为**真正的 MTEXT**：
        // DXF 的一个组码值不能跨行，把含 \n 的字符串写进 TEXT 的 1 组码
        // 会产出非法文件（实测 AutoCAD 直接拒绝打开，ezdxf 也报
        // "Invalid group code"）——这正是 MTEXT 用 \P 表示段落的原因。
        if (content.contains(QLatin1Char('\n'))) {
            DRW_MText record;
            applyCommonWrite(record, text);
            applyCoord(record.basePoint, text.effectivePosition());
            record.text = QString(content)
                              .replace(QLatin1Char('\r'), QString())
                              .replace(QLatin1Char('\n'), QStringLiteral("\\P"))
                              .toStdString();
            record.height = text.height();
            record.textgen = mtextAttachment(text);
            // MTEXT 的 72/73 与 TEXT 的对齐枚举不同义：72 是流向
            // （1=从左到右），73 是行距风格（1=至少）。按 MTEXT 规范写。
            record.alignH = static_cast<DRW_Text::HAlign>(1);
            record.alignV = static_cast<DRW_Text::VAlign>(1);
            record.interlin = 1.0;
            record.widthscale = 0.0;           // 41=0：无宽度限制（对方 CAD 打开时也不会重新折行）
            record.angle = text.rotation() * kRadToDeg;
            record.style = styleName;
            m_rw->writeMText(&record);
            return;
        }

        DRW_Text record;
        applyCommonWrite(record, text);
        applyCoord(record.basePoint, text.position());
        record.secPoint = record.basePoint;
        // TEXT 的值必须是单行：走到这里说明内容本来就没有换行，
        // 这里再折一次只是兜底（合法文件里不会出现跨行的组码值）
        record.text = QString(content)
                          .replace(QLatin1Char('\r'), QString())
                          .replace(QLatin1Char('\n'), QLatin1Char(' '))
                          .toStdString();
        record.height = text.height();
        record.angle = text.rotation() * kRadToDeg;
        record.widthscale = text.widthFactor();
        record.style = styleName;
        record.textgen = (text.isBackwards() ? 2 : 0) | (text.isUpsideDown() ? 4 : 0);
        if (text.usesAlignmentPoint()) {
            record.alignH = toDrwHAlign(text.hAlign());
            record.alignV = toDrwVAlign(text.vAlign());
            applyCoord(record.secPoint, text.alignmentPoint());
        }
        m_rw->writeText(&record);
    }

    /// 多行文字的写出：按 MTEXT 自身的组码语义逐字段回写。
    ///
    /// 与 TEXT 的区别（也是不能共享一条通道的原因）：
    ///   · 71 = 附着点（1~9），不是对齐枚举；
    ///   · 72 = 书写方向、73 = 行距样式、44 = 行距系数；
    ///   · 11 = X 轴方向向量（旋转的唯一来源，DWG 与 DXF 两条路径同义）；
    ///   · 41 = 参考矩形宽度（编辑时的折行宽度，0 = 无限制；显示不改内容）；
    ///   · 内容原样带出——内联格式码（\P、\f、\H…）由模型逐字保存，不再改写。
    void writeMText(const MTextEntity& text)
    {
        if (text.content().isEmpty()) {
            // 空文字不是合法的 DXF 实体，跳过而非产出坏文件
            return;
        }

        const std::string styleName =
            (text.fontStyle().isEmpty() ? QStringLiteral("Standard") : text.fontStyle())
                .toStdString();

        DRW_MText record;
        applyCommonWrite(record, text);
        applyCoord(record.basePoint, text.insertionPoint());

        record.height = text.height();
        record.widthscale = text.referenceWidth();
        record.textgen = static_cast<int>(text.attachment());
        record.alignH = static_cast<DRW_Text::HAlign>(static_cast<int>(text.drawingDirection()));
        record.alignV = static_cast<DRW_Text::VAlign>(static_cast<int>(text.lineSpacingStyle()));
        record.interlin = text.lineSpacingFactor();
        record.text = text.content().toStdString();
        record.style = styleName;

        // 210 挤出方向：原样回写（几何按 XY 平面计算，这里只保证文件一致）
        const MTextEntity::Normal3D normal = text.normal();
        record.extPoint = DRW_Coord(normal.x, normal.y, normal.z);

        // 旋转：DWG 写出路径由 angle（度）反算方向向量；DXF 路径由 11 组码带出
        record.angle = text.rotation() * kRadToDeg;
        const Vector2D direction = text.direction();
        record.setXAxisDirection(DRW_Coord(direction.x, direction.y, 0.0));

        m_rw->writeMText(&record);
    }

    void writeHatch(const HatchEntity& hatch)
    {
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

        // 图案定义线：一条都不能少。上游写出侧只写条数（78 组码）而不写内容，
        // 于是非实体填充成了「有图案名、无定义」，实测 GstarCAD 会把它画成一大片
        // 乱纹盖住整张图（点一下重生成才恢复）。这里按图元保存的定义如实回写；
        // 文件里本来就没带定义的（例如本软件自己新建的填充），补一条与自身渲染
        // 一致的 45° 平行线，保证导出的文件在任何 CAD 里都画得出来。
        // 定义线按 DXF 的约定改写两处，实测量产 CAD（GstarCAD）只认这种写法：
        //   · 基点写原点——它只决定图案相位；照抄源图的远基点会被画成一大片乱纹。
        //   · 偏移写进「图案线坐标系」（X 轴沿图案线方向），也就是 AutoCAD 写
        //     ANSI31 时的 (0, 间距) 形式。源文件里的偏移是世界坐标下的垂直向量
        //     （例如 45° 线上的 (-1.1225, 1.1225)），照抄同样会让 GstarCAD 把
        //     填充画成乱纹：删掉全部填充就恢复正常，补上这处换算也恢复正常。
        const double writeScale = hatch.patternScale() > kGeomTol ? hatch.patternScale() : 1.0;
        for (const HatchEntity::PatternLine& line : hatch.patternLines()) {
            DRW_HatchDefLine def;
            def.angle  = line.angle;
            def.base   = DRW_Coord(0.0, 0.0, 0.0);
            // 写出的偏移是「图案线坐标系」里的 (0, 间距/比例)：模型里存的是最终
            // 间距，而 DXF 的 41 组码会把定义整体缩放，所以这里先除掉比例。
            // 直接照抄源文件的世界坐标偏移（斜向向量）同样会让 GstarCAD 画成乱纹。
            def.offset = DRW_Coord(0.0, hatch.definedSpacing() / writeScale, 0.0);
            def.dashes = line.dashes;
            record.deflist.push_back(std::move(def));
        }
        if (!hatch.isSolid() && record.deflist.empty()) {
            DRW_HatchDefLine def;
            def.angle  = HatchEntity::kAnsi31BaseAngle;
            def.base   = DRW_Coord(0.0, 0.0, 0.0);
            def.offset = DRW_Coord(0.0, hatch.patternSpacing() / writeScale, 0.0);
            record.deflist.push_back(std::move(def));
        }
        record.deflines = static_cast<int>(record.deflist.size());

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

    /// 样条：按原生 SPLINE 写出
    void writeSplineCurve(const SplineEntity& spline)
    {
        DRW_Spline record;
        applyCommonWrite(record, spline);
        record.degree = spline.degree();
        record.flags = spline.isClosed() ? 1 : 0;
        record.normalVec = DRW_Coord(0.0, 0.0, 1.0);
        record.knotslist = spline.knots();
        for (const Point2D& p : spline.controlPoints()) {
            record.controllist.push_back(new DRW_Coord(p.x, p.y, 0.0));
        }
        record.nknots = static_cast<int>(record.knotslist.size());
        record.ncontrol = static_cast<int>(record.controllist.size());
        record.nfit = 0;
        m_rw->writeSpline(&record);
    }

    /// 椭圆：按原生 ELLIPSE 写出
    void writeEllipseCurve(const EllipseEntity& ellipse)
    {
        DRW_Ellipse record;
        applyCommonWrite(record, ellipse);
        applyCoord(record.basePoint, ellipse.center());
        const Point2D& axis = ellipse.majorAxisEnd();
        record.secPoint = DRW_Coord(axis.x, axis.y, 0.0);
        record.ratio = ellipse.ratio();
        record.staparam = ellipse.startParam();
        record.endparam = ellipse.endParam();
        m_rw->writeEllipse(&record);
    }

    /// 构造线：按原生 XLINE 写出（基点 + 单位方向向量，11 组码）
    void writeXLine(const XLineEntity& line)
    {
        DRW_Xline record;
        applyCommonWrite(record, line);
        applyCoord(record.basePoint, line.basePoint());
        record.secPoint = toCoord(line.direction());
        m_rw->writeXline(&record);
    }

    /// 射线：与构造线同构，按原生 RAY 写出
    void writeRay(const RayEntity& ray)
    {
        DRW_Ray record;
        applyCommonWrite(record, ray);
        applyCoord(record.basePoint, ray.basePoint());
        record.secPoint = toCoord(ray.direction());
        m_rw->writeRay(&record);
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
            diametric->setLeaderLength(dim.textHeight());
            record = std::move(diametric);
            break;
        }
        case DimensionEntity::DimType::Ordinate: {
            auto ordinate = std::make_unique<DRW_DimOrdinate>();
            // 10 是坐标基准（量的是被测点相对基准的轴向坐标），13 被测点，14 引线端点
            ordinate->setOriginPoint(toCoord(dim.dimLinePoint()));
            ordinate->setFirstLine(toCoord(dim.firstPoint()));
            ordinate->setSecondLine(toCoord(dim.secondPoint()));
            // 70 的低 3 位是类型（6=坐标），bit6（64）置位表示 X 型
            ordinate->type = static_cast<int>(DimensionEntity::DimType::Ordinate)
                             | (dim.isOrdinateXType() ? 64 : 0);
            record = std::move(ordinate);
            break;
        }
        case DimensionEntity::DimType::ArcLength: {
            // 弧长标注不是 DIMENSION 的变体，实体名也不同，单独走一条写出路径
            auto arc = std::make_unique<DRW_DimArc>();
            arc->setDefPoint(toCoord(dim.dimLinePoint()));      // 10 尺寸弧经过的点
            arc->setXLine1Point(toCoord(dim.firstPoint()));     // 13 被测弧起点
            arc->setXLine2Point(toCoord(dim.secondPoint()));    // 14 被测弧终点
            arc->setCenterPoint(toCoord(dim.arcCenter()));      // 15 弧心
            arc->setArcStartParam(dim.arcStartAngle());         // 40 弧度
            arc->setArcEndParam(dim.arcEndAngle());             // 41 弧度
            arc->setSubFlags(0);
            arc->setHasLeader(0);
            arc->type = static_cast<int>(DimensionEntity::DimType::ArcLength);
            arc->setMeasurement(dim.measurement());
            applyCommonWrite(*arc, dim);
            if (blockIt != m_dimBlocks.end()) arc->setName(blockIt->second);
            // 文件里给了 11 组码就用它：自己重算的锚点与图里写的会差在最后一位
            // （实测 1e-12 量级），往返多次也不会漂。
            arc->setTextPoint(toCoord(dim.textPoint().value_or(geo.textAnchor)));
            arc->setStyle("Standard");
            if (!dim.textOverride().isEmpty() && dim.textOverride() != QLatin1String("<>")) {
                arc->setText(dim.textOverride().toStdString());
            }
            m_rw->writeArcDimension(arc.release());
            return;
        }
        default:
            return;
        }

        applyCommonWrite(*record, dim);
        if (blockIt != m_dimBlocks.end()) record->setName(blockIt->second);
        record->setTextPoint(toCoord(geo.textAnchor));
        // bit 32 表示「几何块引用已在文件中」，读取端无需自行重绘。
        // 这里只**补**这一位，不覆盖各类型自己写进 70 的值：坐标标注的 70 里
        // 还带着 bit6（X 型 / Y 型），直接赋值会把它抹掉——实测症状是
        // 「X 型坐标标注往返一次就变成 Y 型，量出来的数从 X 坐标变成 Y 坐标」。
        if (record->type == 0) record->type = static_cast<int>(dim.dimType());
        record->type |= 32;
        record->setMeasurement(dim.measurement());
        record->setStyle("Standard");
        if (!dim.textOverride().isEmpty() && dim.textOverride() != QLatin1String("<>")) {
            record->setText(dim.textOverride().toStdString());
        }
        m_rw->writeDimension(record.release());
    }

    /// 全图图元范围。`$EXTMIN/$EXTMAX` 与 `*ACTIVE` 视口必须用同一个范围，
    /// 否则「缩放到范围」与实际显示对不上；因此只算一次并缓存。
    const Box2D& drawingExtents()
    {
        if (m_extentsReady) return m_extents;
        for (EntityId id : m_doc.drawOrder()) {
            if (const Entity* e = m_doc.entity(id)) {
                if (!e->isDisplayable(m_doc.layers())) continue;
                m_extents.expand(e->boundingBox());
            }
        }
        m_extentsReady = true;
        return m_extents;
    }

    /// 写出目标是否为 DWG（dwgW）。属性对象只在 DXF 写出端存在。
    static constexpr bool kIsDwgWriter = std::is_same_v<RW, dwgW>;

    const Document& m_doc;
    RW* m_rw = nullptr;
    DRW::Version m_version = DRW::AC1015;
    /// 目标版本的 DXF 文本是否按 UTF-8 存放（AC1021 及以后为真）
    bool m_utf8Text = false;
    std::map<EntityId, std::string> m_dimBlocks;
    /// 表格实体的匿名几何块名（*T1、*T2……）
    std::map<EntityId, std::string> m_tableBlocks;
    /// 参与写出的块定义（文档引用的 + 块表里的），以及它们在文件里的名字
    std::vector<std::shared_ptr<BlockDefinition>> m_blockOrder;
    std::map<const BlockDefinition*, std::string> m_blockWriteNames;
    Box2D m_extents;
    bool m_extentsReady = false;
};

/// DXF 导出沿用 dxfRW 写出端
using Exporter = ExporterBase<dxfRW>;

} // namespace

IoResult writeDxfWithLibDxfRw(const QString& path, const Document& document,
                              const WriteOptions& options)
{
    // 版本归一化：认得出的可写版本走各自的目标版本，其余一律退回 AC1015。
    // 调用方 DxfCodec::write 已按 WriteOptions::availableDxfVersions() 归一过一次，
    // 这里再兜一次底，保证直接调用本函数的场景也不会写出版本不明的文件。
    // 注意 AC1009 (R12) 已不在可写清单里，传进来同样会落到 AC1015——
    // R12 的降级写出通道已随「移除 R12 导出」删除（可读不可写）。
    DRW::Version version = writableVersionFromCode(options.version);
    if (version == DRW::UNKNOWNV) version = DRW::AC1015;

    Exporter exporter(document, version);
    exporter.prescanBlocks();
    exporter.prescanDimensionBlocks();
    exporter.prescanTableBlocks();

    const QByteArray native = QFile::encodeName(path);
    dxfRW writer(native.constData());
    exporter.setDxfRw(&writer);

    const bool ok = writer.write(&exporter, version, /*bin=*/false);
    if (!ok) {
        return IoResult::failure(QStringLiteral("DXF 写出失败（libdxfrw）。"));
    }

    return IoResult::success(document.entityCount());
}

// -----------------------------------------------------------------------------
//  DWG 原生写出（libdxfrw-modify 的 dwgW，目标版本恒为 R2000/AC1015）
// -----------------------------------------------------------------------------
IoResult writeDwgWithLibDxfRw(const QString& path, const Document& document,
                              const WriteOptions& options)
{
    // 目标版本由 WriteOptions::dwgVersion 决定：AC1018（AutoCAD 2004 / R2004）
    // 走分页 + LZ77 容器（intern/dwgwriter18.cpp），AC1015（AutoCAD 2000 /
    // R2000）走线性容器（intern/dwgwriter15.cpp）。非法取值回退到 AC1015。
    const bool wantAc1018 =
        options.dwgVersion.compare(QStringLiteral("AC1018"), Qt::CaseInsensitive) == 0;
    const DRW::Version target = wantAc1018 ? DRW::AC1018 : DRW::AC1015;

    ExporterBase<dwgW> exporter(document, target);
    exporter.prescanBlocks();
    exporter.prescanDimensionBlocks();
    exporter.prescanTableBlocks();

    const QByteArray native = QFile::encodeName(path);
    dwgW writer(native.constData());
    exporter.setDxfRw(&writer);

    const bool ok = writer.write(&exporter, target);
    if (!ok) {
        return IoResult::failure(
            QStringLiteral("DWG 写出失败（libdxfrw dwgW，%1）。")
                .arg(wantAc1018 ? QStringLiteral("AC1018/R2004")
                                : QStringLiteral("AC1015/R2000")));
    }

    return IoResult::success(document.entityCount());
}

} // namespace lycad
