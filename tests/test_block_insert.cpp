// =============================================================================
//  LyCAD - tests/test_block_insert.cpp
//  块参照（块定义 + INSERT）的单元测试。
//
//  覆盖三块内容
//    1. 模型：块表查找、插入变换、展开（图层继承 / 随块颜色 / 嵌套 / 属性）、
//       非相似变换下的烘焙、分解命令的撤销；
//    2. 文件往返：DXF 各版本写出后读回，块定义与 INSERT 必须原样还在；
//    3. 坏数据：引用了未定义块的 INSERT 必须被计数上报而不是静默消失。
// =============================================================================
#include "core/Block.h"
#include "core/CircleEntity.h"
#include "core/Document.h"
#include "core/InsertEntity.h"
#include "core/LineEntity.h"
#include "core/TextEntity.h"
#include "io/CadFile.h"
#include "render/CanvasTypes.h"
#include "render/EntityRenderer.h"
#include "render/ViewTransform.h"

#include <QFile>
#include <QImage>
#include <QPainter>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <memory>

using namespace lycad;

namespace {

constexpr double kTol = 1e-9;

/// 造一个含圆与直线的块定义："0" 层的圆 + "B" 层的直线
std::shared_ptr<BlockDefinition> makeBlock(const QString& name)
{
    auto block = std::make_shared<BlockDefinition>(name);
    block->setBasePoint(Point2D(0.0, 0.0));

    auto circle = std::make_unique<CircleEntity>(Point2D(0.0, 0.0), 3.0);
    circle->setLayerName(QStringLiteral("0"));
    block->addEntity(std::move(circle));

    auto line = std::make_unique<LineEntity>(Point2D(0.0, 0.0), Point2D(10.0, 0.0));
    line->setLayerName(QStringLiteral("B"));
    block->addEntity(std::move(line));
    return block;
}

/// 在文档里放一个引用该块的块参照，并返回它的 id
EntityId addInsert(Document& document, const std::shared_ptr<BlockDefinition>& block,
                   const Point2D& point, double sx, double sy, double rotationRad = 0.0)
{
    auto insert = std::make_unique<InsertEntity>(block);
    insert->setInsertionPoint(point);
    insert->setScale(sx, sy);
    insert->setRotation(rotationRad);
    insert->setLayerName(QStringLiteral("0"));
    return document.addEntity(std::move(insert));
}

const InsertEntity* insertAt(const Document& document, EntityId id)
{
    const Entity* entity = document.entity(id);
    if (!entity || entity->type() != EntityType::Insert) return nullptr;
    return static_cast<const InsertEntity*>(entity);
}

/// 展开结果里第一个指定类型的图元（找不到返回 nullptr）
template <typename T>
const T* firstExpanded(const InsertEntity& insert)
{
    for (const std::unique_ptr<Entity>& child : insert.expanded()) {
        if (const auto* typed = dynamic_cast<const T*>(child.get())) return typed;
    }
    return nullptr;
}

/// 离屏渲染一张图。返回 (墨迹像素数, 图像)；path 非空时同时落盘，
/// 便于人工看一眼「块参照到底画成了什么样」。
std::pair<int, QImage> renderDocument(const Document& document, const QSize& size,
                                      const QString& path = QString())
{
    const Box2D worldBox = document.extents();
    if (!worldBox.isValid()) return { 0, QImage() };

    ViewTransform view;
    view.fitTo(worldBox, size, 0.1);

    DisplaySettings settings;
    settings.showLineweight = false;
    settings.themeIsDark = false;

    EntityRenderer::DrawContext context;
    context.view = &view;
    context.settings = &settings;
    context.viewport = QRectF(0.0, 0.0, size.width(), size.height());
    context.layers = &document.layers();

    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);

    for (EntityId id : document.drawOrder()) {
        const Entity* entity = document.entity(id);
        if (!entity || !entity->isDisplayable(document.layers())) continue;
        const QColor color = entity->resolvedColor(document.layers(), false);
        const double lineweight = entity->resolvedLineweight(document.layers());
        QString linetype = entity->linetypeName();
        if (linetype.isEmpty()) {
            linetype = document.layers().linetypeOf(entity->layerName(),
                                                    QStringLiteral("CONTINUOUS"));
        }
        EntityRenderer::draw(painter, *entity, context, color, lineweight, linetype);
    }
    painter.end();

    int ink = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            if (qRed(pixel) < 200 || qGreen(pixel) < 200 || qBlue(pixel) < 200) ++ink;
        }
    }
    if (!path.isEmpty()) image.save(path);
    return { ink, image };
}

} // namespace

class TestBlockInsert : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void blockTableLookupIsCaseInsensitive();
    void insertTransformsBlockContent();
    void layerZeroInheritsInsertLayer();
    void byBlockColorFollowsInsert();
    void nestedBlockExpandsRecursively();
    void attributesSyncFromDefinitions();
    void attributesFollowInsertEdits();
    void nonSimilarityTransformKeepsGeometry();
    void explodeReplacesInsertInOneUndoStep();
    void rendersInsertIntoPixels();

    void dxfRoundTripPreservesBlockAndInsert();
    void dxfRoundTripAllVersions();
    void dwgRoundTripKeepsBlockReference();
    void missingBlockIsReported();
    void circularBlockReferenceIsBroken();

    void realSampleIfProvided();
    void dwgAndDxfAgreeOnBlocks();

private:
    QTemporaryDir m_dir;
};

void TestBlockInsert::initTestCase()
{
    QVERIFY2(m_dir.isValid(), "无法创建临时目录，测试无法继续");
    registerBuiltinCodecs();
}

