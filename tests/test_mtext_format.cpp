// =============================================================================
//  LyCAD - tests/test_mtext_format.cpp
//  多行文字（MTEXT）**内联格式码**的解析与显示。
//
//  背景
//    MTEXT 的内容是一串带内联格式码的文本：
//        \fArial|b1;加粗\fArial|b0; 与斜体 \fArial|i1;Italic\fArial|i0;
//        \C1;红\C3;绿\C5;蓝
//    早期实现把这些码**整段剥掉只看字**，于是屏幕上一律是普通格式——
//    加粗、斜体、颜色全都看不出来（正是本文件要守住的行为）。
//
//  覆盖
//    1. 解析：粗/斜/颜色/字高/宽度/倾斜/字距被切成带格式的片段（Run）；
//       分组 `{…}` 的作用域、转义与「无分号的 \ 按字面保留」；
//    2. 排版：行宽取各段之和，字高倍数影响段宽，可见文本与剥码口径一致；
//    3. 渲染：离屏画到图，红/绿/蓝三色必须真的落在图上、粗体墨迹必须多于常规。
// =============================================================================
#include "core/Document.h"
#include "core/MTextEntity.h"
#include "core/TextEntity.h"
#include "io/CadFile.h"
#include "render/CanvasTypes.h"
#include "render/EntityRenderer.h"
#include "render/ViewTransform.h"

#include <QColor>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QTest>

#include <algorithm>
#include <memory>

using namespace lycad;

namespace {

/// 样例生成器写入的 MTEXT 内容（与 tools/sample_generator.cpp 逐字一致）
QString sampleContent()
{
    return QStringLiteral("多行文字（MTEXT）示例\\P"
                          "第二行：内联格式 \\fArial|b1;加粗\\fArial|b0; 与斜体 "
                          "\\fArial|i1;Italic\\fArial|i0;\\P"
                          "第三行：颜色 \\C1;红\\C3;绿\\C5;蓝");
}

/// 把一段 MTEXT 离屏渲染成图（自适应铺满，字够大，便于数像素）
QImage renderMText(const MTextEntity& text, const QSize& size = QSize(700, 220))
{
    Box2D box;
    for (const Point2D& p : text.outlineCorners()) box.expand(p);
    if (!box.isValid()) return QImage();

    ViewTransform view;
    view.fitTo(box, size, 0.12);

    DisplaySettings settings;
    settings.themeIsDark = false;

    EntityRenderer::DrawContext context;
    context.view = &view;
    context.settings = &settings;
    context.viewport = QRectF(0.0, 0.0, size.width(), size.height());

    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    EntityRenderer::draw(painter, text, context, QColor(Qt::black), 0.0,
                         QStringLiteral("CONTINUOUS"));
    painter.end();
    return image;
}

/// 非白像素数（墨迹量）
int inkCount(const QImage& image)
{
    int ink = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (qRed(pixel) < 200 || qGreen(pixel) < 200 || qBlue(pixel) < 200) ++ink;
        }
    }
    return ink;
}

/// 与目标色「同调」的像素数：主通道明显强于其余通道（抗锯齿边缘会被算进来，
/// 故只要求数量级上「确实有」，不要求精确等于某种纯色）
int countDominant(const QImage& image, int channel)
{
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            const int c[3] = { qRed(pixel), qGreen(pixel), qBlue(pixel) };
            const int others = c[(channel + 1) % 3] + c[(channel + 2) % 3];
            if (c[channel] < 100 && others < 100) continue;      // 近黑（未着色文字）
            if (c[channel] > 60 && c[channel] - std::max(c[(channel + 1) % 3],
                                                        c[(channel + 2) % 3]) > 50) {
                ++count;
            }
        }
    }
    return count;
}

