// =============================================================================
//  LyCAD - core/TextEntity.cpp
// =============================================================================
#include "core/TextEntity.h"

#include "core/Format.h"

#include <QFontMetricsF>

#include <cmath>

namespace lycad {

namespace {

/// 文字宽度估算系数：单位字高下，ASCII 平均可视宽度
constexpr double kAsciiWidthFactor = 0.55;
/// CJK 全角字符宽度系数（一个汉字约为一个字高宽）
constexpr double kCjkWidthFactor = 1.0;

bool isWideChar(char32_t code)
{
    // CJK 统一表意文字、全角标点、假名等近似判定
    return (code >= 0x1100 && code <= 0x115F)       // 谚文兼容字母
        || (code >= 0x2E80 && code <= 0xA4CF)       // CJK 部首 ~ 彝文
        || (code >= 0xAC00 && code <= 0xD7A3)       // 谚文音节
        || (code >= 0xF900 && code <= 0xFAFF)       // CJK 兼容表意
        || (code >= 0xFE30 && code <= 0xFE4F)       // CJK 兼容形式
        || (code >= 0xFF00 && code <= 0xFF60)       // 全角形式
        || (code >= 0xFFE0 && code <= 0xFFE6)
        || (code >= 0x20000 && code <= 0x3FFFD);    // 扩展表意区
}

/// 单个字符的估算宽度（单位：字高）
double charWidthFactor(char32_t code)
{
    if (code == U' ') return 0.35;
    if (code < 0x20)  return 0.0;
    return isWideChar(code) ? kCjkWidthFactor : kAsciiWidthFactor;
}

} // namespace

TextEntity::TextEntity()
    : Entity(EntityType::Text)
{
}

TextEntity::TextEntity(const QString& content, const Point2D& position,
                       double height, double rotationRad)
    : Entity(EntityType::Text)
    , m_content(content)
    , m_position(position)
    , m_alignmentPoint(position)
    , m_height(height > 0.0 ? height : 3.5)
    , m_rotation(normalizeAngle(rotationRad))
{
}

QString TextEntity::displayString() const
{
    QString out = m_content;
    out.remove(QLatin1Char('\n'));
    out.remove(QLatin1Char('\r'));
    return out;
}

void TextEntity::setHeight(double h)
{
    if (h > kGeomTol) m_height = h;
}

double TextEntity::estimatedWidth() const noexcept
{
    double width = 0.0;
    for (const QChar ch : m_content) {
        // QString 是 UTF-16：增补平面字符按代理对出现，此处跳过低代理项
        const ushort code = ch.unicode();
        if (code >= 0xDC00 && code <= 0xDFFF) continue;
        char32_t cp = code;
        if (code >= 0xD800 && code <= 0xDBFF) {
            // 高代理项：与后续低代理项合成码点（估算宽度按 CJK 处理即可）
            cp = isWideChar(code) ? 0x4E00 : code;
        }
        width += charWidthFactor(cp);
    }
    return width * m_height;
}

std::vector<Point2D> TextEntity::outlineCorners() const
{
    // 以「插入点 = 左下角」的局部矩形，再旋转到世界方向
    const double w = std::max(estimatedWidth(), m_height * 0.5);
    const double h = m_height;

    // 局部坐标：X 沿文字方向，Y 向上；基线位于局部 y = 0
    const std::vector<Point2D> local = {
        { 0.0, 0.0 },
        { w,   0.0 },
        { w,   h * 0.75 },
        { 0.0, h * 0.75 },
    };

    const double cosA = std::cos(m_rotation);
    const double sinA = std::sin(m_rotation);
    const Point2D origin = effectivePosition();

    std::vector<Point2D> corners;
    corners.reserve(local.size());
    for (const Point2D& p : local) {
        corners.push_back(Point2D(origin.x + p.x * cosA - p.y * sinA,
                                  origin.y + p.x * sinA + p.y * cosA));
    }
    return corners;
}

std::unique_ptr<Entity> TextEntity::clone() const
{
    // Entity 删除了拷贝构造，clone 需逐字段复制
    auto copy = std::make_unique<TextEntity>();
    copy->m_content        = m_content;
    copy->m_position       = m_position;
    copy->m_alignmentPoint = m_alignmentPoint;
    copy->m_height         = m_height;
    copy->m_rotation       = m_rotation;
    copy->m_hAlign         = m_hAlign;
    copy->m_vAlign         = m_vAlign;
    copyCommonPropertiesTo(*copy);
    return copy;
}

Box2D TextEntity::boundingBox() const
{
    Box2D box;
    for (const Point2D& p : outlineCorners()) box.expand(p);
    return box;
}

double TextEntity::distanceTo(const Point2D& p) const
{
    const Box2D box = boundingBox();
    if (!box.isValid()) return p.distanceTo(effectivePosition());
    if (box.contains(p)) return 0.0;

    // 到包围盒边缘的最短距离
    const double dx = std::max(std::max(box.minX - p.x, p.x - box.maxX), 0.0);
    const double dy = std::max(std::max(box.minY - p.y, p.y - box.maxY), 0.0);
    return std::hypot(dx, dy);
}

void TextEntity::transform(const Transform2D& t)
{
    m_position = t.apply(m_position);
    m_alignmentPoint = t.apply(m_alignmentPoint);

    if (t.isSimilarity()) {
        m_height *= t.uniformScale();
        m_rotation = normalizeAngle(m_rotation + t.rotationAngle());
    } else {
        // 非等比变换无法保持文字形状，退化为：按 x 方向长度缩放字高，丢弃旋转
        m_height *= t.lengthScaleAlong(Vector2D(1.0, 0.0));
    }
}

double TextEntity::length() const
{
    return 0.0;
}

std::vector<Point2D> TextEntity::snapPoints() const
{
    return { effectivePosition() };
}

std::vector<Point2D> TextEntity::gripPoints() const
{
    return { effectivePosition() };
}

bool TextEntity::moveGrip(std::size_t index, const Point2D& newPos)
{
    if (index != 0) return false;
    if (usesAlignmentPoint()) {
        m_alignmentPoint = newPos;
    } else {
        m_position = newPos;
    }
    return true;
}

std::vector<Point2D> TextEntity::flatten(double sagitta) const
{
    Q_UNUSED(sagitta);
    return outlineCorners();
}

QString TextEntity::describe() const
{
    return QStringLiteral("文字  \"%1\"  字高 %2")
        .arg(displayString(), formatLength(m_height));
}

QString TextEntity::geometrySummary() const
{
    static const char* kH[] = { "左", "居中", "右" };
    static const char* kV[] = { "基线", "底部", "中部", "顶部" };

    return QStringLiteral("内容: %1\n插入点: %2\n字高: %3\n旋转: %4\n对齐: %5/%6")
        .arg(displayString(),
             formatPoint(effectivePosition()),
             formatLength(m_height),
             formatAngle(m_rotation))
        .arg(kH[static_cast<int>(m_hAlign)], kV[static_cast<int>(m_vAlign)]);
}

} // namespace lycad
