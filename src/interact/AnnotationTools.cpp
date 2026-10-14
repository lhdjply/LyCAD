// =============================================================================
//  LyCAD - interact/AnnotationTools.cpp
// =============================================================================
#include "interact/AnnotationTools.h"

#include "core/ArcEntity.h"
#include "core/CircleEntity.h"
#include "core/Document.h"
#include "core/Format.h"
#include "core/HatchEntity.h"
#include "core/MTextEntity.h"
#include "core/TextEntity.h"
#include "render/CadCanvas.h"

#include <QKeyEvent>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <limits>

namespace lycad {

namespace {

/// 预览线条的颜色与样式，与最终实体区分开
const QColor kPreviewColor(0x1E, 0x88, 0xE5);

/// 新建文字的默认字高（世界单位）。与绘图单位无关，是 CAD 里最常用的 3.5。
constexpr double kDefaultTextHeight = 3.5;

/// 新图元归入当前图层，颜色随层
template <typename T>
void applyDefaults(T& entity, const ToolContext& context)
{
    if (!context.document) return;
    entity.setLayerName(context.document->layers().currentLayerName());
    entity.setColorByLayer();
}

/// 用虚线绘制一个临时图元的预览
void drawPreviewEntity(QPainter& painter, const Entity& entity,
                       const EntityRenderer::DrawContext& context)
{
    QPen pen(kPreviewColor);
    pen.setWidthF(1.2);
    pen.setStyle(Qt::DashLine);
    pen.setCosmetic(true);

    painter.save();
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(EntityRenderer::toDevicePath(entity, *context.view, context.viewport));
    painter.restore();
}

/// 在两点之间画橡皮筋
void drawRubberBand(QPainter& painter, const EntityRenderer::DrawContext& context,
                    const Point2D& from, const Point2D& to)
{
    const Point2D a = context.view->toDevice(from);
    const Point2D b = context.view->toDevice(to);

    QPen pen(kPreviewColor);
    pen.setWidthF(1.0);
    pen.setStyle(Qt::DashLine);
    pen.setCosmetic(true);

    painter.save();
    painter.setPen(pen);
    painter.drawLine(QPointF(a.x, a.y), QPointF(b.x, b.y));
    painter.restore();
}

/// 坐标标注量 X 还是量 Y：AutoCAD 按**引线方向**定——引线偏竖直时量 X 坐标，
/// 偏水平时量 Y 坐标。这与显示几何同源（引线是沿测量轴的垂直方向拉开的）。
bool ordinateMeasuresX(const Point2D& feature, const Point2D& leader)
{
    const Vector2D d = leader - feature;
    return std::abs(d.y) >= std::abs(d.x);
}

/// 在文档中拾取距离光标最近的**圆弧**（弧长标注的目标）。
/// 只认 ARC：圆没有起止参数，也就没有弧长可量。
bool pickArcGeometry(const ToolContext& context, const Point2D& worldPos,
                     Point2D* center, double* radius, double* startAngle, double* endAngle)
{
    if (!context.document) return false;

    const double tolerance = context.pickToleranceWorld() * 2.0;
    double best = tolerance;
    bool found = false;

    for (EntityId id : context.document->drawOrder()) {
        const Entity* entity = context.document->entity(id);
        if (!entity || entity->type() != EntityType::Arc) continue;

        const double d = entity->distanceTo(worldPos);
        if (d > best) continue;

        const auto* arc = static_cast<const ArcEntity*>(entity);
        *center = arc->center();
        *radius = arc->radius();
        *startAngle = arc->startAngle();
        *endAngle = arc->endAngle();
        best = d;
        found = true;
    }
    return found;
}

/// 在文档中拾取距离光标最近的圆或圆弧（半径/直径标注的目标）
bool pickCircleGeometry(const ToolContext& context, const Point2D& worldPos,
                        Point2D* center, Point2D* onCircle, double* radius)
{
    if (!context.document) return false;

    const double tolerance = context.pickToleranceWorld() * 2.0;
    double best = tolerance;
    bool found = false;

    for (EntityId id : context.document->drawOrder()) {
        const Entity* entity = context.document->entity(id);
        if (!entity) continue;
        if (entity->type() != EntityType::Circle && entity->type() != EntityType::Arc) continue;

        const double d = entity->distanceTo(worldPos);
        if (d > best) continue;

        const Point2D closest = entity->closestPointTo(worldPos);
        double r = 0.0;
        Point2D c;
        if (entity->type() == EntityType::Circle) {
            const auto* circle = static_cast<const CircleEntity*>(entity);
            c = circle->center();
            r = circle->radius();
        } else {
            const auto* arc = static_cast<const ArcEntity*>(entity);
            c = arc->center();
            r = arc->radius();
        }

        *center = c;
        *onCircle = closest;
        *radius = r;
        best = d;
        found = true;
    }
    return found;
}

} // namespace

// =============================================================================
//  文字（单行 / 多行）
//
//  两种文字的差别只在两处，因此共用一份交互：
//    · 内容输入走哪种编辑框（单行输入框 / 可换行的多行编辑框）——multiLine()；
//    · 落成哪种图元（TextEntity / MTextEntity）——commit() 里按 multiLine() 分支。
// =============================================================================
QString DrawTextToolBase::prompt() const
{
    // 内容还没预输入时，提示里点明"确认后会弹编辑框"，用户不会以为必须先在
    // 命令行打字；已经预输入过内容时，提示直接指向插入点。
    if (m_text.isEmpty()) {
        return multiLine() ? QStringLiteral("指定多行文字插入点（确认后弹出编辑框）:")
                           : QStringLiteral("指定文字插入点（确认后弹出编辑框）:");
    }
    return QStringLiteral("指定文字插入点:");
}

bool DrawTextToolBase::textInput(const QString& text)
{
    // 命令行预输入：仅作为编辑框的初值。已经预输入过之后不再吃文本，
    // 好让插入点仍可用键盘坐标输入（"100,50"）。
    if (!m_text.isEmpty()) return false;

    if (text.trimmed().isEmpty()) {
        message(QStringLiteral("文字内容不能为空。"));
        return true;
    }
    m_text = text;
    notifyPromptChanged();
    return true;
}

bool DrawTextToolBase::mousePress(const CanvasEvent& event)
{
    if (event.button != Qt::LeftButton || !ctx().document) return false;

    const ResolvedPoint resolved = resolve(event.worldPos);

    // 内容、字体与字高都从编辑框取。未装配回调（纯交互环境 / 单元测试）时按"取消"
    // 处理——不能静默落一条空文字下去，那会让用户以为点击没反应。
    QString content = m_text;
    QString fontFamily;
    double textHeight = kDefaultTextHeight;
    if (!ctx().editText
        || !ctx().editText(multiLine(), m_text, QString(), textHeight,
                           content, fontFamily)) {
        message(QStringLiteral("已取消文字输入。"));
        return true;
    }

    // 首尾空白一律去掉（避免误留空行改变字高与包围盒），行内的换行原样保留
    content = content.trimmed();
    if (content.isEmpty()) {
        message(QStringLiteral("文字内容不能为空。"));
        return true;
    }

    commit(resolved.position, content, fontFamily, textHeight);
    finish();
    return true;
}

bool DrawTextToolBase::mouseMove(const CanvasEvent& event)
{
    m_cursor = resolve(event.worldPos);
    m_hasCursor = true;
    ctx().setSnapMarker(m_cursor.kind != SnapKind::None
                            ? std::optional<SnapResult>(m_cursor.toSnapResult())
                            : std::nullopt);
    repaint();
    return true;
}

void DrawTextToolBase::paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context)
{
    if (m_text.isEmpty() || !m_hasCursor) return;

    // 预览：在光标处显示即将插入的文字（预输入的内容，换行按空格排成一行）
    const Point2D d = context.view->toDevice(m_cursor.position);
    const double hPx = kDefaultTextHeight * context.view->scale();

    QFont font = painter.font();
    font.setPixelSize(std::max(2, static_cast<int>(std::lround(hPx))));

    painter.save();
    painter.setFont(font);
    painter.setPen(kPreviewColor);
    painter.drawText(QPointF(d.x, d.y),
                     QString(m_text).replace(QLatin1Char('\n'), QLatin1Char(' ')));
    painter.restore();
}

