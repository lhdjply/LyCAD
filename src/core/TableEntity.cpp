// =============================================================================
//  LyCAD - core/TableEntity.cpp
// =============================================================================
#include "core/TableEntity.h"

#include "core/Format.h"

#include <QStringList>

#include <algorithm>
#include <limits>

namespace lycad {

namespace {

/// 网格最小尺寸，防止零高/零宽导致的退化几何
constexpr double kMinSize = 1e-6;

} // namespace

TableEntity::TableEntity()
    : Entity(EntityType::Table)
{
    reset(3, 3);
}

TableEntity::TableEntity(std::size_t rowCount, std::size_t columnCount)
    : Entity(EntityType::Table)
{
    reset(rowCount, columnCount);
}

void TableEntity::setRotation(double radians)
{
    m_rotation = normalizeAngle(radians);
}

void TableEntity::reset(std::size_t rowCount, std::size_t columnCount,
                        double rowHeight, double columnWidth)
{
    rowCount = std::max<std::size_t>(rowCount, 1);
    columnCount = std::max<std::size_t>(columnCount, 1);

    m_rowHeights.assign(rowCount, std::max(rowHeight, kMinSize));
    m_columnWidths.assign(columnCount, std::max(columnWidth, kMinSize));
    // 没有整表字高可取：单元格为空时也就没有样式，字高在写入文字时按行高冻一个初值
    m_cells.clear();
    m_merges.clear();
}

void TableEntity::setRowHeight(std::size_t row, double height)
{
    if (row >= m_rowHeights.size()) return;
    m_rowHeights[row] = std::max(height, kMinSize);
}

void TableEntity::setColumnWidth(std::size_t col, double width)
{
    if (col >= m_columnWidths.size()) return;
    m_columnWidths[col] = std::max(width, kMinSize);
}

void TableEntity::insertRow(std::size_t index, double height)
{
    index = std::min(index, m_rowHeights.size());
    m_rowHeights.insert(m_rowHeights.begin() + static_cast<std::ptrdiff_t>(index),
                        std::max(height, kMinSize));
    // 单元格与合并区按行平移
    std::map<std::pair<int, int>, TableCell> shifted;
    for (auto& [key, value] : m_cells) {
        int row = key.first;
        if (static_cast<std::size_t>(row) >= index) ++row;
        shifted[{ row, key.second }] = value;
    }
    m_cells.swap(shifted);
    for (TableMergeRange& range : m_merges) {
        if (static_cast<std::size_t>(range.row) >= index) ++range.row;
    }
}

void TableEntity::insertColumn(std::size_t index, double width)
{
    index = std::min(index, m_columnWidths.size());
    m_columnWidths.insert(m_columnWidths.begin() + static_cast<std::ptrdiff_t>(index),
                          std::max(width, kMinSize));
    std::map<std::pair<int, int>, TableCell> shifted;
    for (auto& [key, value] : m_cells) {
        int col = key.second;
        if (static_cast<std::size_t>(col) >= index) ++col;
        shifted[{ key.first, col }] = value;
    }
    m_cells.swap(shifted);
    for (TableMergeRange& range : m_merges) {
        if (static_cast<std::size_t>(range.col) >= index) ++range.col;
    }
}

void TableEntity::removeRow(std::size_t index)
{
    if (index >= m_rowHeights.size() || m_rowHeights.size() <= 1) return;
    m_rowHeights.erase(m_rowHeights.begin() + static_cast<std::ptrdiff_t>(index));

    std::map<std::pair<int, int>, TableCell> kept;
    for (auto& [key, value] : m_cells) {
        const int row = key.first;
        if (static_cast<std::size_t>(row) == index) continue;
        kept[{ row > static_cast<int>(index) ? row - 1 : row, key.second }] = value;
    }
    m_cells.swap(kept);
    clearMerges();   // 合并区在删行后语义不再可靠，保守清空
}

void TableEntity::removeColumn(std::size_t index)
{
    if (index >= m_columnWidths.size() || m_columnWidths.size() <= 1) return;
    m_columnWidths.erase(m_columnWidths.begin() + static_cast<std::ptrdiff_t>(index));

    std::map<std::pair<int, int>, TableCell> kept;
    for (auto& [key, value] : m_cells) {
        const int col = key.second;
        if (static_cast<std::size_t>(col) == index) continue;
        kept[{ key.first, col > static_cast<int>(index) ? col - 1 : col }] = value;
    }
    m_cells.swap(kept);
    clearMerges();
}

double TableEntity::rowHeight(std::size_t row) const noexcept
{
    return row < m_rowHeights.size() ? m_rowHeights[row] : 0.0;
}

double TableEntity::columnWidth(std::size_t col) const noexcept
{
    return col < m_columnWidths.size() ? m_columnWidths[col] : 0.0;
}

double TableEntity::totalHeight() const noexcept
{
    double sum = 0.0;
    for (double h : m_rowHeights) sum += h;
    return sum;
}

double TableEntity::totalWidth() const noexcept
{
    double sum = 0.0;
    for (double w : m_columnWidths) sum += w;
    return sum;
}

TableCell TableEntity::cell(std::size_t row, std::size_t col) const
{
    const auto it = m_cells.find({ static_cast<int>(row), static_cast<int>(col) });
    return it == m_cells.end() ? TableCell() : it->second;
}

double TableEntity::defaultCellTextHeight(std::size_t row) const
{
    return std::max(rowHeight(row) * 0.5, kMinSize);
}

void TableEntity::assignMissingCellHeights()
{
    for (auto& [key, value] : m_cells) {
        if (value.text.isEmpty()) continue;
        if (!(value.height > 0.0)) {
            value.height = defaultCellTextHeight(static_cast<std::size_t>(key.first));
        }
    }
}

double TableEntity::cellHeightOr(std::size_t row, std::size_t col) const
{
    const TableCell c = cell(row, col);
    return (c.height > 0.0) ? c.height : defaultCellTextHeight(row);
}

void TableEntity::setCell(std::size_t row, std::size_t col, const TableCell& c)
{
    if (row >= rowCount() || col >= columnCount()) return;
    if (c.text.isEmpty() && c.align == TableCellAlign::MiddleCenter) {
        m_cells.erase({ static_cast<int>(row), static_cast<int>(col) });
        return;
    }
    // 有文字却没有字高（调用方只填了内容）：按该行行高冻一个初值。**必须在写入时冻**，
    // 不能在渲染/写出时实时算——那样拖一下行界线，格里的字就会跟着变大变小。
    TableCell stored = c;
    if (!stored.text.isEmpty() && !(stored.height > 0.0)) {
        stored.height = defaultCellTextHeight(row);
    }
    m_cells[{ static_cast<int>(row), static_cast<int>(col) }] = stored;
}

void TableEntity::setCellText(std::size_t row, std::size_t col, const QString& text)
{
    TableCell c = cell(row, col);
    c.text = text;
    setCell(row, col, c);
}

void TableEntity::setCellAlign(std::size_t row, std::size_t col, TableCellAlign align)
{
    TableCell c = cell(row, col);
    c.align = align;
    setCell(row, col, c);
}


std::vector<std::tuple<int, int, TableCell>> TableEntity::nonEmptyCells() const
{
    std::vector<std::tuple<int, int, TableCell>> out;
    out.reserve(m_cells.size());
    for (const auto& [key, value] : m_cells) {
        if (value.text.isEmpty()) continue;
        out.emplace_back(key.first, key.second, value);
    }
    return out;
}

std::optional<std::pair<int, int>> TableEntity::cellAtWorld(const Point2D& world) const
{
    const std::size_t rows = rowCount();
    const std::size_t cols = columnCount();
    if (rows == 0 || cols == 0) return std::nullopt;

    const Point2D local = toLocal(world);
    const std::vector<double> rowYs = rowBoundaries();      // 上边界 0，向下为负
    const std::vector<double> colXs = columnBoundaries();   // 左边界 0，向右为正

    // 容差与 distanceTo 的外接矩形判定同一口径：外接矩形内的点一定能落到某一格。
    // 点正好压在格线上时归给左侧 / 上方那一格（下标的推进条件是严格大于），
    // 这样既不会因浮点误差落到格外，也不需要调用方自己加容差。
    if (local.x < -kGeomTol || local.x > colXs.back() + kGeomTol) return std::nullopt;
    if (local.y > kGeomTol || local.y < rowYs.back() - kGeomTol) return std::nullopt;

    std::size_t col = 0;
    while (col + 1 < cols && local.x > colXs[col + 1]) ++col;
    std::size_t row = 0;
    while (row + 1 < rows && local.y < rowYs[row + 1]) ++row;

    // 合并区：文字归属左上角（见头文件说明）
    if (const auto merge = mergeContaining(row, col)) {
        return std::make_pair(merge->row, merge->col);
    }
    return std::make_pair(static_cast<int>(row), static_cast<int>(col));
}

void TableEntity::addMerge(const TableMergeRange& range)
{
    if (range.row < 0 || range.col < 0) return;
    if (range.rowSpan < 1 || range.colSpan < 1) return;
    if (static_cast<std::size_t>(range.row) >= rowCount()
        || static_cast<std::size_t>(range.col) >= columnCount()) {
        return;
    }
    // 与已有合并区重叠则忽略，避免出现交错的自相矛盾结构
    for (const TableMergeRange& existing : m_merges) {
        const bool disjoint = range.row + range.rowSpan <= existing.row
            || existing.row + existing.rowSpan <= range.row
            || range.col + range.colSpan <= existing.col
            || existing.col + existing.colSpan <= range.col;
        if (!disjoint) return;
    }
    m_merges.push_back(range);
}

std::optional<TableMergeRange> TableEntity::mergeContaining(std::size_t row, std::size_t col) const
{
    for (const TableMergeRange& range : m_merges) {
        if (static_cast<int>(row) >= range.row && static_cast<int>(row) < range.row + range.rowSpan
            && static_cast<int>(col) >= range.col
            && static_cast<int>(col) < range.col + range.colSpan) {
            return range;
        }
    }
    return std::nullopt;
}

std::vector<double> TableEntity::rowBoundaries() const
{
    // 第 0 行在最上方：上边界为 0，向下（负 Y）累积
    std::vector<double> bounds;
    bounds.reserve(m_rowHeights.size() + 1);
    double y = 0.0;
    bounds.push_back(y);
    for (double h : m_rowHeights) {
        y -= h;
        bounds.push_back(y);
    }
    return bounds;
}

std::vector<double> TableEntity::columnBoundaries() const
{
    std::vector<double> bounds;
    bounds.reserve(m_columnWidths.size() + 1);
    double x = 0.0;
    bounds.push_back(x);
    for (double w : m_columnWidths) {
        x += w;
        bounds.push_back(x);
    }
    return bounds;
}

Point2D TableEntity::toWorld(const Point2D& local) const
{
    const double c = std::cos(m_rotation);
    const double s = std::sin(m_rotation);
    return Point2D(m_insertionPoint.x + local.x * c - local.y * s,
                   m_insertionPoint.y + local.x * s + local.y * c);
}

Point2D TableEntity::toLocal(const Point2D& world) const
{
    const double dx = world.x - m_insertionPoint.x;
    const double dy = world.y - m_insertionPoint.y;
    const double c = std::cos(m_rotation);
    const double s = std::sin(m_rotation);
    return Point2D(dx * c + dy * s, -dx * s + dy * c);
}

std::array<Point2D, 4> TableEntity::cellCornersLocal(std::size_t row, std::size_t col) const
{
    const std::vector<double> rows = rowBoundaries();
    const std::vector<double> cols = columnBoundaries();
    const double top = rows[row];
    const double bottom = rows[row + 1];
    const double left = cols[col];
    const double right = cols[col + 1];
    // 左上、右上、右下、左下（局部坐标，Y 向下为负）
    return { Point2D(left, top), Point2D(right, top),
             Point2D(right, bottom), Point2D(left, bottom) };
}

std::vector<std::pair<Point2D, Point2D>> TableEntity::gridLinesLocal() const
{
    const std::vector<double> rows = rowBoundaries();
    const std::vector<double> cols = columnBoundaries();
    const double width = totalWidth();
    const double height = totalHeight();

    std::vector<std::pair<Point2D, Point2D>> lines;

    // 横线：每一行的上下边界，但被合并区跨越时不画中断段
    for (std::size_t r = 0; r < rows.size(); ++r) {
        double x = 0.0;
        for (std::size_t c = 0; c < m_columnWidths.size(); ++c) {
            const double x2 = x + m_columnWidths[c];

            // 判断该线段是否位于某个合并区内部（即应被擦除）
            bool erased = false;
            if (r > 0 && r < rows.size() - 1) {
                // 水平内线：若它正处于某个合并区的**垂直跨度内**，则该段被擦除
                for (const TableMergeRange& range : m_merges) {
                    const bool inColBand =
                        static_cast<int>(c) >= range.col
                        && static_cast<int>(c) < range.col + range.colSpan;
                    const bool strictlyInsideRow =
                        static_cast<int>(r) > range.row
                        && static_cast<int>(r) < range.row + range.rowSpan;
                    if (inColBand && strictlyInsideRow) { erased = true; break; }
                }
            }
            if (!erased) {
                lines.emplace_back(Point2D(x, rows[r]), Point2D(x2, rows[r]));
            }
            x = x2;
        }
    }

    // 竖线：每一列的左右边界，同理擦除合并区内部的竖直段
    for (std::size_t c = 0; c < cols.size(); ++c) {
        double y0 = 0.0;
        for (std::size_t r = 0; r < m_rowHeights.size(); ++r) {
            const double y1 = y0 - m_rowHeights[r];

            bool erased = false;
            if (c > 0 && c < cols.size() - 1) {
                for (const TableMergeRange& range : m_merges) {
                    const bool inRowBand =
                        static_cast<int>(r) >= range.row
                        && static_cast<int>(r) < range.row + range.rowSpan;
                    const bool strictlyInsideCol =
                        static_cast<int>(c) > range.col
                        && static_cast<int>(c) < range.col + range.colSpan;
                    if (inRowBand && strictlyInsideCol) { erased = true; break; }
                }
            }
            if (!erased) {
                lines.emplace_back(Point2D(cols[c], y0), Point2D(cols[c], y1));
            }
            y0 = y1;
        }
    }

    Q_UNUSED(width);
    Q_UNUSED(height);
    return lines;
}

// -----------------------------------------------------------------------------
//  Entity 接口
// -----------------------------------------------------------------------------
std::unique_ptr<Entity> TableEntity::clone() const
{
    // Entity 的拷贝构造被删除（图元天然多态），只能手工逐字段复制。
    auto copy = std::make_unique<TableEntity>();
    copyCommonPropertiesTo(*copy);
    copy->m_insertionPoint = m_insertionPoint;
    copy->m_rotation = m_rotation;
    copy->m_rowHeights = m_rowHeights;
    copy->m_columnWidths = m_columnWidths;
    copy->m_cells = m_cells;
    copy->m_merges = m_merges;
    return copy;
}

Box2D TableEntity::boundingBox() const
{
    Box2D box;
    const double w = totalWidth();
    const double h = totalHeight();
    // 局部四角：(0,0) (w,0) (w,-h) (0,-h)
    const std::array<Point2D, 4> corners = {
        toWorld(Point2D(0.0, 0.0)), toWorld(Point2D(w, 0.0)),
        toWorld(Point2D(w, -h)), toWorld(Point2D(0.0, -h))
    };
    for (const Point2D& p : corners) box.expand(p);
    return box;
}

double TableEntity::distanceTo(const Point2D& p) const
{
    const Point2D local = toLocal(p);
    const double w = totalWidth();
    const double h = totalHeight();

    // 落在表格外接矩形内 -> 距离为 0（点选表格任意位置都算命中）
    if (local.x >= -kGeomTol && local.x <= w + kGeomTol
        && local.y <= kGeomTol && local.y >= -h - kGeomTol) {
        return 0.0;
    }

    // 否则取到最近网格线的距离
    double best = std::numeric_limits<double>::max();
    for (const auto& [a, b] : gridLinesLocal()) {
        best = std::min(best, closestPointOnSegment(local, a, b).distanceTo(local));
    }
    return best;
}

Point2D TableEntity::closestPointTo(const Point2D& p) const
{
    const Point2D local = toLocal(p);
    const auto corners = std::array<Point2D, 4>{
        Point2D(0.0, 0.0), Point2D(totalWidth(), 0.0),
        Point2D(totalWidth(), -totalHeight()), Point2D(0.0, -totalHeight())
    };

    Point2D bestLocal = corners[0];
    double bestDist = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const Point2D& a = corners[i];
        const Point2D& b = corners[(i + 1) % corners.size()];
        const Point2D candidate = closestPointOnSegment(local, a, b);
        const double d = candidate.distanceTo(local);
        if (d < bestDist) { bestDist = d; bestLocal = candidate; }
    }
    return toWorld(bestLocal);
}

