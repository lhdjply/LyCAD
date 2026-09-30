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
#include "interact/ToolManager.h"
#include "io/CadFile.h"
#include "render/CadCanvas.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
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
    }

    out() << QStringLiteral("完成，失败 %1 项。").arg(failures) << Qt::endl;
    return failures == 0 ? 0 : 1;
}
