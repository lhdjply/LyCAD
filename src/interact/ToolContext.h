// =============================================================================
//  LyCAD - interact/ToolContext.h
//  工具运行上下文：工具与文档、画布、捕捉引擎之间的唯一连接点。
//
//  为什么需要它
//    工具只应关心「我要一个点」「我要一个选择集」，而不应分别持有文档指针、
//    画布指针、捕捉设置、正交开关、状态栏回调。把这些收拢为一个上下文对象后：
//      · 工具构造函数保持单参数，扩展交互选项不需要改动任何工具；
//      · 捕捉、正交、极轴的组合逻辑集中在一处，不会在各工具里各写一遍。
// =============================================================================
#pragma once

#include "core/Types.h"
#include "interact/SnapEngine.h"
#include "render/CanvasTypes.h"

#include <QString>

#include <optional>

namespace lycad {

class Document;
class CadCanvas;
class ToolManager;
class LayerManager;

/**
 * 一次点位输入的求解结果。
 * 与 SnapResult 同构，但额外表达「这是经过正交/极轴约束的位置」，
 * 以便画布给出对应的视觉反馈。
 */
struct ResolvedPoint {
    Point2D  position;
    SnapKind kind = SnapKind::None;
    QString  label;

    /// 是否命中了某个真实的几何特征（而非仅仅落在栅格或约束方向上）
    bool isObjectSnap() const
    {
        switch (kind) {
        case SnapKind::Endpoint:
        case SnapKind::Midpoint:
        case SnapKind::Center:
        case SnapKind::Quadrant:
        case SnapKind::Intersection:
        case SnapKind::Perpendicular:
        case SnapKind::Tangent:
        case SnapKind::Nearest:
        case SnapKind::NodePoint:
            return true;
        default:
            return false;
        }
    }

    SnapResult toSnapResult() const { return SnapResult{ position, kind, label }; }
};

struct ToolContext {
    Document*    document = nullptr;
    CadCanvas*   canvas = nullptr;
    ToolManager* manager = nullptr;

    // ---- 交互模式（由主窗口的状态开关驱动） ------------------------------------
    bool   orthoMode = false;
    bool   polarTracking = false;
    double polarStepDeg = 45.0;
    SnapSettings snap;

    bool isValid() const { return document != nullptr && canvas != nullptr; }

    /// 拾取容差（世界单位），等于「拾取框像素」在当前缩放下的世界长度
    double pickToleranceWorld() const;

    /// 捕捉光圈半径（世界单位）
    double apertureWorld() const;

    /**
     * 求解一个输入点：对象捕捉优先，未命中时再施加正交 / 极轴约束。
     * base 为空表示当前步骤不需要基准点（如绘制的第一个点）。
     */
    ResolvedPoint resolvePoint(const Point2D& raw,
                               const std::optional<Point2D>& base = std::nullopt) const;

    /// 在当前光标处显示捕捉标记
    void setSnapMarker(const std::optional<SnapResult>& snap) const;

    void requestRepaint() const;

    /// 向命令行 / 状态栏输出一行信息
    void message(const QString& text) const;

    /// 结束当前命令（回到默认的选择工具）
    void finishCommand() const;

    /// 可选：工具预览需要在文档中临时插入图元时使用的辅助
    /// （例如多段线绘制过程中的实时线段）
    const LayerManager* layers() const;
};

} // namespace lycad
