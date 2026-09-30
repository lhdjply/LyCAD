// =============================================================================
//  LyCAD - tools/dxf_sample_generator.cpp
//  开发用样例生成器。
//
//  用途：生成一份覆盖全部图元类型与典型属性的 DXF，供外部工具
//  （AutoCAD / LibreCAD / ezdxf / ODA File Converter）做互操作性校验。
//  自动化测试只验证「自己写、自己读」的一致性，无法发现「双方都不满足规范」
//  的情况；用独立实现交叉验证才能暴露这类问题，本工具正是为此提供输入。
//
//  另有一个隐含作用：它把 io 与 core 模块作为纯库来链接，
//  从而在每次构建时验证这两个模块确实不依赖任何界面代码。
//
//  用法：
//    lycad_sample <输出目录>        生成两份参考 DXF
//    lycad_sample --inspect <文件>  读取 DXF 并打印内容摘要
//
//  --inspect 用于反向交叉校验：由外部工具（ezdxf / AutoCAD）写出的文件，
//  交给 LyCAD 读取后打印其理解到的内容，从另一方向验证读取器。
// =============================================================================
#include "core/ArcEntity.h"
#include "core/CircleEntity.h"
#include "core/DimensionEntity.h"
#include "core/Document.h"
#include "core/HatchEntity.h"
#include "core/LineEntity.h"
#include "core/Linetype.h"
#include "core/PointEntity.h"
#include "core/PolylineEntity.h"
#include "core/TextEntity.h"
#include "io/CadFile.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>

#include <cmath>
#include <memory>
#include <vector>

using namespace lycad;

