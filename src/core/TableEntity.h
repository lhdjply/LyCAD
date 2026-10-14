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
//    · 夹点只有**外框四角**（整体等比缩放）；改行高 / 列宽不走夹点，而是**直接把
//      光标压到行列界线上拖**（Excel 的习惯）——界线上不画方块，画满方块的表太闹，
//      而且拖线的命中范围本来就是整条线，不需要一个点来代言。
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
    /// 字高（世界单位，> 0）。**每个格子各有各的**——表格没有"整表字号"这回事。
    ///
    /// 没设过时（0）由 TableEntity 在写入文字时按**该行行高的一半**冻一个初值：只有
    /// 冻住，"拖行界线改字大小"才不会发生（这正是曾经按行高实时算留下的坑）。
    double         height = 0.0;
    /// 字体（DXF 文字样式名，空 = 默认字体）。同样是逐格的。
    QString        fontStyle;
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

    /// 第 row 行的默认字高：**行高的一半**（下限 kMinSize）。
    ///
    /// 只在"这一格还没有字高"时用一次（新建 / 导入的初值），得到的值随即写进单元格，
    /// 此后行高怎么变都不再影响它——所以拖行界线不会改字的大小。
    double defaultCellTextHeight(std::size_t row) const;

    /// 给**还没有显式字高**的非空单元格补上默认值（逐格、按各自行的行高）。
    ///
    /// 导入收尾时统一补一次，保证文档里任何非空单元格的字高都是明确的：渲染与写出
    /// 因此永远不需要"实时算一个"——那正是"拖一下行高，字跟着变"的来源。
    void assignMissingCellHeights();

    /// 第 (row, col) 格实际使用的字高：有显式值就用它，没有才退回该行的默认值。
    ///
    /// 正常情况恒等于 `cell(row, col).height`（写入过文字的格子都有明确值，见 setCell）；
    /// 这个回退只为"手工构造、值没填"的表格兜底，调用方不必自己判 0——判 0 的地方一多，
    /// 总有一处会顺手写成实时算。
    double cellHeightOr(std::size_t row, std::size_t col) const;

    /// 全部非空单元格的 (行, 列, 内容) 列表，供渲染与导出使用
    std::vector<std::tuple<int, int, TableCell>> nonEmptyCells() const;

    /// 命中测试（世界坐标）：点所在的单元格。
    ///
    /// 点在合并区内时返回该区的**左上角**单元格——文字就存在那里，渲染、读入、
    /// 写出三处都是这个约定，所以「编辑点击处的文字」= 编辑这一格。
    /// 点不在表格范围内时返回空。旋转已经被 toLocal 抵消，无需调用方预处理。
    std::optional<std::pair<int, int>> cellAtWorld(const Point2D& world) const;

    /// 光标压在**行列界线**上的结果（直接把线拖走用它）。
    struct EdgeHit {
        bool        hit = false;
        bool        vertical = false;    ///< true = 列界线（左右拖）；false = 行界线（上下拖）
        std::size_t index = 0;           ///< 界线序号（1 基）：第 index 列的右界线 / 第 index 行的下界线
        QString     label;               ///< 人话说明，命令行提示直接用它
    };

    /// 命中测试（世界坐标）：点是否压在行列界线上（距离在 tolerance 内）。
    ///
    /// 判据与"单元格四角"不同：界线是**整条线**都能抓，而不是只有某个点——
    /// 所以按到各条界线的垂距取最近者，且只在表格的跨度内算（表格上方/左侧的空白不算）。
    /// 最上一条与最左一条（插入点所在的两条边）**不算界线**：拖它们等于移动表格。
    ///
    /// 界线上**不画夹点方块**（一整条表上十几个方块太闹），拖线是它唯一的入口，
    /// 因此结果里带上 label，界面据此在命令行说明正在拖的是哪一条。
    EdgeHit edgeAtWorld(const Point2D& world, double tolerance) const;

    /// 把第 index 条界线挪到世界坐标处（世界坐标，旋转在内部抵消）。
    ///
    /// 内部界线**只挪线**（Word 表格的手感）：左右（上下）两格互相让，一格长多少
    /// 邻格就短多少，表格总宽 / 总高与插入点都不动。最右 / 最下两条界线没有邻格可让，
    /// 这时才改总尺寸。拖过头会停在邻格还剩 kMinSize 的地方，两格都不会出现负尺寸。
    bool moveColumnBoundary(std::size_t index, const Point2D& world);
    bool moveRowBoundary(std::size_t index, const Point2D& world);

    /// 界线的两个端点（局部坐标，供拖动时的虚线预览只用画这一条线）
    std::pair<Point2D, Point2D> columnBoundarySegment(std::size_t index) const;
    std::pair<Point2D, Point2D> rowBoundarySegment(std::size_t index) const;

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

    /// 外框四角（世界坐标，顺序：左上、右上、右下、左下）
    /// ——夹点与捕捉共用同一份几何，两处都不会各自算一遍
    std::array<Point2D, 4> outerCorners() const;

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
    QString gripLabel(std::size_t index) const override;
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
