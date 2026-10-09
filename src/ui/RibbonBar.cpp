#include "ui/RibbonBar.h"

#include "ui/IconLibrary.h"
#include "ui/Theme.h"

#include <QAction>
#include <QActionGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QFontMetrics>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPolygonF>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace lycad {
namespace {

/// 按钮上记住的图标名。切换主题后图标要按新配色重新取，
/// 但按钮本身只持有 QIcon（一份位图），无从知道它来自哪个资源，
/// 因此创建时把名字记在动态属性上，供 refreshIcons 重新取用。
const char* const kIconNameProperty = "lycadIconName";

/// 下拉按钮上「当前主命令」的名字。主体点击执行的就是它——在下拉里选过别的项后
/// 按钮会切换成那一项（见 RibbonPanel::selectDropDownItem），命令随之改变，
/// 因此不能像普通命令按钮那样把命令写死在连接里。
const char* const kCommandProperty = "lycadCommand";

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

/// 功能区按钮。
///
/// 只有「带下拉菜单」的按钮会走到自定义绘制：MenuButtonPopup 会给箭头单独划出
/// 一小块区域，而 Qt 在那里的默认画法与功能区的扁平风格不搭——箭头颜色不随主题
/// 走（深浅两套主题下都是同一种灰）、尺寸也偏大。这里保留 menu 区的占位（文字
/// 布局与「点箭头展开菜单」的热区都不变），仅去掉箭头本身，改画一个与按钮文字
/// 同色的小三角。
///
/// 普通命令按钮没有菜单，paintEvent 与基类行为完全一致。
class RibbonButton final : public QToolButton {
public:
    explicit RibbonButton(QWidget* parent = nullptr)
        : QToolButton(parent) {}

    /// 箭头颜色。由 RibbonBar 在套用主题时统一灌入（与 QSS 的前景色同源），
    /// 免得按钮自己去猜当前是深色还是浅色主题。
    void setArrowColor(const QColor& color)
    {
        if (m_arrowColor == color) return;
        m_arrowColor = color;
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event);
        const bool dropDown = menu() != nullptr
                              && popupMode() == QToolButton::MenuButtonPopup;

        QStylePainter painter(this);
        QStyleOptionToolButton option;
        initStyleOption(&option);
        if (dropDown) {
            // 只去掉箭头本身：HasMenu 保留，menu 区的占位因此不变——文字不会被
            // 挤到箭头底下，展开菜单的点击热区也还在原处。
            option.features &= ~QStyleOptionToolButton::Arrow;
        }
        painter.drawComplexControl(QStyle::CC_ToolButton, option);

        if (dropDown) drawArrow(painter);
    }

private:
    void drawArrow(QPainter& painter)
    {
        const bool large = toolButtonStyle() == Qt::ToolButtonTextUnderIcon;
        // 尺寸取小：功能区里的图标都是细笔画的线性图，箭头一大就显得笨重
        const double w = large ? 8.0 : 6.0;
        const double h = large ? 4.0 : 3.0;
        const double marginX = 5.0;
        const double marginY = large ? 5.0 : 0.0;

        const double left = width() - marginX - w;
        const double top = large ? height() - marginY - h
                                 : (height() - h) * 0.5;

        QPolygonF triangle;
        triangle << QPointF(left, top)
                 << QPointF(left + w, top)
                 << QPointF(left + w * 0.5, top + h);

        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_arrowColor);
        painter.drawPolygon(triangle);
    }

    QColor m_arrowColor;
};

} // namespace

// -----------------------------------------------------------------------------
//  RibbonPanel
// -----------------------------------------------------------------------------
RibbonPanel::RibbonPanel(const QString& title, QWidget* parent)
    : QWidget(parent)
{
    // 面板自带底色与描边（见 RibbonBar::applyStyle 里的 #ribbonPanel 规则），
    // 好让「文件 / 绘图 / 修改」在功能区上明确地分成几块——只靠面板之间一条
    // 1px 竖线时，按钮一多就看不出分组了。
    // QWidget 的派生类默认不拿样式表里的 background 作画，必须显式打开。
    setObjectName(QStringLiteral("ribbonPanel"));
    setAttribute(Qt::WA_StyledBackground, true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 4, 6, 1);
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
    // 结束当前小按钮列。列本身不需要在底部补空白来「托住」按钮——
    // 靠顶由列的对齐方式决定（见 startColumn），这里只负责收尾。
    m_column = nullptr;
    m_columnRows = 0;
}