// -----------------------------------------------------------------------------
//  模型
// -----------------------------------------------------------------------------
void TestBlockInsert::blockTableLookupIsCaseInsensitive()
{
    BlockTable table;
    table.add(makeBlock(QStringLiteral("Bloko")));

    QVERIFY(table.contains(QStringLiteral("BLOKO")));
    QVERIFY(table.contains(QStringLiteral("bloko")));
    QVERIFY(!table.contains(QStringLiteral("other")));

    // 大小写不敏感地替换，而不是并存两条
    table.add(makeBlock(QStringLiteral("BLOKO")));
    QCOMPARE(table.size(), std::size_t(1));
    QCOMPARE(table.find(QStringLiteral("bloko"))->name(), QStringLiteral("BLOKO"));

    // 重名时 uniqueName 给出不冲突的新名
    QCOMPARE(table.uniqueName(QStringLiteral("BLOKO")), QStringLiteral("BLOKO2"));
    QCOMPARE(table.uniqueName(QStringLiteral("fresh")), QStringLiteral("fresh"));
}

void TestBlockInsert::insertTransformsBlockContent()
{
    Document document;
    const auto block = makeBlock(QStringLiteral("B1"));
    document.blocks().add(block);

    // 插入点 (10,20)、等比放大 2 倍、旋转 90°
    const EntityId id = addInsert(document, block, Point2D(10.0, 20.0), 2.0, 2.0, kHalfPi);

    const InsertEntity* insert = insertAt(document, id);
    QVERIFY(insert != nullptr);

    const auto* circle = firstExpanded<CircleEntity>(*insert);
    QVERIFY(circle != nullptr);
    QVERIFY(circle->center().distanceTo(Point2D(10.0, 20.0)) < kTol);
    QVERIFY(std::abs(circle->radius() - 6.0) < kTol);

    // 直线原为 (0,0)-(10,0)：旋转 90° 后沿 +Y，再放大 2 倍、平移到插入点
    const auto* line = firstExpanded<LineEntity>(*insert);
    QVERIFY(line != nullptr);
    QVERIFY(line->start().distanceTo(Point2D(10.0, 20.0)) < kTol);
    QVERIFY(line->end().distanceTo(Point2D(10.0, 40.0)) < kTol);

    // 包围盒必须罩住全部内容（渲染剔除与范围缩放都靠它）
    const Box2D box = insert->boundingBox();
    QVERIFY(box.isValid());
    QVERIFY(box.contains(Point2D(10.0, 40.0), 1e-6));
    QVERIFY(box.maxX >= 16.0 - kTol);
}

void TestBlockInsert::layerZeroInheritsInsertLayer()
{
    Document document;
    document.layers().ensure(QStringLiteral("KLUJO"));

    const auto block = makeBlock(QStringLiteral("B1"));
    document.blocks().add(block);

    auto insert = std::make_unique<InsertEntity>(block);
    insert->setLayerName(QStringLiteral("KLUJO"));
    insert->setInsertionPoint(Point2D(0.0, 0.0));
    const EntityId id = document.addEntity(std::move(insert));

    const InsertEntity* stored = insertAt(document, id);
    QVERIFY(stored != nullptr);

    const auto* circle = firstExpanded<CircleEntity>(*stored);
    QVERIFY(circle != nullptr);
    // 块内 "0" 层的图元继承块参照所在图层
    QCOMPARE(circle->layerName(), QStringLiteral("KLUJO"));

    const auto* line = firstExpanded<LineEntity>(*stored);
    QVERIFY(line != nullptr);
    // 块内有明确图层的图元保持原样
    QCOMPARE(line->layerName(), QStringLiteral("B"));
}

void TestBlockInsert::byBlockColorFollowsInsert()
{
    Document document;
    auto block = std::make_shared<BlockDefinition>(QStringLiteral("B1"));

    auto circle = std::make_unique<CircleEntity>(Point2D(0.0, 0.0), 1.0);
    circle->setColorByBlock();                     // DXF 颜色 0 = 随块
    block->addEntity(std::move(circle));

    auto line = std::make_unique<LineEntity>(Point2D(0.0, 0.0), Point2D(1.0, 0.0));
    line->setColorByLayer();
    block->addEntity(std::move(line));

    document.blocks().add(block);

    auto insert = std::make_unique<InsertEntity>(block);
    insert->setColorIndex(3);                      // 块参照显式取绿色
    const EntityId id = document.addEntity(std::move(insert));

    const InsertEntity* stored = insertAt(document, id);
    QVERIFY(stored != nullptr);

    const auto* expandedCircle = firstExpanded<CircleEntity>(*stored);
    QVERIFY(expandedCircle != nullptr);
    QCOMPARE(expandedCircle->colorIndex(), 3);     // 随块 -> 取块参照的颜色

    const auto* expandedLine = firstExpanded<LineEntity>(*stored);
    QVERIFY(expandedLine != nullptr);
    QVERIFY(expandedLine->isColorByLayer());       // 随层 -> 保持随层
}

