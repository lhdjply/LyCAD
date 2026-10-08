// =============================================================================
//  LyCAD - tools/sample_generator.cpp
//  开发用样例生成器。
//
//  用途：生成一份覆盖全部图元类型与典型属性的参考图纸（DXF R2000 ~ 2018 各一份，
//  外加 DWG R2000），供外部工具（AutoCAD / LibreCAD / ezdxf / ODA File Converter）
//  做互操作性校验。自动化测试只验证「自己写、自己读」的一致性，无法发现
//  「双方都不满足规范」的情况；用独立实现交叉验证才能暴露这类问题，
//  本工具正是为此提供输入。
//
//  另有一个隐含作用：它把 io 与 core 模块作为纯库来链接，
//  从而在每次构建时验证这两个模块确实不依赖任何界面代码。
//
//  输出位置：默认写入仓库的 sample/ 目录（路径由构建系统注入，
//  见 tools/CMakeLists.txt 的 LYCAD_SAMPLE_DIR），按目标版本分目录：
//      sample/2000/lycad_sample_2000.dxf      AC1015（最低的可写版本）
//      sample/2000/lycad_sample_2000.dwg      原生 dwgW 写出（AC1015）
//      sample/2004/lycad_sample_2004.dxf      AC1018
//      sample/2007/lycad_sample_2007.dxf      AC1021（起：文本按 UTF-8 存放）
//      sample/2010/lycad_sample_2010.dxf      AC1024
//      sample/2013/lycad_sample_2013.dxf      AC1027
//      sample/2018/lycad_sample_2018.dxf      AC1032
//  命令行给出目录参数时全部文件平铺写到该目录，便于临时检查。
//
//  用法：
//    lycad_sample_generator [输出目录]          生成参考 DXF / DWG
//    lycad_sample_generator --inspect <文件>    读取 DXF/DWG 并打印内容摘要
//
//  --inspect 用于反向交叉校验：由外部工具（ezdxf / AutoCAD）写出的文件，
//  交给 LyCAD 读取后打印其理解到的内容，从另一方向验证读取器。
// =============================================================================
#include "core/ArcEntity.h"
#include "core/CircleEntity.h"
#include "core/DimensionEntity.h"
#include "core/Document.h"
#include "core/EllipseEntity.h"
#include "core/Format.h"
#include "core/HatchEntity.h"
#include "core/InfiniteLineEntity.h"
#include "core/LineEntity.h"
#include "core/Linetype.h"
#include "core/PointEntity.h"
#include "core/PolylineEntity.h"
#include "core/RayEntity.h"
#include "core/SplineEntity.h"
#include "core/TextEntity.h"
#include "core/XLineEntity.h"
#include "io/CadFile.h"
#include "io/LibDxfRwAdapter.h"

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

    // 椭圆：整椭圆随层颜色；椭圆弧带参数范围与显式颜色。
    // 注意：ELLIPSE 是 R13+ 实体，R12 目标下写出时会降级为多段线，由导出警告说明。
    {
        auto ellipse = std::make_unique<EllipseEntity>(Point2D(150.0, 92.0),
                                                        Point2D(16.0, 0.0),
                                                        0.55, 0.0, degToRad(360.0));
        ellipse->setLayerName(QStringLiteral("轮廓"));
        document.addEntity(std::move(ellipse));

        auto ellipticArc = std::make_unique<EllipseEntity>(Point2D(163.0, 30.0),
                                                            Point2D(11.0, 0.0),
                                                            0.6, 0.0, degToRad(270.0));
        ellipticArc->setLayerName(QStringLiteral("轮廓"));
        ellipticArc->setColorIndex(3);
        document.addEntity(std::move(ellipticArc));
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

    // 样条曲线：三次 B 样条，夹持节点向量（首末节点重复度 = 阶数 + 1）。
    // 注意：SPLINE 也是 R13+ 实体，R12 目标下写出时降级为多段线。
    {
        auto spline = std::make_unique<SplineEntity>(
            3,
            std::vector<Point2D>{ Point2D(12.0, 86.0), Point2D(26.0, 102.0),
                                  Point2D(40.0, 78.0), Point2D(52.0, 96.0) },
            std::vector<double>{ 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0 });
        spline->setLayerName(QStringLiteral("轮廓"));
        document.addEntity(std::move(spline));
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

    // 无界直线：构造线（双向）与射线（单向）。
    // 注意：样例同时面向外部工具互操作校验——这两类实体是 R13+ 的，
    // R12 目标下写出时会降级为覆盖全图范围的直线，由导出警告说明。
    {
        auto xline = std::make_unique<XLineEntity>(Point2D(30.0, 30.0), Vector2D(1.0, 0.0));
        xline->setLayerName(QStringLiteral("中心线"));
        document.addEntity(std::move(xline));

        auto ray = std::make_unique<RayEntity>(Point2D(60.0, 90.0), Vector2D(1.0, 1.0));
        ray->setLayerName(QStringLiteral("中心线"));
        document.addEntity(std::move(ray));
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

/// 读取并打印文件摘要，用于反向交叉校验
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

    const Box2D extents = document.extents();
    out() << QStringLiteral("范围: ")
          << (extents.isValid()
                  ? QStringLiteral("x[%1, %2] y[%3, %4]")
                        .arg(formatLength(extents.minCorner().x, 2),
                             formatLength(extents.maxCorner().x, 2),
                             formatLength(extents.minCorner().y, 2),
                             formatLength(extents.maxCorner().y, 2))
                  : QStringLiteral("无效（无可显示图元或包围盒异常）"))
          << Qt::endl;

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
    if (arguments.size() >= 4 && arguments.at(1) == QLatin1String("--convert")) {
        Document document;
        const IoResult loaded = CadFile::load(arguments.at(2), document);
        if (!loaded.ok) {
            out() << QStringLiteral("读取失败: %1").arg(loaded.error) << Qt::endl;
            return 1;
        }
        const QString version = (arguments.size() > 4) ? arguments.at(4)
                                                       : QStringLiteral("AC1015");
        // 按目标扩展名路由编解码器（.dwg 走 DwgCodec 的原生 dwgW 写出），
        // 不能直接调 writeDxfWithLibDxfRw——否则 .dwg 里装的是 DXF 文本，
        // 再打开时会被文件头签名检查拦下（提示"不具备 DWG 的文件头签名"）。
        WriteOptions options;
        options.version = version;
        const IoResult written = CadFile::save(arguments.at(3), document, options);
        out() << QStringLiteral("转换 %1 → %2 (%3)：图元 %4，%5")
                     .arg(arguments.at(2), arguments.at(3), version)
                     .arg(document.entityCount())
                     .arg(written.ok ? QStringLiteral("成功") : QStringLiteral("失败"))
              << Qt::endl;
        for (const QString& w : written.warnings) {
            out() << QStringLiteral("  · %1").arg(w) << Qt::endl;
        }
        return written.ok ? 0 : 1;
    }

    // 默认写入仓库的 sample/ 目录（构建期注入），按目标版本分目录；
    // 显式给出目录参数时全部文件平铺写到那里，便于临时检查。
    const QString defaultDir = QStringLiteral(LYCAD_SAMPLE_DIR);
    const QString outputDir = (arguments.size() > 1) ? arguments.at(1) : defaultDir;

    QDir baseDir(outputDir);
    if (!baseDir.exists() && !baseDir.mkpath(QStringLiteral("."))) {
        out() << QStringLiteral("无法创建输出目录: ") << outputDir << Qt::endl;
        return 2;
    }

    // 目标版本矩阵：目录名（同时也是文件名里的年代）-> DXF 版本代号。
    // 每个年代出一个文件，便于直接用对应版本的 CAD 打开核对。
    struct Target { const char* year; const char* version; };
    static const Target kTargets[] = {
        { "2000", "AC1015" },     // AutoCAD 2000
        { "2004", "AC1018" },     // AutoCAD 2004
        { "2007", "AC1021" },     // AutoCAD 2007
        { "2010", "AC1024" },     // AutoCAD 2010
        { "2013", "AC1027" },     // AutoCAD 2013
        { "2018", "AC1032" },     // AutoCAD 2018
    };

    const bool flat = arguments.size() > 1;
    const auto dirOf = [&](const Target& target) {
        return flat ? outputDir
                    : QStringLiteral("%1/%2").arg(outputDir, QLatin1String(target.year));
    };

    for (const Target& target : kTargets) {
        const QString dir = dirOf(target);
        QDir d(dir);
        if (!d.exists() && !d.mkpath(QStringLiteral("."))) {
            out() << QStringLiteral("无法创建输出目录: ") << dir << Qt::endl;
            return 2;
        }
    }

    Document document;
    buildSample(document);

    out() << QStringLiteral("LyCAD 样例生成") << Qt::endl;
    out() << QStringLiteral("输出目录: %1").arg(QDir(outputDir).absolutePath()) << Qt::endl;
    out() << QStringLiteral("图元总数: %1，图层数: %2")
                 .arg(document.entityCount())
                 .arg(document.layers().count())
          << Qt::endl;

    int failures = 0;
    for (const Target& target : kTargets) {
        failures += writeOne(document,
                             dirOf(target) + QStringLiteral("/lycad_sample_%1.dxf")
                                                 .arg(QLatin1String(target.year)),
                             QString::fromLatin1(target.version));
    }
    // DWG：走内置 dwgW 原生写出（目标版本恒为 R2000），与 DXF 同内容，
    // 供「同一张图两种格式」的交叉比对。只出 R2000 一份——
    // DWG 的写出版本不随 DXF 的版本矩阵变化，重复 6 份没有意义。
    failures += writeOne(document,
                         dirOf(kTargets[0]) + QStringLiteral("/lycad_sample_2000.dwg"),
                         QStringLiteral("AC1015"));

    if (failures == 0) {
        out() << QStringLiteral("完成。") << Qt::endl;
    }
    return failures == 0 ? 0 : 1;
}
