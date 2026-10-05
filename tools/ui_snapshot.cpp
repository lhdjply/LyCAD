// =============================================================================
//  LyCAD - tools/ui_snapshot.cpp
//  界面快照工具（开发期使用）。
//
//  为什么不用外部截图命令
//    依赖窗口管理器的截图既不稳定（窗口可能被遮挡、被重排），也无法在
//    无显示环境的 CI 中运行。直接在进程内渲染窗口到 QPixmap，
//    结果完全确定，且可在离屏平台下执行。
//
//  额外价值
//    本工具通过 QTest 向画布注入真实的鼠标事件来驱动完整交互链路
//    （画布 → ToolManager → 工具状态机 → 文档），
//    因此它的成功运行本身就是一次端到端的功能验证，
//    而不仅仅是「窗口能画出来」。
//
//  用法： lycad_ui_snapshot <输入.dxf> <输出目录>
// =============================================================================
#include "core/Document.h"
#include "core/TextEntity.h"
#include "interact/ToolManager.h"
#include "io/CadFile.h"
#include "render/CadCanvas.h"
#include "ui/IconLibrary.h"
#include "ui/MainWindow.h"
#include "ui/RibbonBar.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextCursor>
#include <QToolButton>
#include <QUndoStack>
#include <QTest>
#include <QTextStream>
#include <QTimer>

#include <memory>

using namespace lycad;

namespace {

QTextStream& out()
{
    static QTextStream stream(stdout);
    return stream;
}

/// 推进事件循环，让布局与绘制完成
void settle(QApplication& application, int milliseconds = 250)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds) {
        application.processEvents(QEventLoop::AllEvents, 20);
    }
}