void TestBlockInsert::nestedBlockExpandsRecursively()
{
    Document document;

    auto inner = std::make_shared<BlockDefinition>(QStringLiteral("INNER"));
    auto innerCircle = std::make_unique<CircleEntity>(Point2D(0.0, 0.0), 2.0);
    innerCircle->setLayerName(QStringLiteral("0"));
    inner->addEntity(std::move(innerCircle));
    document.blocks().add(inner);

    auto outer = std::make_shared<BlockDefinition>(QStringLiteral("OUTER"));
    auto nested = std::make_unique<InsertEntity>(inner);
    nested->setScale(0.5, 0.5);                    // 内层再缩一半
    nested->setLayerName(QStringLiteral("0"));
    outer->addEntity(std::move(nested));
    document.blocks().add(outer);

    // 外层 3 倍 -> 内层 0.5 倍 => 半径 2 × 3 × 0.5 = 3
    auto insert = std::make_unique<InsertEntity>(outer);
    insert->setInsertionPoint(Point2D(100.0, 0.0));
    insert->setScale(3.0, 3.0);
    insert->setLayerName(QStringLiteral("NESTED"));
    const EntityId id = document.addEntity(std::move(insert));

    const InsertEntity* stored = insertAt(document, id);
    QVERIFY(stored != nullptr);

    // 嵌套在展开阶段解析完毕：结果里不应再出现块参照
    for (const std::unique_ptr<Entity>& child : stored->expanded()) {
        QVERIFY(child->type() != EntityType::Insert);
    }

    const auto* circle = firstExpanded<CircleEntity>(*stored);
    QVERIFY(circle != nullptr);
    QVERIFY(circle->center().distanceTo(Point2D(100.0, 0.0)) < kTol);
    QVERIFY(std::abs(circle->radius() - 3.0) < kTol);
    QCOMPARE(circle->layerName(), QStringLiteral("NESTED"));
}

void TestBlockInsert::attributesSyncFromDefinitions()
{
    Document document;
    auto block = std::make_shared<BlockDefinition>(QStringLiteral("B1"));

    AttributeDefinition definition;
    definition.tag = QStringLiteral("ETIKEDO");
    definition.prompt = QStringLiteral("Etikedo");
    definition.defaultValue = QStringLiteral("valoro");
    definition.position = Point2D(10.0, 0.0);
    definition.height = 2.0;
    block->addAttributeDefinition(definition);
    document.blocks().add(block);

    const EntityId id = addInsert(document, block, Point2D(0.0, 0.0), 1.0, 1.0);
    const InsertEntity* insert = insertAt(document, id);
    QVERIFY(insert != nullptr);

    QCOMPARE(insert->attributes().size(), std::size_t(1));
    QCOMPARE(insert->attributes().front().tag, QStringLiteral("ETIKEDO"));
    QCOMPARE(insert->attributes().front().value, QStringLiteral("valoro"));
    QVERIFY(insert->attributes().front().position.distanceTo(Point2D(10.0, 0.0)) < kTol);

    // 属性文字参与展开：块参照里能看见它
    const auto* text = firstExpanded<TextEntity>(*insert);
    QVERIFY(text != nullptr);
    QCOMPARE(text->content(), QStringLiteral("valoro"));
    QVERIFY(std::abs(text->height() - 2.0) < kTol);
}

void TestBlockInsert::attributesFollowInsertEdits()
{
    // ATTRIB 的坐标按 DXF 规范是世界坐标，块参照改了插入点/比例/旋转，
    // 属性必须跟着走——否则改完参数，属性文字会留在原地。
    Document document;
    auto block = std::make_shared<BlockDefinition>(QStringLiteral("B1"));

    AttributeDefinition definition;
    definition.tag = QStringLiteral("T");
    definition.defaultValue = QStringLiteral("v");
    definition.position = Point2D(10.0, 0.0);
    definition.height = 2.0;
    block->addAttributeDefinition(definition);
    document.blocks().add(block);

    auto insert = std::make_unique<InsertEntity>(block);
    const EntityId id = document.addEntity(std::move(insert));
    const Entity* raw = document.entity(id);
    QVERIFY(raw != nullptr);

    // 移动：属性平移同样的量
    document.transformEntities({ id }, Transform2D::translation(5.0, 1.0),
                               QStringLiteral("移动"));
    const auto* moved = static_cast<const InsertEntity*>(document.entity(id));
    QVERIFY(moved->attributes().front().position.distanceTo(Point2D(15.0, 1.0)) < 1e-9);

    // 缩放 2 倍（插入点不动）：相对插入点的位移同样翻倍
    document.transformEntities({ id }, Transform2D::scalingAround(Point2D(5.0, 1.0), 2.0, 2.0),
                               QStringLiteral("缩放"));
    const auto* scaled = static_cast<const InsertEntity*>(document.entity(id));
    QVERIFY(scaled->attributes().front().position.distanceTo(Point2D(25.0, 1.0)) < 1e-9);
    QVERIFY(std::abs(scaled->attributes().front().height - 4.0) < 1e-9);

    // 局部编辑同理：直接改比例与旋转，属性也要跟到同一个位置
    Document second;
    second.blocks().add(block);
    auto direct = std::make_unique<InsertEntity>(block);
    direct->setInsertionPoint(Point2D(1.0, 2.0));
    direct->setScale(3.0, 3.0);
    direct->setRotation(degToRad(90.0));
    const InsertEntity* stored = direct.get();
    second.addEntity(std::move(direct));

    // 与「先定参数、后按定义生成属性」的结果一致
    const Attribute& attribute = stored->attributes().front();
    const Point2D expected = stored->blockToParentTransform().apply(Point2D(10.0, 0.0));
    QVERIFY2(attribute.position.distanceTo(expected) < 1e-9,
             qPrintable(QStringLiteral("属性位置 %1,%2，期望 %3,%4")
                            .arg(attribute.position.x).arg(attribute.position.y)
                            .arg(expected.x).arg(expected.y)));
    QVERIFY(std::abs(attribute.height - 6.0) < 1e-9);   // 字高 2 × 比例 3
}

