// =============================================================================
//  LyCAD - tests/test_document.cpp
//  文档模型：增删改、撤销重做、图层联动、选择集的单元测试。
// =============================================================================
#include <QtTest>

#include <QUndoStack>

#include "core/CircleEntity.h"
#include "core/Document.h"
#include "core/LineEntity.h"
#include "core/PolylineEntity.h"
#include "core/TextEntity.h"

using namespace lycad;

class TestDocument : public QObject {
    Q_OBJECT

private slots:
    void addEntityAssignsStableId();
    void addAndUndoRestoresEmptyState();
    void removeAndUndoRestoresDrawOrder();
    void transformAndUndoRestoresGeometry();
    void gripEditIsReplaceable();
    void layerRenameRetargetsEntities();
    void layerRemovalMovesEntitiesToDefault();
    void deletingEntityClearsSelection();
    void selectabilityRespectsLayerState();
    void extentsExcludeHiddenLayers();
    void batchLoadDoesNotPolluteUndoStack();
    void textPropertiesAreEditableAndUndoable();
    void textPropertiesIgnoreNonTextSelection();

private:
    static std::unique_ptr<LineEntity> makeLine(double x1, double y1, double x2, double y2);
};

std::unique_ptr<LineEntity> TestDocument::makeLine(double x1, double y1, double x2, double y2)
{
    return std::make_unique<LineEntity>(Point2D(x1, y1), Point2D(x2, y2));
}

// -----------------------------------------------------------------------------
void TestDocument::addEntityAssignsStableId()
{
    Document document;

    const EntityId a = document.addEntity(makeLine(0, 0, 10, 0));
    const EntityId b = document.addEntity(makeLine(0, 1, 10, 1));

    QVERIFY(a != kInvalidEntityId);
    QVERIFY(b != kInvalidEntityId);
    QVERIFY(a != b);
    QCOMPARE(document.entityCount(), std::size_t(2));

    QVERIFY(document.entity(a) != nullptr);
    QCOMPARE(document.entity(a)->id(), a);
    QCOMPARE(document.entity(a)->layerName(), QStringLiteral("0"));
}

void TestDocument::addAndUndoRestoresEmptyState()
{
    Document document;

    document.addEntity(makeLine(0, 0, 10, 0));
    document.addEntity(makeLine(0, 1, 10, 1));
    QCOMPARE(document.entityCount(), std::size_t(2));
    QVERIFY(document.isModified());

    document.undoStack()->undo();
    QCOMPARE(document.entityCount(), std::size_t(1));

    document.undoStack()->undo();
    QCOMPARE(document.entityCount(), std::size_t(0));
    QVERIFY(!document.isModified());

    // 重做后 id 应保持不变，避免撤销/重做导致图元标识漂移
    document.undoStack()->redo();
    document.undoStack()->redo();
    QCOMPARE(document.entityCount(), std::size_t(2));
}

void TestDocument::removeAndUndoRestoresDrawOrder()
{
    Document document;

    const EntityId first = document.addEntity(makeLine(0, 0, 1, 0));
    const EntityId second = document.addEntity(makeLine(0, 1, 1, 1));
    const EntityId third = document.addEntity(makeLine(0, 2, 1, 2));

    QCOMPARE(document.drawOrder().size(), std::size_t(3));

    // 删除中间一个，再撤销，绘制次序必须完全还原
    document.removeEntity(second);
    QCOMPARE(document.drawOrder().size(), std::size_t(2));
    QCOMPARE(document.drawOrder()[0], first);
    QCOMPARE(document.drawOrder()[1], third);

    document.undoStack()->undo();
    QCOMPARE(document.drawOrder().size(), std::size_t(3));
    QCOMPARE(document.drawOrder()[0], first);
    QCOMPARE(document.drawOrder()[1], second);
    QCOMPARE(document.drawOrder()[2], third);
}

