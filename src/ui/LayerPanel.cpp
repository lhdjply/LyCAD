// =============================================================================
//  LyCAD - ui/LayerPanel.cpp
// =============================================================================
#include "ui/LayerPanel.h"

#include "core/AciPalette.h"
#include "core/Document.h"
#include "core/Entity.h"
#include "core/Layer.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QVBoxLayout>

namespace lycad {

namespace {

/// 图层表可选的线宽档位（DXF 370 值，单位为 1/100 mm）
const QVector<int>& lineweightChoices()
{
    static const QVector<int> kChoices = {
        -3, 0, 5, 9, 13, 15, 18, 20, 25, 30, 35, 40, 50, 53, 60, 70, 80, 90, 100, 106, 120
    };
    return kChoices;
}

/// 标准九色，供双击颜色单元格时快速选择
const QVector<int>& standardColors()
{
    static const QVector<int> kColors = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    return kColors;
}

QTableWidgetItem* makeCheckItem(bool checked)
{
    auto* item = new QTableWidgetItem();
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    item->setTextAlignment(Qt::AlignCenter);
    return item;
}

QTableWidgetItem* makeReadOnlyItem(const QString& text)
{
    auto* item = new QTableWidgetItem(text);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return item;
}

} // namespace

LayerPanel::LayerPanel(QWidget* parent)
    : QWidget(parent)
{
    buildUi();
}

void LayerPanel::buildUi()
{
    // ---- 顶部：当前图层下拉框与操作按钮 ----
    m_currentLayerCombo = new QComboBox(this);
    m_currentLayerCombo->setToolTip(QStringLiteral("当前图层：新建图元将归入该图层"));
    m_currentLayerCombo->setMinimumWidth(120);

    m_addButton = new QPushButton(QStringLiteral("新建"), this);
    m_removeButton = new QPushButton(QStringLiteral("删除"), this);
    m_renameButton = new QPushButton(QStringLiteral("重命名"), this);

    m_addButton->setToolTip(QStringLiteral("新建图层"));
    m_removeButton->setToolTip(QStringLiteral("删除选中的图层（其上的图元将移至图层 0）"));
    m_renameButton->setToolTip(QStringLiteral("重命名选中的图层"));

    auto* topLayout = new QHBoxLayout();
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(4);
    topLayout->addWidget(new QLabel(QStringLiteral("当前:"), this));
    topLayout->addWidget(m_currentLayerCombo, 1);
    topLayout->addWidget(m_addButton);
    topLayout->addWidget(m_removeButton);
    topLayout->addWidget(m_renameButton);

    // ---- 图层表格 ----
    m_table = new QTableWidget(this);
    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("显示"), QStringLiteral("冻结"), QStringLiteral("锁定"),
        QStringLiteral("图层名"), QStringLiteral("颜色"),
        QStringLiteral("线型"), QStringLiteral("线宽")
    });
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setColumnWidth(ColumnVisible, 46);
    m_table->setColumnWidth(ColumnFrozen, 46);
    m_table->setColumnWidth(ColumnLocked, 46);
    m_table->setColumnWidth(ColumnName, 110);
    m_table->setColumnWidth(ColumnColor, 70);

    // ---- 下方：选中图层的「对象列表」 ----
    //
    // 单纯靠画布拾取无法恢复已隐藏的图元（隐藏后不可选），因此这里提供一条
    // 独立于画布的恢复通道：选中图层后，列出该图层上的全部图元（含已隐藏者），
    // 每一行带「显示」复选框，勾选即可重新显示。
    m_objectTitle = new QLabel(QStringLiteral("对象列表"), this);
    m_objectTitle->setToolTip(
        QStringLiteral("选中上方图层后，这里列出该图层上的全部对象（含已隐藏的）。\n"
                       "取消 / 勾选「显示」列即可隐藏或恢复显示对象。"));

    m_objectTable = new QTableWidget(this);
    m_objectTable->setColumnCount(ObjectColumnCount);
    m_objectTable->setHorizontalHeaderLabels({
        QStringLiteral("显示"), QStringLiteral("类型"),
        QStringLiteral("#"), QStringLiteral("图层")
    });
    m_objectTable->verticalHeader()->setVisible(false);
    m_objectTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_objectTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_objectTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_objectTable->setAlternatingRowColors(true);
    m_objectTable->horizontalHeader()->setStretchLastSection(true);
    m_objectTable->setColumnWidth(ObjectColumnVisible, 46);
    m_objectTable->setColumnWidth(ObjectColumnType, 90);
    m_objectTable->setColumnWidth(ObjectColumnId, 60);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);
    layout->addLayout(topLayout);
    layout->addWidget(m_table, 1);
    layout->addWidget(m_objectTitle);
    layout->addWidget(m_objectTable, 1);

    // ---- 信号连接 ----
    connect(m_table, &QTableWidget::itemChanged, this, &LayerPanel::onItemChanged);
    connect(m_table, &QTableWidget::cellDoubleClicked, this, &LayerPanel::onCellDoubleClicked);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        // 切换选中图层时刷新其对象列表；这里不改「当前图层」，只重列对象。
        emit layerHighlightChanged();
    });

    connect(m_objectTable, &QTableWidget::itemChanged, this,
            &LayerPanel::onObjectItemChanged);
    connect(m_objectTable, &QTableWidget::itemSelectionChanged, this,
            &LayerPanel::onObjectSelectionChanged);
    connect(m_objectTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        // 双击行 = 选中该对象（便于到画布上定位），并保持「可见」行为不变
        if (QTableWidgetItem* idItem = m_objectTable->item(row, ObjectColumnId)) {
            const EntityId id = idItem->data(Qt::UserRole).toULongLong();
            if (id != kInvalidEntityId) emit entitySelectionRequested({ id });
        }
    });

    connect(m_addButton, &QPushButton::clicked, this, &LayerPanel::addLayerRequested);
    connect(m_removeButton, &QPushButton::clicked, this, &LayerPanel::requestRemoveSelected);
    connect(m_renameButton, &QPushButton::clicked, this, &LayerPanel::requestRenameSelected);

    connect(m_currentLayerCombo, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        if (m_updating || name.isEmpty()) return;
        emit currentLayerChangeRequested(name);
    });
}

