// =============================================================================
//  LyCAD - tests/test_dxf_roundtrip.cpp
//  DXF 文件层的往返一致性测试。
//
//  测试策略：构造一份覆盖全部图元类型与典型属性的文档，写出后再读回，
//  逐图元比对「类型 / 图层 / 颜色 / 可见性 / 几何参数」。
//  同时覆盖三种文件形态：ASCII-R12、ASCII-R2000、二进制 DXF。
// =============================================================================
#include <QtTest>

#include <QTemporaryDir>

#include <cstring>

#include "core/ArcEntity.h"
#include "core/CircleEntity.h"
#include "core/DimensionEntity.h"
#include "core/Document.h"
#include "core/Format.h"
#include "core/HatchEntity.h"
#include "core/LineEntity.h"
#include "core/PointEntity.h"
#include "core/PolylineEntity.h"
#include "core/TextEntity.h"
#include "io/CadFile.h"
#include "io/DwgCodec.h"

#include <QFile>
#include <QTemporaryDir>

#include <cmath>

using namespace lycad;

class TestDxfRoundTrip : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void roundTripR12();
    void roundTripR2000();
    void binaryDxfIsReadable();
    void unsupportedEntitiesAreCountedNotDropped();
    void dwgSignatureIsRecognized();

private:
    /// includeHatch 为 false 时（R12）不构造填充图元——
    /// R12 没有 HATCH 实体，导出会降级为闭合多段线，往返签名必然不同。
    static void buildSampleDocument(Document& document, bool includeHatch);
    static QStringList entitySignatures(const Document& document);
    /// includeLineweight 为 false 时忽略线宽字段——
    /// R12 的 LAYER 记录没有 370 组码，线宽本身无法表达，比对时需排除。
    static QStringList layerSignatures(const Document& document, bool includeLineweight);

    /// 写出后读回，返回两侧的签名列表是否一致
    bool roundTrip(const QString& version, const QString& fileName,
                   QString* failure);

    QTemporaryDir m_dir;
};

// -----------------------------------------------------------------------------
void TestDxfRoundTrip::initTestCase()
{
    QVERIFY2(m_dir.isValid(), "无法创建临时目录，测试无法继续");
    registerBuiltinCodecs();
}

