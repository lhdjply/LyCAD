// =============================================================================
//  LyCAD - core/MTextEntity.cpp
// =============================================================================
#include "core/MTextEntity.h"

#include "core/Format.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lycad {

namespace {

/// 段落分隔（stripInlineCodes 把 \P 归一成它）
constexpr QChar kParagraph = QLatin1Char('\n');

/// 单字符步进宽度（世界单位）。与 TextEntity 共用同一套字宽表，
/// 保证「模型算出的折行 / 包围盒」与「渲染按同一份结果绘制」不会各算一套。
double charAdvance(QChar ch, double height)
{
    return TextEntity::estimateLineWidth(QString(ch), height, 1.0);
}

/// 按给定宽度把一段文本折行（CJK 可任意断行，拉丁优先断在最后一个空格处）。
///
/// **显示不走这里**：MTEXT 的换行位置由内容里的 `\P` 决定（见 layout 的说明），
/// 打开文件时不再按参考栏宽重新折行。这个函数保留给"编辑时的折行"用——真实 CAD 在
/// MTEXT 编辑器里依据栏宽实时折行并把结果写成 `\P`，本软件提供 MTEXT 编辑时可直接复用。
[[maybe_unused]] std::vector<QString> wrapParagraph(const QString& text, double limit, double height)
{
    if (text.isEmpty()) return { QString() };
    if (limit <= kGeomTol) return { text };

    std::vector<QString> lines;
    QString current;
    double width = 0.0;
    int lastSpace = -1;
    for (const QChar ch : text) {
        const double advance = charAdvance(ch, height);
        if (width + advance > limit && !current.isEmpty()) {
            QString overflow;
            if (lastSpace > 0) {                    // 断在空格处，空格归上一行
                overflow = current.mid(lastSpace + 1);
                current.truncate(lastSpace);
            }
            lines.push_back(current);
            current = overflow;
            width = TextEntity::estimateLineWidth(current, height, 1.0);
            lastSpace = -1;
        }
        current.append(ch);
        if (ch == QLatin1Char(' ')) lastSpace = current.size() - 1;
        width += advance;
    }
    lines.push_back(current);
    return lines;
}

/// 把正文按段落切开（`\P` 与裸换行都算段落分隔）
std::vector<QString> splitParagraphs(const QString& plain)
{
    std::vector<QString> paragraphs;
    int start = 0;
    for (int i = 0; i <= plain.size(); ++i) {
        if (i < plain.size() && plain.at(i) != kParagraph) continue;
        paragraphs.push_back(plain.mid(start, i - start));
        start = i + 1;
    }
    return paragraphs;
}

} // namespace

// -----------------------------------------------------------------------------
//  构造
// -----------------------------------------------------------------------------
MTextEntity::MTextEntity()
    : Entity(EntityType::MText)
{
}

MTextEntity::MTextEntity(const QString& content, const Point2D& insertionPoint, double height)
    : Entity(EntityType::MText)
    , m_content(content)
    , m_insertion(insertionPoint)
    , m_height(height > kGeomTol ? height : 3.5)
{
}

// -----------------------------------------------------------------------------
//  内容
// -----------------------------------------------------------------------------
QString MTextEntity::plainText() const
{
    return stripInlineCodes(m_content);
}

int MTextEntity::paragraphCount() const noexcept
{
    int count = 1;
    for (int i = 0; i < m_content.size(); ++i) {
        const QChar ch = m_content.at(i);
        if (ch == QLatin1Char('\n')) {
            ++count;
        } else if (ch == QLatin1Char('\\') && i + 1 < m_content.size()
                   && m_content.at(i + 1) == QLatin1Char('P')) {
            ++count;
            ++i;
        }
    }
    return count;
}

QString MTextEntity::inlineFontName() const
{
    return inlineFontNameOf(m_content);
}

// -----------------------------------------------------------------------------
//  排布参数
// -----------------------------------------------------------------------------
void MTextEntity::setDirection(const Vector2D& d)
{
    const double len = d.length();
    if (len <= kGeomTol) return;            // 退化方向忽略，保持原值
    m_direction = Vector2D(d.x / len, d.y / len);
}

double MTextEntity::rotation() const noexcept
{
    return normalizeAngle(m_direction.angle());
}

