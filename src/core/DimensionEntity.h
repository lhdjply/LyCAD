// =============================================================================
//  LyCAD - core/DimensionEntity.h
//  尺寸标注图元（DXF: DIMENSION）
//
//  建模范围
//    · 类型：线性（旋转角可控）、对齐、半径、直径、**坐标（DIMORDINATE）**、
//      **弧长（DIMARC / DXF 的 ARC_DIMENSION）**。角度标注暂不建模。
//    · 几何（尺寸线、界线、箭头、测量文字）不作为独立存储，而是由定义点
//      实时推导——这与 DXF 的语义一致：DIMENSION 实体只存定义点，
//      显示几何放在匿名块 *Dnn 中，两处必须由同一套公式生成。
//      推导逻辑集中在 geometry() 一处，渲染、DXF 块生成、拾取共用。
//
//  定义点与 DXF 组码的对应（随类型而不同，读写时注意）
//    线性/对齐：firstPoint=13/23  secondPoint=14/24  dimLinePoint=10/20
//    半径：     firstPoint=圆心 10/20   secondPoint=圆上点 15/25  dimLinePoint=文字位置 11/21
//    直径：     firstPoint=10/20  secondPoint=15/25（圆上一对对径点）dimLinePoint=11/21
//    坐标：     firstPoint=被测点 13/23  secondPoint=引线端点 14/24
//              dimLinePoint=坐标基准原点 10/20  textPoint=文字位置 11/21
//    弧长：     firstPoint=被测弧起点 13/23  secondPoint=被测弧终点 14/24
//              arcCenter=弧心 15/25  arcStart/arcEnd=起止参数（弧度，40/41）
//              dimLinePoint=尺寸弧经过的点 10/20  textPoint=文字位置 11/21
// =============================================================================
#pragma once

#include "core/Entity.h"

#include <optional>

#include <utility>

namespace lycad {

class DimensionEntity final : public Entity {
public:
    /// 标注类型。前五个取值与 DXF 70 组码的低 3 位一致；弧长标注是后加的
    /// 类型，AutoCAD 用 bit3（值 8）表达它，DXF 里写作独立实体 `ARC_DIMENSION`。
    enum class DimType {
        Linear    = 0,   ///< 线性（旋转角由 rotation() 指定）
        Aligned   = 1,   ///< 对齐
        Diametric = 3,   ///< 直径
        Radial    = 4,   ///< 半径
        Ordinate  = 6,   ///< 坐标（X 型或 Y 型，见 isOrdinateXType()）
        ArcLength = 8    ///< 弧长（DIMARC）
    };

    /// 箭头三角形（世界坐标）
    struct Arrow {
        Point2D tip;
        Point2D base1;
        Point2D base2;
    };

    /// 推导出的显示几何（渲染 / DXF 匿名块 / 拾取共用同一份结果）
    struct Geometry {
        std::vector<std::pair<Point2D, Point2D>> segments;  ///< 界线 + 尺寸线 + 引线
        std::vector<Arrow>        arrows;                   ///< 实心箭头
        Point2D                   textAnchor;               ///< 测量文字的定位中点
        double                    textAngle = 0.0;          ///< 文字方向（弧度，已做可读性翻转）
        QString                   text;                     ///< 最终显示的文字
        double                    textHeight = 3.5;
        bool                      valid = false;
    };

    DimensionEntity();
    explicit DimensionEntity(DimType type);

    // ---- 类型与定义点 ----------------------------------------------------------
    DimType dimType() const noexcept { return m_dimType; }
    void setDimType(DimType type) noexcept { m_dimType = type; }

    const Point2D& firstPoint() const noexcept { return m_firstPoint; }
    void setFirstPoint(const Point2D& p) noexcept { m_firstPoint = p; }
    const Point2D& secondPoint() const noexcept { return m_secondPoint; }
    void setSecondPoint(const Point2D& p) noexcept { m_secondPoint = p; }
    const Point2D& dimLinePoint() const noexcept { return m_dimLinePoint; }
    void setDimLinePoint(const Point2D& p) noexcept { m_dimLinePoint = p; }

    /// 尺寸线旋转角（弧度；仅线性标注有效）
    double rotation() const noexcept { return m_rotation; }
    void setRotation(double radians) noexcept { m_rotation = normalizeAngle(radians); }

