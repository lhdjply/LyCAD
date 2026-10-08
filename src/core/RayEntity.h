// =============================================================================
//  LyCAD - core/RayEntity.h
//  射线图元（DXF: RAY）——自起点沿方向单向无限延伸。
// =============================================================================
#pragma once

#include "core/InfiniteLineEntity.h"

namespace lycad {

class RayEntity final : public InfiniteLineEntity {
public:
    RayEntity();
    RayEntity(const Point2D& start, const Vector2D& direction);

    /// 以「起点 + 通过点」的方式构造（AutoCAD 的默认输入方式）
    static RayEntity throughPoints(const Point2D& start, const Point2D& second);

    std::unique_ptr<Entity> clone() const override;

    bool extendsBackward() const noexcept override { return false; }

    /// 图元类型判别捷径，避免调用方使用 dynamic_cast
    static constexpr EntityType kType = EntityType::Ray;

protected:
    QString baseLabel() const override { return QStringLiteral("起点"); }
};

} // namespace lycad
