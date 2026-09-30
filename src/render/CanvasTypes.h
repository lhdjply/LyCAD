// =============================================================================
//  LyCAD - render/CanvasTypes.h
//  渲染与交互之间共享的轻量数据类型。
//
//  放在 render 而非 interact，是因为画布需要绘制捕捉标记与工具预览，
//  而工具需要画布的坐标换算能力——这些类型是两者唯一的耦合面。
//  交互模块依赖渲染模块，反之不成立。
// =============================================================================
#pragma once

#include "core/Types.h"

#include <QColor>
#include <QPoint>
#include <QSize>
#include <QString>
#include <Qt>

#include <optional>

namespace lycad {

// -----------------------------------------------------------------------------
//  对象捕捉
// -----------------------------------------------------------------------------
enum class SnapKind {
    None,
    Endpoint,       ///< 端点
    Midpoint,       ///< 中点
    Center,         ///< 圆心
    Quadrant,       ///< 象限点
    Intersection,   ///< 交点
    Perpendicular,  ///< 垂足
    Tangent,        ///< 切点
    Nearest,        ///< 最近点
    Grid,           ///< 栅格吸附
    Ortho,          ///< 正交投影
    Polar,          ///< 极轴追踪
    NodePoint       ///< 点实体
};

/// 捕捉结果
struct SnapResult {
    Point2D position;
    SnapKind kind = SnapKind::None;
    QString label;              ///< 画布上显示的单字提示，如「端」「中」「心」

    bool isValid() const { return kind != SnapKind::None; }
};

QString snapKindName(SnapKind kind);
/// 捕捉标记上显示的短标签（1-2 个汉字）
QString snapKindShortLabel(SnapKind kind);
/// 捕捉标记的默认配色
QColor snapKindColor(SnapKind kind);

// -----------------------------------------------------------------------------
//  画布鼠标事件（设备坐标 + 已换算的世界坐标）
// -----------------------------------------------------------------------------
struct CanvasEvent {
    Point2D devicePos;                          ///< 相对画布左上角的像素坐标
    Point2D worldPos;                           ///< 由 devicePos 换算得到的原始世界坐标
    QPoint devicePosRounded;                    ///< 取整后的设备坐标，供命中判断使用

    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons = Qt::NoButton;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;

    /// 当前缩放下的拾取容差（世界单位），等于 N 像素对应的世界长度
    double pickToleranceWorld = 0.0;
    /// 拾取框边长（像素），供画布绘制
    double pickBoxPixels = 6.0;

    bool hasModifier(Qt::KeyboardModifier m) const { return modifiers.testFlag(m); }
};

// -----------------------------------------------------------------------------
//  显示设置
// -----------------------------------------------------------------------------
struct DisplaySettings {
    // 背景与配色
    QColor background{ 0xFF, 0xFF, 0xFF };
    QColor gridMajorColor{ 0xD0, 0xD0, 0xD0 };
    QColor gridMinorColor{ 0xEC, 0xEC, 0xEC };
    QColor axisXColor{ 0xC8, 0x30, 0x30 };
    QColor axisYColor{ 0x30, 0x80, 0x30 };
    QColor selectionColor{ 0x1E, 0x88, 0xE5 };
    QColor highlightColor{ 0xF5, 0xA6, 0x23 };
    QColor crosshairColor{ 0x88, 0x88, 0x88 };
    QColor snapColor{ 0x2E, 0xA0, 0x43 };

    // 网格
    bool   showGrid = true;
    bool   showAxes = true;
    /// 网格间距自适应：随缩放自动选取 1/2/5 × 10^n 的档位
    bool   adaptiveGrid = true;
    double gridSpacing = 10.0;      ///< 关闭自适应时使用的固定间距（世界单位）
    int    gridSubdivisions = 5;    ///< 主格之间的细分数

    // 绘制质量
    bool   antialias = true;
    /// 是否按线宽绘制。默认关闭——与 AutoCAD 的默认行为一致，
    /// 屏幕上线框恒为 1 像素，图纸的信息层级通过颜色与线型表达。
    bool   showLineweight = false;
    double minLinewidthPixels = 1.0;
    double maxLinewidthPixels = 12.0;
    /// 弧线离散允许的最大像素矢高，决定圆/弧在多大缩放下仍显平滑
    double maxSagittaPixels = 0.35;

    // 光标与拾取
    bool   showCrosshair = true;
    double crosshairSizePixels = 12.0;
    double pickBoxPixels = 6.0;

    /// 深色主题下 7 号色（黑/白）需要反相显示
    bool   themeIsDark = false;

    /// 是否绘制「已选中的图元」
    bool   showSelection = true;
};

} // namespace lycad