QVBoxLayout* RibbonPanel::startColumn()
{
    flushColumn();

    auto* column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(1);
    m_body->addLayout(column);

    // 这一列在面板内**靠顶部**摆放：列只占自身内容的高度，多出来的高度交给
    // 会伸缩的大按钮。曾经靠「列内上下各加一段弹性空白」来摆位置，效果是垂直
    // 居中——面板里大按钮的个数不同、分给各列的高度就不同，于是同一条工具栏上
    // 「撤销/重做」「矩形/点」「填充」各停在一个高度上，参差不齐。
    if (QLayoutItem* item = m_body->itemAt(m_body->count() - 1)) {
        item->setAlignment(Qt::AlignTop);
    }

    m_column = column;
    return column;
}

QToolButton* RibbonPanel::addLarge(const QString& iconName, const QString& text,
                                   const QString& tooltip)
{
    flushColumn();

    auto* button = new RibbonButton(this);
    button->setProperty(kIconNameProperty, iconName);
    button->setArrowColor(theme::palette(theme::isDark()).buttonText);
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

    auto* button = new RibbonButton(this);
    button->setProperty(kIconNameProperty, iconName);
    button->setArrowColor(theme::palette(theme::isDark()).buttonText);
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

QToolButton* RibbonPanel::addLargeDropDown(const QString& iconName, const QString& text,
                                           const QString& tooltip, const QString& primaryCommand)
{
    QToolButton* button = addLarge(iconName, text, tooltip);

    // MenuButtonPopup 正是「主体 + 箭头」的分工：点按钮主体不弹菜单，只有箭头
    // 那一小块才展开。默认的 InstantPopup 会把整个按钮变成弹出键——单击即用的
    // 手感就没了，而这类合并按钮的第一诉求恰恰是「单击还是主命令」。
    //
    // 主命令存在按钮的动态属性上、不在此处捕获：在下拉里选过别的项后按钮会切换
    // 成那一项（见 selectDropDownItem），捕获下来的那个值随即过期。
    button->setProperty(kCommandProperty, primaryCommand);
    connect(button, &QToolButton::clicked, this, [this, button] {
        emit commandTriggered(button->property(kCommandProperty).toString());
    });

    auto* menu = new QMenu(button);
    button->setMenu(menu);
    button->setPopupMode(QToolButton::MenuButtonPopup);

    // 菜单项互斥勾选：当前生效的是哪一项，展开菜单就能看出来
    auto* group = new QActionGroup(button);
    group->setExclusive(true);

    // 右侧还要留出下拉箭头的宽度：大按钮的最小宽度只按图标与文字算（见
    // largeButtonWidth），没把箭头算进去，不留这一截箭头会压在文字上。
    constexpr int kArrowWidth = 18;
    button->setMinimumWidth(button->minimumWidth() + kArrowWidth);

    return button;
}

QToolButton* RibbonPanel::addSmallDropDown(const QString& iconName, const QString& text,
                                           const QString& tooltip, const QString& primaryCommand)
{
    QToolButton* button = addSmall(iconName, text, tooltip);

    // 分工与 addLargeDropDown 完全一致：主体点击执行当前主命令，只有箭头展开菜单
    button->setProperty(kCommandProperty, primaryCommand);
    connect(button, &QToolButton::clicked, this, [this, button] {
        emit commandTriggered(button->property(kCommandProperty).toString());
    });

    auto* menu = new QMenu(button);
    button->setMenu(menu);
    button->setPopupMode(QToolButton::MenuButtonPopup);

    auto* group = new QActionGroup(button);
    group->setExclusive(true);

    // 小按钮同样要留出箭头宽度（smallButtonWidth 也只算了图标与文字）
    constexpr int kArrowWidth = 14;
    button->setMinimumWidth(button->minimumWidth() + kArrowWidth);

    return button;
}

void RibbonPanel::addDropDownItem(QToolButton* button, const QString& iconName,
                                  const QString& text, const QString& command)
{
    if (!button) return;
    QMenu* menu = button->menu();
    if (!menu) return;

    QAction* action = menu->addAction(text);
    // 与按钮同一套做法：图标名记在动态属性上，供 refreshIcons 在换主题后重取
    action->setProperty(kIconNameProperty, iconName);
    const bool large = button->toolButtonStyle() == Qt::ToolButtonTextUnderIcon;
    action->setIcon(IconLibrary::instance().icon(iconName, large ? kLargeIcon : kSmallIcon));

    if (auto* group = button->findChild<QActionGroup*>()) {
        action->setCheckable(true);
        group->addAction(action);
        // 当前主命令对应的那一项默认打勾：展开菜单就能看出按钮此刻代表哪一项
        if (command == button->property(kCommandProperty).toString()) {
            action->setChecked(true);
        }
    }

    connect(action, &QAction::triggered, this, [this, button, iconName, text, command] {
        emit commandTriggered(command);
        // 选过哪一项，按钮就切换成哪一项，下次单击主体直接用它
        selectDropDownItem(button, iconName, text, command);
    });
}

void RibbonPanel::selectDropDownItem(QToolButton* button, const QString& iconName,
                                     const QString& text, const QString& command)
{
    button->setProperty(kCommandProperty, command);
    button->setProperty(kIconNameProperty, iconName);
    button->setText(text);

    const bool large = button->toolButtonStyle() == Qt::ToolButtonTextUnderIcon;
    button->setIcon(IconLibrary::instance().icon(iconName, large ? kLargeIcon : kSmallIcon));

    // 文字换了，最小宽度要按新文字重算（原来那截是按旧文字留的），再补上箭头宽度。
    // 不重算的话按钮装不下新文字，标签会被省略成「椭…圆」这种读不出来的样子。
    const int content = large ? largeButtonWidth(button->font(), text)
                              : smallButtonWidth(button->font(), text);
    button->setMinimumWidth(content + (large ? 18 : 14));
}

void RibbonPanel::addWidget(QWidget* widget)
{
    flushColumn();

    // 与大按钮、小按钮列同一条顶线：靠顶部对齐，多出来的高度落在下方。
    // 上下各留一段空白做垂直居中会把下拉框压在面板中间，与相邻面板的按钮
    // 不在一条顶线上。
    auto* column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->addWidget(widget);
    m_body->addLayout(column);
    if (QLayoutItem* item = m_body->itemAt(m_body->count() - 1)) {
        item->setAlignment(Qt::AlignTop);
    }
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
    // 面板自带边框，四周与彼此之间都要留出间距，否则描边会贴着窗口边缘或相邻面板
    m_layout->setContentsMargins(4, 4, 4, 4);
    m_layout->setSpacing(4);

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
    // 面板之间不再画竖线：每个面板自带底色与描边，「哪里是一块」由面板本身
    // 表达（见 RibbonPanel 的构造函数与 applyStyle）。只留一点间距，避免两块贴在一起。
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

    // 下拉菜单项（QAction）也记了图标名：不一起换的话，深色主题下按钮图标已经
    // 换成深色配色，展开的菜单里仍是原来的浅色底图，一浅一深并排出现。
    const QList<QAction*> actions = findChildren<QAction*>();
    for (QAction* action : actions) {
        const QVariant name = action->property(kIconNameProperty);
        if (!name.isValid()) continue;
        action->setIcon(IconLibrary::instance().icon(name.toString(), RibbonPanel::kSmallIcon));
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
        { "@panelBg@",        c.ribbonPanelBackground },
        { "@panelBorder@",    c.ribbonPanelBorder },
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
/* 面板自成一块：底色与描边把「文件 / 绘图 / 修改…」分开，
   面板之间的分隔线因此不再需要（见 RibbonBar::addPanel）。 */
#ribbonBar QWidget#ribbonPanel {
    background: @panelBg@;
    border: 1px solid @panelBorder@;
    border-radius: 4px;
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
/* 下拉按钮的箭头区：MenuButtonPopup 会把这一小块单独绘制，而未指定子控件样式时
   Qt 走 QStyle 默认画法——在扁平的功能区上表现为一条深色实心竖条，与周围格格
   不入（深浅两套主题下都是一条黑条）。这里让它与按钮同底色、无边框。
   宽度要保留：这是「点箭头展开菜单」的热区，归零后就点不到菜单了。 */
#ribbonBar QToolButton::menu-button {
    background: transparent;
    border: none;
    width: 13px;
}
/* 箭头本身由 RibbonButton 自绘（颜色随主题走）。但样式表一旦接管了 menu-button，
   Qt 会顺带用样式表的默认箭头再画一个——不关掉的话按钮上会出现两个箭头。 */
#ribbonBar QToolButton::menu-arrow {
    image: none;
}
#ribbonBar QLabel#ribbonPanelTitle {
    color: @titleText@;
}
/* 下拉框上方的小字说明（如「颜色」「线宽」）：与面板标题同一档颜色，
   只作说明用，不与控件里的正文抢视线 */
#ribbonBar QLabel#ribbonFieldCaption {
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

    // 下拉箭头由 RibbonButton 自绘：颜色取与 QSS 前景色同源的 buttonText，
    // 因此换主题时箭头跟着一起变，不必两处各维护一遍配色。
    for (QToolButton* button : findChildren<QToolButton*>()) {
        if (auto* ribbonButton = dynamic_cast<RibbonButton*>(button)) {
            ribbonButton->setArrowColor(c.buttonText);
        }
    }
}

} // namespace lycad
