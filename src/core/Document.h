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

#include "core/Block.h"
#include "core/Entity.h"
#include "core/Layer.h"
#include "core/Selection.h"
#include "core/MTextEntity.h"
#include "core/TextEntity.h"
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
class ExplodeEntitiesCommand;

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

    /// 指定图层上的全部图元（按绘制顺序）。
    /// 供图层面板的「对象列表」使用——隐藏的图元同样包含在内，
    /// 否则用户将失去把已隐藏图元重新设为可见的入口。
    std::vector<Entity*> entitiesOnLayer(const QString& layerName) const;

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

    // ---- 文字属性（只作用于选择集里的文字图元，其它类型忽略）------------------
    //
    // 选择集里一个文字都没有时返回 false 且**不产生撤销记录**——属性面板据此
    // 判断是否给出提示，也避免用户看到一条点了没反应的「撤销 修改字高」。
    //
    // 水平与垂直对齐分开成两个入口：面板上它们是两列，多选时若其中一列取值不一致，
    // 那一列无从给出统一旧值，但另一列仍应可改。
    /// 修改字高（世界单位，必须 > 0）
    bool setEntitiesTextHeight(const std::vector<EntityId>& ids, double height);
    /// 修改旋转（弧度；内部会归一化到 [0, 2π)）
    bool setEntitiesTextRotation(const std::vector<EntityId>& ids, double radians);
    /// 修改水平对齐（DXF 72）
    bool setEntitiesTextHAlign(const std::vector<EntityId>& ids, TextEntity::HAlign hAlign);
    /// 修改垂直对齐（DXF 73）
    bool setEntitiesTextVAlign(const std::vector<EntityId>& ids, TextEntity::VAlign vAlign);
    /// 修改文字字体（字体族名；同名的真实字体在渲染时被采用）。
    /// 空串表示清除字体设置、回退到默认字体。
    bool setEntitiesTextFont(const std::vector<EntityId>& ids, const QString& fontFamily);

    // ---- 多行文字（MTEXT）独有属性 --------------------------------------------
    //
    // MTEXT 的定位靠「附着点 + 参考矩形宽度」，没有 72/73 对齐档，因此单独两个入口；
    // 与上面同一口径：选择集里一个多行文字都没有时返回 false 且不产生撤销记录。
    /// 修改附着点（DXF 71 九宫格）
    bool setEntitiesMTextAttachment(const std::vector<EntityId>& ids,
                                    MTextEntity::Attachment attachment);
    /// 修改参考矩形宽度（DXF 41，世界单位；编辑时的折行宽度，0 = 无限制）
    bool setEntitiesMTextWidth(const std::vector<EntityId>& ids, double width);

    // ---- 块参照属性（只作用于选择集里的块参照，其它类型忽略） -------------------
    //
    // 与文字属性同一口径：选择集里一个块参照都没有时返回 false 且不产生撤销记录。
    /// 修改插入比例（块参照的 X/Y 比例；Z 保持不变）
    bool setEntitiesInsertScale(const std::vector<EntityId>& ids, double scaleX, double scaleY);
    /// 修改插入旋转（弧度）
    bool setEntitiesInsertRotation(const std::vector<EntityId>& ids, double radians);

    /// 以新几何整体替换某图元的几何（夹点拖拽结束后调用）
    bool replaceEntity(EntityId id,
                       std::unique_ptr<Entity> replacement,
                       const QString& actionLabel = QString());

    /// 分解块参照（INSERT）：把每个块参照换成它展开后的独立图元，单条撤销记录。
    /// 括号内的选择集里没有块参照时返回 false 且**不产生撤销记录**
    /// ——与 setEntitiesTextXxx 的口径一致，界面据此提示而不是留下空白操作。
    /// 返回被替换掉的块参照个数（out参数）。
    bool explodeEntities(const std::vector<EntityId>& ids, int* explodedCount = nullptr);

    // ---- 块定义表 --------------------------------------------------------------
    /// 块表：块参照按名解析定义。图元自身也持有定义的 shared_ptr，
    /// 因此从表里删除一条定义不会让既有块参照变成悬空引用。
    BlockTable& blocks() noexcept { return m_blocks; }
    const BlockTable& blocks() const noexcept { return m_blocks; }

    /// 文档实际引用到的全部块定义（含块参照私有的匿名副本），按名字去重、保序。
    /// 写出端必须用这个而不是 blocks().all()：块参照在非相似变换下会把几何
    /// 烘焙进一份**不在表里**的私有定义，只遍历表会把这些内容整块丢掉。
    std::vector<std::shared_ptr<BlockDefinition>> referencedBlocks() const;

    // ---- 图层 ----------------------------------------------------------------
    LayerManager& layers() noexcept { return m_layers; }
    const LayerManager& layers() const noexcept { return m_layers; }

    // ---- 文字样式 --------------------------------------------------------------
    /// 文字样式表：图元只带样式名（DXF 7），字体按名在这里解析
    TextStyleTable& textStyles() noexcept { return m_textStyles; }
    const TextStyleTable& textStyles() const noexcept { return m_textStyles; }

    /// 把某个字体族登记为一条同名文字样式（幂等、不进撤销栈）。
    ///
    /// 字体信息是文档级数据：图元只引用样式名，字体本体存在 STYLE 表里。
    /// 若只改图元的样式名而不登记样式，导出时该样式没有字体信息，
    /// 会被兜底成 txt.shx——字体选择在文件往返后就丢了。
    void ensureTextFontStyle(const QString& fontFamily);

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
    /// 块定义表发生变化（新建/删除块定义、批量导入完成）
    void blocksChanged();
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
    friend class TextPropertyCommand;
    friend class InsertPropertyCommand;
    friend class ReplaceEntityCommand;
    friend class LayerTableCommand;
    friend class ExplodeEntitiesCommand;

    /// 图元集合变化后统一发出的收尾信号
    void afterEntitiesMutated();

    std::unordered_map<EntityId, std::unique_ptr<Entity>> m_entities;
    std::vector<EntityId> m_order;
    EntityId m_nextId = 1;

    LayerManager m_layers;
    TextStyleTable m_textStyles;
    BlockTable m_blocks;
    Selection* m_selection = nullptr;
    QUndoStack* m_undoStack = nullptr;

    QString m_filePath;
    int m_batchDepth = 0;
};

} // namespace lycad
