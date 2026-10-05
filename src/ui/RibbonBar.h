// =============================================================================
//  LyCAD - ui/RibbonBar.h
//  功能区（Ribbon）：仿 AutoCAD 的面板式工具栏。
//
//  与普通 QToolBar 的区别在于「面板」这一层：一组相关命令被包在一个带
//  底部标题的方框里，面板之间用竖线隔开。这样按钮数量上百时仍然能靠
//  分组找到东西，而不是退化成一长条需要横向查找的按钮带。
//
//  两种按钮形态同时存在，分工是明确的：
//    · 大按钮（图标在上、文字在下）——面板里的主要命令，按视觉优先级挑；
//    · 小按钮（图标在左、文字在右）——次要命令，每列三个紧凑排布。
//  面板还可以直接嵌入任意控件（下拉框、指示器），因为「选当前图层」
//  这类操作本质上是一个下拉框，而不是一个按钮。
// =============================================================================
#pragma once

#include <QString>
#include <QWidget>

class QFrame;
class QHBoxLayout;
class QToolButton;
class QVBoxLayout;

namespace lycad {

/**
 * 功能区面板：底部标题 + 内容区。
 *
 * 内容的排布完全由 add* 的调用顺序决定——调用方按视觉顺序写下来即可，
 * 不需要关心「什么时候该换一列」这类布局细节，那由面板自己处理。
 */
class RibbonPanel : public QWidget {
    Q_OBJECT

public:
    explicit RibbonPanel(const QString& title, QWidget* parent = nullptr);

    /// 大按钮；返回按钮以便调用方接自己的槽
    QToolButton* addLarge(const QString& iconName, const QString& text, const QString& tooltip);
    /// 小按钮；每列最多 kSmallRowsPerColumn 个，超出自动另起一列
    QToolButton* addSmall(const QString& iconName, const QString& text, const QString& tooltip);

    /// 大 / 小按钮的便捷形式：点击后发出 commandTriggered
    QToolButton* addLargeCommand(const QString& iconName, const QString& text,
                                 const QString& command, const QString& tooltip);
    QToolButton* addSmallCommand(const QString& iconName, const QString& text,
                                 const QString& command, const QString& tooltip);

    /// 嵌入任意控件（下拉框等），与按钮一样靠面板顶部对齐
    void addWidget(QWidget* widget);

    /// 插入一条竖线，把面板内容分成两组
    void addSeparator();

    /// 追加弹性空白，把后面的内容推到右侧
    void addStretch();

    // 图标尺寸与每列行数：这些数值决定了面板的高度，集中放在这里，
    // 免得散落在创建按钮的代码里对不上。
    static constexpr int kLargeIcon = 24;
    static constexpr int kSmallIcon = 16;
    static constexpr int kSmallRowsPerColumn = 3;

signals:
    /// 某个命令型按钮被点击，参数是命令名
    void commandTriggered(const QString& command);

private:
    /// 结束当前小按钮列（若存在），使下一个元素另起一组
    void flushColumn();
    /// 结束当前列并新开一列小按钮
    QVBoxLayout* startColumn();

    QHBoxLayout* m_body = nullptr;
    QVBoxLayout* m_column = nullptr;
    int m_columnRows = 0;
};

/**
 * 功能区本体：一排面板，面板之间自动插入竖线分隔。
 */
class RibbonBar : public QWidget {
    Q_OBJECT

public:
    explicit RibbonBar(QWidget* parent = nullptr);

    /// 追加一个面板；面板内部仍可继续 addSeparator 细分
    RibbonPanel* addPanel(const QString& title);

    /// 在末尾追加弹性空白
    void addStretch();

    /// 深色外观。图标配色由 IconLibrary 同步切换。
    void setDarkTheme(bool dark);
    bool isDarkTheme() const noexcept { return m_dark; }

signals:
    /// 汇总所有面板的命令点击，便于上层只连一次
    void commandTriggered(const QString& command);

private:
    /// 换主题后按新配色重新取用全部按钮的图标
    void refreshIcons();
    void applyStyle();

    QHBoxLayout* m_layout = nullptr;
    bool m_dark = false;
};

} // namespace lycad
