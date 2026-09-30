// =============================================================================
//  LyCAD - ui/CommandLineEdit.cpp
// =============================================================================
#include "ui/CommandLineEdit.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

namespace lycad {

namespace {

constexpr int kMaxHistory = 200;

} // namespace

CommandLineEdit::CommandLineEdit(QWidget* parent)
    : QWidget(parent)
{
    m_promptLabel = new QLabel(this);
    m_promptLabel->setMinimumWidth(260);
    m_promptLabel->setTextFormat(Qt::PlainText);
    m_promptLabel->setStyleSheet(QStringLiteral("QLabel { font-weight: 600; }"));

    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(QStringLiteral("输入命令、坐标或选项，例如 LINE / 100,50 / @50,0 / 50<45"));
    m_input->installEventFilter(this);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 2, 6, 2);
    layout->setSpacing(6);
    layout->addWidget(m_promptLabel);
    layout->addWidget(m_input, 1);

    // 补全下拉：默认隐藏，按需显示
    m_completion = new QListWidget(this);
    m_completion->setWindowFlag(Qt::Popup, true);
    m_completion->setFocusPolicy(Qt::NoFocus);
    m_completion->hide();

    connect(m_completion, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!item) return;
        m_input->setText(item->text());
        hideCompletion();
        m_input->setFocus();
    });

    setPrompt(QString());
}

void CommandLineEdit::setPrompt(const QString& prompt)
{
    m_promptLabel->setText(prompt.trimmed().isEmpty() ? QStringLiteral("命令:") : prompt);
}

void CommandLineEdit::setCompletionSource(const QStringList& commands)
{
    m_commands = commands;
}

void CommandLineEdit::focusInput()
{
    m_input->setFocus(Qt::ShortcutFocusReason);
    m_input->selectAll();
}

void CommandLineEdit::appendHistory(const QString& text)
{
    if (text.isEmpty()) return;

    // 输出通过 tooltip 与输入框提示展示，避免占据垂直空间
    m_input->setToolTip(text);
}

void CommandLineEdit::clearInput()
{
    m_input->clear();
}

void CommandLineEdit::insertText(const QString& text)
{
    if (text.isEmpty()) return;
    m_input->insert(text);
}

QString CommandLineEdit::currentText() const
{
    return m_input->text();
}

bool CommandLineEdit::eventFilter(QObject* watched, QEvent* event)
{
    if (watched != m_input) return QWidget::eventFilter(watched, event);

    if (event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        switch (keyEvent->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            handleReturn();
            return true;
        case Qt::Key_Tab:
            handleTab();
            return true;
        case Qt::Key_Up:
            if (!m_completion->isVisible()) {
                navigateHistory(-1);
                return true;
            }
            break;
        case Qt::Key_Down:
            if (!m_completion->isVisible()) {
                navigateHistory(1);
                return true;
            }
            break;
        case Qt::Key_Escape:
            if (m_completion->isVisible()) {
                hideCompletion();
            } else {
                m_input->clear();
                emit escapePressed();
            }
            return true;
        default:
            break;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void CommandLineEdit::handleReturn()
{
    hideCompletion();

    const QString text = m_input->text().trimmed();
    m_input->clear();
    m_historyIndex = -1;

    if (text.isEmpty()) {
        emit emptySubmitted();
        return;
    }

    // 去重：连续重复的命令不在历史中堆叠
    if (m_history.isEmpty() || m_history.back() != text) {
        m_history.append(text);
        if (m_history.size() > kMaxHistory) m_history.removeFirst();
    }

    emit submitted(text);
}

void CommandLineEdit::handleTab()
{
    updateCompletion(m_input->text());
}

void CommandLineEdit::navigateHistory(int direction)
{
    if (m_history.isEmpty()) return;

    if (m_historyIndex == -1) {
        m_pendingDraft = m_input->text();
        m_historyIndex = m_history.size();
    }

    m_historyIndex += direction;

    if (m_historyIndex < 0) m_historyIndex = 0;
    if (m_historyIndex > m_history.size()) m_historyIndex = m_history.size();

    if (m_historyIndex == m_history.size()) {
        m_input->setText(m_pendingDraft);
        return;
    }

    m_input->setText(m_history.at(m_historyIndex));
    m_input->setCursorPosition(m_input->text().size());
}

void CommandLineEdit::updateCompletion(const QString& prefix)
{
    hideCompletion();

    const QString trimmed = prefix.trimmed().toUpper();
    if (trimmed.isEmpty() || m_commands.isEmpty()) return;

    QStringList matches;
    for (const QString& command : m_commands) {
        if (command.startsWith(trimmed)) matches << command;
        if (matches.size() >= 12) break;
    }
    if (matches.size() < 2) return;      // 唯一候选时不值得弹出列表

    m_completion->clear();
    m_completion->addItems(matches);
    m_completion->setCurrentRow(0);

    const QPoint origin = m_input->mapToGlobal(QPoint(0, m_input->height()));
    m_completion->move(origin);
    m_completion->setFixedWidth(m_input->width());
    m_completion->show();
}

void CommandLineEdit::hideCompletion()
{
    m_completion->hide();
}

} // namespace lycad