void DrawTextToolBase::commit(const Point2D& position, const QString& content,
                              const QString& fontFamily, double textHeight)
{
    Document* document = ctx().document;
    if (!document) return;

    // 字体：与双击编辑、特性面板同一口径——先登记进文字样式表（否则导出时会
    // 退化成 txt.shx），再把样式名写到图元上。
    const QString style = fontFamily.trimmed();
    if (!style.isEmpty()) document->ensureTextFontStyle(style);

    // 字高来自编辑框（用户可以在那里改）；异常值退回默认，绝不落一条画不出来的文字
    const double height = (textHeight > 0.0) ? textHeight : kDefaultTextHeight;

    if (multiLine()) {
        // 多行文字：编辑框里的换行是段落分隔，模型内部一律存 \P
        // （与文件里的写法一致，双击编辑时再转回换行显示）。
        QString raw = content;
        raw.replace(QStringLiteral("\n"), QStringLiteral("\\P"));
        auto entity = std::make_unique<MTextEntity>(raw, position, height);
        if (!style.isEmpty()) entity->setFontStyle(style);
        applyDefaults(*entity, ctx());
        const int paragraphs = entity->paragraphCount();
        document->addEntity(std::move(entity));
        message(paragraphs > 1
                    ? QStringLiteral("已创建多行文字（%1 段）").arg(paragraphs)
                    : QStringLiteral("已创建多行文字"));
    } else {
        // 单行文字：编辑框就是单行输入框，内容里本不该有换行；仍做一次防御性
        // 归并，避免换行混进单行对象把版面语义（以及导出时的对象类型）改掉。
        QString plain = content;
        plain.replace(QLatin1Char('\n'), QLatin1Char(' '));
        auto entity = std::make_unique<TextEntity>(plain, position, height);
        if (!style.isEmpty()) entity->setFontStyle(style);
        applyDefaults(*entity, ctx());
        document->addEntity(std::move(entity));
        message(QStringLiteral("已创建文字 \"%1\"").arg(plain));
    }
}

