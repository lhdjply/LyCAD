#include "ui/PropertyChoices.h"

#include "core/AciPalette.h"
#include "core/Entity.h"

#include <QPainter>
#include <QPixmap>

namespace lycad {
namespace propertyChoices {

const QVector<int>& colors()
{
    static const QVector<int> kChoices = { -1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 250, 252, 254, 255 };
    return kChoices;
}

const QVector<int>& lineweights()
{
    static const QVector<int> kChoices = {
        -1, -3, 0, 5, 9, 13, 15, 18, 20, 25, 30, 35, 40, 50, 53, 60, 70, 80, 90, 100, 106, 120
    };
    return kChoices;
}

QString colorText(int aci)
{
    if (aci < 0) return QStringLiteral("随层");
    return aci::toName(aci);
}

QString lineweightText(int dxfValue)
{
    if (dxfValue == lineweight::kByLayer) return QStringLiteral("随层");
    return lineweight::toDisplayString(dxfValue);
}

QIcon colorSwatch(int aci)
{
    if (aci < 0) return {};          // 「随层」没有具体颜色

    constexpr int kSize = 14;
    QPixmap swatch(kSize, kSize);
    swatch.fill(Qt::transparent);

    QPainter painter(&swatch);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF body(1.0, 1.0, kSize - 2.0, kSize - 2.0);
    painter.setBrush(aci::toColor(aci));
    // 描边用半透明黑：白色与黑色色块在深浅两种底色上都能看出边界
    painter.setPen(QColor(0, 0, 0, 110));
    painter.drawRoundedRect(body, 2.0, 2.0);
    painter.end();

    return QIcon(swatch);
}

} // namespace propertyChoices
} // namespace lycad
