// =============================================================================
//  LyCAD - render/EntityRenderer.h
//  图元绘制。
//
//  渲染后端说明
//    采用 QPainter 光栅后端而非 OpenGL。理由：
//      · 二维 CAD 的瓶颈在几何离散精度与图元数量，而非 GPU 填充率；
//      · 光栅后端在远程桌面 / 虚拟机 / 无 GPU 环境下行为一致，
//        这对工程软件的可部署性比峰值帧率更重要；
//      · 细线段的抗锯齿质量在两种后端下相当，而 QPainter 的实现更稳定。
//    若后续需要接入 GPU，只需替换本文件与 CadCanvas 的绘制循环，
//    文档与交互层不受影响。
//
//  精度策略
//    · 直线、圆、圆弧使用解析图元直接绘制，不做折线近似，任意缩放下均精确；
//    · 多段线的圆弧段（凸度）按「像素矢高」自适应离散，缩得越深分段越细；
//    · 当圆/圆弧的屏幕半径超过阈值时，其曲率已低于一个像素，改用切线近似，
//      既避免 Qt 定点坐标溢出，也省去无意义的巨量分段。
// =============================================================================
#pragma once

#include "core/Entity.h"
#include "render/CanvasTypes.h"
#include "render/ViewTransform.h"

#include <QColor>
#include <QPainterPath>
#include <QRectF>
#include <QString>

class QPainter;

namespace lycad {

namespace EntityRenderer {

/// 屏幕半径超过该值（像素）时改用切线近似
constexpr double kMaxExactRadiusPixels = 1.0e5;

/// 单次绘制的上下文
struct DrawContext {
    const ViewTransform* view = nullptr;
    const DisplaySettings* settings = nullptr;
    /// 当前视口（设备坐标），用于图元剔除
    QRectF viewport;

    bool isValid() const { return view != nullptr && settings != nullptr; }
};

/// 图元在设备坐标下的绘制路径（用于包围盒估算与高亮描边）。
///
/// clip 为当前视口（设备坐标），无界图元（构造线、射线）必须靠它裁出可见段：
/// 它们没有有限包围盒，路径形状完全取决于「裁到哪个矩形」。有界图元忽略该参数；
/// 无界图元在 clip 无效时返回空路径（宁可不画，也不画一条贯穿宇宙的线）。
QPainterPath toDevicePath(const Entity& entity, const ViewTransform& view, const QRectF& clip);

/// 设备坐标下的包围矩形；图元范围无效时返回空矩形。
/// 无界图元（构造线、射线）恒返回空矩形——它们的设备范围无定义。
QRectF deviceBounds(const Entity& entity, const ViewTransform& view);

/**
 * 绘制单个图元。
 * @param color         已解析的最终颜色（随层/随块均已处理完毕）
 * @param lineweightMm  最终线宽（毫米）；<= 0 表示按最细线绘制
 * @param linetypeName  最终线型名（如 "DASHED"）；空或 CONTINUOUS 表示实线
 */
void draw(QPainter& painter, const Entity& entity, const DrawContext& context,
          const QColor& color, double lineweightMm, const QString& linetypeName);

/// 绘制点实体的标记（十字 + 圆点），尺寸固定为像素，不随缩放变化
void drawPointMarker(QPainter& painter, const Point2D& devicePos, const QColor& color,
                     double sizePixels);

/// 按图元类型返回其「最小可见」的曲线离散矢高（世界单位）
double adaptiveSagitta(const DrawContext& context);

} // namespace EntityRenderer

} // namespace lycad
