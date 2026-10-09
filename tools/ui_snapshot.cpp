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
//  用法： lycad_ui_snapshot <输入.dxf|.dwg> <输出目录>
// =============================================================================
#include "core/Document.h"
#include "core/EllipseEntity.h"
#include "core/Layer.h"
#include "core/TextEntity.h"
#include "interact/ToolManager.h"
#include "io/CadFile.h"
#include "render/CadCanvas.h"
#include "render/PlotRenderer.h"
#include "ui/IconLibrary.h"
#include "ui/MainWindow.h"
#include "ui/PropertyPanel.h"
#include "ui/RibbonBar.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>

#include <functional>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPalette>
#include <QPlainTextEdit>
#include <QRadioButton>
#include <QPrintPreviewDialog>
#include <QPrintPreviewWidget>
#include <QPushButton>
#include <QPrinter>
#include <QPixmap>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextCursor>
#include <QToolButton>
#include <QUndoStack>
#include <QTest>
#include <QTextStream>
#include <QTimer>

#include <limits>
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

/// 让保存对话框指向指定路径。
///
/// 别用 QFileDialog::selectFile()：目录模型是异步加载的，它设进去的名字会被
/// 随后的模型重置吃掉——实测 lineEdit 仍是初始名、selectedFiles() 返回的还是
/// suggested 路径，结果是文件被写到别处（甚至是仓库里的样例文件）。
/// 直接改文件名输入框最可靠，200 ms / 600 ms 都验证过。
void pointSaveDialogAt(QFileDialog& dialog, const QString& path)
{
    if (auto* edit = dialog.findChild<QLineEdit*>(QStringLiteral("fileNameEdit"))) {
        edit->setText(path);
        return;
    }
    dialog.selectFile(path);
}

/// 驱动一次「保存文件」模态流程：trigger 会在文件对话框的嵌套事件循环里阻塞，
/// 这里用定时器把对话框找出来，交给 inspect 摆好状态；**inspect 返回 true
///（目标路径确实已经生效）才接受**。
///
/// 三个坑都是实测踩出来的，少一个就会出问题：
///   · **要轮询，不能只等一次**。只查一次的写法一旦早于对话框弹出（或那一刻
///     恰好有别的模态框浮在上面），就再也没人关它——工具静静挂死在嵌套事件
///     循环里，没有输出也没有退出码，看起来就是「卡了」。
///   · **不能用 selectFile()**：目录模型异步加载，它设进去的名字会被模型重置
///     吃掉，对话框仍指向初始的 suggested 路径（即「当前图纸所在的目录」）——
///     照此接受就会把文件写进仓库的 sample/，实测确实写进去过。
///     改成直接改文件名输入框。
///   · **要等目标路径真的生效再接受**：只看「非空」不够，初始 suggested 的名字
///     同样非空。所以每轮都重新摆一次状态，并核对当前值是否已等于目标值。
/// 另配一个兜底定时器：到点仍未收工就强行关掉挡路的模态框，
/// 宁可让这项检查失败，也不让工具挂住。
bool driveSaveDialog(QApplication& application, QWidget& context,
                     const std::function<bool(QFileDialog&)>& inspect,
                     const std::function<void()>& trigger)
{
    bool handled = false;

    QTimer poll;
    poll.setInterval(50);
    QObject::connect(&poll, &QTimer::timeout, &context, [&]() {
        auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        if (!inspect(*dialog)) return;                   // 状态还没就绪，下一轮再来

        // accept() 在 QFileDialog 里是 protected，经 QDialog 的公开接口虚调用过去
        // 才是合法的（走的仍是 QFileDialog 的重载）
        static_cast<QDialog*>(dialog)->accept();
        if (!dialog->isVisible()) {
            handled = true;
            poll.stop();
        }
    });
    poll.start();

    QTimer watchdog;
    watchdog.setSingleShot(true);
    watchdog.setInterval(8000);
    QObject::connect(&watchdog, &QTimer::timeout, &context, [&]() {
        auto* blocker = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!blocker) return;
        out() << QStringLiteral("  [警告] 保存对话框未能正常接受，强制关闭以免工具挂住")
              << Qt::endl;
        blocker->reject();
    });
    watchdog.start();

    trigger();
    poll.stop();
    watchdog.stop();
    settle(application, 200);
    return handled;
}

} // namespace