void TestBlockInsert::nonSimilarityTransformKeepsGeometry()
{
    Document document;
    const auto block = makeBlock(QStringLiteral("B1"));
    document.blocks().add(block);

    // 非等比比例的块参照：旋转 30° 之后再被非等比拉伸，结果含错切，
    // 无法用 (插入点, 比例, 旋转) 表达，必须走烘焙路径。
    auto insert = std::make_unique<InsertEntity>(block);
    insert->setInsertionPoint(Point2D(5.0, 5.0));
    insert->setScale(2.0, 1.0);
    insert->setRotation(degToRad(30.0));
    const EntityId id = document.addEntity(std::move(insert));

    const Transform2D stretch = Transform2D::scaling(3.0, 1.0);

    // 变换前的几何，逐点施加同一个变换，作为对照
    std::vector<std::unique_ptr<Entity>> expected = insertAt(document, id)->explode();
    Box2D expectedBox;
    for (std::unique_ptr<Entity>& entity : expected) {
        entity->transform(stretch);
        expectedBox.expand(entity->boundingBox());
    }

    document.transformEntities({ id }, stretch, QStringLiteral("拉伸"));

    const InsertEntity* stored = insertAt(document, id);
    QVERIFY(stored != nullptr);

    const Box2D actualBox = stored->boundingBox();
    QVERIFY(actualBox.isValid());
    QVERIFY(expectedBox.isValid());

    // 烘焙把几何原样搬进私有块定义，包围盒必须逐位吻合
    QVERIFY(std::abs(actualBox.minX - expectedBox.minX) < 1e-6);
    QVERIFY(std::abs(actualBox.minY - expectedBox.minY) < 1e-6);
    QVERIFY(std::abs(actualBox.maxX - expectedBox.maxX) < 1e-6);
    QVERIFY(std::abs(actualBox.maxY - expectedBox.maxY) < 1e-6);
    QCOMPARE(stored->expandedCount(), expected.size());

    // 该块参照现在指向一份私有定义；文档必须能把它收集出来写出
    const std::vector<std::shared_ptr<BlockDefinition>> referenced = document.referencedBlocks();
    bool found = false;
    for (const std::shared_ptr<BlockDefinition>& candidate : referenced) {
        if (candidate.get() == stored->block().get()) found = true;
    }
    QVERIFY(found);
}

void TestBlockInsert::explodeReplacesInsertInOneUndoStep()
{
    Document document;
    const auto block = makeBlock(QStringLiteral("B1"));
    document.blocks().add(block);

    const EntityId id = addInsert(document, block, Point2D(10.0, 20.0), 2.0, 2.0);
    const std::size_t children = insertAt(document, id)->expandedCount();
    QVERIFY(children >= 2);

    int exploded = 0;
    QVERIFY(document.explodeEntities({ id }, &exploded));
    QCOMPARE(exploded, 1);
    QVERIFY(document.entity(id) == nullptr);
    QCOMPARE(document.entityCount(), children);

    // 一次撤销回到分解前：块参照回来、展开出的图元全部消失
    document.undoStack()->undo();
    QCOMPARE(document.entityCount(), std::size_t(1));
    QVERIFY(document.entity(id) != nullptr);

    document.undoStack()->redo();
    QCOMPARE(document.entityCount(), children);

    // 选择集里没有块参照时不产生撤销记录
    const int before = document.undoStack()->count();
    QVERIFY(!document.explodeEntities(document.drawOrder(), nullptr));
    QCOMPARE(document.undoStack()->count(), before);
}

// -----------------------------------------------------------------------------
//  渲染
//
//  只测「数据对不对」是不够的：块参照是新增的图元类型，渲染层没有对应分支时
//  一切模型测试照样通过，用户看到的却是一张空图。这里离屏渲染一遍并数墨迹像素。
// -----------------------------------------------------------------------------
void TestBlockInsert::rendersInsertIntoPixels()
{
    Document document;
    auto block = std::make_shared<BlockDefinition>(QStringLiteral("B1"));

    auto circle = std::make_unique<CircleEntity>(Point2D(0.0, 0.0), 30.0);
    circle->setLayerName(QStringLiteral("0"));
    block->addEntity(std::move(circle));

    auto line = std::make_unique<LineEntity>(Point2D(-30.0, -30.0), Point2D(30.0, 30.0));
    line->setLayerName(QStringLiteral("B"));
    block->addEntity(std::move(line));
    document.blocks().add(block);

    auto insert = std::make_unique<InsertEntity>(block);
    insert->setInsertionPoint(Point2D(0.0, 0.0));
    insert->setScale(2.0, 2.0);
    insert->setRotation(degToRad(20.0));
    document.addEntity(std::move(insert));

    const QSize size(400, 400);
    const int insertInk = renderDocument(document, size).first;
    QVERIFY2(insertInk > 500,
             qPrintable(QStringLiteral("块参照没有画出任何东西（墨迹像素 %1）").arg(insertInk)));

    // 分解之后画出来的必须是同一张图：块参照的渲染路径与普通图元一致
    QVERIFY(document.explodeEntities(document.drawOrder(), nullptr));
    const int explodedInk = renderDocument(document, size).first;
    QVERIFY(std::abs(explodedInk - insertInk) <= insertInk / 20 + 50);
}

