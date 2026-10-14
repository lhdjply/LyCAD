// =============================================================================
//  LyCAD - interact/SelectionTool.h
//  默认工具：对象选择与夹点编辑。
//
//  它是 ToolManager 的兜底工具，没有命令运行时始终在线。因此它的交互
//  必须与「无命令」状态一致：左键在空白处拖拽即为框选，而不是什么都不做。
//
//  窗选与交叉选遵循 AutoCAD 的方向约定：
//      自左向右拖拽 —— 窗选（完全包含才选中）
//      自右向左拖拽 —— 交叉选（接触到即选中）
//  两种模式在画布上的颜色与线型不同，用户无需记忆即可分辨。
// =============================================================================
#pragma once

#include "core/TableEntity.h"
#include "interact/Tool.h"

namespace lycad {

class SelectionTool : public Tool {
public:
    explicit SelectionTool(ToolContext& context);

    QString name() const override;
    QString displayName() const override;
    QString prompt() const override;

    /// 悬停在表格的行列界线上时换成「可拖动」的调整光标
    Qt::CursorShape cursorShape() const override;

    /// 不在空白处独占左键，从而允许框选
    bool capturesLeftButton() const override { return false; }

    bool mousePress(const CanvasEvent& event) override;
    bool mouseMove(const CanvasEvent& event) override;
    bool mouseRelease(const CanvasEvent& event) override;
    bool mouseDoubleClick(const CanvasEvent& event) override;
    bool keyPress(QKeyEvent* event) override;

    void paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context) override;
    void deactivate() override;

private:
    enum class Mode {
        Idle,
        BoxSelect,
        GripDrag,       ///< 拖角点：整体等比缩放
        EdgeDrag        ///< 拖表格的行列界线：改行高 / 列宽（内部界线只挪线）
    };

    /// 在光标处查找选中图元的夹点，命中返回 true
    bool findGripUnderCursor(const Point2D& world, double tolerance,
                             EntityId& entityId, int& gripIndex) const;

    /// 在光标处查找表格的行列界线（**不必先选中表格**）的命中结果
    struct EdgeTarget {
        EntityId            id = kInvalidEntityId;
        TableEntity::EdgeHit hit;
        explicit operator bool() const noexcept { return id != kInvalidEntityId; }
    };
    EdgeTarget findTableEdgeUnderCursor(const Point2D& world, double tolerance) const;

    /// 刷新「光标是否压在表格界线上」的状态，供 cursorShape 使用
    void updateHoverEdge(const Point2D& world);

    /// 提交夹点拖拽（生成一条可撤销的替换记录）
    void commitGripDrag();

    /// 提交界线拖拽（同上，但走的是改行高 / 列宽那条路）
    void commitEdgeDrag();

    Mode    m_mode = Mode::Idle;
    bool    m_additive = false;         ///< 按住 Shift：加入选择集而非替换

    Point2D m_pressWorld;
    Point2D m_cursorWorld;

    /// 悬停反馈：压在列界线（左右拖）或行界线（上下拖）上
    bool    m_hoverColumnEdge = false;
    bool    m_hoverRowEdge = false;

    EntityId m_gripEntity = kInvalidEntityId;
    int      m_gripIndex = -1;
    Point2D  m_gripOriginal;

    /// 界线拖拽：正在拖的那一条（列界线左右拖、行界线上下拖）
    EntityId    m_edgeEntity = kInvalidEntityId;
    bool        m_edgeVertical = false;
    std::size_t m_edgeIndex = 0;
    QString     m_edgeLabel;
    Point2D     m_edgeOriginal;         ///< 按下时线的中点：只用于"有没有真的拖动"
    bool        m_edgeArmed = false;    ///< 光标已越过拖动阈值，才真的开始改尺寸
};

} // namespace lycad