void TestDxfRoundTrip::buildSampleDocument(Document& document, bool includeHatch)
{
    document.clear(true);

    QString error;
    QVERIFY(document.addLayer(Layer(QStringLiteral("轮廓")), &error));
    QVERIFY(document.addLayer(Layer(QStringLiteral("中心线")), &error));

    document.layers().ensure(QStringLiteral("轮廓")).setColorIndex(1);
    document.layers().ensure(QStringLiteral("中心线")).setColorIndex(4);
    document.layers().ensure(QStringLiteral("中心线")).setLinetype(QStringLiteral("DASHED"));
    document.layers().ensure(QStringLiteral("轮廓")).setLineweightDxf(35);
    document.setCurrentLayer(QStringLiteral("轮廓"));

    document.beginBatchLoad();

    // 直线：显式颜色 3，位于 0 层
    {
        auto line = std::make_unique<LineEntity>(Point2D(0.0, 0.0), Point2D(120.0, 45.5));
        line->setColorIndex(3);
        document.addEntity(std::move(line));
    }

    // 圆：随层颜色
    document.addEntity(std::make_unique<CircleEntity>(Point2D(60.0, 60.0), 25.0));

    // 圆弧：随层颜色，逆时针自 30° 到 200°
    document.addEntity(std::make_unique<ArcEntity>(Point2D(-30.0, 15.0), 40.0,
                                                   degToRad(30.0), degToRad(200.0)));

    // 多段线：含一个顺时针圆弧段与一个逆时针圆弧段，且闭合
    {
        auto polyline = std::make_unique<PolylineEntity>();
        polyline->appendVertex(Point2D(0.0, 100.0), 0.5);      // 逆时针弧段
        polyline->appendVertex(Point2D(50.0, 100.0), -0.25);   // 顺时针弧段
        polyline->appendVertex(Point2D(50.0, 130.0), 0.0);
        polyline->appendVertex(Point2D(0.0, 130.0), 0.0);
        polyline->setClosed(true);
        document.addEntity(std::move(polyline));
    }

    // 带宽度多段线：R2000 走 43 组码，R12 走 POLYLINE 头部的 40/41
    {
        auto wide = std::make_unique<PolylineEntity>();
        wide->appendVertex(Point2D(60.0, 20.0), 0.0);
        wide->appendVertex(Point2D(60.0, 70.0), 0.0);
        wide->appendVertex(Point2D(90.0, 70.0), 0.0);
        wide->setConstantWidth(2.5);
        wide->setClosed(false);
        document.addEntity(std::move(wide));
    }

    // 点
    document.addEntity(std::make_unique<PointEntity>(Point2D(7.5, -3.25)));

    // 文字：默认左/基线对齐；含中文内容
    document.addEntity(std::make_unique<TextEntity>(QStringLiteral("LyCAD 往返测试"),
                                                    Point2D(5.0, 140.0), 5.0));

    // 文字：居中/居中 对齐 + 水平镜像（背面文字）+ 宽度因子
    // （走 DXF 72/73 + 11/21 + 71 生成标志 + 41 通道）
    {
        auto centered = std::make_unique<TextEntity>(QStringLiteral("中心文字"),
                                                     Point2D(60.0, 60.0), 3.0);
        centered->setHAlign(TextEntity::HAlign::Center);
        centered->setVAlign(TextEntity::VAlign::Middle);
        centered->setAlignmentPoint(Point2D(60.0, 60.0));
        centered->setLayerName(QStringLiteral("轮廓"));
        centered->setBackwards(true);
        centered->setWidthFactor(1.1);
        document.addEntity(std::move(centered));
    }

    // 填充（R2000 专属；R12 导出会降级为多段线，故在该版本下跳过）
    if (includeHatch) {
        auto hatch = std::make_unique<HatchEntity>(
            std::vector<Point2D>{ Point2D(80.0, 80.0), Point2D(110.0, 80.0),
                                  Point2D(115.0, 100.0), Point2D(85.0, 105.0) },
            QStringLiteral("ANSI31"), 2.0, 0.0);
        hatch->setLayerName(QStringLiteral("轮廓"));
        document.addEntity(std::move(hatch));
    }

    // 尺寸标注：对齐 / 线性 / 半径 / 直径
    {
        auto aligned = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Aligned);
        aligned->setFirstPoint(Point2D(0.0, 0.0));
        aligned->setSecondPoint(Point2D(120.0, 45.5));
        aligned->setDimLinePoint(Point2D(60.0, -20.0));
        aligned->setLayerName(QStringLiteral("中心线"));
        document.addEntity(std::move(aligned));

        auto linear = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Linear);
        linear->setFirstPoint(Point2D(130.0, 0.0));
        linear->setSecondPoint(Point2D(130.0, 60.0));
        linear->setDimLinePoint(Point2D(150.0, 30.0));
        linear->setRotation(degToRad(90.0));
        linear->setLayerName(QStringLiteral("中心线"));
        document.addEntity(std::move(linear));

        const Point2D circleCenter(60.0, 60.0);
        auto radial = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Radial);
        radial->setFirstPoint(circleCenter);
        radial->setSecondPoint(circleCenter + Vector2D(25.0 * std::cos(degToRad(30.0)),
                                                       25.0 * std::sin(degToRad(30.0))));
        radial->setDimLinePoint(Point2D(100.0, 90.0));
        radial->setLayerName(QStringLiteral("中心线"));
        document.addEntity(std::move(radial));

        auto diametric = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Diametric);
        diametric->setFirstPoint(Point2D(35.0, 60.0));
        diametric->setSecondPoint(Point2D(85.0, 60.0));
        diametric->setDimLinePoint(Point2D(60.0, 95.0));
        diametric->setLayerName(QStringLiteral("中心线"));
        document.addEntity(std::move(diametric));
    }

    document.endBatchLoad();

    // 把部分图元分配到非默认图层
    std::vector<EntityId> circleIds;
    std::vector<EntityId> arcIds;
    std::vector<EntityId> polylineIds;
    std::vector<EntityId> pointIds;
    std::vector<EntityId> splineIds;
    std::vector<EntityId> ellipseIds;
    for (EntityId id : document.drawOrder()) {
        switch (document.entity(id)->type()) {
        case EntityType::Circle:   circleIds.push_back(id); break;
        case EntityType::Arc:      arcIds.push_back(id); break;
        case EntityType::Polyline: polylineIds.push_back(id); break;
        case EntityType::Point:    pointIds.push_back(id); break;
        case EntityType::Spline:   splineIds.push_back(id); break;
        case EntityType::Ellipse:  ellipseIds.push_back(id); break;
        default: break;
        }
    }
    document.setEntitiesLayer(circleIds, QStringLiteral("轮廓"));
    document.setEntitiesLayer(arcIds, QStringLiteral("轮廓"));
    document.setEntitiesLayer(polylineIds, QStringLiteral("中心线"));

    // 隐藏一个图元，验证 60 组码（不可见标志）的往返保真
    document.setEntitiesVisible(pointIds, false);
}

