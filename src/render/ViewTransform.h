// =============================================================================
//  LyCAD - render/ViewTransform.h
//  世界坐标 <-> 设备坐标 的变换（视图缩放与平移的数学核心）。
//
//  坐标约定
//    世界坐标：右手系，Y 轴向上，单位默认毫米。这是 CAD 的通行约定，
//              也是 DXF 的约定，因此从文件读入的坐标无需翻转。
//    设备坐标：Qt 的窗口像素坐标，原点在左上角，Y 轴向下。
//
//    两者的换算为「平移 + 各向同性缩放 + Y 轴翻转」：
//        device.x = (world.x - origin.x) * scale
//        device.y = (origin.y - world.y) * scale
//    其中 origin 是设备原点 (0,0) 所对应的世界坐标，scale 为「像素/世界单位」。
//    该形式刻意只保留两个参数，使所有视图操作都能写出闭式解，无需求逆矩阵。
// =============================================================================
#pragma once

#include "core/Types.h"

#include <QSize>

namespace lycad {

class ViewTransform {
public:
    /// 缩放倍率的边界：1e-6 ~ 1e7 像素/单位，覆盖从零件细节到城市级平面图
    static constexpr double kMinScale = 1e-6;
    static constexpr double kMaxScale = 1e7;

    ViewTransform() = default;

    // ---- 基本换算 ------------------------------------------------------------
    Point2D toDevice(const Point2D& world) const noexcept;
    Point2D toWorld(const Point2D& device) const noexcept;

    double scale() const noexcept { return m_scale; }
    double worldPerDevice() const noexcept;
    double worldLengthToDevice(double worldLength) const noexcept;
    double deviceLengthToWorld(double deviceLength) const noexcept;

    /// 把「屏幕像素长度」换算为世界长度，用于拾取容差、离散精度等
    double pixelsToWorld(double pixels) const noexcept { return deviceLengthToWorld(pixels); }

    const Point2D& origin() const noexcept { return m_origin; }

    // ---- 视图操作 ------------------------------------------------------------
    /// 以 deviceAnchor 为不动点设置缩放倍率（滚轮缩放的核心）
    void setScale(double pixelsPerWorld, const Point2D& deviceAnchor);

    /// 相对缩放
    void zoomAt(const Point2D& deviceAnchor, double factor);

    /// 按设备像素平移视图（中键拖拽）
    void panByDevice(const Vector2D& deviceDelta) noexcept;

    /// 按世界单位平移视图
    void panByWorld(const Vector2D& worldDelta) noexcept;

    /// 把世界范围铺满视口。marginRatio 为四周留白占视口的比例。
    void fitTo(const Box2D& worldBox, const QSize& viewportSize, double marginRatio = 0.06);

    /// 把指定世界点置于视口中心（不改变缩放）
    void centerOn(const Point2D& worldPoint, const QSize& viewportSize);

    /// 恢复为 1:1 缩放、原点在左上角
    void reset();

    // ---- 可见范围 ------------------------------------------------------------
    /// 当前视口对应的世界范围（用于剔除与网格绘制）
    Box2D visibleWorld(const QSize& viewportSize) const;

private:
    /// 设备原点对应的世界坐标
    Point2D m_origin{ 0.0, 0.0 };
    /// 像素 / 世界单位
    double m_scale = 1.0;
};

} // namespace lycad
