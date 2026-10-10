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

    // ---- 字形范围比例（与渲染器 kCapHeightToEmRatio 的换算保持一致） ----------
    //
    // 渲染器把 DXF 字高 h 解释为大写字母高度：em = h / 0.72。
    // 据此，字形墨迹相对基线的范围为：
    //   上方（大写高度 + CJK 出头）≈ +1.35h，下方（下伸部）≈ -0.35h。
    // 估算刻意取宽松值，保证选择框/包围盒完整罩住实际字形。
    static constexpr double kInkAboveBaseline = 1.45;   ///< 基线上方墨迹（× 字高）
    static constexpr double kInkBelowBaseline = 0.40;   ///< 基线下方墨迹（× 字高）
    /// 垂直对齐换算中用到的字体内距比例（× 字高）
    static constexpr double kDescentRatio = 0.30;
    static constexpr double kAscentRatio  = 1.05;
    /// 多行文字的行距系数（包围盒估算用；渲染器用 QFont 真实行距绘制）
    static constexpr double kLineSpacingFactor = 1.66;

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

    /// 内容行数（\n 分段；MTEXT 降级的多行文字 ≥ 2）
    int lineCount() const noexcept;

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

    // ---- 生成标志与宽度因子（DXF 71 / 41） ------------------------------------
    //
    // 「背面文字」（如 PCB 底面丝印）在 DXF 中以生成标志表达：
    //   bit 2（值 2）= 水平镜像（backwards），bit 4（值 4）= 垂直镜像（upside-down）。
    // 宽度因子为字符宽度与字高之比；负值同样表示水平镜像（读入时归一为正因子 + 镜像标志）。

    bool isBackwards() const noexcept { return m_backwards; }
    void setBackwards(bool on) noexcept { m_backwards = on; }

    bool isUpsideDown() const noexcept { return m_upsideDown; }
    void setUpsideDown(bool on) noexcept { m_upsideDown = on; }

    double widthFactor() const noexcept { return m_widthFactor; }
    void setWidthFactor(double factor);

    /// DXF 文字样式名（如 "黑体"/"Arial"）。渲染时若系统存在同名字体则采用。
    const QString& fontStyle() const noexcept { return m_fontStyle; }
    void setFontStyle(const QString& style) { m_fontStyle = style.trimmed(); }

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

    /// 单位字符宽度估计（CJK 记 1.0，ASCII 记 0.55，单位为字高；含宽度因子）
    double estimatedWidth() const noexcept;

    /// 单行文本的估算宽度（世界单位）。宽度按「字宽表逐字符累加 × 字高 × 宽度因子」
    /// 计算，与本类 estimatedWidth() 同一口径；MTextEntity 的包围盒与行在参考矩形内的
    /// 对齐位置也用它，保证模型层（包围盒、拾取）与渲染层不会各算一套。
    static double estimateLineWidth(const QString& line, double height, double widthFactor = 1.0);

    /// 基线相对锚点的世界偏移（向上为正；与渲染器各对齐档的偏移换算一致）
    double baselineOffsetWorld() const noexcept;

    /// 旋转后的文字矩形四角（覆盖实际渲染的字形范围，含镜像与宽度因子）
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
    bool      m_backwards = false;              ///< DXF 71 bit2：水平镜像
    bool      m_upsideDown = false;             ///< DXF 71 bit4：垂直镜像
    double    m_widthFactor = 1.0;              ///< DXF 41
    QString   m_fontStyle;                      ///< DXF 7
};

} // namespace lycad
