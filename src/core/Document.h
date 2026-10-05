// =============================================================================
//  LyCAD - core/Document.h
//  文档模型：图元容器 + 图层表 + 选择集 + 撤销栈。
//
//  分层约束
//    本类刻意不依赖 io 模块，因此不提供 load/save 接口。文件读写由 io 模块的
//    CadFile 门面完成，它通过 beginBatchLoad()/endBatchLoad() 与 addEntity()
//    装配文档，从而保持 core 处于依赖链的最底层、可独立复用与单元测试。
//
//  撤销模型
//    所有修改都经由 QUndoCommand 落到 Document 内部实现 (internalXxx)，
//    命令对象负责保存前后状态。批量装载模式下自动旁路撤销栈，避免打开大图时
//    产生成千上万条撤销记录。
// =============================================================================
#pragma once

#include "core/Entity.h"
#include "core/Layer.h"
#include "core/Selection.h"
#include "core/TextStyle.h"
#include "core/Types.h"

#include <QObject>
#include <QString>

#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

class QUndoStack;

namespace lycad {

class AddEntitiesCommand;
class RemoveEntitiesCommand;
class TransformEntitiesCommand;
class EntityPropertyCommand;
class ReplaceEntityCommand;
class LayerTableCommand;

class Document : public QObject {
    Q_OBJECT

public:
    explicit Document(QObject* parent = nullptr);
    ~Document() override;

    // ---- 图元查询 ------------------------------------------------------------
    /// 绘制顺序（后加入者在上层）
    const std::vector<EntityId>& drawOrder() const noexcept { return m_order; }

    /// 按 id 取图元，不存在返回 nullptr
    Entity* entity(EntityId id) const;

    std::size_t entityCount() const noexcept { return m_order.size(); }
    bool isEmpty() const noexcept { return m_order.empty(); }

    std::vector<Entity*> allEntities() const;

    // ---- 图元编辑（可撤销） ---------------------------------------------------
    /// 追加单个图元，返回其 id
    EntityId addEntity(std::unique_ptr<Entity> entity);

    /// 追加一批图元（单条撤销记录），返回各图元 id
    std::vector<EntityId> addEntities(std::vector<std::unique_ptr<Entity>> entities);

    /// 删除单个/批量图元
    bool removeEntity(EntityId id);
    bool removeEntities(const std::vector<EntityId>& ids);

    /// 对指定图元群施加仿射变换（移动/旋转/缩放/镜像的统一入口）
    bool transformEntities(const std::vector<EntityId>& ids,
                           const Transform2D& transform,
                           const QString& actionLabel = QString());

    /// 修改图层归属
    bool setEntitiesLayer(const std::vector<EntityId>& ids, const QString& layerName);

    /// 修改颜色（ACI，<0 表示随层）
    bool setEntitiesColor(const std::vector<EntityId>& ids, int aci);

    /// 修改线宽（DXF 370）
    bool setEntitiesLineweight(const std::vector<EntityId>& ids, int dxfValue);

    /// 修改可见性
    bool setEntitiesVisible(const std::vector<EntityId>& ids, bool visible);

    /// 以新几何整体替换某图元的几何（夹点拖拽结束后调用）
    bool replaceEntity(EntityId id,
                       std::unique_ptr<Entity> replacement,
                       const QString& actionLabel = QString());

    // ---- 图层 ----------------------------------------------------------------
    LayerManager& layers() noexcept { return m_layers; }
    const LayerManager& layers() const noexcept { return m_layers; }

    // ---- 文字样式 --------------------------------------------------------------
    /// 文字样式表：图元只带样式名（DXF 7），字体按名在这里解析
    TextStyleTable& textStyles() noexcept { return m_textStyles; }
    const TextStyleTable& textStyles() const noexcept { return m_textStyles; }

