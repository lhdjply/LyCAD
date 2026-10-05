#include "ui/Theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QPainter>
#include <QPalette>
#include <QProxyStyle>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleOption>

namespace lycad {
namespace theme {
namespace {

const Palette& lightPalette()
{
    static const Palette kLight = {
        /* window            */ QColor(0xf4, 0xf5, 0xf7),
        /* windowText        */ QColor(0x26, 0x2b, 0x33),
        /* base              */ QColor(0xff, 0xff, 0xff),
        /* alternateBase     */ QColor(0xf7, 0xf8, 0xfa),
        /* text              */ QColor(0x26, 0x2b, 0x33),
        /* button            */ QColor(0xf4, 0xf5, 0xf7),
        /* buttonText        */ QColor(0x26, 0x2b, 0x33),
        /* brightText        */ QColor(0xff, 0xff, 0xff),
        /* highlight         */ QColor(0x3b, 0x78, 0xc3),
        /* highlightedText   */ QColor(0xff, 0xff, 0xff),
        /* toolTipBase       */ QColor(0xfb, 0xfb, 0xf3),
        /* toolTipText       */ QColor(0x26, 0x2b, 0x33),
        /* disabledText      */ QColor(0xa0, 0xa6, 0xae),
        /* disabledFill      */ QColor(0xe4, 0xe6, 0xea),
        /* border            */ QColor(0xc4, 0xc9, 0xd0),
        /* subtleText        */ QColor(0x6b, 0x72, 0x80),

        /* ribbonBackground  */ QColor(0xf4, 0xf5, 0xf7),
        /* ribbonBorder      */ QColor(0xc4, 0xc9, 0xd0),
        // 面板比功能区底色更亮：浅色主题里「亮」= 抬起，与输入框用白色表示凹陷
        // 是相反的一路；描边比底色深一档，保证分块边界清晰
        /* ribbonPanelBackground */ QColor(0xff, 0xff, 0xff),
        /* ribbonPanelBorder     */ QColor(0xd2, 0xd7, 0xdd),
        /* hoverBackground   */ QColor(0xe2, 0xec, 0xf8),
        /* hoverBorder       */ QColor(0xa8, 0xc6, 0xe6),
        /* pressedBackground */ QColor(0xca, 0xdf, 0xf5),
        /* checkedBackground */ QColor(0xcf, 0xe2, 0xf6),
        /* checkedBorder     */ QColor(0x7f, 0xa9, 0xd8),

        /* fieldBackground   */ QColor(0xff, 0xff, 0xff),
        /* fieldBorder       */ QColor(0xb9, 0xbf, 0xc7),
        /* fieldText         */ QColor(0x26, 0x2b, 0x33),
        /* fieldHighlight    */ QColor(0xcf, 0xe2, 0xf6),

        /* dark              */ false,
    };
    return kLight;
}

const Palette& darkPalette()
{
    static const Palette kDark = {
        // 面板底色与功能区一致，两者相接处才不会出现断层
        /* window            */ QColor(0x2b, 0x30, 0x38),
        /* windowText        */ QColor(0xd5, 0xdb, 0xe3),
        // 输入框 / 表格比窗口再暗一档：深色主题下靠「更暗」来表达凹陷，
        // 与浅色主题用白色表达凹陷是同一个思路
        /* base              */ QColor(0x23, 0x28, 0x30),
        /* alternateBase     */ QColor(0x28, 0x2e, 0x37),
        /* text              */ QColor(0xd5, 0xdb, 0xe3),
        /* button            */ QColor(0x33, 0x3a, 0x45),
        /* buttonText        */ QColor(0xd5, 0xdb, 0xe3),
        /* brightText        */ QColor(0xff, 0xff, 0xff),
        /* highlight         */ QColor(0x3f, 0x6f, 0xa8),
        /* highlightedText   */ QColor(0xff, 0xff, 0xff),
        /* toolTipBase       */ QColor(0x23, 0x28, 0x30),
        /* toolTipText       */ QColor(0xd5, 0xdb, 0xe3),
        /* disabledText      */ QColor(0x6d, 0x76, 0x84),
        /* disabledFill      */ QColor(0x3a, 0x41, 0x4c),
        /* border            */ QColor(0x3c, 0x43, 0x4e),
        /* subtleText        */ QColor(0x8b, 0x94, 0xa3),

        /* ribbonBackground  */ QColor(0x2b, 0x30, 0x38),
        /* ribbonBorder      */ QColor(0x19, 0x1d, 0x23),
        // 深色下面板反过来比功能区更暗：按钮的悬停/按下配色是按功能区底色调的，
        // 面板若做成更亮的一档，悬停色（0x39…) 会贴得几乎看不出来
        /* ribbonPanelBackground */ QColor(0x26, 0x2b, 0x33),
        /* ribbonPanelBorder     */ QColor(0x45, 0x4d, 0x59),
        /* hoverBackground   */ QColor(0x39, 0x41, 0x4c),
        /* hoverBorder       */ QColor(0x4d, 0x56, 0x63),
        /* pressedBackground */ QColor(0x22, 0x29, 0x32),
        /* checkedBackground */ QColor(0x3f, 0x5a, 0x7d),
        /* checkedBorder     */ QColor(0x6a, 0x9b, 0xd4),

        /* fieldBackground   */ QColor(0x35, 0x3c, 0x46),
        /* fieldBorder       */ QColor(0x4d, 0x56, 0x63),
        /* fieldText         */ QColor(0xd5, 0xdb, 0xe3),
        /* fieldHighlight    */ QColor(0x3f, 0x5a, 0x7d),

        /* dark              */ true,
    };
    return kDark;
}

bool g_dark = false;

/**
 * 只用来补 Fusion 在深色下沉掉的一处：主窗口里拖动停靠面板的分隔条。
 *
 * 刻意只碰这一处。滚动条也一度纳入过这里，但 Fusion 原生的滚动条在深色下
 * 本来就清楚（滑块比底色亮三十多级），而且带两端箭头；自绘反而把箭头丢了、
 * 样式也不如原生。凡是调色板已经能表达的东西，就不要用代理风格重画。
 * Fusion 以 Window 色绘制它，深色主题下与背景几乎同色，用户看不出哪里能拖。
 *
 * 这里刻意用代理风格而不是应用级样式表。样式表一旦设在 QApplication 上，
 * 控件会改用 QStyleSheetStyle，新建控件便不再继承应用调色板——表现就是
 * 深色主题「只生效一半」：窗口底色变了，下拉框、按钮却还是浅色。
 * 调色板 + 代理风格不破坏调色板的继承链。
 */
class ThemeStyle final : public QProxyStyle {
public:
    explicit ThemeStyle(QStyle* base) : QProxyStyle(base) {}

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option,
                       QPainter* painter, const QWidget* widget) const override
    {
        if (element == PE_IndicatorDockWidgetResizeHandle) {
            const Palette& c = palette(isDark());
            // 平时是一条淡分隔线，指针悬停时换成高亮色，提示可拖动
            painter->save();
            painter->fillRect(option->rect,
                              (option->state & State_MouseOver) ? c.highlight : c.border);
            painter->restore();
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

};

} // namespace

const Palette& palette(bool dark)
{
    return dark ? darkPalette() : lightPalette();
}

bool isDark()
{
    return g_dark;
}

void apply(bool dark)
{
    auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!application) return;      // 无界面上下文（例如核心单元测试）时静默跳过

