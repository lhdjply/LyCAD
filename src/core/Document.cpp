// =============================================================================
//  LyCAD - core/Document.cpp
// =============================================================================
#include "core/Document.h"

#include "core/Commands.h"
#include "core/InsertEntity.h"

#include <QFileInfo>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace lycad {

Document::Document(QObject* parent)
    : QObject(parent)
    , m_selection(new Selection(this))
    , m_undoStack(new QUndoStack(this))
{
    connect(m_selection, &Selection::changed, this, &Document::selectionChanged);

    // QUndoStack 只在「干净状态」翻转时通知，正好对应文档的修改标记
    connect(m_undoStack, &QUndoStack::cleanChanged, this, [this](bool clean) {
        emit modificationChanged(!clean);
    });
}

Document::~Document() = default;

// -----------------------------------------------------------------------------
//  图元查询
// -----------------------------------------------------------------------------
Entity* Document::entity(EntityId id) const
{
    const auto it = m_entities.find(id);
    return it == m_entities.end() ? nullptr : it->second.get();
}

std::vector<Entity*> Document::allEntities() const
{
    std::vector<Entity*> out;
    out.reserve(m_order.size());
    for (EntityId id : m_order) {
        if (Entity* e = entity(id)) out.push_back(e);
    }
    return out;
}

bool Document::isSelectable(EntityId id) const
{
    const Entity* e = entity(id);
    if (!e || !e->isVisible()) return false;
    return m_layers.isSelectable(e->layerName());
}

std::vector<Entity*> Document::selectedEntities() const
{
    std::vector<Entity*> out;
    out.reserve(m_selection->size());
    for (EntityId id : m_selection->ids()) {
        if (Entity* e = entity(id)) out.push_back(e);
    }
    return out;
}

// -----------------------------------------------------------------------------
//  撤销栈与内部存取
// -----------------------------------------------------------------------------
EntityId Document::allocateId() noexcept
{
    return m_nextId++;
}

EntityId Document::internalAdd(std::unique_ptr<Entity> entity, std::optional<std::size_t> orderIndex)
{
    if (!entity) return kInvalidEntityId;

    EntityId id = entity->id();
    if (id == kInvalidEntityId || m_entities.find(id) != m_entities.end()) {
        // id 缺失或与既有图元冲突，重新分配
        id = allocateId();
        entity->setId(id);
    }
    if (id >= m_nextId) m_nextId = id + 1;

    m_entities.emplace(id, std::move(entity));

    if (orderIndex && *orderIndex <= m_order.size()) {
        m_order.insert(m_order.begin() + static_cast<std::ptrdiff_t>(*orderIndex), id);
    } else {
        m_order.push_back(id);
    }
    return id;
}

std::unique_ptr<Entity> Document::internalTake(EntityId id, std::size_t* outOrderIndex)
{
    const auto it = m_entities.find(id);
    if (it == m_entities.end()) return nullptr;

    std::unique_ptr<Entity> entity = std::move(it->second);
    m_entities.erase(it);

    const auto orderIt = std::find(m_order.begin(), m_order.end(), id);
    if (orderIt != m_order.end()) {
        if (outOrderIndex) *outOrderIndex = static_cast<std::size_t>(orderIt - m_order.begin());
        m_order.erase(orderIt);
    } else if (outOrderIndex) {
        *outOrderIndex = m_order.size();
    }

    m_selection->remove(id);
    return entity;
}

std::unique_ptr<Entity> Document::internalSwap(EntityId id, std::unique_ptr<Entity> replacement)
{
    if (!replacement) return nullptr;

    const auto it = m_entities.find(id);
    if (it == m_entities.end()) {
        // 目标图元不存在：把替换物原样退回，交由命令持有
        return replacement;
    }

    replacement->setId(id);
    std::unique_ptr<Entity> previous = std::move(it->second);
    it->second = std::move(replacement);
    return previous;
}

void Document::internalSetLayers(std::vector<Layer> layers)
{
    m_layers.setAll(std::move(layers));
}

