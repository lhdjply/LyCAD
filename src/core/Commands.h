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
#include "core/Layer.h"

#include <QUndoCommand>

#include <memory>
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
