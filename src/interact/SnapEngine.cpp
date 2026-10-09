// =============================================================================
//  LyCAD - interact/SnapEngine.cpp
// =============================================================================
#include "interact/SnapEngine.h"

#include "core/InsertEntity.h"

#include "core/ArcEntity.h"
#include "core/CircleEntity.h"
#include "core/DimensionEntity.h"
#include "core/EllipseEntity.h"
#include "core/Document.h"
#include "core/HatchEntity.h"
#include "core/InfiniteLineEntity.h"
#include "core/LineEntity.h"
#include "core/PointEntity.h"
#include "core/PolylineEntity.h"
#include "core/SplineEntity.h"
#include "core/TableEntity.h"
#include "core/TextEntity.h"

#include <algorithm>
#include <array>

namespace lycad {

namespace {

/// 参与交点计算的邻近图元数量上限，避免高密度图纸下的组合爆炸
constexpr std::size_t kMaxIntersectionEntities = 6;

/// 单个图元参与交点计算的离散段数上限
constexpr std::size_t kMaxSegmentsPerEntity = 512;

/// 各类捕捉的优先级（数值越小越优先）
int priorityOf(SnapKind kind)
{
    switch (kind) {
    case SnapKind::Endpoint:
    case SnapKind::NodePoint:    return 1;
    case SnapKind::Intersection: return 2;
    case SnapKind::Center:       return 3;
    case SnapKind::Midpoint:     return 4;
    case SnapKind::Quadrant:     return 5;
    case SnapKind::Perpendicular:
    case SnapKind::Tangent:      return 6;
    case SnapKind::Nearest:      return 7;
    case SnapKind::Grid:         return 8;
    case SnapKind::Ortho:
    case SnapKind::Polar:        return 9;
    case SnapKind::None:         break;
    }
    return 99;
}

bool withinAperture(const Point2D& a, const Point2D& b, double aperture)
{
    return a.distanceSquaredTo(b) <= aperture * aperture;
}

/// 收集图元在光标附近的离散线段，用于交点与垂足计算
std::vector<std::pair<Point2D, Point2D>> nearbySegments(const Entity& entity,
                                                        const Point2D& cursor,
                                                        double radius)
{
    std::vector<std::pair<Point2D, Point2D>> out;
    if (radius <= kGeomTol) return out;

    // 块参照：把展开后的子图元**分别**离散。直接把整块的离散点首尾连成一条
    // 折线，会在「上一个子图元的末点」与「下一个的首点」之间造出并不存在的
    // 线段——交点捕捉于是在块内的空白处冒出假交点。
    if (entity.type() == EntityType::Insert) {
        const auto& insert = static_cast<const InsertEntity&>(entity);
        for (const std::unique_ptr<Entity>& child : insert.expanded()) {
            if (!child) continue;
            const std::vector<std::pair<Point2D, Point2D>> sub =
                nearbySegments(*child, cursor, radius);
            out.insert(out.end(), sub.begin(), sub.end());
            if (out.size() >= kMaxSegmentsPerEntity) break;
        }
        return out;
    }

    // 无界图元（构造线、射线）不能按「离散整条图元」处理：它名义上有一百万
    // 单位长，拿它去算交点会白白丢掉精度（两端点相差如此悬殊时，交点的有效
    // 位数会被大端吞掉）。裁到光标邻域，得到的就是唯一有意义的那一段。
    if (entity.isUnbounded()) {
        const auto& line = static_cast<const InfiniteLineEntity&>(entity);
        if (const auto seg = line.clippedTo(Box2D::aroundPoint(cursor, radius))) {
            out.push_back(*seg);
        }
        return out;
    }

    // 离散精度取光圈半径的 1/40，足以保证交点定位精度
    const double sagitta = std::max(radius / 40.0, kGeomTol);
    const std::vector<Point2D> points = entity.flatten(sagitta);
    if (points.size() < 2) return out;

    const std::size_t count = points.size();
    const std::size_t segmentCount = entity.isClosed() ? count : count - 1;
    const double radiusSq = radius * radius;

    for (std::size_t i = 0; i < segmentCount; ++i) {
        const Point2D& a = points[i];
        const Point2D& b = points[(i + 1) % count];

        // 只保留靠近光标的线段：盒-圆快速筛除
        const double minX = std::min(a.x, b.x);
        const double maxX = std::max(a.x, b.x);
        const double minY = std::min(a.y, b.y);
        const double maxY = std::max(a.y, b.y);

        const double cx = std::clamp(cursor.x, minX, maxX);
        const double cy = std::clamp(cursor.y, minY, maxY);
        const double dx = cursor.x - cx;
        const double dy = cursor.y - cy;
        if (dx * dx + dy * dy > radiusSq) continue;

        out.emplace_back(a, b);
        if (out.size() >= kMaxSegmentsPerEntity) break;
    }
    return out;
}

} // namespace

SnapEngine::SnapEngine(const SnapSettings& settings)
    : m_settings(settings)
{
}

// -----------------------------------------------------------------------------
//  候选点收集
// -----------------------------------------------------------------------------
void SnapEngine::collectEntityCandidates(const Document& document,
                                         const Point2D& cursor,
                                         double aperture,
                                         std::vector<Candidate>& out,
                                         std::vector<EntityId>& nearby) const
{
    const LayerManager& layers = document.layers();

    auto add = [&](const Point2D& p, SnapKind kind) {
        if (withinAperture(p, cursor, aperture)) {
            out.push_back(Candidate{ p, kind, priorityOf(kind) });
            return true;
        }
        return false;
    };

    // 单个图元的捕捉候选。写成可递归的 lambda，是为了让块参照直接复用同一份
    // 判定：块内的圆心/象限点/中点不能一律退化成「端点」。
    auto collectFrom = [&](auto&& self, const Entity& e) -> bool {
        bool hit = false;
        switch (e.type()) {
        case EntityType::Line: {
            const auto& line = static_cast<const LineEntity&>(e);
            if (m_settings.endpoint) {
                hit |= add(line.start(), SnapKind::Endpoint);
                hit |= add(line.end(), SnapKind::Endpoint);
            }
            if (m_settings.midpoint) hit |= add(line.midpoint(), SnapKind::Midpoint);
            break;
        }
        case EntityType::Circle: {
            const auto& circle = static_cast<const CircleEntity&>(e);
            if (m_settings.center) hit |= add(circle.center(), SnapKind::Center);
            if (m_settings.quadrant) {
                for (int i = 0; i < 4; ++i) {
                    hit |= add(circle.quadrantPoint(i), SnapKind::Quadrant);
                }
            }
            break;
        }
        case EntityType::Arc: {
            const auto& arc = static_cast<const ArcEntity&>(e);
            if (m_settings.center) hit |= add(arc.center(), SnapKind::Center);
            if (m_settings.endpoint) {
                hit |= add(arc.startPoint(), SnapKind::Endpoint);
                hit |= add(arc.endPoint(), SnapKind::Endpoint);
            }
            if (m_settings.midpoint) hit |= add(arc.middlePoint(), SnapKind::Midpoint);
            if (m_settings.quadrant) {
                const std::array<double, 4> quadrants = { 0.0, kHalfPi, kPi, 3.0 * kHalfPi };
                for (double q : quadrants) {
                    if (arc.containsAngle(q)) {
                        hit |= add(arc.pointAtAngle(q), SnapKind::Quadrant);
                    }
                }
            }
            break;
        }
        case EntityType::Polyline: {
            const auto& polyline = static_cast<const PolylineEntity&>(e);
            if (m_settings.endpoint) {
                for (const auto& vertex : polyline.vertices()) {
                    hit |= add(vertex.point, SnapKind::Endpoint);
                }
            }
            if (m_settings.midpoint) {
                const std::size_t count = polyline.segmentCount();
                for (std::size_t i = 0; i < count; ++i) {
                    if (auto seg = polyline.segment(i)) {
                        hit |= add(seg->middlePoint(), SnapKind::Midpoint);
                    }
                }
            }
            if (m_settings.center) {
                const std::size_t count = polyline.segmentCount();
                for (std::size_t i = 0; i < count; ++i) {
                    if (auto seg = polyline.segment(i); seg && seg->isArc) {
                        hit |= add(seg->center, SnapKind::Center);
                    }
                }
            }
            break;
        }
        case EntityType::Point: {
            const auto& point = static_cast<const PointEntity&>(e);
            if (m_settings.node) hit |= add(point.position(), SnapKind::NodePoint);
            break;
        }
        case EntityType::Text: {
            // 文字只有一个定位点（NodePoint 捕捉语义）
            const auto& text = static_cast<const TextEntity&>(e);
            if (m_settings.node) hit |= add(text.effectivePosition(), SnapKind::NodePoint);
            break;
        }
        case EntityType::Hatch: {
            const auto& hatch = static_cast<const HatchEntity&>(e);
            if (m_settings.endpoint) {
                for (const Point2D& vertex : hatch.boundary()) {
                    hit |= add(vertex, SnapKind::Endpoint);
                }
            }
            break;
        }
        case EntityType::Dimension: {
            // 标注的定义点按端点语义捕捉
            const auto& dim = static_cast<const DimensionEntity&>(e);
            if (m_settings.endpoint) {
                hit |= add(dim.firstPoint(), SnapKind::Endpoint);
                hit |= add(dim.secondPoint(), SnapKind::Endpoint);
            }
            break;
        }
        case EntityType::Spline: {
            // 样条：捕捉两端点（闭合样条首尾重合，只报一次）
            if (m_settings.endpoint) {
                for (const Point2D& p : e.snapPoints()) {
                    hit |= add(p, SnapKind::Endpoint);
                }
            }
            break;
        }
        case EntityType::Ellipse: {
            const auto& ellipse = static_cast<const EllipseEntity&>(e);
            if (m_settings.center) hit |= add(ellipse.center(), SnapKind::Center);
            if (m_settings.quadrant) {
                for (const Point2D& p : ellipse.snapPoints()) {
                    if (p == ellipse.center()) continue;
                    hit |= add(p, SnapKind::Quadrant);
                }
            }
            break;
        }
        case EntityType::XLine:
        case EntityType::Ray: {
            // 无界图元没有端点、中点、圆心，AutoCAD 对它们也只提供最近点与交点。
            // 前者由下面的通用逻辑处理（走精确的投影解），后者由 collectIntersections
            // 的邻域裁剪处理。定义点额外按节点语义给出：辅助线常被当作基准来用。
            const auto& line = static_cast<const InfiniteLineEntity&>(e);
            if (m_settings.node) hit |= add(line.basePoint(), SnapKind::NodePoint);
            break;
        }
        case EntityType::Insert: {
            // 块参照：把块内图元按**各自的类型**重新走一遍上面的判定。
            // 若一律按端点上报，块里的圆就再也捕捉不到圆心与象限点。
            // 展开结果里不会再有 Insert（嵌套已在展开阶段解析），递归至多两层。
            const auto& insert = static_cast<const InsertEntity&>(e);
            for (const std::unique_ptr<Entity>& child : insert.expanded()) {
                if (child) hit |= self(self, *child);
            }
            break;
        }
        case EntityType::Table: {
            // 表格：单元格角点按节点语义捕捉（对齐到表格网格的最常用落点），
            // 外框四角额外按端点上报。
            const auto& table = static_cast<const TableEntity&>(e);
            if (m_settings.node) {
                for (const Point2D& p : table.snapPoints()) {
                    hit |= add(p, SnapKind::NodePoint);
                }
            }
            if (m_settings.endpoint) {
                for (const Point2D& p : table.gripPoints()) {
                    hit |= add(p, SnapKind::Endpoint);
                }
            }
            break;
        }
        }
        return hit;
    };

    for (EntityId id : document.drawOrder()) {
        const Entity* entity = document.entity(id);
        if (!entity || !entity->isDisplayable(layers)) continue;

        // 粗筛：包围盒与光标的距离超过光圈则整段跳过。
        // 无界图元没有包围盒，也无法用「基点是否在光圈内」代替——一条构造线
        // 可以离基点十万八千里仍从光标下穿过，因此这一类不做粗筛。
        if (!entity->isUnbounded()) {
            Box2D box = entity->boundingBox();
            if (!box.isValid()) continue;
            box.expand(aperture);
            if (!box.contains(cursor)) continue;
        }

        bool touched = collectFrom(collectFrom, *entity);

        // 最近点：候选为图元上离光标最近的位置
        if (m_settings.nearest) {
            const Point2D nearest = entity->closestPointTo(cursor);
            touched |= add(nearest, SnapKind::Nearest);
        }

        if (touched && nearby.size() < kMaxIntersectionEntities) {
            nearby.push_back(id);
        }
    }
}

void SnapEngine::collectIntersections(const Document& document,
                                      const Point2D& cursor,
                                      double aperture,
                                      const std::vector<EntityId>& nearby,
                                      std::vector<Candidate>& out) const
{
    if (nearby.size() < 2) return;

    // 预先离散各邻近图元，避免在两两配对时重复离散
    std::vector<std::vector<std::pair<Point2D, Point2D>>> segmentSets;
    segmentSets.reserve(nearby.size());
    for (EntityId id : nearby) {
        const Entity* entity = document.entity(id);
        if (!entity) {
            segmentSets.emplace_back();
            continue;
        }
        segmentSets.push_back(nearbySegments(*entity, cursor, aperture * 2.0));
    }

    for (std::size_t i = 0; i < segmentSets.size(); ++i) {
        for (std::size_t j = i + 1; j < segmentSets.size(); ++j) {
            for (const auto& segA : segmentSets[i]) {
                for (const auto& segB : segmentSets[j]) {
                    const auto hit = segmentSegmentIntersection(segA.first, segA.second,
                                                                segB.first, segB.second, 1e-9);
                    if (!hit) continue;
                    if (!withinAperture(*hit, cursor, aperture)) continue;

                    // 去重：同一位置可能由多条线段产生
                    const bool duplicate = std::any_of(
                        out.begin(), out.end(), [&](const Candidate& c) {
                            return c.kind == SnapKind::Intersection
                                && c.position.distanceTo(*hit) <= kGeomTol * 100.0;
                        });
                    if (!duplicate) {
                        out.push_back(Candidate{ *hit, SnapKind::Intersection,
                                                 priorityOf(SnapKind::Intersection) });
                    }
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
//  求解
// -----------------------------------------------------------------------------
std::optional<SnapResult> SnapEngine::resolve(const Document& document,
                                              const Point2D& worldCursor,
                                              double apertureWorld,
                                              const std::optional<Point2D>& basePoint,
                                              const std::vector<EntityId>& exclude) const
{
    if (!m_settings.enabled || apertureWorld <= 0.0) return std::nullopt;

    std::vector<Candidate> candidates;
    std::vector<EntityId> nearby;

    if (m_settings.anyObjectSnapEnabled()) {
        collectEntityCandidates(document, worldCursor, apertureWorld, candidates, nearby);

        // 排除正在绘制中的临时图元，避免「捕捉到自己」
        if (!exclude.empty()) {
            nearby.erase(std::remove_if(nearby.begin(), nearby.end(),
                                        [&](EntityId id) {
                                            return std::find(exclude.begin(), exclude.end(), id)
                                                 != exclude.end();
                                        }),
                         nearby.end());
        }

        if (m_settings.intersection) {
            collectIntersections(document, worldCursor, apertureWorld, nearby, candidates);
        }
    }

    // 垂足 / 切点：依赖基准点，仅在已有基准点时可用
    if (basePoint) {
        const Point2D base = *basePoint;

        for (EntityId id : nearby) {
            const Entity* entity = document.entity(id);
            if (!entity) continue;

            if (entity->type() == EntityType::Line) {
                if (!m_settings.perpendicular) continue;
                const auto& line = static_cast<const LineEntity&>(*entity);
                const Vector2D direction = line.end() - line.start();
                const double lengthSq = direction.lengthSquared();
                if (lengthSq <= kEpsilon) continue;

                const double t = dot(base - line.start(), direction) / lengthSq;
                if (t < 0.0 || t > 1.0) continue;      // 垂足落在延长线上，不做捕捉

                const Point2D foot = line.start() + direction * t;
                if (withinAperture(foot, worldCursor, apertureWorld)) {
                    candidates.push_back(Candidate{ foot, SnapKind::Perpendicular,
                                                    priorityOf(SnapKind::Perpendicular) });
                }
            } else if (entity->type() == EntityType::Circle) {
                const auto& circle = static_cast<const CircleEntity&>(*entity);
                const Vector2D toBase = base - circle.center();
                const double distance = toBase.length();
                if (distance <= circle.radius() + kGeomTol) continue;   // 基准点在圆内，无切点

                // 自基准点向圆作切线，切点与「基准点-圆心」连线夹角为 acos(R/d)
                const double alpha = std::acos(std::clamp(circle.radius() / distance, -1.0, 1.0));
                const double baseAngle = std::atan2(toBase.y, toBase.x);
                const double tangentLength = std::sqrt(std::max(0.0, distance * distance
                                                                       - circle.radius() * circle.radius()));

                for (double sign : { 1.0, -1.0 }) {
                    const double angle = baseAngle + sign * alpha;
                    const Point2D tangentPoint(circle.center().x + circle.radius() * std::cos(angle),
                                               circle.center().y + circle.radius() * std::sin(angle));
                    // 校验切点确实位于该切线方向上
                    const double check = (tangentPoint - base).length();
                    if (std::abs(check - tangentLength) > tangentLength * 1e-6 + 1e-6) continue;
                    if (!withinAperture(tangentPoint, worldCursor, apertureWorld)) continue;
                    candidates.push_back(Candidate{ tangentPoint, SnapKind::Tangent,
                                                    priorityOf(SnapKind::Tangent) });
                }
            }
        }
    }

    // 栅格吸附
    if (m_settings.grid && m_settings.gridSpacing > kGeomTol) {
        const double spacing = m_settings.gridSpacing;
        const Point2D gridPoint(std::round(worldCursor.x / spacing) * spacing,
                                std::round(worldCursor.y / spacing) * spacing);
        candidates.push_back(Candidate{ gridPoint, SnapKind::Grid, priorityOf(SnapKind::Grid) });
    }

    if (candidates.empty()) return std::nullopt;

    // 先按优先级、再按距离取最优
    const Candidate* best = nullptr;
    double bestDistance = std::numeric_limits<double>::max();

    for (const Candidate& candidate : candidates) {
        const double distance = candidate.position.distanceTo(worldCursor);
        if (!best || candidate.priority < best->priority
            || (candidate.priority == best->priority && distance < bestDistance)) {
            best = &candidate;
            bestDistance = distance;
        }
    }

    if (!best) return std::nullopt;

    SnapResult result;
    result.position = best->position;
    result.kind = best->kind;
    result.label = snapKindShortLabel(best->kind);
    return result;
}

// -----------------------------------------------------------------------------
//  方向约束
// -----------------------------------------------------------------------------
Point2D SnapEngine::applyOrtho(const Point2D& base, const Point2D& candidate)
{
    const double dx = candidate.x - base.x;
    const double dy = candidate.y - base.y;

    // 就近吸附到水平或垂直方向
    if (std::abs(dx) >= std::abs(dy)) return Point2D(candidate.x, base.y);
    return Point2D(base.x, candidate.y);
}

Point2D SnapEngine::applyPolar(const Point2D& base, const Point2D& candidate,
                               double stepDeg, double angleToleranceDeg)
{
    const double dx = candidate.x - base.x;
    const double dy = candidate.y - base.y;
    const double distance = std::hypot(dx, dy);

    if (distance <= kEpsilon || stepDeg <= 0.0) return candidate;

    const double step = degToRad(stepDeg);
    const double angle = std::atan2(dy, dx);
    const double snapped = std::round(angle / step) * step;

    // 与吸附方向的偏差超过容差时不触发极轴
    double delta = normalizeAngle(angle - snapped);
    if (delta > kPi) delta -= kTwoPi;
    if (std::abs(delta) > degToRad(angleToleranceDeg)) return candidate;

    return Point2D(base.x + distance * std::cos(snapped),
                   base.y + distance * std::sin(snapped));
}

} // namespace lycad