/// 解析结果里所有 Run 的文本拼起来（按段落）
QStringList paragraphTexts(const std::vector<std::vector<MTextEntity::Run>>& paragraphs)
{
    QStringList out;
    for (const auto& paragraph : paragraphs) {
        QString text;
        for (const MTextEntity::Run& run : paragraph) text.append(run.text);
        out.push_back(text);
    }
    return out;
}

const MTextEntity::RunStyle* firstStyled(
    const std::vector<std::vector<MTextEntity::Run>>& paragraphs, int paragraph, bool bold,
    bool italic)
{
    if (paragraph >= static_cast<int>(paragraphs.size())) return nullptr;
    for (const MTextEntity::Run& run : paragraphs[paragraph]) {
        if (run.style.bold == bold && run.style.italic == italic) return &run.style;
    }
    return nullptr;
}

} // namespace

class TestMTextFormat : public QObject {
    Q_OBJECT

private slots:
    // ---- 解析 ---------------------------------------------------------------

    /// 段落切分与可见文本：内联码不显示、`\P` 分段
    void parseSplitsParagraphsAndHidesCodes();
    /// `\f…|b1|i1;` 在后续文字上生效，`b0/i0` 关掉
    void parseAppliesBoldAndItalic();
    /// `\C1/3/5;` 给出三段带 ACI 颜色的片段
    void parseAppliesColors();
    /// `\H2x;` `\W0.5;` `\Q15;` `\T2;` 落到段宽/样式上
    void parseAppliesHeightWidthObliqueTracking();
    /// `{…}` 分组的作用域：出组即恢复
    void groupScopingRestoresFormat();
    /// 转义与「无分号的 \」按字面保留（与 stripInlineCodes 同口径）
    void escapesAndUnclosedCodesAreLiteral();
    /// 堆叠码 `\S1/2;` 平铺成可见文本（不得整段消失）
    void stackCodeKeepsVisibleText();

    // ---- 排版 ---------------------------------------------------------------

    /// 行宽 = 各段右端；字高倍数把该段撑宽
    void layoutWidthFollowsRuns();
    /// 无内联格式时行为不变（回归：字宽/行宽与剥码后的估算一致）
    void layoutUnchangedForPlainText();

    // ---- 渲染 ---------------------------------------------------------------

    /// 红/绿/蓝三色必须真的画到图上
    void renderShowsInlineColors();
    /// 加粗段的墨迹多于同文常规段
    void renderShowsBold();
    /// 整条样例 MTEXT 渲染后有彩色像素（端到端）
    void renderSampleMTextHasColors();
    /// 实样文件（可选）：读出的 MTEXT 内容原样保留格式码
    void loadedSampleKeepsInlineCodes();
};

void TestMTextFormat::parseSplitsParagraphsAndHidesCodes()
{
    const auto paragraphs = MTextEntity::parseRuns(sampleContent(), 4.0);

    QCOMPARE(paragraphs.size(), std::size_t(3));
    QCOMPARE(paragraphTexts(paragraphs),
             QStringList({ QStringLiteral("多行文字（MTEXT）示例"),
                           QStringLiteral("第二行：内联格式 加粗 与斜体 Italic"),
                           QStringLiteral("第三行：颜色 红绿蓝") }));

    // 与「只剥码」的可见文本口径一致
    const MTextEntity text(sampleContent(), Point2D(0.0, 0.0), 4.0);
    QCOMPARE(text.plainText(),
             QStringLiteral("多行文字（MTEXT）示例\n第二行：内联格式 加粗 与斜体 Italic\n"
                            "第三行：颜色 红绿蓝"));
}

void TestMTextFormat::parseAppliesBoldAndItalic()
{
    const auto paragraphs = MTextEntity::parseRuns(sampleContent(), 4.0);

    const MTextEntity::RunStyle* bold = firstStyled(paragraphs, 1, true, false);
    QVERIFY2(bold != nullptr, "第二行应有一段粗体");
    const MTextEntity::RunStyle* italic = firstStyled(paragraphs, 1, false, true);
    QVERIFY2(italic != nullptr, "第二行应有一段斜体");

    // 粗/斜来自同一个 \f 码，字体名也应一并采用
    QCOMPARE(bold->fontFamily, QStringLiteral("Arial"));
    QCOMPARE(italic->fontFamily, QStringLiteral("Arial"));

    // 第一行没有格式码
    QVERIFY(paragraphs[0].size() == 1 && paragraphs[0][0].style.isPlain());
}

