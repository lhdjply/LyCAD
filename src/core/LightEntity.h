// =============================================================================
//  LyCAD - core/LightEntity.h
//  光源图元（DXF / DWG: LIGHT）
//
//  为什么需要单独建模
//
//    LIGHT 是 R2007 起的「光度控制光源」。它**没有几何**——在 AutoCAD / 浩辰 /
//    中望里都只是屏幕上的一个小标记（圆 + 光芒，聚光灯再加一个方向锥），属性才是
//    它的全部内容：名称、类型（平行光 / 点光源 / 聚光灯 / 光域网）、开关、光色、
//    强度、位置与目标、衰减、聚光角与衰退角、阴影。
//
//    之前的实现里这一类实体在**库层就被丢弃**（libdxfrw 只把 LIGHT 列在注释里），
//    于是导入 example.dxf 这样的图纸时整条光源不翼而飞、保存时又静默丢失，
//    用户只能在别的 CAD 里看到它（那边显示成一个「明」字样的标记）。所以这里
//    建原生图元，让它在图里看得见、选得中、存得回。
//
//  坐标与标记尺寸
//
//    位置（DXF 10）与目标（DXF 11）都有 Z 分量，但本软件是二维绘图的，
//    Z 不参与显示与排版，只按原值往返。屏幕上的标记是**固定像素尺寸**的
//    （见 kGlyphRadiusPixels）——与 AutoCAD 一致：放大图纸不会让光标记跟着变大。
// =============================================================================
#pragma once

#include "core/Entity.h"

namespace lycad {

class LightEntity final : public Entity {
public:
    /// 光源类型（DXF 70）
    enum class LightType {
        Distant = 1,   ///< 平行光
        Point = 2,     ///< 点光源
        Spot = 3,      ///< 聚光灯
        Web = 4        ///< 光域网
    };

    /// 衰减方式（DXF 72）
    enum class Attenuation {
        None = 0,           ///< 不衰减
        InverseLinear = 1,  ///< 线性
        InverseSquare = 2   ///< 平方反比
    };

    static constexpr EntityType kType = EntityType::Light;

    /// 屏幕标记的半径（像素）。固定像素尺寸，不随缩放变化（与 AutoCAD 同）。
    static constexpr double kGlyphRadiusPixels = 7.0;

    LightEntity();
    LightEntity(const Point2D& position, LightType type = LightType::Point);

    // ---- 名称与类型 -----------------------------------------------------------
    const QString& name() const noexcept { return m_name; }
    void setName(const QString& name) { m_name = name; }

    LightType lightType() const noexcept { return m_type; }
    void setLightType(LightType type) noexcept { m_type = type; }
    /// 类型的中文名（属性面板、命令行与 describe 共用）
    static QString lightTypeDisplayName(LightType type);

    // ---- 开关与光色 -----------------------------------------------------------
    bool isOn() const noexcept { return m_status; }
    void setOn(bool on) noexcept { m_status = on; }

    /// 光源颜色（DXF 63，ACI）——与图元自身颜色（随层/随块）是两回事：
    /// 前者是**投出来的光**的颜色，后者是这个标记符号的颜色。
    int lightColorIndex() const noexcept { return m_lightColor; }
    void setLightColorIndex(int aci) noexcept { m_lightColor = aci; }

    /// 是否打印标记（DXF 291）
    bool plotGlyph() const noexcept { return m_plotGlyph; }
    void setPlotGlyph(bool on) noexcept { m_plotGlyph = on; }

    // ---- 强度与位置 -----------------------------------------------------------
    double intensity() const noexcept { return m_intensity; }
    void setIntensity(double value) noexcept { m_intensity = value; }

    const Point2D& position() const noexcept { return m_position; }
    void setPosition(const Point2D& p) noexcept { m_position = p; }
    /// 位置的 Z（DXF 30）；二维显示不使用，按原值往返
    double positionZ() const noexcept { return m_positionZ; }
    void setPositionZ(double z) noexcept { m_positionZ = z; }

