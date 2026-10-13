// =============================================================================
//  LyCAD - core/MTextEntity.h
//  多行文字图元（DXF / DWG: MTEXT）
//
//  为什么单独建模（不再降级成 TEXT）
//
//    MTEXT 与 TEXT 是两种定位语义完全不同的对象：
//      · TEXT  由「第一/第二对齐点 + 72/73 对齐档」定位，垂直档量的是
//               **大写高度框**（顶部档贴大写高度顶、中部档取基线到大写高度的中点）；
//      · MTEXT 由「插入点 + 附着点(1~9) + 参考矩形宽度(41)」定位，中间行的
//               垂直基准是**基线到小写高度(x-height)的中点**，与 TEXT 的
//               Middle Center(72=1/73=2) 不是同一个量。
//    把 MTEXT 降级成 TEXT 会丢掉这一区别：文件在 AutoCAD / 浩辰里重新打开时，
//    每条文字都会整体偏移 (capHeight − xHeight)/2 ≈ 0.13 倍字高。所以这里把
//    MTEXT 作为独立图元保真往返，附着点语义不再经过 TEXT 的 72/73 转译。
//
//  内容与内联格式码
//
//    内容按**原始字符串**保存：`\P` 段落、`\f字体|b0|i0;`、`\H2.5x;`、`\C1;`
//    等内联码一字不改地原样往返（真实 CAD 打开时按原样解释）。屏幕显示只做
//    「尽力还原」：剥码取可见文本、识别段落对齐码与内联字体码，其余按正文绘制。
//
//  坐标约定
//
//    局部分标系：X 沿文字方向（阅读方向）、Y 为文字上方，原点在插入点。
//    排版结果（layout()）给出的都是这一坐标系下的量，渲染与包围盒共用同一份，
//    避免「模型一套、画出来另一套」。
// =============================================================================
#pragma once

#include "core/Entity.h"
#include "core/TextEntity.h"

#include <cmath>

namespace lycad {

class MTextEntity final : public Entity {
public:
    /// 附着点（DXF 71）：行 × 列 的九宫格，取值与规范一致（1~9）
    enum class Attachment {
        TopLeft = 1, TopCenter, TopRight,
        MiddleLeft, MiddleCenter, MiddleRight,
        BottomLeft, BottomCenter, BottomRight
    };

    /// 书写方向（DXF 72）
    enum class DrawingDirection {
        LeftToRight = 1,     ///< 从左到右
        TopToBottom = 3,     ///< 从上到下（竖排）
        ByStyle = 5          ///< 随文字样式
    };

    /// 行距样式（DXF 73）
    enum class LineSpacingStyle {
        AtLeast = 1,         ///< 「至少」：较高的字符会撑高行距
        Exact = 2            ///< 「固定」：较高的字符不会撑高行距
    };

    static constexpr EntityType kType = EntityType::MText;

    // ---- 字形比例（× 字高）----------------------------------------------------
    //
    // 与 TextEntity 的墨迹估算同一口径：DXF 字高 = 大写高度（cap height）。
    /// 小写高度 / 大写高度。中间行附着点的垂直基准就是它的一半（关键差异点）。
    static constexpr double kXHeightRatio = 0.74;
    /// 基线上方墨迹（大写高度 + CJK 出头）
    static constexpr double kAscentRatio = 1.05;
    /// 基线下方墨迹（下伸部）
    static constexpr double kDescentRatio = 0.30;
    /// 默认行距系数（× 字高）：DXF 44 为 1.0 时的「3-on-5」基准行距
    static constexpr double kLineSpacingFactor = 1.66;

    MTextEntity();
    MTextEntity(const QString& content, const Point2D& insertionPoint, double height = 3.5);

    // ---- 内容（原始字符串，含内联格式码）--------------------------------------
    const QString& content() const noexcept { return m_content; }
    void setContent(const QString& raw) { m_content = raw; }