void TestMTextFormat::parseAppliesColors()
{
    // 第三行 = 常规前缀 + 三个着色段
    const auto paragraphs = MTextEntity::parseRuns(sampleContent(), 4.0);
    QCOMPARE(paragraphs[2].size(), std::size_t(4));
    QCOMPARE(paragraphs[2][0].text, QStringLiteral("第三行：颜色 "));
    QVERIFY(!paragraphs[2][0].style.hasColor);
    QCOMPARE(paragraphs[2][1].text, QStringLiteral("红"));
    QCOMPARE(paragraphs[2][2].text, QStringLiteral("绿"));
    QCOMPARE(paragraphs[2][3].text, QStringLiteral("蓝"));
    QCOMPARE(paragraphs[2][1].style.colorAci, 1);
    QCOMPARE(paragraphs[2][2].style.colorAci, 3);
    QCOMPARE(paragraphs[2][3].style.colorAci, 5);
    for (int i = 1; i < 4; ++i) QVERIFY(paragraphs[2][i].style.hasColor);
    // 颜色不影响粗斜
    QVERIFY(!paragraphs[2][1].style.bold && !paragraphs[2][1].style.italic);
}

void TestMTextFormat::parseAppliesHeightWidthObliqueTracking()
{
    const auto paragraphs = MTextEntity::parseRuns(
        QStringLiteral("A\\H2x;B\\W0.5;C\\Q15;D\\T2;E"), 10.0);
    QCOMPARE(paragraphs.size(), std::size_t(1));
    const auto& runs = paragraphs[0];
    QCOMPARE(runs.size(), std::size_t(5));

    QVERIFY(std::abs(runs[0].style.heightFactor - 1.0) < 1e-9);      // A 未受后续码影响
    QVERIFY(std::abs(runs[1].style.heightFactor - 2.0) < 1e-9);      // \H2x; -> 2 倍字高
    QVERIFY(std::abs(runs[2].style.widthFactor - 0.5) < 1e-9);       // \W0.5;
    QVERIFY(std::abs(runs[3].style.obliqueDeg - 15.0) < 1e-9);       // \Q15;
    QVERIFY(std::abs(runs[4].style.tracking - 2.0) < 1e-9);          // \T2;

    // 绝对字高 `\H5;` 按图元字高换算成倍数
    const auto absolute = MTextEntity::parseRuns(QStringLiteral("\\H5;X"), 10.0);
    QVERIFY(std::abs(absolute[0][0].style.heightFactor - 0.5) < 1e-9);
}

void TestMTextFormat::groupScopingRestoresFormat()
{
    const auto paragraphs = MTextEntity::parseRuns(
        QStringLiteral("{\\fArial|b1;A}B"), 4.0);
    QCOMPARE(paragraphs.size(), std::size_t(1));
    QCOMPARE(paragraphs[0].size(), std::size_t(2));
    QCOMPARE(paragraphs[0][0].text, QStringLiteral("A"));
    QCOMPARE(paragraphs[0][1].text, QStringLiteral("B"));
    QVERIFY2(paragraphs[0][0].style.bold, "组内应是粗体");
    QVERIFY2(!paragraphs[0][1].style.bold, "出组后应恢复常规");
    QVERIFY2(paragraphs[0][1].style.fontFamily.isEmpty(), "出组后不应残留组内字体");
    // 分组符本身不显示
    QVERIFY(!paragraphTexts(paragraphs)[0].contains(QLatin1Char('{')));
}