void LayerPanel::refresh(const LayerManager& layers, const QString& currentLayer)
{
    m_updating = true;

    // 重建前记住当前选中的图层名——setRowCount(0) 会把 currentRow() 复位为 -1，
    // 若不恢复，后续 refreshObjectList() 就查不到选中图层，对象列表会被清空。
    const QString keepSelected = selectedLayerName();

    m_currentLayerCombo->clear();
    for (const Layer& layer : layers.all()) {
        m_currentLayerCombo->addItem(layer.name());
    }
    const int currentIndex = m_currentLayerCombo->findText(currentLayer);
    if (currentIndex >= 0) m_currentLayerCombo->setCurrentIndex(currentIndex);

    m_table->setRowCount(0);
    const std::vector<Layer>& all = layers.all();
    m_table->setRowCount(static_cast<int>(all.size()));

    for (int row = 0; row < static_cast<int>(all.size()); ++row) {
        const Layer& layer = all[static_cast<std::size_t>(row)];
        const bool isDefaultLayer = (layer.name() == QLatin1String("0"));

        m_table->setItem(row, ColumnVisible, makeCheckItem(layer.isVisible()));
        m_table->setItem(row, ColumnFrozen, makeCheckItem(layer.isFrozen()));
        m_table->setItem(row, ColumnLocked, makeCheckItem(layer.isLocked()));

        auto* nameItem = makeReadOnlyItem(layer.name());
        nameItem->setData(Qt::UserRole, layer.name());
        if (isDefaultLayer) {
            nameItem->setToolTip(QStringLiteral("图层 0 为系统保留图层，不可删除或重命名"));
        }
        m_table->setItem(row, ColumnName, nameItem);

        auto* colorItem = makeReadOnlyItem(aci::toName(layer.colorIndex()));
        colorItem->setData(Qt::UserRole, layer.colorIndex());
        colorItem->setBackground(aci::toColor(layer.colorIndex()));
        colorItem->setForeground(
            aci::toColor(layer.colorIndex()).lightness() > 140 ? QColor(Qt::black)
                                                               : QColor(Qt::white));
        colorItem->setToolTip(QStringLiteral("双击选择颜色"));
        m_table->setItem(row, ColumnColor, colorItem);

        auto* linetypeItem = makeReadOnlyItem(layer.linetype());
        linetypeItem->setToolTip(QStringLiteral("线型在 DXF 往返中保留"));
        m_table->setItem(row, ColumnLinetype, linetypeItem);

        auto* lwItem = makeReadOnlyItem(lineweight::toDisplayString(layer.lineweightDxf()));
        lwItem->setData(Qt::UserRole, layer.lineweightDxf());
        lwItem->setToolTip(QStringLiteral("双击选择线宽"));
        m_table->setItem(row, ColumnLineweight, lwItem);

        // 当前图层以粗体标示
        if (layer.name() == currentLayer) {
            for (int column = 0; column < ColumnCount; ++column) {
                if (QTableWidgetItem* item = m_table->item(row, column)) {
                    QFont font = item->font();
                    font.setBold(true);
                    item->setFont(font);
                }
            }
        }

        // 恢复重建前选中的图层行，使下方对象列表保持可见
        if (!keepSelected.isEmpty() && layer.name() == keepSelected) {
            m_table->selectRow(row);
        }
    }

    m_updating = false;
}

