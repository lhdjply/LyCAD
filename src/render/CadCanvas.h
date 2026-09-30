// =============================================================================
//  LyCAD - render/CadCanvas.h
//  CAD 绘图画布。
//
//  职责边界
//    画布负责「把文档画出来」与「把鼠标键盘事件标准化为世界坐标事件」，
//    但它完全不理解任何命令语义——不认识直线工具，也不知道什么是窗选。
//    所有语义由 CanvasInputHandler 的实现（interact 模块的 ToolManager）承担。
//
//    这样划分的好处是可测试性与可替换性：换一套交互方案（脚本驱动、
//    自动化测试、触屏手势）只需要替换处理器，画布与文档均无需改动。
//
//  视图操作（缩放、平移）属于「观察」而非「编辑」，因此由画布自身内建，
//    不进入撤销栈，也不需经过工具。
// =============================================================================
#pragma once

#include "core/Document.h"
#include "core/Types.h"
#include "render/CanvasInputHandler.h"
#include "render/CanvasTypes.h"
#include "render/EntityRenderer.h"
#include "render/ViewTransform.h"

#include <QWidget>

#include <optional>
#include <vector>

class QKeyEvent;
class QMouseEvent;
class QWheelEvent;
class QPainter;

namespace lycad {

class CadCanvas : public QWidget {
    Q_OBJECT

public:
    explicit CadCanvas(QWidget* parent = nullptr);
    ~CadCanvas() override;

    // ---- 关联对象 ------------------------------------------------------------
    void setDocument(Document* document);
    Document* document() const noexcept { return m_document; }

    void setInputHandler(CanvasInputHandler* handler);
    CanvasInputHandler* inputHandler() const noexcept { return m_handler; }

    // ---- 视图与显示 ----------------------------------------------------------
    ViewTransform& view() noexcept { return m_view; }
    const ViewTransform& view() const noexcept { return m_view; }

    DisplaySettings& display() noexcept { return m_display; }
    const DisplaySettings& display() const noexcept { return m_display; }

    void applyTheme(bool dark);

    /// 当前缩放下的拾取容差（世界单位）
    double pickTolerance() const;

    // ---- 视图操作 ------------------------------------------------------------
    void zoomIn();
    void zoomOut();
    /// 缩放至全部图形范围；图为空时回到默认视图
    void zoomExtents();
    /// 缩放至当前选择集范围；无选择时等价于 zoomExtents()
    void zoomToSelection();
    /// 缩放到指定世界范围
    void zoomToBox(const Box2D& worldBox);
    /// 以视口中心为锚点设置缩放倍率
    void setZoomScale(double pixelsPerWorldUnit);
    void centerOn(const Point2D& worldPoint);

    // ---- 捕捉标记 ------------------------------------------------------------
    void setSnapResult(const std::optional<SnapResult>& snap);
    const std::optional<SnapResult>& snapResult() const noexcept { return m_snap; }

    // ---- 拾取（供工具层复用） -------------------------------------------------
    /// 拾取最上层命中的图元；未命中返回 kInvalidEntityId
    EntityId pickEntity(const Point2D& world, double tolerance) const;

    /// 按框选择。crossing 为 true 时对应「交叉选择」，false 对应「窗选」
    std::vector<EntityId> pickEntitiesInBox(const Box2D& worldBox, bool crossing) const;

    /// 绘制事件中使用的上下文（工具预览需要一致的坐标与设置）
    EntityRenderer::DrawContext drawContext() const;

    /// 请求重绘（合并同一事件循环内的多次请求）
    void requestRepaint();

signals:
    void mouseWorldPositionChanged(const lycad::Point2D& world, const lycad::Point2D& device);
    void viewChanged();
    void cursorLeftCanvas();
    /// 右键点击（设备坐标），由主窗口弹出上下文菜单
    void contextMenuRequested(const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void drawBackground(QPainter& painter);
    void drawGrid(QPainter& painter);
    void drawAxes(QPainter& painter);
    void drawEntities(QPainter& painter);
    void drawSelectionHighlight(QPainter& painter);
    void drawGrips(QPainter& painter);
    void drawSnapMarker(QPainter& painter);
    void drawCrosshair(QPainter& painter);

    /// 组装标准化的画布事件
    CanvasEvent makeEvent(const QPointF& devicePos, Qt::MouseButton button,
                          Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers) const;

    /// 自适应的网格主间距（世界单位）
    double majorGridSpacing() const;

    void emitViewChanged();

    Document* m_document = nullptr;
    CanvasInputHandler* m_handler = nullptr;

    ViewTransform  m_view;
    DisplaySettings m_display;

    std::optional<SnapResult> m_snap;

    // 视图平移状态（中键拖拽）
    bool    m_panning = false;
    QPointF m_lastPanPos;

    // 光标位置（设备与世界坐标），用于绘制十字光标与状态栏显示
    bool    m_cursorInside = false;
    Point2D m_cursorDevice;
    Point2D m_cursorWorld;

    bool m_repaintPending = false;
};

} // namespace lycad
