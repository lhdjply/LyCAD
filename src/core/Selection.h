// =============================================================================
//  LyCAD - core/Selection.h
//  选择集。
//
//  采用「有序集合」结构：对外按选择顺序枚举（便于命令行统计与属性面板显示），
//  内部维护哈希表用于 O(1) 命中判断。选择集会主动剔除已被删除的图元。
// =============================================================================
#pragma once

#include "core/Types.h"

#include <QObject>

#include <functional>
#include <unordered_set>
#include <vector>

namespace lycad {

class Selection : public QObject {
    Q_OBJECT

public:
    explicit Selection(QObject* parent = nullptr);

    const std::vector<EntityId>& ids() const noexcept { return m_ids; }

    bool contains(EntityId id) const { return m_lookup.find(id) != m_lookup.end(); }
    bool isEmpty() const noexcept { return m_ids.empty(); }
    std::size_t size() const noexcept { return m_ids.size(); }

    /// 整体替换
    void setIds(std::vector<EntityId> ids);

    void add(EntityId id);
    void add(const std::vector<EntityId>& ids);
    void remove(EntityId id);
    void toggle(EntityId id);
    void clear();

    /// 剔除不满足条件的图元（文档删除图元后调用）
    void prune(const std::function<bool(EntityId)>& keep);

signals:
    void changed();

private:
    std::vector<EntityId> m_ids;
    std::unordered_set<EntityId> m_lookup;
};

} // namespace lycad