void DrawTextToolBase::deactivate()
{
    m_text.clear();
}

QString DrawTextTool::name() const { return QStringLiteral("TEXT"); }
QString DrawTextTool::displayName() const { return QStringLiteral("单行文字"); }

QString DrawMTextTool::name() const { return QStringLiteral("MTEXT"); }
QString DrawMTextTool::displayName() const { return QStringLiteral("多行文字"); }

// =============================================================================
//  填充
// =============================================================================
DrawHatchTool::DrawHatchTool(ToolContext& context)
    : Tool(context)
{
}

QString DrawHatchTool::name() const { return QStringLiteral("HATCH"); }
QString DrawHatchTool::displayName() const { return QStringLiteral("填充"); }

QString DrawHatchTool::prompt() const
{
    if (m_vertices.empty()) return QStringLiteral("指定边界第一点:");
    return QStringLiteral("指定下一点 [放弃(U)] <回车闭合并填充>:");
}

bool DrawHatchTool::mousePress(const CanvasEvent& event)
{
    if (!ctx().document) return false;

    if (event.button == Qt::RightButton) {
        if (ready()) {
            commit();
            finish();
        }
        return true;
    }
    if (event.button != Qt::LeftButton) return false;

    const std::optional<Point2D> base = m_vertices.empty()
        ? std::nullopt
        : std::optional<Point2D>(m_vertices.back());
    const ResolvedPoint resolved = resolve(event.worldPos, base);

    m_vertices.push_back(resolved.position);
    notifyPromptChanged();
    repaint();
    return true;
}

bool DrawHatchTool::mouseMove(const CanvasEvent& event)
{
    const std::optional<Point2D> base = m_vertices.empty()
        ? std::nullopt
        : std::optional<Point2D>(m_vertices.back());
    m_cursor = resolve(event.worldPos, base);
    m_hasCursor = true;
    ctx().setSnapMarker(m_cursor.kind != SnapKind::None
                            ? std::optional<SnapResult>(m_cursor.toSnapResult())
                            : std::nullopt);
    repaint();
    return true;
}

