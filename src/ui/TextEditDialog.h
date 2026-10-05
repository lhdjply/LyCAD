// =============================================================================
//  LyCAD - ui/TextEditDialog.h
//  文字内容编辑对话框。
//
//  为什么不直接用 QInputDialog::getText
//    它是单行输入框：MTEXT 降级导入的多行文字（内容里带 '\n'）打开后只看得见
//    第一行，中间的行凭空消失；而且回车就直接提交，用户没有任何办法补一个换行。
//    这里换成多行编辑框，并把回车按 CAD 的习惯分工：
//      · Enter / Return（无修饰键）→ 确认
//      · **Alt+Enter → 在光标处换行**
//      · Esc → 取消
//    「回车确认」是 CAD 用户的肌肉记忆，不能因为要支持多行就把它让给换行；
//    换行让给带修饰键的组合，这也是 AutoCAD 多行文字编辑器的一贯做法。
// =============================================================================
#pragma once

#include <QDialog>

class QPlainTextEdit;

namespace lycad {

class TextEditDialog : public QDialog {
    Q_OBJECT

public:
    /// content 里的 '\n' 会原样显示为换行（不转换成任何转义形式）。
    explicit TextEditDialog(const QString& content, QWidget* parent = nullptr);

    /// 编辑后的内容，换行是真换行（'\n'），与 TextEntity::content() 同一口径。
    QString content() const;
    /// 内容占用的行数（用于界面提示与测试断言）
    int lineCount() const;

private:
    QPlainTextEdit* m_edit = nullptr;
};

} // namespace lycad