// -----------------------------------------------------------------------------
//  文件往返
// -----------------------------------------------------------------------------
void TestBlockInsert::dxfRoundTripPreservesBlockAndInsert()
{
    Document source;
    source.layers().ensure(QStringLiteral("KLUJO"));

    auto block = makeBlock(QStringLiteral("bloko"));
    AttributeDefinition definition;
    definition.tag = QStringLiteral("ETIKEDO");
    definition.defaultValue = QStringLiteral("valoro");
    definition.position = Point2D(5.0, 1.0);
    definition.height = 2.5;
    block->addAttributeDefinition(definition);
    source.blocks().add(block);

    auto insert = std::make_unique<InsertEntity>(block);
    insert->setInsertionPoint(Point2D(12.0, -3.0));
    insert->setScale(2.0, -1.5);
    insert->setRotation(degToRad(311.3439));
    insert->setLayerName(QStringLiteral("KLUJO"));
    const EntityId id = source.addEntity(std::move(insert));
    const Box2D sourceBox = source.entity(id)->boundingBox();

    const QString path = m_dir.filePath(QStringLiteral("blocks.dxf"));
    WriteOptions options;
    options.version = QStringLiteral("AC1032");
    const IoResult written = CadFile::save(path, source, options);
    QVERIFY2(written.ok, qPrintable(written.error));

    Document loaded;
    const IoResult read = CadFile::load(path, loaded);
    QVERIFY2(read.ok, qPrintable(read.error));

    // 读回来必须**还是块参照**，而不是被炸成的一堆图元
    QCOMPARE(loaded.entityCount(), std::size_t(1));
    const EntityId loadedId = loaded.drawOrder().front();
    const InsertEntity* restored = insertAt(loaded, loadedId);
    QVERIFY(restored != nullptr);
    QCOMPARE(restored->blockName(), QStringLiteral("bloko"));
    QCOMPARE(restored->layerName(), QStringLiteral("KLUJO"));

    QVERIFY(std::abs(restored->scaleX() - 2.0) < 1e-9);
    QVERIFY(std::abs(restored->scaleY() - (-1.5)) < 1e-9);
    QVERIFY(std::abs(restored->rotation() - degToRad(311.3439)) < 1e-9);

    // 块表与块内容一并回来
    QCOMPARE(loaded.blocks().size(), std::size_t(1));
    const BlockDefinition* loadedBlock = loaded.blocks().find(QStringLiteral("BLOKO"));
    QVERIFY(loadedBlock != nullptr);
    QCOMPARE(loadedBlock->entityCount(), std::size_t(2));
    QCOMPARE(loadedBlock->attributeDefinitions().size(), std::size_t(1));
    QCOMPARE(loadedBlock->attributeDefinitions().front().tag, QStringLiteral("ETIKEDO"));

    // 属性值随块参照往返
    QCOMPARE(restored->attributes().size(), std::size_t(1));
    QCOMPARE(restored->attributes().front().value, QStringLiteral("valoro"));

    // 几何等价：包围盒逐位一致
    const Box2D loadedBox = restored->boundingBox();
    QVERIFY(loadedBox.isValid());
    QVERIFY(std::abs(loadedBox.minX - sourceBox.minX) < 1e-6);
    QVERIFY(std::abs(loadedBox.minY - sourceBox.minY) < 1e-6);
    QVERIFY(std::abs(loadedBox.maxX - sourceBox.maxX) < 1e-6);
    QVERIFY(std::abs(loadedBox.maxY - sourceBox.maxY) < 1e-6);
}

void TestBlockInsert::dxfRoundTripAllVersions()
{
    for (const QString& version : WriteOptions::availableDxfVersions()) {
        Document source;
        const auto block = makeBlock(QStringLiteral("B1"));
        source.blocks().add(block);
        addInsert(source, block, Point2D(1.0, 2.0), 2.0, 3.0, degToRad(45.0));

        const QString path = m_dir.filePath(QStringLiteral("blocks_%1.dxf").arg(version));
        WriteOptions options;
        options.version = version;

        const IoResult written = CadFile::save(path, source, options);
        QVERIFY2(written.ok, qPrintable(QStringLiteral("%1: %2").arg(version, written.error)));

        Document loaded;
        const IoResult read = CadFile::load(path, loaded);
        QVERIFY2(read.ok, qPrintable(QStringLiteral("%1: %2").arg(version, read.error)));

        QVERIFY2(loaded.entityCount() == 1,
                 qPrintable(QStringLiteral("%1: 图元数 %2").arg(version).arg(loaded.entityCount())));

        const InsertEntity* restored = insertAt(loaded, loaded.drawOrder().front());
        QVERIFY2(restored != nullptr, qPrintable(version));
        QVERIFY2(std::abs(restored->scaleX() - 2.0) < 1e-9, qPrintable(version));
        QVERIFY2(std::abs(restored->scaleY() - 3.0) < 1e-9, qPrintable(version));
        QVERIFY2(std::abs(restored->rotation() - degToRad(45.0)) < 1e-9, qPrintable(version));
        QVERIFY2(loaded.blocks().size() == 1, qPrintable(version));
    }
}