bool DrawHatchTool::keyPress(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (ready()) {
            commit();
            finish();
        } else {
            message(QStringLiteral("边界至少需要 3 个顶点。"));
        }
        return true;
    case Qt::Key_U:
        if (!m_vertices.empty()) m_vertices.pop_back();
        notifyPromptChanged();
        repaint();
        return true;
    default:
        return false;
    }
}

bool DrawHatchTool::textInput(const QString& text)
{
    if (matchesOption(text, QStringLiteral("U"))) {
        if (!m_vertices.empty()) m_vertices.pop_back();
        notifyPromptChanged();
        repaint();
        return true;
    }

    const std::optional<Point2D> base = m_vertices.empty() ? std::nullopt
                                                           : std::optional<Point2D>(m_vertices.back());
    const std::optional<Point2D> point = parsePointInput(text, base);
    if (!point) return true;

    CanvasEvent synthetic;
    synthetic.worldPos = *point;
    synthetic.button = Qt::LeftButton;
    return mousePress(synthetic);
}

void DrawHatchTool::paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context)
{
    if (m_vertices.empty()) return;

    std::vector<Point2D> preview = m_vertices;
    if (m_hasCursor) preview.push_back(m_cursor.position);

    HatchEntity temp(preview, QStringLiteral("ANSI31"), 1.0, 0.0);
    drawPreviewEntity(painter, temp, context);
}

void DrawHatchTool::commit()
{
    if (!ctx().document || !ready()) return;

    auto hatch = std::make_unique<HatchEntity>(m_vertices,
                                               QStringLiteral("ANSI31"), 1.0, 0.0);
    applyDefaults(*hatch, ctx());
    ctx().document->addEntity(std::move(hatch));

    message(QStringLiteral("已创建填充（%1 个边界顶点）").arg(m_vertices.size()));
    m_vertices.clear();
}

void DrawHatchTool::deactivate()
{
    m_vertices.clear();
}

// =============================================================================
//  线性 / 对齐标注公共流程
// =============================================================================
bool LinearDimensionToolBase::mousePress(const CanvasEvent& event)
{
    if (event.button != Qt::LeftButton || !ctx().document) return false;

    const std::optional<Point2D> base = m_points.empty()
        ? std::nullopt
        : std::optional<Point2D>(m_points.back());
    const ResolvedPoint resolved = resolve(event.worldPos, base);

    m_points.push_back(resolved.position);
    if (m_points.size() == 3) {
        commit();
    }

    notifyPromptChanged();
    repaint();
    return true;
}

bool LinearDimensionToolBase::mouseMove(const CanvasEvent& event)
{
    const std::optional<Point2D> base = m_points.empty()
        ? std::nullopt
        : std::optional<Point2D>(m_points.back());
    m_cursor = resolve(event.worldPos, base);
    m_hasCursor = true;
    ctx().setSnapMarker(m_cursor.kind != SnapKind::None
                            ? std::optional<SnapResult>(m_cursor.toSnapResult())
                            : std::nullopt);
    repaint();
    return true;
}

bool LinearDimensionToolBase::keyPress(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_U:
        if (!m_points.empty()) m_points.pop_back();
        notifyPromptChanged();
        repaint();
        return true;
    default:
        return false;
    }
}

bool LinearDimensionToolBase::textInput(const QString& text)
{
    if (matchesOption(text, QStringLiteral("U"))) {
        if (!m_points.empty()) m_points.pop_back();
        notifyPromptChanged();
        repaint();
        return true;
    }

    const std::optional<Point2D> base = m_points.empty() ? std::nullopt
                                                         : std::optional<Point2D>(m_points.back());
    const std::optional<Point2D> point = parsePointInput(text, base);
    if (!point) return true;

    CanvasEvent synthetic;
    synthetic.worldPos = *point;
    synthetic.button = Qt::LeftButton;
    return mousePress(synthetic);
}

