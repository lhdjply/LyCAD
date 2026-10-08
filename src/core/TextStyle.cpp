// =============================================================================
//  LyCAD - core/TextStyle.cpp
//  文字样式表的实现。
// =============================================================================
#include "core/TextStyle.h"

namespace lycad {
namespace {

/// 样式名比较与其它符号表一致：DXF 的符号表名大小写不敏感。
/// 别的 CAD 写出的文件里大小写并不统一（旧版本转 R12 时还会整体转大写），
/// 按原样比对会让同一条样式变成两条。
bool sameName(const QString& a, const QString& b)
{
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

} // namespace

TextStyle TextStyleTable::cjkSafeStandard()
{
    TextStyle style(QStringLiteral("Standard"));
    style.setFontFile(QStringLiteral("txt"));
    style.setBigFontFile(QStringLiteral("gbcbig.shx"));
    return style;
}

// -----------------------------------------------------------------------------
TextStyleTable::TextStyleTable()
{
    clear();
}

void TextStyleTable::clear()
{
    m_styles.clear();
    m_styles.push_back(cjkSafeStandard());
}

const TextStyle* TextStyleTable::find(const QString& name) const
{
    for (const TextStyle& style : m_styles) {
        if (sameName(style.name(), name)) return &style;
    }
    return nullptr;
}

TextStyle* TextStyleTable::find(const QString& name)
{
    for (TextStyle& style : m_styles) {
        if (sameName(style.name(), name)) return &style;
    }
    return nullptr;
}

TextStyle& TextStyleTable::ensure(const QString& name)
{
    if (TextStyle* existing = find(name)) return *existing;
    m_styles.emplace_back(name);
    return m_styles.back();
}

} // namespace lycad
