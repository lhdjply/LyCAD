// =============================================================================
//  LyCAD - core/XLineEntity.h
//  构造线图元（DXF: XLINE）——过基点向两个方向无限延伸。
// =============================================================================
#pragma once

#include "core/InfiniteLineEntity.h"

namespace lycad {

class XLineEntity final : public InfiniteLineEntity {
public:
    XLineEntity();
    XLineEntity(const Point2D& base, const Vector2D& direction);

    /// 以「通过两点」的方式构造（AutoCAD 的默认输入方式）
    static XLineEntity throughPoints(const Point2D& first, const Point2D& second);

    std::unique_ptr<Entity> clone() const override;

    bool extendsBackward() const noexcept override { return true; }

    /// 图元类型判别捷径，避免调用方使用 dynamic_cast
    static constexpr EntityType kType = EntityType::XLine;

protected:
    QString baseLabel() const override { return QStringLiteral("基点"); }
};

} // namespace lycad
