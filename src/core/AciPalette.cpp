// =============================================================================
//  LyCAD - core/AciPalette.cpp
// =============================================================================
#include "core/AciPalette.h"

#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace lycad {
namespace aci {

namespace {

struct Rgb { int r; int g; int b; };

/// 1-9 号标准色（AutoCAD 官方取值）
constexpr std::array<Rgb, 10> kStandard = {{
    { 0,   0,   0   },   // 0 ByBlock（占位）
    { 255, 0,   0   },   // 1 红
    { 255, 255, 0   },   // 2 黄
    { 0,   255, 0   },   // 3 绿
    { 0,   255, 255 },   // 4 青
    { 0,   0,   255 },   // 5 蓝
    { 255, 0,   255 },   // 6 洋红
    { 255, 255, 255 },   // 7 白/黑（随背景反相）
    { 128, 128, 128 },   // 8 深灰
    { 192, 192, 192 },   // 9 浅灰
}};

/// 250-255 灰阶
constexpr std::array<Rgb, 6> kGrays = {{
    { 51,  51,  51  },
    { 91,  91,  91  },
    { 132, 132, 132 },
    { 173, 173, 173 },
    { 214, 214, 214 },
    { 255, 255, 255 },
}};

/// 10-249 区间每一级明度对应的 value / saturation 系数
constexpr std::array<double, 10> kValueRamp = {{ 1.00, 1.00, 0.90, 0.80, 0.70, 0.60, 0.50, 0.40, 0.30, 0.20 }};
constexpr std::array<double, 10> kSatRamp   = {{ 1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 0.95, 0.90, 0.85, 0.80 }};

Rgb hsvToRgb(double h, double s, double v)
{
    const double c = v * s;
    const double hp = h / 60.0;
    const double x = c * (1.0 - std::abs(std::fmod(hp, 2.0) - 1.0));
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    const int seg = static_cast<int>(std::floor(hp)) % 6;
    switch (seg) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    const double m = v - c;
    return Rgb{ static_cast<int>(std::lround((r + m) * 255.0)),
                static_cast<int>(std::lround((g + m) * 255.0)),
                static_cast<int>(std::lround((b + m) * 255.0)) };
}

Rgb lookup(int index)
{
    if (index >= 1 && index <= 9) return kStandard[static_cast<std::size_t>(index)];
    if (index >= 250 && index <= 255) return kGrays[static_cast<std::size_t>(index - 250)];

    if (index >= 10 && index <= 249) {
        const int offset = index - 10;
        const int hueIdx = offset / 10;         // 0..23
        const int shade  = offset % 10;         // 0..9
        const double hue = hueIdx * 15.0;       // 24 色相，均分 360°
        return hsvToRgb(hue, kSatRamp[static_cast<std::size_t>(shade)],
                        kValueRamp[static_cast<std::size_t>(shade)]);
    }
    return kStandard[7];
}

} // namespace

bool isSpecial(int index)
{
    return index == kByBlock || index == kByLayer || index < 0;
}

bool isVisibleIndex(int index)
{
    return index >= 1 && index <= 255;
}

QColor toColor(int index, bool themeIsDark)
{
    if (index < 0) index = 7;                     // 未知 -> 用 7 号
    if (index == kByLayer || index == kByBlock) index = 7;

    // 7 号色随背景反相，保证可见性
    if (index == 7) return themeIsDark ? QColor(255, 255, 255) : QColor(0, 0, 0);

    const Rgb rgb = lookup(index);
    return QColor(rgb.r, rgb.g, rgb.b);
}

QString toName(int index)
{
    switch (index) {
    case kByBlock: return QStringLiteral("随块 (ByBlock)");
    case kByLayer: return QStringLiteral("随层 (ByLayer)");
    case 1:  return QStringLiteral("红 (1)");
    case 2:  return QStringLiteral("黄 (2)");
    case 3:  return QStringLiteral("绿 (3)");
    case 4:  return QStringLiteral("青 (4)");
    case 5:  return QStringLiteral("蓝 (5)");
    case 6:  return QStringLiteral("洋红 (6)");
    case 7:  return QStringLiteral("黑/白 (7)");
    case 8:  return QStringLiteral("深灰 (8)");
    case 9:  return QStringLiteral("浅灰 (9)");
    case 250: case 251: case 252: case 253: case 254: case 255:
        return QStringLiteral("灰阶 (%1)").arg(index);
    default:
        if (index < 0) return QStringLiteral("随层 (ByLayer)");
        return QStringLiteral("索引 %1").arg(index);
    }
}

int fromColor(const QColor& color)
{
    if (!color.isValid()) return 7;

    // 先比对精确值
    int best = 7;
    double bestDist = std::numeric_limits<double>::max();

    auto consider = [&](int idx, const Rgb& c) {
        const double dr = c.r - color.red();
        const double dg = c.g - color.green();
        const double db = c.b - color.blue();
        // 感知加权距离
        const double dist = 0.30 * dr * dr + 0.59 * dg * dg + 0.11 * db * db;
        if (dist < bestDist) { bestDist = dist; best = idx; }
    };

    for (int i = 1; i <= 9; ++i) consider(i, kStandard[static_cast<std::size_t>(i)]);
    for (int i = 250; i <= 255; ++i) consider(i, kGrays[static_cast<std::size_t>(i - 250)]);
    for (int i = 10; i <= 249; ++i) consider(i, lookup(i));

    return best;
}

int nextIndexForNewLayer(int seedIndex)
{
    static const std::array<int, 9> kCycle = {{ 1, 3, 5, 2, 4, 6, 30, 90, 150 }};
    const auto it = std::find(kCycle.begin(), kCycle.end(), seedIndex);
    if (it == kCycle.end()) return kCycle.front();
    const std::size_t next = (static_cast<std::size_t>(it - kCycle.begin()) + 1) % kCycle.size();
    return kCycle[next];
}

} // namespace aci
} // namespace lycad
