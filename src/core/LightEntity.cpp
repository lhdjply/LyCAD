// =============================================================================
//  LyCAD - core/LightEntity.cpp
// =============================================================================
#include "core/LightEntity.h"

#include "core/Format.h"

#include <algorithm>
#include <cmath>

namespace lycad {

LightEntity::LightEntity()
    : Entity(EntityType::Light)
{
}

LightEntity::LightEntity(const Point2D& position, LightType type)
    : Entity(EntityType::Light)
    , m_type(type)
    , m_position(position)
    , m_target(position.x + 1.0, position.y + 1.0)   // 目标缺省在右上方，与 AutoCAD 一致
{
}

QString LightEntity::lightTypeDisplayName(LightType type)
{
    switch (type) {
    case LightType::Distant: return QStringLiteral("平行光");
    case LightType::Point:   return QStringLiteral("点光源");
    case LightType::Spot:    return QStringLiteral("聚光灯");
    case LightType::Web:     return QStringLiteral("光域网");
    }
    return QStringLiteral("光源");
}

void LightEntity::setAttenuationLimits(double start, double end) noexcept
{
    // 原值保存：文件里的区间可能是倒置的（AutoCAD 允许），如实保留才能往返一致；
    // 取值合法性由编辑入口负责，导入路径不改动文件里的数字。
    m_attenuationStart = start;
    m_attenuationEnd = end;
}

void LightEntity::setConeAngles(double hotspot, double falloff) noexcept
{
    m_hotspotAngle = hotspot;
    m_falloffAngle = falloff;
}

// -----------------------------------------------------------------------------
//  Entity 接口
// -----------------------------------------------------------------------------
std::unique_ptr<Entity> LightEntity::clone() const
{
    auto copy = std::make_unique<LightEntity>();
    copy->m_name = m_name;
    copy->m_type = m_type;
    copy->m_status = m_status;
    copy->m_lightColor = m_lightColor;
    copy->m_plotGlyph = m_plotGlyph;
    copy->m_intensity = m_intensity;
    copy->m_position = m_position;
    copy->m_positionZ = m_positionZ;
    copy->m_target = m_target;
    copy->m_targetZ = m_targetZ;
    copy->m_attenuation = m_attenuation;
    copy->m_useAttenuationLimits = m_useAttenuationLimits;
    copy->m_attenuationStart = m_attenuationStart;
    copy->m_attenuationEnd = m_attenuationEnd;
    copy->m_hotspotAngle = m_hotspotAngle;
    copy->m_falloffAngle = m_falloffAngle;
    copy->m_castShadows = m_castShadows;
    copy->m_shadowType = m_shadowType;
    copy->m_shadowMapSize = m_shadowMapSize;
    copy->m_shadowSoftness = m_shadowSoftness;
    copy->m_classVersion = m_classVersion;
    copyCommonPropertiesTo(*copy);
    return copy;
}

Box2D LightEntity::boundingBox() const
{
    // 屏幕标记不参与世界范围的度量（放大图纸不会让标记变大），因此包围盒退化成一个点：
    // 与 POINT 同口径，既不污染「缩放到范围」，也不会被误当成一块有面积的几何。
    Box2D box;
    box.expand(m_position);
    return box;
}

double LightEntity::distanceTo(const Point2D& p) const
{
    return p.distanceTo(m_position);
}

Point2D LightEntity::closestPointTo(const Point2D& p) const
{
    Q_UNUSED(p);
    return m_position;
}

bool LightEntity::crosses(const Box2D& box) const
{
    return box.contains(m_position, kGeomTol);
}

void LightEntity::transform(const Transform2D& t)
{
    // 目标点按「从位置出发的方向向量」一起变：平移照搬、旋转跟着转、缩放伸长，
    // 这样旋转图纸后聚光灯仍然照着同一个方向。
    const Vector2D aim(m_target.x - m_position.x, m_target.y - m_position.y);
    const Vector2D movedAim = t.applyVector(aim);

    m_position = t.apply(m_position);
    m_target = Point2D(m_position.x + movedAim.x, m_position.y + movedAim.y);

    // Z 只在等比变换下有意义（非等比变换无法保持方向语义，按长度缩放估算）
    const double zScale = t.isSimilarity() ? t.uniformScale()
                                           : t.lengthScaleAlong(Vector2D(0.0, 1.0));
    m_positionZ *= zScale;
    m_targetZ *= zScale;
}

double LightEntity::length() const
{
    return 0.0;
}

std::vector<Point2D> LightEntity::snapPoints() const
{
    return { m_position };
}

std::vector<Point2D> LightEntity::gripPoints() const
{
    // 0 = 位置；聚光灯/光域网再给一个目标夹点（拖动它改照射方向与距离）
    if (m_type == LightType::Spot || m_type == LightType::Web) {
        return { m_position, m_target };
    }
    return { m_position };
}

bool LightEntity::moveGrip(std::size_t index, const Point2D& newPos)
{
    if (index == 0) {
        // 位置与目标一起平移（保持照射方向不变），与 AutoCAD 的拖动行为一致
        const Vector2D delta(newPos.x - m_position.x, newPos.y - m_position.y);
        m_position = newPos;
        m_target = Point2D(m_target.x + delta.x, m_target.y + delta.y);
        return true;
    }
    if (index == 1 && (m_type == LightType::Spot || m_type == LightType::Web)) {
        m_target = newPos;
        return true;
    }
    return false;
}

std::vector<Point2D> LightEntity::flatten(double sagitta) const
{
    Q_UNUSED(sagitta);
    return { m_position };
}

QString LightEntity::describe() const
{
    const QString typeName = lightTypeDisplayName(m_type);
    const QString shown = m_name.isEmpty() ? typeName
                                           : QStringLiteral("%1「%2」").arg(typeName, m_name);
    return QStringLiteral("%1  %2%3")
        .arg(shown, formatPoint(m_position),
             m_status ? QString() : QStringLiteral("  （已关闭）"));
}

QString LightEntity::geometrySummary() const
{
    const QString attenuationName = [this]() {
        switch (m_attenuation) {
        case Attenuation::InverseLinear: return QStringLiteral("线性");
        case Attenuation::InverseSquare: return QStringLiteral("平方反比");
        case Attenuation::None:          break;
        }
        return QStringLiteral("不衰减");
    }();

    return QStringLiteral("名称: %1\n类型: %2\n状态: %3\n强度: %4\n"
                          "位置: %5\n目标: %6\n"
                          "衰减: %7%8\n聚光角: %9 / 衰退角: %10\n"
                          "光色: %11\n阴影: %12")
        .arg(m_name.isEmpty() ? QStringLiteral("（未命名）") : m_name,
             lightTypeDisplayName(m_type),
             m_status ? QStringLiteral("开") : QStringLiteral("关"))
        .arg(formatLength(m_intensity),
             formatPoint(m_position),
             formatPoint(m_target),
             attenuationName,
             m_useAttenuationLimits
                 ? QStringLiteral("（%1 ~ %2）").arg(formatLength(m_attenuationStart),
                                                     formatLength(m_attenuationEnd))
                 : QString())
        .arg(formatAngle(degToRad(m_hotspotAngle)), formatAngle(degToRad(m_falloffAngle)))
        .arg(aci::toName(m_lightColor),
             m_castShadows ? QStringLiteral("投射") : QStringLiteral("不投射"));
}

} // namespace lycad
