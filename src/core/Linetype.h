// =============================================================================
//  LyCAD - core/Linetype.h
//  线型定义。
//
//  线型既有显示语义（渲染层按虚线模式绘制），又有文件语义（DXF 的 LTYPE 表
//  必须以 49 组码逐段描述虚线模式）。因此把定义放在 core，由 render 与 io
//  两个模块共享，避免两处各写一份而出现不一致。
//
//  模式数组约定（单位：图形单位）
//    正值 = 实线段长度
//    负值 = 间隔长度
//    0    = 一个点
// =============================================================================
#pragma once

#include <QString>
#include <QStringList>

#include <vector>

namespace lycad {

struct LinetypePattern {
    QString name;                          ///< DXF 中的线型名，如 "DASHED"
    QString description;                   ///< 供 LTYPE 表 3 组码
    std::vector<double> pattern;           ///< 虚线模式；空表示实线

    bool isContinuous() const { return pattern.empty(); }

    /// 一个完整周期的长度，作为 LTYPE 表的 40 组码
    double cycleLength() const;
};

namespace linetypes {

/// 内置标准线型集合（CONTINUOUS 恒为首项）
const std::vector<LinetypePattern>& standard();

/// 按名称查找，大小写不敏感；不存在返回 nullptr
const LinetypePattern* find(const QString& name);

/// 按名称查找，不存在时回退到 CONTINUOUS
LinetypePattern resolve(const QString& name);

/// 全部线型名
QStringList names();

/// 判断名称是否有效
bool exists(const QString& name);

/**
 * 归一化线型名。
 *
 * 内置线型返回其规范拼写（全大写，如 "DASHED"）；未知名称去除首尾空白后原样返回。
 * 该函数是「线型名大小写不敏感」这一约定的唯一落地点：只要所有写入路径都经过它，
 * 同名线型在内存与文件之间就不会因大小写差异而漂移。
 */
QString canonicalName(const QString& name);

} // namespace linetypes

} // namespace lycad
