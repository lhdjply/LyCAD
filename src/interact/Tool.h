// =============================================================================
//  LyCAD - interact/Tool.h
//  工具基类。
//
//  工具被建模为「由输入事件驱动的小型状态机」，而命令行的提示文字就是
//  当前状态的直接投影——这与 AutoCAD 的交互模型一致：命令行永远告诉
//  用户下一步要做什么，因此 prompt() 是工具的核心接口之一，而非附属品。
//
//  生命周期
//      activate()  -> 循环接收输入事件 -> deactivate()
//    工具通过 ctx().finishCommand() 请求结束；管理器负责销毁与状态复位。
//    被 Esc 取消时同样走 deactivate()，因此工具应把「清理临时状态」放在
//    deactivate() 而非析构函数中，以便区分正常结束与异常中断。
// =============================================================================
#pragma once

#include "interact/ToolContext.h"
#include "render/CanvasTypes.h"
#include "render/EntityRenderer.h"

#include <QString>

class QKeyEvent;
class QPainter;

namespace lycad {

class Tool {
public:
    explicit Tool(ToolContext& context);
    virtual ~Tool();

    // ---- 元信息 --------------------------------------------------------------
    /// 命令名（大写，如 "LINE"），用于命令行调用
    virtual QString name() const = 0;
    /// 中文显示名（如 "直线"）
    virtual QString displayName() const = 0;
    /// 当前步骤的提示文字，会显示在命令行
    virtual QString prompt() const = 0;

    // ---- 生命周期 ------------------------------------------------------------
    virtual void activate() {}
    virtual void deactivate() {}

    // ---- 输入事件 ------------------------------------------------------------
    virtual bool mousePress(const CanvasEvent& event);
    virtual bool mouseMove(const CanvasEvent& event);
    virtual bool mouseRelease(const CanvasEvent& event);
    virtual bool mouseDoubleClick(const CanvasEvent& event);
    virtual bool keyPress(QKeyEvent* event);
    /// 命令行文本输入：坐标、半径、选项字母等
    virtual bool textInput(const QString& text);

    /// 在实体层之上绘制工具自身的预览
    virtual void paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context);

    /// 工具是否需要独占鼠标左键。选择类工具返回 false 时，左键在空白处的
    /// 拖拽会退化为选择框，而绘制类工具返回 true 会始终接管左键。
    virtual bool capturesLeftButton() const { return true; }

    /// 该工具生效时画布使用的光标形状
    virtual Qt::CursorShape cursorShape() const { return Qt::CrossCursor; }

    /// 是否正在执行中（用于主窗口决定「空格 = 重复上一命令」的行为）
    bool isRunning() const noexcept { return m_running; }

protected:
    ToolContext& ctx() const noexcept { return m_context; }

    /// 请求结束当前命令
    void finish();
    /// 提示文字已变化，通知管理器刷新命令行
    void notifyPromptChanged();

    /// 便捷：把点解析为经过捕捉与约束的最终位置
    ResolvedPoint resolve(const Point2D& raw,
                          const std::optional<Point2D>& base = std::nullopt) const;

    /// 便捷：向命令行输出信息
    void message(const QString& text) const;

    /// 便捷：请求重绘画布
    void repaint() const;

    /// 把当前图层作为新图元的默认归属
    QString currentLayerName() const;

    // ---- 命令行输入解析 --------------------------------------------------------
    /// 解析坐标输入（绝对/相对/极坐标）。失败时已向命令行输出原因。
    std::optional<Point2D> parsePointInput(const QString& text,
                                           const std::optional<Point2D>& lastPoint) const;

    /// 解析标量输入（长度、角度、比例、半径……）。失败时已向命令行输出原因。
    std::optional<double> parseScalarInput(const QString& text, const QString& what) const;

    /// 文本是否命中某个选项关键字（大小写不敏感）
    static bool matchesOption(const QString& text, const QString& option);

private:
    ToolContext& m_context;
    bool m_running = true;
};

} // namespace lycad
