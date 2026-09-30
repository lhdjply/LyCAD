// =============================================================================
//  LyCAD - ui/PropertyPanel.cpp
// =============================================================================
#include "ui/PropertyPanel.h"

#include "core/Document.h"
#include "core/Entity.h"
#include "core/Format.h"
#include "ui/PropertyChoices.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPlainTextEdit>

#include <limits>
#include <QVBoxLayout>

namespace lycad {

PropertyPanel::PropertyPanel(QWidget* parent)
    : QWidget(parent)
{
    m_summaryLabel = new QLabel(QStringLiteral("未选择对象"), this);
    QFont summaryFont = m_summaryLabel->font();
    summaryFont.setBold(true);
    m_summaryLabel->setFont(summaryFont);

    m_layerCombo = new QComboBox(this);
    m_layerCombo->setToolTip(QStringLiteral("把选中对象移动到其它图层"));

    m_colorCombo = new QComboBox(this);
    for (int aci : propertyChoices::colors()) {
        m_colorCombo->addItem(propertyChoices::colorSwatch(aci),
                              propertyChoices::colorText(aci), QVariant(aci));
    }
    m_colorCombo->setToolTip(QStringLiteral("图元颜色；选择「随层」时继承所属图层"));

    m_lineweightCombo = new QComboBox(this);
    for (int value : propertyChoices::lineweights()) {
        m_lineweightCombo->addItem(propertyChoices::lineweightText(value), value);
    }
    m_lineweightCombo->setToolTip(QStringLiteral("图元线宽（打印与线宽显示模式下生效）"));

    m_visibleCheck = new QCheckBox(QStringLiteral("可见"), this);
    m_visibleCheck->setToolTip(QStringLiteral("取消勾选可在不删除的前提下隐藏图元"));

    auto* form = new QFormLayout();
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->addRow(QStringLiteral("图层"), m_layerCombo);
    form->addRow(QStringLiteral("颜色"), m_colorCombo);
    form->addRow(QStringLiteral("线宽"), m_lineweightCombo);
    form->addRow(QString(), m_visibleCheck);

    auto* formGroup = new QGroupBox(QStringLiteral("通用属性"), this);
    formGroup->setLayout(form);

    m_geometryText = new QPlainTextEdit(this);
    m_geometryText->setReadOnly(true);
    m_geometryText->setMinimumHeight(160);
    m_geometryText->setLineWrapMode(QPlainTextEdit::NoWrap);

    auto* geometryGroup = new QGroupBox(QStringLiteral("几何信息"), this);
    auto* geometryLayout = new QVBoxLayout(geometryGroup);
    geometryLayout->addWidget(m_geometryText);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);
    layout->addWidget(m_summaryLabel);
    layout->addWidget(formGroup);
    layout->addWidget(geometryGroup, 1);

    connect(m_layerCombo, &QComboBox::currentTextChanged, this, [this](const QString& name) {
        if (m_updating || name.isEmpty()) return;
        emit layerChangeRequested(name);
    });
    connect(m_colorCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_updating || index < 0) return;
        emit colorChangeRequested(m_colorCombo->itemData(index).toInt());
    });
    connect(m_lineweightCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_updating || index < 0) return;
        emit lineweightChangeRequested(m_lineweightCombo->itemData(index).toInt());
    });
    connect(m_visibleCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_updating) return;
        emit visibleChangeRequested(checked);
    });

    refresh(nullptr);
}

void PropertyPanel::setEnabledState(bool enabled)
{
    m_layerCombo->setEnabled(enabled);
    m_colorCombo->setEnabled(enabled);
    m_lineweightCombo->setEnabled(enabled);
    m_visibleCheck->setEnabled(enabled);
}

