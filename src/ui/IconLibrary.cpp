#include "ui/IconLibrary.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QSvgRenderer>

// 资源由本静态库携带。显式初始化可确保链接器把 qrc 生成的目标文件拉进来:
// 静态库中仅靠静态初始化器的资源注册，在目标文件未被引用时会被整块丢弃。
// 注意宏必须出现在全局作用域，否则 extern 声明会落到命名空间里而链接不上。
static void lycadInitUiResources()
{
    Q_INIT_RESOURCE(lycad);
}

namespace lycad {
namespace {

/// 图标源文件里的固定调色板，以及它在深色主题下的对应色。
///
/// 这里只做「逐色值整体替换」，不做色相推导：结果完全可预期，
/// 也便于用肉眼在图标总览图里核对。两列的颜色语义必须一一对应，
/// 例如「主色」在浅色下是深蓝、在深色下必须是亮蓝，而不是随便换个颜色。
struct PalettePair {
    const char* light;
    const char* dark;
};

const PalettePair kPalette[] = {
    { "#3b78c3", "#6cb2f0" },   // 主色：图元轮廓
    { "#9dc2e8", "#3d6b9c" },   // 次色：辅助轮廓、填充面
    { "#d9822b", "#f0a952" },   // 强调：夹点、箭头
    { "#4c9a4c", "#6ec96e" },   // 正向
    { "#c05046", "#ec7a6e" },   // 负向：删除
    { "#8a94a6", "#9aa6b8" },   // 参考线、虚线、文字基线
    { "#2b3038", "#14181d" },   // 「深色画布」图标中的暗半
    { "#e8ecf1", "#e8ecf1" },   // …与之相对的亮半
};

} // namespace

IconLibrary::IconLibrary()
{
    lycadInitUiResources();
}

IconLibrary& IconLibrary::instance()
{
    static IconLibrary library;
    return library;
}

QString IconLibrary::resourcePath(const QString& name) const
{
    return QStringLiteral(":/icons/%1.svg").arg(name);
}

bool IconLibrary::contains(const QString& name) const
{
    return QFile::exists(resourcePath(name));
}

QStringList IconLibrary::names()
{
    lycadInitUiResources();

    QStringList out;
    const QDir dir(QStringLiteral(":/icons"));
    const QStringList files = dir.entryList({ QStringLiteral("*.svg") }, QDir::Files, QDir::Name);
    out.reserve(files.size());
    for (const QString& file : files) {
        out << QFileInfo(file).completeBaseName();
    }
    return out;
}

void IconLibrary::setDarkTheme(bool dark)
{
    if (m_dark == dark) return;

    m_dark = dark;
    // 换色后的源码与已栅格化的位图都必须重来，否则会继续用旧配色的位图
    m_sources.clear();
    m_icons.clear();
}

QByteArray IconLibrary::sourceOf(const QString& name)
{
    const auto cached = m_sources.constFind(name);
    if (cached != m_sources.constEnd()) return cached.value();

    QFile file(resourcePath(name));
    if (!file.open(QIODevice::ReadOnly)) return {};

    QByteArray svg = file.readAll();
    if (m_dark) {
        for (const PalettePair& pair : kPalette) {
            // 源文件统一使用小写十六进制，因此可以按字面量直接替换
            svg.replace(pair.light, pair.dark);
        }
    }

    m_sources.insert(name, svg);
    return svg;
}

QIcon IconLibrary::applicationIcon()
{
    // 一次性栅格化常用档位：窗口标题栏一般取 16/24，任务栏 32/48，
    // Alt+Tab 与文件管理器会要 128/256。QIcon 内部按请求尺寸取最接近的一档。
    static const int kSizes[] = { 16, 24, 32, 48, 64, 128, 256 };

    QIcon icon;
    QSvgRenderer renderer(QStringLiteral(":/app/app-icon.svg"));
    if (!renderer.isValid()) {
        // 与应用图标有关的问题（qrc 前缀写错、SVG 语法错、资源没编进来）
        // 只有一个症状：软件在任务栏里显示成默认的空白图标。这里明确报出来。
        qWarning("IconLibrary: 无法渲染应用图标 :/app/app-icon.svg");
        return icon;
    }

    for (const int size : kSizes) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter painter(&pm);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        renderer.render(&painter);
        painter.end();
        icon.addPixmap(pm);
    }
    return icon;
}

QPixmap IconLibrary::pixmap(const QString& name, int size)
{
    const QByteArray svg = sourceOf(name);
    if (svg.isEmpty() || size <= 0) return {};

    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) return {};

    QPixmap pm(size, size);
    pm.fill(Qt::transparent);

    QPainter painter(&pm);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&painter);
    painter.end();

    return pm;
}

QIcon IconLibrary::icon(const QString& name, int size)
{
    const QString key = name + QLatin1Char('@') + QString::number(size);
    const auto cached = m_icons.constFind(key);
    if (cached != m_icons.constEnd()) return cached.value();

    if (!contains(name) || size <= 0) {
        // 名字写错时不要静默返回空图标——那会让按钮看起来像坏了。
        // 留一条警告，配合 tools/icon_sheet 可以立刻定位。
        qWarning("IconLibrary: 找不到图标 \"%s\"", qPrintable(name));
        return {};
    }

    QIcon icon;
    for (const int factor : { 1, 2 }) {
        QPixmap pm = pixmap(name, size * factor);
        if (pm.isNull()) continue;
        // DPR 只影响 Qt 选择哪一档，逻辑尺寸始终是 size
        pm.setDevicePixelRatio(factor);
        icon.addPixmap(pm);
    }

    m_icons.insert(key, icon);
    return icon;
}

} // namespace lycad