void LinearDimensionToolBase::paintOverlay(QPainter& painter,
                                           const EntityRenderer::DrawContext& context)
{
    if (m_points.empty()) return;

    // 构造临时标注做实时预览
    DimensionEntity temp(dimType());
    temp.setFirstPoint(m_points[0]);
    temp.setSecondPoint(m_points.size() >= 2 ? m_points[1] : m_points[0]);
    temp.setDimLinePoint(m_points.size() >= 3 ? m_points[2]
                                              : (m_hasCursor ? m_cursor.position : m_points.back()));
    if (dimType() == DimensionEntity::DimType::Linear) {
        temp.setRotation(0.0);      // 线性标注默认水平测量
    }
    drawPreviewEntity(painter, temp, context);

    // 第二步之间画橡皮筋
    if (m_points.size() == 1 && m_hasCursor) {
        drawRubberBand(painter, context, m_points[0], m_cursor.position);
    }
}

void LinearDimensionToolBase::commit()
{
    if (!ctx().document || m_points.size() < 3) return;

    auto dim = std::make_unique<DimensionEntity>(dimType());
    dim->setFirstPoint(m_points[0]);
    dim->setSecondPoint(m_points[1]);
    dim->setDimLinePoint(m_points[2]);
    if (dimType() == DimensionEntity::DimType::Linear) {
        dim->setRotation(0.0);
    }
    applyDefaults(*dim, ctx());

    const QString text = dim->displayText();
    ctx().document->addEntity(std::move(dim));

    message(QStringLiteral("已创建标注：%1").arg(text));
    m_points.clear();
}

void LinearDimensionToolBase::deactivate()
{
    m_points.clear();
}

QString DrawDimAlignedTool::name() const { return QStringLiteral("DIMALIGNED"); }
QString DrawDimAlignedTool::displayName() const { return QStringLiteral("对齐标注"); }

QString DrawDimAlignedTool::prompt() const
{
    switch (pointCount()) {
    case 0: return QStringLiteral("指定第一条界线原点:");
    case 1: return originPrompt();
    default: return QStringLiteral("指定尺寸线位置:");
    }
}

QString DrawDimAlignedTool::originPrompt() const
{ return QStringLiteral("指定第二条界线原点:"); }

QString DrawDimLinearTool::name() const { return QStringLiteral("DIMLINEAR"); }
QString DrawDimLinearTool::displayName() const { return QStringLiteral("线性标注"); }

QString DrawDimLinearTool::prompt() const
{
    switch (pointCount()) {
    case 0: return QStringLiteral("指定第一条界线原点:");
    case 1: return originPrompt();
    default: return QStringLiteral("指定尺寸线位置:");
    }
}

QString DrawDimLinearTool::originPrompt() const
{ return QStringLiteral("指定第二条界线原点:"); }

// =============================================================================
//  半径 / 直径标注公共流程
// =============================================================================
bool RadialDimensionToolBase::mousePress(const CanvasEvent& event)
{
    if (!ctx().document) return false;
    if (event.button != Qt::LeftButton) return false;

    if (!m_hasCircle) {
        Point2D center, onCircle;
        double radius = 0.0;
        if (!pickCircleGeometry(ctx(), event.worldPos, &center, &onCircle, &radius)) {
            message(QStringLiteral("未拾取到圆或圆弧，请点击圆的边线附近。"));
            return true;
        }
        m_center = center;
        m_pickPoint = onCircle;
        m_radius = radius;
        m_hasCircle = true;
        notifyPromptChanged();
        repaint();
        return true;
    }

    const ResolvedPoint resolved = resolve(event.worldPos, m_pickPoint);
    m_cursor = resolved;
    commit();
    return true;
}

bool RadialDimensionToolBase::mouseMove(const CanvasEvent& event)
{
    const std::optional<Point2D> base = m_hasCircle
        ? std::optional<Point2D>(m_pickPoint)
        : std::nullopt;
    m_cursor = resolve(event.worldPos, base);
    m_hasCursor = true;
    ctx().setSnapMarker(m_cursor.kind != SnapKind::None
                            ? std::optional<SnapResult>(m_cursor.toSnapResult())
                            : std::nullopt);
    repaint();
    return true;
}

