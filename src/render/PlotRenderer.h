// =============================================================================
//  LyCAD - render/PlotRenderer.h
//  出图（打印 / 打印预览）的绘制核心。
//
//  与屏幕渲染的区别集中在「画什么、按什么比例画」两件事上：
//    · 屏幕上画布跟随视图——可缩放、可平移，有栅格、坐标轴、十字光标、选择
//      高亮与夹点；纸面上这些都不存在。纸只有一种取景：**把出图范围铺满纸张
//      的可打印区域**，底恒为白。
//    · 屏幕上线框恒为 1 像素（与 AutoCAD 的默认口径一致，信息层级靠颜色与线型
//      表达）；纸面上线宽必须落到打印机的物理分辨率上，否则 0.5 mm 的粗线打
//      出来仍是一根细线。
//
//  因此本模块不碰 CadCanvas，也不认识 QPrinter：它只接受「一个 QPainter + 一
//  块目标矩形 + 每毫米多少设备像素」。于是打印、打印预览、导出 PDF、渲染成位图
//  四条路共用同一份绘制代码——**预览即所得**不是靠两处代码写得一样来保证的。
//
//  出图范围（plotExtents）与「缩放至图形范围」刻意不同：
//    · 图层「不打印」（Layer::isPlottable）与关闭/冻结的图层上的图元都不计入，
//      否则一张关掉的辅助图层会把主体压成纸上的一个小角落；
//    · 不可见图元（Entity::isVisible）同样不计入。
// =============================================================================
#pragma once

#include "core/Types.h"

#include <QPixmap>
#include <QRectF>

#include <functional>

class QPainter;

namespace lycad {

class Document;

/// 出图比例。纸面与图纸的换算约定：**纸面 1 mm = n 个图形单位**，
/// 与工程制图的「1:n」一致（图上 1 mm 代表实物 n mm，前提是文件单位为毫米）。
struct PlotScale {
    enum class Mode {
        FitToPaper,   ///< 适应图纸：把出图范围铺满可打印区（默认）
        OneToOne,     ///< 1:1：纸面 1 mm = 1 个图形单位
        Custom        ///< 自定义 1:n
    };

    Mode mode = Mode::FitToPaper;
    /// 自定义比例的 n（仅 mode == Custom 时使用）；n 越大，图在纸上越小
    double custom = 1.0;

    /// 换算成「每个图形单位多少纸面毫米」；FitToPaper 没有固定值，返回 0
    double millimetersPerUnit() const noexcept;
};

/// 比例的可读名称，供命令行消息与设置对话框使用
QString plotScaleDisplayName(const PlotScale& scale);

/// 出图参数
struct PlotOptions {
    PlotScale scale;
    /// 目标矩形再向内让出的边距（毫米）。多数打印设备有不可打印的边，
    /// 贴边画的内容会被裁掉，因此默认留出 6 mm。
    double marginMm = 6.0;
};

/// 固定比例出图的分页网格：图纸超出一张纸时按「先行后列」铺开，
/// 第 0 页在最左上角，与 AutoCAD 的拼页方向一致。
struct PlotTiling {
    int rows = 1;
    int columns = 1;

    int count() const noexcept { return rows * columns; }
    int rowOf(int page) const noexcept { return page / std::max(1, columns); }
    int columnOf(int page) const noexcept { return page % std::max(1, columns); }
};

/// 计算分页网格。适应图纸恒为 1×1；固定比例按
/// 「每页可容纳的世界尺寸 = 可打印区像素 ÷ 像素每单位」向上取整。
PlotTiling plotTiling(const Document& document, const QRectF& targetRect,
                      double devicePerMillimeter, const PlotOptions& options);

/// 会打印出来的图元的包围盒；没有任何可打印内容时返回无效盒
Box2D plotExtents(const Document& document);

/**
 * 把图纸铺满 targetRect 后绘制。
 *
 * @param painter              已 begin 的画笔；本函数只做 save/restore，不改其状态
 * @param document             要出的图
 * @param targetRect           目标矩形（设备像素），原点即纸张可打印区左上角
 * @param devicePerMillimeter  每毫米对应的设备像素数（打印机分辨率 / 25.4，
 *                             位图则由调用方按需要的精度给出）
 * @param options              出图参数
 * @param page                 页号（0 起）。适应图纸恒为 0；固定比例超出一张纸时
 *                             按 plotTiling 的网格取对应拼块
 */
void renderToRect(QPainter& painter, const Document& document, const QRectF& targetRect,
                  double devicePerMillimeter, const PlotOptions& options = PlotOptions{},
                  int page = 0);

/**
 * 把整份图纸绘到 targetRect 上：固定比例超出一张纸时自动分页，
 * 页与页之间走 QPainter::newPage()。
 *
 * 打印与预览**都必须用它**：预览的页数、每页内容与真正打印的输出由此保证一致——
 * 如果两处各自算页数，改一处忘另一处就会变成「预览 4 页、打出来 1 页」。
 */
void renderAllPages(QPainter& painter, const Document& document, const QRectF& targetRect,
                    double devicePerMillimeter, const PlotOptions& options = PlotOptions{},
                    const std::function<bool()>& newPage = {});

/// 渲染成一张位图，用于预览缩略、自检与文档配图。
///
/// 边距按 **300 dpi** 折算，因此 pixelSize 应当传与纸张同比例的尺寸
/// （A4 横向即 3508×2480），这样位图上的边距和纸面一致；传任意尺寸也能画，
/// 只是边距与画面的相对比例会随之变化。
QPixmap renderToPixmap(const Document& document, const QSize& pixelSize,
                       const PlotOptions& options = PlotOptions{});

} // namespace lycad
