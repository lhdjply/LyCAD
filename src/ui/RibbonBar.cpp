#include "ui/RibbonBar.h"

#include "ui/IconLibrary.h"
#include "ui/Theme.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QFontMetrics>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace lycad {
namespace {

/// 按钮上记住的图标名。切换主题后图标要按新配色重新取，
/// 但按钮本身只持有 QIcon（一份位图），无从知道它来自哪个资源，
/// 因此创建时把名字记在动态属性上，供 refreshIcons 重新取用。
const char* const kIconNameProperty = "lycadIconName";

/// 竖线分隔。功能区里的分隔线从不贯穿整个高度——面板标题那一行必须留空，
/// 否则视觉上会和标题文字撞在一起。用一层带外边距的容器来让出这段空间。
QWidget* makeVerticalSeparator(QWidget* parent, int topMargin, int bottomMargin)
{
    auto* holder = new QWidget(parent);
    auto* layout = new QHBoxLayout(holder);
    layout->setContentsMargins(3, topMargin, 3, bottomMargin);
    layout->setSpacing(0);

    auto* line = new QFrame(holder);
    line->setObjectName(QStringLiteral("ribbonSeparator"));
    line->setFixedWidth(1);
    layout->addWidget(line);

    return holder;
}

/// 大按钮所需的最小宽度：图标宽度与文字宽度取大者，再加内边距。
/// 文字在下、图标在上的排布下，两者不会同时占用横向空间。
int largeButtonWidth(const QFont& font, const QString& text)
{
    const QFontMetrics metrics(font);
    const int textWidth = metrics.horizontalAdvance(text);
    return std::max(RibbonPanel::kLargeIcon + 18, textWidth + 12);
}

/// 小按钮所需的最小宽度：图标在左、文字在右，两者相加
int smallButtonWidth(const QFont& font, const QString& text)
{
    const QFontMetrics metrics(font);
    return RibbonPanel::kSmallIcon + 10 + metrics.horizontalAdvance(text) + 10;
}

/// 把控件字号缩小若干磅，并保证不会小到读不出来
QFont smallerFont(const QFont& base, double delta)
{
    QFont font = base;
    font.setPointSizeF(std::max(6.0, base.pointSizeF() - delta));
    return font;
}

} // namespace

// -----------------------------------------------------------------------------
//  RibbonPanel
// -----------------------------------------------------------------------------
RibbonPanel::RibbonPanel(const QString& title, QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 3, 4, 0);
    root->setSpacing(1);

    m_body = new QHBoxLayout;
    m_body->setContentsMargins(0, 0, 0, 0);
    m_body->setSpacing(2);
    root->addLayout(m_body, 1);

    auto* titleLabel = new QLabel(title, this);
    titleLabel->setObjectName(QStringLiteral("ribbonPanelTitle"));
    titleLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    titleLabel->setFont(smallerFont(font(), 1.5));
    root->addWidget(titleLabel);
}

void RibbonPanel::flushColumn()
{
    // 收尾时在底部补一段空白，让不足三行的小按钮列在面板内保持垂直居中
    if (m_column) m_column->addStretch(1);
    m_column = nullptr;
    m_columnRows = 0;
}

QVBoxLayout* RibbonPanel::startColumn()
{
    flushColumn();

    auto* column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(1);
    column->addStretch(1);
    m_body->addLayout(column);

    m_column = column;
    return column;
}

QToolButton* RibbonPanel::addLarge(const QString& iconName, const QString& text,
                                   const QString& tooltip)
{
    flushColumn();

    auto* button = new QToolButton(this);
    button->setProperty(kIconNameProperty, iconName);
    button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    button->setIcon(IconLibrary::instance().icon(iconName, kLargeIcon));
    button->setIconSize(QSize(kLargeIcon, kLargeIcon));
    button->setText(text);
    button->setToolTip(tooltip);
    button->setFont(smallerFont(font(), 0.5));
    button->setMinimumHeight(52);
    // 最小宽度必须容得下文字。功能区整体压缩时，若按钮能缩到比文字还窄，
    // QToolButton 会把标签省略成「缩…集」——按钮还在，但看不出是干什么的。
    // 让最小宽度贴合内容后，宽度不足只会让功能区变宽并出现滚动条，
    // 而不是把文字吃掉。
    button->setMinimumWidth(largeButtonWidth(button->font(), text));
    button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
    m_body->addWidget(button);

    return button;
}