void Document::internalRetargetEntities(const QString& fromLayer, const QString& toLayer)
{
    if (fromLayer.isEmpty() || fromLayer == toLayer) return;

    for (const auto& pair : m_entities) {
        Entity* e = pair.second.get();
        if (e && e->layerName() == fromLayer) {
            e->setLayerName(toLayer);
            emit entityChanged(pair.first);
        }
    }
}

void Document::afterEntitiesMutated()
{
    emit extentsChanged();
}

// -----------------------------------------------------------------------------
//  图元编辑
// -----------------------------------------------------------------------------
EntityId Document::addEntity(std::unique_ptr<Entity> entity)
{
    if (!entity) return kInvalidEntityId;

    if (isBatchLoading()) {
        return internalAdd(std::move(entity));
    }

    const QString label = QStringLiteral("添加%1").arg(entity->typeDisplayName());
    auto entities = std::vector<std::unique_ptr<Entity>>{};
    entities.push_back(std::move(entity));

    auto* cmd = new AddEntitiesCommand(this, std::move(entities), label);
    m_undoStack->push(cmd);

    const auto& ids = cmd->createdIds();
    return ids.empty() ? kInvalidEntityId : ids.front();
}

std::vector<EntityId> Document::addEntities(std::vector<std::unique_ptr<Entity>> entities)
{
    if (entities.empty()) return {};

    if (isBatchLoading()) {
        std::vector<EntityId> ids;
        ids.reserve(entities.size());
        for (auto& e : entities) ids.push_back(internalAdd(std::move(e)));
        return ids;
    }

    const QString label = QStringLiteral("添加 %1 个图元").arg(entities.size());
    auto* cmd = new AddEntitiesCommand(this, std::move(entities), label);
    m_undoStack->push(cmd);
    return cmd->createdIds();
}

bool Document::removeEntity(EntityId id)
{
    return removeEntities({ id });
}

bool Document::removeEntities(const std::vector<EntityId>& ids)
{
    std::vector<EntityId> existing;
    existing.reserve(ids.size());
    for (EntityId id : ids) {
        if (m_entities.find(id) != m_entities.end()) existing.push_back(id);
    }
    if (existing.empty()) return false;

    if (isBatchLoading()) {
        for (EntityId id : existing) {
            internalTake(id);
            emit entityRemoved(id);
        }
        afterEntitiesMutated();
        return true;
    }

    const QString label = existing.size() == 1
        ? QStringLiteral("删除图元")
        : QStringLiteral("删除 %1 个图元").arg(existing.size());
    m_undoStack->push(new RemoveEntitiesCommand(this, std::move(existing), label));
    return true;
}

bool Document::explodeEntities(const std::vector<EntityId>& ids, int* explodedCount)
{
    if (explodedCount) *explodedCount = 0;
    if (ids.empty()) return false;

    // 先取几何、再删除：展开结果的包围盒与图层继承都依赖源图元仍然存在。
    std::vector<EntityId> targets;
    std::vector<std::unique_ptr<Entity>> children;

    for (EntityId id : ids) {
        const Entity* entity = this->entity(id);
        if (!entity || entity->type() != EntityType::Insert) continue;

        const auto& insert = static_cast<const InsertEntity&>(*entity);
        // 块定义缺失（文件里引用了未定义的块）时分解只会把图元删掉，
        // 那不是用户想要的——保持原样，由导入警告负责说明。
        if (!insert.hasBlock()) continue;

        std::vector<std::unique_ptr<Entity>> pieces = insert.explode();
        for (std::unique_ptr<Entity>& piece : pieces) {
            if (piece) children.push_back(std::move(piece));
        }
        targets.push_back(id);
    }

    if (targets.empty()) return false;
    if (explodedCount) *explodedCount = static_cast<int>(targets.size());

    if (isBatchLoading()) {
        for (const std::unique_ptr<Entity>& child : children) internalAdd(child->clone());
        for (EntityId id : targets) {
            internalTake(id);
            emit entityRemoved(id);
        }
        afterEntitiesMutated();
        return true;
    }

    // 删除 + 添加合成一条撤销记录：分开压栈的话，用户要按两次撤销才能回到分解前
    m_undoStack->beginMacro(QStringLiteral("分解块参照"));
    removeEntities(targets);
    if (!children.empty()) addEntities(std::move(children));
    m_undoStack->endMacro();
    return true;
}

