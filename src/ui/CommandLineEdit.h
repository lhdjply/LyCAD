// =============================================================================
//  LyCAD - ui/CommandLineEdit.h
//  命令行输入控件。
//
//  CAD 的命令行不只是「输入框」，它同时是：
//    · 提示区 —— 显示当前命令正在等待什么输入；
//    · 输入区 —— 接受命令名、坐标、选项关键字与数值；
//    · 历史区 —— 上下键回溯，Tab 补全。
//  因此它由一个提示标签与一个输入框组成，并对外只暴露一个 submitted 信号，
//  由主窗口决定文本流向（交给活动工具还是当作命令执行）。
// =============================================================================
#pragma once

#include <QStringList>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;

namespace lycad {

class CommandLineEdit : public QWidget {
    Q_OBJECT

public:
    explicit CommandLineEdit(QWidget* parent = nullptr);

    /// 设置提示文字（如「指定下一点:」）。空字符串会显示默认的「命令:」。
    void setPrompt(const QString& prompt);

    /// 启用 / 禁用补全提示列表（工具运行中无需补全）
    void setCompletionSource(const QStringList& commands);

    /// 把焦点交给输入框
    void focusInput();

    /// 在提示区追加一行输出（历史消息）
    void appendHistory(const QString& text);

    /// 清空输入框
    void clearInput();

    /// 在光标处插入文本（用于「在画布上直接打字」的输入重定向）
    void insertText(const QString& text);

    QString currentText() const;

signals:
    /// 用户提交了文本（回车）
    void submitted(const QString& text);
    /// 用户在输入框中按下了 Esc
    void escapePressed();
    /// 用户按下回车但输入框为空（语义为「确认 / 重复上一命令」）
    void emptySubmitted();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void handleReturn();
    void handleTab();
    void navigateHistory(int direction);
    void updateCompletion(const QString& prefix);
    void hideCompletion();

    QLabel*      m_promptLabel = nullptr;
    QLineEdit*   m_input = nullptr;
    QListWidget* m_completion = nullptr;

    QStringList m_commands;      ///< 可供补全的命令名
    QStringList m_history;
    int         m_historyIndex = -1;   ///< -1 表示不在历史浏览中
    QString     m_pendingDraft;        ///< 进入历史浏览前用户已输入的内容
};

} // namespace lycad