bool TableEntity::crosses(const Box2D& box) const
{
    return box.intersects(boundingBox());
}

void TableEntity::transform(const Transform2D& t)
{
    // 表格以「插入点 + 旋转」表达，无法表示一般的斜切/非均匀缩放。
    // 这里取插入点的变换结果，并从变换后的 X 轴方向重新求旋转角；
    // 非均匀缩放会退化（尺寸不随缩放变化），这是刻意的保守处理。
    const Point2D newOrigin = t.apply(m_insertionPoint);
    const Vector2D xAxis = t.applyVector(Vector2D(std::cos(m_rotation), std::sin(m_rotation)));
    if (xAxis.length() > kGeomTol) {
        m_rotation = normalizeAngle(std::atan2(xAxis.y, xAxis.x));
    }
    m_insertionPoint = newOrigin;
}

double TableEntity::length() const
{
    // 表格周长（外框），与「闭合多段线周长」语义一致
    return 2.0 * (totalWidth() + totalHeight());
}

std::vector<Point2D> TableEntity::snapPoints() const
{
    std::vector<Point2D> points;
    // 四个外角 + 每个单元格的四个角（去重交给调用方/捕捉引擎）
    const std::size_t rows = rowCount();
    const std::size_t cols = columnCount();
    points.reserve((rows + 1) * (cols + 1));
    const std::vector<double> rB = rowBoundaries();
    const std::vector<double> cB = columnBoundaries();
    for (double y : rB) {
        for (double x : cB) {
            points.push_back(toWorld(Point2D(x, y)));
        }
    }
    return points;
}

