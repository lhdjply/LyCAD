// =============================================================================
//  LyCAD - core/DimensionEntity.h
//  尺寸标注图元（DXF: DIMENSION）
//
//  建模范围
//    · 类型：线性（旋转角可控）、对齐、半径、直径。角度标注暂不建模。
//    · 几何（尺寸线、界线、箭头、测量文字）不作为独立存储，而是由定义点
//      实时推导——这与 DXF 的语义一致：DIMENSION 实体只存定义点，
//      显示几何放在匿名块 *Dnn 中，两处必须由同一套公式生成。
//      推导逻辑集中在 geometry() 一处，渲染、DXF 块生成、拾取共用。
//
//  定义点与 DXF 组码的对应（随类型而不同，读写时注意）
//    线性/对齐：firstPoint=13/23  secondPoint=14/24  dimLinePoint=10/20
//    半径：     firstPoint=圆心 10/20   secondPoint=圆上点 15/25  dimLinePoint=文字位置 11/21
//    直径：     firstPoint=10/20  secondPoint=15/25（圆上一对对径点）dimLinePoint=11/21
// =============================================================================
#pragma once

#include "core/Entity.h"

#include <utility>

namespace lycad {

class DimensionEntity final : public Entity {
public:
    /// 标注类型（取值与 DXF 70 组码的低 3 位一致）
    enum class DimType {
        Linear    = 0,   ///< 线性（旋转角由 rotation() 指定）
        Aligned   = 1,   ///< 对齐
        Diametric = 3,   ///< 直径
        Radial    = 4    ///< 半径
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
};

} // namespace lycad
