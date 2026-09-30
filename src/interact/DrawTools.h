// =============================================================================
//  LyCAD - interact/DrawTools.h
//  绘制类工具：直线、圆、圆弧、多段线、点、矩形。
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

} // namespace lycad