QString LayerPanel::selectedLayerName() const
{
    const int row = m_table->currentRow();
    if (row < 0) return QString();
    if (QTableWidgetItem* item = m_table->item(row, ColumnName)) {
        return item->data(Qt::UserRole).toString();
    }
    return QString();
}

void LayerPanel::refreshObjectList(Document* document)
{
    // 若本面板正在处理用户点击（如点选对象行），此时文档选择集变化会同步回调到
    // 这里。在点击处理栈内销毁表格项会打断 Qt 的选中逻辑，导致列表错乱/消失，
    // 因此这里记下「稍后重建」，等交互结束后补做一次。
    if (m_interacting) {
        m_pendingObjectReload = true;
        return;
    }

    m_updatingObjects = true;

    m_objectDocument = document;

    const QString layerName = selectedLayerName();
    if (!document || layerName.isEmpty()) {
        m_objectTable->setRowCount(0);
        m_objectTitle->setText(QStringLiteral("对象列表"));
        m_updatingObjects = false;
        return;
    }

    const std::vector<Entity*> entities = document->entitiesOnLayer(layerName);
    m_objectTitle->setText(
        QStringLiteral("对象列表 — 图层 \"%1\"（%2 个）").arg(layerName).arg(entities.size()));

    // 行选中状态直接以文档选择集为准。这样无论来源是画布拾取还是本列表点击，
    // 列表高亮都与实际选择一致，不会再出现「选了别的行、旧行还亮着」的错位。
    const std::vector<EntityId>& docSelection = document->selection().ids();
    QSet<EntityId> selectedIds(docSelection.begin(), docSelection.end());

    m_objectTable->setRowCount(static_cast<int>(entities.size()));
    m_objectTable->clearSelection();
    for (int row = 0; row < static_cast<int>(entities.size()); ++row) {
        Entity* entity = entities[static_cast<std::size_t>(row)];
        if (!entity) continue;

        // 「显示」复选框：以图元真实的 m_visible 为准
        m_objectTable->setItem(row, ObjectColumnVisible, makeCheckItem(entity->isVisible()));

        auto* typeItem = makeReadOnlyItem(entity->typeDisplayName());
        m_objectTable->setItem(row, ObjectColumnType, typeItem);

        auto* idItem = makeReadOnlyItem(QString::number(entity->id()));
        idItem->setData(Qt::UserRole, static_cast<qulonglong>(entity->id()));
        m_objectTable->setItem(row, ObjectColumnId, idItem);

        m_objectTable->setItem(row, ObjectColumnLayer, makeReadOnlyItem(entity->layerName()));

        if (selectedIds.contains(entity->id())) {
            m_objectTable->selectRow(row);
        }
    }

    m_updatingObjects = false;
}

void LayerPanel::onObjectItemChanged(QTableWidgetItem* item)
{
    if (m_updatingObjects || !item) return;
    if (item->column() != ObjectColumnVisible) return;

    QTableWidgetItem* idItem = m_objectTable->item(item->row(), ObjectColumnId);
    if (!idItem) return;
    const EntityId id = idItem->data(Qt::UserRole).toULongLong();
    if (id == kInvalidEntityId) return;

    const bool visible = (item->checkState() == Qt::Checked);

    m_interacting = true;
    emit entityVisibilityChangeRequested({ id }, visible);
    m_interacting = false;

    if (m_pendingObjectReload) {
        m_pendingObjectReload = false;
        if (Document* doc = m_objectDocument) refreshObjectList(doc);
    }
}