    // ---- 坐标标注（DXF: DIMORDINATE） ------------------------------------------
    /// 是否为 X 型坐标标注：量被测点的 X 坐标；否则量 Y 坐标。
    /// 对应 DXF 70 的 bit6（值 64）：置位为 X 型。
    bool isOrdinateXType() const noexcept { return m_ordinateX; }
    void setOrdinateXType(bool xType) noexcept { m_ordinateX = xType; }

    // ---- 弧长标注（DXF: ARC_DIMENSION / DIMARC） --------------------------------
    /// 被测弧的圆心（DXF 15/25）
    const Point2D& arcCenter() const noexcept { return m_arcCenter; }
    void setArcCenter(const Point2D& p) noexcept { m_arcCenter = p; }

    /// 被测弧的起止参数（弧度，DXF 40/41）。与 ArcEntity 同一口径：逆时针自
    /// arcStartAngle() 到 arcEndAngle()，跨度 ∈ (0, 2π]。
    double arcStartAngle() const noexcept { return m_arcStartAngle; }
    double arcEndAngle() const noexcept { return m_arcEndAngle; }
    void setArcAngles(double startRad, double endRad) noexcept
    {
        m_arcStartAngle = normalizeAngle(startRad);
        m_arcEndAngle = normalizeAngle(endRad);
    }

    /// 被测弧半径（由「弧心 -> firstPoint」推出，不单独存）
    double measuredArcRadius() const noexcept
    {
        return m_arcCenter.distanceTo(m_firstPoint);
    }

    // ---- 测量文字的定位点（DXF 11/21） -----------------------------------------
    //  坐标标注与弧长标注都要用到它；线性/对齐/半径/直径仍按各自公式推文字位置，
    //  因此这里保持「可选」：文件里给了就用，没给就按类型推导。
    const std::optional<Point2D>& textPoint() const noexcept { return m_textPoint; }
    void setTextPoint(const Point2D& p) { m_textPoint = p; }
    void clearTextPoint() noexcept { m_textPoint.reset(); }

    /// 测量文字覆盖。空串或 "<>" 表示使用自动测量值
    const QString& textOverride() const noexcept { return m_textOverride; }
    void setTextOverride(const QString& text) { m_textOverride = text.trimmed(); }

    double textHeight() const noexcept { return m_textHeight; }
    void setTextHeight(double h);
    double arrowLength() const noexcept { return m_textHeight; }

    // ---- 几何推导 --------------------------------------------------------------
    /// 测量值（线性/对齐为长度，半径为半径值，直径为直径值）
    double measurement() const noexcept;

    /// 自动测量文字（半径带 R 前缀，直径带 Ø 前缀）
    QString autoText() const;

    /// 最终显示文字（考虑覆盖）。dxfStyle 为 true 时 Ø 转为 DXF 的 %%c
    QString displayText(bool dxfStyle = false) const;

    /// 由定义点推导全部显示几何
    Geometry geometry() const;

    // ---- Entity 接口 ---------------------------------------------------------
    std::unique_ptr<Entity> clone() const override;
    Box2D boundingBox() const override;
    double distanceTo(const Point2D& p) const override;
    void transform(const Transform2D& t) override;
    double length() const override;
    std::vector<Point2D> snapPoints() const override;
    std::vector<Point2D> gripPoints() const override;
    bool moveGrip(std::size_t index, const Point2D& newPos) override;
    std::vector<Point2D> flatten(double sagitta) const override;
    QString describe() const override;
    QString geometrySummary() const override;

    static constexpr EntityType kType = EntityType::Dimension;

private:
    DimType   m_dimType = DimType::Aligned;
    Point2D   m_firstPoint;
    Point2D   m_secondPoint;
    Point2D   m_dimLinePoint;
    double    m_rotation = 0.0;              ///< 弧度（线性标注）
    QString   m_textOverride;
    double    m_textHeight = 3.5;

    bool      m_ordinateX = true;            ///< 坐标标注：X 型 / Y 型
    Point2D   m_arcCenter;                   ///< 弧长标注：被测弧圆心
    double    m_arcStartAngle = 0.0;         ///< 弧长标注：起参数（弧度）
    double    m_arcEndAngle = 0.0;           ///< 弧长标注：止参数（弧度）
    std::optional<Point2D> m_textPoint;      ///< DXF 11/21（坐标、弧长用）
};

} // namespace lycad