void TestBlockInsert::dwgRoundTripKeepsBlockReference()
{
    Document source;
    const auto block = makeBlock(QStringLiteral("B1"));

    // 带一条属性：DWG 侧必须写成真正的 ATTRIB 对象。曾经这里是「属性文字降级为
    // 普通 TEXT」——保存一次 DWG，块参照上的属性就散成了模型空间里的独立文字，
    // 这条测试正是盯住那次回归（实体数、属性表、属性值三处都会露馅）。
    AttributeDefinition definition;
    definition.tag = QStringLiteral("ETIKEDO");
    definition.defaultValue = QStringLiteral("valoro");
    definition.position = Point2D(5.0, 1.0);
    definition.height = 2.5;
    block->addAttributeDefinition(definition);

    source.blocks().add(block);
    auto insert = std::make_unique<InsertEntity>(block);
    insert->setInsertionPoint(Point2D(7.0, -2.0));
    insert->setScale(2.0, 2.0);
    insert->setRotation(degToRad(30.0));
    insert->setAttributeValue(QStringLiteral("ETIKEDO"), QStringLiteral("valoro-2"));
    const EntityId id = source.addEntity(std::move(insert));
    const Box2D sourceBox = source.entity(id)->boundingBox();

    const QString path = m_dir.filePath(QStringLiteral("blocks.dwg"));
    const IoResult written = CadFile::save(path, source, WriteOptions());
    QVERIFY2(written.ok, qPrintable(written.error));

    Document loaded;
    const IoResult read = CadFile::load(path, loaded);
    QVERIFY2(read.ok, qPrintable(read.error));

    // 属性必须是块参照的 ATTRIB，而不是被拆到模型空间里的独立文字
    QCOMPARE(loaded.entityCount(), std::size_t(1));
    for (EntityId loadedId : loaded.drawOrder()) {
        QVERIFY2(loaded.entity(loadedId)->type() != EntityType::Text,
                 "块属性被写成了独立的文字图元");
    }

    const InsertEntity* restored = insertAt(loaded, loaded.drawOrder().front());
    QVERIFY(restored != nullptr);
    QVERIFY(std::abs(restored->scaleX() - 2.0) < 1e-6);
    QVERIFY(std::abs(restored->scaleY() - 2.0) < 1e-6);
    QVERIFY(std::abs(restored->rotation() - degToRad(30.0)) < 1e-6);

    QCOMPARE(restored->attributes().size(), std::size_t(1));
    const Attribute* attribute = restored->findAttribute(QStringLiteral("ETIKEDO"));
    QVERIFY(attribute != nullptr);
    QCOMPARE(attribute->value, QStringLiteral("valoro-2"));
    QVERIFY(std::abs(attribute->height - 5.0) < 1e-6);      // 2.5 × 比例 2

    // 块定义里的属性定义也要一起回来，否则再插一次这个块就没有属性了
    const BlockDefinition* restoredBlock = loaded.blocks().find(QStringLiteral("B1"));
    QVERIFY(restoredBlock != nullptr);
    QCOMPARE(restoredBlock->attributeDefinitions().size(), std::size_t(1));
    QCOMPARE(restoredBlock->attributeDefinitions().front().tag, QStringLiteral("ETIKEDO"));
    QCOMPARE(restoredBlock->attributeDefinitions().front().defaultValue,
             QStringLiteral("valoro"));

    const Box2D loadedBox = restored->boundingBox();
    QVERIFY(loadedBox.isValid());
    QVERIFY(std::abs(loadedBox.minX - sourceBox.minX) < 1e-3);
    QVERIFY(std::abs(loadedBox.maxY - sourceBox.maxY) < 1e-3);
}

void TestBlockInsert::missingBlockIsReported()
{
    // 手写一份引用了未定义块的 DXF：这类文件在现实中很常见（块被 purge 过）
    const QString path = m_dir.filePath(QStringLiteral("missing.dxf"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));

    const QString content = QStringLiteral(
        "0\nSECTION\n2\nHEADER\n0\nENDSEC\n"
        "0\nSECTION\n2\nENTITIES\n"
        "0\nINSERT\n5\n100\n100\nAcDbEntity\n8\n0\n"
        "100\nAcDbBlockReference\n2\nNEKONATA\n"
        "10\n1.0\n20\n2.0\n30\n0.0\n"
        "41\n1.0\n42\n1.0\n43\n1.0\n50\n0.0\n"
        "0\nENDSEC\n0\nEOF\n");
    file.write(content.toUtf8());
    file.close();

    Document loaded;
    const IoResult read = CadFile::load(path, loaded);
    QVERIFY2(read.ok, qPrintable(read.error));

    // 不静默丢弃：既要有警告，也不能留下一个指向不存在定义的块参照
    QCOMPARE(loaded.entityCount(), std::size_t(0));
    bool warned = false;
    for (const QString& warning : read.warnings) {
        if (warning.contains(QStringLiteral("INSERT(引用的块未定义)"))) warned = true;
    }
    QVERIFY2(warned, qPrintable(read.warnings.join(QStringLiteral(" | "))));
}