QToolButton* RibbonPanel::addSmall(const QString& iconName, const QString& text,
                                   const QString& tooltip)
{
    if (!m_column || m_columnRows >= kSmallRowsPerColumn) {
        startColumn();
    }

    auto* button = new QToolButton(this);
    button->setProperty(kIconNameProperty, iconName);
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setIcon(IconLibrary::instance().icon(iconName, kSmallIcon));
    button->setIconSize(QSize(kSmallIcon, kSmallIcon));
    button->setText(text);
    button->setToolTip(tooltip);
    button->setFont(smallerFont(font(), 0.5));
    button->setMinimumHeight(21);
    button->setMinimumWidth(smallButtonWidth(button->font(), text));
    button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    m_column->addWidget(button);

    ++m_columnRows;
    return button;
}

QToolButton* RibbonPanel::addLargeCommand(const QString& iconName, const QString& text,
                                          const QString& command, const QString& tooltip)
{
    QToolButton* button = addLarge(iconName, text, tooltip);
    connect(button, &QToolButton::clicked, this, [this, command] {
        emit commandTriggered(command);
    });
    return button;
}

QToolButton* RibbonPanel::addSmallCommand(const QString& iconName, const QString& text,
                                          const QString& command, const QString& tooltip)
{
    QToolButton* button = addSmall(iconName, text, tooltip);
    connect(button, &QToolButton::clicked, this, [this, command] {
        emit commandTriggered(command);
    });
    return button;
}

void RibbonPanel::addWidget(QWidget* widget)
{
    flushColumn();

    auto* column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->addStretch(1);
    column->addWidget(widget);
    column->addStretch(1);
    m_body->addLayout(column);
}

void RibbonPanel::addSeparator()
{
    flushColumn();
    m_body->addWidget(makeVerticalSeparator(this, 2, 2));
}

void RibbonPanel::addStretch()
{
    flushColumn();
    m_body->addStretch(1);
}

// -----------------------------------------------------------------------------
//  RibbonBar
// -----------------------------------------------------------------------------
RibbonBar::RibbonBar(QWidget* parent)
    : QWidget(parent)
{
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(2, 3, 2, 0);
    m_layout->setSpacing(0);

    // 高度由内容决定，宽度跟随窗口
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    // 样式表用对象名而不是类名来选中本控件：本类位于 lycad 命名空间下，
    // 而在 Qt 样式表里命名空间内的类必须写成 "lycad--RibbonBar" 这种形式，
    // 直接写 "RibbonBar" 不会匹配，规则会被静默忽略。用对象名可以绕开这个坑。
    setObjectName(QStringLiteral("ribbonBar"));

    // 光有样式表还不够：QWidget 的派生类默认不会用样式表里的 background 作画，
    // 必须显式打开样式化背景，否则功能区会一直显示窗口默认底色。
    setAttribute(Qt::WA_StyledBackground, true);

    // 主题可能在创建本控件之前就已由 MainWindow 设定，构造时按其取值
    m_dark = theme::isDark();
    applyStyle();
}

RibbonPanel* RibbonBar::addPanel(const QString& title)
{
    if (!m_layout->isEmpty()) {
        // 底部留出面板标题的高度，分隔线不与标题齐平
        m_layout->addWidget(makeVerticalSeparator(this, 4, 19));
    }

    auto* panel = new RibbonPanel(title, this);
    connect(panel, &RibbonPanel::commandTriggered, this, &RibbonBar::commandTriggered);
    m_layout->addWidget(panel);

    return panel;
}

void RibbonBar::addStretch()
{
    m_layout->addStretch(1);
}

