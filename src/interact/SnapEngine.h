// =============================================================================
//  LyCAD - interact/SnapEngine.h
//  对象捕捉（OSNAP）、栅格吸附、正交与极轴追踪。
//
//  捕捉优先级（同一光标位置存在多个候选时，序号小者胜出）
//      1 端点 / 节点      5 象限点
//      2 交点             6 垂足
//      3 圆心             7 最近点
//      4 中点             8 栅格点
//  优先级位于对象捕捉之上：只要某个高优先级候选落在光圈（aperture）内，
//  低优先级候选即便更靠近光标也不会被选中。这与 AutoCAD 的行为一致，
//  因为「端点」的工程意义远大于「最近点」。
//
//  正交与极轴不是「捕捉到某个已有点」，而是对当前点的方向约束，
//  因此与对象捕捉构成两级处理：先求对象捕捉结果，再做方向约束。
//  但二者同时启用时，对象捕捉优先——用户明确指到某个端点时，
//  不应被正交模式拉走。
// =============================================================================
#pragma once

#include "core/Types.h"
#include "render/CanvasTypes.h"

#include <optional>
#include <vector>

namespace lycad {

class Document;

/// 各类捕捉的开关与参数
struct SnapSettings {
    bool enabled = true;

    bool endpoint = true;
    bool midpoint = true;
    bool center = true;
    bool quadrant = true;
    bool intersection = true;
    bool perpendicular = false;     ///< 默认关闭：需要基准点，且易与最近点混淆
    bool nearest = false;           ///< 默认关闭：连续吸附会干扰精确作图
    bool node = true;
    bool grid = false;

    /// 光圈半径（像素）。光标与候选点的屏幕距离在此范围内才考虑捕捉。
    double aperturePixels = 12.0;

    /// 栅格吸附的格距（世界单位）。通常与画布显示的栅格间距保持一致。
    double gridSpacing = 10.0;

    bool anyObjectSnapEnabled() const
    {
        return enabled && (endpoint || midpoint || center || quadrant
                           || intersection || perpendicular || nearest || node);
    }
};

class SnapEngine {
public:
    explicit SnapEngine(const SnapSettings& settings);

    /**
     * 求最优捕捉点。
     * @param document    当前文档
     * @param worldCursor 光标的世界坐标
     * @param apertureWorld 光圈半径（世界单位，由像素换算而来）
     * @param basePoint   基准点（用于垂足捕捉与极轴追踪），可为空
     * @param exclude     需要排除的图元（例如正在绘制中的临时图元）
     */
    std::optional<SnapResult> resolve(const Document& document,
                                      const Point2D& worldCursor,
                                      double apertureWorld,
                                      const std::optional<Point2D>& basePoint = std::nullopt,
                                      const std::vector<EntityId>& exclude = {}) const;

    /// 正交约束：把候选点投影到自基准点出发的水平或垂直方向
    static Point2D applyOrtho(const Point2D& base, const Point2D& candidate);

    /// 极轴追踪：吸附到以 base 为中心、步距为 stepDeg 的射线上
    static Point2D applyPolar(const Point2D& base, const Point2D& candidate,
                              double stepDeg, double angleToleranceDeg = 4.0);

    const SnapSettings& settings() const noexcept { return m_settings; }

private:
    /// 候选点及其优先级
    struct Candidate {
        Point2D  position;
        SnapKind kind;
        int      priority;
    };

    void collectEntityCandidates(const Document& document, const Point2D& cursor,
                                 double aperture, std::vector<Candidate>& out,
                                 std::vector<EntityId>& nearby) const;

    void collectIntersections(const Document& document, const Point2D& cursor,
                              double aperture, const std::vector<EntityId>& nearby,
                              std::vector<Candidate>& out) const;

    SnapSettings m_settings;
};

} // namespace lycad