    /// 剥掉内联格式码后的可见文本（段落以 '\n' 分段）——显示与编辑用
    QString plainText() const;
    /// 段落数（\P 或 \n 分段；至少 1）
    int paragraphCount() const noexcept;

    // ---- 排布参数（与 DXF 组码一一对应）---------------------------------------
    const Point2D& insertionPoint() const noexcept { return m_insertion; }
    void setInsertionPoint(const Point2D& p) noexcept { m_insertion = p; }

    /// 文字方向（单位向量，DXF 11 的 X 轴方向；同时决定旋转角）
    const Vector2D& direction() const noexcept { return m_direction; }
    void setDirection(const Vector2D& d);

    /// 挤出方向（DXF 210）。几何一律按「文字平面即 XY 平面、文字上方 = 方向逆时针转 90°」
    /// 计算，法向只做原样往返——翻转法向的 MTEXT 在图纸里罕见，且要正确渲染需要完整
    /// 的 OCS 支持，这里如实保留数值、不参与排版。
    struct Normal3D {
        double x = 0.0;
        double y = 0.0;
        double z = 1.0;
    };
    const Normal3D& normal() const noexcept { return m_normal; }
    void setNormal(const Normal3D& n) noexcept { m_normal = n; }
    /// 是否落在 WCS 平面（(0,0,±1)），排版计算的前提
    bool hasStandardNormal() const noexcept
    {
        return std::abs(m_normal.x) <= kGeomTol && std::abs(m_normal.y) <= kGeomTol;
    }

    /// 旋转角（弧度，逆时针为正）——由 direction 派生，与 DXF 50 一致
    double rotation() const noexcept;

    /// 字高（DXF 40，世界单位、大写高度）
    double height() const noexcept { return m_height; }
    void setHeight(double h);

    /// 参考矩形宽度（DXF 41）。这是**编辑时的折行宽度**：真实 CAD 在 MTEXT 编辑器里
    /// 依据它在段落内插入 `\P`，折行结果写在内容里；**显示时不再据此重排**（见 layout()）。
    /// 本字段按原值往返，0 表示"无宽度限制"。
    double referenceWidth() const noexcept { return m_referenceWidth; }
    void setReferenceWidth(double w) noexcept;

    Attachment attachment() const noexcept { return m_attachment; }
    void setAttachment(Attachment a) noexcept { m_attachment = a; }

    DrawingDirection drawingDirection() const noexcept { return m_drawingDirection; }
    void setDrawingDirection(DrawingDirection d) noexcept { m_drawingDirection = d; }

    LineSpacingStyle lineSpacingStyle() const noexcept { return m_lineSpacingStyle; }
    void setLineSpacingStyle(LineSpacingStyle s) noexcept { m_lineSpacingStyle = s; }

    /// 行距系数（DXF 44；1.0 = 默认 3-on-5 行距）
    double lineSpacingFactor() const noexcept { return m_lineSpacingFactor; }
    void setLineSpacingFactor(double f) noexcept;

    /// 字符实际范围（DXF 42/43，输出型字段：由生成方写入，按原值往返）
    double extentsWidth() const noexcept { return m_extentsWidth; }
    double extentsHeight() const noexcept { return m_extentsHeight; }
    void setExtents(double w, double h) noexcept;

    /// 文字样式名（DXF 7）
    const QString& fontStyle() const noexcept { return m_fontStyle; }
    void setFontStyle(const QString& style) { m_fontStyle = style.trimmed(); }

    /// 内联字体码（`\f字体|b…;`）声明的字体名；空表示内容里没有该码
    QString inlineFontName() const;

    /// 竖直方向的行距（世界单位）：行距系数 × 基准行距
    double lineHeight() const noexcept;