std::vector<std::shared_ptr<BlockDefinition>> Document::referencedBlocks() const
{
    std::vector<std::shared_ptr<BlockDefinition>> out;
    std::unordered_set<QString> seen;

    std::vector<std::shared_ptr<BlockDefinition>> pending;
    for (const std::shared_ptr<BlockDefinition>& block : m_blocks.all()) {
        if (block) pending.push_back(block);
    }
    for (EntityId id : m_order) {
        const Entity* item = entity(id);
        if (!item || item->type() != EntityType::Insert) continue;
        const auto& insert = static_cast<const InsertEntity&>(*item);
        if (insert.block()) pending.push_back(insert.block());
    }

    while (!pending.empty()) {
        std::shared_ptr<BlockDefinition> block = pending.back();
        pending.pop_back();
        if (!block) continue;

        const QString key = BlockTable::normalizedKey(block->name());
        if (key.isEmpty() || seen.count(key) != 0) continue;
        seen.insert(key);
        out.push_back(block);

        // 嵌套块同样要写出去，否则外层块里的 INSERT 会指向一个不存在的定义
        for (const std::unique_ptr<Entity>& child : block->entities()) {
            if (!child || child->type() != EntityType::Insert) continue;
            const auto& nested = static_cast<const InsertEntity&>(*child);
            if (nested.block()) pending.push_back(nested.block());
        }
    }

    std::reverse(out.begin(), out.end());
    return out;
}

bool Document::transformEntities(const std::vector<EntityId>& ids,
                                 const Transform2D& transform,
                                 const QString& actionLabel)
{
    std::vector<EntityId> existing;
    existing.reserve(ids.size());
    for (EntityId id : ids) {
        if (m_entities.find(id) != m_entities.end()) existing.push_back(id);
    }
    if (existing.empty()) return false;

    const QString label = actionLabel.isEmpty() ? QStringLiteral("修改图元") : actionLabel;

    if (isBatchLoading()) {
        for (EntityId id : existing) {
            if (Entity* e = entity(id)) {
                e->transform(transform);
                emit entityChanged(id);
            }
        }
        afterEntitiesMutated();
        return true;
    }

    m_undoStack->push(new TransformEntitiesCommand(this, std::move(existing), transform, label));
    return true;
}

bool Document::setEntitiesLayer(const std::vector<EntityId>& ids, const QString& layerName)
{
    if (!m_layers.contains(layerName)) return false;
    if (ids.empty()) return false;

    m_undoStack->push(new EntityPropertyCommand(this, ids, EntityProperty::LayerName,
                                                layerName, 0, true,
                                                QStringLiteral("修改图层")));
    return true;
}

bool Document::setEntitiesColor(const std::vector<EntityId>& ids, int aci)
{
    if (ids.empty()) return false;
    m_undoStack->push(new EntityPropertyCommand(this, ids, EntityProperty::ColorIndex,
                                                QString(), aci, true,
                                                QStringLiteral("修改颜色")));
    return true;
}

bool Document::setEntitiesLineweight(const std::vector<EntityId>& ids, int dxfValue)
{
    if (ids.empty()) return false;
    m_undoStack->push(new EntityPropertyCommand(this, ids, EntityProperty::Lineweight,
                                                QString(), dxfValue, true,
                                                QStringLiteral("修改线宽")));
    return true;
}