void TestDocument::transformAndUndoRestoresGeometry()
{
    Document document;

    const EntityId id = document.addEntity(makeLine(0, 0, 10, 0));
    document.addEntity(std::make_unique<CircleEntity>(Point2D(20.0, 20.0), 5.0));

    QVERIFY(document.transformEntities({ id }, Transform2D::translation(5.0, 7.0),
                                       QStringLiteral("移动")));

    const auto* moved = static_cast<const LineEntity*>(document.entity(id));
    QVERIFY(std::abs(moved->start().x - 5.0) < 1e-9);
    QVERIFY(std::abs(moved->start().y - 7.0) < 1e-9);

    // 撤销必须精确还原，包括圆在非等比缩放下被近似过的半径
    document.undoStack()->undo();
    const auto* restored = static_cast<const LineEntity*>(document.entity(id));
    QVERIFY(std::abs(restored->start().x - 0.0) < 1e-12);
    QVERIFY(std::abs(restored->end().x - 10.0) < 1e-12);
}

void TestDocument::gripEditIsReplaceable()
{
    Document document;

    const EntityId id = document.addEntity(makeLine(0, 0, 10, 0));

    auto edited = std::unique_ptr<Entity>(document.entity(id)->clone());
    QVERIFY(edited->moveGrip(1, Point2D(10.0, 10.0)));
    QVERIFY(document.replaceEntity(id, std::move(edited), QStringLiteral("夹点编辑")));

    const auto* changed = static_cast<const LineEntity*>(document.entity(id));
    QVERIFY(std::abs(changed->end().y - 10.0) < 1e-12);
    QCOMPARE(changed->id(), id);        // 替换后 id 不变

    document.undoStack()->undo();
    const auto* reverted = static_cast<const LineEntity*>(document.entity(id));
    QVERIFY(std::abs(reverted->end().y - 0.0) < 1e-12);
}

void TestDocument::layerRenameRetargetsEntities()
{
    Document document;

    QString error;
    QVERIFY(document.addLayer(Layer(QStringLiteral("轮廓")), &error));

    const EntityId id = document.addEntity(makeLine(0, 0, 1, 0));
    QVERIFY(document.setEntitiesLayer({ id }, QStringLiteral("轮廓")));
    QCOMPARE(document.entity(id)->layerName(), QStringLiteral("轮廓"));

    QVERIFY(document.renameLayer(QStringLiteral("轮廓"), QStringLiteral("外形"), &error));
    QCOMPARE(document.entity(id)->layerName(), QStringLiteral("外形"));

    // 撤销后图层名与图元归属应一并回退
    document.undoStack()->undo();
    QCOMPARE(document.entity(id)->layerName(), QStringLiteral("轮廓"));
    QVERIFY(document.layers().contains(QStringLiteral("轮廓")));

    // 系统保留图层不可重命名
    QVERIFY(!document.renameLayer(QStringLiteral("0"), QStringLiteral("X"), &error));
    QVERIFY(!error.isEmpty());
}

void TestDocument::layerRemovalMovesEntitiesToDefault()
{
    Document document;

    QString error;
    QVERIFY(document.addLayer(Layer(QStringLiteral("临时")), &error));

    const EntityId id = document.addEntity(makeLine(0, 0, 1, 0));
    QVERIFY(document.setEntitiesLayer({ id }, QStringLiteral("临时")));

    QVERIFY(document.removeLayer(QStringLiteral("临时"), true, &error));
    QVERIFY(!document.layers().contains(QStringLiteral("临时")));
    QCOMPARE(document.entity(id)->layerName(), QStringLiteral("0"));

    // 不允许删除图层 0
    QVERIFY(!document.removeLayer(QStringLiteral("0"), true, &error));
}

void TestDocument::deletingEntityClearsSelection()
{
    Document document;

    const EntityId id = document.addEntity(makeLine(0, 0, 1, 0));
    document.selection().setIds({ id });
    QCOMPARE(document.selection().size(), std::size_t(1));

    document.removeEntity(id);
    QVERIFY(document.selection().isEmpty());

    document.undoStack()->undo();
    QCOMPARE(document.entityCount(), std::size_t(1));
    QVERIFY(document.selection().isEmpty());     // 撤销不会恢复选择集
}

