// =============================================================================
//  LyCAD - ui/Theme.h
//  界面主题：Fusion 风格 + 深浅两套调色板。
//
//  为什么走调色板而不是全局样式表
//    QPalette 是 Qt 原生机制，Fusion 风格会据此绘制菜单、停靠面板、表格、
//    滚动条、状态栏等全部标准控件；一条 QSS 规则要照顾的控件，调色板一个
//    角色就覆盖了。只有调色板管不到的地方（功能区这种自绘外观）才另外用
//    样式表，并且颜色同样取自这里，避免两处各写一套后跑偏。
//
//  一处定义，多处取用
//    「深色」在界面上意味着十几个具体色值。它们全部集中在本模块的
//    Palette 里：功能区的样式表、图标的换色、滚动容器的底色都从这里取。
//    如果分散在各处，改一个主题就等于要做一次全工程搜索。
// =============================================================================
#pragma once

#include <QColor>

namespace lycad {
namespace theme {

/// 一套完整的界面配色。
///
/// 字段按用途分组：`window` 起是标准控件（直接映射到 QPalette 角色），
/// `ribbon` 起是功能区专用，最后是下拉框等输入类控件。
/// 深浅两个主题共用同一份结构，只有取值不同。
struct Palette {
    // ---- 标准控件（映射到 QPalette 角色）----
    QColor window;              ///< 窗口、停靠面板底色
    QColor windowText;
    QColor base;                ///< 输入框、列表、表格底色
    QColor alternateBase;       ///< 表格交替行
    QColor text;
    QColor button;              ///< 按钮、表头
    QColor buttonText;
    QColor brightText;
    QColor highlight;           ///< 选中背景
    QColor highlightedText;
    QColor toolTipBase;
    QColor toolTipText;
    QColor disabledText;
    QColor disabledFill;
    QColor border;              ///< 分隔线、边框
    QColor subtleText;          ///< 次要文字（面板标题、提示）

    // ---- 功能区 ----
    QColor ribbonBackground;
    QColor ribbonBorder;
    QColor hoverBackground;
    QColor hoverBorder;
    QColor pressedBackground;
    QColor checkedBackground;
    QColor checkedBorder;

    // ---- 下拉框等输入控件 ----
    QColor fieldBackground;
    QColor fieldBorder;
    QColor fieldText;
    QColor fieldHighlight;

    bool dark = false;
};

/// 取指定主题的配色
const Palette& palette(bool dark);

/// 当前生效的是否为深色主题
bool isDark();

/// 应用主题：把风格切到 Fusion，并设置应用级调色板。
///
/// 应该在创建控件之前调用一次；之后再调用会就地换肤（Qt 会派发
/// PaletteChange 事件并重绘），但对于构造时就要按主题取色的控件
/// （如功能区），必须保证它们在创建前主题已经落定。
void apply(bool dark);

} // namespace theme
} // namespace lycad
