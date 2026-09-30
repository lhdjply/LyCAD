// =============================================================================
//  LyCAD - core/Format.cpp
// =============================================================================
#include "core/Format.h"

#include <QStringList>

#include <cmath>

namespace lycad {

namespace {

double roundTo(double value, int decimals)
{
    const double factor = std::pow(10.0, decimals);
    return std::round(value * factor) / factor;
}

} // namespace

QString formatLength(double value, int decimals)
{
    if (!std::isfinite(value)) return QStringLiteral("--");

    const double rounded = roundTo(value, decimals);
    QString s = QString::number(rounded, 'f', decimals);

    // 去除尾随零，但至少保留 1 位小数，避免 "12." 这种形态
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0'))) s.chop(1);
        if (s.endsWith(QLatin1Char('.'))) s.chop(1);
    }
    if (s.isEmpty() || s == QLatin1String("-0")) s = QStringLiteral("0");
    return s;
}

QString formatPoint(const Point2D& p, int decimals)
{
    return QStringLiteral("%1, %2").arg(formatLength(p.x, decimals), formatLength(p.y, decimals));
}

QString formatAngle(double radians, int decimals)
{
    return QStringLiteral("%1°").arg(QString::number(radToDeg(normalizeAngle(radians)), 'f', decimals));
}

QString formatDxfDouble(double value, int decimals)
{
    if (!std::isfinite(value)) return QStringLiteral("0.0");
    QString s = QString::number(value, 'f', decimals);
    // DXF 规范不要求尾随零，去掉可显著减小文件体积
    if (s.contains(QLatin1Char('.'))) {
        while (s.endsWith(QLatin1Char('0'))) s.chop(1);
        if (s.endsWith(QLatin1Char('.'))) s += QLatin1Char('0');
    }
    return s;
}

QString formatArea(double value, int decimals)
{
    return formatLength(value, decimals);
}

} // namespace lycad
