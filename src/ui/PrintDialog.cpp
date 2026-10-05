// =============================================================================
//  LyCAD - ui/PrintDialog.cpp
// =============================================================================
#include "ui/PrintDialog.h"

#include "core/Document.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPageSize>
#include <QPrintPreviewWidget>
#include <QPrinterInfo>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

namespace lycad {

namespace {

/// 对话框里的纸张清单：工程图常用幅面在前，通用幅面在后
struct PaperEntry {
    QPageSize::PageSizeId id;
    const char* label;
};

const PaperEntry kPapers[] = {
    { QPageSize::A4,       "A4" },
    { QPageSize::A3,       "A3" },
    { QPageSize::A2,       "A2" },
    { QPageSize::A1,       "A1" },
    { QPageSize::A0,       "A0" },
    { QPageSize::B5,       "B5" },
    { QPageSize::B4,       "B4" },
    { QPageSize::Letter,   "Letter" },
    { QPageSize::Legal,    "Legal" },
    { QPageSize::Tabloid,  "Tabloid" },
};

constexpr int kPdfDestinationIndex = 0;   // 「导出为 PDF」固定是下拉框第一项

} // namespace

PrintDialog::PrintDialog(const Document* document, const PlotScale& currentScale,
                         QWidget* parent)
    : QDialog(parent)
    , m_scale(currentScale)      // 初始化顺序须与声明顺序一致（-Wreorder）
    , m_document(document)
{
    setWindowTitle(QStringLiteral("打印"));
    setModal(true);
    resize(1120, 740);

    // 打印机对象是整台对话框的中枢：左边每改一项都落到它身上，右边的预览
    // 与点「打印」后的真正出图用的都是它
    m_printer.setOutputFormat(QPrinter::NativeFormat);
    if (const QPrinterInfo defaultPrinter = QPrinterInfo::defaultPrinter();
        !defaultPrinter.isNull()) {
        m_printer.setPrinterName(defaultPrinter.printerName());
    }
    m_printer.setPageSize(QPageSize(QPageSize::A4));
    m_printer.setPageOrientation(QPageLayout::Landscape);
    // 封顶 600 dpi：A4 横向在 1200 dpi 下可打印区是 13633×9516 像素，预览每翻
    // 一页都要在这个坐标系里走一遍；600 dpi 已高于工程出图的实际精度，且线宽
    // 与字高的像素值从此不随设备漂移
    m_printer.setResolution(std::min(m_printer.resolution(), 600));

    // ---- 左列：设置 ----
    auto* settings = new QVBoxLayout;
    settings->setContentsMargins(0, 0, 0, 0);

    buildDestinationPage();
    buildPaperPage();
    buildScalePage();
    buildCopiesPage();
    for (QWidget* page : {findChild<QWidget*>(QStringLiteral("printDialog.page.destination")),
                          findChild<QWidget*>(QStringLiteral("printDialog.page.paper")),
                          findChild<QWidget*>(QStringLiteral("printDialog.page.scale")),
                          findChild<QWidget*>(QStringLiteral("printDialog.page.copies"))}) {
        if (page) settings->addWidget(page);
    }
    settings->addStretch(1);

    // ---- 右列：页码导航 + 预览 ----
    m_prevPage = new QToolButton(this);
    m_prevPage->setText(QStringLiteral("◀"));
    m_prevPage->setToolTip(QStringLiteral("上一页"));
    m_nextPage = new QToolButton(this);
    m_nextPage->setText(QStringLiteral("▶"));
    m_nextPage->setToolTip(QStringLiteral("下一页"));
    m_pageSpin = new QSpinBox(this);
    m_pageSpin->setRange(1, 1);
    m_pageSpin->setObjectName(QStringLiteral("printDialog.pageSpin"));
    m_pageTotal = new QLabel(QStringLiteral("共 1 页"), this);

    connect(m_prevPage, &QToolButton::clicked, this, [this]() {
        m_preview->setCurrentPage(m_preview->currentPage() - 1);
        refreshPageBar();
    });
    connect(m_nextPage, &QToolButton::clicked, this, [this]() {
        m_preview->setCurrentPage(m_preview->currentPage() + 1);
        refreshPageBar();
    });
    connect(m_pageSpin, &QSpinBox::valueChanged, this, [this](int value) {
        if (value - 1 != m_preview->currentPage()) {
            m_preview->setCurrentPage(value - 1);
        }
        refreshPageBar();
    });

    m_preview = new QPrintPreviewWidget(&m_printer, this);
    m_preview->setMinimumWidth(560);
    connect(m_preview, &QPrintPreviewWidget::paintRequested, this,
            [this](QPrinter* target) {
                QPainter painter;
                if (!painter.begin(target) || !m_document) return;
                const QRectF page = target->pageLayout().paintRectPixels(target->resolution());
                PlotOptions options;
                options.scale = selectedScale();
                renderAllPages(painter, *m_document, page,
                               target->resolution() / 25.4, options,
                               [target]() { return target->newPage(); });
                painter.end();
            });

    auto* pageBar = new QHBoxLayout;
    pageBar->setContentsMargins(0, 0, 0, 4);
    pageBar->addWidget(m_prevPage);
    pageBar->addWidget(m_pageSpin);
    pageBar->addWidget(m_pageTotal);
    pageBar->addWidget(m_nextPage);
    pageBar->addStretch(1);

    auto* right = new QVBoxLayout;
    right->setContentsMargins(0, 0, 0, 0);
    right->addLayout(pageBar);
    right->addWidget(m_preview, 1);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(10, 10, 10, 6);
    auto* settingsHolder = new QWidget(this);
    settingsHolder->setLayout(settings);
    settingsHolder->setFixedWidth(330);
    body->addWidget(settingsHolder);
    body->addLayout(right, 1);

    // ---- 底部：按钮 ----
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, this);
    QPushButton* printButton = buttons->button(QDialogButtonBox::Ok);
    printButton->setText(QStringLiteral("打印"));
    printButton->setObjectName(QStringLiteral("printDialog.printButton"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    buttons->button(QDialogButtonBox::Cancel)->setObjectName(
        QStringLiteral("printDialog.cancelButton"));
    connect(printButton, &QPushButton::clicked, this, [this]() {
        if (validate()) accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addLayout(body, 1);
    auto* buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(10, 0, 10, 8);
    buttonRow->addStretch(1);
    buttonRow->addWidget(buttons);
    root->addLayout(buttonRow);

    applyDestination();
    applyAndRefresh();
}

void PrintDialog::buildDestinationPage()
{
    auto* page = new QWidget(this);
    page->setObjectName(QStringLiteral("printDialog.page.destination"));
    auto* form = new QFormLayout(page);
    form->setContentsMargins(0, 0, 0, 0);

    m_destinationCombo = new QComboBox(page);
    m_destinationCombo->setObjectName(QStringLiteral("printDialog.destination"));
    const QList<QPrinterInfo> printers = QPrinterInfo::availablePrinters();
    for (const QPrinterInfo& info : printers) {
        m_destinationCombo->addItem(info.printerName());
    }
    m_destinationCombo->insertItem(kPdfDestinationIndex, QStringLiteral("导出为 PDF"));
    // 默认选「导出为 PDF」：无显示环境的机器与装了打印机的机器行为一致，
    // 也避免验证工具或误触把作业真的送到打印机上
    m_destinationCombo->setCurrentIndex(kPdfDestinationIndex);
    connect(m_destinationCombo, &QComboBox::currentIndexChanged, this,
            [this](int) { applyDestination(); applyAndRefresh(); });
    form->addRow(QStringLiteral("目标"), m_destinationCombo);

    m_printerSummary = new QLabel(page);
    m_printerSummary->setWordWrap(true);
    m_printerSummary->setStyleSheet(QStringLiteral("color: palette(mid);"));
    form->addRow(QString(), m_printerSummary);

    m_pdfLabel = new QLabel(QStringLiteral("PDF 文件"), page);
    m_pdfPath = new QLineEdit(page);
    m_pdfPath->setObjectName(QStringLiteral("printDialog.pdfPath"));
    m_pdfBrowse = new QToolButton(page);
    m_pdfBrowse->setText(QStringLiteral("…"));
    m_pdfBrowse->setToolTip(QStringLiteral("选择 PDF 保存位置"));
    connect(m_pdfBrowse, &QToolButton::clicked, this, [this]() {
        const QString suggested = m_pdfPath->text().isEmpty()
                                      ? QDir::homePath() + QStringLiteral("/drawing.pdf")
                                      : m_pdfPath->text();
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("导出为 PDF"), suggested,
            QStringLiteral("PDF 文件 (*.pdf)"));
        if (!path.isEmpty()) m_pdfPath->setText(path);
    });
    auto* pathRow = new QHBoxLayout;
    pathRow->setContentsMargins(0, 0, 0, 0);
    pathRow->addWidget(m_pdfPath, 1);
    pathRow->addWidget(m_pdfBrowse);
    form->addRow(m_pdfLabel, pathRow);

    connect(m_pdfPath, &QLineEdit::textChanged, this,
            [this](const QString&) { applyAndRefresh(); });
}

void PrintDialog::buildPaperPage()
{
    auto* page = new QWidget(this);
    page->setObjectName(QStringLiteral("printDialog.page.paper"));
    auto* form = new QFormLayout(page);
    form->setContentsMargins(0, 0, 0, 0);

    m_paperCombo = new QComboBox(page);
    for (const PaperEntry& entry : kPapers) {
        m_paperCombo->addItem(QString::fromLatin1(entry.label),
                              static_cast<int>(entry.id));
    }
    m_paperCombo->setCurrentIndex(0);   // A4
    connect(m_paperCombo, &QComboBox::currentIndexChanged, this,
            [this](int) { applyAndRefresh(); });
    form->addRow(QStringLiteral("纸张"), m_paperCombo);

    m_landscape = new QRadioButton(QStringLiteral("横向"), page);
    m_portrait = new QRadioButton(QStringLiteral("纵向"), page);
    m_landscape->setChecked(true);
    connect(m_landscape, &QRadioButton::toggled, this,
            [this](bool) { applyAndRefresh(); });
    auto* orientationRow = new QHBoxLayout;
    orientationRow->setContentsMargins(0, 0, 0, 0);
    orientationRow->addWidget(m_landscape);
    orientationRow->addWidget(m_portrait);
    orientationRow->addStretch(1);
    form->addRow(QStringLiteral("方向"), orientationRow);
}

void PrintDialog::buildScalePage()
{
    auto* page = new QWidget(this);
    page->setObjectName(QStringLiteral("printDialog.page.scale"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);

    m_fit = new QRadioButton(QStringLiteral("适应图纸（铺满可打印区）"), page);
    m_oneToOne = new QRadioButton(QStringLiteral("1:1（纸面 1 mm = 1 个图形单位）"), page);
    m_custom = new QRadioButton(QStringLiteral("自定义：纸面 1 mm ="), page);
    m_customValue = new QDoubleSpinBox(page);
    m_customValue->setDecimals(4);
    m_customValue->setRange(0.0001, 1000000.0);
    m_customValue->setSingleStep(0.5);
    m_customValue->setToolTip(QStringLiteral("即工程制图的 1:n：n 越大，图在纸上越小"));

    // 初始勾选必须与当前设置一致，否则打开对话框时三个单选全是空的——
    // 用户看不出现在的出图比例是什么，也点不出「取消」之外的安全状态
    switch (m_scale.mode) {
    case PlotScale::Mode::FitToPaper: m_fit->setChecked(true); break;
    case PlotScale::Mode::OneToOne:   m_oneToOne->setChecked(true); break;
    case PlotScale::Mode::Custom:     m_custom->setChecked(true); break;
    }
    m_customValue->setValue(m_scale.custom);

    auto* customRow = new QHBoxLayout;
    customRow->setContentsMargins(0, 0, 0, 0);
    customRow->addWidget(m_custom);
    customRow->addWidget(m_customValue, 1);
    customRow->addWidget(new QLabel(QStringLiteral("个图形单位"), page));

    layout->addWidget(new QLabel(QStringLiteral("出图比例"), page));
    layout->addWidget(m_fit);
    layout->addWidget(m_oneToOne);
    layout->addLayout(customRow);

    connect(m_fit, &QRadioButton::toggled, this, [this](bool) { applyAndRefresh(); });
    connect(m_oneToOne, &QRadioButton::toggled, this, [this](bool) { applyAndRefresh(); });
    connect(m_custom, &QRadioButton::toggled, this, [this](bool) { applyAndRefresh(); });
    connect(m_customValue, &QDoubleSpinBox::valueChanged, this,
            [this](double) { applyAndRefresh(); });
}

void PrintDialog::buildCopiesPage()
{
    auto* page = new QWidget(this);
    page->setObjectName(QStringLiteral("printDialog.page.copies"));
    auto* form = new QFormLayout(page);
    form->setContentsMargins(0, 0, 0, 0);

    m_copies = new QSpinBox(page);
    m_copies->setRange(1, 999);
    m_copies->setValue(1);
    form->addRow(QStringLiteral("份数"), m_copies);
}

void PrintDialog::applyDestination()
{
    const bool pdf = m_destinationCombo->currentIndex() == kPdfDestinationIndex;

    // 默认 PDF 文件名：与图形同目录同名（未保存的新图退回主目录）
    if (m_pdfPath->text().isEmpty() && m_document) {
        const QString source = m_document->filePath();
        QDir dir = source.isEmpty() ? QDir::home() : QFileInfo(source).dir();
        m_pdfPath->setText(dir.filePath(m_document->displayName()
                                        + QStringLiteral(".pdf")));
    }

    m_pdfLabel->setVisible(pdf);
    m_pdfPath->setVisible(pdf);
    m_pdfBrowse->setVisible(pdf);
    m_printerSummary->setVisible(!pdf);
    if (!pdf) {
        m_printerSummary->setText(
            QStringLiteral("将使用系统打印驱动输出；预览即为打印内容。"));
    }
}

bool PrintDialog::validate()
{
    if (destination() == Destination::PdfFile && m_pdfPath->text().trimmed().isEmpty()) {
        m_pdfPath->setFocus();
        return false;
    }
    return true;
}

void PrintDialog::applyAndRefresh()
{
    // 构造顺序是「先建设置页、后建预览」：初始勾选会触发 toggled 信号走到这里，
    // 此时预览控件还不存在，刷新要让路——构造结束处会统一刷一次
    if (m_updating || !m_preview) return;
    m_updating = true;

    // ---- 目标 ----
    const bool pdf = destination() == Destination::PdfFile;
    if (pdf) {
        if (m_printer.outputFormat() != QPrinter::PdfFormat) {
            m_printer.setOutputFormat(QPrinter::PdfFormat);
        }
        m_printer.setOutputFileName(m_pdfPath->text());
    } else {
        if (m_printer.outputFormat() != QPrinter::NativeFormat) {
            m_printer.setOutputFormat(QPrinter::NativeFormat);
        }
        m_printer.setPrinterName(m_destinationCombo->currentText());
    }

    // ---- 纸张与方向 ----
    const auto paperId = static_cast<QPageSize::PageSizeId>(
        m_paperCombo->currentData().toInt());
    m_printer.setPageSize(QPageSize(paperId));
    m_printer.setPageOrientation(m_landscape->isChecked() ? QPageLayout::Landscape
                                                          : QPageLayout::Portrait);
    m_printer.setCopyCount(m_copies->value());
    m_printer.setResolution(std::min(m_printer.resolution(), 600));

    m_updating = false;
    m_preview->updatePreview();     // 右侧跟着重画：预览即所得
    refreshPageBar();               // 页数可能随比例/纸张变化
}

void PrintDialog::refreshPageBar()
{
    // QPrintPreviewWidget 不提供页数变化信号，页数在每次刷新后直接读它的状态。
    // 它数的页数来自绘制回调里真正翻过的页，与打印输出天然一致。
    const int count = std::max(1, m_preview->pageCount());
    const int current = std::clamp(m_preview->currentPage(), 0, count - 1);

    m_pageSpin->blockSignals(true);
    m_pageSpin->setRange(1, count);
    m_pageSpin->setValue(current + 1);
    m_pageSpin->blockSignals(false);
    m_pageTotal->setText(QStringLiteral("共 %1 页").arg(count));
    m_prevPage->setEnabled(current > 0);
    m_nextPage->setEnabled(current < count - 1);
}

void PrintDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    m_preview->updatePreview();
    refreshPageBar();
}

PlotScale PrintDialog::selectedScale() const
{
    PlotScale scale;
    if (m_oneToOne->isChecked()) {
        scale.mode = PlotScale::Mode::OneToOne;
    } else if (m_custom->isChecked()) {
        scale.mode = PlotScale::Mode::Custom;
        scale.custom = m_customValue->value();
    } else {
        scale.mode = PlotScale::Mode::FitToPaper;
    }
    return scale;
}

PlotOptions PrintDialog::plotOptions() const
{
    PlotOptions options;
    options.scale = selectedScale();
    return options;
}

PrintDialog::Destination PrintDialog::destination() const
{
    return m_destinationCombo->currentIndex() == kPdfDestinationIndex
               ? Destination::PdfFile
               : Destination::Printer;
}

QPrinter* PrintDialog::printer()
{
    return &m_printer;
}

QString PrintDialog::pdfFilePath() const
{
    return m_pdfPath->text();
}

} // namespace lycad
