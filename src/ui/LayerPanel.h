// =============================================================================
//  LyCAD - ui/LayerPanel.h
//  图层管理面板。
//
//  面板是「纯视图」：它只发出意图信号，绝不直接修改文档。
//  所有变更都由主窗口落到 Document 的对应方法上，从而保证：
//    · 每一次图层变更都会进入撤销栈；
//    · 面板本身无需关心撤销、标记为已修改等横切逻辑。
// =============================================================================
#pragma once

#include <QWidget>

#include <vector>

#include "core/Types.h"

class QComboBox;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;
class QLabel;

namespace lycad {

class Document;
class LayerManager;

class LayerPanel : public QWidget {
    Q_OBJECT

public:
    explicit LayerPanel(QWidget* parent = nullptr);

    /// 依据文档重建表格。currentLayer 用于同步顶部下拉框。
    void refresh(const LayerManager& layers, const QString& currentLayer);

    /// 重建下方「对象列表」：列出当前选中图层上的全部图元（含已隐藏者）。
    /// 传入文档以便读取图元属性；无选中图层时清空列表。
    void refreshObjectList(Document* document);

    /// 当前在表格中选中的图层名；无选中返回空
    QString selectedLayerName() const;

signals:
    void currentLayerChangeRequested(const QString& name);
    void addLayerRequested();
    void removeLayerRequested(const QString& name);
    void renameLayerRequested(const QString& oldName, const QString& newName);
    void layerVisibilityChangeRequested(const QString& name, bool visible);
    void layerFrozenChangeRequested(const QString& name, bool frozen);
    void layerLockedChangeRequested(const QString& name, bool locked);
    void layerColorChangeRequested(const QString& name, int aci);
    void layerLineweightChangeRequested(const QString& name, int dxfValue);

    /// 请求把若干图元设为可见 / 不可见（来自下方对象列表的复选框）
    void entityVisibilityChangeRequested(const std::vector<EntityId>& ids, bool visible);
    /// 请求选中若干图元（把面板里的选择同步到画布）
    void entitySelectionRequested(const std::vector<EntityId>& ids);
    /// 图层表选中行发生变化（用于刷新下方对象列表，不改「当前图层」）
    void layerHighlightChanged();

private:
    enum Column {
        ColumnVisible = 0,
        ColumnFrozen,
        ColumnLocked,
        ColumnName,
        ColumnColor,
        ColumnLinetype,
        ColumnLineweight,
        ColumnCount
    };

    enum ObjectColumn {
        ObjectColumnVisible = 0,
        ObjectColumnType,
        ObjectColumnId,
        ObjectColumnLayer,
        ObjectColumnCount
    };

    void buildUi();
    void onItemChanged(QTableWidgetItem* item);
    void onCellDoubleClicked(int row, int column);
    void onObjectItemChanged(QTableWidgetItem* item);
    void onObjectSelectionChanged();
    void requestRemoveSelected();
    void requestRenameSelected();

    QComboBox*    m_currentLayerCombo = nullptr;
    QPushButton*  m_addButton = nullptr;
    QPushButton*  m_removeButton = nullptr;
    QPushButton*  m_renameButton = nullptr;
    QTableWidget* m_table = nullptr;

    QLabel*       m_objectTitle = nullptr;
    QTableWidget* m_objectTable = nullptr;

    /// 最近一次 refreshObjectList 传入的文档，用于交互结束后补刷新
    Document*     m_objectDocument = nullptr;

    /// 重建表格期间置为 true，用于屏蔽 itemChanged 造成的回环
    bool m_updating = false;

    /// 重建对象列表期间置为 true，用于屏蔽复选框/选中回环
    bool m_updatingObjects = false;

    /// 本面板正处于「用户交互」中（点击对象行）。置位期间拒绝来自文档的
    /// 对象列表重建，避免在点击处理栈内销毁正在被操作的表格项。
    bool m_interacting = false;

    /// 延迟重建请求：交互结束后补做一次刷新
    bool m_pendingObjectReload = false;
};

} // namespace lycad