    // Fusion 是唯一在三大平台上表现一致、且完全遵循调色板的内置风格。
    // 原生风格（Windows / macOS）大量色值写死在系统里，深色主题会只生效一半。
    // 再套一层代理风格补齐 Fusion 在深色下漏掉的分隔条绘制。
    // 用 dynamic_cast 而不是 qobject_cast：QProxyStyle 的子类不必带 Q_OBJECT 宏
    if (dynamic_cast<ThemeStyle*>(application->style()) == nullptr) {
        application->setStyle(
            new ThemeStyle(QStyleFactory::create(QStringLiteral("Fusion"))));
    }

    const Palette& c = palette(dark);

    QPalette p;
    p.setColor(QPalette::Window, c.window);
    p.setColor(QPalette::WindowText, c.windowText);
    p.setColor(QPalette::Base, c.base);
    p.setColor(QPalette::AlternateBase, c.alternateBase);
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::Button, c.button);
    p.setColor(QPalette::ButtonText, c.buttonText);
    p.setColor(QPalette::BrightText, c.brightText);
    p.setColor(QPalette::Highlight, c.highlight);
    p.setColor(QPalette::HighlightedText, c.highlightedText);
    p.setColor(QPalette::ToolTipBase, c.toolTipBase);
    p.setColor(QPalette::ToolTipText, c.toolTipText);
    p.setColor(QPalette::Link, c.highlight);
    p.setColor(QPalette::LinkVisited, c.highlight);
#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
    p.setColor(QPalette::PlaceholderText, c.subtleText);
#endif

    // 禁用态必须显式给出。否则 Fusion 会按自己的规则从常态色推导，
    // 在深色底上推出的往往仍是深色，禁用按钮看起来和可用按钮没区别。
    p.setColor(QPalette::Disabled, QPalette::Text, c.disabledText);
    p.setColor(QPalette::Disabled, QPalette::WindowText, c.disabledText);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c.disabledText);
    p.setColor(QPalette::Disabled, QPalette::Highlight, c.disabledFill);
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, c.disabledText);
    p.setColor(QPalette::Disabled, QPalette::Base, c.window);
    p.setColor(QPalette::Disabled, QPalette::Button, c.window);

    // 调色板之外只补一处：主窗口里用于拖动停靠面板的分隔条。
    // Fusion 默认用 Window 色绘制，在深色底上完全看不见，拖动范围无从判断。
    // [实验] 暂时不设应用级样式表
    application->setStyleSheet(QString());

    application->setPalette(p);

    g_dark = dark;
}

} // namespace theme
} // namespace lycad
