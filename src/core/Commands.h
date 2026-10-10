// =============================================================================
//  LyCAD - core/Commands.h
//  撤销/重做命令。
//
//  所有对文档的修改都封装为 QUndoCommand，命令对象负责在自身内部保留
//  「修改前」状态，从而保证撤销的精确性与幂等性。
//  文档的 internalXxx 系列方法只做纯粹的存取，不触碰撤销栈——命令是唯一
//  的写入入口，避免出现「修改被记录两次」的经典缺陷。
// =============================================================================
#pragma once

#include "core/Document.h"
#include "core/Entity.h"
#include "core/InsertEntity.h"
#include "core/Layer.h"
#include "core/MTextEntity.h"
#include "core/TextEntity.h"

#include <QUndoCommand>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace lycad {

// -----------------------------------------------------------------------------
//  新增图元
// -----------------------------------------------------------------------------
class AddEntitiesCommand : public QUndoCommand {
public:
    AddEntitiesCommand(Document* document,
                       std::vector<std::unique_ptr<Entity>> entities,
                       const QString& text);
    ~AddEntitiesCommand() override;

    void undo() override;
    void redo() override;

    /// 命令执行后各图元的 id，顺序与传入顺序一致
    const std::vector<EntityId>& createdIds() const noexcept { return m_ids; }

private:
    Document* m_document;
    std::vector<std::unique_ptr<Entity>> m_pending;
    std::vector<EntityId> m_ids;
};

// -----------------------------------------------------------------------------
//  删除图元
// -----------------------------------------------------------------------------
class RemoveEntitiesCommand : public QUndoCommand {
public:
    RemoveEntitiesCommand(Document* document, std::vector<EntityId> ids, const QString& text);

    void undo() override;
    void redo() override;

private:
    struct RemovedItem {
        EntityId id;
        std::size_t orderIndex;
        std::unique_ptr<Entity> entity;
    };

    Document* m_document;
    std::vector<EntityId> m_ids;
    std::vector<RemovedItem> m_removed;
};

// -----------------------------------------------------------------------------
//  变换图元（移动 / 旋转 / 缩放 / 镜像的统一实现）
//
//  采用「前置快照 + 重放」策略而非「施加逆变换」：
//    · 逆变换在非等比缩放下不可逆（圆会被近似为圆而非还原为原圆）；
//    · 前置快照对任何变换都是精确可逆的，代价是一次深拷贝。
// -----------------------------------------------------------------------------
class TransformEntitiesCommand : public QUndoCommand {
public:
    TransformEntitiesCommand(Document* document,
                             std::vector<EntityId> ids,
                             Transform2D transform,
                             const QString& text);

    void undo() override;
    void redo() override;

private:
    Document* m_document;
    std::vector<EntityId> m_ids;
    Transform2D m_transform;
    std::vector<std::unique_ptr<Entity>> m_before;
};

// -----------------------------------------------------------------------------
//  修改图元属性
// -----------------------------------------------------------------------------
enum class EntityProperty {
    LayerName,
    ColorIndex,
    Lineweight,
    Visible
};

class EntityPropertyCommand : public QUndoCommand {
public:
    EntityPropertyCommand(Document* document,
                          std::vector<EntityId> ids,
                          EntityProperty property,
                          QString stringValue,
                          int intValue,
                          bool boolValue,
                          const QString& text);

    void undo() override;
    void redo() override;

private:
    struct Snapshot {
        EntityId id;
        QString  layerName;
        int      intValue = 0;
        bool     boolValue = true;
        bool     found = false;
    };

    void apply(const QString& stringValue, int intValue, bool boolValue);

    Document* m_document;
    std::vector<Snapshot> m_before;
    EntityProperty m_property;
    QString m_newString;
    int  m_newInt = 0;
    bool m_newBool = true;
};

// -----------------------------------------------------------------------------
//  修改文字属性
//
//  字高 / 旋转 / 水平对齐 / 垂直对齐是文字独有的属性，取值是浮点与枚举——
//  塞进 EntityPropertyCommand 的 string/int/bool 三个槽只会把类型越塞越歪，
//  因此单独一条命令。与通用属性一致：构造时逐图元留快照，撤销按快照逐图元还原。
//
//  旋转存**弧度**（与 TextEntity 同一口径），界面层负责度与弧度的换算。
// -----------------------------------------------------------------------------
/// 要写入的文字属性。**未赋值的字段表示本次不动它**。
///
/// 这样一条命令既能写单个字段，也能一次写多个字段；更重要的一层原因是属性面板
/// 会拿它表达「只改这一项」：多选时若某一列取值不一致，那一列无从给出统一旧值，
/// 但用户仍然应当能改另一列——把四个字段都做成必填就做不到这一点。
struct TextPropertyValues {
    std::optional<double> height;
    std::optional<double> rotation;                             ///< 弧度
    std::optional<TextEntity::HAlign> hAlign;
    std::optional<TextEntity::VAlign> vAlign;
    /// 字体（DXF 7 样式名，同时也是渲染所用的字体族名）。空串表示「不指定、
    /// 用默认字体」，因此这里用 optional 区分「本次不动」与「清空字体」。
    std::optional<QString> fontStyle;
    /// 多行文字（MTEXT）独有：附着点（DXF 71）与参考矩形宽度（DXF 41，编辑时的折行宽度；0 = 无限制）。
    /// 单行文字没有这两个属性，写入时按图元类型各自忽略不适用的字段。
    std::optional<MTextEntity::Attachment> mtextAttachment;
    std::optional<double> mtextWidth;
};

