// =============================================================================
//  LyCAD - render/CanvasTypes.cpp
// =============================================================================
#include "render/CanvasTypes.h"

namespace lycad {

QString snapKindName(SnapKind kind)
{
    switch (kind) {
    case SnapKind::None:         return QStringLiteral("无");
    case SnapKind::Endpoint:     return QStringLiteral("端点");
    case SnapKind::Midpoint:     return QStringLiteral("中点");
    case SnapKind::Center:       return QStringLiteral("圆心");
    case SnapKind::Quadrant:     return QStringLiteral("象限点");
    case SnapKind::Intersection: return QStringLiteral("交点");
    case SnapKind::Perpendicular:return QStringLiteral("垂足");
    case SnapKind::Tangent:      return QStringLiteral("切点");
    case SnapKind::Nearest:      return QStringLiteral("最近点");
    case SnapKind::Grid:         return QStringLiteral("栅格点");
    case SnapKind::Ortho:        return QStringLiteral("正交投影");
    case SnapKind::Polar:        return QStringLiteral("极轴追踪");
    case SnapKind::NodePoint:    return QStringLiteral("节点");
    }
    return QStringLiteral("无");
}

QString snapKindShortLabel(SnapKind kind)
{
    switch (kind) {
    case SnapKind::Endpoint:     return QStringLiteral("端");
    case SnapKind::Midpoint:     return QStringLiteral("中");
    case SnapKind::Center:       return QStringLiteral("心");
    case SnapKind::Quadrant:     return QStringLiteral("象");
    case SnapKind::Intersection: return QStringLiteral("交");
    case SnapKind::Perpendicular:return QStringLiteral("垂");
    case SnapKind::Tangent:      return QStringLiteral("切");
    case SnapKind::Nearest:      return QStringLiteral("近");
    case SnapKind::Grid:         return QStringLiteral("栅");
    case SnapKind::Ortho:        return QStringLiteral("正");
    case SnapKind::Polar:        return QStringLiteral("极");
    case SnapKind::NodePoint:    return QStringLiteral("点");
    case SnapKind::None:         break;
    }
    return QString();
}

QColor snapKindColor(SnapKind kind)
{
    switch (kind) {
    case SnapKind::Endpoint:     return QColor(0x2E, 0xA0, 0x43);   // 绿
    case SnapKind::Midpoint:     return QColor(0x1E, 0x88, 0xE5);   // 蓝
    case SnapKind::Center:       return QColor(0xE5, 0x7A, 0x1E);   // 橙
    case SnapKind::Quadrant:     return QColor(0x8E, 0x44, 0xAD);   // 紫
    case SnapKind::Intersection: return QColor(0xD3, 0x2F, 0x2F);   // 红
    case SnapKind::Perpendicular:return QColor(0x00, 0x89, 0x8B);   // 青
    case SnapKind::Tangent:      return QColor(0xC2, 0x18, 0x5B);   // 品红
    case SnapKind::Nearest:      return QColor(0x6D, 0x4C, 0x41);   // 棕
    case SnapKind::Grid:         return QColor(0x75, 0x75, 0x75);   // 灰
    case SnapKind::Ortho:        return QColor(0x2E, 0x7D, 0x32);
    case SnapKind::Polar:        return QColor(0x5E, 0x35, 0xB1);
    case SnapKind::NodePoint:    return QColor(0x2E, 0xA0, 0x43);
    case SnapKind::None:         break;
    }
    return QColor(0x75, 0x75, 0x75);
}

} // namespace lycad
