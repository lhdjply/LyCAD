// =============================================================================
//  LyCAD - core/Commands.cpp
// =============================================================================
#include "core/Commands.h"

#include "core/TextEntity.h"

#include <algorithm>

namespace lycad {

// -----------------------------------------------------------------------------
//  AddEntitiesCommand
// -----------------------------------------------------------------------------
AddEntitiesCommand::AddEntitiesCommand(Document* document,
                                       std::vector<std::unique_ptr<Entity>> entities,
                                       const QString& text)
    : QUndoCommand(text)
    , m_document(document)
    , m_pending(std::move(entities))
{
    // id 在构造期一次性分配，保证撤销/重做过程中 id 稳定不变
    m_ids.reserve(m_pending.size());
    for (auto& e : m_pending) {
        if (!e) {
            m_ids.push_back(kInvalidEntityId);
            continue;
        }
        e->setId(m_document->allocateId());
        m_ids.push_back(e->id());
    }
}

AddEntitiesCommand::~AddEntitiesCommand() = default;

void AddEntitiesCommand::redo()
{
    for (std::size_t i = 0; i < m_pending.size(); ++i) {
        if (!m_pending[i]) continue;                 // 理论上不会发生，防御性判断
        const EntityId id = m_document->internalAdd(std::move(m_pending[i]));
        emit m_document->entityAdded(id);
    }
    m_document->afterEntitiesMutated();
}

void AddEntitiesCommand::undo()
{
    for (std::size_t i = m_pending.size(); i-- > 0;) {
        const EntityId id = m_ids[i];
        if (id == kInvalidEntityId) continue;
        m_pending[i] = m_document->internalTake(id);
        emit m_document->entityRemoved(id);
    }
    m_document->afterEntitiesMutated();
}

// -----------------------------------------------------------------------------
//  RemoveEntitiesCommand
// -----------------------------------------------------------------------------
RemoveEntitiesCommand::RemoveEntitiesCommand(Document* document,
                                             std::vector<EntityId> ids,
                                             const QString& text)
    : QUndoCommand(text)
    , m_document(document)
    , m_ids(std::move(ids))
{
}

void RemoveEntitiesCommand::redo()
{
    m_removed.clear();
    m_removed.reserve(m_ids.size());

    for (EntityId id : m_ids) {
        std::size_t index = 0;
        auto entity = m_document->internalTake(id, &index);
        if (!entity) continue;                       // 图元已不存在，跳过

        m_removed.push_back(RemovedItem{ id, index, std::move(entity) });
        emit m_document->entityRemoved(id);
    }

    // 按原始顺序升序回插，才能精确还原绘制次序
    std::sort(m_removed.begin(), m_removed.end(),
              [](const RemovedItem& a, const RemovedItem& b) { return a.orderIndex < b.orderIndex; });

    m_document->afterEntitiesMutated();
}

void RemoveEntitiesCommand::undo()
{
    for (RemovedItem& item : m_removed) {
        if (!item.entity) continue;
        m_document->internalAdd(std::move(item.entity), item.orderIndex);
        emit m_document->entityAdded(item.id);
    }
    m_document->afterEntitiesMutated();
}

// -----------------------------------------------------------------------------
//  TransformEntitiesCommand
// -----------------------------------------------------------------------------
TransformEntitiesCommand::TransformEntitiesCommand(Document* document,
                                                   std::vector<EntityId> ids,
                                                   Transform2D transform,
                                                   const QString& text)
    : QUndoCommand(text)
    , m_document(document)
    , m_ids(std::move(ids))
    , m_transform(transform)
{
    m_before.reserve(m_ids.size());
    for (EntityId id : m_ids) {
        const Entity* e = m_document->entity(id);
        m_before.push_back(e ? e->clone() : nullptr);
    }
}

void TransformEntitiesCommand::redo()
{
    for (EntityId id : m_ids) {
        if (Entity* e = m_document->entity(id)) {
            e->transform(m_transform);
            emit m_document->entityChanged(id);
        }
    }
    m_document->afterEntitiesMutated();
}

void TransformEntitiesCommand::undo()
{
    for (std::size_t i = 0; i < m_ids.size(); ++i) {
        if (!m_before[i]) continue;
        m_document->internalSwap(m_ids[i], m_before[i]->clone());
        emit m_document->entityChanged(m_ids[i]);
    }
    m_document->afterEntitiesMutated();
}

// -----------------------------------------------------------------------------
//  EntityPropertyCommand
// -----------------------------------------------------------------------------
EntityPropertyCommand::EntityPropertyCommand(Document* document,
                                             std::vector<EntityId> ids,
                                             EntityProperty property,
                                             QString stringValue,
                                             int intValue,
                                             bool boolValue,
                                             const QString& text)
    : QUndoCommand(text)
    , m_document(document)
    , m_property(property)
    , m_newString(std::move(stringValue))
    , m_newInt(intValue)
    , m_newBool(boolValue)
{
    m_before.reserve(ids.size());
    for (EntityId id : ids) {
        Snapshot snap;
        snap.id = id;
        if (const Entity* e = m_document->entity(id)) {
            snap.found = true;
            snap.layerName = e->layerName();
            snap.intValue = e->colorIndex();
            snap.boolValue = e->isVisible();
        }
        m_before.push_back(std::move(snap));
    }
}

void EntityPropertyCommand::apply(const QString& stringValue, int intValue, bool boolValue)
{
    for (const Snapshot& snap : m_before) {
        Entity* e = m_document->entity(snap.id);
        if (!e) continue;

        switch (m_property) {
        case EntityProperty::LayerName:  e->setLayerName(stringValue); break;
        case EntityProperty::ColorIndex: e->setColorIndex(intValue); break;
        case EntityProperty::Lineweight: e->setLineweightDxf(intValue); break;
        case EntityProperty::Visible:    e->setVisible(boolValue); break;
        }
        emit m_document->entityChanged(snap.id);
    }
    m_document->afterEntitiesMutated();
}

void EntityPropertyCommand::redo()
{
    apply(m_newString, m_newInt, m_newBool);
}

void EntityPropertyCommand::undo()
{
    // 逐图元还原，因此不能走统一的 apply()：旧值各不相同
    for (const Snapshot& snap : m_before) {
        Entity* e = m_document->entity(snap.id);
        if (!e || !snap.found) continue;

        switch (m_property) {
        case EntityProperty::LayerName:  e->setLayerName(snap.layerName); break;
        case EntityProperty::ColorIndex: e->setColorIndex(snap.intValue); break;
        case EntityProperty::Lineweight: e->setLineweightDxf(snap.intValue); break;
        case EntityProperty::Visible:    e->setVisible(snap.boolValue); break;
        }
        emit m_document->entityChanged(snap.id);
    }
    m_document->afterEntitiesMutated();
}

// -----------------------------------------------------------------------------
//  TextPropertyCommand
// -----------------------------------------------------------------------------
namespace {

/// 取出图元的文字属性快照。单行文字与多行文字都算「文字」：两者共享字高/旋转/字体，
/// 各自独有的属性（对齐档 / 附着点与栏宽）一并取出——快照多取不影响正确性，
/// assign 只写 m_new 里赋了值的那些字段。
bool textValuesOf(const Entity* entity, TextPropertyValues& values)
{
    if (const auto* text = dynamic_cast<const TextEntity*>(entity)) {
        values.height = text->height();
        values.rotation = text->rotation();
        values.hAlign = text->hAlign();
        values.vAlign = text->vAlign();
        values.fontStyle = text->fontStyle();
        return true;
    }
    if (const auto* text = dynamic_cast<const MTextEntity*>(entity)) {
        values.height = text->height();
        values.rotation = text->rotation();
        values.fontStyle = text->fontStyle();
        values.mtextAttachment = text->attachment();
        values.mtextWidth = text->referenceWidth();
        return true;
    }
    return false;
}

} // namespace

void TextPropertyCommand::assign(TextEntity& text, const TextPropertyValues& values)
{
    // 只写已赋值的字段：未赋值 = 本次不动它，撤销时同样成立。
    // hAlign/vAlign 是 TEXT 独有的对齐档，MTEXT 用附着点表达，故此处互不影响。
    if (values.height) text.setHeight(*values.height);
    if (values.rotation) text.setRotation(*values.rotation);
    if (values.hAlign) text.setHAlign(*values.hAlign);
    if (values.vAlign) text.setVAlign(*values.vAlign);
    if (values.fontStyle) text.setFontStyle(*values.fontStyle);
}

void TextPropertyCommand::assign(MTextEntity& text, const TextPropertyValues& values)
{
    if (values.height) text.setHeight(*values.height);
    if (values.rotation) text.setDirection(Vector2D(std::cos(*values.rotation),
                                                   std::sin(*values.rotation)));
    if (values.fontStyle) text.setFontStyle(*values.fontStyle);
    if (values.mtextAttachment) text.setAttachment(*values.mtextAttachment);
    if (values.mtextWidth) text.setReferenceWidth(*values.mtextWidth);
}

TextPropertyCommand::TextPropertyCommand(Document* document,
                                         std::vector<EntityId> ids,
                                         TextPropertyValues values,
                                         const QString& text)
    : QUndoCommand(text)
    , m_document(document)
    , m_new(std::move(values))
{
    m_ids.reserve(ids.size());
    m_before.reserve(ids.size());
    for (EntityId id : ids) {
        TextPropertyValues snapshot;
        if (!textValuesOf(m_document->entity(id), snapshot)) continue;
        m_ids.push_back(id);
        m_before.push_back(snapshot);
    }
}

void TextPropertyCommand::redo()
{
    for (std::size_t i = 0; i < m_ids.size(); ++i) {
        Entity* entity = m_document->entity(m_ids[i]);
        if (!entity) continue;
        if (auto* text = dynamic_cast<TextEntity*>(entity)) {
            assign(*text, m_new);
        } else if (auto* mtext = dynamic_cast<MTextEntity*>(entity)) {
            assign(*mtext, m_new);
        } else {
            continue;
        }
        emit m_document->entityChanged(m_ids[i]);
    }
    m_document->afterEntitiesMutated();
}

void TextPropertyCommand::undo()
{
    // 逐图元还原，因此不能走统一的赋值：旧值各不相同
    for (std::size_t i = 0; i < m_ids.size(); ++i) {
        Entity* entity = m_document->entity(m_ids[i]);
        if (!entity) continue;
        if (auto* text = dynamic_cast<TextEntity*>(entity)) {
            assign(*text, m_before[i]);
        } else if (auto* mtext = dynamic_cast<MTextEntity*>(entity)) {
            assign(*mtext, m_before[i]);
        } else {
            continue;
        }
        emit m_document->entityChanged(m_ids[i]);
    }
    m_document->afterEntitiesMutated();
}

// -----------------------------------------------------------------------------
//  InsertPropertyCommand
// -----------------------------------------------------------------------------
namespace {

/// 取出块参照的比例/旋转快照（三个字段全取，assign 只写赋了值的那些）
bool insertValuesOf(const Entity* entity, InsertPropertyValues& values)
{
    if (!entity || entity->type() != EntityType::Insert) return false;
    const auto& insert = static_cast<const InsertEntity&>(*entity);

    values.scaleX = insert.scaleX();
    values.scaleY = insert.scaleY();
    values.rotation = insert.rotation();
    return true;
}

} // namespace

void InsertPropertyCommand::assign(InsertEntity& insert, const InsertPropertyValues& values)
{
    // 只写已赋值的字段：未赋值 = 本次不动它，撤销时同样成立
    if (values.scaleX || values.scaleY) {
        insert.setScale(values.scaleX.value_or(insert.scaleX()),
                        values.scaleY.value_or(insert.scaleY()),
                        insert.scaleZ());
    }
    if (values.rotation) insert.setRotation(*values.rotation);
}

InsertPropertyCommand::InsertPropertyCommand(Document* document,
                                             std::vector<EntityId> ids,
                                             InsertPropertyValues values,
                                             const QString& text)
    : QUndoCommand(text)
    , m_document(document)
    , m_new(std::move(values))
{
    m_ids.reserve(ids.size());
    m_before.reserve(ids.size());
    for (EntityId id : ids) {
        InsertPropertyValues snapshot;
        if (!insertValuesOf(m_document->entity(id), snapshot)) continue;
        m_ids.push_back(id);
        m_before.push_back(snapshot);
    }
}

void InsertPropertyCommand::redo()
{
    for (std::size_t i = 0; i < m_ids.size(); ++i) {
        Entity* entity = m_document->entity(m_ids[i]);
        if (!entity || entity->type() != EntityType::Insert) continue;
        assign(static_cast<InsertEntity&>(*entity), m_new);
        emit m_document->entityChanged(m_ids[i]);
    }
    m_document->afterEntitiesMutated();
}

void InsertPropertyCommand::undo()
{
    // 逐图元还原，因此不能走统一的赋值：旧值各不相同
    for (std::size_t i = 0; i < m_ids.size(); ++i) {
        Entity* entity = m_document->entity(m_ids[i]);
        if (!entity || entity->type() != EntityType::Insert) continue;
        assign(static_cast<InsertEntity&>(*entity), m_before[i]);
        emit m_document->entityChanged(m_ids[i]);
    }
    m_document->afterEntitiesMutated();
}

// -----------------------------------------------------------------------------
//  ReplaceEntityCommand
// -----------------------------------------------------------------------------
ReplaceEntityCommand::ReplaceEntityCommand(Document* document,
                                           EntityId id,
                                           std::unique_ptr<Entity> replacement,
                                           const QString& text)
    : QUndoCommand(text)
    , m_document(document)
    , m_id(id)
    , m_pending(std::move(replacement))
{
}

void ReplaceEntityCommand::redo()
{
    // 交换是对称操作：redo 与 undo 逻辑完全一致，只是语义方向相反
    m_pending = m_document->internalSwap(m_id, std::move(m_pending));
    emit m_document->entityChanged(m_id);
    m_document->afterEntitiesMutated();
}

void ReplaceEntityCommand::undo()
{
    redo();
}

// -----------------------------------------------------------------------------
//  LayerTableCommand
// -----------------------------------------------------------------------------
LayerTableCommand::LayerTableCommand(Document* document,
                                     std::vector<Layer> before,
                                     std::vector<Layer> after,
                                     QString retargetFrom,
                                     QString retargetTo,
                                     const QString& text)
    : QUndoCommand(text)
    , m_document(document)
    , m_before(std::move(before))
    , m_after(std::move(after))
    , m_retargetFrom(std::move(retargetFrom))
    , m_retargetTo(std::move(retargetTo))
{
}

void LayerTableCommand::apply(const std::vector<Layer>& layers,
                              const QString& from,
                              const QString& to)
{
    m_document->internalSetLayers(layers);
    if (!from.isEmpty() && from != to) {
        m_document->internalRetargetEntities(from, to);
    }
    emit m_document->layersChanged();
    m_document->afterEntitiesMutated();
}

void LayerTableCommand::redo()
{
    apply(m_after, m_retargetFrom, m_retargetTo);
}

void LayerTableCommand::undo()
{
    apply(m_before, m_retargetTo, m_retargetFrom);
}

} // namespace lycad
