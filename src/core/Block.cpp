// =============================================================================
//  LyCAD - core/Block.cpp
// =============================================================================
#include "core/Block.h"

#include "core/InsertEntity.h"

#include <algorithm>

namespace lycad {

// -----------------------------------------------------------------------------
//  BlockDefinition
// -----------------------------------------------------------------------------
BlockDefinition::BlockDefinition() = default;

BlockDefinition::BlockDefinition(const QString& name)
    : m_name(name)
{
}

BlockDefinition::~BlockDefinition() = default;

void BlockDefinition::addEntity(std::unique_ptr<Entity> entity)
{
    if (!entity) return;
    m_entities.push_back(std::move(entity));
}

const AttributeDefinition* BlockDefinition::findAttributeDefinition(const QString& tag) const
{
    const QString key = tag.trimmed();
    for (const AttributeDefinition& def : m_attributeDefs) {
        if (def.tag.compare(key, Qt::CaseInsensitive) == 0) return &def;
    }
    return nullptr;
}

std::unique_ptr<BlockDefinition> BlockDefinition::clone() const
{
    auto copy = std::make_unique<BlockDefinition>(m_name);
    copy->m_basePoint = m_basePoint;
    copy->m_flags = m_flags;
    copy->m_attributeDefs = m_attributeDefs;
    copy->m_entities.reserve(m_entities.size());
    for (const std::unique_ptr<Entity>& entity : m_entities) {
        if (entity) copy->m_entities.push_back(entity->clone());
    }
    return copy;
}

Box2D BlockDefinition::boundingBox() const
{
    // 嵌套块不需要在这里展开：InsertEntity::boundingBox() 返回的正是「它在父坐标系
    // （对本块而言就是块坐标系）下的范围」，与普通图元的包围盒口径一致。
    Box2D box;
    for (const std::unique_ptr<Entity>& entity : m_entities) {
        if (entity) box.expand(entity->boundingBox());
    }
    return box;
}

std::size_t BlockDefinition::expandedEntityCount(std::size_t maxDepth) const
{
    std::size_t total = 0;
    for (const std::unique_ptr<Entity>& entity : m_entities) {
        if (!entity) continue;
        if (entity->type() == EntityType::Insert) {
            total += static_cast<const InsertEntity&>(*entity).expandedCount(maxDepth);
        } else {
            ++total;
        }
    }
    return total;
}

// -----------------------------------------------------------------------------
//  BlockTable
// -----------------------------------------------------------------------------
QString BlockTable::normalizedKey(const QString& name)
{
    return name.trimmed().toUpper();
}

bool BlockTable::contains(const QString& name) const
{
    return m_index.find(normalizedKey(name)) != m_index.end();
}

const BlockDefinition* BlockTable::find(const QString& name) const
{
    const auto it = m_index.find(normalizedKey(name));
    return it == m_index.end() ? nullptr : m_blocks[it->second].get();
}

BlockDefinition* BlockTable::find(const QString& name)
{
    const auto it = m_index.find(normalizedKey(name));
    return it == m_index.end() ? nullptr : m_blocks[it->second].get();
}

void BlockTable::add(std::shared_ptr<BlockDefinition> block)
{
    if (!block) return;

    const QString key = normalizedKey(block->name());
    const auto it = m_index.find(key);
    if (it != m_index.end()) {
        m_blocks[it->second] = std::move(block);
        return;
    }

    m_index.emplace(key, m_blocks.size());
    m_blocks.push_back(std::move(block));
}

bool BlockTable::remove(const QString& name)
{
    const auto it = m_index.find(normalizedKey(name));
    if (it == m_index.end()) return false;

    const std::size_t index = it->second;
    m_blocks.erase(m_blocks.begin() + static_cast<std::ptrdiff_t>(index));
    m_index.erase(it);

    // 后面的下标整体前移
    for (auto& entry : m_index) {
        if (entry.second > index) --entry.second;
    }
    return true;
}

void BlockTable::clear()
{
    m_blocks.clear();
    m_index.clear();
}

QString BlockTable::uniqueName(const QString& base) const
{
    QString candidate = base.trimmed();
    if (candidate.isEmpty()) candidate = QStringLiteral("Block");

    if (!contains(candidate)) return candidate;
    for (int i = 2; i < 100000; ++i) {
        const QString next = QStringLiteral("%1%2").arg(candidate).arg(i);
        if (!contains(next)) return next;
    }
    return candidate;
}

} // namespace lycad