class TextPropertyCommand : public QUndoCommand {
public:
    /// ids 里不是文字的图元会被剔除；一个文字都没有时命令不做任何事
    /// （Document 的 setEntitiesTextXxx 会先筛一遍，因此不会压入空命令）。
    TextPropertyCommand(Document* document,
                        std::vector<EntityId> ids,
                        TextPropertyValues values,
                        const QString& text);

    void undo() override;
    void redo() override;

private:
    /// 按「已赋值才写」的规则把 values 落到图元上（redo 与 undo 共用）。
    /// 单行文字与多行文字各有一个重载：两者共用的字段（字高/旋转/字体）写法一致，
    /// 对齐/附着点这类各自独有的字段互不影响。
    static void assign(TextEntity& text, const TextPropertyValues& values);
    static void assign(MTextEntity& text, const TextPropertyValues& values);

    Document* m_document;
    std::vector<EntityId> m_ids;                ///< 命中的文字图元
    std::vector<TextPropertyValues> m_before;   ///< 与 m_ids 一一对应的前置快照
    TextPropertyValues m_new;
};

// -----------------------------------------------------------------------------
//  修改块参照属性（比例 / 旋转）
//
//  比例与旋转是块参照独有的几何参数，取值是浮点——与文字属性同理，单独一条命令。
//  字段同样采用「未赋值即不动」的约定，多选时某一列取值不一致不影响改其它列。
// -----------------------------------------------------------------------------
struct InsertPropertyValues {
    std::optional<double> scaleX;
    std::optional<double> scaleY;
    std::optional<double> rotation;     ///< 弧度（与 InsertEntity 同一口径）
};

class InsertPropertyCommand : public QUndoCommand {
public:
    /// ids 里不是块参照的图元会被剔除；一个块参照都没有时命令不做任何事
    InsertPropertyCommand(Document* document,
                          std::vector<EntityId> ids,
                          InsertPropertyValues values,
                          const QString& text);

    void undo() override;
    void redo() override;

private:
    /// 按「已赋值才写」的规则把 values 落到块参照上（redo 与 undo 共用）
    static void assign(InsertEntity& insert, const InsertPropertyValues& values);

    Document* m_document;
    std::vector<EntityId> m_ids;
    std::vector<InsertPropertyValues> m_before;   ///< 与 m_ids 一一对应的前置快照
    InsertPropertyValues m_new;
};

// -----------------------------------------------------------------------------
//  整体替换图元几何（夹点编辑）
// -----------------------------------------------------------------------------
class ReplaceEntityCommand : public QUndoCommand {
public:
    ReplaceEntityCommand(Document* document,
                         EntityId id,
                         std::unique_ptr<Entity> replacement,
                         const QString& text);

    void undo() override;
    void redo() override;

private:
    Document* m_document;
    EntityId m_id;
    std::unique_ptr<Entity> m_pending;   ///< 在「待装入」与「已卸下」之间往复
};

// -----------------------------------------------------------------------------
//  图层表整体变更（新建 / 删除 / 重命名 / 属性修改的统一实现）
//
//  图层属性修改可能牵连图元的归属名称（如重命名、删除后迁移），
//  因此命令同时携带一个可选的「图元归属重定向」动作，保证二者在同一步撤销。
// -----------------------------------------------------------------------------
class LayerTableCommand : public QUndoCommand {
public:
    LayerTableCommand(Document* document,
                      std::vector<Layer> before,
                      std::vector<Layer> after,
                      QString retargetFrom,
                      QString retargetTo,
                      const QString& text);

    void undo() override;
    void redo() override;

private:
    void apply(const std::vector<Layer>& layers, const QString& from, const QString& to);

    Document* m_document;
    std::vector<Layer> m_before;
    std::vector<Layer> m_after;
    QString m_retargetFrom;
    QString m_retargetTo;
};

} // namespace lycad