void RibbonBar::setDarkTheme(bool dark)
{
    if (m_dark == dark) return;

    m_dark = dark;
    IconLibrary::instance().setDarkTheme(dark);
    refreshIcons();
    applyStyle();
}

void RibbonBar::refreshIcons()
{
    const QList<QToolButton*> buttons = findChildren<QToolButton*>();
    for (QToolButton* button : buttons) {
        const QVariant name = button->property(kIconNameProperty);
        if (!name.isValid()) continue;

        const int size = (button->toolButtonStyle() == Qt::ToolButtonTextUnderIcon)
                             ? RibbonPanel::kLargeIcon
                             : RibbonPanel::kSmallIcon;
        button->setIcon(IconLibrary::instance().icon(name.toString(), size));
    }
}

void RibbonBar::applyStyle()
{
    // 样式表挂在 RibbonBar 上而不是应用级：功能区是唯一需要这种「面板 + 平铺
    // 按钮」外观的地方，把它限制在这棵子树内，菜单、停靠面板、状态栏都不会
    // 被意外波及（它们由 Theme 的调色板统一管理）。
    //
    // 这里用 "#ribbonBar" 而不是类名选择器：本类位于 lycad 命名空间下，
    // 在 Qt 样式表里命名空间内的类必须写成 "lycad--RibbonBar"，直接写
    // "RibbonBar" 不会匹配，规则会被静默忽略。
    const theme::Palette& c = theme::palette(m_dark);

    // 颜色有十几个，用具名占位符而不是 QString::arg 的 %1/%2…：
    // %1 与 %15 在同一个串里并存时阅读和生成都容易出错，具名替换没有歧义。
    const struct {
        const char* key;
        QColor value;
    } tokens[] = {
        { "@background@",     c.ribbonBackground },
        { "@border@",         c.ribbonBorder },
        { "@text@",           c.buttonText },
        { "@hoverBg@",        c.hoverBackground },
        { "@hoverBorder@",    c.hoverBorder },
        { "@pressedBg@",      c.pressedBackground },
        { "@checkedBg@",      c.checkedBackground },
        { "@checkedBorder@",  c.checkedBorder },
        { "@disabledText@",   c.disabledText },
        { "@titleText@",      c.subtleText },
        { "@separator@",      c.border },
        { "@fieldBg@",        c.fieldBackground },
        { "@fieldBorder@",    c.fieldBorder },
        { "@fieldText@",      c.fieldText },
        { "@selectBg@",       c.fieldHighlight },
    };

    QString qss = QStringLiteral(R"(
#ribbonBar {
    background: @background@;
    border-bottom: 1px solid @border@;
}
#ribbonBar QToolButton {
    background: transparent;
    color: @text@;
    border: 1px solid transparent;
    border-radius: 3px;
    padding: 1px 2px;
}
#ribbonBar QToolButton:hover {
    background: @hoverBg@;
    border-color: @hoverBorder@;
}
#ribbonBar QToolButton:pressed {
    background: @pressedBg@;
    border-color: @hoverBorder@;
}
#ribbonBar QToolButton:checked {
    background: @checkedBg@;
    border-color: @checkedBorder@;
}
#ribbonBar QToolButton:disabled {
    color: @disabledText@;
}
#ribbonBar QLabel#ribbonPanelTitle {
    color: @titleText@;
}
#ribbonBar QFrame#ribbonSeparator {
    background: @separator@;
}
#ribbonBar QComboBox {
    background: @fieldBg@;
    color: @fieldText@;
    border: 1px solid @fieldBorder@;
    border-radius: 3px;
    padding: 1px 4px;
    min-height: 18px;
}
#ribbonBar QComboBox::drop-down {
    border: none;
    width: 14px;
}
#ribbonBar QComboBox QAbstractItemView {
    background: @fieldBg@;
    color: @fieldText@;
    border: 1px solid @fieldBorder@;
    selection-background-color: @selectBg@;
    selection-color: @fieldText@;
}
)");

    for (const auto& token : tokens) {
        qss.replace(QLatin1String(token.key), token.value.name());
    }

    setStyleSheet(qss);
}

} // namespace lycad
