// =============================================================================
//  LyCAD - render/PlotRenderer.cpp
// =============================================================================
#include "render/PlotRenderer.h"

#include "core/Document.h"
#include "core/Entity.h"
#include "core/Layer.h"
#include "render/CanvasTypes.h"
#include "render/EntityRenderer.h"
#include "render/ViewTransform.h"

#include <QPainter>

#include <algorithm>
#include <cmath>

namespace lycad {

namespace {

/// 内容与图框之间再留一点余量（占图框的 2%）。
/// 取 0 时最外侧的图元会正好压在框线上：线宽有一半落在框外被裁掉，
/// 标注尺寸线看起来像被切了一刀。2% 在纸面上几乎看不出来，却避免了这种观感。
constexpr double kContentMarginRatio = 0.02;

/// 出图用的显示设置：与屏幕渲染只差「画什么」
DisplaySettings plotSettings()
{
    DisplaySettings settings;
    settings.background = QColor(Qt::white);
    settings.showGrid = false;          // 纸上没有栅格
    settings.showAxes = false;          // 也没有坐标轴
    settings.showCrosshair = false;
    settings.showSelection = false;     // 选择高亮与夹点属于编辑态，不进图纸
    settings.antialias = true;
    settings.themeIsDark = false;       // 7 号色的反相只属于深色屏幕

    // 纸上按线宽绘制：0.25 mm 的细线与 0.7 mm 的粗线必须分得出来。
    // 屏幕上默认恒为 1 像素（AutoCAD 的默认口径），纸面照搬会让粗细全丢。
    settings.showLineweight = true;
    settings.minLinewidthPixels = 1.0;
    settings.maxLinewidthPixels = 400.0;   // 上限给足，真正的宽度由毫米决定
    settings.maxSagittaPixels = 0.2;       // 离散精度比屏幕高一档
    return settings;
}

/// 该图元是否应该出现在图纸上：可见 + 图层未关闭/冻结 + 图层允许打印。
/// 与 Entity::isDisplayable（只管屏幕可见性）的区别就在最后一条。
bool shouldPlot(const Entity& entity, const LayerManager& layers)
{
    if (!entity.isDisplayable(layers)) return false;
    if (const Layer* layer = layers.find(entity.layerName())) {
        return layer->isPlottable();
    }
    return true;   // 图层缺失时按可打印处理，与「图层缺失时按可见处理」同一口径
}

} // namespace

namespace {

/// 可打印区：目标矩形去掉边距。renderToRect 与 plotTiling 必须用它同一份结果，
/// 否则「算出来的页数」和「画出来的页数」会各说各话。
QRectF plotFrameRect(const QRectF& targetRect, const PlotOptions& options,
                     double devicePerMillimeter)
{
    const double margin = std::max(0.0, options.marginMm) * devicePerMillimeter;
    QRectF frame = targetRect.adjusted(margin, margin, -margin, -margin);
    if (frame.width() < 8.0 || frame.height() < 8.0) frame = targetRect;
    return frame;
}

/// 像素 / 图形单位；适应图纸没有固定比例，返回 0（由 fitTo 决定）
double plotPixelsPerUnit(const PlotOptions& options, double devicePerMillimeter)
{
    switch (options.scale.mode) {
    case PlotScale::Mode::OneToOne: return devicePerMillimeter;
    case PlotScale::Mode::Custom:
        return devicePerMillimeter / std::max(1e-6, options.scale.custom);
    case PlotScale::Mode::FitToPaper: break;
    }
    return 0.0;
}

/// 单个方向上的页数：内容尺寸 ÷ 每页可容纳的世界尺寸，向上取整。
/// 封顶 1000 是最后一道闸：自定义比例可以小到 0.0001（放大一万倍），
/// 没有闸的话页数会爆成一个天文数字，把预览和打印一起拖死。
constexpr int kMaxPagesPerAxis = 1000;

int pagesFor(double contentSize, double pageSize)
{
    if (!(pageSize > 0.0)) return 1;
    const double pages = std::ceil(contentSize / pageSize);
    return static_cast<int>(std::clamp(pages, 1.0, static_cast<double>(kMaxPagesPerAxis)));
}

} // namespace

double PlotScale::millimetersPerUnit() const noexcept
{
    switch (mode) {
    case Mode::OneToOne: return 1.0;
    case Mode::Custom:   return std::max(1e-6, custom);
    case Mode::FitToPaper: break;
    }
    return 0.0;   // 适应图纸没有固定比例
}

QString plotScaleDisplayName(const PlotScale& scale)
{
    switch (scale.mode) {
    case PlotScale::Mode::FitToPaper: return QStringLiteral("适应图纸");
    case PlotScale::Mode::OneToOne:   return QStringLiteral("1:1");
    case PlotScale::Mode::Custom:
        return QStringLiteral("自定义 1:%1").arg(scale.custom, 0, 'g', 6);
    }
    return QString();
}

Box2D plotExtents(const Document& document)
{
    Box2D box;
    const LayerManager& layers = document.layers();
    for (EntityId id : document.drawOrder()) {
        const Entity* entity = document.entity(id);
        if (!entity || !shouldPlot(*entity, layers)) continue;
        box.expand(entity->boundingBox());
    }
    return box;
}

PlotTiling plotTiling(const Document& document, const QRectF& targetRect,
                      double devicePerMillimeter, const PlotOptions& options)
{
    PlotTiling tiling;
    if (options.scale.mode == PlotScale::Mode::FitToPaper) return tiling;   // 恒 1×1

    const Box2D content = plotExtents(document);
    if (!content.isValid()) return tiling;

    const double pxPerUnit = plotPixelsPerUnit(options, devicePerMillimeter);
    if (!(pxPerUnit > 0.0) || !std::isfinite(pxPerUnit)) return tiling;

    const QRectF frame = plotFrameRect(targetRect, options, devicePerMillimeter);
    // 每页可容纳的世界尺寸 = 可打印区像素 ÷ 像素每单位
    tiling.columns = pagesFor(content.width(), frame.width() / pxPerUnit);
    tiling.rows = pagesFor(content.height(), frame.height() / pxPerUnit);
    return tiling;
}

void renderToRect(QPainter& painter, const Document& document, const QRectF& targetRect,
                  double devicePerMillimeter, const PlotOptions& options, int page)
{
    if (!(targetRect.width() > 1.0) || !(targetRect.height() > 1.0)) return;
    if (!(devicePerMillimeter > 0.0) || !std::isfinite(devicePerMillimeter)) return;

    const DisplaySettings settings = plotSettings();
    const LayerManager& layers = document.layers();

    // 图框：纸张可打印区再让出边距。多数设备有不可打印的纸边，贴边的内容会被裁掉。
    const double margin = std::max(0.0, options.marginMm) * devicePerMillimeter;
    QRectF frame = targetRect.adjusted(margin, margin, -margin, -margin);
    if (frame.width() < 8.0 || frame.height() < 8.0) frame = targetRect;

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, settings.antialias);

