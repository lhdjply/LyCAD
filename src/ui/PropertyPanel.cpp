// =============================================================================
//  LyCAD - ui/PropertyPanel.cpp
// =============================================================================
#include "ui/PropertyPanel.h"

#include "core/Document.h"
#include "core/Entity.h"
#include "core/Format.h"
#include "core/TextEntity.h"
#include "ui/PropertyChoices.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPlainTextEdit>

#include <cmath>
#include <limits>
#include <QVBoxLayout>

namespace lycad {
namespace {

/// 面板上按两位小数显示，因此「取值是否一致」也按两位小数判断：
/// 数字显示得一模一样却报「多种取值」会让人莫名其妙。
long long displayKey(double value, double scale)
{
    return std::llround(value * scale);
}

} // namespace

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

    // ---- 文字属性 ------------------------------------------------------------
    // 字高与旋转是连续量，用 spin box 让用户直接键入数值；对齐是离散枚举，用下拉框。
    // 两个 spin box 都关掉 keyboardTracking：否则键入「12」会先按「1」发一次
    // valueChanged、再按「12」发一次，撤销栈里就多出一条「修改字高 1」的记录。
    m_textHeight = new QDoubleSpinBox(this);
    m_textHeight->setDecimals(2);
    m_textHeight->setRange(0.01, 100000.0);
    m_textHeight->setSingleStep(0.5);
    m_textHeight->setKeyboardTracking(false);
    m_textHeight->setToolTip(QStringLiteral("文字字高（大写字母高度，世界单位）"));
    // objectName 供界面验证工具（tools/lycad_ui_snapshot）按名取控件：
    // 面板里有多个同类控件，靠类型 findChild 拿不准是哪一个。
    m_textHeight->setObjectName(QStringLiteral("propertyPanel.textHeight"));

    m_textRotation = new QDoubleSpinBox(this);
    m_textRotation->setDecimals(2);
    m_textRotation->setRange(-3600.0, 3600.0);
    m_textRotation->setSingleStep(15.0);
    m_textRotation->setSuffix(QStringLiteral(" °"));
    m_textRotation->setKeyboardTracking(false);
    m_textRotation->setToolTip(QStringLiteral("文字旋转角，逆时针为正；超出 0~360 会自动归一"));
    m_textRotation->setObjectName(QStringLiteral("propertyPanel.textRotation"));

    m_textHAlign = new QComboBox(this);
    m_textHAlign->addItem(QStringLiteral("左"), static_cast<int>(TextEntity::HAlign::Left));
    m_textHAlign->addItem(QStringLiteral("居中"), static_cast<int>(TextEntity::HAlign::Center));
    m_textHAlign->addItem(QStringLiteral("右"), static_cast<int>(TextEntity::HAlign::Right));
    m_textHAlign->setPlaceholderText(QStringLiteral("多种取值"));
    m_textHAlign->setObjectName(QStringLiteral("propertyPanel.textHAlign"));
    m_textHAlign->setToolTip(QStringLiteral(
        "水平对齐。除「左 + 基线」外，定位点取第二对齐点（DXF 11/21），改动会让文字相对该点重新对正"));

    m_textVAlign = new QComboBox(this);
    m_textVAlign->addItem(QStringLiteral("基线"), static_cast<int>(TextEntity::VAlign::Baseline));
    m_textVAlign->addItem(QStringLiteral("底部"), static_cast<int>(TextEntity::VAlign::Bottom));
    m_textVAlign->addItem(QStringLiteral("中部"), static_cast<int>(TextEntity::VAlign::Middle));
    m_textVAlign->addItem(QStringLiteral("顶部"), static_cast<int>(TextEntity::VAlign::Top));
    m_textVAlign->setPlaceholderText(QStringLiteral("多种取值"));
    m_textVAlign->setObjectName(QStringLiteral("propertyPanel.textVAlign"));
    m_textVAlign->setToolTip(QStringLiteral("垂直对齐（DXF 73）"));

    auto* textForm = new QFormLayout();
    textForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    textForm->addRow(QStringLiteral("字高"), m_textHeight);
    textForm->addRow(QStringLiteral("旋转"), m_textRotation);
    textForm->addRow(QStringLiteral("水平对齐"), m_textHAlign);
    textForm->addRow(QStringLiteral("垂直对齐"), m_textVAlign);

    m_textGroup = new QGroupBox(QStringLiteral("文字"), this);
    m_textGroup->setLayout(textForm);
    m_textGroup->setObjectName(QStringLiteral("propertyPanel.textGroup"));
    m_textGroup->setToolTip(QStringLiteral(
        "只作用于选择集里的文字图元。多选时字段留空表示「多种取值」，仍可单独修改该字段"));

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
    layout->addWidget(m_textGroup);
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