bool Document::setEntitiesVisible(const std::vector<EntityId>& ids, bool visible)
{
    if (ids.empty()) return false;
    m_undoStack->push(new EntityPropertyCommand(this, ids, EntityProperty::Visible,
                                                QString(), 0, visible,
                                                visible ? QStringLiteral("显示图元")
                                                        : QStringLiteral("隐藏图元")));
    return true;
}

namespace {

/// 选择集里的文字图元。
///
/// 文字属性命令本身就只改文字，这里先筛一遍是为了「一个文字都没有时不压撤销栈」：
/// 否则在纯直线选择集上点一下字高，栈里会多出一条什么也没做的记录，
/// 用户按撤销时看到「撤销 修改字高」却毫无变化。
std::vector<EntityId> textEntitiesIn(const Document& document, const std::vector<EntityId>& ids)
{
    std::vector<EntityId> texts;
    texts.reserve(ids.size());
    for (EntityId id : ids) {
        const Entity* entity = document.entity(id);
        if (entity && entity->type() == EntityType::Text) texts.push_back(id);
    }
    return texts;
}

/// 选择集里的块参照。与 textEntitiesIn 同理：先筛一遍，避免空命令进撤销栈。
std::vector<EntityId> insertEntitiesIn(const Document& document, const std::vector<EntityId>& ids)
{
    std::vector<EntityId> inserts;
    inserts.reserve(ids.size());
    for (EntityId id : ids) {
        const Entity* entity = document.entity(id);
        if (entity && entity->type() == EntityType::Insert) inserts.push_back(id);
    }
    return inserts;
}

} // namespace

bool Document::setEntitiesTextHeight(const std::vector<EntityId>& ids, double height)
{
    if (!(height > 0.0)) return false;

    std::vector<EntityId> texts = textEntitiesIn(*this, ids);
    if (texts.empty()) return false;

    TextPropertyValues values;
    values.height = height;
    m_undoStack->push(new TextPropertyCommand(this, std::move(texts), values,
                                              QStringLiteral("修改字高")));
    return true;
}

bool Document::setEntitiesTextRotation(const std::vector<EntityId>& ids, double radians)
{
    std::vector<EntityId> texts = textEntitiesIn(*this, ids);
    if (texts.empty()) return false;

    TextPropertyValues values;
    values.rotation = radians;
    m_undoStack->push(new TextPropertyCommand(this, std::move(texts), values,
                                              QStringLiteral("修改文字旋转")));
    return true;
}

bool Document::setEntitiesTextHAlign(const std::vector<EntityId>& ids, TextEntity::HAlign hAlign)
{
    std::vector<EntityId> texts = textEntitiesIn(*this, ids);
    if (texts.empty()) return false;

    TextPropertyValues values;
    values.hAlign = hAlign;
    m_undoStack->push(new TextPropertyCommand(this, std::move(texts), values,
                                              QStringLiteral("修改文字水平对齐")));
    return true;
}

bool Document::setEntitiesTextVAlign(const std::vector<EntityId>& ids, TextEntity::VAlign vAlign)
{
    std::vector<EntityId> texts = textEntitiesIn(*this, ids);
    if (texts.empty()) return false;

    TextPropertyValues values;
    values.vAlign = vAlign;
    m_undoStack->push(new TextPropertyCommand(this, std::move(texts), values,
                                              QStringLiteral("修改文字垂直对齐")));
    return true;
}

void Document::ensureTextFontStyle(const QString& fontFamily)
{
    const QString family = fontFamily.trimmed();
    // Standard 是默认样式，始终在表里，且带的是中文兜底 SHX 字体——
    // 那不是用户选定的字体族，不应被这里改写。
    if (family.isEmpty()
        || family.compare(QStringLiteral("Standard"), Qt::CaseInsensitive) == 0) {
        return;
    }

    TextStyle& style = m_textStyles.ensure(family);
    if (style.hasFontInfo()) return;   // 已有字体信息（如文件读入的同名样式），保持原样

    // SHX 字体（"simplex.shx"）走主字体文件名；其余按 TrueType 族名登记，
    // 导出时写成 1000 扩展数据，渲染时按族名匹配系统字体。
    if (family.endsWith(QLatin1String(".shx"), Qt::CaseInsensitive)) {
        style.setFontFile(family);
    } else {
        style.setTtfName(family);
    }
}