    // 取景：视口取「可打印区」尺寸，随后把画笔原点移到可打印区左上角，
    // 坐标就落在它的局部坐标系里
    const Box2D content = plotExtents(document);
    const QSize frameSize(static_cast<int>(frame.width()),
                          static_cast<int>(frame.height()));
    const ViewTransform view = [&]() {
        ViewTransform v;
        switch (options.scale.mode) {
        case PlotScale::Mode::FitToPaper:
            // 铺满：内容四周再留 2%，否则最外侧图元会顶到纸边，线宽有一半被裁掉
            v.fitTo(content, frameSize, kContentMarginRatio);
            break;
        case PlotScale::Mode::OneToOne:
        case PlotScale::Mode::Custom: {
            // 固定比例：像素/单位由「毫米/单位」换算而来（见 PlotScale 的约定）。
            // 内容超出一张纸时按 plotTiling 的网格取拼块：网格整体相对内容
            // 居中，因此只有一页时内容同样居中，多页时四周边距对称。
            const double pxPerUnit = plotPixelsPerUnit(options, devicePerMillimeter);
            const PlotTiling tiling =
                plotTiling(document, targetRect, devicePerMillimeter, options);
            const int index = std::clamp(page, 0, tiling.count() - 1);
            const int row = tiling.rowOf(index);
            const int column = tiling.columnOf(index);

            const double pageW = frame.width() / pxPerUnit;
            const double pageH = frame.height() / pxPerUnit;
            const double gridLeft =
                content.minX - (tiling.columns * pageW - content.width()) * 0.5;
            const double gridTop =
                content.maxY + (tiling.rows * pageH - content.height()) * 0.5;
            const Point2D tileCenter(gridLeft + (column + 0.5) * pageW,
                                     gridTop - (row + 0.5) * pageH);

            v.setScale(pxPerUnit, Point2D(0.0, 0.0));
            v.centerOn(tileCenter, frameSize);
            break;
        }
        }
        return v;
    }();