void MTextEntity::setHeight(double h)
{
    if (h > kGeomTol) m_height = h;
}

void MTextEntity::setReferenceWidth(double w) noexcept
{
    // 负宽度无意义（AutoCAD 用 0 表示「不折行」）
    m_referenceWidth = w > kGeomTol ? w : 0.0;
}

void MTextEntity::setLineSpacingFactor(double f) noexcept
{
    m_lineSpacingFactor = std::clamp(f, 0.25, 4.00);   // DXF 44 的合法范围
}

void MTextEntity::setExtents(double w, double h) noexcept
{
    m_extentsWidth = w > 0.0 ? w : 0.0;
    m_extentsHeight = h > 0.0 ? h : 0.0;
}

double MTextEntity::lineHeight() const noexcept
{
    return m_lineSpacingFactor * kLineSpacingFactor * m_height;
}

int MTextEntity::attachmentRow(Attachment a) noexcept
{
    return (static_cast<int>(a) - 1) / 3;        // 0 上 / 1 中 / 2 下
}

int MTextEntity::attachmentColumn(Attachment a) noexcept
{
    return (static_cast<int>(a) - 1) % 3;        // 0 左 / 1 中 / 2 右
}

MTextEntity::Attachment MTextEntity::attachmentFromRowColumn(int row, int column) noexcept
{
    return static_cast<Attachment>(std::clamp(row, 0, 2) * 3 + std::clamp(column, 0, 2) + 1);
}

QString MTextEntity::attachmentDisplayName(Attachment a)
{
    static const char* kNames[3][3] = {
        { "左上", "中上", "右上" },
        { "左中", "正中", "右中" },
        { "左下", "中下", "右下" },
    };
    return QString::fromUtf8(kNames[attachmentRow(a)][attachmentColumn(a)]);
}

// -----------------------------------------------------------------------------
//  内联格式码解析
// -----------------------------------------------------------------------------
QString MTextEntity::stripInlineCodes(const QString& raw)
{
    QString out;
    out.reserve(raw.size());
    for (int i = 0; i < raw.size(); ++i) {
        const QChar ch = raw.at(i);
        if (ch != QLatin1Char('\\')) {
            if (ch == QLatin1Char('{') || ch == QLatin1Char('}')) continue;   // 分组括号剥除
            out.append(ch == QLatin1Char('\r') ? kParagraph : ch);
            continue;
        }
        if (i + 1 >= raw.size()) break;
        const QChar next = raw.at(i + 1);
        if (next == QLatin1Char('\\')) { out.append(QLatin1Char('\\')); ++i; continue; }
        if (next == QLatin1Char('{') || next == QLatin1Char('}')) { out.append(next); ++i; continue; }
        if (next == QLatin1Char('~')) { out.append(QLatin1Char(' ')); ++i; continue; }  // 不换行空格
        if (next == QLatin1Char('P')) { out.append(kParagraph); ++i; continue; }        // 段落
        int j = i + 1;
        while (j < raw.size() && raw.at(j) != QLatin1Char(';')) ++j;
        if (j < raw.size()) { i = j; continue; }   // 到分号为止整段剥除
        out.append(ch);                            // 没有分号：按字面保留
    }
    return out;
}

std::vector<int> MTextEntity::paragraphJustifications(const QString& raw)
{
    // 按 \P 切段，逐段找 \p…q<l|c|r|j|d>…;。段落对齐优先于附着点的隐含对齐。
    std::vector<QString> paragraphs;
    QString current;
    for (int i = 0; i < raw.size(); ++i) {
        if (raw.at(i) == QLatin1Char('\\') && i + 1 < raw.size()
            && raw.at(i + 1) == QLatin1Char('P')) {
            paragraphs.push_back(current);
            current.clear();
            ++i;
            continue;
        }
        current.append(raw.at(i));
    }
    paragraphs.push_back(current);

    std::vector<int> result;
    result.reserve(paragraphs.size());
    for (const QString& paragraph : paragraphs) {
        int alignment = -1;
        for (int i = 0; i + 2 < paragraph.size() && alignment < 0; ++i) {
            if (paragraph.at(i) != QLatin1Char('\\') || paragraph.at(i + 1) != QLatin1Char('p')) {
                continue;
            }
            int end = i + 2;
            while (end < paragraph.size() && paragraph.at(end) != QLatin1Char(';')) ++end;
            for (int k = i + 2; k + 1 < end; ++k) {
                if (paragraph.at(k) != QLatin1Char('q')) continue;
                switch (paragraph.at(k + 1).toLatin1()) {
                case 'l': alignment = 0; break;
                case 'c': alignment = 1; break;
                case 'r': alignment = 2; break;
                // 两端/分散对齐在绘制上没有对应画法，居中是最接近的近似
                case 'j':
                case 'd': alignment = 1; break;
                default: break;
                }
                if (alignment >= 0) break;
            }
            i = end;
        }
        result.push_back(alignment);
    }
    return result;
}

