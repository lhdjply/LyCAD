// =============================================================================
//  LyCAD - interact/CoordinateInput.h
//  命令行坐标解析。
//
//  支持 AutoCAD 的四类输入形式：
//      100,50        绝对直角坐标
//      @50,0         相对上一点的直角坐标
//      50<45         相对上一点的极坐标（长度 < 角度°）
//      120           裸数字（具体含义由工具决定：半径、角度、比例……）
//
//  另接受 4 则运算形式的简单表达式（如 "25*2"），便于按图纸标注直接输入。
// =============================================================================
#pragma once

#include "core/Types.h"

#include <QString>

#include <optional>

namespace lycad {

struct ParsedCoordinate {
    enum class Kind {
        Invalid,
        Absolute,      ///< 绝对直角坐标
        Relative,      ///< 相对直角坐标
        Polar,         ///< 极坐标
        Scalar         ///< 裸标量
    };

    Kind    kind = Kind::Invalid;
    Point2D point;              ///< Absolute 为绝对坐标；Relative/Polar 为相对增量
    double  scalar = 0.0;       ///< Scalar 形式的数值
    double  distance = 0.0;     ///< Polar 的长度
    double  angleDegrees = 0.0; ///< Polar 的角度
    QString error;

    bool isValid() const { return kind != Kind::Invalid; }
    bool isPointLike() const
    {
        return kind == Kind::Absolute || kind == Kind::Relative || kind == Kind::Polar;
    }

    /**
     * 解析为世界坐标。
     * @param lastPoint 相对坐标与极坐标的基准点；此类输入缺少基准点时解析失败。
     */
    std::optional<Point2D> resolve(const std::optional<Point2D>& lastPoint) const;
};

/// 解析输入文本
ParsedCoordinate parseCoordinate(const QString& text);

/// 计算简单的四则运算表达式（仅支持 + - * / 与括号），失败返回 nullopt
std::optional<double> evaluateExpression(const QString& text);

/// 把世界坐标格式化为 "x,y" 文本，用于回显与状态栏
QString formatCoordinate(const Point2D& point, int decimals = 4);

} // namespace lycad