TableEntity::EdgeHit TableEntity::edgeAtWorld(const Point2D& world, double tolerance) const
{
    EdgeHit result;
    const std::size_t rows = rowCount();
    const std::size_t cols = columnCount();
    if (rows == 0 || cols == 0 || tolerance <= 0.0) return result;

    const Point2D local = toLocal(world);
    const std::vector<double> rB = rowBoundaries();      // 上 0，向下为负
    const std::vector<double> cB = columnBoundaries();   // 左 0，向右为正
    const double w = cB.back();
    const double h = -rB.back();                         // 表格总高（正数）

    // 取垂距最近的一条：交点（距离相等）时列界线先算、且用严格小于，结果确定
    double best = tolerance;

    // 列界线 j = 1..cols（第 j 列的右边界）：只在表格纵向跨度内可抓
    if (local.y <= tolerance && local.y >= -h - tolerance) {
        for (std::size_t j = 1; j <= cols; ++j) {
            const double d = std::abs(local.x - cB[j]);
            if (d < best) {
                best = d;
                result.hit = true;
                result.vertical = true;
                result.index = j;
                result.label = QStringLiteral("第 %1 列的右界线").arg(j);
            }
        }
    }

    // 行界线 i = 1..rows（第 i 行的下边界）：只在表格横向跨度内可抓
    if (local.x >= -tolerance && local.x <= w + tolerance) {
        for (std::size_t i = 1; i <= rows; ++i) {
            const double d = std::abs(local.y - rB[i]);
            if (d < best) {
                best = d;
                result.hit = true;
                result.vertical = false;
                result.index = i;
                result.label = QStringLiteral("第 %1 行的下界线").arg(i);
            }
        }
    }

    return result;
}

