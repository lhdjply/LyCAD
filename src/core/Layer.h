// =============================================================================
//  LyCAD - core/Layer.h
//  图层与图层表管理。
//
//  图层属性与 DXF LAYER 表记录的对应关系
//    70   标志位   bit0=冻结, bit2=锁定, bit1=冻结于新视口
//    62   颜色索引 ACI，负值表示该图层已关闭（取绝对值即颜色）
//    6    线型名
//    370  线宽（1/100 mm），-3 表示默认
//
//  注意 DXF 用「颜色取负」表达图层关闭，本模块在读写时做双向换算，
//  内部一律使用语义清晰的 visible / frozen / locked 三个布尔量。
// =============================================================================
#pragma once

#include "core/Types.h"

#include <QColor>
#include <QHash>
#include <QString>
#include <QStringList>

#include <cstddef>
#include <vector>

namespace lycad {

// -----------------------------------------------------------------------------
//  Layer
// -----------------------------------------------------------------------------
class Layer {
public:
    Layer() = default;
    explicit Layer(const QString& name) : m_name(name) {}

    // ---- 名称 ----------------------------------------------------------------
    const QString& name() const noexcept { return m_name; }
    void setName(const QString& n) { m_name = n; }

    // ---- 状态 ----------------------------------------------------------------
    bool isVisible() const noexcept { return m_visible; }
    void setVisible(bool v) noexcept { m_visible = v; }

    bool isFrozen() const noexcept { return m_frozen; }
    void setFrozen(bool f) noexcept { m_frozen = f; }

    bool isLocked() const noexcept { return m_locked; }
    void setLocked(bool l) noexcept { m_locked = l; }

    bool isPlottable() const noexcept { return m_plottable; }
    void setPlottable(bool p) noexcept { m_plottable = p; }

    /// 关闭或冻结均导致不可见
    bool isHidden() const noexcept { return !m_visible || m_frozen; }

    /// 是否可被选择：隐藏与锁定图层上的图元不可选
    bool isSelectable() const noexcept { return !isHidden() && !m_locked; }

    /// 是否为强制保留图层（"0" 层不可删除、不可改名）
    bool isMandatory() const noexcept { return m_name == QLatin1String("0"); }

    // ---- 显示属性 ------------------------------------------------------------
    int colorIndex() const noexcept { return m_colorIndex; }
    void setColorIndex(int aci) noexcept { m_colorIndex = aci; }
    QColor color(bool themeIsDark = false) const;

    const QString& linetype() const noexcept { return m_linetype; }
    void setLinetype(const QString& lt);

    int lineweightDxf() const noexcept { return m_lineweight; }
    void setLineweightDxf(int v) noexcept { m_lineweight = v; }

    /// 图层面板中的状态文字
    QString stateText() const;

    /// 状态标志位（用于 DXF 70 组码）
    int flagsForDxf() const;
    void setFlagsFromDxf(int flags);

private:
    QString m_name = QStringLiteral("0");
    bool    m_visible = true;
    bool    m_frozen = false;
    bool    m_locked = false;
    bool    m_plottable = true;
    int     m_colorIndex = 7;                          ///< ACI，0 号层默认为白/黑
    QString m_linetype = QStringLiteral("CONTINUOUS");
    int     m_lineweight = -3;                         ///< DXF 370 默认值
};

// -----------------------------------------------------------------------------
//  LayerManager
// -----------------------------------------------------------------------------
class LayerManager {
public:
    LayerManager();

    // ---- 查询 ----------------------------------------------------------------
    std::size_t count() const noexcept { return m_layers.size(); }
    bool isEmpty() const noexcept { return m_layers.empty(); }
    const std::vector<Layer>& all() const noexcept { return m_layers; }
    std::vector<Layer>& all() noexcept { return m_layers; }

    const Layer* find(const QString& name) const;
    Layer* find(const QString& name);

    bool contains(const QString& name) const;

    /// 不存在则创建并返回引用；用于导入外部文件时补齐缺失图层
    Layer& ensure(const QString& name);

    // ---- 修改 ----------------------------------------------------------------
    /// 新建图层。重名或名称为空时失败。
    bool add(const Layer& layer, QString* error = nullptr);

    /// 删除图层。"0" 层不可删除；inUse 为 true 时由调用方决定是否强制。
    bool remove(const QString& name, QString* error = nullptr);

    /// 重命名。"0" 层不可重命名。
    bool rename(const QString& from, const QString& to, QString* error = nullptr);

    /// 重置为仅含 "0" 层的初始状态
    void clear();

    /// 整体替换图层表（撤销/重做与批量装载使用）。
    /// 若原当前图层仍存在于新表中则保留，否则回落到 "0" 层。
    void setAll(std::vector<Layer> layers);

    // ---- 当前图层 ------------------------------------------------------------
    const QString& currentLayerName() const noexcept { return m_currentLayer; }
    void setCurrentLayerName(const QString& name);
    const Layer* currentLayer() const { return find(m_currentLayer); }

    // ---- 属性解析（供图元随层取属性） ------------------------------------------
    int colorIndexOf(const QString& layerName, int fallback = 7) const;
    int lineweightOf(const QString& layerName, int fallback = -3) const;
    QString linetypeOf(const QString& layerName, const QString& fallback = QStringLiteral("Continuous")) const;

    bool isHidden(const QString& layerName) const;
    bool isLocked(const QString& layerName) const;
    bool isSelectable(const QString& layerName) const;

    // ---- 命名辅助 ------------------------------------------------------------
    /// 生成不与现有图层冲突的名称，如 "图层1"、"图层2"
    QString makeUniqueName(const QString& base) const;

    /// 按顺序分配下一个建议颜色
    int suggestColorIndex() const;

    /// 所有图层名
    QStringList names() const;

private:
    void rebuildIndex();

    std::vector<Layer> m_layers;
    QHash<QString, std::size_t> m_index;
    QString m_currentLayer = QStringLiteral("0");
};

} // namespace lycad