bool RadialDimensionToolBase::keyPress(QKeyEvent* event)
{
    if (event->key() == Qt::Key_U && m_hasCircle) {
        m_hasCircle = false;
        notifyPromptChanged();
        repaint();
        return true;
    }
    return false;
}

bool RadialDimensionToolBase::textInput(const QString& text)
{
    if (!m_hasCircle) return false;     // 第一步必须点选圆

    const std::optional<Point2D> point = parsePointInput(text, m_pickPoint);
    if (!point) return true;

    CanvasEvent synthetic;
    synthetic.worldPos = *point;
    synthetic.button = Qt::LeftButton;
    return mousePress(synthetic);
}

void RadialDimensionToolBase::paintOverlay(QPainter& painter,
                                           const EntityRenderer::DrawContext& context)
{
    if (!m_hasCircle) return;
    if (!m_hasCursor) return;

    DimensionEntity temp(diametric() ? DimensionEntity::DimType::Diametric
                                     : DimensionEntity::DimType::Radial);
    if (diametric()) {
        temp.setFirstPoint(m_pickPoint);
        temp.setSecondPoint(m_center + (m_center - m_pickPoint));
    } else {
        temp.setFirstPoint(m_center);
        temp.setSecondPoint(m_pickPoint);
    }
    temp.setDimLinePoint(m_cursor.position);
    drawPreviewEntity(painter, temp, context);
}

void RadialDimensionToolBase::commit()
{
    if (!ctx().document || !m_hasCircle) return;

    auto dim = std::make_unique<DimensionEntity>(
        diametric() ? DimensionEntity::DimType::Diametric : DimensionEntity::DimType::Radial);
    if (diametric()) {
        dim->setFirstPoint(m_pickPoint);
        dim->setSecondPoint(m_center + (m_center - m_pickPoint));
    } else {
        dim->setFirstPoint(m_center);
        dim->setSecondPoint(m_pickPoint);
    }
    dim->setDimLinePoint(m_cursor.position);
    applyDefaults(*dim, ctx());

    const double value = dim->measurement();
    ctx().document->addEntity(std::move(dim));

    message(diametric() ? QStringLiteral("已创建直径标注：⌀%1").arg(formatLength(value))
                        : QStringLiteral("已创建半径标注：R%1").arg(formatLength(value)));

    m_hasCircle = false;
}

void RadialDimensionToolBase::deactivate()
{
    m_hasCircle = false;
}

QString DrawDimRadialTool::name() const { return QStringLiteral("DIMRADIUS"); }
QString DrawDimRadialTool::displayName() const { return QStringLiteral("半径标注"); }
QString DrawDimRadialTool::prompt() const
{
    return hasCircle() ? QStringLiteral("指定尺寸线位置:")
                       : pickPrompt();
}
QString DrawDimRadialTool::pickPrompt() const
{ return QStringLiteral("选择圆或圆弧:"); }

QString DrawDimDiametricTool::name() const { return QStringLiteral("DIMDIAMETER"); }
QString DrawDimDiametricTool::displayName() const { return QStringLiteral("直径标注"); }
QString DrawDimDiametricTool::prompt() const
{
    return hasCircle() ? QStringLiteral("指定尺寸线位置:")
                       : pickPrompt();
}
QString DrawDimDiametricTool::pickPrompt() const
{ return QStringLiteral("选择圆或圆弧:"); }

// =============================================================================
//  坐标标注：被测点 -> 引线端点
// =============================================================================
QString DrawDimOrdinateTool::name() const { return QStringLiteral("DIMORDINATE"); }
QString DrawDimOrdinateTool::displayName() const { return QStringLiteral("坐标标注"); }
QString DrawDimOrdinateTool::prompt() const
{
    return m_feature ? QStringLiteral("指定引线端点:")
                     : QStringLiteral("指定点坐标:");
}