QStringList TestDxfRoundTrip::entitySignatures(const Document& document)
{
    QStringList out;

    for (EntityId id : document.drawOrder()) {
        const Entity* entity = document.entity(id);
        if (!entity) continue;

        QString geometry;
        switch (entity->type()) {
        case EntityType::Line: {
            const auto& line = static_cast<const LineEntity&>(*entity);
            geometry = QStringLiteral("S%1;E%2")
                           .arg(formatDxfDouble(line.start().x) + QLatin1Char(',')
                                    + formatDxfDouble(line.start().y),
                                formatDxfDouble(line.end().x) + QLatin1Char(',')
                                    + formatDxfDouble(line.end().y));
            break;
        }
        case EntityType::Circle: {
            const auto& circle = static_cast<const CircleEntity&>(*entity);
            geometry = QStringLiteral("C%1;R%2")
                           .arg(formatDxfDouble(circle.center().x) + QLatin1Char(',')
                                    + formatDxfDouble(circle.center().y),
                                formatDxfDouble(circle.radius()));
            break;
        }
        case EntityType::Arc: {
            const auto& arc = static_cast<const ArcEntity&>(*entity);
            geometry = QStringLiteral("C%1;R%2;A%3-%4")
                           .arg(formatDxfDouble(arc.center().x) + QLatin1Char(',')
                                    + formatDxfDouble(arc.center().y),
                                formatDxfDouble(arc.radius()),
                                formatDxfDouble(radToDeg(arc.startAngle()), 6),
                                formatDxfDouble(radToDeg(arc.endAngle()), 6));
            break;
        }
        case EntityType::Polyline: {
            const auto& polyline = static_cast<const PolylineEntity&>(*entity);
            QStringList vertexTexts;
            for (const auto& vertex : polyline.vertices()) {
                vertexTexts << QStringLiteral("%1,%2/%3")
                                   .arg(formatDxfDouble(vertex.point.x),
                                        formatDxfDouble(vertex.point.y),
                                        formatDxfDouble(vertex.bulge));
            }
            geometry = QStringLiteral("N%1;%2;%3;W%4")
                           .arg(polyline.vertexCount())
                           .arg(polyline.isClosed() ? 1 : 0)
                           .arg(vertexTexts.join(QLatin1Char('|')),
                                formatDxfDouble(polyline.maxWidth()));
            break;
        }
        case EntityType::Point: {
            const auto& point = static_cast<const PointEntity&>(*entity);
            geometry = QStringLiteral("P%1").arg(formatDxfDouble(point.position().x)
                                                 + QLatin1Char(',')
                                                 + formatDxfDouble(point.position().y));
            break;
        }
        case EntityType::Text: {
            const auto& text = static_cast<const TextEntity&>(*entity);
            geometry = QStringLiteral("T[%1];P%2;H%3;R%4;A%5/%6;M%7;F%8")
                           .arg(text.displayString(),
                                formatDxfDouble(text.position().x) + QLatin1Char(',')
                                    + formatDxfDouble(text.position().y),
                                formatDxfDouble(text.height()),
                                formatDxfDouble(radToDeg(text.rotation()), 6))
                           .arg(static_cast<int>(text.hAlign()))
                           .arg(static_cast<int>(text.vAlign()))
                           .arg(text.isBackwards() ? 1 : 0)
                           .arg(formatDxfDouble(text.widthFactor()));
            break;
        }
        case EntityType::Hatch: {
            const auto& hatch = static_cast<const HatchEntity&>(*entity);
            QStringList vertexTexts;
            for (const Point2D& p : hatch.boundary()) {
                vertexTexts << formatDxfDouble(p.x) + QLatin1Char(',')
                               + formatDxfDouble(p.y);
            }
            geometry = QStringLiteral("N%1;%2;PT%3;S%4;K%5")
                           .arg(hatch.vertexCount())
                           .arg(vertexTexts.join(QLatin1Char('|')),
                                hatch.patternName(),
                                formatDxfDouble(hatch.patternScale()))
                           .arg(formatDxfDouble(radToDeg(hatch.patternAngle()), 6));
            break;
        }
        case EntityType::Spline:   break;
        case EntityType::Ellipse:  break;
        case EntityType::Dimension: {
            const auto& dim = static_cast<const DimensionEntity&>(*entity);
            geometry = QStringLiteral("D%1;F%2;S%3;L%4;R%5;TX[%6]")
                           .arg(static_cast<int>(dim.dimType()))
                           .arg(formatDxfDouble(dim.firstPoint().x) + QLatin1Char(',')
                                    + formatDxfDouble(dim.firstPoint().y),
                                formatDxfDouble(dim.secondPoint().x) + QLatin1Char(',')
                                    + formatDxfDouble(dim.secondPoint().y),
                                formatDxfDouble(dim.dimLinePoint().x) + QLatin1Char(',')
                                    + formatDxfDouble(dim.dimLinePoint().y),
                                formatDxfDouble(radToDeg(dim.rotation()), 6))
                           .arg(dim.textOverride());
            break;
        }
        }

        out << QStringLiteral("%1|L=%2|C=%3|V=%4|%5")
                   .arg(entityTypeName(entity->type()),
                        entity->layerName())
                   .arg(entity->colorIndex())
                   .arg(entity->isVisible() ? 1 : 0)
                   .arg(geometry);
    }

    return out;
}

