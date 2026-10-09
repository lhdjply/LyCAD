// =============================================================================
//  LyCAD - core/InsertEntity.h
//  块参照图元（DXF: INSERT / MINSERT）。
//
//  模型
//    块参照 = 「对某个块定义的引用」+ 一组插入参数。它**不复制**块内图元，
//    而是持有定义的 shared_ptr，几何在查询时由变换实时推导：
//
//        p_父 = T(插入点) · R(旋转) · S(比例) · T(-块基点) · p_块
//
//  这与标注（DimensionEntity）的处理思路一致：只有定义数据落盘，几何一处推导，
//  渲染 / 拾取 / 包围盒 / 捕捉共用同一份结果，从结构上杜绝「存下来的与画出来的
//  不一致」。
//
//  为什么不是「导入时炸开」
//    · 炸开后文件往返必然丢失块结构：同一个定义被引用 20 次会变成 20 份图元，
//      别的 CAD 再次打开时看到的不再是块参照；
//    · 无法再实现 分解(EXPLODE) / 块编辑 / 属性提取；
//    · 大图里同一块反复出现时，文档体积成倍膨胀。
//
//  循环引用的边界
//    块参照以 shared_ptr 持有定义，而定义又拥有块内图元——A 里插 B、B 里插 A
//    会让两者互相持有，引用计数永远降不到 0。导入端会主动断开这种环
//    （见 io/LibDxfRwAdapter.cpp 的 m_resolveChain），API 调用方自己构造环时
//    需要自行负责。展开本身不会因此死循环：InsertEntity::collectInto 有深度上限。
//
//  变换的可分解性
//    块参照的线性部分恒为 R(θ)·S(sx,sy) 的形式（两列正交）。对它左乘任意
//    **相似变换**（平移/旋转/等比缩放/镜像）后两列仍然正交，因此总能重新分解
//    回 (插入点, 比例, 旋转)。只有非等比缩放（如 X/Y 不同比例的拉伸）才会产生
//    错切——此时把结果「烘焙」进该块参照私有的一份块定义副本，几何精确不丢。
// =============================================================================
#pragma once

#include "core/Block.h"
#include "core/Entity.h"
#include "core/Types.h"

#include <QString>

#include <cstddef>
#include <memory>
#include <vector>

namespace lycad {

// -----------------------------------------------------------------------------
//  属性值（DXF: ATTRIB）
//
//  与 AttributeDefinition 的区别：ATTDEF 在块定义里、坐标是块坐标；ATTRIB 挂在
//  块参照上、坐标是**世界坐标**（DXF 规范如此），因此移动块参照时属性要跟着变换。
// -----------------------------------------------------------------------------
struct Attribute {
    QString tag;             ///< 2 组码：标签（与 ATTDEF 对应）
    QString value;           ///< 1 组码：值
    Point2D position;        ///< 10/20：世界坐标下的文字起点
    double  height = 1.0;    ///< 40：字高
    double  rotation = 0.0;  ///< 50：旋转（弧度）
    QString style;           ///< 7 组码：文字样式
    QString layer;           ///< 8 组码：所在图层（空 = 随块参照）
    int     flags = 0;       ///< 70

    bool isInvisible() const noexcept { return (flags & 1) != 0; }
    bool isConstant() const noexcept { return (flags & 2) != 0; }
};

// -----------------------------------------------------------------------------
//  InsertEntity
// -----------------------------------------------------------------------------
class InsertEntity final : public Entity {
public:
    /// 嵌套块展开的深度上限：既防自引用块造成无限递归，也防指数级展开
    static constexpr std::size_t kMaxNestingDepth = 16;

    InsertEntity();
    explicit InsertEntity(std::shared_ptr<BlockDefinition> block);

    static constexpr EntityType kType = EntityType::Insert;

    // ---- 块定义 --------------------------------------------------------------
    const std::shared_ptr<BlockDefinition>& block() const noexcept { return m_block; }
    void setBlock(std::shared_ptr<BlockDefinition> block);
    bool hasBlock() const noexcept { return m_block != nullptr; }

    /// 引用的块名。定义缺失时返回原文件里写的名字，便于提示与写回。
    QString blockName() const;
    /// 设置引用名（无定义时使用；有定义时以定义的名字为准）
    void setBlockName(const QString& name);

    // ---- 插入参数 ------------------------------------------------------------
    const Point2D& insertionPoint() const noexcept { return m_point; }
    void setInsertionPoint(const Point2D& p);

    double scaleX() const noexcept { return m_scaleX; }
    double scaleY() const noexcept { return m_scaleY; }
    double scaleZ() const noexcept { return m_scaleZ; }
    void setScale(double sx, double sy, double sz = 1.0);

    double rotation() const noexcept { return m_rotation; }
    void setRotation(double radians);

    // ---- 阵列（MINSERT：行列重复） --------------------------------------------
    int columnCount() const noexcept { return m_columns; }
    int rowCount() const noexcept { return m_rows; }
    double columnSpacing() const noexcept { return m_columnSpacing; }
    double rowSpacing() const noexcept { return m_rowSpacing; }
    void setColumnCount(int count);
    void setRowCount(int count);
    void setColumnSpacing(double spacing);
    void setRowSpacing(double spacing);
    bool isArray() const noexcept { return m_columns > 1 || m_rows > 1; }
    int instanceCount() const noexcept { return m_columns * m_rows; }