int main(int argc, char* argv[])
{
    // 文件对话框一律用 Qt 自绘版本：本工具要在离屏环境里**驱动**它们
    // （找到模态对话框 → 填路径 → 接受），原生对话框是平台的东西，抓不到也点不动。
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
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

    // 输入文件先检查一遍再交给主窗口。打开失败时主窗口会弹一个模态错误框，
    // 而离屏环境里没人点得动它——进程就静静挂在那里，输出缓冲全丢，
    // 看上去像「卡死」而不是「文件不存在」。这里给它一个能看懂的出口。
    if (!inputPath.isEmpty() && !QFileInfo::exists(inputPath)) {
        out() << QStringLiteral("输入文件不存在: ") << inputPath << Qt::endl;
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

        // ---- 快照 2b：椭圆 —— 从功能区的真实按钮进入，再用坐标落一个椭圆 ----
        //
        // 盯的是「工具栏里到底有没有椭圆入口」：椭圆的实体、渲染、读写都齐了，
        // 唯独绘图面板上没有按钮时，用户看到的和「没实现」没有区别——而这类
        // 缺口不会让任何单元测试变红，只有把按钮点一遍才暴露得出来。
        {
            // 椭圆与圆共用一个下拉按钮，「椭圆」是菜单项而不是独立按钮：
            // 相应地改为在功能区的下拉菜单里找它（菜单项对应的 QAction）。
            auto* ribbonBar = window.findChild<RibbonBar*>();
            QAction* ellipseAction = nullptr;
            if (ribbonBar) {
                for (QAction* action : ribbonBar->findChildren<QAction*>()) {
                    if (action->text() == QStringLiteral("椭圆")) {
                        ellipseAction = action;
                        break;
                    }
                }
            }

            if (!ellipseAction) {
                out() << QStringLiteral("  功能区「绘图」面板的下拉菜单里没有椭圆入口。")
                      << Qt::endl;
                ++failures;
            } else if (!tools->hasCommand(QStringLiteral("ELLIPSE"))) {
                out() << QStringLiteral("  椭圆入口存在，但 ELLIPSE 命令没有注册。") << Qt::endl;
                ++failures;
            } else if (canvas->document()) {
                Document* target = canvas->document();

                // 本段要按精确坐标断言几何，先关掉对象捕捉——否则坐标点会被吸到
                // 附近图元上，断言随即变成随机失败。用完恢复。
                const SnapSettings savedSnap = tools->snapSettings();
                tools->snapSettings().enabled = false;

                // ① 只走视觉与方向反馈：按钮 → 定轴的第一点 → 在正方向附近移动光标
                //    → 两点定轴 → 移动光标预览 → 截图
                //
                // 触发前先记住按钮文字：菜单项被选中后，下拉按钮本身要切换成那一项
                // （图标 + 文字一起换）。这是「选过哪项、按钮就是哪项」的行为，不
                // 盯住它，后续重构很容易悄悄退化回「永远显示圆」。
                auto* dropDownButton = [&]() -> QToolButton* {
                    QObject* menu = ellipseAction->parent();
                    return menu ? qobject_cast<QToolButton*>(menu->parent()) : nullptr;
                }();
                const QString textBefore = dropDownButton ? dropDownButton->text() : QString();

                ellipseAction->trigger();
                settle(application, 150);

                if (dropDownButton) {
                    const bool switched = dropDownButton->text() == QStringLiteral("椭圆")
                                          && textBefore != QStringLiteral("椭圆");
                    out() << QStringLiteral("  下拉按钮随选择切换（%1 → %2）: %3")
                                 .arg(textBefore, dropDownButton->text(),
                                      switched ? QStringLiteral("通过")
                                               : QStringLiteral("失败"))
                          << Qt::endl;
                    if (!switched) ++failures;
                }

                // 光标移动直接送进工具管理器，而不是走 QTest::mouseMove：
                // 离屏平台下合成移动会被合并/丢弃（实测三次里只有一次到达），
                // 而这一段考察的是「工具是否把方向吸到正方向」，与事件投递无关。
                const auto moveTo = [&](const QPoint& p) {
                    CanvasEvent event;
                    event.devicePos = Point2D(p.x(), p.y());
                    event.devicePosRounded = p;
                    event.worldPos = canvas->view().toWorld(event.devicePos);
                    tools->handleMouseMove(event);
                };

                // 画布控件本身只有 752×754，坐标要留在画布内，预览与追踪线才完整
                const QPoint firstPoint(150, 170);
                QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, firstPoint);
                settle(application, 120);

                // 定轴方向上「快摆平了」时应当吸附到水平/竖直，并给出文字指示。
                // 判据是到正方向射线的**像素**垂距落在光圈（12 px）内，所以这里
                // 特意只偏 6 px：再差一点就不该吸。
                const auto directionSnapLabel = [&canvas]() -> QString {
                    const std::optional<SnapResult>& snap = canvas->snapResult();
                    if (!snap || !snap->isValid()) return QString();
                    return snap->kind == SnapKind::Ortho ? snap->label : QString();
                };

                moveTo(QPoint(firstPoint.x() + 430, firstPoint.y() + 6));
                settle(application, 60);
                const QString horizontalLabel = directionSnapLabel();
                // 抓一张「方向已吸附」的画面：过基点的长追踪线 + 「水平」标记
                if (!capture(application, window,
                             dir.filePath(QStringLiteral("ui_ellipse_direction.png")))) {
                    ++failures;
                }

                moveTo(QPoint(firstPoint.x() + 6, firstPoint.y() + 430));
                settle(application, 60);
                const QString verticalLabel = directionSnapLabel();

                // 反例：方向偏离正方向 200 px（远超光圈），不得吸附
                moveTo(QPoint(firstPoint.x() + 430, firstPoint.y() + 200));
                settle(application, 60);
                const QString freeLabel = directionSnapLabel();

                const bool snapOk = horizontalLabel == QStringLiteral("水平")
                                    && verticalLabel == QStringLiteral("竖直")
                                    && freeLabel.isEmpty();
                out() << QStringLiteral("  椭圆轴方向吸附：偏 6 px →「%1」/「%2」，"
                                        "偏 200 px →%3")
                             .arg(horizontalLabel.isEmpty() ? QStringLiteral("未吸附")
                                                            : horizontalLabel,
                                  verticalLabel.isEmpty() ? QStringLiteral("未吸附")
                                                          : verticalLabel,
                                  freeLabel.isEmpty() ? QStringLiteral("未吸附（正确）")
                                                      : freeLabel)
                      << Qt::endl;
                if (!snapOk) ++failures;

                // 顺着水平吸附落第二点，预览就是一条水平的轴 + 竖直的短轴
                QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier,
                                  QPoint(firstPoint.x() + 430, firstPoint.y() + 6));
                settle(application, 120);
                moveTo(QPoint(firstPoint.x() + 180, firstPoint.y() + 130));
                settle(application, 200);
                if (!capture(application, window, dir.filePath(QStringLiteral("ui_ellipse_preview.png")))) {
                    ++failures;
                }
                tools->cancel();
                settle(application, 120);

                // ② 按坐标精确落一个椭圆：轴端点 (100000,100000)-(100300,100000)，
                //    另一条半轴 90 → 中心 (100150,100000)、长半轴 150、比值 0.6
                const int undoBefore = target->undoStack()->index();

                ellipseAction->trigger();
                settle(application, 120);
                tools->submitText(QStringLiteral("100000,100000"));
                tools->submitText(QStringLiteral("100300,100000"));
                tools->submitText(QStringLiteral("90"));
                settle(application, 150);

                const EllipseEntity* createdEllipse = nullptr;
                for (EntityId id : target->drawOrder()) {
                    const Entity* entity = target->entity(id);
                    if (!entity || entity->type() != EntityType::Ellipse) continue;
                    const auto* candidate = static_cast<const EllipseEntity*>(entity);
                    if (std::abs(candidate->center().x - 100150.0) < 1.0) {
                        createdEllipse = candidate;
                    }
                }

                if (!createdEllipse) {
                    out() << QStringLiteral("  点击椭圆按钮并输入坐标后没有生成椭圆。") << Qt::endl;
                    ++failures;
                } else {
                    // 中心 / 长半轴 / 比值 / 闭合 四项各自能独立出错：
                    // 中心错说明轴向取反，比值错说明长短轴挑错了那个，
                    // 不闭合则说明副主轴比没被正确换算成整椭圆。
                    const bool centerOk =
                        std::abs(createdEllipse->center().y - 100000.0) < 1.0;
                    const bool majorOk = std::abs(createdEllipse->majorRadius() - 150.0) < 1.0;
                    const bool ratioOk = std::abs(createdEllipse->ratio() - 0.6) < 1e-3;
                    const bool closedOk = createdEllipse->isClosed();
                    const bool recordsOk = target->undoStack()->index() == undoBefore + 1;

                    out() << QStringLiteral("  椭圆工具：中心 %1 / 长半轴 %2 / 副主轴比 %3 / 整椭圆 %4 "
                                            "/ 撤销记录 %5")
                                 .arg(centerOk ? QStringLiteral("通过") : QStringLiteral("失败"),
                                      majorOk ? QStringLiteral("通过") : QStringLiteral("失败"),
                                      ratioOk ? QStringLiteral("通过") : QStringLiteral("失败"),
                                      closedOk ? QStringLiteral("通过") : QStringLiteral("失败"),
                                      recordsOk ? QStringLiteral("+1")
                                                : QStringLiteral("异常"))
                          << Qt::endl;
                    if (!centerOk || !majorOk || !ratioOk || !closedOk || !recordsOk) {
                        ++failures;
                    }
                }

                // ③ 退化输入（轴的两个端点重合）必须被拦下，且不留下撤销记录
                const std::size_t beforeDegenerate = target->entityCount();
                const int undoAtDegenerate = target->undoStack()->index();
                tools->submitText(QStringLiteral("200000,200000"));
                tools->submitText(QStringLiteral("200000,200000"));
                tools->submitText(QStringLiteral("90"));
                settle(application, 120);

                const bool degenerateRejected =
                    target->entityCount() == beforeDegenerate
                    && target->undoStack()->index() == undoAtDegenerate;
                out() << QStringLiteral("  椭圆退化输入（轴端点重合）：%1")
                             .arg(degenerateRejected ? QStringLiteral("已拦下")
                                                     : QStringLiteral("产生了图元"))
                      << Qt::endl;
                if (!degenerateRejected) ++failures;

                tools->cancel();
                tools->snapSettings() = savedSnap;
                settle(application, 120);

                // 后面几条链路（双击编辑、面板自检）都会数撤销记录，先把命令状态
                // 清干净，再撤掉这个测试用的椭圆，保证后续断言不受影响
                while (target->undoStack()->index() > undoBefore) {
                    target->undoStack()->undo();
                }
                settle(application, 120);
            }
        }

        // ---- 快照 2c：出图（打印）----
        //
        //  纸面上看到的东西与画布上不是一回事：有边距、没有栅格与选择高亮、线宽要
        //  按毫米落到设备分辨率上、图层还能带「不打印」标志。因此这一段从四个方向
        //  盯它：
        //    · 入口——菜单与功能区的「打印」是否真的接上了；
        //    · 页面——用与打印同一段代码渲染一页，数墨迹（不是空白页，也没糊满整页），
        //      并验证「不打印」图层真的没被画上去；
        //    · 比例——1:1 / 1:2 / 1:4 的纸面宽度必须成比例（见下方那段）；
        //    · 出口——打印对话框（设置 + 预览同屏）走 PDF 目标完整走一遍，
        //      这是离屏环境里唯一不会把作业送到真打印机的路径。
        {
            const auto findAction = [&window](const QString& text) -> QAction* {
                for (QAction* action : window.findChildren<QAction*>()) {
                    if (action->text() == text) return action;
                }
                return nullptr;
            };
            QAction* printAction = findAction(QStringLiteral("打印(&P)…"));
            // 打印、打印预览、打印设置已合并成一个对话框，旧的独立入口不该再出现
            QAction* stalePreview = findAction(QStringLiteral("打印预览(&V)…"));
            QAction* staleSettings = findAction(QStringLiteral("打印设置(&T)…"));

            int printButtons = 0;
            if (auto* ribbonBar = window.findChild<RibbonBar*>()) {
                for (QToolButton* button : ribbonBar->findChildren<QToolButton*>()) {
                    const QString text = button->text();
                    if (text == QStringLiteral("打印")) ++printButtons;
                }
            }

            const bool shortcutsOk = printAction && !printAction->shortcuts().isEmpty();
            const bool entriesOk = printAction && !stalePreview && !staleSettings
                                   && printButtons == 1 && shortcutsOk;
            out() << QStringLiteral("  出图入口：菜单 %1，功能区按钮 %2 个，"
                                    "旧「预览/设置」入口 %3，打印快捷键 %4 → %5")
                         .arg(printAction ? QStringLiteral("1 项") : QStringLiteral("缺失"),
                              QString::number(printButtons),
                              (stalePreview || staleSettings)
                                  ? QStringLiteral("仍存在")
                                  : QStringLiteral("已合并"),
                              printAction && !printAction->shortcuts().isEmpty()
                                  ? printAction->shortcut().toString()
                                  : QStringLiteral("无"),
                              entriesOk ? QStringLiteral("就绪") : QStringLiteral("不完整"))
                  << Qt::endl;
            if (!entriesOk) ++failures;

            Document* plotDoc = canvas->document();
            if (!plotDoc) {
                out() << QStringLiteral("  没有文档，跳过出图验证。") << Qt::endl;
                ++failures;
            } else {

                // 墨迹统计：白纸上的墨 = 任一通道明显低于白
                const auto inkStats = [](const QPixmap& page, int& percent, QRect& box) {
                    const QImage image = page.toImage().convertToFormat(QImage::Format_RGB32);
                    box = QRect();
                    qint64 painted = 0;
                    for (int y = 0; y < image.height(); ++y) {
                        const QRgb* line = reinterpret_cast<const QRgb*>(image.constScanLine(y));
                        for (int x = 0; x < image.width(); ++x) {
                            const QRgb pixel = line[x];
                            if (qRed(pixel) > 240 && qGreen(pixel) > 240 && qBlue(pixel) > 240) {
                                continue;
                            }
                            ++painted;
                            const QRect dot(x, y, 1, 1);
                            box = box.isNull() ? dot : box.united(dot);
                        }
                    }
                    percent = image.width() * image.height() > 0
                                  ? static_cast<int>(painted * 100
                                                     / (image.width() * image.height()))
                                  : 0;
                    return painted;
                };

                // A4 横向 @300 dpi：与真实纸面同尺寸，因此边距与内容的比例就是纸上的比例
                const QSize sheetSize(3508, 2480);

                const QPixmap page = renderToPixmap(*plotDoc, sheetSize);

                int inkPercent = 0;
                QRect inkBox;
                const qint64 inkPixels = inkStats(page, inkPercent, inkBox);
                const qint64 drawingPixels = inkPixels;
                const QRect& drawingBox = inkBox;

                const QString pageShot = dir.filePath(QStringLiteral("ui_print_page.png"));
                if (!page.save(pageShot)) {
                    out() << QStringLiteral("  出图页面保存失败: ") << pageShot << Qt::endl;
                    ++failures;
                } else {
                    out() << QStringLiteral("  已保存 %1  (%2×%3)")
                                 .arg(QFileInfo(pageShot).fileName())
                                 .arg(page.width())
                                 .arg(page.height())
                          << Qt::endl;
                }

                // 图元必须铺满可打印区：页面去掉 6 mm 边距后，内容还留 2% 余量，
                // 因此图纸内容宽度应当接近可打印区的九成以上
                const double plotWidth = sheetSize.width() - 2.0 * (6.0 * 300.0 / 25.4);
                const double fillRatio = drawingBox.isNull()
                                             ? 0.0
                                             : drawingBox.width() / plotWidth;
                // 墨迹占比只用来排除两个极端（整页空白 / 整页糊黑），不设下限——
                // 图纸疏密不同，稀疏的图占比本来就低；真正要钉住的是「铺满」
                const bool pageOk = drawingPixels > 0 && inkPercent <= 60;
                const bool filled = fillRatio >= 0.9;

                out() << QStringLiteral("  出图页面：墨迹 %1%（%2 像素），图元铺满可打印区 %3% → %4")
                             .arg(inkPercent)
                             .arg(inkPixels)
                             .arg(fillRatio * 100.0, 0, 'f', 1)
                             .arg(pageOk && filled ? QStringLiteral("正常")
                                                   : QStringLiteral("异常"))
                      << Qt::endl;
                if (!pageOk || !filled) ++failures;

                // ---- 出图比例：固定比例必须真的改变纸面大小 ----
                //
                //  判据是墨迹宽度与比例严格成反比。基准比例不能写死：不同图纸
                //  宽窄差别极大（样例 286 mm，实测 DWG 776 mm），1:2 对前者装得下、
                //  对后者本来就该分两页。因此基准取「刚好装进可打印区」的 2 的幂，
                //  再比较基准、两倍、四倍三档——三档都保证单页，倍半关系直接
                //  钉住换算本身。
                //
                //  例外：无界图元（构造线、射线）在任何比例下都会贯穿整张纸
                //  （AutoCAD 的行为一致），墨迹宽度因此恒为满幅，「与比例成反比」
                //  对含无界图元的图纸不成立——此时不量墨迹，也不计失败。
                bool hasUnbounded = false;
                for (EntityId id : plotDoc->drawOrder()) {
                    const Entity* entity = plotDoc->entity(id);
                    if (entity && entity->isUnbounded()) { hasUnbounded = true; break; }
                }
                {
                    const double pxPerMm = 300.0 / 25.4;
                    const double printableMm =
                        (sheetSize.width() - 2.0 * (6.0 * pxPerMm)) / pxPerMm;
                    // 文件单位按毫米计（DXF/DWG 的常见约定），内容宽度即纸面毫米
                    const double contentMm = plotExtents(*plotDoc).width();

                    int base = 1;
                    while (contentMm / base > printableMm * 0.9 && base < 1024) base *= 2;

                    const auto inkWidthFor = [&](double n) {
                        PlotOptions options;
                        options.scale.mode = PlotScale::Mode::Custom;
                        options.scale.custom = n;
                        const QPixmap page = renderToPixmap(*plotDoc, sheetSize, options);
                        int percent = 0;
                        QRect box;
                        inkStats(page, percent, box);
                        return box.isNull() ? 0.0 : static_cast<double>(box.width());
                    };

                    double w1 = 0.0, w2 = 0.0, w4 = 0.0;
                    if (!hasUnbounded) {
                        w1 = inkWidthFor(base);
                        w2 = inkWidthFor(base * 2.0);
                        w4 = inkWidthFor(base * 4.0);
                    }
                    const bool ratiosOk = hasUnbounded
                        || (w1 > 0.0 && w2 > 0.0 && w4 > 0.0
                            && std::abs(w2 / w1 - 0.5) < 0.02
                            && std::abs(w4 / w2 - 0.5) < 0.02);

                    // 顺手把缩小档的整页存下来，与「适应图纸」的整页并排就是
                    // 比例设置最直观的证据
                    PlotOptions halfOptions;
                    halfOptions.scale.mode = PlotScale::Mode::Custom;
                    halfOptions.scale.custom = base * 2.0;
                    const QString halfShot =
                        dir.filePath(QStringLiteral("ui_print_page_scaled.png"));
                    renderToPixmap(*plotDoc, sheetSize, halfOptions).save(halfShot);

                    out() << QStringLiteral("  出图比例：1:%1 / 1:%2 / 1:%3 墨迹宽 "
                                            "%4 / %5 / %6 px → %7")
                                 .arg(base)
                                 .arg(base * 2)
                                 .arg(base * 4)
                                 .arg(std::lround(w1))
                                 .arg(std::lround(w2))
                                 .arg(std::lround(w4))
                                 .arg(hasUnbounded
                                          ? QStringLiteral("含无界图元，不适用")
                                          : (ratiosOk ? QStringLiteral("严格成反比")
                                                      : QStringLiteral("换算有误")))
                          << Qt::endl;
                    if (!ratiosOk) ++failures;
                }

                // 「不打印」图层：取一个恰好压在出图范围边缘上的图元所属图层
                // （它的 maxX 决定了范围），关掉打印后范围必须缩小——
                // 这条判据是确定的，不像「墨迹变少」那样可能被浅色图元的可见性干扰。
                const Box2D extentsBefore = plotExtents(*plotDoc);
                QString edgeLayer;
                for (EntityId id : plotDoc->drawOrder()) {
                    const Entity* entity = plotDoc->entity(id);
                    if (!entity || !entity->isDisplayable(plotDoc->layers())) continue;
                    if (std::abs(entity->boundingBox().maxX - extentsBefore.maxX) < 1e-9) {
                        edgeLayer = entity->layerName();
                        break;
                    }
                }

                if (edgeLayer.isEmpty()) {
                    out() << QStringLiteral("  找不到决定出图范围的图层，跳过「不打印」验证。")
                          << Qt::endl;
                } else if (Layer* layer = plotDoc->layers().find(edgeLayer)) {
                    layer->setPlottable(false);
                    const Box2D extentsAfter = plotExtents(*plotDoc);

                    // 被排除的图元数：出图范围的缩小是判据，这个数只是让人看清
                    // 「关掉的确实是有点分量的那一层」。至于墨迹百分比——它不能跨
                    // 两次渲染比较：图元少了、范围小了，剩下的内容反而被放得更大，
                    // 墨迹可以变多，拿它当判据会得出反的结论。
                    std::size_t excludedCount = 0;
                    for (EntityId id : plotDoc->drawOrder()) {
                        const Entity* entity = plotDoc->entity(id);
                        if (entity && entity->layerName() == edgeLayer) ++excludedCount;
                    }

                    const bool excluded = extentsAfter.isValid()
                                          && extentsAfter.maxX < extentsBefore.maxX - 1e-9;
                    out() << QStringLiteral("  不打印图层「%1」：出图范围 maxX %2 → %3"
                                            "（该层 %4 个图元）→ %5")
                                 .arg(edgeLayer)
                                 .arg(extentsBefore.maxX, 0, 'g', 6)
                                 .arg(extentsAfter.isValid()
                                          ? QString::number(extentsAfter.maxX, 'g', 6)
                                          : QStringLiteral("无内容"))
                                 .arg(excludedCount)
                                 .arg(excluded ? QStringLiteral("已排除")
                                               : QStringLiteral("仍被画出"))
                          << Qt::endl;
                    if (!excluded) ++failures;

                    layer->setPlottable(true);
                    Q_UNUSED(drawingPixels);
                }

                // ---- 打印对话框：设置 + 预览 + 出口，一屏完成 ----
                //
                //  驱动方式刻意选「PDF 目标」：点「打印」后界面层只是把页面写进
                //  PDF 文件，绝不会把作业送到本机打印机上——这是离屏环境里能安全
                //  走完整条链路的唯一出口。断言有四段：对话框能开、比例改动经
                //  MainWindow::plotOptions() 生效并被持久化、PDF 真的落盘、
                //  「适应图纸」能再设回去。
                {
                    const QString pdfPath = dir.filePath(QStringLiteral("ui_print_page.pdf"));
                    QFile::remove(pdfPath);

                    bool dialogOpened = false;
                    int pageCount = 0;
                    int pageAfterJump = -1;
                    int spinMax = 0;
                    int spinValue = 0;

                    QTimer::singleShot(700, &window, [&]() {
                        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                        if (!dialog) return;
                        dialogOpened = true;

                        // 目标选 PDF 并填输出路径
                        for (QComboBox* combo : dialog->findChildren<QComboBox*>()) {
                            for (int i = 0; i < combo->count(); ++i) {
                                if (combo->itemText(i) == QStringLiteral("导出为 PDF")) {
                                    combo->setCurrentIndex(i);
                                }
                            }
                        }
                        if (auto* pathEdit = dialog->findChild<QLineEdit*>(
                                QStringLiteral("printDialog.pdfPath"))) {
                            pathEdit->setText(pdfPath);
                        }

                        // 比例选自定义 1:0.25（放大 4 倍）：样例图纸在 A4 横向下
                        // 一页放不下，预览必须给出多页，且页码栏能翻页
                        for (QRadioButton* radio : dialog->findChildren<QRadioButton*>()) {
                            if (radio->text().startsWith(QStringLiteral("自定义"))) {
                                radio->click();
                                break;
                            }
                        }
                        if (auto* value = dialog->findChild<QDoubleSpinBox*>()) {
                            value->setValue(0.25);
                        }

                        // dialog 按值捕获：它是外层 lambda 的局部变量，按引用捕获
                        // 是悬垂引用（实测段错误，而且崩在 grab 里，很难归因）
                        QTimer::singleShot(400, dialog, [&, dialog]() {
                            if (!capture(application, *dialog, dir.filePath(
                                    QStringLiteral("ui_print_dialog.png")))) {
                                ++failures;
                            }

                            // 预览控件汇报的页数：固定比例放大后必须 > 1 页，
                            // 且页码栏的范围与之一致（切换页码走 setCurrentPage）
                            if (auto* preview = dialog->findChild<QPrintPreviewWidget*>()) {
                                preview->updatePreview();
                                pageCount = preview->pageCount();

                                // 翻到第 3 页再回来，页码栏要跟上
                                preview->setCurrentPage(2);
                                pageAfterJump = preview->currentPage();
                                if (auto* spin = dialog->findChild<QSpinBox*>(
                                        QStringLiteral("printDialog.pageSpin"))) {
                                    spinMax = spin->maximum();
                                    spinValue = spin->value();
                                }
                            }

                            // 点「打印」：PDF 目标下这一步只是写文件
                            if (auto* button = dialog->findChild<QPushButton*>(
                                    QStringLiteral("printDialog.printButton"))) {
                                button->click();
                            }
                        });
                    });

                    printAction->trigger();
                    settle(application, 200);

                    const bool pdfWritten = QFileInfo(pdfPath).exists()
                                            && QFileInfo(pdfPath).size() > 2048;
                    const PlotOptions applied = window.plotOptions();
                    const bool scalePersisted =
                        applied.scale.mode == PlotScale::Mode::Custom
                        && std::abs(applied.scale.custom - 0.25) < 1e-9;
                    // 放大 4 倍后 A4 横向一页只装得下图纸的一部分，必须分页；
                    // 页码栏（spin 上限）与预览的实际页数一致，跳页后回到原页码
                    // 页码栏的 spin 只在用户操作（页码栏/按钮）时刷新；
                    // 这里直接调 setCurrentPage 属于程序化跳页，spin 不跟着走，
                    // 因此只断言页数、栏上限与跳页本身
                    const bool pagesOk = pageCount >= 2 && spinMax == pageCount
                                         && pageAfterJump == 2;

                    out() << QStringLiteral("  打印对话框：%1，比例 1:0.25 → %2 页"
                                            "（页码栏上限 %3），跳页 %4 → %5，"
                                            "比例 %6，PDF %7（%8 字节）")
                                 .arg(dialogOpened ? QStringLiteral("已打开并已截图")
                                                   : QStringLiteral("未能打开"),
                                      QString::number(pageCount),
                                      QString::number(spinMax),
                                      QString::number(pageAfterJump + 1),
                                      pagesOk ? QStringLiteral("分页与翻页正常")
                                              : QStringLiteral("分页或翻页异常"),
                                      scalePersisted ? QStringLiteral("已持久化")
                                                     : QStringLiteral("未生效"),
                                      pdfWritten ? QStringLiteral("已落盘")
                                                 : QStringLiteral("缺失"),
                                      QString::number(QFileInfo(pdfPath).size()))
                          << Qt::endl;
                    if (!dialogOpened || !pagesOk || !scalePersisted || !pdfWritten) {
                        ++failures;
                    }

                    // 恢复「适应图纸」：重开对话框 → 选适应图纸 → 打印（仍是 PDF，
                    // 不会碰真打印机）。这同时把比例设置恢复到默认，
                    // 后续阶段的断言不受这次改动影响。
                    bool restoredDialog = false;
                    QTimer::singleShot(500, &window, [&]() {
                        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                        if (!dialog) return;
                        restoredDialog = true;
                        // PDF 输出必须显式指到工具的目录：这个对话框的默认 PDF 路径
                        // 是「当前图纸所在目录 + 图纸名.pdf」，不指定就会把文件写进
                        // 仓库的 sample/（实测写过 lycad_sample_2000.dxf.pdf）。
                        // 工具的产物一律只落在输出目录里。
                        if (auto* pathEdit = dialog->findChild<QLineEdit*>(
                                QStringLiteral("printDialog.pdfPath"))) {
                            pathEdit->setText(
                                dir.filePath(QStringLiteral("ui_print_restore.pdf")));
                        }
                        for (QRadioButton* radio : dialog->findChildren<QRadioButton*>()) {
                            if (radio->text().startsWith(QStringLiteral("适应图纸"))) {
                                radio->click();
                                break;
                            }
                        }
                        QTimer::singleShot(300, dialog, [dialog]() {
                            if (auto* button = dialog->findChild<QPushButton*>(
                                    QStringLiteral("printDialog.printButton"))) {
                                button->click();
                            }
                        });
                    });
                    printAction->trigger();
                    settle(application, 200);

                    const bool restored =
                        window.plotOptions().scale.mode == PlotScale::Mode::FitToPaper;
                    out() << QStringLiteral("  打印对话框：恢复适应图纸 → %1")
                                 .arg(restored && restoredDialog
                                          ? QStringLiteral("已恢复")
                                          : QStringLiteral("未恢复"))
                          << Qt::endl;
                    if (!restored || !restoredDialog) ++failures;
                }
            }
        }

        // ---- 快照 2d：文件菜单——另存为挑版本 / 导出 PDF ----
        //
        //  盯两条界面约定：
        //    · DXF 目标版本不再是常驻菜单项，普通保存一律走默认版本，
        //      版本选择被收进「另存为」流程里（只有 DXF 才问）；
        //    · 「导出」只剩 PDF 一种去向，且真的能落下一份 PDF。
        {
            const auto findAction = [&window](const QString& text) -> QAction* {
                for (QAction* action : window.findChildren<QAction*>()) {
                    if (action->text() == text) return action;
                }
                return nullptr;
            };

            QAction* saveAsAction = findAction(QStringLiteral("另存为(&A)…"));
            QAction* exportAction = findAction(QStringLiteral("导出(&E)…"));
            QAction* staleExport = findAction(QStringLiteral("导出为其它格式(&E)…"));
            QAction* staleVersion = findAction(QStringLiteral("DXF 目标版本…"));

            const bool menuOk = saveAsAction && exportAction && !staleExport && !staleVersion;
            out() << QStringLiteral("  文件菜单：另存为 %1，导出 %2，"
                                    "旧「导出为其它格式」%3，旧「DXF 目标版本」%4 → %5")
                         .arg(saveAsAction ? QStringLiteral("有") : QStringLiteral("缺失"),
                              exportAction ? QStringLiteral("有") : QStringLiteral("缺失"),
                              staleExport ? QStringLiteral("仍在") : QStringLiteral("已移除"),
                              staleVersion ? QStringLiteral("仍在") : QStringLiteral("已移除"),
                              menuOk ? QStringLiteral("就绪") : QStringLiteral("不符"))
                  << Qt::endl;
            if (!menuOk) ++failures;

            // ---- 导出 PDF：动作 → 文件对话框 → 渲染 → 落盘 ----
            if (exportAction) {
                // 比较与填写都用绝对路径：QFileDialog 返回的是绝对路径
                const QString pdfPath =
                    QFileInfo(dir.filePath(QStringLiteral("ui_export.pdf"))).absoluteFilePath();
                QFile::remove(pdfPath);

                const bool handled = driveSaveDialog(
                    application, window,
                    [&](QFileDialog& dialog) {
                        pointSaveDialogAt(dialog, pdfPath);
                        // 目标路径真的生效才放行；否则宁可让看门狗兜底，
                        // 也不能接受对话框默认的目录（那是当前图纸所在处，
                        // 会把 PDF 写进仓库的 sample/ —— 实测发生过）
                        return dialog.selectedFiles().value(0) == pdfPath;
                    },
                    [&]() { exportAction->trigger(); });

                QFile pdf(pdfPath);
                const bool opened = pdf.open(QIODevice::ReadOnly);
                const QByteArray head = opened ? pdf.read(5) : QByteArray();
                const qint64 size = opened ? pdf.size() : 0;
                // 真 PDF 的判据：有 %PDF- 魔数，且不是只有个空壳
                const bool pdfOk =
                    handled && opened && head == QByteArray("%PDF-") && size > 1024;
                out() << QStringLiteral("  导出 PDF：对话框 %1，落盘 %2 字节，头部「%3」→ %4")
                             .arg(handled ? QStringLiteral("已驱动")
                                          : QStringLiteral("未出现"),
                                  QString::number(size), QString::fromLatin1(head),
                                  pdfOk ? QStringLiteral("正常") : QStringLiteral("异常"))
                      << Qt::endl;
                if (!pdfOk) ++failures;
            }

            // ---- 另存为：文件类型列表就是版本选择器 ----
            if (saveAsAction) {
                const QString dxfPath =
                    QFileInfo(dir.filePath(QStringLiteral("ui_saveas.dxf"))).absoluteFilePath();
                QFile::remove(dxfPath);

                QStringList typeItems;
                QString defaultType;

                const bool handled = driveSaveDialog(
                    application, window,
                    [&](QFileDialog& dialog) {
                        // 过滤器只在第一轮记录：后面几轮是我们自己改的，不是初值
                        if (typeItems.isEmpty()) {
                            typeItems = dialog.nameFilters();
                            defaultType = dialog.selectedNameFilter();
                        }

                        // 挑一个非默认的版本档，回头核对文件头里写的到底是不是它
                        const QString target =
                            QStringLiteral("AutoCAD 2004 DXF 文件 (*.dxf)");
                        if (typeItems.contains(target)
                            && dialog.selectedNameFilter() != target) {
                            dialog.selectNameFilter(target);
                        }
                        pointSaveDialogAt(dialog, dxfPath);
                        return dialog.selectedNameFilter() == target
                               && dialog.selectedFiles().value(0) == dxfPath;
                    },
                    [&]() { saveAsAction->trigger(); });

                QString acadVer;
                QFile saved(dxfPath);
                if (saved.open(QIODevice::ReadOnly)) {
                    const QList<QByteArray> lines = saved.readAll().split('\n');
                    for (int i = 0; i + 3 < lines.size(); ++i) {
                        if (lines.at(i).trimmed() == "9"
                            && lines.at(i + 1).trimmed() == "$ACADVER"
                            && lines.at(i + 2).trimmed() == "1") {
                            acadVer = QString::fromLatin1(lines.at(i + 3).trimmed());
                            break;
                        }
                    }
                }

                // 逐版本一条 DXF 条目（共六档，R12 已不再可写），默认选中最新档（AC1032），
                // 且选中的那一档必须真的写进文件头的 $ACADVER；
                // DWG 条目也要点明写出版本（恒为 R2000），否则与逐版本的
                // DXF 条目并列时说明不了会写成什么
                int dxfTypes = 0;
                bool dwgNamed = false;
                for (const QString& type : typeItems) {
                    if (type.contains(QStringLiteral("(*.dxf)"))) ++dxfTypes;
                    if (type == QStringLiteral("AutoCAD 2000 DWG 文件 (*.dwg)")) dwgNamed = true;
                }
                const bool versionOk = handled && dxfTypes == 6 && dwgNamed
                                       && defaultType.startsWith(QStringLiteral("AutoCAD 2018"))
                                       && acadVer == QStringLiteral("AC1018");
                out() << QStringLiteral("  另存为：类型列表 DXF %1 档，DWG 条目 %2，默认「%3」，"
                                        "选 2004 后文件头为 %4 → %5")
                             .arg(dxfTypes)
                             .arg(dwgNamed ? QStringLiteral("AutoCAD 2000 DWG 文件")
                                           : QStringLiteral("未按版本命名"),
                                  defaultType.isEmpty() ? QStringLiteral("未弹出对话框")
                                                        : defaultType,
                                  acadVer.isEmpty() ? QStringLiteral("未写出") : acadVer,
                                  versionOk ? QStringLiteral("正常") : QStringLiteral("异常"))
                      << Qt::endl;
                if (!versionOk) ++failures;
            }
        }

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

        // ---- 快照 5：特性面板的「文字」组（字高 / 旋转 / 对齐）------------------
        //
        // 面板只发意图信号，信号到文档的那几根线在 MainWindow 里。这里从面板控件
        // **反向驱动**一遍，验证的正是那几根线：改控件要真的改到文档、要进撤销栈。
        {
            Document* propertyDoc = canvas->document();
            auto* panel = window.findChild<PropertyPanel*>();
            auto* heightBox = panel ? panel->findChild<QDoubleSpinBox*>(
                                          QStringLiteral("propertyPanel.textHeight"))
                                    : nullptr;
            auto* rotationBox = panel ? panel->findChild<QDoubleSpinBox*>(
                                            QStringLiteral("propertyPanel.textRotation"))
                                      : nullptr;
            auto* hAlignBox = panel ? panel->findChild<QComboBox*>(
                                          QStringLiteral("propertyPanel.textHAlign"))
                                    : nullptr;
            auto* vAlignBox = panel ? panel->findChild<QComboBox*>(
                                          QStringLiteral("propertyPanel.textVAlign"))
                                    : nullptr;
            auto* fontBox = panel ? panel->findChild<QComboBox*>(
                                        QStringLiteral("propertyPanel.textFont"))
                                  : nullptr;
            auto* textGroup = panel ? panel->findChild<QGroupBox*>(
                                          QStringLiteral("propertyPanel.textGroup"))
                                    : nullptr;

            if (!propertyDoc || !heightBox || !rotationBox || !hAlignBox || !vAlignBox
                || !fontBox || !textGroup) {
                out() << QStringLiteral("  找不到特性面板的文字属性控件。") << Qt::endl;
                ++failures;
            } else {
                // 选一条文字：面板应显示它的字高与旋转，并露出「文字」组
                const EntityId target = [propertyDoc]() -> EntityId {
                    for (EntityId id : propertyDoc->drawOrder()) {
                        const Entity* e = propertyDoc->entity(id);
                        if (e && e->type() == EntityType::Text) return id;
                    }
                    return kInvalidEntityId;
                }();

                if (target == kInvalidEntityId) {
                    out() << QStringLiteral("  文档中没有文字图元，跳过特性面板验证。") << Qt::endl;
                } else {
                    propertyDoc->selection().setIds({ target });
                    settle(application, 120);

                    const auto* before =
                        static_cast<const TextEntity*>(propertyDoc->entity(target));
                    const double originalHeight = before->height();
                    const double originalRotation = before->rotation();
                    const TextEntity::HAlign originalHAlign = before->hAlign();
                    const TextEntity::VAlign originalVAlign = before->vAlign();
                    const QString originalFont = before->fontStyle();
                    const int records = propertyDoc->undoStack()->index();

                    if (!textGroup->isVisible()) {
                        out() << QStringLiteral("  选中文字后「文字」属性组仍未显示。") << Qt::endl;
                        ++failures;
                    }

                    // 目标值必须与当前值**确实不同**：这个工具要能跑在任意图纸上，
                    // 而被选中的文字本来就可能是 90° 或居中——设成同一个值不会产生
                    // 撤销记录，后面的「+4/+5 条记录」断言就会莫名其妙地失败。
                    const double targetHeight =
                        std::abs(originalHeight - 4.0) > 1e-6 ? 4.0 : 6.0;
                    const double targetRotationDeg =
                        std::abs(radToDeg(originalRotation) - 90.0) > 0.5 ? 90.0 : 30.0;
                    const TextEntity::HAlign targetHAlign =
                        originalHAlign == TextEntity::HAlign::Center ? TextEntity::HAlign::Right
                                                                    : TextEntity::HAlign::Center;
                    const TextEntity::VAlign targetVAlign =
                        originalVAlign == TextEntity::VAlign::Top ? TextEntity::VAlign::Baseline
                                                                  : TextEntity::VAlign::Top;

                    // 字体：目标同样要与当前值不同——当前已有字体就切回「（默认）」
                    // （数据是空串），本来就是默认则挑第一款系统字体。
                    const QString targetFont =
                        originalFont.isEmpty() && fontBox->count() > 1
                            ? fontBox->itemData(1).toString()
                            : QString();
                    const bool fontChanges = targetFont != originalFont;

                    // 逐项驱动：字高、旋转、水平对齐、垂直对齐、字体
                    heightBox->setValue(targetHeight);
                    rotationBox->setValue(targetRotationDeg);          // 面板给的是度
                    hAlignBox->setCurrentIndex(
                        hAlignBox->findData(static_cast<int>(targetHAlign)));
                    vAlignBox->setCurrentIndex(
                        vAlignBox->findData(static_cast<int>(targetVAlign)));
                    if (fontChanges) {
                        fontBox->setCurrentIndex(fontBox->findData(targetFont));
                    }
                    settle(application, 120);

                    const auto* after =
                        static_cast<const TextEntity*>(propertyDoc->entity(target));
                    const bool heightOk = std::abs(after->height() - targetHeight) < 1e-6;
                    const bool rotationOk =
                        std::abs(after->rotation() - degToRad(targetRotationDeg)) < 1e-6;
                    const bool hAlignOk = after->hAlign() == targetHAlign;
                    const bool vAlignOk = after->vAlign() == targetVAlign;
                    const bool fontOk = !fontChanges || after->fontStyle() == targetFont;
                    const bool undoOk =
                        propertyDoc->undoStack()->index() == records + 4 + (fontChanges ? 1 : 0);

                    out() << QStringLiteral("  特性面板文字组：字高 %1 / 旋转 %2 / 水平对齐 %3 / "
                                            "垂直对齐 %4 / 字体 %5 / 撤销记录 %6")
                                 .arg(heightOk ? QStringLiteral("通过") : QStringLiteral("失败"),
                                      rotationOk ? QStringLiteral("通过") : QStringLiteral("失败"),
                                      hAlignOk ? QStringLiteral("通过") : QStringLiteral("失败"),
                                      vAlignOk ? QStringLiteral("通过") : QStringLiteral("失败"),
                                      fontOk ? QStringLiteral("通过") : QStringLiteral("失败"))
                                 .arg(undoOk ? QStringLiteral("+%1").arg(4 + (fontChanges ? 1 : 0))
                                             : QStringLiteral("异常"))
                          << Qt::endl;
                    if (!heightOk || !rotationOk || !hAlignOk || !vAlignOk || !fontOk
                        || !undoOk) {
                        ++failures;
                    }

                    // 撤销回去，后面的截图与原始图纸一致
                    while (propertyDoc->undoStack()->index() > records) {
                        propertyDoc->undoStack()->undo();
                    }
                    const auto* restored =
                        static_cast<const TextEntity*>(propertyDoc->entity(target));
                    if (std::abs(restored->height() - originalHeight) > 1e-9
                        || std::abs(restored->rotation() - originalRotation) > 1e-9
                        || restored->hAlign() != originalHAlign
                        || restored->vAlign() != originalVAlign
                        || restored->fontStyle() != originalFont) {
                        out() << QStringLiteral("  撤销后文字属性没有回到原值。") << Qt::endl;
                        ++failures;
                    }

                    // 选中非文字图元时「文字」组应整组隐藏
                    const EntityId other = [propertyDoc]() -> EntityId {
                        for (EntityId id : propertyDoc->drawOrder()) {
                            const Entity* e = propertyDoc->entity(id);
                            if (e && e->type() != EntityType::Text) return id;
                        }
                        return kInvalidEntityId;
                    }();
                    if (other != kInvalidEntityId) {
                        propertyDoc->selection().setIds({ other });
                        settle(application, 120);
                        const bool hidden = !textGroup->isVisible();
                        out() << QStringLiteral("  非文字选择时「文字」组隐藏: %1")
                                     .arg(hidden ? QStringLiteral("通过")
                                                 : QStringLiteral("失败"))
                              << Qt::endl;
                        if (!hidden) ++failures;
                    }
                    propertyDoc->selection().clear();
                    settle(application, 80);
                }
            }
        }

        // ---- 快照 6：功能区自检 + 深色主题 ----
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

        // 面板要能一眼看出「哪里是一块」：面板底色必须与功能区底色有可分辨的差异。
        // 早先面板与功能区同色，只靠面板之间一条 1px 竖线区分，按钮一多就看不出
        // 分组了。这里直接取渲染结果的两个像素比对，比核对调色板更接近用户所见。
        // 深浅两套主题各查一次（深色主题在下面切换）。
        const auto reportPanelContrast = [&](const QString& theme) {
            const QList<RibbonPanel*> panels = ribbon->findChildren<RibbonPanel*>();
            if (panels.size() < 2) return;

            const QImage shot = ribbon->grab().toImage();
            // 面板左上角往里三个像素：避开 1px 描边，落在面板自身的底色上
            const QRect first = panels.at(0)->geometry();
            const QRect second = panels.at(1)->geometry();
            const QPoint inside(first.left() + first.width() / 2, first.top() + 3);
            const QPoint gap(second.left() - 2, second.top() + 3);

            const int insideGray = qGray(shot.pixel(inside));
            const int gapGray = qGray(shot.pixel(gap));
            const int delta = std::abs(insideGray - gapGray);
            const bool distinct = delta >= 3;

            out() << QStringLiteral("  面板分块（%1）：面板底色灰度 %2，功能区底色灰度 %3（差 %4）→ %5")
                         .arg(theme)
                         .arg(insideGray)
                         .arg(gapGray)
                         .arg(delta)
                         .arg(distinct ? QStringLiteral("看得出分块")
                                       : QStringLiteral("与背景同色，看不出分块"))
                  << Qt::endl;
            if (!distinct) ++failures;
        };
        reportPanelContrast(QStringLiteral("浅色"));

        // 同一个面板里，小按钮必须与大按钮**共用一条顶线**：次要命令竖排成列时
        // 一律靠顶部排列，而不是在面板里垂直居中。
        //
        // 这条曾经是错的：每列上下各留一段空白做居中，居中量又随面板不同
        // （面板里大按钮个数不同，分到的高度不同），结果同一条工具栏上
        // 「撤销/重做」比「新建/打开/保存」低一大截、「矩形/点」又是另一个高度。
        // 用坐标钉住它，比看截图可靠。
        {
            int worstDelta = 0;
            QString worstPanel;
            for (RibbonPanel* panel : ribbon->findChildren<RibbonPanel*>()) {
                int largeTop = std::numeric_limits<int>::max();
                int smallTop = std::numeric_limits<int>::max();
                for (QToolButton* button : panel->findChildren<QToolButton*>()) {
                    if (button->text().isEmpty()) continue;
                    const int top = button->mapTo(&window, QPoint(0, 0)).y();
                    if (button->toolButtonStyle() == Qt::ToolButtonTextUnderIcon) {
                        largeTop = std::min(largeTop, top);
                    } else {
                        smallTop = std::min(smallTop, top);
                    }
                }
                if (largeTop == std::numeric_limits<int>::max()
                    || smallTop == std::numeric_limits<int>::max()) {
                    continue;      // 只有大按钮或只有小按钮的面板无需比较
                }
                const int delta = std::abs(smallTop - largeTop);
                if (delta > worstDelta) {
                    worstDelta = delta;
                    const auto* label =
                        panel->findChild<QLabel*>(QStringLiteral("ribbonPanelTitle"));
                    worstPanel = label ? label->text() : QStringLiteral("?");
                }
            }

            const bool aligned = worstDelta <= 3;      // 允许布局取整的几像素误差
            out() << QStringLiteral("  功能区顶线：小按钮与大按钮顶边最大相差 %1 px%2 → %3")
                         .arg(worstDelta)
                         .arg(worstPanel.isEmpty() ? QString()
                                                   : QStringLiteral("（%1）").arg(worstPanel),
                              aligned ? QStringLiteral("靠顶对齐")
                                      : QStringLiteral("有按钮浮在中间"))
                  << Qt::endl;
            if (!aligned) ++failures;
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

            // 深色下再查一次面板分块：两套主题的面板底色是各自取的，
            // 只查浅色会让深色那一档悄悄退回「与功能区同色」。
            reportPanelContrast(QStringLiteral("深色"));

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

        // ---- 快照 7：图层与特性下拉框就近取景 ----
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
