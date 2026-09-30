// =============================================================================
//  LyCAD - interact/ModifyTools.h
//  编辑类工具：移动、复制、旋转、缩放、镜像、删除。
//
//  统一的三段式流程
//      选择对象  ->  指定基点  ->  指定目标点（或输入数值）
//    这一流程与 AutoCAD 的 MOVE/COPY/ROTATE/SCALE/MIRROR 完全一致，
//    因此抽成公共基类 ModifyToolBase，各命令只需实现「如何构造变换」。
//
//  选择阶段刻意复用文档的选择集与标准拾取交互，而不是另起一套：
//    这样用户在命令执行前若已选好对象，可直接跳过选择阶段；
//    命令行与夹点等既有交互也无需为编辑命令重复实现。
// =============================================================================
#pragma once

#include "interact/Tool.h"

#include <optional>
#include <vector>

namespace lycad {

// -----------------------------------------------------------------------------
//  公共基类
// -----------------------------------------------------------------------------
class ModifyToolBase : public Tool {
public:
    explicit ModifyToolBase(ToolContext& context);

    QString prompt() const override;

    bool mousePress(const CanvasEvent& event) override;
    bool mouseMove(const CanvasEvent& event) override;
    bool keyPress(QKeyEvent* event) override;
    bool textInput(const QString& text) override;
    void paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context) override;
    void activate() override;
    void deactivate() override;

protected:
    enum class Phase {
        Selecting,      ///< 选择对象
        BasePoint,      ///< 指定基点
        TargetPoint     ///< 指定目标点 / 输入数值
    };

    /// 目标阶段的提示文字
    virtual QString targetPrompt() const = 0;

    /// 在目标点处执行操作。
    /// 返回 true 表示命令完成；返回 false 表示继续等待下一个目标点（如 COPY）。
    virtual bool applyAt(const Point2D& target) = 0;

    /// 目标阶段是否接受数值输入（旋转角度、缩放比例）
    virtual bool handleTargetScalar(double value)
    {
        Q_UNUSED(value);
        return false;
    }

    /// 数值输入的名称，用于提示与错误信息
    virtual QString targetScalarName() const { return QString(); }

    /// 目标阶段的橡皮筋预览
    virtual void paintTargetPreview(QPainter& painter,
                                    const EntityRenderer::DrawContext& context)
    {
        Q_UNUSED(painter);
        Q_UNUSED(context);
    }

    const std::vector<EntityId>& targets() const noexcept { return m_targets; }
    const Point2D& basePoint() const noexcept { return m_base; }

    /// 由子类在构造时设置，用于提示文字中的命令名
    void setActionName(const QString& name) { m_actionName = name; }

    Phase m_phase = Phase::Selecting;
    std::vector<EntityId> m_targets;
    Point2D m_base;
    ResolvedPoint m_cursor;
    bool m_hasCursor = false;

private:
    QString m_actionName;
};

// -----------------------------------------------------------------------------
//  移动
// -----------------------------------------------------------------------------
class MoveTool : public ModifyToolBase {
public:
    explicit MoveTool(ToolContext& context);
    QString name() const override;
    QString displayName() const override;

protected:
    QString targetPrompt() const override;
    bool applyAt(const Point2D& target) override;
    void paintTargetPreview(QPainter& painter,
                            const EntityRenderer::DrawContext& context) override;
};

// -----------------------------------------------------------------------------
//  复制（可连续放置多份，回车结束）
// -----------------------------------------------------------------------------
class CopyTool : public ModifyToolBase {
public:
    explicit CopyTool(ToolContext& context);
    QString name() const override;
    QString displayName() const override;

protected:
    QString targetPrompt() const override;
    bool applyAt(const Point2D& target) override;
    void paintTargetPreview(QPainter& painter,
                            const EntityRenderer::DrawContext& context) override;

private:
    int m_copyCount = 0;
};

// -----------------------------------------------------------------------------
//  旋转
// -----------------------------------------------------------------------------
class RotateTool : public ModifyToolBase {
public:
    explicit RotateTool(ToolContext& context);
    QString name() const override;
    QString displayName() const override;

protected:
    QString targetPrompt() const override;
    bool applyAt(const Point2D& target) override;
    bool handleTargetScalar(double degrees) override;
    QString targetScalarName() const override;
    void paintTargetPreview(QPainter& painter,
                            const EntityRenderer::DrawContext& context) override;

private:
    bool rotateBy(double radians);
};

// -----------------------------------------------------------------------------
//  缩放
// -----------------------------------------------------------------------------
class ScaleTool : public ModifyToolBase {
public:
    explicit ScaleTool(ToolContext& context);
    QString name() const override;
    QString displayName() const override;

protected:
    QString targetPrompt() const override;
    bool applyAt(const Point2D& target) override;
    bool handleTargetScalar(double factor) override;
    QString targetScalarName() const override;
    void paintTargetPreview(QPainter& painter,
                            const EntityRenderer::DrawContext& context) override;
    void activate() override;

private:
    bool scaleBy(double factor);

    /// 首次移动鼠标时记录的距离，作为「1 倍」的参照
    double m_referenceLength = 0.0;
};

// -----------------------------------------------------------------------------
//  镜像
// -----------------------------------------------------------------------------
class MirrorTool : public ModifyToolBase {
public:
    explicit MirrorTool(ToolContext& context);
    QString name() const override;
    QString displayName() const override;

protected:
    QString targetPrompt() const override;
    bool applyAt(const Point2D& target) override;
    void paintTargetPreview(QPainter& painter,
                            const EntityRenderer::DrawContext& context) override;

private:
    /// 镜像轴的第一点；为空表示下一步应指定轴的起点
    std::optional<Point2D> m_axisFirst;
};

// -----------------------------------------------------------------------------
//  删除
// -----------------------------------------------------------------------------
class EraseTool : public Tool {
public:
    explicit EraseTool(ToolContext& context);

    QString name() const override;
    QString displayName() const override;
    QString prompt() const override;

    bool mousePress(const CanvasEvent& event) override;
    bool keyPress(QKeyEvent* event) override;
    bool textInput(const QString& text) override;
    void activate() override;
    void deactivate() override;
};

} // namespace lycad