void TestDocument::selectabilityRespectsLayerState()
{
    Document document;

    QString error;
    QVERIFY(document.addLayer(Layer(QStringLiteral("锁定层")), &error));
    QVERIFY(document.addLayer(Layer(QStringLiteral("关闭层")), &error));

    const EntityId locked = document.addEntity(makeLine(0, 0, 1, 0));
    const EntityId hidden = document.addEntity(makeLine(0, 1, 1, 1));
    QVERIFY(document.setEntitiesLayer({ locked }, QStringLiteral("锁定层")));
    QVERIFY(document.setEntitiesLayer({ hidden }, QStringLiteral("关闭层")));

    QVERIFY(document.setLayerLocked(QStringLiteral("锁定层"), true));
    QVERIFY(document.setLayerVisible(QStringLiteral("关闭层"), false));

    QVERIFY(!document.isSelectable(locked));      // 锁定图层不可选择
    QVERIFY(!document.isSelectable(hidden));      // 关闭图层不可选择

    // 解锁并打开后恢复可选
    QVERIFY(document.setLayerLocked(QStringLiteral("锁定层"), false));
    QVERIFY(document.isSelectable(locked));
}

void TestDocument::extentsExcludeHiddenLayers()
{
    Document document;

    QString error;
    QVERIFY(document.addLayer(Layer(QStringLiteral("远景")), &error));

    document.addEntity(makeLine(0, 0, 10, 0));
    const EntityId far = document.addEntity(makeLine(0, 0, 1000, 1000));
    QVERIFY(document.setEntitiesLayer({ far }, QStringLiteral("远景")));

    Box2D extents = document.extents();
    QVERIFY(std::abs(extents.maxX - 1000.0) < 1e-9);

    // 关闭该图层后，「缩放范围」不应再把不可见内容计入
    QVERIFY(document.setLayerVisible(QStringLiteral("远景"), false));
    extents = document.extents();
    QVERIFY(std::abs(extents.maxX - 10.0) < 1e-9);
}

void TestDocument::batchLoadDoesNotPolluteUndoStack()
{
    Document document;

    document.beginBatchLoad();
    for (int i = 0; i < 50; ++i) {
        document.addEntity(makeLine(0, i, 10, i));
    }
    document.endBatchLoad();

    QCOMPARE(document.entityCount(), std::size_t(50));
    QVERIFY(!document.undoStack()->canUndo());    // 批量装载不产生撤销记录
    QVERIFY(!document.isModified());              // 且视为「干净」状态
}

// -----------------------------------------------------------------------------
//  文字属性：字高 / 旋转 / 水平对齐 / 垂直对齐
//
//  这四个字段由属性面板直接改（Document::setEntitiesTextXxx），因此在文档层
//  成对验证两件事：**只改文字图元**、**一次改动一条撤销记录且能精确还原**。
//  单位口径也在这一层钉住：旋转存弧度，界面的度换算不归这里管。
void TestDocument::textPropertiesAreEditableAndUndoable()
{
    Document document;

    auto first = std::make_unique<TextEntity>(QStringLiteral("第一"), Point2D(10.0, 20.0), 3.5);
    first->setAlignmentPoint(Point2D(30.0, 40.0));      // 第二对齐点与插入点不同
    const EntityId a = document.addEntity(std::move(first));
    const EntityId b = document.addEntity(
        std::make_unique<TextEntity>(QStringLiteral("第二"), Point2D(50.0, 20.0), 5.0));
    const EntityId line = document.addEntity(makeLine(0, 0, 10, 0));

    const auto* textA = static_cast<const TextEntity*>(document.entity(a));
    const auto* textB = static_cast<const TextEntity*>(document.entity(b));
    QCOMPARE(textA->height(), 3.5);
    QCOMPARE(textB->height(), 5.0);

    // 用 index() 而不是 count()：undo 之后命令仍留在栈上供重做，count() 不会减少
    const int baselineRecords = document.undoStack()->index();

    // ---- 字高：批量改，非文字图元不受影响 ------------------------------------
    QVERIFY(document.setEntitiesTextHeight({ a, b, line }, 2.5));
    textA = static_cast<const TextEntity*>(document.entity(a));
    textB = static_cast<const TextEntity*>(document.entity(b));
    QCOMPARE(textA->height(), 2.5);
    QCOMPARE(textB->height(), 2.5);
    QCOMPARE(document.entity(line)->type(), EntityType::Line);
    QCOMPARE(document.undoStack()->index(), baselineRecords + 1);   // 一条记录

    // ---- 旋转：界面给度、文档存弧度；超出 0~360 归一 --------------------------
    QVERIFY(document.setEntitiesTextRotation({ a }, degToRad(450.0)));
    textA = static_cast<const TextEntity*>(document.entity(a));
    QVERIFY(std::abs(textA->rotation() - degToRad(90.0)) < 1e-9);

    // ---- 对齐：水平与垂直各自独立 --------------------------------------------
    // 改成「居中」后定位点改用第二对齐点——这正是面板上改对齐时文字会移动的原因
    QVERIFY(document.setEntitiesTextHAlign({ a }, TextEntity::HAlign::Center));
    textA = static_cast<const TextEntity*>(document.entity(a));
    QCOMPARE(textA->hAlign(), TextEntity::HAlign::Center);
    QCOMPARE(textA->vAlign(), TextEntity::VAlign::Baseline);        // 垂直档未被动过
    QVERIFY(textA->usesAlignmentPoint());
    QVERIFY(std::abs(textA->effectivePosition().x - 30.0) < 1e-9);

    QVERIFY(document.setEntitiesTextVAlign({ a }, TextEntity::VAlign::Middle));
    textA = static_cast<const TextEntity*>(document.entity(a));
    QCOMPARE(textA->vAlign(), TextEntity::VAlign::Middle);
    QCOMPARE(textA->hAlign(), TextEntity::HAlign::Center);          // 水平档未被覆盖

    // ---- 撤销：逐个字段精确还原 ----------------------------------------------
    const auto undoAll = [&document, baselineRecords]() {
        while (document.undoStack()->index() > baselineRecords) document.undoStack()->undo();
    };
    undoAll();
    textA = static_cast<const TextEntity*>(document.entity(a));
    textB = static_cast<const TextEntity*>(document.entity(b));
    QCOMPARE(textA->height(), 3.5);
    QCOMPARE(textB->height(), 5.0);
    QVERIFY(std::abs(textA->rotation()) < 1e-12);
    QCOMPARE(textA->hAlign(), TextEntity::HAlign::Left);
    QCOMPARE(textA->vAlign(), TextEntity::VAlign::Baseline);

    // 重做（只重放第一条：字高）后文字回到改过的状态
    document.undoStack()->redo();
    QCOMPARE(static_cast<const TextEntity*>(document.entity(a))->height(), 2.5);
    QCOMPARE(static_cast<const TextEntity*>(document.entity(b))->height(), 2.5);
}