bool TableEntity::moveColumnBoundary(std::size_t index, const Point2D& world)
{
    const std::size_t cols = columnCount();
    if (index < 1 || index > cols) return false;

    const std::vector<double> bounds = columnBoundaries();
    const double x = toLocal(world).x;

    if (index < cols) {
        // 内部界线：左右两格互相让，总宽不变
        const double delta = x - bounds[index];
        const double d = std::clamp(delta,
                                    -(m_columnWidths[index - 1] - kMinSize),
                                    m_columnWidths[index] - kMinSize);
        m_columnWidths[index - 1] += d;
        m_columnWidths[index] -= d;
        return true;
    }

    // 最右界线：没有右邻可让，只能改总宽
    setColumnWidth(index - 1, x - bounds[index - 1]);
    return true;
}

bool TableEntity::moveRowBoundary(std::size_t index, const Point2D& world)
{
    const std::size_t rows = rowCount();
    if (index < 1 || index > rows) return false;

    const std::vector<double> bounds = rowBoundaries();
    const double y = toLocal(world).y;

    if (index < rows) {
        // 内部界线：上下两格互相让，总高不变
        const double delta = y - bounds[index];
        const double d = std::clamp(delta,
                                    -(m_rowHeights[index] - kMinSize),
                                    m_rowHeights[index - 1] - kMinSize);
        m_rowHeights[index - 1] -= d;
        m_rowHeights[index] += d;
        return true;
    }

    // 最下界线：没有下邻可让，只能改总高
    setRowHeight(index - 1, bounds[index - 1] - y);
    return true;
}

