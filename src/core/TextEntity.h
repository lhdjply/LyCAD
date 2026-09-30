// =============================================================================
//  LyCAD - core/TextEntity.h
//  文字图元（DXF: TEXT）
//
//  对齐约定（与 DXF 72/73 组码一致）
//    · 水平对齐 halign：0 左 / 1 居中 / 2 右
//    · 垂直对齐 valign：0 基线 / 1 底部 / 2 中部 / 3 顶部
//    · 当且仅当 (halign, valign) != (左, 基线) 时，定位点使用第二对齐点
//      （DXF 11/21），否则使用第一对齐点（10/20）。两个字段均被完整保留，
//      以保证文件往返后对齐方式不漂移。
// =============================================================================
#pragma once

#include "core/Entity.h"

namespace lycad {

class TextEntity final : public Entity {
public:
    /// 水平对齐（对应 DXF 72）
    enum class HAlign { Left = 0, Center = 1, Right = 2 };
    /// 垂直对齐（对应 DXF 73）
    enum class VAlign { Baseline = 0, Bottom = 1, Middle = 2, Top = 3 };

    TextEntity();
    explicit TextEntity(const QString& content,
                        const Point2D& position,
                        double height = 3.5,
                        double rotationRad = 0.0);

    // ---- 内容 ----------------------------------------------------------------
    const QString& content() const noexcept { return m_content; }
    void setContent(const QString& text) { m_content = text; }

    /// 单行显示所用的文本（去掉换行）
    QString displayString() const;

    // ---- 几何参数 --------------------------------------------------------------
    const Point2D& position() const noexcept { return m_position; }
    void setPosition(const Point2D& p) noexcept { m_position = p; }

    const Point2D& alignmentPoint() const noexcept { return m_alignmentPoint; }
    void setAlignmentPoint(const Point2D& p) noexcept { m_alignmentPoint = p; }

    /// 字高（大写字母高度，世界单位，> 0）
    double height() const noexcept { return m_height; }
    void setHeight(double h);

    /// 旋转角（弧度，逆时针为正）
    double rotation() const noexcept { return m_rotation; }
    void setRotation(double radians) noexcept { m_rotation = normalizeAngle(radians); }

    HAlign hAlign() const noexcept { return m_hAlign; }
    void setHAlign(HAlign a) noexcept { m_hAlign = a; }
    VAlign vAlign() const noexcept { return m_vAlign; }
    void setVAlign(VAlign a) noexcept { m_vAlign = a; }

    /// 是否使用第二对齐点（非 左/基线 对齐时）
    bool usesAlignmentPoint() const noexcept
    {
        return m_hAlign != HAlign::Left || m_vAlign != VAlign::Baseline;
    }

    /// 实际参与定位的点（对齐规则消解后的结果）
    Point2D effectivePosition() const noexcept
    {
        return usesAlignmentPoint() ? m_alignmentPoint : m_position;
    }

    /// 单位字符宽度估计（CJK 记 1.0，ASCII 记 0.55，单位为字高）
    double estimatedWidth() const noexcept;

    /// 旋转后的文字矩形四角（未对齐到包围盒，仅几何）
    std::vector<Point2D> outlineCorners() const;

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
    bool isClosed() const override { return true; }
    QString describe() const override;
    QString geometrySummary() const override;

    static constexpr EntityType kType = EntityType::Text;

private:
    QString   m_content;
    Point2D   m_position;                       ///< 第一对齐点（DXF 10/20）
    Point2D   m_alignmentPoint;                 ///< 第二对齐点（DXF 11/21）
    double    m_height = 3.5;
    double    m_rotation = 0.0;                 ///< 弧度
    HAlign    m_hAlign = HAlign::Left;
    VAlign    m_vAlign = VAlign::Baseline;
};

} // namespace lycad
