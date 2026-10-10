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
// =============================================================================
#pragma once

#include <QDialog>

class QComboBox;
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
    /// 单行模式下内容里若带换行，只取第一行显示（不会把换行带回来）。
    explicit TextEditDialog(Mode mode,
                            const QString& content,
                            const QString& fontFamily,
                            QWidget* parent = nullptr);

    /// 编辑后的内容，换行是真换行（'\n'），与 TextEntity::content() 同一口径。
    QString content() const;
    /// 内容占用的行数（用于界面提示与测试断言）
    int lineCount() const;

    /// 用户选定的字体族名；空串表示「（默认）」，即不指定字体。
    QString fontFamily() const;

private:
    Mode m_mode = Mode::MultiLine;
    QPlainTextEdit* m_multiEdit = nullptr;
    QLineEdit*      m_singleEdit = nullptr;
    QComboBox*      m_font = nullptr;
};

} // namespace lycad