void TestBlockInsert::circularBlockReferenceIsBroken()
{
    // A 里插 B、B 里又插 A：现实中的 CAD 会拒绝建立这种结构，但手改过或损坏的
    // 文件里存在。InsertEntity 持有块定义的 shared_ptr，而块定义又拥有这个块参照，
    // 真连起来两者互相引用、引用计数永远降不到 0（整块定义连同图元一起泄漏），
    // 展开时也会在两条边上来回走到深度上限。导入端必须把环断开并如实上报。
    const QString path = m_dir.filePath(QStringLiteral("circular.dxf"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));

    const QString content = QStringLiteral(
        "0\nSECTION\n2\nHEADER\n0\nENDSEC\n"
        "0\nSECTION\n2\nBLOCKS\n"
        "0\nBLOCK\n8\n0\n2\nAAA\n70\n0\n10\n0.0\n20\n0.0\n30\n0.0\n3\nAAA\n1\n\n"
        "0\nINSERT\n8\n0\n2\nBBB\n10\n0.0\n20\n0.0\n30\n0.0\n"
        "41\n1.0\n42\n1.0\n43\n1.0\n50\n0.0\n"
        "0\nENDBLK\n8\n0\n"
        "0\nBLOCK\n8\n0\n2\nBBB\n70\n0\n10\n0.0\n20\n0.0\n30\n0.0\n3\nBBB\n1\n\n"
        "0\nINSERT\n8\n0\n2\nAAA\n10\n0.0\n20\n0.0\n30\n0.0\n"
        "41\n1.0\n42\n1.0\n43\n1.0\n50\n0.0\n"
        "0\nENDBLK\n8\n0\n"
        "0\nENDSEC\n"
        "0\nSECTION\n2\nENTITIES\n"
        "0\nINSERT\n5\n100\n100\nAcDbEntity\n8\n0\n"
        "100\nAcDbBlockReference\n2\nAAA\n"
        "10\n0.0\n20\n0.0\n30\n0.0\n41\n1.0\n42\n1.0\n43\n1.0\n50\n0.0\n"
        "0\nENDSEC\n0\nEOF\n");
    file.write(content.toUtf8());
    file.close();

    Document loaded;
    const IoResult read = CadFile::load(path, loaded);
    QVERIFY2(read.ok, qPrintable(read.error));

    // 环被断开：展开必须立刻返回（而不是在两条边上走到深度上限）
    QCOMPARE(loaded.entityCount(), std::size_t(1));
    const InsertEntity* insert = insertAt(loaded, loaded.drawOrder().front());
    QVERIFY(insert != nullptr);
    QVERIFY(insert->block() != nullptr);
    QCOMPARE(insert->block()->name(), QStringLiteral("AAA"));

    // 两个块都还在（环只断在引用上，不是整块丢弃）
    QCOMPARE(loaded.blocks().size(), std::size_t(2));

    bool warned = false;
    for (const QString& warning : read.warnings) {
        if (warning.contains(QStringLiteral("循环引用"))) warned = true;
    }
    QVERIFY2(warned, qPrintable(read.warnings.join(QStringLiteral(" | "))));

    // 再存再读一遍不应崩，也不应把块表撑大
    const QString out = m_dir.filePath(QStringLiteral("circular_out.dxf"));
    QVERIFY(CadFile::save(out, loaded, WriteOptions()).ok);
    Document again;
    QVERIFY(CadFile::load(out, again).ok);
    QCOMPARE(again.blocks().size(), std::size_t(2));
}

// -----------------------------------------------------------------------------
//  真实图纸（可选）
//
//  属性块/嵌套块的真实样本不随仓库分发（体积大、多为第三方图纸）。
//  需要时由使用者指定：
//    LYCAD_BLOCK_SAMPLE=/path/to/file.dxf ctest -R test_block_insert
// -----------------------------------------------------------------------------
void TestBlockInsert::realSampleIfProvided()
{
    const QByteArray sample = qgetenv("LYCAD_BLOCK_SAMPLE");
    if (sample.isEmpty()) {
        QSKIP("未设置 LYCAD_BLOCK_SAMPLE，跳过真实图纸测试");
    }

    const QString path = QString::fromLocal8Bit(sample);
    if (!QFileInfo::exists(path)) {
        QSKIP("LYCAD_BLOCK_SAMPLE 指向的文件不存在");
    }

    Document loaded;
    const IoResult read = CadFile::load(path, loaded);
    QVERIFY2(read.ok, qPrintable(read.error));
    QVERIFY(loaded.entityCount() > 0);

    // 每个块参照都必须能展开出几何，且范围有限。
    // 样本里不一定有块参照（例如只用来核对标注几何的图纸），
    // 那种情况下这一段自然为空，后面的渲染核对照做。
    int inserts = 0;
    for (EntityId id : loaded.drawOrder()) {
        const InsertEntity* insert = insertAt(loaded, id);
        if (!insert) continue;
        ++inserts;
        QVERIFY(insert->hasBlock());
        QVERIFY(insert->expandedCount() > 0);
        QVERIFY(insert->boundingBox().isValid());
    }

    // 写出再读回：块结构与块参照个数必须保持不变
    const QString out = m_dir.filePath(QStringLiteral("sample_roundtrip.dxf"));
    WriteOptions options;
    options.version = QStringLiteral("AC1032");
    QVERIFY(CadFile::save(out, loaded, options).ok);

    Document again;
    QVERIFY2(CadFile::load(out, again).ok, "读回失败");
    QCOMPARE(again.blocks().size(), loaded.blocks().size());

    int insertsAgain = 0;
    for (EntityId id : again.drawOrder()) {
        if (insertAt(again, id)) ++insertsAgain;
    }
    QCOMPARE(insertsAgain, inserts);

    // 可选：把样本渲染成图片，便于人工核对块参照画出来的样子。
    //   LYCAD_BLOCK_SAMPLE=... LYCAD_BLOCK_SNAPSHOT=/tmp/sample.png ctest -R test_block_insert
    const QByteArray snapshot = qgetenv("LYCAD_BLOCK_SNAPSHOT");
    if (!snapshot.isEmpty()) {
        const QString target = QString::fromLocal8Bit(snapshot);
        // 读入的、以及「写出再读回」的两份都渲染：前者验几何，后者验往返保真
        const int loadedInk = renderDocument(loaded, QSize(1200, 900), target).first;
        qInfo("样本渲染（读入）：墨迹像素 %d → %s", loadedInk, qPrintable(target));
        QVERIFY(loadedInk > 500);

        QString roundTripPath = target;
        roundTripPath.replace(QStringLiteral(".png"), QStringLiteral("_roundtrip.png"));
        const int roundTripInk = renderDocument(again, QSize(1200, 900), roundTripPath).first;
        qInfo("样本渲染（往返）：墨迹像素 %d → %s", roundTripInk, qPrintable(roundTripPath));
        QVERIFY(roundTripInk > 500);
    }
}