// -----------------------------------------------------------------------------
//  选择集里没有文字（或字高非法）时：返回 false，且**不产生撤销记录**。
//
//  「点了没反应」本身可以接受，留下一条什么也没做的撤销记录不行——
//  用户按 Ctrl+Z 会看到「撤销 修改字高」却毫无变化，进而怀疑撤销坏了。
void TestDocument::textPropertiesIgnoreNonTextSelection()
{
    Document document;

    const EntityId line = document.addEntity(makeLine(0, 0, 10, 0));
    const EntityId circle = document.addEntity(
        std::make_unique<CircleEntity>(Point2D(0.0, 0.0), 5.0));
    const EntityId text = document.addEntity(
        std::make_unique<TextEntity>(QStringLiteral("文字"), Point2D(0.0, 0.0), 3.5));

    const int records = document.undoStack()->index();

    QVERIFY(!document.setEntitiesTextHeight({ line, circle }, 4.0));
    QVERIFY(!document.setEntitiesTextRotation({ line }, 1.0));
    QVERIFY(!document.setEntitiesTextHAlign({ circle }, TextEntity::HAlign::Right));
    QVERIFY(!document.setEntitiesTextVAlign({ circle }, TextEntity::VAlign::Top));
    QVERIFY(!document.setEntitiesTextHeight({ text }, 0.0));        // 字高必须 > 0
    QVERIFY(!document.setEntitiesTextHeight({ text }, -1.0));

    QCOMPARE(document.undoStack()->index(), records);
    const auto* t = static_cast<const TextEntity*>(document.entity(text));
    QCOMPARE(t->height(), 3.5);

    // 混合选择集里只有文字会被改：这条命令的语义是「改文字属性」，不是「改所有图元」
    QVERIFY(document.setEntitiesTextHeight({ line, circle, text }, 8.0));
    QCOMPARE(static_cast<const TextEntity*>(document.entity(text))->height(), 8.0);
    QCOMPARE(document.undoStack()->index(), records + 1);
}

QTEST_MAIN(TestDocument)
#include "test_document.moc"