bool capture(QApplication& application, QWidget& widget, const QString& path)
{
    settle(application, 350);
    const QPixmap pixmap = widget.grab();
    if (pixmap.isNull()) {
        out() << QStringLiteral("  截取失败: ") << path << Qt::endl;
        return false;
    }
    if (!pixmap.save(path)) {
        out() << QStringLiteral("  保存失败: ") << path << Qt::endl;
        return false;
    }
    out() << QStringLiteral("  已保存 %1  (%2×%3)")
                 .arg(QFileInfo(path).fileName())
                 .arg(pixmap.width())
                 .arg(pixmap.height())
          << Qt::endl;
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("LyCAD-Snapshot"));
    application.setOrganizationName(QStringLiteral("LyCAD-Snapshot"));

    registerBuiltinCodecs();

    const QStringList arguments = application.arguments();
    const QString inputPath = (arguments.size() > 1) ? arguments.at(1) : QString();
    const QString outputDir = (arguments.size() > 2) ? arguments.at(2) : QStringLiteral(".");

    QDir dir(outputDir);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        out() << QStringLiteral("无法创建输出目录: ") << outputDir << Qt::endl;
        return 2;
    }

    int failures = 0;

    // ---- 快照 1：加载图形、缩放至范围 ----
    {
        MainWindow window;
        window.resize(1500, 950);
        window.show();
        settle(application, 400);

        if (!inputPath.isEmpty()) {
            if (!window.openFile(inputPath)) {
                out() << QStringLiteral("打开文件失败: ") << inputPath << Qt::endl;
                ++failures;
            }
        }

        auto* canvas = window.findChild<CadCanvas*>();
        if (!canvas) {
            out() << QStringLiteral("找不到画布控件。") << Qt::endl;
            return 3;
        }
        canvas->zoomExtents();

        if (!capture(application, window, dir.filePath(QStringLiteral("ui_overview.png")))) {
            ++failures;
        }

        // ---- 快照 2：执行绘制命令，显示实时预览 ----
        auto* tools = window.findChild<ToolManager*>();
        if (!tools) {
            out() << QStringLiteral("找不到工具管理器。") << Qt::endl;
            return 3;
        }

        // 通过真实的工具命令与鼠标事件驱动交互链路
        if (!tools->execute(QStringLiteral("LINE"))) {
            out() << QStringLiteral("无法启动 LINE 命令。") << Qt::endl;
            ++failures;
        }
        settle(application, 120);

        const QPoint points[] = { QPoint(430, 320), QPoint(700, 520), QPoint(980, 380) };
        for (const QPoint& point : points) {
            QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, point);
            settle(application, 120);
        }
        // 移动光标以产生橡皮筋预览
        QTest::mouseMove(canvas, QPoint(1120, 600));
        settle(application, 200);

        const std::size_t created = canvas->document() ? canvas->document()->entityCount() : 0;
        out() << QStringLiteral("  交互链路：已创建图元总数 %1").arg(created) << Qt::endl;

        if (!capture(application, window, dir.filePath(QStringLiteral("ui_draw_preview.png")))) {
            ++failures;
        }

        tools->cancel();
        settle(application, 120);

        // ---- 快照 3：全选以显示高亮与夹点 ----
        if (canvas->document()) {
            canvas->document()->selection().setIds(canvas->document()->drawOrder());
        }
        if (!capture(application, window, dir.filePath(QStringLiteral("ui_selection.png")))) {
            ++failures;
        }

        // ---- 快照 4：双击文字 → 编辑对话框 → 内容修改（可撤销） ----
        Document* doc = canvas->document();
        // 优先挑一条**多行**文字：MTEXT 降级导入的文字内容里带换行，双击后编辑框
        // 必须把每一行都显示出来——这正是「双击多行文字点开没有多行」要盯的场景。
        // 文档里没有多行文字时退回第一条，至少验证单行的编辑链路。
        const EntityId textId = [doc]() -> EntityId {
            if (!doc) return kInvalidEntityId;
            EntityId first = kInvalidEntityId;
            for (EntityId id : doc->drawOrder()) {
                const Entity* entity = doc->entity(id);
                if (!entity || entity->type() != EntityType::Text) continue;
                if (first == kInvalidEntityId) first = id;
                if (static_cast<const TextEntity*>(entity)->lineCount() > 1) return id;
            }
            return first;
        }();

        if (textId == kInvalidEntityId || !doc) {
            out() << QStringLiteral("  文档中没有文字图元，跳过双击编辑验证。") << Qt::endl;
        } else {
            auto* text = static_cast<const TextEntity*>(doc->entity(textId));

            // 文字插入点 → 屏幕坐标
            const Point2D device = canvas->view().toDevice(text->effectivePosition());
            const QPoint screen(static_cast<int>(device.x), static_cast<int>(device.y));

            // 记录双击实际命中的文字：光标处可能有多条文字的包围盒重叠
            //（宽容包围盒 + 填充遮挡），双击编辑的是其中最上层的那条
            EntityId editTarget = kInvalidEntityId;
            QObject::connect(tools, &ToolManager::textEditRequested,
                             [&editTarget](EntityId id) { editTarget = id; });

            // 编辑前记录全部文字内容（命中对象在双击前不可预知）
            QHash<EntityId, QString> contentsBefore;
            for (EntityId id : doc->drawOrder()) {
                if (doc->entity(id) && doc->entity(id)->type() == EntityType::Text) {
                    contentsBefore[id] = static_cast<const TextEntity*>(doc->entity(id))->content();
                }
            }

            // 预约自动应答：先把「刚打开的样子」截下来，再追加后缀 → Alt+Enter 换行
            // → 写第二行 → 再截一张 → 回车确认。截图是必需的：多行文字双击后必须
            // **看得见多行**、Alt+Enter 必须**看得见换行**，这两点无法从文档内容断言
            // 出来，只能看图。
            const QString dialogShot = dir.filePath(QStringLiteral("ui_text_dialog.png"));
            const QString editedShot = dir.filePath(QStringLiteral("ui_text_dialog_edit.png"));
            QTimer::singleShot(250, [dialogShot, editedShot]() {
                auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (!modal) return;

                const auto saveShot = [](QWidget& widget, const QString& path) {
                    const QPixmap pixmap = widget.grab();
                    if (pixmap.isNull() || !pixmap.save(path)) return;
                    out() << QStringLiteral("  已保存 %1  (%2×%3)")
                                 .arg(QFileInfo(path).fileName())
                                 .arg(pixmap.width())
                                 .arg(pixmap.height())
                          << Qt::endl;
                };

                saveShot(*modal, dialogShot);

                auto* edit = modal->findChild<QPlainTextEdit*>();
                if (!edit) return;

                edit->setPlainText(edit->toPlainText() + QStringLiteral("-已编辑"));
                edit->moveCursor(QTextCursor::End);
                QTest::keyClick(edit, Qt::Key_Return, Qt::AltModifier);   // 换行
                edit->insertPlainText(QStringLiteral("第二行"));
                QApplication::processEvents();
                saveShot(*modal, editedShot);
                QTest::keyClick(edit, Qt::Key_Return, Qt::NoModifier);    // 确认
            });

            QTest::mouseDClick(canvas, Qt::LeftButton, Qt::NoModifier, screen);
            settle(application, 300);

            if (editTarget == kInvalidEntityId) {
                out() << QStringLiteral("  双击未命中任何文字，编辑链路失效。") << Qt::endl;
                ++failures;
            } else {
                const QString before = contentsBefore.value(editTarget);
                const QString expected =
                    before + QStringLiteral("-已编辑\n第二行");
                const auto* edited =
                    static_cast<const TextEntity*>(doc->entity(editTarget));
                const QString after = edited->content();

                if (after == expected) {
                    out() << QStringLiteral("  双击编辑链路：命中文字 #%1，"
                                            "原文 %2 行 → 编辑后 %3 行")
                                 .arg(editTarget)
                                 .arg(before.count(QLatin1Char('\n')) + 1)
                                 .arg(edited->lineCount())
                          << Qt::endl;

                    // 撤销应还原原文，重做应再次生效
                    doc->undoStack()->undo();
                    const bool undoOk =
                        static_cast<const TextEntity*>(doc->entity(editTarget))->content() == before;
                    doc->undoStack()->redo();
                    const bool redoOk =
                        static_cast<const TextEntity*>(doc->entity(editTarget))->content()
                            == expected;
                    out() << QStringLiteral("  撤销/重做: %1 / %2")
                                 .arg(undoOk ? QStringLiteral("通过")
                                             : QStringLiteral("失败"),
                                      redoOk ? QStringLiteral("通过")
                                             : QStringLiteral("失败"))
                          << Qt::endl;
                    if (!undoOk || !redoOk) ++failures;

                    doc->selection().setIds({ editTarget });
                    if (!capture(application, window,
                                 dir.filePath(QStringLiteral("ui_text_edit.png")))) {
                        ++failures;
                    }
                } else {
                    // 期望值里那半行「第二行」是 Alt+Enter 换行后才有的：内容对不上
                    // 就意味着多行编辑或 Alt+Enter 换行失效了。
                    out() << QStringLiteral("  双击后文字内容与预期不符（Alt+Enter 换行未生效？）"
                                            "\n    期望: %1\n    实际: %2")
                                 .arg(expected, after)
                          << Qt::endl;
                    ++failures;
                }
            }
            doc->selection().clear();
            QObject::disconnect(tools, &ToolManager::textEditRequested, nullptr, nullptr);
        }

        // ---- 快照 5：功能区自检 + 深色主题 ----
        auto* ribbon = window.findChild<RibbonBar*>();
        if (!ribbon) {
            out() << QStringLiteral("找不到功能区控件。") << Qt::endl;
            return 3;
        }

        // 逐个数一遍按钮：既确认面板真的建起来了，也顺带兜住图标名写错的情况
        // —— IconLibrary 对缺失的图标只会返回空 QIcon 并留一条警告，
        // 界面上表现为「按钮没有图案」，靠肉眼很容易漏掉。
        int iconMissing = 0;
        int largeButtons = 0;
        int smallButtons = 0;
        int toggleButtons = 0;
        const QList<QToolButton*> ribbonButtons = ribbon->findChildren<QToolButton*>();
        for (QToolButton* button : ribbonButtons) {
            if (button->text().isEmpty()) continue;

            const bool large = (button->toolButtonStyle() == Qt::ToolButtonTextUnderIcon);
            large ? ++largeButtons : ++smallButtons;
            if (button->isCheckable()) ++toggleButtons;
            if (button->icon().isNull()) {
                ++iconMissing;
                out() << QStringLiteral("  图标缺失：按钮 \"%1\"").arg(button->text()) << Qt::endl;
            }
        }

        const int comboCount = ribbon->findChildren<QComboBox*>().size();
        out() << QStringLiteral("  功能区：大按钮 %1，小按钮 %2，开关 %3，下拉框 %4，图标缺失 %5")
                     .arg(largeButtons)
                     .arg(smallButtons)
                     .arg(toggleButtons)
                     .arg(comboCount)
                     .arg(iconMissing)
              << Qt::endl;
        if (iconMissing > 0) ++failures;
        if (largeButtons == 0 || smallButtons == 0 || comboCount < 3) {
            out() << QStringLiteral("  功能区内容不完整。") << Qt::endl;
            ++failures;
        }

        // 通过功能区上的真实按钮切换深色主题，顺带验证：
        // 按钮 → MainWindow → RibbonBar + IconLibrary + 画布 这条链路。
        QToolButton* themeButton = nullptr;
        for (QToolButton* button : ribbonButtons) {
            if (button->text() == QStringLiteral("深色主题")) {
                themeButton = button;
                break;
            }
        }
        if (!themeButton) {
            out() << QStringLiteral("  功能区上找不到「深色主题」按钮。") << Qt::endl;
            ++failures;
        } else {
            themeButton->click();
            settle(application, 250);

            const bool ribbonDark = ribbon->isDarkTheme();
            const bool iconsDark = IconLibrary::instance().isDarkTheme();
            const bool canvasDark = canvas->display().themeIsDark;

            // 调色板核对。深色主题最常见的失败方式是「QApplication::setPalette
            // 被某个环节重置」，而它不会报任何错，只表现为部分控件仍是浅色。
            // 抽查一个位于停靠面板里的下拉框：功能区里的下拉框有自己的样式表
            // 兜底，反而看不出调色板是否生效。
            if (auto* combo = window.findChild<QComboBox*>()) {
                const QColor base = combo->palette().color(QPalette::Base);
                const QColor appWindow = QApplication::palette().color(QPalette::Window);
                const bool ok = base.lightness() < 128 && appWindow.lightness() < 128;
                out() << QStringLiteral("  调色板：应用 Window=%1，停靠面板下拉框 Base=%2 → %3")
                             .arg(appWindow.name(), base.name(),
                                  ok ? QStringLiteral("深色已生效")
                                     : QStringLiteral("仍为浅色"))
                      << Qt::endl;
                if (!ok) ++failures;
            }
            out() << QStringLiteral("  深色主题：功能区 %1，图标 %2，画布 %3")
                         .arg(ribbonDark ? QStringLiteral("已切换")
                                         : QStringLiteral("未切换"),
                              iconsDark ? QStringLiteral("已换色")
                                        : QStringLiteral("未换色"),
                              canvasDark ? QStringLiteral("已切换")
                                         : QStringLiteral("未切换"))
                  << Qt::endl;
            if (!ribbonDark || !iconsDark || !canvasDark) ++failures;

            if (!capture(application, window, dir.filePath(QStringLiteral("ui_theme_dark.png")))) {
                ++failures;
            }

            // 把窗口调小，逼出滚动条：功能区要横向滚动、图层列表要纵向滚动。
            // 默认尺寸下内容刚好放得下，滚动条根本不出现，而「深色底上的深色
            // 滑块」恰恰是深色主题最容易漏掉的一处。
            window.resize(1060, 620);
            settle(application, 350);

            int visibleBars = 0;
            for (QScrollBar* bar : ribbon->window()->findChildren<QScrollBar*>()) {
                if (bar->isVisible() && bar->maximum() > bar->minimum()) ++visibleBars;
            }
            out() << QStringLiteral("  深色小窗口：可见滚动条 %1 条").arg(visibleBars) << Qt::endl;
            if (visibleBars == 0) {
                out() << QStringLiteral("  未能逼出滚动条，无法核对滚动条配色。") << Qt::endl;
                ++failures;
            }
            if (!capture(application, window,
                         dir.filePath(QStringLiteral("ui_compact_dark.png")))) {
                ++failures;
            }

            window.resize(1500, 950);
            settle(application, 250);

            themeButton->click();     // 切回浅色，避免影响后续用例
            settle(application, 200);
        }

        // ---- 快照 6：图层与特性下拉框就近取景 ----
        // 这两个下拉框在窄窗口下位于功能区的右端，需要滚动才能看到，
        // 因此单独抓一张「滚动到末端」的图，方便核对它们的外观。
        if (auto* scroller =
                window.findChild<QScrollArea*>(QStringLiteral("ribbonViewport"))) {
            scroller->horizontalScrollBar()->setValue(
                scroller->horizontalScrollBar()->maximum());
            if (!capture(application, window,
                         dir.filePath(QStringLiteral("ui_ribbon_tail.png")))) {
                ++failures;
            }
            scroller->horizontalScrollBar()->setValue(0);
        } else {
            out() << QStringLiteral("  找不到功能区的滚动容器。") << Qt::endl;
            ++failures;
        }
    }

    out() << QStringLiteral("完成，失败 %1 项。").arg(failures) << Qt::endl;
    return failures == 0 ? 0 : 1;
}