std::pair<Point2D, Point2D> TableEntity::columnBoundarySegment(std::size_t index) const
{
    if (index < 1 || index > columnCount()) return { Point2D(), Point2D() };
    const double x = columnBoundaries()[index];
    return { Point2D(x, 0.0), Point2D(x, -totalHeight()) };
}

std::pair<Point2D, Point2D> TableEntity::rowBoundarySegment(std::size_t index) const
{
    if (index < 1 || index > rowCount()) return { Point2D(), Point2D() };
    const double y = rowBoundaries()[index];
    return { Point2D(0.0, y), Point2D(totalWidth(), y) };
}

std::array<Point2D, 4> TableEntity::outerCorners() const
{
    const double w = totalWidth();
    const double h = totalHeight();
    // 左上、右上、右下、左下（局部坐标 Y 向下为负）
    return { toWorld(Point2D(0.0, 0.0)), toWorld(Point2D(w, 0.0)),
             toWorld(Point2D(w, -h)), toWorld(Point2D(0.0, -h)) };
}

std::vector<Point2D> TableEntity::gripPoints() const
{
    // 只有四角：夹点用来整体等比缩放。改行高 / 列宽不走夹点——直接把线拖走
    // （见 moveColumnBoundary / moveRowBoundary）。界线上画满方块既闹，也和
    // "整条线都能抓"的命中范围重复。
    const std::array<Point2D, 4> corners = outerCorners();
    return { corners[0], corners[1], corners[2], corners[3] };
}