bool Document::setEntitiesTextFont(const std::vector<EntityId>& ids, const QString& fontFamily)
{
    const QString family = fontFamily.trimmed();

    std::vector<EntityId> texts = textEntitiesIn(*this, ids);
    if (texts.empty()) return false;

    // 先登记样式、再让图元引用它。顺序反过来的话，导出时样式表里还没有这条样式，
    // 字体信息会被兜底成 txt.shx，选择在文件往返后就丢了。
    // 样式条目的新增不随撤销回退（与 AutoCAD 一致，多余样式需 PURGE 清理），
    // 但图元上的样式名引用是可撤销的——那才是用户感知到的这次修改。
    ensureTextFontStyle(family);

    TextPropertyValues values;
    values.fontStyle = family;
    m_undoStack->push(new TextPropertyCommand(this, std::move(texts), values,
                                              QStringLiteral("修改文字字体")));
    return true;
}

bool Document::setEntitiesInsertScale(const std::vector<EntityId>& ids,
                                      double scaleX, double scaleY)
{
    // 比例为 0 会把块压成一条线，之后再也无法通过属性面板恢复
    if (std::abs(scaleX) <= kGeomTol || std::abs(scaleY) <= kGeomTol) return false;

    std::vector<EntityId> inserts = insertEntitiesIn(*this, ids);
    if (inserts.empty()) return false;

    InsertPropertyValues values;
    values.scaleX = scaleX;
    values.scaleY = scaleY;
    m_undoStack->push(new InsertPropertyCommand(this, std::move(inserts), values,
                                                QStringLiteral("修改插入比例")));
    return true;
}

bool Document::setEntitiesInsertRotation(const std::vector<EntityId>& ids, double radians)
{
    std::vector<EntityId> inserts = insertEntitiesIn(*this, ids);
    if (inserts.empty()) return false;

    InsertPropertyValues values;
    values.rotation = radians;
    m_undoStack->push(new InsertPropertyCommand(this, std::move(inserts), values,
                                                QStringLiteral("修改插入旋转")));
    return true;
}

bool Document::replaceEntity(EntityId id,
                             std::unique_ptr<Entity> replacement,
                             const QString& actionLabel)
{
    if (!replacement || m_entities.find(id) == m_entities.end()) return false;

    const QString label = actionLabel.isEmpty() ? QStringLiteral("编辑图元") : actionLabel;
    m_undoStack->push(new ReplaceEntityCommand(this, id, std::move(replacement), label));
    return true;
}

// -----------------------------------------------------------------------------
//  图层
// -----------------------------------------------------------------------------
bool Document::addLayer(const Layer& layer, QString* error)
{
    const QString name = layer.name().trimmed();
    if (name.isEmpty()) {
        if (error) *error = QStringLiteral("图层名不能为空。");
        return false;
    }
    if (m_layers.contains(name)) {
        if (error) *error = QStringLiteral("图层 \"%1\" 已存在。").arg(name);
        return false;
    }

    Layer created = layer;
    created.setName(name);

    std::vector<Layer> before = m_layers.all();
    std::vector<Layer> after = before;
    after.push_back(created);

    m_undoStack->push(new LayerTableCommand(this, std::move(before), std::move(after),
                                            QString(), QString(),
                                            QStringLiteral("新建图层 \"%1\"").arg(name)));
    return true;
}