bool DrawDimOrdinateTool::mousePress(const CanvasEvent& event)
{
    if (event.button != Qt::LeftButton || !ctx().document) return false;

    const ResolvedPoint resolved = resolve(event.worldPos, m_feature);
    if (!m_feature) {
        m_feature = resolved.position;
        notifyPromptChanged();
        repaint();
        return true;
    }

    m_cursor = resolved;
    m_hasCursor = true;
    commit();
    return true;
}

bool DrawDimOrdinateTool::mouseMove(const CanvasEvent& event)
{
    m_cursor = resolve(event.worldPos, m_feature);
    m_hasCursor = true;
    ctx().setSnapMarker(m_cursor.kind != SnapKind::None
                            ? std::optional<SnapResult>(m_cursor.toSnapResult())
                            : std::nullopt);
    repaint();
    return true;
}

bool DrawDimOrdinateTool::keyPress(QKeyEvent* event)
{
    if (event->key() == Qt::Key_U && m_feature) {
        m_feature.reset();
        notifyPromptChanged();
        repaint();
        return true;
    }
    return false;
}

bool DrawDimOrdinateTool::textInput(const QString& text)
{
    if (!m_feature) {
        // 第一步也可以直接键入坐标
        const std::optional<Point2D> point = parsePointInput(text, std::nullopt);
        if (!point) return true;
        m_feature = *point;
        notifyPromptChanged();
        repaint();
        return true;
    }

    const std::optional<Point2D> point = parsePointInput(text, m_feature);
    if (!point) return true;

    CanvasEvent synthetic;
    synthetic.worldPos = *point;
    synthetic.button = Qt::LeftButton;
    return mousePress(synthetic);
}

void DrawDimOrdinateTool::paintOverlay(QPainter& painter,
                                       const EntityRenderer::DrawContext& context)
{
    if (!m_feature || !m_hasCursor) return;

    DimensionEntity temp(DimensionEntity::DimType::Ordinate);
    temp.setFirstPoint(*m_feature);
    temp.setSecondPoint(m_cursor.position);
    temp.setDimLinePoint(Point2D(0.0, 0.0));    // 以 WCS 原点为坐标基准
    temp.setOrdinateXType(ordinateMeasuresX(*m_feature, m_cursor.position));
    drawPreviewEntity(painter, temp, context);
}

void DrawDimOrdinateTool::commit()
{
    if (!ctx().document || !m_feature) return;

    const Point2D feature = *m_feature;
    const Point2D leader = m_cursor.position;

    auto dim = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Ordinate);
    dim->setFirstPoint(feature);                // 13 被测点
    dim->setSecondPoint(leader);                // 14 引线端点
    dim->setDimLinePoint(Point2D(0.0, 0.0));    // 10 坐标基准 = WCS 原点
    dim->setOrdinateXType(ordinateMeasuresX(feature, leader));
    applyDefaults(*dim, ctx());

    const QString text = dim->displayText();
    const bool xType = dim->isOrdinateXType();
    ctx().document->addEntity(std::move(dim));

    message(QStringLiteral("已创建坐标标注：%1 坐标 %2")
                .arg(xType ? QStringLiteral("X") : QStringLiteral("Y"), text));
    m_feature.reset();
    notifyPromptChanged();
}

void DrawDimOrdinateTool::deactivate()
{
    m_feature.reset();
    m_hasCursor = false;
}

// =============================================================================
//  弧长标注：拾取圆弧 -> 指定尺寸弧位置
// =============================================================================
QString DrawDimArcTool::name() const { return QStringLiteral("DIMARC"); }
QString DrawDimArcTool::displayName() const { return QStringLiteral("弧长标注"); }
QString DrawDimArcTool::prompt() const
{
    return m_hasArc ? QStringLiteral("指定尺寸弧位置:")
                    : QStringLiteral("选择圆弧:");
}

