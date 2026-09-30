// =============================================================================
//  LyCAD - ui/IconLibrary.h
//  SVG 图标库：界面图标的唯一来源。
//
//  图标以 SVG 源文件存放于 qrc 资源中，运行时按需要的像素尺寸栅格化。
//  不预先烘焙成 PNG 有两个原因：同一枚图标要在大按钮（24px）与小按钮（16px）
//  两种尺寸下使用，矢量源在每种尺寸下都清晰；资源体积也小得多。
//
//  深色主题：Qt 的 SVG 渲染器不会按控件调色板重新着色，因此深色底上的
//  可辨识性由「取源码 → 替换调色板 → 重新栅格化」保证。图标源文件刻意
//  只用一组固定色值（见 IconLibrary.cpp 的 kPalette），就是为让这个替换
//  保持简单可预期；换错一个色值在工具生成的图标总览图里会立刻露出来。
// =============================================================================
#pragma once

#include <QByteArray>
#include <QHash>
#include <QIcon>
#include <QPixmap>
#include <QString>
#include <QStringList>

namespace lycad {

class IconLibrary {
public:
    static IconLibrary& instance();

    /// 切换深色配色。已缓存的图标会被清除，后续取用按新配色渲染。
    void setDarkTheme(bool dark);
    bool isDarkTheme() const noexcept { return m_dark; }

    /// 取图标。size 为逻辑像素边长，会同时准备 1x 与 2x 两档以适配高分屏。
    QIcon icon(const QString& name, int size = 24);

    /// 取单张位图（供自检工具拼图标总览图使用）
    QPixmap pixmap(const QString& name, int size = 24);

    /// 资源中是否真的存在这个名字的图标
    bool contains(const QString& name) const;

    /// 当前资源里的全部图标名（按名称排序），用于自检与文档
    static QStringList names();

private:
    IconLibrary();

    QString resourcePath(const QString& name) const;
    /// 读取 SVG 源码，按当前主题替换调色板
    QByteArray sourceOf(const QString& name);

    bool m_dark = false;
    QHash<QString, QByteArray> m_sources;   ///< 已换色的 SVG 源码，按名字缓存
    QHash<QString, QIcon>      m_icons;     ///< 已完成栅格化的图标，键为「名字@尺寸」
};

} // namespace lycad