QString MTextEntity::inlineFontNameOf(const QString& raw)
{
    for (int i = 0; i + 2 < raw.size(); ++i) {
        if (raw.at(i) != QLatin1Char('\\') || raw.at(i + 1) != QLatin1Char('f')) continue;
        int end = i + 2;
        while (end < raw.size() && raw.at(end) != QLatin1Char(';')) ++end;
        const QStringList parts = raw.mid(i + 2, end - i - 2).split(QLatin1Char('|'));
        if (!parts.isEmpty() && !parts.first().trimmed().isEmpty()) {
            return parts.first().trimmed();
        }
        i = end;
    }
    return QString();
}

// -----------------------------------------------------------------------------
//  排版
// -----------------------------------------------------------------------------
MTextEntity::Layout MTextEntity::layout() const
{
    Layout out;
    out.lineHeight = lineHeight();

    const QString plain = plainText();
    const std::vector<int> paragraphAlign = paragraphJustifications(m_content);
    const double anchor = attachmentColumn(m_attachment) * 0.5;   // 附着点的列锚：0 / 0.5 / 1
    const double box = m_referenceWidth;

    // 1) 分行：**只按内容里的段落分隔**（`\P`，stripInlineCodes 已归一为换行），
    //    **不依据参考栏宽(41)重新折行**。
    //
    //    这是 AutoCAD / 浩辰的实际显示行为：MTEXT 的换行位置在**编辑时**就已确定并写进
    //    内容（以 `\P` 记录），打开文件时按内容原样分段；41 只是"编辑时的折行宽度"，
    //    不参与显示。早期实现按 41 自动折行，结果把本来单行的文字硬折成多行——实测
    //    一张 47 条 MTEXT 的图纸有 26 条被误折（"USB"、"±12V"、"GND"、"WZJ-Z1 信号
    //    采集模块"等原本都是一行）。根因是 41 由生成器按「字高即 em」的口径估宽
    //    （统计 47 条：41/行宽 的中位数 1.34，最小值 1.05，**全部 ≥ 1**，即条条都
    //    "装得下"），而显示字体按「字高 = 0.72 em」解释，同样的字符串要宽出约 1.39 倍
    //    ——用显示的度量去比对一个按另一套口径写出的宽度，就必然误折。
    //
    //    每行的起点由「附着点定矩形、段落码定行位」共同决定（矩形宽 W、行宽 w、
    //    附着列锚 a、行锚 t）：x = -a·W + (W - w)·t，t 缺省取 a 时退化为 -w·t；
    //    W = 0（不折行）时 x = -w·t。
    const std::vector<QString> paragraphs = splitParagraphs(plain);
    for (std::size_t index = 0; index < paragraphs.size(); ++index) {
        const int align = (index < paragraphAlign.size()) ? paragraphAlign[index] : -1;
        const double lineAnchor = align >= 0 ? align * 0.5 : anchor;
        const QString& text = paragraphs[index];
        const double lineWidth = TextEntity::estimateLineWidth(text, m_height, 1.0);
        const double x = (box > kGeomTol)
                             ? (-anchor * box + (box - lineWidth) * lineAnchor)
                             : (-lineWidth * lineAnchor);
        out.lines.push_back(LayoutLine { text, x, lineWidth });
    }
    if (out.lines.empty()) out.lines.push_back(LayoutLine { QString(), 0.0, 0.0 });

    double minX = out.lines.front().x;
    double maxX = minX + out.lines.front().width;
    for (const LayoutLine& line : out.lines) {
        minX = std::min(minX, line.x);
        maxX = std::max(maxX, line.x + line.width);
    }

    // 2) 垂直：附着点的行决定首行基线的位置。三档的垂直基准（与真实 CAD 一致）：
    //      上 = 首行**大写高度**顶贴插入点
    //      中 = 首行**小写高度**顶与末行基线的中点落在插入点
    //            （注意不是大写高度——TEXT 的 72=1/73=2 用的才是大写高度，两者差
    //             (capHeight − xHeight)/2 ≈ 0.13 倍字高，正是降级会错位的原因）
    //      下 = 末行下伸部底贴插入点
    const double span = out.lineHeight * static_cast<double>(out.lines.size() - 1);
    switch (attachmentRow(m_attachment)) {
    case 0:  out.firstBaseline = -m_height; break;
    case 1:  out.firstBaseline = (span - kXHeightRatio * m_height) * 0.5; break;
    default: out.firstBaseline = span + kDescentRatio * m_height; break;
    }

    out.minY = out.firstBaseline - span - kDescentRatio * m_height;
    out.maxY = out.firstBaseline + kAscentRatio * m_height;
    out.minX = minX;
    out.maxX = maxX;
    if (maxX - minX <= kGeomTol) {
        out.maxX = out.minX + m_height * 0.5;    // 空文字仍留一个可见的框
    }
    out.valid = true;
    return out;
}

