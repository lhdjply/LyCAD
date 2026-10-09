// =============================================================================
//  LyCAD - core/TableEntity.h
//  表格图元（DXF: ACAD_TABLE）
//
//  设计要点
//    · 与 AutoCAD 一致，表格以「插入点 + 旋转角」定位，行列尺寸各自可设；
//    · 行列尺寸用两个等差数列描述（行高列表、列宽列表），既支持等距表格，
//      也支持单行/单列加宽；列表长度即行数 / 列数；
//    · 单元格文字以「行列索引 -> 内容」的稀疏表存放，空白单元格不占内存；
//    · 合并单元格用「左上角 + 跨行/跨列数」描述，渲染时跳过被覆盖的网格线；
//    · 自身不持有子图元：网格线、文字在渲染期按需生成，避免文档里出现
//      大量隐藏图元（这一点与块参照不同——表格是**单一图元**）。
//
//  与块参照（InsertEntity）的分工
//    块参照是「引用一份可复用的定义」；表格的几何完全由自身参数决定，
//    不存在可复用的定义，因此建为独立图元类型而不是伪装成块。
// =============================================================================
#pragma once

#include "core/Entity.h"

#include <QString>

#include <array>
#include <map>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace lycad {

/// 单元格对齐方式（与 DXF ACAD_TABLE 的 cell alignment 对应）
enum class TableCellAlign {
    TopLeft,
    TopCenter,
    TopRight,
    MiddleLeft,
    MiddleCenter,
    MiddleRight,
    BottomLeft,
    BottomCenter,
    BottomRight
};

/// 单元格内容
struct TableCell {
    QString        text;
    TableCellAlign align = TableCellAlign::MiddleCenter;
};

/// 合并区域：以 (row, col) 为左上角，跨 rowSpan 行、colSpan 列
struct TableMergeRange {
    int row = 0;
    int col = 0;
    int rowSpan = 1;
    int colSpan = 1;
};

class TableEntity final : public Entity {
public:
    TableEntity();
    TableEntity(std::size_t rowCount, std::size_t columnCount);

    // ---- 定位 ----------------------------------------------------------------
    const Point2D& insertionPoint() const noexcept { return m_insertionPoint; }
    void setInsertionPoint(const Point2D& p) noexcept { m_insertionPoint = p; }

    /// 旋转角（弧度，逆时针为正）
    double rotation() const noexcept { return m_rotation; }
    void setRotation(double radians);

    // ---- 行列尺寸 ------------------------------------------------------------
    std::size_t rowCount() const noexcept { return m_rowHeights.size(); }
    std::size_t columnCount() const noexcept { return m_columnWidths.size(); }

    /// 重置为 r 行 c 列等距表格（行高 rowHeight，列宽 columnWidth）
    void reset(std::size_t rowCount, std::size_t columnCount,
               double rowHeight = 8.0, double columnWidth = 30.0);

    const std::vector<double>& rowHeights() const noexcept { return m_rowHeights; }
    const std::vector<double>& columnWidths() const noexcept { return m_columnWidths; }

    void setRowHeight(std::size_t row, double height);
    void setColumnWidth(std::size_t col, double width);

    /// 插入一列/一行（在 index 处，尺寸复制自参考列/行）
    void insertRow(std::size_t index, double height);
    void insertColumn(std::size_t index, double width);
    void removeRow(std::size_t index);
    void removeColumn(std::size_t index);

    double rowHeight(std::size_t row) const noexcept;
    double columnWidth(std::size_t col) const noexcept;

    /// 无合并时表格的总高 / 总宽（局部坐标，未旋转）
    double totalHeight() const noexcept;
    double totalWidth() const noexcept;

    // ---- 单元格 --------------------------------------------------------------
    /// 取单元格内容；越界返回空
    TableCell cell(std::size_t row, std::size_t col) const;
    void setCell(std::size_t row, std::size_t col, const TableCell& cell);
    void setCellText(std::size_t row, std::size_t col, const QString& text);
    void setCellAlign(std::size_t row, std::size_t col, TableCellAlign align);

    /// 全部非空单元格的 (行, 列, 内容) 列表，供渲染与导出使用
    std::vector<std::tuple<int, int, TableCell>> nonEmptyCells() const;

    // ---- 合并单元格 ----------------------------------------------------------
    const std::vector<TableMergeRange>& merges() const noexcept { return m_merges; }
    void addMerge(const TableMergeRange& range);
    void clearMerges() noexcept { m_merges.clear(); }

    /// 若 (row, col) 落在某个合并区域内，返回它；否则返回空
    std::optional<TableMergeRange> mergeContaining(std::size_t row, std::size_t col) const;

    // ---- 行高/列宽的累积边界 ------------------------------------------------
    /// 各行的上边界（局部 Y，自插入点向上为正），长度 = 行数 + 1
    std::vector<double> rowBoundaries() const;
    /// 各列的左边界（局部 X），长度 = 列数 + 1
    std::vector<double> columnBoundaries() const;

    /// 局部坐标 -> 世界坐标
    Point2D toWorld(const Point2D& local) const;
    /// 世界坐标 -> 局部坐标
    Point2D toLocal(const Point2D& world) const;

    /// 第 (row, col) 个单元格在局部坐标系下的矩形四角（左上、右上、右下、左下）
    std::array<Point2D, 4> cellCornersLocal(std::size_t row, std::size_t col) const;

    /// 网格线（局部坐标）——渲染、拾取、导出共用的几何真源
    std::vector<std::pair<Point2D, Point2D>> gridLinesLocal() const;

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

    static constexpr EntityType kType = EntityType::Table;

private:
    Point2D m_insertionPoint{ 0.0, 0.0 };
    double  m_rotation = 0.0;

    std::vector<double> m_rowHeights;      ///< 自插入点向上，第 0 行在最上方
    std::vector<double> m_columnWidths;    ///< 自插入点向右

    /// (row, col) -> 单元格。用 map 保证顺序稳定，便于导出。
    std::map<std::pair<int, int>, TableCell> m_cells;
    std::vector<TableMergeRange> m_merges;
};

} // namespace lycad