QStringList TestDxfRoundTrip::layerSignatures(const Document& document, bool includeLineweight)
{
    QStringList out;
    for (const Layer& layer : document.layers().all()) {
        out << QStringLiteral("%1|c%2|lt%3|lw%4|v%5|f%6|k%7")
                   .arg(layer.name())
                   .arg(layer.colorIndex())
                   .arg(layer.linetype())
                   .arg(includeLineweight ? QString::number(layer.lineweightDxf())
                                          : QStringLiteral("-"))
                   .arg(layer.isVisible() ? 1 : 0)
                   .arg(layer.isFrozen() ? 1 : 0)
                   .arg(layer.isLocked() ? 1 : 0);
    }
    return out;
}

bool TestDxfRoundTrip::roundTrip(const QString& version, const QString& fileName,
                                 QString* failure)
{
    Document source;
    // R12 没有 HATCH 实体，导出会降级为闭合多段线，
    // 因此在该版本下不构造填充，其余图元的往返保真不受影响
    buildSampleDocument(source, version != QLatin1String("AC1009"));

    const QString path = m_dir.filePath(fileName);

    WriteOptions options;
    options.version = version;

    const IoResult writeResult = CadFile::save(path, source, options);
    if (!writeResult.ok) {
        *failure = QStringLiteral("写出失败: %1").arg(writeResult.error);
        return false;
    }

    Document loaded;
    const IoResult readResult = CadFile::load(path, loaded);
    if (!readResult.ok) {
        *failure = QStringLiteral("读回失败: %1").arg(readResult.error);
        return false;
    }

    const QStringList before = entitySignatures(source);
    const QStringList after = entitySignatures(loaded);

    if (before.size() != after.size()) {
        *failure = QStringLiteral("图元数量不一致：写出 %1，读回 %2")
                       .arg(before.size()).arg(after.size());
        return false;
    }

    for (int i = 0; i < before.size(); ++i) {
        if (before.at(i) != after.at(i)) {
            *failure = QStringLiteral("第 %1 个图元不一致：\n  原: %2\n  读: %3")
                           .arg(i).arg(before.at(i), after.at(i));
            return false;
        }
    }

    // 图层属性同样需要往返一致。R12 不支持线宽（370 组码为 R2000 引入），
    // 因此在该版本下把线宽字段排除在比对之外，而不是放宽整个断言。
    const bool hasLineweight = (version != QLatin1String("AC1009"));
    const QStringList layersBefore = layerSignatures(source, hasLineweight);
    const QStringList layersAfter = layerSignatures(loaded, hasLineweight);
    if (layersBefore != layersAfter) {
        *failure = QStringLiteral("图层表不一致：\n  原: %1\n  读: %2")
                       .arg(layersBefore.join(QLatin1Char('\n')), layersAfter.join(QLatin1Char('\n')));
        return false;
    }

    return true;
}