    // ---- 内联格式（\f 的粗/斜/字体、\C 颜色、\H 字高、\W 宽度、\Q 倾斜、\T 字距）----
    //
    //  MTEXT 的一段文字可以用内联码改变自身格式，例如
    //      \fArial|b1;加粗\fArial|b0; 与斜体 \fArial|i1;Italic\fArial|i0;
    //      \C1;红\C3;绿\C5;蓝
    //  这些码是**行内**生效的（只影响其后、且受 `{…}` 分组约束的那一段文字），
    //  所以「一行」可以被切成若干**同格式片段（Run）**。下面这组类型就是该切分结果，
    //  排版（行宽）与渲染（逐段设字体/颜色）共用同一份，避免两边各解析一遍。
    //
    /// 一个 Run 的格式快照。所有量都以「相对图元自身」为准。
    struct RunStyle {
        bool    bold = false;
        bool    italic = false;
        bool    hasColor = false;      ///< `\C` 给出了有效的 ACI 索引
        int     colorAci = 0;          ///< `\C` 的 ACI 索引（0/256 = 随块/随层，视作未着色）
        QString fontFamily;            ///< `\f` 的字体名；空 = 未指定
        double  heightFactor = 1.0;    ///< 相对图元字高的倍数（`\H2.5x;` 直接给 2.5，`\H5;` 按绝对高换算）
        double  widthFactor = 1.0;     ///< `\W`：宽度因子
        double  obliqueDeg = 0.0;      ///< `\Q`：倾斜角（度）
        double  tracking = 1.0;        ///< `\T`：字距因子（1.0 = 常规）

        /// 是否为「无格式」（可以走单段快路径）
        bool isPlain() const noexcept
        {
            return !bold && !italic && !hasColor && fontFamily.isEmpty()
                && std::abs(heightFactor - 1.0) <= 1e-9
                && std::abs(widthFactor - 1.0) <= 1e-9
                && std::abs(obliqueDeg) <= 1e-9
                && std::abs(tracking - 1.0) <= 1e-9;
        }
    };

    /// 行内的一段同格式文本。offsetX / width 由 parseRuns 按图元字高算好，
    /// 与渲染用的位置、包围盒用的行宽是同一份数据。
    struct Run {
        QString  text;              ///< 可见文本（内联码已剥除）
        double   offsetX = 0.0;     ///< 行内起始 X（局部坐标，世界单位）
        double   width = 0.0;       ///< 该段宽度（世界单位）
        RunStyle style;
    };

    // ---- 排版 -----------------------------------------------------------------
    /// 一行可见文本及其在局部坐标系里的起始 X（沿文字方向，自插入点起算）
    struct LayoutLine {
        QString text;           ///< 该行可见文本（各 Run 拼接；无内联格式时即整行）
        double x = 0.0;         ///< 起始 X（局部坐标）
        double width = 0.0;     ///< 行宽（世界单位）
        std::vector<Run> runs;  ///< 带格式的片段（含 offsetX / width）；每段的文本拼起来 = text
    };

    /// 排版结果：分行 + 行起点 + 首行基线与块的墨迹范围（局部坐标系）
    struct Layout {
        std::vector<LayoutLine> lines;
        double lineHeight = 0.0;      ///< 行距
        double firstBaseline = 0.0;   ///< 首行基线的 Y（局部坐标，向上为正）
        double minX = 0.0;            ///< 块的水平范围（局部坐标）
        double maxX = 0.0;
        double minY = 0.0;            ///< 块的垂直范围（含墨迹，局部坐标）
        double maxY = 0.0;
        bool valid = false;
    };

    /// 按内容的段落分隔（`\P`）分行、按附着点与段落对齐码定位的排版结果
    /// （渲染、包围盒、拾取共用）。
    ///
    /// **不依据参考栏宽（41）重新折行**——真实 CAD 打开文件时按内容里的 `\P` 原样显示，
    /// 折行只在编辑时发生（见 referenceWidth 的说明）。
    Layout layout() const;

    /// 块的外接四角（世界坐标，含旋转）——包围盒 / 拾取 / 夹点共用
    std::vector<Point2D> outlineCorners() const;