std::vector<Point2D> MTextEntity::outlineCorners() const
{
    const Layout laid = layout();
    const std::vector<Point2D> local = {
        { laid.minX, laid.minY },
        { laid.maxX, laid.minY },
        { laid.maxX, laid.maxY },
        { laid.minX, laid.maxY },
    };

    // 局部（X 沿文字方向、Y 为文字上方）-> 世界
    const double cosA = m_direction.x;
    const double sinA = m_direction.y;
    std::vector<Point2D> corners;
    corners.reserve(local.size());
    for (const Point2D& p : local) {
        corners.push_back(Point2D(m_insertion.x + p.x * cosA - p.y * sinA,
                                  m_insertion.y + p.x * sinA + p.y * cosA));
    }
    return corners;
}

// -----------------------------------------------------------------------------
//  Entity 接口
// -----------------------------------------------------------------------------
std::unique_ptr<Entity> MTextEntity::clone() const
{
    auto copy = std::make_unique<MTextEntity>();
    copy->m_content           = m_content;
    copy->m_insertion         = m_insertion;
    copy->m_direction         = m_direction;
    copy->m_normal            = m_normal;
    copy->m_height            = m_height;
    copy->m_referenceWidth    = m_referenceWidth;
    copy->m_attachment        = m_attachment;
    copy->m_drawingDirection  = m_drawingDirection;
    copy->m_lineSpacingStyle  = m_lineSpacingStyle;
    copy->m_lineSpacingFactor = m_lineSpacingFactor;
    copy->m_extentsWidth      = m_extentsWidth;
    copy->m_extentsHeight     = m_extentsHeight;
    copy->m_fontStyle         = m_fontStyle;
    copyCommonPropertiesTo(*copy);
    return copy;
}

Box2D MTextEntity::boundingBox() const
{
    Box2D box;
    for (const Point2D& p : outlineCorners()) box.expand(p);
    return box;
}

double MTextEntity::distanceTo(const Point2D& p) const
{
    const std::vector<Point2D> corners = outlineCorners();

    // 先在文字局部坐标系里判「是否落在块内」，块内距离为 0
    const double dx = p.x - m_insertion.x;
    const double dy = p.y - m_insertion.y;
    const double lx = dx * m_direction.x + dy * m_direction.y;
    const double ly = -dx * m_direction.y + dy * m_direction.x;

    const Layout laid = layout();
    if (lx >= laid.minX && lx <= laid.maxX && ly >= laid.minY && ly <= laid.maxY) return 0.0;

    double best = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < corners.size(); ++i) {
        const Point2D& a = corners[i];
        const Point2D& b = corners[(i + 1) % corners.size()];
        const double vx = b.x - a.x;
        const double vy = b.y - a.y;
        const double len2 = vx * vx + vy * vy;
        double t = 0.0;
        if (len2 > kGeomTol) {
            t = std::clamp(((p.x - a.x) * vx + (p.y - a.y) * vy) / len2, 0.0, 1.0);
        }
        best = std::min(best, p.distanceTo(Point2D(a.x + vx * t, a.y + vy * t)));
    }
    return best;
}

