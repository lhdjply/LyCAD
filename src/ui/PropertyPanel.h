// =============================================================================
//  LyCAD - ui/PropertyPanel.h
//  对象属性面板。
//
//  与图层面板遵循同一约定：只发意图信号，不直接改文档。
//  多选时显示公共值，无法统一取值的字段留空，避免给出误导性的「当前值」。
// =============================================================================
#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;

namespace lycad {

class Document;

class PropertyPanel : public QWidget {
    Q_OBJECT

public:
    explicit PropertyPanel(QWidget* parent = nullptr);

    /// 依据文档的选择集刷新。document 为空时显示空状态。
    void refresh(const Document* document);

signals:
    void layerChangeRequested(const QString& layerName);
    void colorChangeRequested(int aci);
    void lineweightChangeRequested(int dxfValue);
    void visibleChangeRequested(bool visible);

private:
    void setEnabledState(bool enabled);

    QLabel*         m_summaryLabel = nullptr;
    QComboBox*      m_layerCombo = nullptr;
    QComboBox*      m_colorCombo = nullptr;
    QComboBox*      m_lineweightCombo = nullptr;
    QCheckBox*      m_visibleCheck = nullptr;
    QPlainTextEdit* m_geometryText = nullptr;

    bool m_updating = false;
};

} // namespace lycad
