// =============================================================================
//  LyCAD - interact/ToolContext.cpp
// =============================================================================
#include "interact/ToolContext.h"

#include "core/Document.h"
#include "interact/ToolManager.h"
#include "render/CadCanvas.h"

#include <algorithm>

namespace lycad {

double ToolContext::pickToleranceWorld() const
{
    if (!canvas) return 1.0;
    return canvas->pickTolerance();
}

double ToolContext::apertureWorld() const
{
    if (!canvas) return 1.0;
    return canvas->view().deviceLengthToWorld(snap.aperturePixels);
}

ResolvedPoint ToolContext::resolvePoint(const Point2D& raw,
                                        const std::optional<Point2D>& base) const
{
    ResolvedPoint result;
    result.position = raw;

    if (document && canvas && snap.enabled) {
        SnapEngine engine(snap);
        const double aperture = apertureWorld();

        std::vector<EntityId> exclude;
        if (auto snapResult = engine.resolve(*document, raw, aperture, base, exclude)) {
            result.position = snapResult->position;
            result.kind = snapResult->kind;
            result.label = snapResult->label;
        }
    }

    // 对象捕捉未命中时，才施加方向约束：
    // 用户明确指到某个几何特征时，不应被正交模式拉走。
    if (result.kind == SnapKind::None && base) {
        if (orthoMode) {
            result.position = SnapEngine::applyOrtho(*base, raw);
            result.kind = SnapKind::Ortho;
            result.label = snapKindShortLabel(SnapKind::Ortho);
        } else if (polarTracking && polarStepDeg > 0.0) {
            const Point2D polar = SnapEngine::applyPolar(*base, raw, polarStepDeg);
            if (polar.distanceTo(raw) > kGeomTol) {
                result.position = polar;
                result.kind = SnapKind::Polar;
                result.label = snapKindShortLabel(SnapKind::Polar);
            }
        }
    }

    return result;
}

void ToolContext::setSnapMarker(const std::optional<SnapResult>& snap) const
{
    if (canvas) canvas->setSnapResult(snap);
}

void ToolContext::requestRepaint() const
{
    if (canvas) canvas->update();
}

void ToolContext::message(const QString& text) const
{
    if (manager) manager->postMessage(text);
}

void ToolContext::finishCommand() const
{
    if (manager) manager->finishCurrentTool();
}

const LayerManager* ToolContext::layers() const
{
    return document ? &document->layers() : nullptr;
}

} // namespace lycad