    /// 附着点 -> 行（0 上 / 1 中 / 2 下）、列（0 左 / 1 中 / 2 右）
    static int attachmentRow(Attachment a) noexcept;
    static int attachmentColumn(Attachment a) noexcept;
    /// 行列 -> 附着点（列 0~2，行 0~2）
    static Attachment attachmentFromRowColumn(int row, int column) noexcept;
    /// 附着点的中文名（属性面板与命令行使用）
    static QString attachmentDisplayName(Attachment a);

    // ---- 内联格式码（导入与渲染共用的静态解析）--------------------------------
    /// 剥掉内联格式码：`\P` -> '\n'，`{…}` 剥除，`\f…;`/`\H…;`/`\W…;`/`\C…;`
    /// /`\p…;`/`\A…;`/`\S…;` 等整段丢弃；遇到没有分号的 `\` 按字面保留。
    static QString stripInlineCodes(const QString& raw);
    /// 每段的水平对齐：0 左 / 1 中 / 2 右；该段没有对齐码时为 -1（由附着点的列决定）
    static std::vector<int> paragraphJustifications(const QString& raw);
    /// `\f…;` 里的字体名（第 1 段）；没有该码返回空串
    static QString inlineFontNameOf(const QString& raw);

    /// 按内联格式码把内容切成**逐段的格式片段**：外层按段落（`\P` / 换行）分段、
    /// 内层是同格式的 Run。每一段的 Run 文本拼起来即该段的可见文本，
    /// `offsetX` / `width` 已按 `baseHeight`（图元字高）算好。
    ///
    /// 与 stripInlineCodes 的关系：后者只给「看什么字」，本函数还给「用什么格式看」。
    /// 两者对可见文本的口径一致（`{`/`}` 分组不显示、无分号的 `\` 按字面保留、
    /// `\~` 作空格）。排版与渲染都走这一份结果，因此屏幕上的行宽与包围盒不会两算。
    ///
    /// 支持的码：`\f`/`\F`（字体名 + `|b0|b1` 粗体 + `|i0|i1` 斜体）、`\C`（ACI 颜色）、
    /// `\H`（字高，可带 `x` 表示倍数）、`\W`（宽度因子）、`\Q`（倾斜角）、`\T`（字距）、
    /// `\S`（堆叠，按 `上/下` 平铺显示）；`{`/`}` 分组、`\P` 段落、`\\`/`\{`/`\}`/`\~` 转义。
    /// 其余码（`\A` 对齐、`\p` 段落属性、`\L\O\K` 装饰、`\X` 等）按无格式剥除。
    static std::vector<std::vector<Run>> parseRuns(const QString& raw, double baseHeight);

    // ---- Entity 接口 ----------------------------------------------------------
    std::unique_ptr<Entity> clone() const override;
    Box2D boundingBox() const override;
    double distanceTo(const Point2D& p) const override;
    void transform(const Transform2D& t) override;
    double length() const override;
    std::vector<Point2D> snapPoints() const override;
    std::vector<Point2D> gripPoints() const override;
    bool moveGrip(std::size_t index, const Point2D& newPos) override;
    std::vector<Point2D> flatten(double sagitta) const override;
    bool isClosed() const override { return false; }
    QString describe() const override;
    QString geometrySummary() const override;

private:
    QString   m_content;
    Point2D   m_insertion;
    Vector2D  m_direction { 1.0, 0.0 };
    Normal3D  m_normal;
    double    m_height = 3.5;
    double    m_referenceWidth = 0.0;
    Attachment m_attachment = Attachment::TopLeft;
    DrawingDirection m_drawingDirection = DrawingDirection::LeftToRight;
    LineSpacingStyle m_lineSpacingStyle = LineSpacingStyle::AtLeast;
    double    m_lineSpacingFactor = 1.0;
    double    m_extentsWidth = 0.0;
    double    m_extentsHeight = 0.0;
    QString   m_fontStyle;
};

} // namespace lycad