    painter.translate(frame.topLeft());
    painter.setClipRect(QRectF(0.0, 0.0, frame.width(), frame.height()));

    EntityRenderer::DrawContext context;
    context.view = &view;
    context.settings = &settings;
    context.viewport = QRectF(0.0, 0.0, frame.width(), frame.height());

    // EntityRenderer 把线宽参数当作**世界长度**（penWidthPixels 内部走
    // worldLengthToDevice），所以纸面毫米必须先换成「与纸面 1 mm 等长的世界长度」。
    // 直接传毫米会让 0.5 mm 的粗线在 1:50 的图上错成 25 mm 的墨块，或反过来细成
    // 一根看不见的丝——取决于比例是放大还是缩小。
    const double worldPerMillimeter = view.worldPerDevice() * devicePerMillimeter;
    // 块参照按子图元各自的图层重新解析样式；线宽换算系数要和块外的完全一致，
    // 否则同一张图里块内的粗线画得跟块外不一样粗。
    context.layers = &layers;
    context.lineweightWorldScale = worldPerMillimeter;

    for (EntityId id : document.drawOrder()) {
        const Entity* entity = document.entity(id);
        if (!entity || !shouldPlot(*entity, layers)) continue;

        const QColor color = entity->resolvedColor(layers, false);
        const double lineweight = entity->resolvedLineweight(layers) * worldPerMillimeter;

        QString linetype = entity->linetypeName();
        if (linetype.isEmpty()) {
            linetype = layers.linetypeOf(entity->layerName(), QStringLiteral("CONTINUOUS"));
        }

        EntityRenderer::draw(painter, *entity, context, color, lineweight, linetype);
    }

    painter.setClipping(false);
    painter.restore();
}

void renderAllPages(QPainter& painter, const Document& document, const QRectF& targetRect,
                    double devicePerMillimeter, const PlotOptions& options,
                    const std::function<bool()>& newPage)
{
    const PlotTiling tiling = plotTiling(document, targetRect, devicePerMillimeter, options);
    for (int page = 0; page < tiling.count(); ++page) {
        // 翻页只能由设备执行（QPrinter::newPage 之类），本模块不认识打印设备，
        // 因此把「怎么翻页」作为回调交给调用方；没有回调时只画第一页
        if (page > 0 && (!newPage || !newPage())) return;
        renderToRect(painter, document, targetRect, devicePerMillimeter, options, page);
    }
}

QPixmap renderToPixmap(const Document& document, const QSize& pixelSize,
                       const PlotOptions& options)
{
    if (pixelSize.width() <= 0 || pixelSize.height() <= 0) return {};

    QPixmap pixmap(pixelSize);
    pixmap.fill(Qt::white);     // 纸是白的：屏幕上可能是深色底色，纸上不是

    QPainter painter(&pixmap);
    // 位图按 300 dpi 的口径折算毫米量：同一份 options 在位图与纸面上观感一致
    renderToRect(painter, document, QRectF(QPointF(0.0, 0.0), QSizeF(pixelSize)),
                 300.0 / 25.4, options);
    return pixmap;
}

} // namespace lycad