    // ---- 变换 ----------------------------------------------------------------
    /// 块坐标 -> 父坐标（父坐标对模型空间的块参照即世界坐标）。
    /// 即 T(插入点)·R(旋转)·S(比例)·T(-块基点)。
    Transform2D blockToParentTransform() const;

    /// 第 (row, col) 个阵列实例的变换
    Transform2D instanceTransform(int row, int col) const;

    // ---- 展开 ----------------------------------------------------------------
    /// 展开为父坐标系下的独立图元（递归解析嵌套块，处理图层/随块属性继承）。
    /// 结果**不含** InsertEntity——嵌套已在本函数内解析完毕，渲染/拾取无需再递归。
    std::vector<std::unique_ptr<Entity>> explode(std::size_t maxDepth = kMaxNestingDepth) const;

    /// 展开结果的**只读**视图（带缓存，不复制）。渲染/拾取这类每帧都要走的高频
    /// 路径用它；需要改动结果时用 explode()。返回的图元生命周期与本对象一致。
    const std::vector<std::unique_ptr<Entity>>& expanded() const;

    /// 展开后的图元个数（供界面显示）
    std::size_t expandedCount(std::size_t maxDepth = kMaxNestingDepth) const;

    // ---- 属性 ----------------------------------------------------------------
    const std::vector<Attribute>& attributes() const noexcept { return m_attributes; }
    std::vector<Attribute>& attributes() noexcept { return m_attributes; }
    void setAttributes(std::vector<Attribute> attributes);
    void addAttribute(const Attribute& attribute);

    const Attribute* findAttribute(const QString& tag) const;
    Attribute* findAttribute(const QString& tag);
    bool setAttributeValue(const QString& tag, const QString& value);
    /// 写入一条属性：同标签的已有条目被**覆盖**（文件里的实际值优先于
    /// 按属性定义补出来的默认值），没有则追加。
    void setAttribute(const Attribute& attribute);

    /// 按块定义的属性定义补齐缺失属性（常量属性不生成），位置/字高/旋转由
    /// 插入参数实时推导。读入文件与新建块参照后都应调用一次。
    void syncAttributesFromDefinitions();

    // ---- Entity 接口 ---------------------------------------------------------
    std::unique_ptr<Entity> clone() const override;
    Box2D boundingBox() const override;
    double distanceTo(const Point2D& p) const override;
    Point2D closestPointTo(const Point2D& p) const override;
    bool isInside(const Box2D& box) const override;
    bool crosses(const Box2D& box) const override;
    void transform(const Transform2D& t) override;
    double length() const override;
    std::vector<Point2D> snapPoints() const override;
    std::vector<Point2D> gripPoints() const override;
    bool moveGrip(std::size_t index, const Point2D& newPos) override;
    std::vector<Point2D> flatten(double sagitta) const override;
    QString describe() const override;
    QString geometrySummary() const override;

    /// 展开结果是否来自缓存（供测试与诊断用）
    bool hasCachedGeometry() const noexcept { return m_cacheValid; }

private:
    /// 把本块参照的内容按 toParent 收集进 out（嵌套块在同一函数内递归解析）。
    /// includeAttributes 为假时只收集块内几何——烘焙私有块时用得到：
    /// 属性另有一份存储，不能连文字一起复制进块定义，否则会画两遍。
    void collectInto(std::vector<std::unique_ptr<Entity>>& out,
                     const Transform2D& toParent,
                     const QString& inheritedLayer,
                     std::size_t depth,
                     std::size_t maxDepth,
                     bool includeAttributes) const;

    /// 清空几何缓存。任何影响几何的字段变更都必须调用。
    void invalidateCache() const;
    /// 把「插入参数的变化量」施加到属性上。
    ///
    /// ATTRIB 的坐标按 DXF 规范是**世界坐标**，因此块参照一动，属性必须跟着动：
    /// 否则改完比例/旋转，属性文字会留在原地。位置按变换精确换算，字高与字向
    /// 按变换的线性部分换算（文字没有错切可言，取方向长度即可）。
    void applyAttributeDelta(const Transform2D& delta);
    /// 取得（必要时重建）展开结果
    const std::vector<std::unique_ptr<Entity>>& cached() const;
    /// 非相似变换落到块参照上时，把结果烘焙进私有的一份块定义副本
    void bakeIntoPrivateBlock(const Transform2D& transform);

    std::shared_ptr<BlockDefinition> m_block;
    QString m_blockName;             ///< m_block 为空时保留的原引用名
    Point2D m_point;
    double  m_scaleX = 1.0;
    double  m_scaleY = 1.0;
    double  m_scaleZ = 1.0;
    double  m_rotation = 0.0;
    int     m_columns = 1;
    int     m_rows = 1;
    double  m_columnSpacing = 0.0;
    double  m_rowSpacing = 0.0;
    std::vector<Attribute> m_attributes;

    // 几何缓存：包围盒/拾取/渲染/捕捉在同一帧里会被反复查询，展开一次即可。
    // 展开是深拷贝，不缓存会让渲染每帧都复制整块内容。
    mutable std::vector<std::unique_ptr<Entity>> m_cache;
    mutable bool m_cacheValid = false;
};

} // namespace lycad
