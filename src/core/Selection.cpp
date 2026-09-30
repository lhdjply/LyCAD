// =============================================================================
//  LyCAD - core/Selection.cpp
// =============================================================================
#include "core/Selection.h"

#include <algorithm>

namespace lycad {

Selection::Selection(QObject* parent)
    : QObject(parent)
{
}

void Selection::setIds(std::vector<EntityId> ids)
{
    std::vector<EntityId> unique;
    unique.reserve(ids.size());
    std::unordered_set<EntityId> lookup;
    lookup.reserve(ids.size());

    for (EntityId id : ids) {
        if (id == kInvalidEntityId) continue;
        if (lookup.insert(id).second) unique.push_back(id);
    }

    if (unique == m_ids) return;

    m_ids = std::move(unique);
    m_lookup = std::move(lookup);
    emit changed();
}

void Selection::add(EntityId id)
{
    if (id == kInvalidEntityId) return;
    if (!m_lookup.insert(id).second) return;
    m_ids.push_back(id);
    emit changed();
}

void Selection::add(const std::vector<EntityId>& ids)
{
    bool any = false;
    for (EntityId id : ids) {
        if (id == kInvalidEntityId) continue;
        if (m_lookup.insert(id).second) {
            m_ids.push_back(id);
            any = true;
        }
    }
    if (any) emit changed();
}

void Selection::remove(EntityId id)
{
    if (m_lookup.erase(id) == 0) return;
    m_ids.erase(std::remove(m_ids.begin(), m_ids.end(), id), m_ids.end());
    emit changed();
}

void Selection::toggle(EntityId id)
{
    if (id == kInvalidEntityId) return;
    if (contains(id)) {
        remove(id);
    } else {
        add(id);
    }
}

void Selection::clear()
{
    if (m_ids.empty()) return;
    m_ids.clear();
    m_lookup.clear();
    emit changed();
}

void Selection::prune(const std::function<bool(EntityId)>& keep)
{
    const std::size_t before = m_ids.size();
    m_ids.erase(std::remove_if(m_ids.begin(), m_ids.end(),
                               [&](EntityId id) { return !keep(id); }),
                m_ids.end());

    if (m_ids.size() == before) return;

    m_lookup.clear();
    m_lookup.reserve(m_ids.size());
    for (EntityId id : m_ids) m_lookup.insert(id);
    emit changed();
}

} // namespace lycad
