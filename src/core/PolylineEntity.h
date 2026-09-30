// =============================================================================
//  LyCAD - core/PolylineEntity.h
//  多段线图元（DXF: LWPOLYLINE / POLYLINE）
//
//  凸度（bulge）约定 —— 与 DXF 规范一致
//    · 每个顶点的 bulge 描述「自该顶点到下一顶点」这一段的弯曲程度。
//    · bulge = tan(包含角 / 4)，正值为逆时针，负值为顺时针；0 表示直线段。
//    · bulge = 1 对应半圆。
//  保留 bulge 的意义在于：读写 DXF 时不丢失圆弧段信息，实现几何无损往返。
// =============================================================================
#pragma once

#include "core/Entity.h"

namespace lycad {

/// 多段线顶点
struct PolylineVertex {
    Point2D point;
    double  bulge = 0.0;
    double  startWidth = 0.0;    ///< 本段起点宽度（DXF 40），0 = 无
    double  endWidth = 0.0;      ///< 本段终点宽度（DXF 41），0 = 无
};

/// 多段线的一段（由相邻两顶点与起始顶点的 bulge 推导）
struct PolylineSegment {
    Point2D start;
    Point2D end;
    bool    isArc = false;
    Point2D center;          ///< 仅圆弧段有效
    double  radius = 0.0;
    double  startAngle = 0.0;    ///< 弧度
    double  endAngle = 0.0;
    double  bulge = 0.0;

    /// 逆时针跨度（已按 bulge 符号还原为真实行进方向），值域 (0, 2π]
    double spanAngle() const;
    /// 判断某角度是否落在本段圆弧上。
    ///
    /// 凸度的符号决定绕行方向，因此不能仅用「起始角 + 跨度」判断：
    /// 凸度为负（顺时针）时，弧体位于弦的另一侧，需反向度量。
    bool containsAngle(double radians) const;
    /// 弧长或弦长
    double length() const;
    /// 段中点
    Point2D middlePoint() const;
    /// 段上离给定点最近的位置
    Point2D closestPointTo(const Point2D& p) const;
};

class PolylineEntity final : public Entity {
public:
    using Vertex = PolylineVertex;

    PolylineEntity();
    explicit PolylineEntity(const std::vector<Point2D>& points, bool closed = false);
    PolylineEntity(std::vector<Vertex> vertices, bool closed);

    /// 轴对齐矩形（闭合多段线）
    static PolylineEntity makeRectangle(const Point2D& corner1, const Point2D& corner2);

    /// 由「起点 + 终点 + 起点凸度」推导一段几何（直线或圆弧）。
    /// 供填充边界、标注等需要相同 bulge 语义的场景复用。
    static PolylineSegment makeSegment(const Point2D& start, const Point2D& end, double bulge);

    // ---- 顶点操作 ------------------------------------------------------------
    const std::vector<Vertex>& vertices() const noexcept { return m_vertices; }
    std::size_t vertexCount() const noexcept { return m_vertices.size(); }
    const Vertex& vertex(std::size_t index) const { return m_vertices[index]; }
    bool hasVertex(std::size_t index) const noexcept { return index < m_vertices.size(); }

    void setVertices(std::vector<Vertex> vertices);
    void appendVertex(const Point2D& p, double bulge = 0.0);
    void insertVertex(std::size_t index, const Vertex& v);
    void removeVertex(std::size_t index);
    void setVertexPosition(std::size_t index, const Point2D& p);
    void setVertexBulge(std::size_t index, double bulge);
    void setVertexWidths(std::size_t index, double startWidth, double endWidth);
    void clear() noexcept;

    /// 仅取坐标序列，供 DXF 输出与几何计算使用
    std::vector<Point2D> points() const;

    // ---- 闭合与分段 ----------------------------------------------------------
    bool isClosed() const override { return m_closed; }
    void setClosed(bool closed) noexcept { m_closed = closed; }

    /// 段数：闭合时为顶点数，否则为顶点数 - 1
    std::size_t segmentCount() const noexcept;

    /// 取出第 index 段的几何（自动按 bulge 还原为直线段或圆弧段）
    std::optional<PolylineSegment> segment(std::size_t index) const;

    /// 所有段
    std::vector<PolylineSegment> segments() const;

    /// 逐段总长（含圆弧段弧长）
    double perimeter() const;

    // ---- 宽度（PCB 走线等场合的核心语义） ------------------------------------
    //
    // DXF 的宽度有两层：
    //   · 恒定宽度（组码 43）：整条多段线统一宽度，设置后逐顶点宽度被忽略；
    //   · 逐顶点宽度（组码 40/41）：每个顶点带起止宽度，宽度沿线段线性过渡。
    // 两者都为 0 时按普通细线处理。

    /// 恒定宽度（世界单位，0 = 无）
    double constantWidth() const noexcept { return m_constantWidth; }
    void setConstantWidth(double w) noexcept { m_constantWidth = std::max(w, 0.0); }

    /// 全线最大宽度（含恒定宽度与逐顶点宽度），用于拾取与包围盒
    double maxWidth() const noexcept;

    /// 是否存在任何非零宽度
    bool hasWidth() const noexcept { return maxWidth() > kGeomTol; }

    /// 第 index 段的有效宽度（恒定宽度优先；否则取该段两端宽度的较大值）
    double segmentWidth(std::size_t index) const noexcept;

    /// 逐顶点宽度是否完全一致（渲染时可走单路径快速通道）
    bool hasUniformWidth() const noexcept;

    // ---- Entity 接口 ---------------------------------------------------------
    std::unique_ptr<Entity> clone() const override;
    Box2D boundingBox() const override;
    double distanceTo(const Point2D& p) const override;
    Point2D closestPointTo(const Point2D& p) const override;
    bool crosses(const Box2D& box) const override;
    void transform(const Transform2D& t) override;
    double length() const override;
    std::vector<Point2D> snapPoints() const override;
    std::vector<Point2D> gripPoints() const override;
    bool moveGrip(std::size_t index, const Point2D& newPos) override;
    std::vector<Point2D> flatten(double sagitta) const override;
    QString describe() const override;
    QString geometrySummary() const override;

    static constexpr EntityType kType = EntityType::Polyline;

private:
    std::vector<Vertex> m_vertices;
    bool m_closed = false;
    double m_constantWidth = 0.0;    ///< DXF 43，0 = 无
};

} // namespace lycad