namespace {

QTextStream& out()
{
    static QTextStream stream(stdout);
    return stream;
}

/// 构造样例文档：图层、线型、颜色、线宽、以及全部八类图元
void buildSample(Document& document)
{
    document.clear(true);

    QString error;
    document.addLayer(Layer(QStringLiteral("轮廓")), &error);
    document.addLayer(Layer(QStringLiteral("中心线")), &error);
    document.addLayer(Layer(QStringLiteral("标注")), &error);

    document.layers().ensure(QStringLiteral("轮廓")).setColorIndex(1);
    document.layers().ensure(QStringLiteral("轮廓")).setLineweightDxf(35);
    document.layers().ensure(QStringLiteral("中心线")).setColorIndex(4);
    document.layers().ensure(QStringLiteral("中心线")).setLinetype(QStringLiteral("CENTER"));
    document.layers().ensure(QStringLiteral("标注")).setColorIndex(2);
    document.layers().ensure(QStringLiteral("标注")).setLinetype(QStringLiteral("DASHED"));
    document.setCurrentLayer(QStringLiteral("轮廓"));

    document.beginBatchLoad();

    // 外框：闭合矩形
    {
        auto frame = std::make_unique<PolylineEntity>(
            PolylineEntity::makeRectangle(Point2D(0.0, 0.0), Point2D(180.0, 110.0)));
        frame->setLayerName(QStringLiteral("轮廓"));
        document.addEntity(std::move(frame));
    }

    // 直线族：带显式颜色
    {
        auto line = std::make_unique<LineEntity>(Point2D(10.0, 10.0), Point2D(170.0, 100.0));
        line->setColorIndex(3);
        document.addEntity(std::move(line));
    }
    {
        auto line = std::make_unique<LineEntity>(Point2D(10.0, 100.0), Point2D(170.0, 10.0));
        document.addEntity(std::move(line));
    }

    // 中心线：十字
    {
        auto horizontal = std::make_unique<LineEntity>(Point2D(0.0, 55.0), Point2D(180.0, 55.0));
        horizontal->setLayerName(QStringLiteral("中心线"));
        document.addEntity(std::move(horizontal));

        auto vertical = std::make_unique<LineEntity>(Point2D(90.0, 0.0), Point2D(90.0, 110.0));
        vertical->setLayerName(QStringLiteral("中心线"));
        document.addEntity(std::move(vertical));
    }

    // 圆：随层颜色
    {
        auto circle = std::make_unique<CircleEntity>(Point2D(60.0, 55.0), 25.0);
        circle->setLayerName(QStringLiteral("轮廓"));
        document.addEntity(std::move(circle));
    }

    // 圆弧：逆时针自 30° 扫到 250°
    {
        auto arc = std::make_unique<ArcEntity>(Point2D(120.0, 55.0), 25.0,
                                               degToRad(30.0), degToRad(250.0));
        arc->setLayerName(QStringLiteral("轮廓"));
        document.addEntity(std::move(arc));
    }

    // 多段线：含两个不同方向的圆弧段（凸度），闭合
    {
        auto polyline = std::make_unique<PolylineEntity>();
        polyline->appendVertex(Point2D(20.0, 20.0), 0.6);      // 逆时针弧段
        polyline->appendVertex(Point2D(70.0, 20.0), -0.4);     // 顺时针弧段
        polyline->appendVertex(Point2D(70.0, 45.0), 0.0);
        polyline->appendVertex(Point2D(20.0, 45.0), 0.0);
        polyline->setClosed(true);
        polyline->setLayerName(QStringLiteral("轮廓"));
        document.addEntity(std::move(polyline));
    }

    // 带宽度多段线：模拟 PCB 走线（DXF 43 恒定宽度）
    {
        auto track = std::make_unique<PolylineEntity>();
        track->appendVertex(Point2D(150.0, 12.0), 0.0);
        track->appendVertex(Point2D(150.0, 60.0), 0.0);
        track->appendVertex(Point2D(168.0, 78.0), 0.0);
        track->setConstantWidth(2.0);
        track->setLayerName(QStringLiteral("轮廓"));
        document.addEntity(std::move(track));
    }

    // 点
    {
        auto point = std::make_unique<PointEntity>(Point2D(90.0, 55.0));
        point->setLayerName(QStringLiteral("标注"));
        document.addEntity(std::move(point));
    }

    // 一个隐藏图元，验证 60 组码
    {
        auto hidden = std::make_unique<CircleEntity>(Point2D(90.0, 55.0), 6.0);
        hidden->setLayerName(QStringLiteral("标注"));
        hidden->setVisible(false);
        document.addEntity(std::move(hidden));
    }

    // 文字：默认对齐 + 居中对齐，内容含中文与数字
    {
        auto text = std::make_unique<TextEntity>(QStringLiteral("LyCAD 样例图纸"),
                                                 Point2D(10.0, 104.0), 6.0);
        text->setLayerName(QStringLiteral("标注"));
        document.addEntity(std::move(text));

        auto centered = std::make_unique<TextEntity>(QStringLiteral("直径 50"),
                                                     Point2D(60.0, 55.0), 4.0);
        centered->setLayerName(QStringLiteral("标注"));
        centered->setHAlign(TextEntity::HAlign::Center);
        centered->setVAlign(TextEntity::VAlign::Middle);
        centered->setAlignmentPoint(Point2D(60.0, 55.0));
        centered->setBackwards(true);          // 背面文字（DXF 71 bit2）
        centered->setWidthFactor(1.1);         // 宽度因子（DXF 41）
        document.addEntity(std::move(centered));
    }

    // 填充：五边形边界，ANSI31 图案
    {
        HatchEntity hatch({ Point2D(100.0, 15.0), Point2D(130.0, 15.0),
                            Point2D(140.0, 38.0), Point2D(115.0, 52.0),
                            Point2D(92.0, 38.0) },
                          QStringLiteral("ANSI31"), 1.5, 0.0);
        hatch.setLayerName(QStringLiteral("轮廓"));
        document.addEntity(std::move(hatch.clone()));
    }

    // 尺寸标注：对齐 / 线性 / 半径 / 直径 各一个
    {
        auto aligned = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Aligned);
        aligned->setFirstPoint(Point2D(0.0, 0.0));
        aligned->setSecondPoint(Point2D(180.0, 0.0));
        aligned->setDimLinePoint(Point2D(90.0, -18.0));
        aligned->setLayerName(QStringLiteral("标注"));
        document.addEntity(std::move(aligned));

        auto linear = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Linear);
        linear->setFirstPoint(Point2D(180.0, 0.0));
        linear->setSecondPoint(Point2D(180.0, 110.0));
        linear->setDimLinePoint(Point2D(198.0, 55.0));
        linear->setRotation(degToRad(90.0));        // 垂直方向测量
        linear->setLayerName(QStringLiteral("标注"));
        document.addEntity(std::move(linear));

        auto radial = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Radial);
        radial->setFirstPoint(Point2D(60.0, 55.0));          // 圆心
        radial->setSecondPoint(Point2D(60.0 + 25.0 * std::cos(degToRad(45.0)),
                                       55.0 + 25.0 * std::sin(degToRad(45.0))));
        radial->setDimLinePoint(Point2D(96.0, 84.0));        // 文字位置
        radial->setLayerName(QStringLiteral("标注"));
        document.addEntity(std::move(radial));

        auto diametric = std::make_unique<DimensionEntity>(DimensionEntity::DimType::Diametric);
        const Point2D circleCenter(120.0, 55.0);
        const double arcRadius = 25.0;
        diametric->setFirstPoint(circleCenter + Vector2D(-arcRadius, 0.0));
        diametric->setSecondPoint(circleCenter + Vector2D(arcRadius, 0.0));
        diametric->setDimLinePoint(Point2D(120.0, 88.0));    // 文字位置
        diametric->setLayerName(QStringLiteral("标注"));
        document.addEntity(std::move(diametric));
    }

    document.endBatchLoad();
}