    bool addLayer(const Layer& layer, QString* error = nullptr);
    bool removeLayer(const QString& name, bool moveEntitiesToDefault = true,
                     QString* error = nullptr);
    bool renameLayer(const QString& from, const QString& to, QString* error = nullptr);
    bool setLayerColor(const QString& name, int aci);
    bool setLayerVisible(const QString& name, bool visible);
    bool setLayerFrozen(const QString& name, bool frozen);
    bool setLayerLocked(const QString& name, bool locked);
    bool setLayerLineweight(const QString& name, int dxfValue);

    /// 设置当前图层（不属于可撤销操作）
    bool setCurrentLayer(const QString& name);

    /// 批量装载完成后，手工通知图层表已变化
    void notifyLayersChanged();

    // ---- 选择集 --------------------------------------------------------------
    Selection& selection() noexcept { return *m_selection; }
    const Selection& selection() const noexcept { return *m_selection; }
    std::vector<Entity*> selectedEntities() const;

    /// 可被选择的图元（隐藏图层、锁定图层、不可见图元被排除）
    bool isSelectable(EntityId id) const;

    // ---- 撤销 ----------------------------------------------------------------
    QUndoStack* undoStack() const noexcept { return m_undoStack; }
    bool isModified() const;
    void markClean();

    // ---- 批量装载 ------------------------------------------------------------
    /// 进入批量装载：期间修改不进撤销栈、不逐条发信号
    void beginBatchLoad();
    void endBatchLoad();
    bool isBatchLoading() const noexcept { return m_batchDepth > 0; }

    /// 清空文档。resetLayers 为 true 时图层表恢复为仅含 "0" 层。
    void clear(bool resetLayers = true);

    // ---- 范围 ----------------------------------------------------------------
    Box2D extents() const;
    Box2D extentsOf(const std::vector<EntityId>& ids) const;

    // ---- 文件关联信息（不含实际读写逻辑） ---------------------------------------
    const QString& filePath() const noexcept { return m_filePath; }
    void setFilePath(const QString& path);
    /// 无路径时返回 "未命名"
    QString displayName() const;
    /// 带修改标记的窗口标题文本
    QString displayTitle() const;

    // ---- 内部实现（仅供命令类调用） ---------------------------------------------
    EntityId allocateId() noexcept;
    EntityId internalAdd(std::unique_ptr<Entity> entity,
                         std::optional<std::size_t> orderIndex = std::nullopt);
    std::unique_ptr<Entity> internalTake(EntityId id, std::size_t* outOrderIndex = nullptr);
    std::unique_ptr<Entity> internalSwap(EntityId id, std::unique_ptr<Entity> replacement);
    void internalSetLayers(std::vector<Layer> layers);
    void internalRetargetEntities(const QString& fromLayer, const QString& toLayer);

signals:
    void entityAdded(lycad::EntityId id);
    void entityRemoved(lycad::EntityId id);
    void entityChanged(lycad::EntityId id);
    /// 文档被整体替换（新建/打开文件）
    void documentReset();
    void layersChanged();
    void selectionChanged();
    void filePathChanged(const QString& path);
    void modificationChanged(bool modified);
    /// 图元集合的包围范围可能发生变化，视图可据此刷新
    void extentsChanged();

private:
    friend class AddEntitiesCommand;
    friend class RemoveEntitiesCommand;
    friend class TransformEntitiesCommand;
    friend class EntityPropertyCommand;
    friend class ReplaceEntityCommand;
    friend class LayerTableCommand;

    /// 图元集合变化后统一发出的收尾信号
    void afterEntitiesMutated();

    std::unordered_map<EntityId, std::unique_ptr<Entity>> m_entities;
    std::vector<EntityId> m_order;
    EntityId m_nextId = 1;

    LayerManager m_layers;
    TextStyleTable m_textStyles;
    Selection* m_selection = nullptr;
    QUndoStack* m_undoStack = nullptr;

    QString m_filePath;
    int m_batchDepth = 0;
};

} // namespace lycad
