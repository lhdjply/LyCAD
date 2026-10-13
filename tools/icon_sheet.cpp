// =============================================================================
//  LyCAD - tools/icon_sheet.cpp
//  图标总览图生成器。
//
//  把 resources/icons 下的全部图标在「浅色 / 深色」两种配色、三个尺寸下
//  渲染到一张 PNG 上，末尾再补一段「应用图标」（不随主题换色，两种底色各一排）。
//  用途有两个：
//
//    · 自检——漏画的路径、写错的色值、在某个尺寸下糊成一团的细节，
//      扫一眼总览图就能发现，比逐个打开 SVG 文件可靠得多；应用图标那一段
//      同时验证 qrc 的 /app 前缀是否编进程序（前缀错时那一段会是空白）；
//    · 文档——README 的「功能」一节或 docs/ 下可以直接引用这张图说明图标集。
//
//  用法： lycad_icon_sheet <输出文件.png>
// =============================================================================
#include "ui/IconLibrary.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QTextStream>

#include <algorithm>
#include <cmath>

namespace {

QTextStream& out()
{
    static QTextStream stream(stdout);
    return stream;
}

constexpr int kCellWidth = 208;
constexpr int kCellHeight = 64;
constexpr int kColumns = 6;
constexpr int kHeaderHeight = 34;
constexpr int kSectionGap = 18;

/// 一个主题的图块：标题 + 若干行单元格
int sectionHeight(int iconCount)
{
    const int rows = static_cast<int>(std::ceil(double(iconCount) / kColumns));
    return kHeaderHeight + rows * kCellHeight;
}

void paintSection(QPainter& painter, const QStringList& names, const QString& title,
                  bool dark, int top, int width)
{
    const QColor background = dark ? QColor(0x2b, 0x30, 0x38) : QColor(0xf4, 0xf5, 0xf7);
    const QColor foreground = dark ? QColor(0xd5, 0xdb, 0xe3) : QColor(0x26, 0x2b, 0x33);
    const QColor grid = dark ? QColor(0x3c, 0x43, 0x4e) : QColor(0xd3, 0xd7, 0xdd);

    painter.fillRect(0, top, width, sectionHeight(names.size()), background);

    QFont titleFont = painter.font();
    titleFont.setPointSizeF(std::max(8.0, titleFont.pointSizeF()));
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(foreground);
    painter.drawText(12, top + 22, title);

    QFont nameFont = painter.font();
    nameFont.setBold(false);
    nameFont.setPointSizeF(std::max(7.0, nameFont.pointSizeF() - 1.5));

    for (int i = 0; i < names.size(); ++i) {
        const int column = i % kColumns;
        const int row = i / kColumns;
        const int x = column * kCellWidth;
        const int y = top + kHeaderHeight + row * kCellHeight;

        painter.setPen(grid);
        painter.drawLine(x, y, x + kCellWidth, y);
        painter.drawLine(x, y, x, y + kCellHeight);

        // 三个尺寸并排：能一眼看出哪枚图标在小尺寸下细节糊掉了
        const QPixmap large = lycad::IconLibrary::instance().pixmap(names.at(i), 32);
        const QPixmap medium = lycad::IconLibrary::instance().pixmap(names.at(i), 24);
        const QPixmap small = lycad::IconLibrary::instance().pixmap(names.at(i), 16);
        painter.drawPixmap(x + 10, y + 16, large);
        painter.drawPixmap(x + 52, y + 20, medium);
        painter.drawPixmap(x + 84, y + 24, small);

        painter.setFont(nameFont);
        painter.setPen(foreground);
        painter.drawText(QRect(x + 106, y, kCellWidth - 112, kCellHeight),
                         Qt::AlignVCenter | Qt::AlignLeft, names.at(i));
    }
}

/// 应用图标单独一段。它不随主题换色，所以两种底色各画一排——一眼就能看出
/// 「同一副面孔在浅色与深色桌面上都立得住」；顺带充当 qrc /app 前缀的自检：
/// 前缀或 SVG 出问题时这一段会是空白。
constexpr int kAppIconSectionHeight = kHeaderHeight + 128 + 20;

void paintAppIconSection(QPainter& painter, bool dark, int top, int width)
{
    const QColor background = dark ? QColor(0x2b, 0x30, 0x38) : QColor(0xf4, 0xf5, 0xf7);
    const QColor foreground = dark ? QColor(0xd5, 0xdb, 0xe3) : QColor(0x26, 0x2b, 0x33);

    painter.fillRect(0, top, width, kAppIconSectionHeight, background);

    QFont titleFont = painter.font();
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(foreground);
    painter.drawText(12, top + 22, QStringLiteral("应用图标（不随主题换色）"));

    const QIcon icon = lycad::IconLibrary::applicationIcon();
    const int baseline = top + kHeaderHeight + 116;      // 各档底部对齐
    int x = 16;
    for (const int size : { 16, 24, 32, 48, 64, 128 }) {
        painter.drawPixmap(x, baseline - size, icon.pixmap(size));
        x += size + 18;
    }
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);

    const QStringList arguments = application.arguments();
    const QString outputPath = (arguments.size() > 1) ? arguments.at(1)
                                                      : QStringLiteral("icon_sheet.png");

    const QStringList names = lycad::IconLibrary::names();
    if (names.isEmpty()) {
        out() << QStringLiteral("图标资源为空——请确认 qrc 已被编译进程序。") << Qt::endl;
        return 1;
    }

    const int width = kColumns * kCellWidth + 1;
    const int lightHeight = sectionHeight(names.size());
    const int darkHeight = sectionHeight(names.size());
    const int totalHeight = lightHeight + kSectionGap + darkHeight
                          + kSectionGap + kAppIconSectionHeight * 2;

    QImage sheet(width, totalHeight, QImage::Format_ARGB32_Premultiplied);
    sheet.fill(QColor(0, 0, 0, 0));

    QPainter painter(&sheet);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    lycad::IconLibrary::instance().setDarkTheme(false);
    paintSection(painter, names, QStringLiteral("浅色主题"), false, 0, width);

    const int darkTop = lightHeight + kSectionGap;
    lycad::IconLibrary::instance().setDarkTheme(true);
    paintSection(painter, names, QStringLiteral("深色主题"), true, darkTop, width);

    // 应用图标：主题无关，两种底色各来一排（见 paintAppIconSection 的说明）
    const int appTop = darkTop + darkHeight + kSectionGap;
    paintAppIconSection(painter, false, appTop, width);
    paintAppIconSection(painter, true, appTop + kAppIconSectionHeight, width);

    painter.end();

    // 换回浅色，避免影响同进程内后续绘制
    lycad::IconLibrary::instance().setDarkTheme(false);

    if (!sheet.save(outputPath)) {
        out() << QStringLiteral("无法写入：") << outputPath << Qt::endl;
        return 1;
    }

    out() << QStringLiteral("图标数：") << names.size()
          << QStringLiteral("，输出：") << QFileInfo(outputPath).absoluteFilePath() << Qt::endl;
    return 0;
}