int writeOne(const Document& document, const QString& path, const QString& version)
{
    WriteOptions options;
    options.version = version;

    const IoResult result = CadFile::save(path, document, options);

    if (!result.ok) {
        out() << QStringLiteral("  失败: ") << result.error << Qt::endl;
        return 1;
    }

    out() << QStringLiteral("  已生成 %1  (%2, %3 字节, %4 个图元)")
                 .arg(QFileInfo(path).fileName(),
                      WriteOptions::versionDisplayName(version))
                 .arg(QFileInfo(path).size())
                 .arg(result.entityCount)
          << Qt::endl;

    for (const QString& warning : result.warnings) {
        out() << QStringLiteral("    · ") << warning << Qt::endl;
    }
    return 0;
}

/// 读取并打印 DXF 摘要，用于反向交叉校验
int inspect(const QString& path)
{
    Document document;
    const IoResult result = CadFile::load(path, document);

    out() << QStringLiteral("文件: ") << path << Qt::endl;
    out() << QStringLiteral("结果: ") << (result.ok ? QStringLiteral("成功") : QStringLiteral("失败"))
          << Qt::endl;

    if (!result.ok) {
        out() << QStringLiteral("错误: ") << result.error << Qt::endl;
        return 1;
    }

    out() << QStringLiteral("图元数: ") << document.entityCount() << Qt::endl;
    out() << QStringLiteral("图层数: ") << document.layers().count() << Qt::endl;

    out() << QStringLiteral("图层: ") << Qt::endl;
    for (const Layer& layer : document.layers().all()) {
        out() << QStringLiteral("  - %1  color=%2  linetype=%3  visible=%4")
                     .arg(layer.name())
                     .arg(layer.colorIndex())
                     .arg(layer.linetype())
                     .arg(layer.isVisible() ? 1 : 0)
              << Qt::endl;
    }

    out() << QStringLiteral("图元: ") << Qt::endl;
    for (EntityId id : document.drawOrder()) {
        const Entity* entity = document.entity(id);
        if (!entity) continue;
        out() << QStringLiteral("  - %1").arg(entity->describe()) << Qt::endl;

        // 类型专属字段：文字内容 / 标注测量值，供脚本比对
        QString extra;
        if (entity->type() == EntityType::Text) {
            extra = QStringLiteral("|text=%1")
                        .arg(static_cast<const TextEntity*>(entity)->displayString());
        } else if (entity->type() == EntityType::Dimension) {
            extra = QStringLiteral("|dim=%1")
                        .arg(static_cast<const DimensionEntity*>(entity)->displayText());
        }

        // 数值化输出便于脚本比对
        out() << QStringLiteral("    raw|type=%1|layer=%2|color=%3|visible=%4|len=%5%6")
                     .arg(entityTypeName(entity->type()),
                          entity->layerName())
                     .arg(entity->colorIndex())
                     .arg(entity->isVisible() ? 1 : 0)
                     .arg(entity->length(), 0, 'f', 6)
                     .arg(extra)
              << Qt::endl;
    }

    if (result.hasWarnings()) {
        out() << QStringLiteral("提示:") << Qt::endl;
        for (const QString& warning : result.warnings) {
            out() << QStringLiteral("  · ") << warning << Qt::endl;
        }
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    registerBuiltinCodecs();

    const QStringList arguments = application.arguments();

    if (arguments.size() >= 3 && arguments.at(1) == QLatin1String("--inspect")) {
        return inspect(arguments.at(2));
    }

    const QString outputDir = (arguments.size() > 1) ? arguments.at(1) : QStringLiteral(".");

    QDir dir(outputDir);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        out() << QStringLiteral("无法创建输出目录: ") << outputDir << Qt::endl;
        return 2;
    }

    Document document;
    buildSample(document);

    out() << QStringLiteral("LyCAD DXF 样例生成") << Qt::endl;
    out() << QStringLiteral("图元总数: %1，图层数: %2")
                 .arg(document.entityCount())
                 .arg(document.layers().count())
          << Qt::endl;

    int failures = 0;
    failures += writeOne(document, dir.filePath(QStringLiteral("lycad_sample_r12.dxf")),
                         QStringLiteral("AC1009"));
    failures += writeOne(document, dir.filePath(QStringLiteral("lycad_sample_2000.dxf")),
                         QStringLiteral("AC1015"));

    if (failures == 0) {
        out() << QStringLiteral("完成。") << Qt::endl;
    }
    return failures == 0 ? 0 : 1;
}