void MTextEntity::transform(const Transform2D& t)
{
    m_insertion = t.apply(m_insertion);

    // 方向向量按变换的线性部分走：旋转正确反映、镜像体现为方向翻转
    const Vector2D moved = t.applyVector(m_direction);
    if (moved.length() > kGeomTol) setDirection(moved);

    if (t.isSimilarity()) {
        const double scale = t.uniformScale();
        m_height *= scale;
        if (m_referenceWidth > kGeomTol) m_referenceWidth *= scale;
        m_extentsWidth *= scale;
        m_extentsHeight *= scale;
    } else {
        // 非等比变换无法保持「字高」语义，按文字方向的长度缩放估算（与 TEXT 同口径）
        m_height *= t.lengthScaleAlong(Vector2D(1.0, 0.0));
    }
}

double MTextEntity::length() const
{
    return 0.0;
}

std::vector<Point2D> MTextEntity::snapPoints() const
{
    return { m_insertion };
}

std::vector<Point2D> MTextEntity::gripPoints() const
{
    // 0 = 插入点；1 = 参考宽度夹点（拖动改栏宽，与 AutoCAD 的 MTEXT 一致）
    const double x = m_referenceWidth > kGeomTol ? m_referenceWidth : layout().maxX;
    return { m_insertion,
             Point2D(m_insertion.x + x * m_direction.x, m_insertion.y + x * m_direction.y) };
}

bool MTextEntity::moveGrip(std::size_t index, const Point2D& newPos)
{
    if (index == 0) {
        m_insertion = newPos;
        return true;
    }
    if (index == 1) {
        // 沿文字方向量出的距离即新的参考宽度
        const double dx = newPos.x - m_insertion.x;
        const double dy = newPos.y - m_insertion.y;
        setReferenceWidth(dx * m_direction.x + dy * m_direction.y);
        return true;
    }
    return false;
}

std::vector<Point2D> MTextEntity::flatten(double sagitta) const
{
    Q_UNUSED(sagitta);
    return outlineCorners();
}

QString MTextEntity::describe() const
{
    QString summary = plainText();
    const int newline = summary.indexOf(kParagraph);
    if (newline >= 0) summary = summary.left(newline) + QStringLiteral("…");
    if (summary.size() > 24) summary = summary.left(24) + QStringLiteral("…");
    return QStringLiteral("多行文字  \"%1\"  字高 %2").arg(summary, formatLength(m_height));
}

QString MTextEntity::geometrySummary() const
{
    const Layout laid = layout();
    const QString font = m_fontStyle.isEmpty() ? QStringLiteral("（默认）") : m_fontStyle;
    const QString inlineName = inlineFontName();
    const bool wrapped = m_referenceWidth > kGeomTol;
    const QString direction = m_drawingDirection == DrawingDirection::TopToBottom
                                  ? QStringLiteral("从上到下")
                                  : (m_drawingDirection == DrawingDirection::ByStyle
                                         ? QStringLiteral("随样式")
                                         : QStringLiteral("从左到右"));

    return QStringLiteral("内容: %1\n字体: %2\n插入点: %3\n字高: %4\n旋转: %5\n"
                          "附着点: %6\n参考栏宽: %7\n行距: %8 / %9\n行数: %10\n书写方向: %11")
        .arg(plainText().replace(kParagraph, QStringLiteral(" / ")),
             inlineName.isEmpty() ? font
                                  : QStringLiteral("%1（内联 %2）").arg(font, inlineName),
             formatPoint(m_insertion),
             formatLength(m_height),
             formatAngle(rotation()))
        .arg(attachmentDisplayName(m_attachment),
             wrapped ? formatLength(m_referenceWidth) : QStringLiteral("不折行"))
        .arg(m_lineSpacingStyle == LineSpacingStyle::AtLeast ? QStringLiteral("至少")
                                                            : QStringLiteral("固定"))
        .arg(m_lineSpacingFactor)
        .arg(laid.lines.size())
        .arg(direction);
}

} // namespace lycad
