// =============================================================================
//  LyCAD - tests/test_light.cpp
//  光源（DXF: LIGHT）图元的单元测试。
//
//  覆盖
//    1. 模型：默认值、字段存取、克隆；
//    2. DXF 往返：写出后读回，全部字段逐项一致（这是「支持」的核心承诺——
//       以前这类实体在库层就被丢掉，保存一次就永久消失）；
//    3. 如实上报：目标版本 / 格式承载不了时给警告，不静默丢弃；
//    4. 渲染：屏幕标记的**像素尺寸固定**，不随缩放变化；
//    5. 实样：仓库根目录的 example.dxf（存在才跑）——那条 Spotlight1 必须读进来。
// =============================================================================
#include "core/Document.h"
#include "core/LightEntity.h"
#include "io/CadCodec.h"
#include "io/CadFile.h"
#include "render/CanvasTypes.h"
#include "render/EntityRenderer.h"
#include "render/ViewTransform.h"

#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QString>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace lycad;

namespace {

/// 造一条字段全部非默认的光源：往返测试要能区分「写丢了」与「值恰好等于默认」
std::unique_ptr<LightEntity> makeSpotlight(const Point2D& position = Point2D(1861.4, 2318.5))
{
    auto light = std::make_unique<LightEntity>(position, LightEntity::LightType::Spot);
    light->setName(QStringLiteral("Spotlight1"));
    light->setLayerName(QStringLiteral("ADSK_SYSTEM_LIGHTS"));
    light->setLightColorIndex(3);
    light->setIntensity(5.4);
    light->setPositionZ(1.25);
    light->setTarget(Point2D(2474.4, 2128.3));
    light->setTargetZ(-2.5);
    light->setAttenuation(LightEntity::Attenuation::InverseSquare);
    light->setUseAttenuationLimits(true);
    light->setAttenuationLimits(2.5, 40.0);
    light->setConeAngles(37.5, 62.5);
    light->setCastShadows(true);
    light->setShadowType(2);
    light->setShadowMapSize(512);
    light->setShadowSoftness(3);
    light->setPlotGlyph(true);
    light->setOn(false);
    light->setClassVersion(2);          // 规范范围 0~10 内的正常值
    return light;
}

/// 文档里第一条光源（没有则返回 nullptr）
const LightEntity* firstLight(const Document& document)
{
    for (EntityId id : document.drawOrder()) {
        const Entity* entity = document.entity(id);
        if (entity && entity->type() == EntityType::Light) {
            return static_cast<const LightEntity*>(entity);
        }
    }
    return nullptr;
}

/// 逐字段比对两条光源——少比一项，往返丢数据就会漏过测试
QString diffLight(const LightEntity& a, const LightEntity& b)
{
    const auto ne = [](double x, double y) { return std::abs(x - y) > 1e-9; };
    if (a.name() != b.name()) return QStringLiteral("name %1 != %2").arg(a.name(), b.name());
    if (a.lightType() != b.lightType()) return QStringLiteral("type 不一致");
    if (a.isOn() != b.isOn()) return QStringLiteral("开关不一致");
    if (a.lightColorIndex() != b.lightColorIndex()) return QStringLiteral("光色不一致");
    if (a.plotGlyph() != b.plotGlyph()) return QStringLiteral("打印标记不一致");
    if (ne(a.intensity(), b.intensity())) return QStringLiteral("强度 %1 != %2")
                                                    .arg(a.intensity()).arg(b.intensity());
    if (ne(a.position().x, b.position().x) || ne(a.position().y, b.position().y))
        return QStringLiteral("位置不一致");
    if (ne(a.positionZ(), b.positionZ())) return QStringLiteral("位置 Z 不一致");
    if (ne(a.target().x, b.target().x) || ne(a.target().y, b.target().y))
        return QStringLiteral("目标不一致");
    if (ne(a.targetZ(), b.targetZ())) return QStringLiteral("目标 Z 不一致");
    if (a.attenuation() != b.attenuation()) return QStringLiteral("衰减方式不一致");
    if (a.useAttenuationLimits() != b.useAttenuationLimits()) return QStringLiteral("衰减界限开关不一致");
    if (ne(a.attenuationStartLimit(), b.attenuationStartLimit())) return QStringLiteral("衰减起点不一致");
    if (ne(a.attenuationEndLimit(), b.attenuationEndLimit())) return QStringLiteral("衰减终点不一致");
    if (ne(a.hotspotAngle(), b.hotspotAngle())) return QStringLiteral("聚光角不一致");
    if (ne(a.falloffAngle(), b.falloffAngle())) return QStringLiteral("衰退角不一致");
    if (a.castShadows() != b.castShadows()) return QStringLiteral("阴影开关不一致");
    if (a.shadowType() != b.shadowType()) return QStringLiteral("阴影类型不一致");
    if (a.shadowMapSize() != b.shadowMapSize()) return QStringLiteral("阴影贴图尺寸不一致");
    if (a.shadowSoftness() != b.shadowSoftness()) return QStringLiteral("阴影柔化不一致");
    if (a.classVersion() != b.classVersion()) return QStringLiteral("类版本不一致");
    if (a.layerName() != b.layerName()) return QStringLiteral("图层不一致");
    if (a.colorIndex() != b.colorIndex()) return QStringLiteral("颜色不一致");
    return QString();
}

/// 离屏渲染（光源的包围盒是退化的一个点，不能像普通图元那样 fitTo 世界范围，
/// 因此这里直接给定缩放并把世界原点对到视口中心）
QImage renderLight(const LightEntity& light, double pixelsPerUnit, const QSize& size = QSize(240, 240))
{
    ViewTransform view;
    view.setScale(pixelsPerUnit, Point2D(size.width() / 2.0, size.height() / 2.0));
    view.centerOn(Point2D(0.0, 0.0), size);

    DisplaySettings settings;
    EntityRenderer::DrawContext context;
    context.view = &view;
    context.settings = &settings;
    context.viewport = QRectF(0.0, 0.0, size.width(), size.height());

    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    EntityRenderer::draw(painter, light, context, QColor(0, 0, 0), 0.0,
                         QStringLiteral("CONTINUOUS"));
    painter.end();
    return image;
}

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

} // namespace