    const Point2D& target() const noexcept { return m_target; }
    void setTarget(const Point2D& p) noexcept { m_target = p; }
    /// 目标的 Z（DXF 31）
    double targetZ() const noexcept { return m_targetZ; }
    void setTargetZ(double z) noexcept { m_targetZ = z; }

    // ---- 衰减 -----------------------------------------------------------------
    Attenuation attenuation() const noexcept { return m_attenuation; }
    void setAttenuation(Attenuation a) noexcept { m_attenuation = a; }
    bool useAttenuationLimits() const noexcept { return m_useAttenuationLimits; }
    void setUseAttenuationLimits(bool on) noexcept { m_useAttenuationLimits = on; }
    double attenuationStartLimit() const noexcept { return m_attenuationStart; }
    double attenuationEndLimit() const noexcept { return m_attenuationEnd; }
    void setAttenuationLimits(double start, double end) noexcept;

    // ---- 聚光角与阴影 ---------------------------------------------------------
    double hotspotAngle() const noexcept { return m_hotspotAngle; }
    double falloffAngle() const noexcept { return m_falloffAngle; }
    void setConeAngles(double hotspot, double falloff) noexcept;

    bool castShadows() const noexcept { return m_castShadows; }
    void setCastShadows(bool on) noexcept { m_castShadows = on; }
    int shadowType() const noexcept { return m_shadowType; }
    void setShadowType(int type) noexcept { m_shadowType = type; }
    int shadowMapSize() const noexcept { return m_shadowMapSize; }
    void setShadowMapSize(int size) noexcept { m_shadowMapSize = size; }
    int shadowSoftness() const noexcept { return m_shadowSoftness; }
    void setShadowSoftness(int value) noexcept { m_shadowSoftness = value; }

    /// 类版本号（DXF 90）：AutoCAD 写入的类数据版本标识，规范范围 0~10。
    ///
    /// 导入时**原值保存**（文件里是什么就存什么）；写出时越界值会被归一为 0 ——
    /// 实测有转换器在本该很小的字段里写出 -1023410171 这种值，而严格解析端
    ///（libredwg 对该字段有越界检查）会因此**中止整条对象的解析**，照抄回去
    /// 等于把坏数据传下去。
    int classVersion() const noexcept { return m_classVersion; }
    void setClassVersion(int version) noexcept { m_classVersion = version; }

    // ---- Entity 接口 ----------------------------------------------------------
    std::unique_ptr<Entity> clone() const override;
    Box2D boundingBox() const override;
    double distanceTo(const Point2D& p) const override;
    Point2D closestPointTo(const Point2D& p) const override;
    bool crosses(const Box2D& box) const override;
    void transform(const Transform2D& t) override;
    double length() const override;
    std::vector<Point2D> snapPoints() const override;
    std::vector<Point2D> gripPoints() const override;
    bool moveGrip(std::size_t index, const Point2D& newPos) override;
    std::vector<Point2D> flatten(double sagitta) const override;
    bool isClosed() const override { return false; }
    QString describe() const override;
    QString geometrySummary() const override;

private:
    QString   m_name;
    LightType m_type = LightType::Point;
    bool      m_status = true;
    int       m_lightColor = 7;          ///< ACI，7 = 白
    bool      m_plotGlyph = false;
    double    m_intensity = 1.0;
    Point2D   m_position;
    double    m_positionZ = 0.0;
    Point2D   m_target { 0.0, 0.0 };
    double    m_targetZ = 0.0;
    Attenuation m_attenuation = Attenuation::None;
    bool      m_useAttenuationLimits = false;
    double    m_attenuationStart = 1.0;
    double    m_attenuationEnd = 10.0;
    double    m_hotspotAngle = 45.0;
    double    m_falloffAngle = 50.0;
    bool      m_castShadows = false;
    int       m_shadowType = 0;
    int       m_shadowMapSize = 0;
    int       m_shadowSoftness = 0;
    int       m_classVersion = 0;
};

} // namespace lycad
