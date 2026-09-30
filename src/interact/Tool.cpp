// =============================================================================
//  LyCAD - interact/Tool.cpp
// =============================================================================
#include "interact/Tool.h"

#include "core/Document.h"
#include "interact/CoordinateInput.h"
#include "interact/ToolManager.h"

#include <cmath>

namespace lycad {

Tool::Tool(ToolContext& context)
    : m_context(context)
{
}

Tool::~Tool() = default;

bool Tool::mousePress(const CanvasEvent& event)
{
    Q_UNUSED(event);
    return false;
}

bool Tool::mouseMove(const CanvasEvent& event)
{
    Q_UNUSED(event);
    return false;
}

bool Tool::mouseRelease(const CanvasEvent& event)
{
    Q_UNUSED(event);
    return false;
}

bool Tool::mouseDoubleClick(const CanvasEvent& event)
{
    Q_UNUSED(event);
    return false;
}

bool Tool::keyPress(QKeyEvent* event)
{
    Q_UNUSED(event);
    return false;
}

bool Tool::textInput(const QString& text)
{
    Q_UNUSED(text);
    return false;
}

void Tool::paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context)
{
    Q_UNUSED(painter);
    Q_UNUSED(context);
}

void Tool::finish()
{
    m_running = false;
    if (m_context.manager) m_context.manager->finishCurrentTool();
}

void Tool::notifyPromptChanged()
{
    if (m_context.manager) m_context.manager->notifyPromptChanged();
}

ResolvedPoint Tool::resolve(const Point2D& raw, const std::optional<Point2D>& base) const
{
    return m_context.resolvePoint(raw, base);
}

void Tool::message(const QString& text) const
{
    m_context.message(text);
}

void Tool::repaint() const
{
    m_context.requestRepaint();
}

QString Tool::currentLayerName() const
{
    if (!m_context.document) return QStringLiteral("0");
    return m_context.document->layers().currentLayerName();
}

std::optional<Point2D> Tool::parsePointInput(const QString& text,
                                             const std::optional<Point2D>& lastPoint) const
{
    const ParsedCoordinate parsed = parseCoordinate(text);
    if (!parsed.isValid()) {
        message(parsed.error);
        return std::nullopt;
    }
    if (!parsed.isPointLike()) {
        message(QStringLiteral("此处需要输入一个点，例如 100,50 或 @50,0 或 50<45。"));
        return std::nullopt;
    }

    const std::optional<Point2D> point = parsed.resolve(lastPoint);
    if (!point) {
        message(QStringLiteral("该输入是相对坐标，但当前没有可参照的上一点。"));
        return std::nullopt;
    }
    return point;
}

std::optional<double> Tool::parseScalarInput(const QString& text, const QString& what) const
{
    const ParsedCoordinate parsed = parseCoordinate(text);
    if (!parsed.isValid()) {
        message(parsed.error);
        return std::nullopt;
    }

    double value = 0.0;
    if (parsed.kind == ParsedCoordinate::Kind::Scalar) {
        value = parsed.scalar;
    } else if (parsed.kind == ParsedCoordinate::Kind::Polar) {
        value = parsed.distance;                 // 「50<45」中的长度部分
    } else {
        message(QStringLiteral("此处需要输入%1，请勿使用点坐标形式。").arg(what));
        return std::nullopt;
    }

    if (!std::isfinite(value)) {
        message(QStringLiteral("%1不是有效数值。").arg(what));
        return std::nullopt;
    }
    return value;
}

bool Tool::matchesOption(const QString& text, const QString& option)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || option.isEmpty()) return false;
    return trimmed.compare(option, Qt::CaseInsensitive) == 0;
}

} // namespace lycad