class TestLight : public QObject {
    Q_OBJECT

private slots:
    void init();

    // ---- 模型 ---------------------------------------------------------------
    void defaultsMatchSpec();
    void storesFieldsWithoutClamping();
    void cloneCopiesEveryField();

    // ---- DXF 往返 -----------------------------------------------------------
    void dxfRoundTripPreservesEveryField();
    void dwgRoundTripPreservesEveryField();
    void dxfTextContainsLightRecord();
    void classVersionIsNormalizedOnWrite();
    void olderDxfVersionReportsInsteadOfDropping();

    // ---- 其它写出目标 -------------------------------------------------------
    void olderDwgVersionReportsInsteadOfDropping();

    // ---- 渲染 ---------------------------------------------------------------
    void glyphIsScreenSized();
    void glyphRendersForEveryType();

    // ---- 实样 ---------------------------------------------------------------
    void importsRealExampleFile();

private:
    QTemporaryDir m_dir;
};

void TestLight::init()
{
    QVERIFY(m_dir.isValid());
}

void TestLight::defaultsMatchSpec()
{
    const LightEntity light;
    // 默认值与 DXF 规范一致：点光源、开启、白光、强度 1、无衰减
    QVERIFY(light.lightType() == LightEntity::LightType::Point);
    QVERIFY(light.isOn());
    QCOMPARE(light.lightColorIndex(), 7);
    QVERIFY(std::abs(light.intensity() - 1.0) < 1e-9);
    QVERIFY(light.attenuation() == LightEntity::Attenuation::None);
    QVERIFY(!light.plotGlyph());
    QVERIFY(!light.castShadows());
    QCOMPARE(entityTypeName(EntityType::Light), QStringLiteral("LIGHT"));
    QCOMPARE(entityTypeDisplayName(EntityType::Light), QStringLiteral("光源"));
    // 光源没有几何
    QVERIFY(std::abs(light.length()) < 1e-9);
    QCOMPARE(LightEntity::lightTypeDisplayName(LightEntity::LightType::Spot),
             QStringLiteral("聚光灯"));
}