void LayerPanel::onObjectSelectionChanged()
{
    if (m_updatingObjects) return;

    std::vector<EntityId> ids;
    const QList<QTableWidgetItem*> selected = m_objectTable->selectedItems();
    ids.reserve(static_cast<std::size_t>(selected.size()));
    for (QTableWidgetItem* item : selected) {
        if (item->column() != ObjectColumnId) continue;
        const EntityId id = item->data(Qt::UserRole).toULongLong();
        if (id != kInvalidEntityId) ids.push_back(id);
    }
    if (ids.empty()) return;

    m_interacting = true;
    emit entitySelectionRequested(ids);
    m_interacting = false;

    if (m_pendingObjectReload) {
        m_pendingObjectReload = false;
        // 交互已结束，补做一次刷新，使列表与实际选择集对齐
        if (Document* doc = m_objectDocument) refreshObjectList(doc);
    }
}

void LayerPanel::onItemChanged(QTableWidgetItem* item)
{
    if (m_updating || !item) return;

    QTableWidgetItem* nameItem = m_table->item(item->row(), ColumnName);
    if (!nameItem) return;
    const QString layerName = nameItem->data(Qt::UserRole).toString();
    if (layerName.isEmpty()) return;

    const bool checked = (item->checkState() == Qt::Checked);

    switch (item->column()) {
    case ColumnVisible:
        emit layerVisibilityChangeRequested(layerName, checked);
        break;
    case ColumnFrozen:
        emit layerFrozenChangeRequested(layerName, checked);
        break;
    case ColumnLocked:
        emit layerLockedChangeRequested(layerName, checked);
        break;
    default:
        break;
    }
}

void LayerPanel::onCellDoubleClicked(int row, int column)
{
    QTableWidgetItem* nameItem = m_table->item(row, ColumnName);
    if (!nameItem) return;
    const QString layerName = nameItem->data(Qt::UserRole).toString();

    if (column == ColumnName) {
        m_table->setCurrentCell(row, column);
        requestRenameSelected();
        return;
    }

    if (column == ColumnColor) {
        QMenu menu(this);
        for (int aciIndex : standardColors()) {
            QAction* action = menu.addAction(aci::toName(aciIndex));
            action->setData(aciIndex);
            const QColor color = aci::toColor(aciIndex);
            QPixmap swatch(14, 14);
            swatch.fill(color);
            action->setIcon(QIcon(swatch));
        }
        QAction* picked = menu.exec(QCursor::pos());
        if (picked) {
            emit layerColorChangeRequested(layerName, picked->data().toInt());
        }
        return;
    }

    if (column == ColumnLineweight) {
        QMenu menu(this);
        for (int value : lineweightChoices()) {
            QAction* action = menu.addAction(lineweight::toDisplayString(value));
            action->setData(value);
        }
        QAction* picked = menu.exec(QCursor::pos());
        if (picked) {
            emit layerLineweightChangeRequested(layerName, picked->data().toInt());
        }
        return;
    }
}

void LayerPanel::requestRemoveSelected()
{
    const QString name = selectedLayerName();
    if (name.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("删除图层"),
                                 QStringLiteral("请先在列表中选择一个图层。"));
        return;
    }
    if (name == QLatin1String("0")) {
        QMessageBox::warning(this, QStringLiteral("删除图层"),
                             QStringLiteral("图层 \"0\" 为系统保留图层，不能删除。"));
        return;
    }

    const auto answer = QMessageBox::question(
        this, QStringLiteral("删除图层"),
        QStringLiteral("确定要删除图层 \"%1\" 吗？\n该图层上的图元将被移动到图层 \"0\"。").arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (answer == QMessageBox::Yes) {
        emit removeLayerRequested(name);
    }
}

void LayerPanel::requestRenameSelected()
{
    const QString name = selectedLayerName();
    if (name.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("重命名图层"),
                                 QStringLiteral("请先在列表中选择一个图层。"));
        return;
    }
    if (name == QLatin1String("0")) {
        QMessageBox::warning(this, QStringLiteral("重命名图层"),
                             QStringLiteral("图层 \"0\" 为系统保留图层，不能重命名。"));
        return;
    }

    bool ok = false;
    const QString newName = QInputDialog::getText(this, QStringLiteral("重命名图层"),
                                                  QStringLiteral("新名称:"),
                                                  QLineEdit::Normal, name, &ok);
    if (!ok) return;

    const QString trimmed = newName.trimmed();
    if (trimmed.isEmpty() || trimmed == name) return;

    emit renameLayerRequested(name, trimmed);
}

} // namespace lycad
