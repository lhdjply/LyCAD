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

QTEST_MAIN(TestDocument)
#include "test_document.moc"