// -----------------------------------------------------------------------------
void TestDxfRoundTrip::roundTripR12()
{
    QString failure;
    QVERIFY2(roundTrip(QStringLiteral("AC1009"), QStringLiteral("sample_r12.dxf"), &failure),
             qPrintable(failure));
}

void TestDxfRoundTrip::roundTripR2000()
{
    QString failure;
    QVERIFY2(roundTrip(QStringLiteral("AC1015"), QStringLiteral("sample_2000.dxf"), &failure),
             qPrintable(failure));
}

// -----------------------------------------------------------------------------
void TestDxfRoundTrip::binaryDxfIsReadable()
{
    // 手工构造一段最小二进制 DXF（1 字节组码 + 按组码区间决定宽度的载荷），
    // 用于验证二进制解析路径，而不依赖外部工具生成样本。
    QByteArray bytes("AutoCAD Binary DXF\r\n\x1a\x00", 22);

    const auto writeString = [&bytes](int code, const char* text) {
        bytes.append(static_cast<char>(code));
        bytes.append(text);
        bytes.append('\0');
    };
    const auto writeReal = [&bytes](int code, double value) {
        bytes.append(static_cast<char>(code));
        char buffer[8];
        std::memcpy(buffer, &value, 8);
        bytes.append(buffer, 8);          // DXF 二进制为小端，与主流平台一致
    };

    writeString(0, "SECTION");
    writeString(2, "ENTITIES");

    writeString(0, "LINE");
    writeString(8, "BIN");
    writeReal(10, 0.0);   writeReal(20, 0.0);   writeReal(30, 0.0);
    writeReal(11, 100.0); writeReal(21, 50.0);  writeReal(31, 0.0);

    writeString(0, "CIRCLE");
    writeString(8, "BIN");
    writeReal(10, 10.0);  writeReal(20, 20.0);  writeReal(30, 0.0);
    writeReal(40, 5.0);

    writeString(0, "ENDSEC");
    writeString(0, "EOF");

    const QString path = m_dir.filePath(QStringLiteral("binary.dxf"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), static_cast<qint64>(bytes.size()));
    file.close();

    Document document;
    const IoResult result = CadFile::load(path, document);

    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(document.entityCount(), std::size_t(2));

    const Entity* first = document.entity(document.drawOrder().at(0));
    QVERIFY(first != nullptr);
    QCOMPARE(first->type(), EntityType::Line);
    QCOMPARE(first->layerName(), QStringLiteral("BIN"));
    QVERIFY(std::abs(static_cast<const LineEntity*>(first)->end().x - 100.0) < 1e-12);
    QVERIFY(std::abs(static_cast<const LineEntity*>(first)->end().y - 50.0) < 1e-12);

    const Entity* second = document.entity(document.drawOrder().at(1));
    QCOMPARE(second->type(), EntityType::Circle);
    QVERIFY(std::abs(static_cast<const CircleEntity*>(second)->radius() - 5.0) < 1e-12);

    // 图元引用了 LAYER 表中不存在的图层时必须自动补齐，而不是丢弃图元
    QVERIFY(document.layers().contains(QStringLiteral("BIN")));
}