bool DrawDimArcTool::mousePress(const CanvasEvent& event)
{
    if (event.button != Qt::LeftButton || !ctx().document) return false;

    if (!m_hasArc) {
        Point2D center;
        double radius = 0.0;
        double startAngle = 0.0;
        double endAngle = 0.0;
        if (!pickArcGeometry(ctx(), event.worldPos, &center, &radius, &startAngle, &endAngle)) {
            message(QStringLiteral("未拾取到圆弧——弧长标注只能量圆弧，请点圆弧的边线附近。"));
            return true;
        }
        m_center = center;
        m_radius = radius;
        m_startAngle = startAngle;
        m_endAngle = endAngle;
        m_hasArc = true;
        notifyPromptChanged();
        repaint();
        return true;
    }

    const ResolvedPoint resolved = resolve(event.worldPos, m_center);
    m_cursor = resolved;
    m_hasCursor = true;
    commit();
    return true;
}

bool DrawDimArcTool::mouseMove(const CanvasEvent& event)
{
    m_cursor = resolve(event.worldPos, m_hasArc ? std::optional<Point2D>(m_center) : std::nullopt);
    m_hasCursor = true;
    ctx().setSnapMarker(m_cursor.kind != SnapKind::None
                            ? std::optional<SnapResult>(m_cursor.toSnapResult())
                            : std::nullopt);
    repaint();
    return true;
}

bool DrawDimArcTool::keyPress(QKeyEvent* event)
{
    if (event->key() == Qt::Key_U && m_hasArc) {
        m_hasArc = false;
        notifyPromptChanged();
        repaint();
        return true;
    }
    return false;
}

bool DrawDimArcTool::textInput(const QString& text)
{
    if (!m_hasArc) return false;

    const std::optional<Point2D> point = parsePointInput(text, m_center);
    if (!point) return true;

    CanvasEvent synthetic;
    synthetic.worldPos = *point;
    synthetic.button = Qt::LeftButton;
    return mousePress(synthetic);
}

void DrawDimArcTool::paintOverlay(QPainter& painter,
                                  const EntityRenderer::DrawContext& context)
{
    if (!m_hasArc || !m_hasCursor) return;

    // 尺寸弧半径取自光标到弧心的距离；与弧心重合时没有可画的半径，不预览
    if (m_cursor.position.distanceTo(m_center) <= kGeomTol) return;

    DimensionEntity temp(DimensionEntity::DimType::ArcLength);
    temp.setArcCenter(m_center);
    temp.setArcAngles(m_startAngle, m_endAngle);
    temp.setFirstPoint(m_center + Vector2D(std::cos(m_startAngle), std::sin(m_startAngle)) * m_radius);
    temp.setSecondPoint(m_center + Vector2D(std::cos(m_endAngle), std::sin(m_endAngle)) * m_radius);
    temp.setDimLinePoint(m_cursor.position);
    drawPreviewEntity(painter, temp, context);
}

void DrawDimArcTool::commit()
{
    if (!ctx().document || !m_hasArc) return;

    // 尺寸弧半径 = 光标到弧心的距离。为 0 时尺寸弧退化成一个点，几何算不出来
    if (m_cursor.position.distanceTo(m_center) <= kGeomTol) {
        message(QStringLiteral("尺寸弧位置不能落在弧心上，请离开圆心再点一次。"));
        return;
    }

    auto dim = std::make_unique<DimensionEntity>(DimensionEntity::DimType::ArcLength);
    dim->setArcCenter(m_center);                // 15 弧心
    dim->setArcAngles(m_startAngle, m_endAngle); // 40/41 起止参数（弧度）
    dim->setFirstPoint(m_center + Vector2D(std::cos(m_startAngle), std::sin(m_startAngle)) * m_radius);
    dim->setSecondPoint(m_center + Vector2D(std::cos(m_endAngle), std::sin(m_endAngle)) * m_radius);
    dim->setDimLinePoint(m_cursor.position);    // 10 尺寸弧经过的点
    applyDefaults(*dim, ctx());

    const double value = dim->measurement();
    ctx().document->addEntity(std::move(dim));

    message(QStringLiteral("已创建弧长标注：%1").arg(formatLength(value)));
    m_hasArc = false;
}

void DrawDimArcTool::deactivate()
{
    m_hasArc = false;
    m_hasCursor = false;
}

} // namespace lycad
