// =============================================================================
//  LyCAD - core/Block.h
//  块定义与块表（DXF: BLOCKS 段 / BLOCK_RECORD 表）。
//
//  为什么需要它
//    块参照（INSERT）不是「一堆图元的快捷方式」，而是一个**具名定义的引用**：
//    同一个定义可以被引用任意多次，改一处定义，所有引用同步变化。要做到这一点，
//    文档必须先把定义本身存下来，而不是在导入时按插入点/比例/旋转把图元炸开。
//
//  设计要点
//    · 定义持有图元的所有权（unique_ptr），图元里可以再出现 InsertEntity，
//      形成任意深度的嵌套；环引用由 InsertEntity::explode 的深度上限兜住。
//    · 块表按**名字大小写不敏感**查找（AutoCAD 的块名不区分大小写），但保留
//      原始拼写，写出时按原样回写。
//    · 定义用 shared_ptr 持有：InsertEntity 直接握着它引用的定义，因此
//      「块表里删掉一条定义」不会让既有引用变成悬空指针——引用照常显示，
//      只是不再能从块表里找到（AutoCAD 的 purge 语义与此一致）。
//    · core 不依赖 io：块定义只描述几何，DXF 的组码映射全部留在 io 层。
// =============================================================================
#pragma once

#include "core/Entity.h"
#include "core/Types.h"

#include <QString>

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

namespace lycad {

// -----------------------------------------------------------------------------
//  属性定义（DXF: ATTDEF）
//
//  块定义里的「文字占位符」：块被引用时，每个 ATTDEF 在块参照上生成一条
//  Attribute（DXF: ATTRIB）——标签相同、值取默认值（或由用户填写）。
//  ATTDEF 本身**不作为几何绘制**，所以它不能混在图元列表里。
// -----------------------------------------------------------------------------
struct AttributeDefinition {
    /// DXF 70 组码标志位
    enum Flag {
        Invisible = 1,   ///< bit1：属性不可见（不绘制）
        Constant  = 2,   ///< bit2：常量属性（不随块参照生成 ATTRIB）
        Verify    = 4,   ///< bit3：插入时需校验
        Preset    = 8    ///< bit4：插入时取默认值且不再询问
    };

    QString tag;             ///< 2 组码：标签（块参照上的键）
    QString prompt;          ///< 3 组码：插入时的提示文字
    QString defaultValue;    ///< 1 组码：默认值
    Point2D position;        ///< 10/20：**块坐标系**下的文字起点
    double  height = 1.0;    ///< 40：字高
    double  rotation = 0.0;  ///< 50：旋转（弧度）
    QString style;           ///< 7 组码：文字样式名
    int     flags = 0;       ///< 70

    bool isInvisible() const noexcept { return (flags & Invisible) != 0; }
    bool isConstant() const noexcept { return (flags & Constant) != 0; }
};

// -----------------------------------------------------------------------------
//  BlockDefinition
// -----------------------------------------------------------------------------
class BlockDefinition {
public:
    /// DXF 70 组码的块类型标志
    enum Flag {
        Anonymous   = 1,   ///< bit1：匿名块（*U/*D/*A…）
        HasAttdefs  = 2,   ///< bit2：含属性定义
        Xref        = 4,   ///< bit4：外部参照
        XrefOverlay = 8,
        ExternallyDependent = 16,
        Resolved    = 32,
        Referenced  = 64
    };

    BlockDefinition();
    explicit BlockDefinition(const QString& name);
    ~BlockDefinition();

    BlockDefinition(const BlockDefinition&) = delete;
    BlockDefinition& operator=(const BlockDefinition&) = delete;

    // ---- 标识 ----------------------------------------------------------------
    const QString& name() const noexcept { return m_name; }
    void setName(const QString& name) { m_name = name; }

    /// 块基点（DXF 10/20）：块坐标里这一点对应 INSERT 的插入点
    const Point2D& basePoint() const noexcept { return m_basePoint; }
    void setBasePoint(const Point2D& p) noexcept { m_basePoint = p; }

    int  flags() const noexcept { return m_flags; }
    void setFlags(int flags) noexcept { m_flags = flags; }
    bool isAnonymous() const noexcept { return (m_flags & Anonymous) != 0; }

    // ---- 几何 ----------------------------------------------------------------
    const std::vector<std::unique_ptr<Entity>>& entities() const noexcept { return m_entities; }
    std::vector<std::unique_ptr<Entity>>& entities() noexcept { return m_entities; }
    void addEntity(std::unique_ptr<Entity> entity);
    void clearEntities() noexcept { m_entities.clear(); }

    std::size_t entityCount() const noexcept { return m_entities.size(); }

    // ---- 属性定义 --------------------------------------------------------------
    const std::vector<AttributeDefinition>& attributeDefinitions() const noexcept
    {
        return m_attributeDefs;
    }
    std::vector<AttributeDefinition>& attributeDefinitions() noexcept { return m_attributeDefs; }
    void addAttributeDefinition(const AttributeDefinition& def) { m_attributeDefs.push_back(def); }

    /// 按标签查找属性定义（大小写不敏感）
    const AttributeDefinition* findAttributeDefinition(const QString& tag) const;

    // ---- 派生 ----------------------------------------------------------------
    /// 深拷贝（图元逐个 clone，属性定义按值复制）。名字一并复制。
    std::unique_ptr<BlockDefinition> clone() const;

    /// 块坐标系下的包围范围（含嵌套块展开）
    Box2D boundingBox() const;

    /// 递归展开后的图元总数（供界面显示「块含 N 个图元」）
    std::size_t expandedEntityCount(std::size_t maxDepth = 16) const;

private:
    QString m_name;
    Point2D m_basePoint;
    int     m_flags = 0;
    std::vector<std::unique_ptr<Entity>> m_entities;
    std::vector<AttributeDefinition> m_attributeDefs;
};

// -----------------------------------------------------------------------------
//  BlockTable
// -----------------------------------------------------------------------------
class BlockTable {
public:
    /// 名字归一化（大小写不敏感查找用）
    static QString normalizedKey(const QString& name);

    bool contains(const QString& name) const;
    const BlockDefinition* find(const QString& name) const;
    BlockDefinition* find(const QString& name);

    /// 加入一条定义；同名（大小写不敏感）时替换，保持原有位置
    void add(std::shared_ptr<BlockDefinition> block);

    bool remove(const QString& name);
    void clear();

    const std::vector<std::shared_ptr<BlockDefinition>>& all() const noexcept { return m_blocks; }
    std::size_t size() const noexcept { return m_blocks.size(); }
    bool isEmpty() const noexcept { return m_blocks.empty(); }

    /// 生成不与现有名字冲突的块名：base、base2、base3……
    QString uniqueName(const QString& base) const;

private:
    std::vector<std::shared_ptr<BlockDefinition>> m_blocks;
    std::unordered_map<QString, std::size_t> m_index;   ///< 归一化名 -> 下标
};

} // namespace lycad
