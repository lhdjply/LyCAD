// =============================================================================
//  LyCAD - core/AciPalette.h
//  AutoCAD 颜色索引（ACI）调色板。
//
//  DXF 中图元颜色以 0-256 的索引存储：
//    0   = ByBlock
//    1-9 = 固定标准色
//    256 = ByLayer
//    250-255 = 灰阶
//    其余为 24 个色相 × 10 级明度的系统性配色
//
//  本模块对 1-9、250-255 给出精确值；10-249 使用标准配色结构做近似合成，
//  仅供屏幕显示。文件的往返读写始终保留原始索引，不依赖该近似值。
// =============================================================================
#pragma once

#include <QColor>
#include <QString>

namespace lycad {

namespace aci {

inline constexpr int kByBlock = 0;
inline constexpr int kByLayer = 256;

/// 索引是否为特殊值（ByBlock / ByLayer）
bool isSpecial(int index);

/// 索引是否落在可显示的有效色范围内
bool isVisibleIndex(int index);

/// 索引 -> 显示颜色。themeIsDark 用于决定 7 号色显示为白还是黑。
QColor toColor(int index, bool themeIsDark = false);

/// 索引 -> 中文/英文可读名称，用于属性面板与图层对话框
QString toName(int index);

/// 从 RGB 反查最接近的 ACI 索引（导入外部数据时使用）
int fromColor(const QColor& color);

/// 按索引循环取色，用于新建图层时自动分配颜色（跳过 7 号与灰阶）
int nextIndexForNewLayer(int seedIndex);

} // namespace aci

} // namespace lycad
