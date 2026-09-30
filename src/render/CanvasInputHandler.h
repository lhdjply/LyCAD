// =============================================================================
//  LyCAD - render/CanvasInputHandler.h
//  画布输入处理接口。
//
//  单独成文件而非塞进 CadCanvas.h，是为了让交互层只依赖这个纯接口，
//  而不必包含 QWidget 与整个画布实现——依赖面越小，替换与测试越容易。
//
//  实现者（interact::ToolManager）返回 true 表示事件已被消费；
//  返回 false 时画布会执行自身的默认行为（例如中键平移）。
// =============================================================================
#pragma once

#include "render/CanvasTypes.h"
#include "render/EntityRenderer.h"

#include <QString>
#include <Qt>

class QKeyEvent;
class QPainter;

namespace lycad {

class CanvasInputHandler {
public:
    virtual ~CanvasInputHandler() = default;

    virtual bool handleMousePress(const CanvasEvent& event) = 0;
    virtual bool handleMouseMove(const CanvasEvent& event) = 0;
    virtual bool handleMouseRelease(const CanvasEvent& event) = 0;
    virtual bool handleMouseDoubleClick(const CanvasEvent& event) = 0;
    virtual bool handleKeyPress(QKeyEvent* event) = 0;

    /// 在实体层之上绘制工具预览（橡皮筋、选择框、夹点等）
    virtual void paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context) = 0;

    /// 命令行提示文字；返回空字符串时命令行显示默认的「命令:」
    virtual QString promptText() const = 0;

    /// 画布光标形状
    virtual Qt::CursorShape cursorShape() const { return Qt::CrossCursor; }
};

} // namespace lycad
