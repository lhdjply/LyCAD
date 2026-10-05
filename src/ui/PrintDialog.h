// =============================================================================
//  LyCAD - ui/PrintDialog.h
//  打印对话框：**左边是全部设置，右边就是打印预览**。
//
//  为什么不用 QPrintDialog：那是 Qt 的通用打印对话框，只有打印机与份数，
//  出图比例这类 CAD 特有设置放不进去，用户得在「设置对话框 → 打印对话框 →
//  再开一个预览」之间来回跳。这里把它们合成一屏：
//
//      ┌───────────────┬──────────────────────────┐
//      │ 目标（打印机/PDF）│                          │
//      │ 纸张 / 方向      │   打印预览（随左侧实时刷新）│
//      │ 出图比例        │                          │
//      │ 份数            │                          │
//      └───────────────┴──────────────────────────┘
//
//  预览与真正打印走的是**同一段绘制代码**（render/PlotRenderer）与同一台
//  QPrinter，因此改任何一项设置，右边看到的就是纸面上将出现的东西；
//  点「打印」后界面层做的只剩把页面画到这台打印机上。
//
//  目标支持两类：
//    · 本机打印机（QPrinterInfo 枚举）；
//    · 导出为 PDF（QPrinter 的 PdfFormat，向量输出）。
//  没有安装打印机时自动只剩 PDF 一项。
// =============================================================================
#pragma once

#include "core/Types.h"
#include "render/PlotRenderer.h"

#include <QDialog>
#include <QPrinter>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpinBox;
class QToolButton;
class QPrintPreviewWidget;

namespace lycad {

class Document;

class PrintDialog : public QDialog {
    Q_OBJECT

public:
    /// 出图去向
    enum class Destination {
        Printer,    ///< 本机打印机
        PdfFile     ///< 导出为 PDF 文件
    };

    /// document 只用于绘制预览与推断默认 PDF 文件名；可为空（此时预览为空白页）
    PrintDialog(const Document* document, const PlotScale& currentScale,
                QWidget* parent = nullptr);

    // ---- 结果（accept 之后读取） ------------------------------------------------
    PlotScale selectedScale() const;
    /// 当前对话框状态的出图参数（比例随左侧选项实时变化）
    PlotOptions plotOptions() const;
    Destination destination() const;
    /// destination == Printer 时返回对话框内配置好的打印机（随对话框存活）
    QPrinter* printer();
    /// destination == PdfFile 时的输出路径
    QString pdfFilePath() const;

protected:
    void showEvent(QShowEvent* event) override;

private:
    void buildDestinationPage();
    void buildPaperPage();
    void buildScalePage();
    void buildCopiesPage();

    /// 把左边的全部设置落到 m_printer 上，然后刷新右侧预览
    void applyAndRefresh();
    /// 目标切换（打印机 ↔ PDF）时的字段可用性与打印机重定向
    void applyDestination();
    /// 打印按钮的可用性校验：PDF 目标必须已有输出路径
    bool validate();

    /// 预览控件不提供页数变化信号，页码栏在每次刷新后按它的实际状态重建
    void refreshPageBar();

    PlotScale m_scale;      ///< 进入对话框时的出图比例（决定初始勾选项）

    const Document* m_document = nullptr;

    // ---- 目标 ----
    QComboBox* m_destinationCombo = nullptr;
    QLabel* m_printerSummary = nullptr;
    QLineEdit* m_pdfPath = nullptr;
    QToolButton* m_pdfBrowse = nullptr;
    QLabel* m_pdfLabel = nullptr;

    // ---- 纸张 ----
    QComboBox* m_paperCombo = nullptr;
    QRadioButton* m_landscape = nullptr;
    QRadioButton* m_portrait = nullptr;

    // ---- 出图比例 ----
    QRadioButton* m_fit = nullptr;
    QRadioButton* m_oneToOne = nullptr;
    QRadioButton* m_custom = nullptr;
    QDoubleSpinBox* m_customValue = nullptr;

    // ---- 份数 ----
    QSpinBox* m_copies = nullptr;

    // ---- 预览与执行 ----
    QPrintPreviewWidget* m_preview = nullptr;
    QToolButton* m_prevPage = nullptr;
    QToolButton* m_nextPage = nullptr;
    QSpinBox* m_pageSpin = nullptr;
    QLabel* m_pageTotal = nullptr;
    QPrinter m_printer;
    bool m_updating = false;    ///< 防止「设置变化 → 刷新预览 → 又触发设置变化」的循环
};

} // namespace lycad