void TestLight::storesFieldsWithoutClamping()
{
    // 导入路径必须**原值保存**：文件里的数字（哪怕区间倒置）不能被悄悄改写
    auto light = std::make_unique<LightEntity>();
    light->setAttenuationLimits(40.0, 2.5);
    QVERIFY(std::abs(light->attenuationStartLimit() - 40.0) < 1e-9);
    QVERIFY(std::abs(light->attenuationEndLimit() - 2.5) < 1e-9);
    light->setConeAngles(-3.0, 200.0);
    QVERIFY(std::abs(light->hotspotAngle() + 3.0) < 1e-9);
    QVERIFY(std::abs(light->falloffAngle() - 200.0) < 1e-9);
    // 位置与目标可以重合（无方向的平行光/点光源）
    light->setTarget(Point2D(1.0, 2.0));
    light->setPosition(Point2D(1.0, 2.0));
    QVERIFY(light->target().distanceTo(light->position()) < 1e-9);
}

void TestLight::cloneCopiesEveryField()
{
    const auto original = makeSpotlight();
    const std::unique_ptr<Entity> copy = original->clone();
    QVERIFY(copy->type() == EntityType::Light);
    const auto* typed = static_cast<const LightEntity*>(copy.get());
    QVERIFY2(diffLight(*original, *typed).isEmpty(), qPrintable(diffLight(*original, *typed)));

    // 改副本不影响原件
    auto* mutableCopy = static_cast<LightEntity*>(copy.get());
    mutableCopy->setName(QStringLiteral("other"));
    QCOMPARE(original->name(), QStringLiteral("Spotlight1"));
}

void TestLight::dxfRoundTripPreservesEveryField()
{
    Document source;
    source.layers().ensure(QStringLiteral("ADSK_SYSTEM_LIGHTS"));
    source.addEntity(makeSpotlight());

    const QString path = m_dir.filePath(QStringLiteral("light.dxf"));
    WriteOptions options;
    options.version = QStringLiteral("AC1032");
    const IoResult writeResult = CadFile::save(path, source, options);
    QVERIFY2(writeResult.ok, qPrintable(writeResult.error));

    Document reloaded;
    const IoResult readResult = CadFile::load(path, reloaded);
    QVERIFY2(readResult.ok, qPrintable(readResult.error));

    const LightEntity* after = firstLight(reloaded);
    QVERIFY2(after != nullptr, "往返后光源必须还在（以前这一类会被整条丢掉）");
    const QString diff = diffLight(*makeSpotlight(), *after);
    QVERIFY2(diff.isEmpty(), qPrintable(diff));
}

void TestLight::dwgRoundTripPreservesEveryField()
{
    // DWG 侧的光源是「类对象」（CLASSES 段里的 AcDbLight），位布局与 DXF 完全不同；
    // 这条用例守住的是：写进 DWG 的那份位布局能被自己的读取端逐字段解回来。
    Document source;
    source.layers().ensure(QStringLiteral("ADSK_SYSTEM_LIGHTS"));
    source.addEntity(makeSpotlight());

    const QString path = m_dir.filePath(QStringLiteral("light.dwg"));
    WriteOptions options;
    options.dwgVersion = QStringLiteral("AC1032");
    const IoResult writeResult = CadFile::save(path, source, options);
    QVERIFY2(writeResult.ok, qPrintable(writeResult.error));
    // 不能有「跳过 LIGHT」这类告警：DWG 路径已支持
    for (const QString& warning : writeResult.warnings) {
        QVERIFY2(!warning.contains(QStringLiteral("LIGHT")), qPrintable(warning));
    }

    Document reloaded;
    const IoResult readResult = CadFile::load(path, reloaded);
    QVERIFY2(readResult.ok, qPrintable(readResult.error));

    const LightEntity* after = firstLight(reloaded);
    QVERIFY2(after != nullptr, "DWG 往返后光源必须还在");
    const QString diff = diffLight(*makeSpotlight(), *after);
    QVERIFY2(diff.isEmpty(), qPrintable(diff));
}

