// =============================================================================
//  LyCAD - ui/PropertyPanel.h
//  对象属性面板。
//
//  与图层面板遵循同一约定：只发意图信号，不直接改文档。
//  多选时显示公共值，无法统一取值的字段留空，避免给出误导性的「当前值」。
//
//  面板分三组：通用属性（所有图元）、文字（仅文字图元，选择集里没有文字时整组隐藏）、
//  几何信息（只读摘要）。
// =============================================================================
#pragma once

#include "core/TextEntity.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
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

    /// 字高（世界单位）
    void textHeightChangeRequested(double height);
    /// 旋转（弧度）。面板上显示的是度，换算在面板内完成，信号与文档同一口径。
    void textRotationChangeRequested(double radians);
    /// 水平对齐（DXF 72）
    void textHAlignChangeRequested(TextEntity::HAlign hAlign);
    /// 垂直对齐（DXF 73）
    void textVAlignChangeRequested(TextEntity::VAlign vAlign);

    /// 块参照插入比例（X / Y；Z 保持不变）
    void blockScaleChangeRequested(double scaleX, double scaleY);
    /// 块参照插入旋转（弧度）。面板上显示的是度，换算在面板内完成。
    void blockRotationChangeRequested(double radians);

private:
    void setEnabledState(bool enabled);
    /// 把数值输入框清空并显示「多种取值」占位（多选且取值不一致时使用）
    void setMixed(QDoubleSpinBox* box);

    QLabel*         m_summaryLabel = nullptr;
    QComboBox*      m_layerCombo = nullptr;
    QComboBox*      m_colorCombo = nullptr;
    QComboBox*      m_lineweightCombo = nullptr;
    QCheckBox*      m_visibleCheck = nullptr;

    QGroupBox*      m_textGroup = nullptr;
    QDoubleSpinBox* m_textHeight = nullptr;
    QDoubleSpinBox* m_textRotation = nullptr;      ///< 显示单位是度
    QComboBox*      m_textHAlign = nullptr;
    QComboBox*      m_textVAlign = nullptr;

    QGroupBox*      m_blockGroup = nullptr;
    QLabel*         m_blockName = nullptr;
    QDoubleSpinBox* m_blockScaleX = nullptr;
    QDoubleSpinBox* m_blockScaleY = nullptr;
    QDoubleSpinBox* m_blockRotation = nullptr;     ///< 显示单位是度
    QLabel*         m_blockInfo = nullptr;

    QPlainTextEdit* m_geometryText = nullptr;

    bool m_updating = false;
};

} // namespace lycad
