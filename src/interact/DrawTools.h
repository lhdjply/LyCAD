// =============================================================================
//  LyCAD - interact/DrawTools.h
//  绘制类工具：直线、圆、圆弧、椭圆、多段线、点、矩形。
//
//  共同的交互范式（与 AutoCAD 一致）
//    · 第一个点之后的操作都有「基准点」，因此可以启用相对坐标与极轴追踪；
//    · 橡皮筋预览在鼠标移动时实时绘制，而不是靠频繁创建/删除临时图元；
//    · 回车 / 空格结束，Esc 取消，U 撤销上一步；
//    · 命令行可随时以坐标形式输入精确点位。
// =============================================================================
#pragma once

#include "interact/Tool.h"

#include <vector>

namespace lycad {

// -----------------------------------------------------------------------------
//  直线：连续绘制，每两点生成一条独立的 LINE 实体
// -----------------------------------------------------------------------------
class DrawLineTool : public Tool {
public:
    explicit DrawLineTool(ToolContext& context);

    QString name() const override;
    QString displayName() const override;
    QString prompt() const override;

    bool mousePress(const CanvasEvent& event) override;
    bool mouseMove(const CanvasEvent& event) override;
    bool keyPress(QKeyEvent* event) override;
    bool textInput(const QString& text) override;
    void paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context) override;
    void deactivate() override;

private:
    void createSegment(const Point2D& from, const Point2D& to);
    void undoLastSegment();

    std::vector<Point2D> m_vertices;
    std::vector<EntityId> m_created;
    ResolvedPoint m_cursor;
    bool m_hasCursor = false;
};

// -----------------------------------------------------------------------------
//  圆：圆心+半径（默认）、三点、两点（直径）、直接输入直径
// -----------------------------------------------------------------------------
class DrawCircleTool : public Tool {
public:
    explicit DrawCircleTool(ToolContext& context);

    QString name() const override;
    QString displayName() const override;
    QString prompt() const override;

    bool mousePress(const CanvasEvent& event) override;
    bool mouseMove(const CanvasEvent& event) override;
    bool keyPress(QKeyEvent* event) override;
    bool textInput(const QString& text) override;
    void paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context) override;
    void deactivate() override;

private:
    enum class Mode {
        CenterRadius,    ///< 圆心 + 半径点
        ThreePoint,      ///< 圆周上三点
        TwoPoint,        ///< 直径两端点
        DiameterTyped    ///< 等待输入直径数值
    };

    void commitCircle(const Point2D& center, double radius);
    void reset();

    Mode m_mode = Mode::CenterRadius;
    std::vector<Point2D> m_points;
    ResolvedPoint m_cursor;
    bool m_hasCursor = false;
};

// -----------------------------------------------------------------------------
//  圆弧：三点（默认）与 圆心-起点-终点
// -----------------------------------------------------------------------------
class DrawArcTool : public Tool {
public:
    explicit DrawArcTool(ToolContext& context);

    QString name() const override;
    QString displayName() const override;
    QString prompt() const override;

    bool mousePress(const CanvasEvent& event) override;
    bool mouseMove(const CanvasEvent& event) override;
    bool keyPress(QKeyEvent* event) override;
    bool textInput(const QString& text) override;
    void paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context) override;
    void deactivate() override;

private:
    enum class Mode {
        ThreePoint,       ///< 起点 / 弧上点 / 终点
        CenterStartEnd    ///< 圆心 / 起点 / 终点
    };

    void commit();
    void reset();

    Mode m_mode = Mode::ThreePoint;
    std::vector<Point2D> m_points;
    ResolvedPoint m_cursor;
    bool m_hasCursor = false;
};

// -----------------------------------------------------------------------------
//  多段线：连续取点，回车结束、C 闭合、U 撤销上一顶点
// -----------------------------------------------------------------------------
class DrawPolylineTool : public Tool {
public:
    explicit DrawPolylineTool(ToolContext& context);

    QString name() const override;
    QString displayName() const override;
    QString prompt() const override;

    bool mousePress(const CanvasEvent& event) override;
    bool mouseMove(const CanvasEvent& event) override;
    bool keyPress(QKeyEvent* event) override;
    bool textInput(const QString& text) override;
    void paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context) override;
    void deactivate() override;

private:
    void commit(bool closed);

    std::vector<Point2D> m_vertices;
    ResolvedPoint m_cursor;
    bool m_hasCursor = false;
};

// -----------------------------------------------------------------------------
//  点：连续落点
// -----------------------------------------------------------------------------
class DrawPointTool : public Tool {
public:
    explicit DrawPointTool(ToolContext& context);