void TestDxfRoundTrip::unsupportedEntitiesAreCountedNotDropped()
{
    // 含 MTEXT 的最小 ASCII DXF：MTEXT 不被建模，读取时会**降级**为单行文字
    // （保留内容与位置）并在警告中如实计数，且不能影响同一文件中 LINE 的导入。
    // （TEXT 自本版本起已被建模，改用仍未建模的 MTEXT 验证降级与计数。）
    const QByteArray text = R"(0
SECTION
2
ENTITIES
0
MTEXT
8
0
10
0.0
20
0.0
30
0.0
40
2.5
1
Hello
0
LINE
8
0
10
0.0
20
0.0
30
0.0
11
10.0
21
0.0
31
0.0
0
ENDSEC
0
EOF
)";

    const QString path = m_dir.filePath(QStringLiteral("with_text.dxf"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(text);
    file.close();

    Document document;
    const IoResult result = CadFile::load(path, document);

    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(document.entityCount(), std::size_t(2));

    // LINE 原样导入
    bool hasLine = false;
    // MTEXT 降级成的单行文字：内容与位置都应保留
    bool hasDegradedText = false;
    for (EntityId id : document.drawOrder()) {
        const Entity* entity = document.entity(id);
        if (!entity) continue;
        if (entity->type() == EntityType::Line) {
            hasLine = true;
        } else if (entity->type() == EntityType::Text) {
            const auto& text = static_cast<const TextEntity&>(*entity);
            hasDegradedText = text.content() == QStringLiteral("Hello")
                              && std::abs(text.position().x) < 1e-9
                              && std::abs(text.position().y) < 1e-9;
        }
    }
    QVERIFY2(hasLine, "同文件的 LINE 必须正常导入");
    QVERIFY2(hasDegradedText, "MTEXT 应降级为保留内容与位置的单行文字");

    bool reported = false;
    for (const QString& warning : result.warnings) {
        if (warning.contains(QStringLiteral("MTEXT"))) {
            reported = true;
            break;
        }
    }
    QVERIFY2(reported, "降级处理的实体类型必须出现在导入警告中");
}

void TestDxfRoundTrip::dwgSignatureIsRecognized()
{
    // DWG 版本识别不依赖任何外部后端，必须始终可用
    QCOMPARE(dwgVersionFromSignature(QByteArray("AC1015")).code, QStringLiteral("AC1015"));
    QVERIFY(dwgVersionFromSignature(QByteArray("AC1015")).recognized);
    QVERIFY(dwgVersionFromSignature(QByteArray("AC1032")).recognized);

    // 非 DWG 内容不应被误判
    QVERIFY(!dwgVersionFromSignature(QByteArray("0\nSECTION")).recognized);
    QVERIFY(!dwgVersionFromSignature(QByteArray("AC")).recognized);
}

QTEST_MAIN(TestDxfRoundTrip)
#include "test_dxf_roundtrip.moc"
