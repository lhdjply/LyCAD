// =============================================================================
//  LyCAD - ui/TextEditDialog.h
//  文字内容编辑对话框（单行 / 多行两种形态）。
//
//  为什么不直接用 QInputDialog::getText
//    它是单行输入框：多行文字的内容里带段落分隔，打开后只看得见第一行，中间的
//    行凭空消失；而且回车就直接提交，用户没有任何办法补一个换行。
//
//  两种形态是**不同的控件**，不只是行为开关：
//    · SingleLine（单行文字 TEXT）—— QLineEdit，回车确认，**没有换行入口**。
//      单行文字的内容里出现换行会改变版面语义（画布按多行绘制、导出到 R2000
//      会被写成 MTEXT），不该给用户一个"看起来能换行、写进去却换了对象类型"的入口。
//    · MultiLine（多行文字 MTEXT）—— QPlainTextEdit，回车确认、**Alt+Enter 换行**
//      （AutoCAD 多行文字编辑器的一贯分工：回车是"输完了"，换行让给修饰键）。
//
//  两种文字双击时各走一种，双击后的界面因此能直接告诉用户"我点的是哪种文字"；
//  新建文字时也按工具选中的类型走对应的形态。
//
//  编辑框里除内容外还带**字体与字高**两个样式项（同属"这一次编辑该顺手改完"的东西，
//  不必再绕回特性面板）。表格单元格也走同一个框：那张表的字体与字高都是**整表属性**，
//  所以框里加一行小字说明它们的作用范围（见 setStyleNote）。
// =============================================================================
#pragma once

#include <QDialog>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;

namespace lycad {

class TextEditDialog : public QDialog {
    Q_OBJECT

public:
    /// 编辑框形态（见文件头说明）
    enum class Mode {
        SingleLine,     ///< 单行输入框：回车确认，不支持换行
        MultiLine       ///< 多行编辑框：回车确认，Alt+Enter 换行
    };

    /// content 里的 '\n' 会原样显示为换行（不转换成任何转义形式）。
    /// fontFamily 是图元当前的字体（空串 = 未指定/默认），用于字体下拉的初始选中。
    /// textHeight 是图元当前字高（世界单位），用于字高数值框的初值；<= 0 时按默认值。
    /// 单行模式下内容里若带换行，只取第一行显示（不会把换行带回来）。
    explicit TextEditDialog(Mode mode,
                            const QString& content,
                            const QString& fontFamily,
                            double textHeight,
                            QWidget* parent = nullptr);

    /// 编辑后的内容，换行是真换行（'\n'），与 TextEntity::content() 同一口径。
    QString content() const;
    /// 内容占用的行数（用于界面提示与测试断言）
    int lineCount() const;

    /// 用户选定的字体族名；空串表示「（默认）」，即不指定字体。
    QString fontFamily() const;

    /// 用户填的字高（世界单位，恒 > 0）。
    ///
    /// 「双击文字顺手改字号」不必再绕回特性面板：字高是文字自己的属性，编辑框里
    /// 与内容一起改，正好是一次编辑该做完的事。
    double textHeight() const;

    /// 在样式块（字体 / 字高）下方加一行小字说明。
    ///
    /// 表格用它把话说清楚：格子里的字体与字高都**属于整张表**，改一处等于改全表——
    /// 不说清，用户会以为只改了当前这一格。用可见的一行字而不是 tooltip：鼠标不悬停
    /// 就该看到这条约定。
    void setStyleNote(const QString& note);

private:
    Mode m_mode = Mode::MultiLine;
    QPlainTextEdit* m_multiEdit = nullptr;
    QLineEdit*      m_singleEdit = nullptr;
    QComboBox*      m_font = nullptr;
    QDoubleSpinBox* m_height = nullptr;
    QLabel*         m_note = nullptr;       ///< 样式块下方的小字说明（默认不显示）
};

} // namespace lycad