bool Document::removeLayer(const QString& name, bool moveEntitiesToDefault, QString* error)
{
    if (!m_layers.contains(name)) {
        if (error) *error = QStringLiteral("图层 \"%1\" 不存在。").arg(name);
        return false;
    }
    if (name == QLatin1String("0")) {
        if (error) *error = QStringLiteral("图层 \"0\" 为系统保留图层，不能删除。");
        return false;
    }

    const std::size_t used = static_cast<std::size_t>(
        std::count_if(m_entities.begin(), m_entities.end(),
                      [&name](const auto& pair) { return pair.second->layerName() == name; }));

    if (used > 0 && !moveEntitiesToDefault) {
        if (error) {
            *error = QStringLiteral("图层 \"%1\" 上仍有 %2 个图元，无法删除。").arg(name).arg(used);
        }
        return false;
    }

    std::vector<Layer> before = m_layers.all();
    std::vector<Layer> after;
    after.reserve(before.size());
    for (const Layer& l : before) {
        if (l.name() != name) after.push_back(l);
    }

    const QString retargetTo = moveEntitiesToDefault ? QStringLiteral("0") : QString();
    m_undoStack->push(new LayerTableCommand(
        this, std::move(before), std::move(after),
        name, retargetTo,
        QStringLiteral("删除图层 \"%1\"").arg(name)));
    return true;
}

bool Document::renameLayer(const QString& from, const QString& to, QString* error)
{
    const QString target = to.trimmed();
    if (target.isEmpty()) {
        if (error) *error = QStringLiteral("图层名不能为空。");
        return false;
    }
    if (from == target) return true;
    if (!m_layers.contains(from)) {
        if (error) *error = QStringLiteral("图层 \"%1\" 不存在。").arg(from);
        return false;
    }
    if (from == QLatin1String("0")) {
        if (error) *error = QStringLiteral("图层 \"0\" 为系统保留图层，不能重命名。");
        return false;
    }
    if (m_layers.contains(target)) {
        if (error) *error = QStringLiteral("图层 \"%1\" 已存在。").arg(target);
        return false;
    }

    std::vector<Layer> before = m_layers.all();
    std::vector<Layer> after = before;
    for (Layer& l : after) {
        if (l.name() == from) { l.setName(target); break; }
    }

    m_undoStack->push(new LayerTableCommand(
        this, std::move(before), std::move(after),
        from, target,
        QStringLiteral("重命名图层 \"%1\"").arg(from)));
    return true;
}

namespace {

/// 在图层表副本中就地修改指定图层，返回是否命中
template <typename Mutator>
bool mutateLayerCopy(std::vector<Layer>& layers, const QString& name, Mutator&& mutator)
{
    for (Layer& l : layers) {
        if (l.name() == name) {
            mutator(l);
            return true;
        }
    }
    return false;
}

} // namespace

bool Document::setLayerColor(const QString& name, int aci)
{
    if (!m_layers.contains(name)) return false;

    std::vector<Layer> before = m_layers.all();
    std::vector<Layer> after = before;
    mutateLayerCopy(after, name, [aci](Layer& l) { l.setColorIndex(aci); });

    m_undoStack->push(new LayerTableCommand(this, std::move(before), std::move(after),
                                            QString(), QString(),
                                            QStringLiteral("修改图层颜色")));
    return true;
}

bool Document::setLayerVisible(const QString& name, bool visible)
{
    if (!m_layers.contains(name)) return false;

    std::vector<Layer> before = m_layers.all();
    std::vector<Layer> after = before;
    mutateLayerCopy(after, name, [visible](Layer& l) { l.setVisible(visible); });

    m_undoStack->push(new LayerTableCommand(this, std::move(before), std::move(after),
                                            QString(), QString(),
                                            visible ? QStringLiteral("打开图层")
                                                    : QStringLiteral("关闭图层")));
    return true;
}

bool Document::setLayerFrozen(const QString& name, bool frozen)
{
    if (!m_layers.contains(name)) return false;

    std::vector<Layer> before = m_layers.all();
    std::vector<Layer> after = before;
    mutateLayerCopy(after, name, [frozen](Layer& l) { l.setFrozen(frozen); });

    m_undoStack->push(new LayerTableCommand(this, std::move(before), std::move(after),
                                            QString(), QString(),
                                            frozen ? QStringLiteral("冻结图层")
                                                   : QStringLiteral("解冻图层")));
    return true;
}

