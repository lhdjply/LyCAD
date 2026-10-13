// =============================================================================
//  LyCAD - core/Entity.h
//  图元抽象基类。
//
//  设计要点
//    · Entity 只描述「几何 + 显示属性」，不持有任何视图状态，也不依赖 QWidget，
//      因此可被 io / render / interact 三个模块共享而不产生反向依赖。
//    · 所有几何查询（包围盒、拾取距离、框选判定、离散化）通过虚函数下发到
//      具体图元，渲染层与交互层无需对图元类型做 switch 分支。
//    · 变换（平移/旋转/缩放/镜像）由图元自行实现，因为圆弧在镜像下需要
//      反转角度方向，无法用统一的矩阵后处理完成。
// =============================================================================
#pragma once

#include "core/AciPalette.h"
#include "core/Types.h"

#include <QColor>
#include <QString>

#include <memory>
#include <vector>

namespace lycad {

class LayerManager;

// -----------------------------------------------------------------------------
//  图元类型
// -----------------------------------------------------------------------------
enum class EntityType {
    Point,
    Line,
    Circle,
    Arc,
    Polyline,
    Text,
    MText,      ///< 多行文字（DXF: MTEXT）——附着点 + 参考矩形宽度的多行排版文字
    Hatch,
    Dimension,
    Spline,
    Ellipse,
    XLine,      ///< 构造线（DXF: XLINE）——过基点双向无限延伸
    Ray,        ///< 射线（DXF: RAY）——自基点沿方向单向无限延伸
    Insert,     ///< 块参照（DXF: INSERT / MINSERT）——对块定义的带变换引用
    Table,      ///< 表格（DXF: ACAD_TABLE）——自身参数化网格，非块引用
    Light       ///< 光源（DXF: LIGHT）——光度控制光源，本体没有几何，只有屏幕标记
};

/// 英文类型名（DXF 中的实体名，如 "LINE"）
QString entityTypeName(EntityType type);

/// 中文显示名（界面与命令行使用）
QString entityTypeDisplayName(EntityType type);

// -----------------------------------------------------------------------------
//  线宽
// -----------------------------------------------------------------------------
namespace lineweight {

inline constexpr int kByLayer = -1;
inline constexpr int kByBlock = -2;
inline constexpr int kDefault = -3;

/// DXF 370 分组值 -> 毫米。ByLayer/ByBlock/Default 返回负值表示未定。
double toMillimeters(int dxfValue);

/// 供属性面板显示
QString toDisplayString(int dxfValue);

/// 由毫米值转为最接近的 DXF 370 合法取值
int fromMillimeters(double mm);

} // namespace lineweight

// -----------------------------------------------------------------------------
//  Entity
// -----------------------------------------------------------------------------
class Entity {
public:
    explicit Entity(EntityType type) noexcept : m_type(type) {}
    virtual ~Entity() = default;

    Entity(const Entity&) = delete;
    Entity& operator=(const Entity&) = delete;

    // 图元天然是多态的，拷贝只能通过 clone() 进行（返回基类指针）。
    // 移动语义则必须显式保留：std::optional<派生图元>、容器重分配等场景都依赖它，
    // 而删除拷贝构造会连带抑制隐式移动构造，故在此显式声明。
    Entity(Entity&&) noexcept = default;
    Entity& operator=(Entity&&) noexcept = default;

    // ---- 标识 ----------------------------------------------------------------
    EntityId id() const noexcept { return m_id; }
    void setId(EntityId id) noexcept { m_id = id; }

    EntityType type() const noexcept { return m_type; }
    QString typeName() const { return entityTypeName(m_type); }
    QString typeDisplayName() const { return entityTypeDisplayName(m_type); }

    // ---- 图层归属 ------------------------------------------------------------
    const QString& layerName() const noexcept { return m_layerName; }
    void setLayerName(const QString& name) { m_layerName = name; }

    // ---- 颜色（ACI 索引，<0 表示随层，0 表示随块） ------------------------------
    bool isColorByLayer() const noexcept { return m_colorIndex < 0; }
    /// 随块（DXF 颜色 0）：颜色由引用它的块参照决定，块定义内的图元常用它。
    /// 顶层图元若也是随块，解析时退回随层（与 AutoCAD 的兜底一致）。
    bool isColorByBlock() const noexcept { return m_colorIndex == aci::kByBlock; }
    int  colorIndex() const noexcept { return m_colorIndex; }
    void setColorIndex(int aci) noexcept { m_colorIndex = aci; }
    void setColorByLayer() noexcept { m_colorIndex = -1; }
    void setColorByBlock() noexcept { m_colorIndex = aci::kByBlock; }

    /// 解析出最终显示颜色（考虑随层继承与主题反相）
    QColor resolvedColor(const LayerManager& layers, bool themeIsDark = false) const;

    // ---- 线宽 ----------------------------------------------------------------
    int lineweightDxf() const noexcept { return m_lineweight; }
    void setLineweightDxf(int v) noexcept { m_lineweight = v; }
    bool isLineweightByLayer() const noexcept { return m_lineweight < 0; }

