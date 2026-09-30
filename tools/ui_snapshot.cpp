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
#include "ui/MainWindow.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QInputDialog>
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
        const EntityId textId = [doc]() -> EntityId {
            if (!doc) return kInvalidEntityId;
            for (EntityId id : doc->drawOrder()) {
                if (doc->entity(id) && doc->entity(id)->type() == EntityType::Text) {
                    return id;
                }
            }
            return kInvalidEntityId;
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

            // 预约自动应答：对话框弹出后在原有内容后追加后缀并确认
            QTimer::singleShot(250, []() {
                if (auto* modal = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                    modal->setTextValue(modal->textValue() + QStringLiteral("-已编辑"));
                    modal->accept();
                }
            });

            QTest::mouseDClick(canvas, Qt::LeftButton, Qt::NoModifier, screen);
            settle(application, 300);

            if (editTarget == kInvalidEntityId) {
                out() << QStringLiteral("  双击未命中任何文字，编辑链路失效。") << Qt::endl;
                ++failures;
            } else {
                const QString before = contentsBefore.value(editTarget);
                const QString after =
                    static_cast<const TextEntity*>(doc->entity(editTarget))->content();

                if (after == before + QStringLiteral("-已编辑")) {
                    out() << QStringLiteral("  双击编辑链路：命中文字 #%1，\"%2\" → \"%3\"")
                                     .arg(editTarget)
                                     .arg(before, after)
                          << Qt::endl;

                    // 撤销应还原原文，重做应再次生效
                    doc->undoStack()->undo();
                    const bool undoOk =
                        static_cast<const TextEntity*>(doc->entity(editTarget))->content() == before;
                    doc->undoStack()->redo();
                    const bool redoOk =
                        static_cast<const TextEntity*>(doc->entity(editTarget))->content()
                            == before + QStringLiteral("-已编辑");
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
                    out() << QStringLiteral("  双击后文字内容未变化，编辑链路失效。") << Qt::endl;
                    ++failures;
                }
            }
            doc->selection().clear();
            QObject::disconnect(tools, &ToolManager::textEditRequested, nullptr, nullptr);
        }
    }

    out() << QStringLiteral("完成，失败 %1 项。").arg(failures) << Qt::endl;
    return failures == 0 ? 0 : 1;
}