bool Document::setLayerLocked(const QString& name, bool locked)
{
    if (!m_layers.contains(name)) return false;

    std::vector<Layer> before = m_layers.all();
    std::vector<Layer> after = before;
    mutateLayerCopy(after, name, [locked](Layer& l) { l.setLocked(locked); });

    m_undoStack->push(new LayerTableCommand(this, std::move(before), std::move(after),
                                            QString(), QString(),
                                            locked ? QStringLiteral("锁定图层")
                                                   : QStringLiteral("解锁图层")));
    return true;
}

bool Document::setLayerLineweight(const QString& name, int dxfValue)
{
    if (!m_layers.contains(name)) return false;

    std::vector<Layer> before = m_layers.all();
    std::vector<Layer> after = before;
    mutateLayerCopy(after, name, [dxfValue](Layer& l) { l.setLineweightDxf(dxfValue); });

    m_undoStack->push(new LayerTableCommand(this, std::move(before), std::move(after),
                                            QString(), QString(),
                                            QStringLiteral("修改图层线宽")));
    return true;
}

bool Document::setCurrentLayer(const QString& name)
{
    if (!m_layers.contains(name)) return false;
    if (m_layers.currentLayerName() == name) return true;

    m_layers.setCurrentLayerName(name);
    emit layersChanged();
    return true;
}

void Document::notifyLayersChanged()
{
    emit layersChanged();
}

// -----------------------------------------------------------------------------
//  批量装载与清空
// -----------------------------------------------------------------------------
void Document::beginBatchLoad()
{
    ++m_batchDepth;
}

void Document::endBatchLoad()
{
    if (m_batchDepth > 0) --m_batchDepth;
    if (m_batchDepth > 0) return;

    m_undoStack->clear();
    m_undoStack->setClean();

    emit documentReset();
    emit layersChanged();
    emit blocksChanged();
    emit extentsChanged();
}

void Document::clear(bool resetLayers)
{
    const bool wasBatch = isBatchLoading();

    m_entities.clear();
    m_order.clear();
    m_selection->clear();
    m_nextId = 1;

    if (resetLayers) {
        m_layers.clear();
        m_textStyles.clear();
        m_blocks.clear();
    }

    if (!wasBatch) {
        m_undoStack->clear();
        m_undoStack->setClean();
    }

    emit documentReset();
    emit layersChanged();
    emit blocksChanged();
    emit extentsChanged();
}

void Document::markClean()
{
    m_undoStack->setClean();
}

bool Document::isModified() const
{
    return !m_undoStack->isClean();
}

// -----------------------------------------------------------------------------
//  范围
// -----------------------------------------------------------------------------
Box2D Document::extents() const
{
    Box2D box;
    for (EntityId id : m_order) {
        if (const Entity* e = entity(id)) {
            // 仅统计可见图元的范围，与「缩放范围」的语义一致
            if (!e->isDisplayable(m_layers)) continue;
            box.expand(e->boundingBox());
        }
    }
    return box;
}

Box2D Document::extentsOf(const std::vector<EntityId>& ids) const
{
    Box2D box;
    for (EntityId id : ids) {
        if (const Entity* e = entity(id)) box.expand(e->boundingBox());
    }
    return box;
}

// -----------------------------------------------------------------------------
//  文件关联信息
// -----------------------------------------------------------------------------
void Document::setFilePath(const QString& path)
{
    if (m_filePath == path) return;
    m_filePath = path;
    emit filePathChanged(m_filePath);
}

QString Document::displayName() const
{
    if (m_filePath.isEmpty()) return QStringLiteral("未命名");
    return QFileInfo(m_filePath).fileName();
}

QString Document::displayTitle() const
{
    return QStringLiteral("%1%2 - LyCAD")
        .arg(displayName(), isModified() ? QStringLiteral(" *") : QString());
}

} // namespace lycad