void TestLight::classVersionIsNormalizedOnWrite()
{
    // 源文件里出现过 -1023410171（0xC3000005）这种越界值：严格解析端会因
    // 「class_version > 10」中止整条对象的解析，因此写出时归一为 0；
    // 导入侧不动模型（文件里是什么就存什么）。
    auto light = makeSpotlight();
    light->setClassVersion(-1023410171);

    Document source;
    source.layers().ensure(QStringLiteral("ADSK_SYSTEM_LIGHTS"));
    source.addEntity(std::move(light));

    WriteOptions options;
    options.version = QStringLiteral("AC1032");
    const QString path = m_dir.filePath(QStringLiteral("light-classversion.dxf"));
    QVERIFY(CadFile::save(path, source, options).ok);

    Document reloaded;
    QVERIFY(CadFile::load(path, reloaded).ok);
    const LightEntity* after = firstLight(reloaded);
    QVERIFY(after != nullptr);
    QCOMPARE(after->classVersion(), 0);

    // 规范范围内的值原样保留（不是一律清零）
    auto sane = makeSpotlight();
    sane->setClassVersion(3);
    Document second;
    second.layers().ensure(QStringLiteral("ADSK_SYSTEM_LIGHTS"));
    second.addEntity(std::move(sane));
    const QString secondPath = m_dir.filePath(QStringLiteral("light-classversion-ok.dxf"));
    QVERIFY(CadFile::save(secondPath, second, options).ok);
    Document reread;
    QVERIFY(CadFile::load(secondPath, reread).ok);
    const LightEntity* kept = firstLight(reread);
    QVERIFY(kept != nullptr);
    QCOMPARE(kept->classVersion(), 3);
}

void TestLight::dxfTextContainsLightRecord()
{
    Document source;
    source.layers().ensure(QStringLiteral("ADSK_SYSTEM_LIGHTS"));
    source.addEntity(makeSpotlight());

    const QString path = m_dir.filePath(QStringLiteral("light-text.dxf"));
    WriteOptions options;
    options.version = QStringLiteral("AC1032");
    QVERIFY(CadFile::save(path, source, options).ok);

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray text = file.readAll();
    file.close();

    // 记录形态与关键组码：少一样就说明写出的是个空壳
    QVERIFY2(text.contains("LIGHT"), "DXF 里应有 LIGHT 实体");
    QVERIFY2(text.contains("AcDbLight"), "应有 AcDbLight 子类标记");
    QVERIFY2(text.contains("Spotlight1"), "光源名应写回");
    QVERIFY2(text.contains("AcDbSavedByObjectVersion") == false,
             "EED 未建模，不该凭空出现");
}

void TestLight::olderDxfVersionReportsInsteadOfDropping()
{
    Document source;
    source.layers().ensure(QStringLiteral("ADSK_SYSTEM_LIGHTS"));
    source.addEntity(makeSpotlight());

    // R2004 及更早的 DXF 里没有 LIGHT 实体：必须**如实上报**，不能静静不写
    const QString path = m_dir.filePath(QStringLiteral("light-r2004.dxf"));
    WriteOptions options;
    options.version = QStringLiteral("AC1018");
    const IoResult result = CadFile::save(path, source, options);
    QVERIFY2(result.ok, qPrintable(result.error));

    bool mentioned = false;
    for (const QString& warning : result.warnings) {
        if (warning.contains(QStringLiteral("LIGHT"))) mentioned = true;
    }
    QVERIFY2(mentioned, "目标版本早于 R2007 时应提示 LIGHT 写不出来");
}

void TestLight::olderDwgVersionReportsInsteadOfDropping()
{
    // LIGHT 是 R2007 起才有的类：目标是更早的 DWG 版本时必须**如实上报**，
    // 既不能写出非法文件，也不能让图元静静消失。
    Document source;
    source.layers().ensure(QStringLiteral("ADSK_SYSTEM_LIGHTS"));
    source.addEntity(makeSpotlight());

    const QString path = m_dir.filePath(QStringLiteral("light-r2000.dwg"));
    WriteOptions options;
    options.dwgVersion = QStringLiteral("AC1015");
    const IoResult result = CadFile::save(path, source, options);
    QVERIFY2(result.ok, qPrintable(result.error));

    bool mentioned = false;
    for (const QString& warning : result.warnings) {
        if (warning.contains(QStringLiteral("LIGHT"))) mentioned = true;
    }
    QVERIFY2(mentioned, "目标版本早于 R2007 时应提示 LIGHT 写不出来");
}

void TestLight::glyphIsScreenSized()
{
    // 标记是固定像素尺寸的：缩放 8 倍后墨迹量应基本不变
    LightEntity light(Point2D(0.0, 0.0), LightEntity::LightType::Point);
    const int atOne = inkCount(renderLight(light, 1.0));
    const int atEight = inkCount(renderLight(light, 8.0));
    QVERIFY2(atOne > 0, "标记应画得出来");
    QVERIFY2(std::abs(atEight - atOne) <= std::max(4, atOne / 4),
             qPrintable(QStringLiteral("标记应固定像素尺寸：1x=%1 8x=%2").arg(atOne).arg(atEight)));
}

