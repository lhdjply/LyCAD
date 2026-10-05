// =============================================================================
//  LyCAD - core/TextStyle.h
//  文字样式与文字样式表。
//
//  为什么需要它
//    文字样式是**文档级**的数据（同名的样式被成百上千个图元共用），
//    字体信息只应该存一份：图元只保留样式名（DXF 7 组码，见 TextEntity），
//    字体由本表按名解析。这与图层表的分工一致。
//
//  与 DXF STYLE 表记录的对应关系
//    2    样式名
//    3    主字体文件名（可以为空）
//    4    大字体文件名（中文等 CJK 字形所在，SHX 时代的标准做法）
//    1001/1000/1071  ACAD 扩展数据：**从字体列表里选中的 TrueType 字体族名**
//          与字体族标志。AutoCAD 用这种方式记录 TrueType 字体，此时 3 组码
//          是空的——字体信息只存在于扩展数据里。丢了它，导出的汉字会因为
//          回退到没有汉字字形的 txt.shx 而整片变成 "?"。
// =============================================================================
#pragma once

#include <QString>

#include <cstddef>
#include <vector>

namespace lycad {

// -----------------------------------------------------------------------------
//  TextStyle
// -----------------------------------------------------------------------------
class TextStyle {
public:
    TextStyle() = default;
    explicit TextStyle(const QString& name) : m_name(name) {}

    const QString& name() const noexcept { return m_name; }
    void setName(const QString& n) { m_name = n; }

    /// 主字体文件名（DXF 3），如 "txt" / "simhei.ttf"；未指定时为空
    const QString& fontFile() const noexcept { return m_fontFile; }
    void setFontFile(const QString& f) { m_fontFile = f.trimmed(); }

    /// 大字体文件名（DXF 4），如 "gbcbig.shx"；未指定时为空
    const QString& bigFontFile() const noexcept { return m_bigFontFile; }
    void setBigFontFile(const QString& f) { m_bigFontFile = f.trimmed(); }

    /// TrueType 字体族名（扩展数据 1000），如 "SimSun"；非 TrueType 样式为空
    const QString& ttfName() const noexcept { return m_ttfName; }
    void setTtfName(const QString& n) { m_ttfName = n.trimmed(); }

    /// TrueType 字体族标志（扩展数据 1071）：倾斜/加粗/字符集打包在一个整数里
    int ttfFlags() const noexcept { return m_ttfFlags; }
    void setTtfFlags(int flags) noexcept { m_ttfFlags = flags; }

    /// 是否携带任何字体信息。全为空说明文件没告诉我们可以用什么字体，
    /// 导出时应当回退到内置的中文安全字体，而不是写出一个没有字形的样式。
    bool hasFontInfo() const noexcept
    {
        return !m_fontFile.isEmpty() || !m_bigFontFile.isEmpty() || !m_ttfName.isEmpty();
    }

private:
    QString m_name;
    QString m_fontFile;
    QString m_bigFontFile;
    QString m_ttfName;
    int     m_ttfFlags = 0;
};

// -----------------------------------------------------------------------------
//  TextStyleTable
// -----------------------------------------------------------------------------
class TextStyleTable {
public:
    /// 初始状态：只含一条 Standard，且带上中文字体（见构造函数说明）
    TextStyleTable();

    std::size_t count() const noexcept { return m_styles.size(); }
    bool isEmpty() const noexcept { return m_styles.empty(); }
    const std::vector<TextStyle>& all() const noexcept { return m_styles; }

    const TextStyle* find(const QString& name) const;
    TextStyle* find(const QString& name);

    /// 不存在则创建（导入外部文件时补齐缺失样式）
    TextStyle& ensure(const QString& name);

    /// 重置为初始状态（仅含 Standard）
    void clear();

    /// 中文安全字体：主字体 txt.shx + GB 码大字体 gbcbig.shx。
    /// 两者都随 AutoCAD / GstarCAD 一同分发，不依赖操作系统装了哪款中文字体。
    static TextStyle cjkSafeStandard();

private:
    std::vector<TextStyle> m_styles;
};

} // namespace lycad