void PropertyPanel::refresh(const Document* document)
{
    m_updating = true;

    if (!document || document->selection().isEmpty()) {
        m_summaryLabel->setText(QStringLiteral("未选择对象"));
        m_geometryText->setPlainText(QStringLiteral("在画布中选择对象以查看其属性。"));
        setEnabledState(false);
        m_updating = false;
        return;
    }

    setEnabledState(true);

    // 图层下拉框需包含文档中的全部图层
    const QString previousLayer = m_layerCombo->currentText();
    m_layerCombo->clear();
    for (const Layer& layer : document->layers().all()) {
        m_layerCombo->addItem(layer.name());
    }

    const std::vector<EntityId> ids = document->selection().ids();

    if (ids.size() == 1) {
        const Entity* entity = document->entity(ids.front());
        if (!entity) {
            m_updating = false;
            return;
        }

        m_summaryLabel->setText(QStringLiteral("%1  #%2")
                                    .arg(entity->typeDisplayName())
                                    .arg(entity->id()));

        const int layerIndex = m_layerCombo->findText(entity->layerName());
        if (layerIndex >= 0) m_layerCombo->setCurrentIndex(layerIndex);

        const int colorIndex = m_colorCombo->findData(entity->colorIndex());
        m_colorCombo->setCurrentIndex(colorIndex >= 0 ? colorIndex : 0);

        const int lwIndex = m_lineweightCombo->findData(entity->lineweightDxf());
        m_lineweightCombo->setCurrentIndex(lwIndex >= 0 ? lwIndex : 0);

        m_visibleCheck->setChecked(entity->isVisible());

        m_geometryText->setPlainText(entity->geometrySummary());
    } else {
        m_summaryLabel->setText(QStringLiteral("已选择 %1 个对象").arg(ids.size()));

        // 多选时只显示公共值，字段留空表示「多种取值」
        QString commonLayer;
        int commonColor = std::numeric_limits<int>::min();
        int commonLineweight = std::numeric_limits<int>::min();
        bool allVisible = true;

        for (EntityId id : ids) {
            const Entity* entity = document->entity(id);
            if (!entity) continue;
            if (commonLayer.isEmpty()) commonLayer = entity->layerName();
            else if (commonLayer != entity->layerName()) commonLayer.clear();

            if (commonColor == std::numeric_limits<int>::min()) commonColor = entity->colorIndex();
            else if (commonColor != entity->colorIndex()) commonColor = -999;

            if (commonLineweight == std::numeric_limits<int>::min()) {
                commonLineweight = entity->lineweightDxf();
            } else if (commonLineweight != entity->lineweightDxf()) {
                commonLineweight = -999;
            }

            allVisible = allVisible && entity->isVisible();
        }

        if (!commonLayer.isEmpty()) {
            const int layerIndex = m_layerCombo->findText(commonLayer);
            if (layerIndex >= 0) m_layerCombo->setCurrentIndex(layerIndex);
        } else {
            m_layerCombo->setCurrentIndex(-1);
        }

        const int colorIndex = m_colorCombo->findData(commonColor);
        m_colorCombo->setCurrentIndex(colorIndex >= 0 ? colorIndex : -1);

        const int lwIndex = m_lineweightCombo->findData(commonLineweight);
        m_lineweightCombo->setCurrentIndex(lwIndex >= 0 ? lwIndex : -1);

        m_visibleCheck->setChecked(allVisible);

        const Box2D box = document->extentsOf(ids);
        QString summary = QStringLiteral("共 %1 个对象\n包围范围: %2\n总长度: %3")
                              .arg(ids.size())
                              .arg(box.isValid()
                                       ? QStringLiteral("%1, %2  →  %3, %4")
                                             .arg(formatLength(box.minX), formatLength(box.minY),
                                                  formatLength(box.maxX), formatLength(box.maxY))
                                       : QStringLiteral("--"));

        double totalLength = 0.0;
        int countedTypes = 0;
        for (EntityId id : ids) {
            if (const Entity* entity = document->entity(id)) {
                if (entity->type() != EntityType::Point) {
                    totalLength += entity->length();
                    ++countedTypes;
                }
            }
        }
        summary = QStringLiteral("共 %1 个对象\n可测长度对象: %2 个\n累计长度: %3\n包围范围: %4")
                      .arg(ids.size())
                      .arg(countedTypes)
                      .arg(formatLength(totalLength))
                      .arg(box.isValid()
                               ? QStringLiteral("(%1, %2) - (%3, %4)")
                                     .arg(formatLength(box.minX), formatLength(box.minY),
                                          formatLength(box.maxX), formatLength(box.maxY))
                               : QStringLiteral("--"));

        m_geometryText->setPlainText(summary);
    }

    Q_UNUSED(previousLayer);

    m_updating = false;
}

} // namespace lycad
