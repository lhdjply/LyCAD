// =============================================================================
//  LyCAD - interact/AnnotationTools.h
//  注释类绘制工具：文字、填充、尺寸标注。
//
//  交互约定
//    · 文字：先在命令行输入内容，再点击插入点（与命令行优先的输入模型一致，
//      也便于自动化测试用确定性的输入序列驱动）；
//    · 填充：连续取点构成闭合边界，回车 / 右键闭合后生成填充；
//    · 线性/对齐标注：三条定义点（两条界线原点 + 尺寸线位置）；
//    · 半径/直径标注：先拾取圆或圆弧，再指定文字位置；
//    · 坐标标注：被测点 + 引线端点（度量轴由引线方向决定，与 AutoCAD 一致）；
//    · 弧长标注：先拾取圆弧，再指定尺寸弧经过的位置。
// =============================================================================
#pragma once

#include "core/DimensionEntity.h"
#include "interact/Tool.h"

#include <vector>

namespace lycad {

// -----------------------------------------------------------------------------
//  文字
// -----------------------------------------------------------------------------
class DrawTextTool final : public Tool {
public:
    explicit DrawTextTool(ToolContext& context);

    QString name() const override;
    QString displayName() const override;
    QString prompt() const override;

    bool mousePress(const CanvasEvent& event) override;
    bool mouseMove(const CanvasEvent& event) override;
    bool textInput(const QString& text) override;
    void paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context) override;
    void deactivate() override;

private:
    QString m_text;
    ResolvedPoint m_cursor;
    bool m_hasCursor = false;
};

// -----------------------------------------------------------------------------
//  填充
// -----------------------------------------------------------------------------
class DrawHatchTool final : public Tool {
public:
    explicit DrawHatchTool(ToolContext& context);

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
    void commit();
    bool ready() const noexcept { return m_vertices.size() >= 3; }

    std::vector<Point2D> m_vertices;
    ResolvedPoint m_cursor;
    bool m_hasCursor = false;
};

// -----------------------------------------------------------------------------
//  线性 / 对齐标注的公共三步流程：界线原点 1 -> 界线原点 2 -> 尺寸线位置
// -----------------------------------------------------------------------------
class LinearDimensionToolBase : public Tool {
public:
    explicit LinearDimensionToolBase(ToolContext& context) : Tool(context) {}

    bool mousePress(const CanvasEvent& event) final;
    bool mouseMove(const CanvasEvent& event) final;
    bool keyPress(QKeyEvent* event) final;
    bool textInput(const QString& text) final;
    void paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context) final;
    void deactivate() final;

protected:
    virtual DimensionEntity::DimType dimType() const = 0;

    /// 步骤提示（第一步 / 第二步共用，第三步固定）
    virtual QString originPrompt() const = 0;

    std::size_t pointCount() const noexcept { return m_points.size(); }

private:
    void commit();

    std::vector<Point2D> m_points;
    ResolvedPoint m_cursor;
    bool m_hasCursor = false;
};

class DrawDimAlignedTool final : public LinearDimensionToolBase {
public:
    using LinearDimensionToolBase::LinearDimensionToolBase;

    QString name() const override;
    QString displayName() const override;
    QString prompt() const override;

protected:
    DimensionEntity::DimType dimType() const override
    { return DimensionEntity::DimType::Aligned; }
    QString originPrompt() const override;
};

class DrawDimLinearTool final : public LinearDimensionToolBase {
public:
    using LinearDimensionToolBase::LinearDimensionToolBase;

    QString name() const override;
    QString displayName() const override;
    QString prompt() const override;

protected:
    DimensionEntity::DimType dimType() const override
    { return DimensionEntity::DimType::Linear; }
    QString originPrompt() const override;
};

// -----------------------------------------------------------------------------
//  半径 / 直径标注的公共两步流程：拾取圆（弧） -> 指定文字位置
// -----------------------------------------------------------------------------
class RadialDimensionToolBase : public Tool {
public:
    explicit RadialDimensionToolBase(ToolContext& context) : Tool(context) {}

    bool mousePress(const CanvasEvent& event) final;
    bool mouseMove(const CanvasEvent& event) final;
    bool keyPress(QKeyEvent* event) final;
    bool textInput(const QString& text) final;
    void paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context) final;
    void deactivate() final;

protected:
    virtual bool diametric() const = 0;

    /// 第一步的提示（拾取圆 / 圆弧）
    virtual QString pickPrompt() const = 0;

    bool hasCircle() const noexcept { return m_hasCircle; }

private:
    void commit();

    // 拾取阶段拷贝目标圆的几何参数，不持有文档实体指针，
    // 避免提交时文档结构变化导致悬垂引用
    Point2D m_center;
    Point2D m_pickPoint;        ///< 圆上拾取点
    double  m_radius = 0.0;
    bool    m_hasCircle = false;

    ResolvedPoint m_cursor;
    bool m_hasCursor = false;
};

class DrawDimRadialTool final : public RadialDimensionToolBase {
public:
    using RadialDimensionToolBase::RadialDimensionToolBase;

    QString name() const override;
    QString displayName() const override;
    QString prompt() const override;

protected:
    bool diametric() const override { return false; }
    QString pickPrompt() const override;
};

class DrawDimDiametricTool final : public RadialDimensionToolBase {
public:
    using RadialDimensionToolBase::RadialDimensionToolBase;

    QString name() const override;
    QString displayName() const override;
    QString prompt() const override;

protected:
    bool diametric() const override { return true; }
    QString pickPrompt() const override;
};

// -----------------------------------------------------------------------------
//  坐标标注：被测点 -> 引线端点
//
//  度量轴不在第一步就问，而是由**引线方向**定：引线偏竖直量 X 坐标，
//  偏水平量 Y 坐标——AutoCAD 的 DIMORDINATE 就是这么定的，也与显示几何一致
//  （引线沿测量轴的垂直方向拉开）。移动鼠标时预览会跟着换轴。
// -----------------------------------------------------------------------------
class DrawDimOrdinateTool final : public Tool {
public:
    explicit DrawDimOrdinateTool(ToolContext& context) : Tool(context) {}

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
    void commit();

    std::optional<Point2D> m_feature;   ///< 被测点（13）
    ResolvedPoint m_cursor;
    bool m_hasCursor = false;
};

// -----------------------------------------------------------------------------
//  弧长标注：拾取圆弧 -> 指定尺寸弧位置
// -----------------------------------------------------------------------------
class DrawDimArcTool final : public Tool {
public:
    explicit DrawDimArcTool(ToolContext& context) : Tool(context) {}

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
    void commit();

    // 拾取阶段拷贝圆弧的几何参数，不持有文档实体指针
    Point2D m_center;
    double  m_radius = 0.0;
    double  m_startAngle = 0.0;
    double  m_endAngle = 0.0;
    bool    m_hasArc = false;

    ResolvedPoint m_cursor;
    bool m_hasCursor = false;
};

} // namespace lycad
