// =============================================================================
//  LyCAD - ui/PropertyChoices.h
//  属性下拉框的取值表与显示文案。
//
//  功能区的「特性」面板与右侧属性面板编辑的是同一批属性，两处必须给出
//  完全一致的选择项与文案——否则同一个线宽在一边显示「0.25 mm」、在
//  另一边显示「默认」，用户会以为它们不是一回事。这些表集中放在这里，
//  全工程只保留一份定义。
// =============================================================================
#pragma once

#include <QIcon>
#include <QString>
#include <QVector>

namespace lycad {
namespace propertyChoices {

/// 颜色可选值：随层（-1）+ 标准九色 + 常用灰阶
const QVector<int>& colors();

/// 线宽可选值（DXF 370 分组值）
const QVector<int>& lineweights();

/// 颜色索引 -> 下拉框显示文本
QString colorText(int aci);

/// 线宽值 -> 下拉框显示文本
QString lineweightText(int dxfValue);

/// 颜色色块图标。带一圈描边，好让白色与黑色在深浅两种底色上都看得见。
QIcon colorSwatch(int aci);

} // namespace propertyChoices
} // namespace lycad