// -----------------------------------------------------------------------------
//  同一张图的 DWG 与 DXF 必须读出同一份块参照与属性
//
//  DWG 的属性对象（ATTRIB）是独立对象、且专属字段只能靠与 DXF 对照来定标，
//  这条对照测试就是那把尺子：只设 LYCAD_BLOCK_DWG_SAMPLE 时会自动找同名的 .dxf。
// -----------------------------------------------------------------------------
void TestBlockInsert::dwgAndDxfAgreeOnBlocks()
{
    const QByteArray dwgSample = qgetenv("LYCAD_BLOCK_DWG_SAMPLE");
    if (dwgSample.isEmpty()) {
        QSKIP("未设置 LYCAD_BLOCK_DWG_SAMPLE，跳过 DWG/DXF 块对照测试");
    }

    const QString dwgPath = QString::fromLocal8Bit(dwgSample);
    if (!QFileInfo::exists(dwgPath)) {
        QSKIP("LYCAD_BLOCK_DWG_SAMPLE 指向的文件不存在");
    }

    QString dxfPath = QString::fromLocal8Bit(qgetenv("LYCAD_BLOCK_DXF_SAMPLE"));
    if (dxfPath.isEmpty()) {
        dxfPath = dwgPath;
        dxfPath.chop(4);
        dxfPath += QStringLiteral(".dxf");
    }
    if (!QFileInfo::exists(dxfPath)) {
        QSKIP("找不到与 DWG 同名的 DXF，跳过对照");
    }

    Document fromDwg;
    Document fromDxf;
    const IoResult dwgRead = CadFile::load(dwgPath, fromDwg);
    const IoResult dxfRead = CadFile::load(dxfPath, fromDxf);
    QVERIFY2(dwgRead.ok, qPrintable(dwgRead.error));
    QVERIFY2(dxfRead.ok, qPrintable(dxfRead.error));

    QCOMPARE(fromDwg.entityCount(), fromDxf.entityCount());
    QCOMPARE(fromDwg.blocks().size(), fromDxf.blocks().size());

    // 块定义：名字、图元数、属性定义（标签/默认值/插入点/字高）
    for (const std::shared_ptr<BlockDefinition>& block : fromDxf.blocks().all()) {
        const BlockDefinition* other = fromDwg.blocks().find(block->name());
        QVERIFY2(other != nullptr, qPrintable(block->name()));
        QCOMPARE(other->entityCount(), block->entityCount());
        QCOMPARE(other->attributeDefinitions().size(), block->attributeDefinitions().size());

        for (std::size_t i = 0; i < block->attributeDefinitions().size(); ++i) {
            const AttributeDefinition& expected = block->attributeDefinitions()[i];
            const AttributeDefinition& actual = other->attributeDefinitions()[i];
            QCOMPARE(actual.tag, expected.tag);
            QCOMPARE(actual.defaultValue, expected.defaultValue);
            QVERIFY(actual.position.distanceTo(expected.position) < 1e-6);
            QVERIFY(std::abs(actual.height - expected.height) < 1e-9);
        }
    }

    // 块参照：插入参数与属性值
    int compared = 0;
    for (EntityId id : fromDxf.drawOrder()) {
        const auto* expected = dynamic_cast<const InsertEntity*>(fromDxf.entity(id));
        if (!expected) continue;
        ++compared;

        const InsertEntity* actual = nullptr;
        for (EntityId otherId : fromDwg.drawOrder()) {
            const auto* candidate = dynamic_cast<const InsertEntity*>(fromDwg.entity(otherId));
            if (candidate && candidate->blockName() == expected->blockName()) {
                actual = candidate;
                break;
            }
        }
        QVERIFY2(actual != nullptr, qPrintable(expected->blockName()));

        QVERIFY(actual->insertionPoint().distanceTo(expected->insertionPoint()) < 1e-6);
        QVERIFY(std::abs(actual->scaleX() - expected->scaleX()) < 1e-9);
        QVERIFY(std::abs(actual->scaleY() - expected->scaleY()) < 1e-9);
        QVERIFY(std::abs(actual->rotation() - expected->rotation()) < 1e-9);
        QCOMPARE(actual->attributes().size(), expected->attributes().size());

        for (const Attribute& attribute : expected->attributes()) {
            const Attribute* other = actual->findAttribute(attribute.tag);
            QVERIFY2(other != nullptr, qPrintable(attribute.tag));
            QCOMPARE(other->value, attribute.value);
            QVERIFY2(other->position.distanceTo(attribute.position) < 1e-6,
                     qPrintable(QStringLiteral("属性 %1 位置 %2,%3 vs %4,%5")
                                    .arg(attribute.tag)
                                    .arg(other->position.x).arg(other->position.y)
                                    .arg(attribute.position.x).arg(attribute.position.y)));
            QVERIFY(std::abs(other->height - attribute.height) < 1.0);
            QVERIFY(std::abs(other->rotation - attribute.rotation) < 1e-6);
        }
    }
    QVERIFY(compared > 0);
}

QTEST_MAIN(TestBlockInsert)
#include "test_block_insert.moc"
