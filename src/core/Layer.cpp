// =============================================================================
//  LyCAD - core/Layer.cpp
// =============================================================================
#include "core/Layer.h"

#include "core/AciPalette.h"
#include "core/Linetype.h"

namespace lycad {

// -----------------------------------------------------------------------------
//  Layer
// -----------------------------------------------------------------------------
void Layer::setLinetype(const QString& linetype)
{
    // 归一化到内置线型的规范拼写，避免 "dashed"/"DASHED" 在文件中来回漂移
    m_linetype = linetypes::canonicalName(linetype);
}

QColor Layer::color(bool themeIsDark) const
{
    return aci::toColor(m_colorIndex, themeIsDark);
}

QString Layer::stateText() const
{
    QStringList parts;
    if (!m_visible) parts << QStringLiteral("关闭");
    if (m_frozen) parts << QStringLiteral("冻结");
    if (m_locked) parts << QStringLiteral("锁定");
    if (!m_plottable) parts << QStringLiteral("不打印");
    return parts.isEmpty() ? QStringLiteral("正常") : parts.join(QLatin1Char('/'));
}

int Layer::flagsForDxf() const
{
    int flags = 0;
    if (m_frozen) flags |= 0x01;
    if (m_locked) flags |= 0x04;
    return flags;
}

void Layer::setFlagsFromDxf(int flags)
{
    m_frozen = (flags & 0x01) != 0;
    m_locked = (flags & 0x04) != 0;
}

// -----------------------------------------------------------------------------
//  LayerManager
// -----------------------------------------------------------------------------
LayerManager::LayerManager()
{
    clear();
}

void LayerManager::clear()
{
    m_layers.clear();
    m_index.clear();
    // "0" 层是 DXF 规范要求必须存在的图层
    m_layers.emplace_back(QStringLiteral("0"));
    m_currentLayer = QStringLiteral("0");
    rebuildIndex();
}

void LayerManager::rebuildIndex()
{
    m_index.clear();
    for (std::size_t i = 0; i < m_layers.size(); ++i) {
        m_index.insert(m_layers[i].name(), i);
    }
}

void LayerManager::setAll(std::vector<Layer> layers)
{
    const QString previousCurrent = m_currentLayer;

    m_layers = std::move(layers);
    rebuildIndex();

    // 图层表至少要保留 "0" 层，否则补齐
    if (m_index.isEmpty() || !contains(QStringLiteral("0"))) {
        m_layers.insert(m_layers.begin(), Layer(QStringLiteral("0")));
        rebuildIndex();
    }

    if (contains(previousCurrent)) {
        m_currentLayer = previousCurrent;
    } else {
        m_currentLayer = QStringLiteral("0");
    }
}

const Layer* LayerManager::find(const QString& name) const
{
    const auto it = m_index.constFind(name);
    if (it == m_index.constEnd()) return nullptr;
    return &m_layers[it.value()];
}

Layer* LayerManager::find(const QString& name)
{
    const auto it = m_index.constFind(name);
    if (it == m_index.constEnd()) return nullptr;
    return &m_layers[it.value()];
}

bool LayerManager::contains(const QString& name) const
{
    return m_index.contains(name);
}

Layer& LayerManager::ensure(const QString& name)
{
    if (Layer* existing = find(name)) return *existing;

    Layer layer(name);
    // 自动补齐的图层按顺序分配一个可辨识的颜色
    layer.setColorIndex(suggestColorIndex());
    m_layers.push_back(layer);
    m_index.insert(name, m_layers.size() - 1);
    return m_layers.back();
}

bool LayerManager::add(const Layer& layer, QString* error)
{
    const QString name = layer.name().trimmed();
    if (name.isEmpty()) {
        if (error) *error = QStringLiteral("图层名不能为空。");
        return false;
    }
    if (contains(name)) {
        if (error) *error = QStringLiteral("图层 \"%1\" 已存在。").arg(name);
        return false;
    }

    Layer copy = layer;
    copy.setName(name);
    m_layers.push_back(copy);
    m_index.insert(name, m_layers.size() - 1);
    return true;
}

bool LayerManager::remove(const QString& name, QString* error)
{
    const auto it = m_index.constFind(name);
    if (it == m_index.constEnd()) {
        if (error) *error = QStringLiteral("图层 \"%1\" 不存在。").arg(name);
        return false;
    }
    if (name == QLatin1String("0")) {
        if (error) *error = QStringLiteral("图层 \"0\" 是系统保留图层，不能删除。");
        return false;
    }

    m_layers.erase(m_layers.begin() + static_cast<std::ptrdiff_t>(it.value()));
    rebuildIndex();

    if (m_currentLayer == name) m_currentLayer = QStringLiteral("0");
    return true;
}

bool LayerManager::rename(const QString& from, const QString& to, QString* error)
{
    const QString target = to.trimmed();
    if (target.isEmpty()) {
        if (error) *error = QStringLiteral("图层名不能为空。");
        return false;
    }
    if (from == target) return true;
    if (from == QLatin1String("0")) {
        if (error) *error = QStringLiteral("图层 \"0\" 是系统保留图层，不能重命名。");
        return false;
    }

    Layer* layer = find(from);
    if (!layer) {
        if (error) *error = QStringLiteral("图层 \"%1\" 不存在。").arg(from);
        return false;
    }
    if (contains(target)) {
        if (error) *error = QStringLiteral("图层 \"%1\" 已存在。").arg(target);
        return false;
    }

    layer->setName(target);
    rebuildIndex();
    if (m_currentLayer == from) m_currentLayer = target;
    return true;
}

void LayerManager::setCurrentLayerName(const QString& name)
{
    if (contains(name)) m_currentLayer = name;
}

int LayerManager::colorIndexOf(const QString& layerName, int fallback) const
{
    if (const Layer* l = find(layerName)) return l->colorIndex();
    return fallback;
}

int LayerManager::lineweightOf(const QString& layerName, int fallback) const
{
    if (const Layer* l = find(layerName)) return l->lineweightDxf();
    return fallback;
}

QString LayerManager::linetypeOf(const QString& layerName, const QString& fallback) const
{
    if (const Layer* l = find(layerName)) {
        return l->linetype().isEmpty() ? fallback : l->linetype();
    }
    return fallback;
}

bool LayerManager::isHidden(const QString& layerName) const
{
    if (const Layer* l = find(layerName)) return l->isHidden();
    return false;
}

bool LayerManager::isLocked(const QString& layerName) const
{
    if (const Layer* l = find(layerName)) return l->isLocked();
    return false;
}

bool LayerManager::isSelectable(const QString& layerName) const
{
    if (const Layer* l = find(layerName)) return l->isSelectable();
    return true;
}

QString LayerManager::makeUniqueName(const QString& base) const
{
    const QString stem = base.trimmed().isEmpty() ? QStringLiteral("图层") : base.trimmed();
    if (!contains(stem)) return stem;
    for (int i = 1; i < 100000; ++i) {
        const QString candidate = QStringLiteral("%1%2").arg(stem).arg(i);
        if (!contains(candidate)) return candidate;
    }
    return stem;
}

int LayerManager::suggestColorIndex() const
{
    // 与既有图层颜色错开，避免新建图层与已有图层同色难以区分
    int seed = 1;
    if (!m_layers.empty()) {
        seed = m_layers.back().colorIndex();
    }
    for (int attempt = 0; attempt < 32; ++attempt) {
        seed = aci::nextIndexForNewLayer(seed);
        bool used = false;
        for (const Layer& l : m_layers) {
            if (l.colorIndex() == seed) { used = true; break; }
        }
        if (!used) return seed;
    }
    return seed;
}

QStringList LayerManager::names() const
{
    QStringList out;
    out.reserve(static_cast<qsizetype>(m_layers.size()));
    for (const Layer& l : m_layers) out << l.name();
    return out;
}

} // namespace lycad