QString TableEntity::gripLabel(std::size_t index) const
{
    return index < 4 ? QStringLiteral("角点（整体缩放）") : QString();
}

bool TableEntity::moveGrip(std::size_t index, const Point2D& newPos)
{
    // ---- 四角：整体等比缩放（保持对角锚点不动）------------------------------
    // 局部四角顺序（与 gripPoints 一致）：0=(0,0) 1=(w,0) 2=(w,-h) 3=(0,-h)
    if (index >= 4) return false;

    const double curW = totalWidth();
    const double curH = totalHeight();
    if (curW <= kGeomTol || curH <= kGeomTol) return false;
    const Point2D localTarget = toLocal(newPos);

    // 锚点 = 对角（index + 2）
    const std::size_t anchorIndex = (index + 2) % 4;

    const double anchorLocalX = (anchorIndex == 1 || anchorIndex == 2) ? curW : 0.0;
    const double anchorLocalY = (anchorIndex == 2 || anchorIndex == 3) ? -curH : 0.0;

    const double newW = std::max(std::abs(localTarget.x - anchorLocalX), kMinSize);
    const double newH = std::max(std::abs(localTarget.y - anchorLocalY), kMinSize);

    const double sx = newW / curW;
    const double sy = newH / curH;
    for (double& w : m_columnWidths) w = std::max(w * sx, kMinSize);
    for (double& h : m_rowHeights) h = std::max(h * sy, kMinSize);

    // 保持锚点世界坐标不动：锚点局部坐标在缩放后变为 (anchorLocalX*sx, anchorLocalY*sy)，
    // 反推插入点使 toWorld(缩放后锚点局部) == 原锚点世界坐标。
    const Point2D anchorWorld = toWorld(Point2D(anchorLocalX, anchorLocalY));
    const Point2D scaledAnchorLocal(anchorLocalX * sx, anchorLocalY * sy);
    const double c = std::cos(m_rotation);
    const double s = std::sin(m_rotation);
    m_insertionPoint = Point2D(
        anchorWorld.x - (scaledAnchorLocal.x * c - scaledAnchorLocal.y * s),
        anchorWorld.y - (scaledAnchorLocal.x * s + scaledAnchorLocal.y * c));
    return true;
}