void TestLight::glyphRendersForEveryType()
{
    std::vector<QImage> panels;
    for (LightEntity::LightType type : { LightEntity::LightType::Distant,
                                         LightEntity::LightType::Point,
                                         LightEntity::LightType::Spot,
                                         LightEntity::LightType::Web }) {
        LightEntity light(Point2D(0.0, 0.0), type);
        light.setTarget(Point2D(30.0, 30.0));       // 方向朝右上
        QVERIFY2(inkCount(renderLight(light, 1.0)) > 0,
                 qPrintable(QStringLiteral("类型 %1 应有可见标记")
                                .arg(LightEntity::lightTypeDisplayName(type))));
        panels.push_back(renderLight(light, 1.0, QSize(90, 90)));
    }

    // 关闭的光源仍要看得见（画成虚线），否则用户找不到它去重新打开
    LightEntity off(Point2D(0.0, 0.0), LightEntity::LightType::Spot);
    off.setOn(false);
    QVERIFY(inkCount(renderLight(off, 1.0)) > 0);

    // 便于人工看一眼四种标记：设 LYCAD_LIGHT_PNG=<路径> 时把四格拼图落盘
    const QByteArray png = qgetenv("LYCAD_LIGHT_PNG");
    if (!png.isEmpty() && panels.size() == 4) {
        QImage sheet(panels[0].width() * 4, panels[0].height(),
                     QImage::Format_ARGB32_Premultiplied);
        sheet.fill(Qt::white);
        QPainter painter(&sheet);
        for (int i = 0; i < 4; ++i) {
            painter.drawImage(QPoint(i * panels[0].width(), 0), panels[static_cast<std::size_t>(i)]);
        }
        painter.end();
        sheet.save(QString::fromLocal8Bit(png));
    }
}

void TestLight::importsRealExampleFile()
{
#ifdef LYCAD_TEST_SOURCE_DIR
    const QString path = QStringLiteral(LYCAD_TEST_SOURCE_DIR) + QStringLiteral("/example.dxf");
    if (!QFileInfo::exists(path)) QSKIP("未找到 example.dxf，跳过实样测试");

    Document document;
    const IoResult result = CadFile::load(path, document);
    QVERIFY2(result.ok, qPrintable(result.error));

    const LightEntity* light = firstLight(document);
    QVERIFY2(light != nullptr, "example.dxf 里的光源应被读进来（以前会在库层就丢掉）");
    QCOMPARE(light->name(), QStringLiteral("Spotlight1"));
    QVERIFY(light->lightType() == LightEntity::LightType::Spot);
    QVERIFY(light->isOn());
    QCOMPARE(light->lightColorIndex(), 7);
    QVERIFY(std::abs(light->intensity() - 5.4) < 1e-9);
    QVERIFY(std::abs(light->position().x - 1861.377715175666) < 1e-6);
    QVERIFY(std::abs(light->position().y - 2318.490304971529) < 1e-6);
    QVERIFY(std::abs(light->target().x - 2474.393801603597) < 1e-6);
    QVERIFY(std::abs(light->hotspotAngle() - 45.0) < 1e-9);
    QVERIFY(std::abs(light->falloffAngle() - 50.0) < 1e-9);
    QVERIFY(light->castShadows());
    QCOMPARE(light->shadowMapSize(), 256);
    QCOMPARE(light->classVersion(), -1023410171);
    QCOMPARE(light->layerName(), QStringLiteral("ADSK_SYSTEM_LIGHTS"));

    // 顺带守住「跳过计数」的口径：库对未知实体是按**实体**上报一次，
    // 不是按组码（否则一条 3DSOLID 会被报成上百条）。
    for (const QString& warning : result.warnings) {
        if (warning.contains(QStringLiteral("3DSOLID"))) {
            QVERIFY2(warning.contains(QStringLiteral("跳过 1 个")),
                     qPrintable(QStringLiteral("跳过计数应按实体：%1").arg(warning)));
        }
        QVERIFY2(!warning.contains(QStringLiteral("LIGHT")),
                 qPrintable(QStringLiteral("已支持的光源不该出现在跳过清单里：%1").arg(warning)));
    }
#else
    QSKIP("未定义 LYCAD_TEST_SOURCE_DIR，跳过实样测试");
#endif
}

QTEST_MAIN(TestLight)
#include "test_light.moc"