    /// 解析出最终线宽（毫米）
    double resolvedLineweight(const LayerManager& layers, double defaultMm = 0.25) const;

    // ---- 线型 ----------------------------------------------------------------
    const QString& linetypeName() const noexcept { return m_linetypeName; }
    void setLinetypeName(const QString& name);
    bool isLinetypeByLayer() const noexcept { return m_linetypeName.isEmpty(); }

    // ---- 可见性 --------------------------------------------------------------
    bool isVisible() const noexcept { return m_visible; }
    void setVisible(bool v) noexcept { m_visible = v; }

    /// 综合图层状态判断是否应当绘制（图层关闭/冻结时不绘制）
    bool isDisplayable(const LayerManager& layers) const;

    // ---- 无界图元 -------------------------------------------------------------
    /// 是否为无界图元（构造线、射线）。
    /// 这类图元的 boundingBox() 无定义（返回无效盒），因为它们的范围是无穷。
    /// 因此依赖真实范围的算法必须先查这里：
    ///   · 范围缩放 / 出图 / $EXTMIN 要排除它们（否则视口会被撑到无穷）；
    ///   · 渲染与拾取要靠「裁到视口 / 选区」判断可见性，不能靠包围盒剔除。
    virtual bool isUnbounded() const noexcept { return false; }

    /// 无界图元的定义点（构造线、射线的基点）。有界图元返回空。
    /// 导入时的坐标体检、属性面板与夹点都以它为基准。
    virtual std::optional<Point2D> definitionPoint() const { return std::nullopt; }

    // ---- 几何接口（子类实现） -------------------------------------------------
    /// 深拷贝（不含 id，由 Document 重新分配）
    virtual std::unique_ptr<Entity> clone() const = 0;

    /// 轴对齐包围盒（世界坐标）。
    /// 无界图元（isUnbounded() 为真）返回**无效盒**——它们没有有限范围，
    /// Box2D::expand 对无效盒是空操作，范围类算法据此自然跳过它们。
    virtual Box2D boundingBox() const = 0;

    /// 点到图元的最短距离，用于拾取判定
    virtual double distanceTo(const Point2D& p) const = 0;

    /// 图元上距离给定点最近的位置，用于「最近点」对象捕捉。
    /// 默认实现基于 flatten 的离散点，圆与圆弧会给出精确解。
    virtual Point2D closestPointTo(const Point2D& p) const;

    /// 图元是否完全位于框内（窗选 Window）
    virtual bool isInside(const Box2D& box) const;

    /// 图元是否与框边界接触或与框相交（交叉选择 Crossing）
    virtual bool crosses(const Box2D& box) const;

    /// 就地施加仿射变换
    virtual void transform(const Transform2D& t) = 0;

    /// 长度（直线为长度，圆为周长，圆弧为弧长，多段线为各段之和）
    virtual double length() const = 0;

    /// 关键点：端点、中点、圆心、象限点 —— 供对象捕捉与夹点使用
    virtual std::vector<Point2D> snapPoints() const = 0;

    /// 夹点集合（拖拽编辑用）。默认与 snapPoints 相同。
    virtual std::vector<Point2D> gripPoints() const { return snapPoints(); }

    /// 拖拽第 index 个夹点到 newPos。返回是否支持该操作。
    virtual bool moveGrip(std::size_t index, const Point2D& newPos);

    /// 曲线离散为折线点序列。sagitta 为允许的最大矢高误差（世界单位）。
    virtual std::vector<Point2D> flatten(double sagitta) const = 0;

    /// 图元是否为闭合轮廓（圆、闭合多段线）
    virtual bool isClosed() const { return false; }

    /// 供属性面板与命令行显示的一行摘要
    virtual QString describe() const = 0;

    /// 图元的几何参数文本（多行），用于属性面板详情
    virtual QString geometrySummary() const = 0;

protected:
    /// 复制公共显示属性（供子类 clone 复用）
    void copyCommonPropertiesTo(Entity& target) const;

    EntityId m_id = kInvalidEntityId;
    EntityType m_type;
    QString m_layerName = QStringLiteral("0");
    int m_colorIndex = -1;                 ///< ACI，-1 = 随层
    int m_lineweight = lineweight::kByLayer;
    QString m_linetypeName;                ///< 空 = 随层
    bool m_visible = true;
};

// -----------------------------------------------------------------------------
//  辅助：基于离散点序列的默认 isInside / crosses 实现
// -----------------------------------------------------------------------------
namespace detail {

/// 折线点序列是否完全位于盒内
bool pointsInsideBox(const std::vector<Point2D>& pts, bool closed, const Box2D& box);

/// 折线点序列是否与盒相交
bool pointsCrossBox(const std::vector<Point2D>& pts, bool closed, const Box2D& box);

/// 由包围盒尺寸推导合适的离散矢高
double sagittaForBox(const Box2D& box);

} // namespace detail

} // namespace lycad