void TestMTextFormat::escapesAndUnclosedCodesAreLiteral()
{
    // \\ -> 反斜杠；\{ \} -> 花括号；\~ -> 空格
    const auto paragraphs = MTextEntity::parseRuns(
        QStringLiteral("a\\\\b\\{c\\}d\\~e"), 4.0);
    QCOMPARE(paragraphTexts(paragraphs).value(0), QStringLiteral("a\\b{c}d e"));

    // 没有分号的 `\f` 不是格式码，按字面保留（与 stripInlineCodes 一致）
    const QString raw = QStringLiteral("x\\fy  z");
    const auto literal = MTextEntity::parseRuns(raw, 4.0);
    QCOMPARE(paragraphTexts(literal).value(0), QStringLiteral("x\\fy  z"));
    const MTextEntity text(raw, Point2D(0.0, 0.0), 4.0);
    QCOMPARE(text.plainText(), literal[0][0].text);
}

void TestMTextFormat::stackCodeKeepsVisibleText()
{
    // `\S1^2;` 是堆叠（上标/下标），本实现平铺成 "1/2"——
    // 关键是**不能整段消失**（早期实现把堆叠内容一起剥掉了）
    const auto paragraphs = MTextEntity::parseRuns(QStringLiteral("面积\\S2^3;平方米"), 4.0);
    QCOMPARE(paragraphTexts(paragraphs).value(0), QStringLiteral("面积2/3平方米"));
}

void TestMTextFormat::layoutWidthFollowsRuns()
{
    const MTextEntity text(
        QStringLiteral("常规\\H3x;大字"), Point2D(0.0, 0.0), 4.0);
    const MTextEntity::Layout laid = text.layout();
    QCOMPARE(laid.lines.size(), std::size_t(1));

    const auto& line = laid.lines.front();
    QCOMPARE(line.runs.size(), std::size_t(2));

    double summed = 0.0;
    for (const MTextEntity::Run& run : line.runs) summed += run.width;
    QVERIFY2(std::abs(line.width - summed) < 1e-9, "行宽应等于各段宽之和");
    QVERIFY2(std::abs(line.x) < 1e-9, "左上附着点且无对齐码时，行首应落在插入点上");

    // 「大字」那段的字高是 3 倍，同样的字应比常规段宽得多
    QVERIFY(line.runs[1].width > line.runs[0].width * 2.0);
    QVERIFY(std::abs(line.runs[1].offsetX - line.runs[0].width) < 1e-9);

    // 内容里的格式码不得出现在可见文本里
    QVERIFY(!line.text.contains(QLatin1Char('\\')));
    QCOMPARE(line.text, QStringLiteral("常规大字"));
}

void TestMTextFormat::layoutUnchangedForPlainText()
{
    // 无格式码：行宽必须与既有的「按字高估算整行」完全一致（回归护栏）
    const QString plain = QStringLiteral("LyCAD 样例图纸");
    const MTextEntity text(plain, Point2D(0.0, 0.0), 6.0);
    const MTextEntity::Layout laid = text.layout();

    QCOMPARE(laid.lines.size(), std::size_t(1));
    QCOMPARE(laid.lines.front().runs.size(), std::size_t(1));
    QVERIFY(laid.lines.front().runs.front().style.isPlain());
    QVERIFY(std::abs(laid.lines.front().width - TextEntity::estimateLineWidth(plain, 6.0, 1.0))
            < 1e-9);
}

void TestMTextFormat::renderShowsInlineColors()
{
    const MTextEntity text(QStringLiteral("\\C1;红\\C3;绿\\C5;蓝"), Point2D(0.0, 0.0), 10.0);
    const QImage image = renderMText(text);
    QVERIFY(!image.isNull());
    QVERIFY(inkCount(image) > 0);

    QVERIFY2(countDominant(image, 0) > 20, "应画出色相为红的像素");
    QVERIFY2(countDominant(image, 1) > 20, "应画出色相为绿的像素");
    QVERIFY2(countDominant(image, 2) > 20, "应画出色相为蓝的像素");
}