    QString name() const override;
    QString displayName() const override;
    QString prompt() const override;

    bool mousePress(const CanvasEvent& event) override;
    bool mouseMove(const CanvasEvent& event) override;
    bool keyPress(QKeyEvent* event) override;
    bool textInput(const QString& text) override;
    void deactivate() override;

private:
    ResolvedPoint m_cursor;
    bool m_hasCursor = false;
    int  m_placed = 0;
};

// -----------------------------------------------------------------------------
//  矩形：两个对角点生成闭合多段线
// -----------------------------------------------------------------------------
class DrawRectangleTool : public Tool {
public:
    explicit DrawRectangleTool(ToolContext& context);

    QString name() const override;
    QString displayName() const override;
    QString prompt() const override;

    bool mousePress(const CanvasEvent& event) override;
    bool mouseMove(const CanvasEvent& event) override;
    bool keyPress(QKeyEvent* event) override;
    bool textInput(const QString& text) override;
    void paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context) override;
    void deactivate() override;

private:
    std::optional<Point2D> m_first;
    ResolvedPoint m_cursor;
    bool m_hasCursor = false;
};

// -----------------------------------------------------------------------------
//  椭圆：轴端点法（默认）与中心点法
//
//  默认与 AutoCAD 的 ELLIPSE 一致：先给出一条轴的**两个端点**，再给另一条
//  半轴的长度——第三步取的是光标到那条轴线的**垂直距离**，而不是第二个角点。
//  这一点与矩形（第二个角点）手感相近却语义不同，提示语里必须写明
//  「另一条半轴长度」，否则用户会按矩形的手感去点，得到一个永远偏小的椭圆。
//
//  选项 C 切换为中心点法：中心 → 轴端点 → 另一条半轴长度。
//
//  方向反馈：椭圆有两个方向要定，两处都给了吸附与指示——
//    · 定轴方向时，方向吸附到**上下左右**四个正方向（光圈内才吸，与对象捕捉
//      同一像素口径），并画出过基点的长追踪线；
//    · 定另一条半轴时，光标沿垂直方向投影到该轴上（标记「半轴」），同时画出
//      另一条半轴与它的两个端点方块，让人一眼看清椭圆会伸到哪里。
//  吸附只作用于鼠标；命令行输入的数字一律按原值使用，不参与方向吸附。
// -----------------------------------------------------------------------------
class DrawEllipseTool : public Tool {
public:
    explicit DrawEllipseTool(ToolContext& context);

    QString name() const override;
    QString displayName() const override;
    QString prompt() const override;

    bool mousePress(const CanvasEvent& event) override;
    bool mouseMove(const CanvasEvent& event) override;
    bool keyPress(QKeyEvent* event) override;
    bool textInput(const QString& text) override;
    void paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context) override;
    void deactivate() override;

private:
    enum class Mode {
        AxisEnds,   ///< 轴的两个端点 + 另一条半轴长度
        Center      ///< 中心 + 轴的端点 + 另一条半轴长度
    };

    /// 已确定的这条轴：中心与半轴向量（两种模式统一成同一份数据）
    struct Axis {
        Point2D  center;
        Vector2D halfAxis;
    };

    /// 一次点位求解：捕捉后的点，外加「是否被方向吸附拉到正方向上」
    struct StepPoint {
        ResolvedPoint point;
        bool directionSnapped = false;
    };

    /// 前两个点是否已凑齐一条可用的轴；退化（长度为 0）时返回空
    std::optional<Axis> currentAxis() const;

    /// 对象捕捉 + 定轴方向时的正方向吸附
    StepPoint resolveStep(const Point2D& raw) const;

    /// 参考点对应的「另一条半轴长度」（= 到轴线的垂直距离）；轴未定或长度为 0 时为空
    std::optional<double> otherHalfLengthFrom(const Point2D& reference) const;

    /// 用「另一条半轴长度」收尾并落盘；失败时给出提示并复位
    void commit(const Point2D& reference);

    /// 按当前状态与一个参考点拼出预览图元；条件不足时返回空
    std::unique_ptr<Entity> makePreview(const Point2D& reference) const;

    void reset();

    Mode m_mode = Mode::AxisEnds;
    std::vector<Point2D> m_points;      ///< AxisEnds: 两个轴端点；Center: 中心 + 轴端点
    ResolvedPoint m_cursor;
    bool m_hasCursor = false;
    bool m_directionSnapped = false;    ///< 当前光标是否被吸到正方向（决定是否画追踪线）
};

} // namespace lycad