std::vector<Point2D> TableEntity::flatten(double sagitta) const
{
    Q_UNUSED(sagitta);
    // 表格的轮廓即外框四角（网格线是内部结构，不作为轮廓）
    const double w = totalWidth();
    const double h = totalHeight();
    return { toWorld(Point2D(0.0, 0.0)), toWorld(Point2D(w, 0.0)),
             toWorld(Point2D(w, -h)), toWorld(Point2D(0.0, -h)) };
}

QString TableEntity::describe() const
{
    return QStringLiteral("表格  %1 行 × %2 列")
        .arg(rowCount())
        .arg(columnCount());
}

QString TableEntity::geometrySummary() const
{
    QStringList lines;
    lines << QStringLiteral("行数: %1").arg(rowCount());
    lines << QStringLiteral("列数: %1").arg(columnCount());
    lines << QStringLiteral("总宽: %1").arg(formatLength(totalWidth()));
    lines << QStringLiteral("总高: %1").arg(formatLength(totalHeight()));
    lines << QStringLiteral("插入点: %1").arg(formatPoint(m_insertionPoint));
    lines << QStringLiteral("旋转: %1").arg(formatAngle(m_rotation));
    if (!m_merges.empty()) {
        lines << QStringLiteral("合并单元格: %1 处").arg(m_merges.size());
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace lycad