void TestMTextFormat::renderShowsBold()
{
    const QString word = QStringLiteral("加粗AB");
    const MTextEntity plain(word, Point2D(0.0, 0.0), 10.0);
    const MTextEntity bold(QStringLiteral("\\fArial|b1;") + word, Point2D(0.0, 0.0), 10.0);

    // 先确认模型层确实认出了粗体
    const auto paragraphs = MTextEntity::parseRuns(bold.content(), bold.height());
    QVERIFY(paragraphs[0][0].style.bold);

    const int plainInk = inkCount(renderMText(plain));
    const int boldInk = inkCount(renderMText(bold));
    QVERIFY(plainInk > 0);
    QVERIFY2(boldInk > plainInk,
             qPrintable(QStringLiteral("粗体墨迹(%1)应多于常规(%2)").arg(boldInk).arg(plainInk)));
}

void TestMTextFormat::renderSampleMTextHasColors()
{
    // 端到端：样例里的那条 MTEXT，渲染后必须同时有墨迹与彩色像素
    MTextEntity text(sampleContent(), Point2D(0.0, 0.0), 6.0);
    text.setAttachment(MTextEntity::Attachment::MiddleLeft);
    text.setReferenceWidth(80.0);

    const QImage image = renderMText(text, QSize(900, 260));
    QVERIFY(!image.isNull());
    QVERIFY(inkCount(image) > 0);
    QVERIFY2(countDominant(image, 0) > 10, "样例第三行的红字应可见");
    QVERIFY2(countDominant(image, 1) > 10, "样例第三行的绿字应可见");
    QVERIFY2(countDominant(image, 2) > 10, "样例第三行的蓝字应可见");

    // 便于人工看一眼效果：设 LYCAD_MTEXT_PNG=<路径> 时把渲染结果落盘
    const QByteArray png = qgetenv("LYCAD_MTEXT_PNG");
    if (!png.isEmpty()) image.save(QString::fromLocal8Bit(png));
}

void TestMTextFormat::loadedSampleKeepsInlineCodes()
{
#ifdef LYCAD_TEST_SOURCE_DIR
    const QString path = QStringLiteral(LYCAD_TEST_SOURCE_DIR)
                         + QStringLiteral("/sample/2018/lycad_sample_2018.dwg");
    if (!QFileInfo::exists(path)) QSKIP("未找到 sample/2018/lycad_sample_2018.dwg");

    Document document;
    const IoResult result = CadFile::load(path, document);
    QVERIFY2(result.ok, qPrintable(result.error));

    const MTextEntity* found = nullptr;
    for (EntityId id : document.drawOrder()) {
        const Entity* entity = document.entity(id);
        if (entity && entity->type() == EntityType::MText) {
            found = static_cast<const MTextEntity*>(entity);
            break;
        }
    }
    QVERIFY2(found != nullptr, "样例里应有一条 MTEXT");

    // 文件里的原文（含格式码）必须一字不改地读回来
    QCOMPARE(found->content(), sampleContent());

    // 解析后三行、且第二行有粗体与斜体、第三行有三个着色段
    const auto paragraphs = MTextEntity::parseRuns(found->content(), found->height());
    QCOMPARE(paragraphs.size(), std::size_t(3));
    QVERIFY(firstStyled(paragraphs, 1, true, false) != nullptr);
    QVERIFY(firstStyled(paragraphs, 1, false, true) != nullptr);
    QCOMPARE(paragraphs[2].size(), std::size_t(4));
    QVERIFY(paragraphs[2][1].style.hasColor);
#else
    QSKIP("未定义 LYCAD_TEST_SOURCE_DIR，跳过实样测试");
#endif
}

QTEST_MAIN(TestMTextFormat)
#include "test_mtext_format.moc"
