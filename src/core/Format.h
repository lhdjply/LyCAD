// =============================================================================
//  LyCAD - core/Format.h
//  统一的数值格式化工具，保证界面、命令行、DXF 输出三处的数字呈现风格一致。
// =============================================================================
#pragma once

#include "core/Types.h"

#include <QString>

namespace lycad {

/// 世界长度值（当前单位制下）的显示格式，自动去除无意义的尾随零
QString formatLength(double value, int decimals = 4);

/// 点坐标显示，形如 "120.0000, 45.5000"
QString formatPoint(const Point2D& p, int decimals = 4);

/// 弧度转角度显示
QString formatAngle(double radians, int decimals = 2);

/// DXF 输出用：固定小数位，不加千分位
QString formatDxfDouble(double value, int decimals = 12);

/// 面积显示
QString formatArea(double value, int decimals = 4);

} // namespace lycad
