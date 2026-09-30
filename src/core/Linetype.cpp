// =============================================================================
//  LyCAD - core/Linetype.cpp
// =============================================================================
#include "core/Linetype.h"

#include <algorithm>
#include <cmath>

namespace lycad {

double LinetypePattern::cycleLength() const
{
    double total = 0.0;
    for (double v : pattern) total += std::abs(v);
    return total;
}

namespace linetypes {

namespace {

std::vector<LinetypePattern> buildStandard()
{
    std::vector<LinetypePattern> out;

    // 实线：模式为空
    out.push_back(LinetypePattern{ QStringLiteral("CONTINUOUS"),
                                   QStringLiteral("Solid line"),
                                   {} });

    // 以下模式取自 AutoCAD acad.lin 的常用定义（按图形单位缩放）
    out.push_back(LinetypePattern{ QStringLiteral("DASHED"),
                                   QStringLiteral("__ __ __ __ __ __ __ __ __ __ __ __ __"),
                                   { 0.5, -0.25 } });

    out.push_back(LinetypePattern{ QStringLiteral("DASHDOT"),
                                   QStringLiteral("__ . __ . __ . __ . __ . __ . __"),
                                   { 0.5, -0.25, 0.0, -0.25 } });

    out.push_back(LinetypePattern{ QStringLiteral("DASHDOTDOT"),
                                   QStringLiteral("__ . . __ . . __ . . __ . . __"),
                                   { 0.5, -0.25, 0.0, -0.25, 0.0, -0.25 } });

    out.push_back(LinetypePattern{ QStringLiteral("DOT"),
                                   QStringLiteral(". . . . . . . . . . . . . . . . ."),
                                   { 0.0, -0.25 } });

    out.push_back(LinetypePattern{ QStringLiteral("HIDDEN"),
                                   QStringLiteral("__ __ __ __ __ __ __ __ __"),
                                   { 0.25, -0.125 } });

    out.push_back(LinetypePattern{ QStringLiteral("CENTER"),
                                   QStringLiteral("____ _ ____ _ ____ _ ____ _ ____"),
                                   { 1.25, -0.25, 0.25, -0.25 } });

    out.push_back(LinetypePattern{ QStringLiteral("PHANTOM"),
                                   QStringLiteral("______ _ _ ______ _ _ ______ _"),
                                   { 1.25, -0.25, 0.25, -0.25, 0.25, -0.25 } });

    out.push_back(LinetypePattern{ QStringLiteral("BORDER"),
                                   QStringLiteral("__ __ . __ __ . __ __ . __ __ . __"),
                                   { 0.5, -0.25, 0.5, -0.25, 0.0, -0.25 } });

    return out;
}

} // namespace

const std::vector<LinetypePattern>& standard()
{
    static const std::vector<LinetypePattern> kPatterns = buildStandard();
    return kPatterns;
}

const LinetypePattern* find(const QString& name)
{
    for (const LinetypePattern& p : standard()) {
        if (p.name.compare(name, Qt::CaseInsensitive) == 0) return &p;
    }
    return nullptr;
}

LinetypePattern resolve(const QString& name)
{
    if (const LinetypePattern* p = find(name)) return *p;
    return standard().front();
}

QStringList names()
{
    QStringList out;
    for (const LinetypePattern& p : standard()) out << p.name;
    return out;
}

bool exists(const QString& name)
{
    return find(name) != nullptr;
}

QString canonicalName(const QString& name)
{
    if (const LinetypePattern* pattern = find(name)) return pattern->name;
    return name.trimmed();
}

} // namespace linetypes

} // namespace lycad
