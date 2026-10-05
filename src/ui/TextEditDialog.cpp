// =============================================================================
//  LyCAD - ui/TextEditDialog.cpp
// =============================================================================
#include "ui/TextEditDialog.h"

#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QTextCursor>
#include <QVBoxLayout>

namespace lycad {
namespace {

/// 对话框里实际使用的编辑框。
///
/// 只有一个职责：把「回车」从 QPlainTextEdit 手里拿回来。基类对回车的处理是
/// 插入段落分隔符，一旦原样放行，对话框就永远收不到确认键——用户必须按 Tab
/// 跑到按钮上再回车，与 CAD 的手感完全相反。因此这里明确分工：
///   Enter / Return（无修饰键）→ 交给对话框 accept()
///   Alt+Enter                 → 插入一个换行（段落分隔符）
/// 其余按键（含 Shift+Enter、方向键、Ctrl+A 等）一律交回基类，编辑行为不变。
class ContentEdit final : public QPlainTextEdit {
public:
    explicit ContentEdit(QWidget* parent = nullptr)
        : QPlainTextEdit(parent) {}

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        const int key = event->key();
        if (key == Qt::Key_Return || key == Qt::Key_Enter) {
            switch (event->modifiers()) {
            case Qt::AltModifier:
                // 与基类的段落分隔一致：QPlainTextEdit 内部就是按 '\n' 分段的，
                // toPlainText() 也以 '\n' 还原，因此这里直接插一个 '\n' 即可。
                insertPlainText(QStringLiteral("\n"));
                return;
            case Qt::NoModifier:
                if (auto* dialog = qobject_cast<QDialog*>(window())) {
                    dialog->accept();
                }
                return;
            default:
                break;      // Shift/Ctrl 组合保持 Qt 默认行为
            }
        }
        QPlainTextEdit::keyPressEvent(event);
    }
};

} // namespace

TextEditDialog::TextEditDialog(const QString& content, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("编辑文字"));
    setModal(true);

    m_edit = new ContentEdit(this);
    m_edit->setPlainText(content);
    // 不自动折行：文字的版面由插入点与内容里的换行决定（DXF 里也是如此），
    // 编辑框自己折行会让人误以为画面上也会折。
    m_edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    // Tab 仍然用来在控件之间移动焦点，避免键盘用户被困在编辑框里
    m_edit->setTabChangesFocus(true);

    auto* hint = new QLabel(
        QStringLiteral("Enter 确认　·　Alt+Enter 换行　·　Esc 取消"), this);
    hint->setEnabled(false);          // 视觉上退到次要位置

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_edit, 1);
    layout->addWidget(hint);
    layout->addWidget(buttons);

    // 尺寸：宽度够放一行中等长度的标注文字；高度按内容行数给——**打开时就要能看见
    // 全部内容**，不然「多行文字点开只有一行」的观感问题只是从「看不见换行」变成
    // 「看不见后面几行」。行数很多时（长表格说明之类）不无限长高：超过 kMaxVisibleLines
    // 行就不再撑，交给编辑框自己的滚动条；对话框高度上限取屏幕可用高度的四分之三。
    const int lineHeight = QFontMetrics(m_edit->font()).lineSpacing();
    constexpr int kMaxVisibleLines = 18;
    const int visibleLines = qMax(1, qMin(lineCount(), kMaxVisibleLines));
    m_edit->setMinimumHeight(visibleLines * lineHeight + 2 * m_edit->frameWidth() + 8);

    int height = layout->sizeHint().height();
    if (const QScreen* screen = this->screen()) {
        height = qMin(height, screen->availableGeometry().height() * 3 / 4);
    }
    resize(640, qMax(200, height));

    m_edit->setFocus();
    m_edit->moveCursor(QTextCursor::End);
}

QString TextEditDialog::content() const
{
    return m_edit->toPlainText();
}

int TextEditDialog::lineCount() const
{
    return m_edit->document()->blockCount();
}

} // namespace lycad
