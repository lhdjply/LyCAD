// =============================================================================
//  LyCAD - ui/TextEditDialog.cpp
// =============================================================================
#include "ui/TextEditDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QTextCursor>
#include <QVBoxLayout>

namespace lycad {
namespace {

/// 多行编辑框。
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

TextEditDialog::TextEditDialog(Mode mode,
                               const QString& content,
                               const QString& fontFamily,
                               QWidget* parent)
    : QDialog(parent)
    , m_mode(mode)
{
    const bool multiLine = (mode == Mode::MultiLine);
    setWindowTitle(multiLine ? QStringLiteral("编辑多行文字") : QStringLiteral("编辑文字"));
    setModal(true);

    // 字体选择：与特性面板同一套表达——首项「（默认）」的数据是空串。
    // 编辑内容时顺手改字体是常见诉求（换一批标注的字体），不必再回特性面板。
    m_font = new QComboBox(this);
    m_font->setObjectName(QStringLiteral("textEditDialog.font"));
    m_font->addItem(QStringLiteral("（默认）"), QString());
    for (const QString& family : QFontDatabase::families()) {
        m_font->addItem(family, family);
        m_font->setItemData(m_font->count() - 1, QFont(family), Qt::FontRole);
    }
    m_font->setToolTip(QStringLiteral(
        "文字所用字体；「（默认）」表示不指定，由系统默认字体绘制"));
    // 当前字体可能是本机没装的（文件带来的）：补一项显示原名，不要显示成「（默认）」
    const QString currentFont = fontFamily.trimmed();
    int fontIndex = m_font->findData(currentFont);
    if (fontIndex < 0 && !currentFont.isEmpty()) {
        m_font->addItem(currentFont, currentFont);
        fontIndex = m_font->count() - 1;
    }
    m_font->setCurrentIndex(fontIndex >= 0 ? fontIndex : 0);

    auto* fontRow = new QHBoxLayout();
    fontRow->setContentsMargins(0, 0, 0, 0);
    fontRow->addWidget(new QLabel(QStringLiteral("字体"), this));
    fontRow->addWidget(m_font, 1);

    QWidget* editor = nullptr;
    QLabel* hint = nullptr;
    if (multiLine) {
        auto* edit = new ContentEdit(this);
        edit->setObjectName(QStringLiteral("textEditDialog.multiLine"));
        edit->setPlainText(content);
        // 不自动折行：文字的版面由插入点与内容里的段落决定（DXF 里也是如此），
        // 编辑框自己折行会让人误以为画面上也会折。
        edit->setLineWrapMode(QPlainTextEdit::NoWrap);
        // Tab 仍然用来在控件之间移动焦点，避免键盘用户被困在编辑框里
        edit->setTabChangesFocus(true);
        m_multiEdit = edit;
        editor = edit;
        hint = new QLabel(QStringLiteral("Enter 确认　·　Alt+Enter 换行　·　Esc 取消"), this);
    } else {
        auto* edit = new QLineEdit(this);
        edit->setObjectName(QStringLiteral("textEditDialog.singleLine"));
        // 单行文字的内容里不该有换行：多出来的行只取第一行显示，
        // 写回时也不会带回去（否则会把对象的版面语义改掉）。
        edit->setText(content.section(QLatin1Char('\n'), 0, 0));
        edit->setPlaceholderText(QStringLiteral("在此输入文字"));
        m_singleEdit = edit;
        editor = edit;
        hint = new QLabel(QStringLiteral("Enter 确认　·　Esc 取消"), this);
    }
    hint->setEnabled(false);          // 视觉上退到次要位置

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确定"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(fontRow);
    layout->addWidget(editor, 1);
    layout->addWidget(hint);
    layout->addWidget(buttons);

    if (multiLine) {
        // 尺寸：宽度够放一行中等长度的标注文字；高度按内容行数给——**打开时就要能看见
        // 全部内容**，不然「多行文字点开只有一行」的观感问题只是从「看不见换行」变成
        // 「看不见后面几行」。行数很多时（长表格说明之类）不无限长高：超过 kMaxVisibleLines
        // 行就不再撑，交给编辑框自己的滚动条；对话框高度上限取屏幕可用高度的四分之三。
        const int lineHeight = QFontMetrics(m_multiEdit->font()).lineSpacing();
        constexpr int kMaxVisibleLines = 18;
        const int visibleLines = qMax(1, qMin(lineCount(), kMaxVisibleLines));
        m_multiEdit->setMinimumHeight(visibleLines * lineHeight
                                      + 2 * m_multiEdit->frameWidth() + 8);

        int height = layout->sizeHint().height();
        if (const QScreen* screen = this->screen()) {
            height = qMin(height, screen->availableGeometry().height() * 3 / 4);
        }
        resize(640, qMax(200, height));
        m_multiEdit->setFocus();
        m_multiEdit->moveCursor(QTextCursor::End);
    } else {
        // 单行没有行数要顾虑：一行的高度加上字体行与按钮行即可
        resize(520, layout->sizeHint().height());
        m_singleEdit->setFocus();
        // 全选：新建时初值通常来自命令行输入或为空，全选便于直接覆盖重打
        m_singleEdit->selectAll();
    }
}

QString TextEditDialog::content() const
{
    if (m_mode == Mode::MultiLine) {
        return m_multiEdit ? m_multiEdit->toPlainText() : QString();
    }
    return m_singleEdit ? m_singleEdit->text() : QString();
}

int TextEditDialog::lineCount() const
{
    if (m_mode == Mode::MultiLine) {
        return m_multiEdit ? m_multiEdit->document()->blockCount() : 0;
    }
    return 1;       // 单行输入框的属性：恒为一行
}

QString TextEditDialog::fontFamily() const
{
    return m_font->currentData().toString();
}

} // namespace lycad