    connect(m_textHeight, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (m_updating) return;
        emit textHeightChangeRequested(value);
    });
    connect(m_textRotation, &QDoubleSpinBox::valueChanged, this, [this](double degrees) {
        if (m_updating) return;
        // 面板上行的是度，文档里存的是弧度
        emit textRotationChangeRequested(degToRad(degrees));
    });
    connect(m_textHAlign, &QComboBox::currentIndexChanged, this, [this](int index) {
        // index < 0 = 列表被清空后的「多种取值」状态，此时不构成一次用户改动
        if (m_updating || index < 0) return;
        emit textHAlignChangeRequested(
            static_cast<TextEntity::HAlign>(m_textHAlign->itemData(index).toInt()));
    });
    connect(m_textVAlign, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_updating || index < 0) return;
        emit textVAlignChangeRequested(
            static_cast<TextEntity::VAlign>(m_textVAlign->itemData(index).toInt()));
    });

    refresh(nullptr);
}

void PropertyPanel::setMixed(QDoubleSpinBox* box)
{
    // 清空输入框：空着即「多种取值」，与下拉框清空到 -1 是同一套表达。
    // spin box 没有占位文字接口，说明写在「文字」组的提示里。
    box->clear();
}

void PropertyPanel::setEnabledState(bool enabled)
{
    m_layerCombo->setEnabled(enabled);
    m_colorCombo->setEnabled(enabled);
    m_lineweightCombo->setEnabled(enabled);
    m_visibleCheck->setEnabled(enabled);

    m_textHeight->setEnabled(enabled);
    m_textRotation->setEnabled(enabled);
    m_textHAlign->setEnabled(enabled);
    m_textVAlign->setEnabled(enabled);
}

void PropertyPanel::refresh(const Document* document)
{
    m_updating = true;

    if (!document || document->selection().isEmpty()) {
        m_summaryLabel->setText(QStringLiteral("未选择对象"));
        m_geometryText->setPlainText(QStringLiteral("在画布中选择对象以查看其属性。"));
        m_textGroup->setVisible(false);
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
                // 点没有长度；构造线/射线的长度是无穷，计入会把累计值变成 inf
                if (entity->type() != EntityType::Point && !entity->isUnbounded()) {
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

    // ---- 文字属性（只统计选择集里的文字图元）--------------------------------
    //
    // 混合选择（文字 + 直线）时只看文字：用户关心的是「这几条文字现在是多少」，
    // 而不是被非文字图元带成「多种取值」。
    std::vector<const TextEntity*> texts;
    for (EntityId id : ids) {
        if (const auto* text = dynamic_cast<const TextEntity*>(document->entity(id))) {
            texts.push_back(text);
        }
    }

    // 选择集里没有文字就整组隐藏——留着四个改不动的控件只会让人以为程序坏了
    m_textGroup->setVisible(!texts.empty());

    if (!texts.empty()) {
        const TextEntity* first = texts.front();
        bool sameHeight = true;
        bool sameRotation = true;
        bool sameHAlign = true;
        bool sameVAlign = true;
        const long long firstHeight = displayKey(first->height(), 100.0);
        const long long firstRotation = displayKey(radToDeg(first->rotation()), 100.0);

        for (const TextEntity* text : texts) {
            sameHeight = sameHeight && displayKey(text->height(), 100.0) == firstHeight;
            sameRotation = sameRotation
                           && displayKey(radToDeg(text->rotation()), 100.0) == firstRotation;
            sameHAlign = sameHAlign && text->hAlign() == first->hAlign();
            sameVAlign = sameVAlign && text->vAlign() == first->vAlign();
        }

        if (sameHeight) m_textHeight->setValue(first->height());
        else setMixed(m_textHeight);

        if (sameRotation) m_textRotation->setValue(radToDeg(first->rotation()));
        else setMixed(m_textRotation);

        // 取值不一致时清空到 -1（显示占位文字「多种取值」）；
        // 水平与垂直各自判断，一列不一致不影响另一列照常显示与修改。
        m_textHAlign->setCurrentIndex(sameHAlign
                                          ? m_textHAlign->findData(
                                                static_cast<int>(first->hAlign()))
                                          : -1);
        m_textVAlign->setCurrentIndex(sameVAlign
                                          ? m_textVAlign->findData(
                                                static_cast<int>(first->vAlign()))
                                          : -1);
    }

    Q_UNUSED(previousLayer);

    m_updating = false;
}

} // namespace lycad
