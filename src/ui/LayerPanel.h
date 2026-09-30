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

class QComboBox;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;

namespace lycad {

class Document;
class LayerManager;

class LayerPanel : public QWidget {
    Q_OBJECT

public:
    explicit LayerPanel(QWidget* parent = nullptr);

    /// 依据文档重建表格。currentLayer 用于同步顶部下拉框。
    void refresh(const LayerManager& layers, const QString& currentLayer);

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

    void buildUi();
    void onItemChanged(QTableWidgetItem* item);
    void onCellDoubleClicked(int row, int column);
    void requestRemoveSelected();
    void requestRenameSelected();

    QComboBox*    m_currentLayerCombo = nullptr;
    QPushButton*  m_addButton = nullptr;
    QPushButton*  m_removeButton = nullptr;
    QPushButton*  m_renameButton = nullptr;
    QTableWidget* m_table = nullptr;

    /// 重建表格期间置为 true，用于屏蔽 itemChanged 造成的回环
    bool m_updating = false;
};

} // namespace lycad
