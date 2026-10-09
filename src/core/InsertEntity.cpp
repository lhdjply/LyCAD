// =============================================================================
//  LyCAD - core/InsertEntity.cpp
// =============================================================================
#include "core/InsertEntity.h"

#include "core/Format.h"
#include "core/TextEntity.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace lycad {

namespace {

/// 烘焙（非相似变换落到块参照上）时给私有块定义起的匿名名。
/// 前缀 `*` 是 DXF 约定的「匿名块」写法，别的 CAD 不会把它列进块列表。
QString anonymousBlockName()
{
    static std::atomic<unsigned> counter{0};
    return QStringLiteral("*U%1").arg(++counter);
}

/// 合并两个包围盒
void expandBy(Box2D& target, const Box2D& box)
{
    target.expand(box);
}

} // namespace

// -----------------------------------------------------------------------------
//  构造与插入参数
// -----------------------------------------------------------------------------
InsertEntity::InsertEntity()
    : Entity(EntityType::Insert)
{
}

InsertEntity::InsertEntity(std::shared_ptr<BlockDefinition> block)
    : Entity(EntityType::Insert), m_block(std::move(block))
{
    syncAttributesFromDefinitions();
}

void InsertEntity::invalidateCache() const
{
    m_cacheValid = false;
    m_cache.clear();
}

void InsertEntity::setBlock(std::shared_ptr<BlockDefinition> block)
{
    m_block = std::move(block);
    if (m_block) m_blockName = m_block->name();
    invalidateCache();
    syncAttributesFromDefinitions();
}

QString InsertEntity::blockName() const
{
    return m_block ? m_block->name() : m_blockName;
}

void InsertEntity::setBlockName(const QString& name)
{
    m_blockName = name;
}

void InsertEntity::applyAttributeDelta(const Transform2D& delta)
{
    if (m_attributes.empty() || delta.isIdentity()) return;

    for (Attribute& attribute : m_attributes) {
        const Vector2D direction = delta.applyVector(
            Vector2D(std::cos(attribute.rotation), std::sin(attribute.rotation)));
        attribute.position = delta.apply(attribute.position);
        const double length = direction.length();
        if (length > kGeomTol) {
            attribute.height *= length;
            attribute.rotation = normalizeAngle(direction.angle());
        }
    }
}

void InsertEntity::setInsertionPoint(const Point2D& p)
{
    if (m_point == p) return;
    applyAttributeDelta(Transform2D::translation(p - m_point));
    m_point = p;
    invalidateCache();
}

void InsertEntity::setScale(double sx, double sy, double sz)
{
    // 比例为 0 会把块压成一条线甚至一个点，几何不可逆；按 1 处理
    if (std::abs(sx) <= kGeomTol) sx = 1.0;
    if (std::abs(sy) <= kGeomTol) sy = 1.0;
    if (std::abs(sz) <= kGeomTol) sz = 1.0;
    if (m_scaleX == sx && m_scaleY == sy && m_scaleZ == sz) return;

    // 属性要跟着比例变化走。变化量是「绕插入点、在块参照自身的旋转坐标系里
    // 把 X/Y 各缩放 sx'/sx、sy'/sy」——直接按世界轴缩放会让旋转过的块参照
    // 把属性推到错误的方向上去。
    if (!m_attributes.empty()) {
        const Transform2D delta =
            Transform2D::translation(m_point.x, m_point.y)
            * Transform2D::rotation(m_rotation)
            * Transform2D::scaling(sx / m_scaleX, sy / m_scaleY)
            * Transform2D::rotation(-m_rotation)
            * Transform2D::translation(-m_point.x, -m_point.y);
        applyAttributeDelta(delta);
    }

    m_scaleX = sx;
    m_scaleY = sy;
    m_scaleZ = sz;
    invalidateCache();
}

void InsertEntity::setRotation(double radians)
{
    const double normalized = normalizeAngle(radians);
    if (m_rotation == normalized) return;
    applyAttributeDelta(Transform2D::rotationAround(m_point, normalized - m_rotation));
    m_rotation = normalized;
    invalidateCache();
}

void InsertEntity::setColumnCount(int count)
{
    const int value = std::max(1, count);
    if (m_columns == value) return;
    m_columns = value;
    invalidateCache();
}

void InsertEntity::setRowCount(int count)
{
    const int value = std::max(1, count);
    if (m_rows == value) return;
    m_rows = value;
    invalidateCache();
}

void InsertEntity::setColumnSpacing(double spacing)
{
    if (m_columnSpacing == spacing) return;
    m_columnSpacing = spacing;
    invalidateCache();
}

void InsertEntity::setRowSpacing(double spacing)
{
    if (m_rowSpacing == spacing) return;
    m_rowSpacing = spacing;
    invalidateCache();
}

// -----------------------------------------------------------------------------
//  变换
// -----------------------------------------------------------------------------
Transform2D InsertEntity::blockToParentTransform() const
{
    const Point2D base = m_block ? m_block->basePoint() : Point2D(0.0, 0.0);
    return Transform2D::translation(m_point.x, m_point.y)
         * Transform2D::rotation(m_rotation)
         * Transform2D::scaling(m_scaleX, m_scaleY)
         * Transform2D::translation(-base.x, -base.y);
}

Transform2D InsertEntity::instanceTransform(int row, int col) const
{
    const Transform2D base = blockToParentTransform();
    if (row == 0 && col == 0) return base;

    // 阵列偏移加在插入点上（与 DXF 的 MINSERT 组码 44/45 对应）
    return Transform2D::translation(col * m_columnSpacing, row * m_rowSpacing) * base;
}

void InsertEntity::transform(const Transform2D& t)
{
    const Transform2D linear(t.a, t.b, t.c, t.d, 0.0, 0.0);
    const Transform2D current =
        Transform2D::rotation(m_rotation) * Transform2D::scaling(m_scaleX, m_scaleY);
    const Transform2D combined = linear * current;

    // 块参照的线性部分恒为 R(θ)·S(sx,sy)（两列正交）。左乘相似变换后两列仍正交，
    // 因此总能重新分解；出现错切（非等比拉伸）或阵列（偏移与插入参数同处一个
    // 坐标系，旋转后不再是一条轴上的等距点列）时无法用 5 个插入参数表达，
    // 只能把结果烘焙进一份私有块定义副本——几何精确，代价是这一处不再是「引用」。
    const double a = combined.a;
    const double b = combined.b;
    const double c = combined.c;
    const double d = combined.d;

    const double sx = std::hypot(a, b);
    const double det = a * d - b * c;
    const double colDot = a * c + b * d;
    const double magnitude = std::max({ sx * sx, std::hypot(c, d) * std::hypot(c, d), kGeomTol });

    const bool decomposable = sx > kGeomTol
        && std::abs(colDot) <= 1e-9 * magnitude
        && std::abs(det) > kGeomTol;

    if (!decomposable || isArray()) {
        bakeIntoPrivateBlock(t);
        return;
    }

    const double theta = std::atan2(b, a);
    const double sy = det / sx;

    // 分解结果自检：与目标矩阵不一致时退回烘焙，宁可多复制也不能画错
    const Transform2D check = Transform2D::rotation(theta) * Transform2D::scaling(sx, sy);
    const double residual = std::max({ std::abs(check.a - a), std::abs(check.b - b),
                                       std::abs(check.c - c), std::abs(check.d - d) });
    if (residual > 1e-9 * std::max(1.0, magnitude)) {
        bakeIntoPrivateBlock(t);
        return;
    }

    const double uniform = t.uniformScale();
    m_scaleZ *= (t.determinant() < 0.0 ? -uniform : uniform);

    m_point = t.apply(m_point);
    m_rotation = normalizeAngle(theta);
    m_scaleX = sx;
    m_scaleY = sy;

    // 属性存的是世界坐标，跟随同一个变换
    applyAttributeDelta(t);

    invalidateCache();
}

void InsertEntity::bakeIntoPrivateBlock(const Transform2D& t)
{
    // 先把当前几何收集出来（不含属性文字——属性另有存储，烘焙后仍要保留）
    std::vector<std::unique_ptr<Entity>> content;
    collectInto(content, Transform2D::identity(), layerName(), 0, kMaxNestingDepth, false);

    auto privateBlock = std::make_unique<BlockDefinition>(anonymousBlockName());
    privateBlock->setFlags(BlockDefinition::Anonymous);
    privateBlock->setBasePoint(Point2D(0.0, 0.0));
    for (std::unique_ptr<Entity>& entity : content) {
        entity->transform(t);
        privateBlock->addEntity(std::move(entity));
    }

    applyAttributeDelta(t);

    m_block = std::move(privateBlock);
    m_point = Point2D(0.0, 0.0);
    m_scaleX = m_scaleY = m_scaleZ = 1.0;
    m_rotation = 0.0;
    m_columns = 1;
    m_rows = 1;
    m_columnSpacing = 0.0;
    m_rowSpacing = 0.0;
    invalidateCache();
}

// -----------------------------------------------------------------------------
//  展开
// -----------------------------------------------------------------------------
void InsertEntity::collectInto(std::vector<std::unique_ptr<Entity>>& out,
                               const Transform2D& toParent,
                               const QString& inheritedLayer,
                               std::size_t depth,
                               std::size_t maxDepth,
                               bool includeAttributes) const
{
    if (!m_block) return;
    if (depth > maxDepth) return;      // 自引用块 / 嵌套过深：截断而不是无限递归

    const int selfColor = colorIndex();
    const QString selfLinetype = linetypeName();
    const int selfLineweight = lineweightDxf();

    for (int row = 0; row < m_rows; ++row) {
        for (int col = 0; col < m_columns; ++col) {
            const Transform2D toWorld = toParent * instanceTransform(row, col);

            for (const std::unique_ptr<Entity>& source : m_block->entities()) {
                if (!source) continue;

                if (source->type() == EntityType::Insert) {
                    const auto& nested = static_cast<const InsertEntity&>(*source);
                    QString nestedLayer = nested.layerName();
                    if (nestedLayer.isEmpty() || nestedLayer == QLatin1String("0")) {
                        nestedLayer = inheritedLayer;
                    }
                    nested.collectInto(out, toWorld, nestedLayer, depth + 1, maxDepth,
                                       includeAttributes);
                    continue;
                }

                std::unique_ptr<Entity> copy = source->clone();

                // 块内位于 "0" 层的图元继承 INSERT 所在图层（DXF 标准语义）
                if (copy->layerName() == QLatin1String("0") && !inheritedLayer.isEmpty()) {
                    copy->setLayerName(inheritedLayer);
                }

                // 随块（ByBlock）：取块参照自身的属性；块参照也是随层时保持随层，
                // 由图层解析接管——与 AutoCAD 的解析顺序一致。
                if (copy->isColorByBlock()) {
                    if (selfColor <= 0) copy->setColorByLayer();
                    else                copy->setColorIndex(selfColor);
                }
                if (copy->lineweightDxf() == lineweight::kByBlock) {
                    copy->setLineweightDxf(selfLineweight);
                }
                if (copy->linetypeName().compare(QLatin1String("ByBlock"), Qt::CaseInsensitive) == 0) {
                    copy->setLinetypeName(selfLinetype);
                }

                copy->transform(toWorld);
                out.push_back(std::move(copy));
            }
        }
    }

    if (!includeAttributes) return;

    // 属性：坐标**本来就在父坐标系**（对模型空间的块参照即世界坐标），
    // 因此只叠加「本层 -> 父坐标系」的外层变换，绝不能再乘一次自身的
    // instanceTransform——那等于把已经变换过的位置又变换一遍，属性会飞到
    // 几百万单位之外去（实测某图纸的块参照范围因此从 2.7e6 变成 2.1e9）。
    const Transform2D attributeTransform = toParent;
    for (const Attribute& attribute : m_attributes) {
        if (attribute.isInvisible() || attribute.value.isEmpty()) continue;

        auto text = std::make_unique<TextEntity>();
        text->setContent(attribute.value);

        const Vector2D direction = attributeTransform.applyVector(
            Vector2D(std::cos(attribute.rotation), std::sin(attribute.rotation)));
        const double length = direction.length();
        text->setHeight(length > kGeomTol ? attribute.height * length : attribute.height);
        text->setRotation(length > kGeomTol ? direction.angle() : attribute.rotation);
        text->setPosition(attributeTransform.apply(attribute.position));
        text->setAlignmentPoint(text->position());
        if (!attribute.style.isEmpty()) text->setFontStyle(attribute.style);

        if (!attribute.layer.isEmpty() && attribute.layer != QLatin1String("0")) {
            text->setLayerName(attribute.layer);
        } else if (!inheritedLayer.isEmpty()) {
            text->setLayerName(inheritedLayer);
        }

        if (selfColor > 0) text->setColorIndex(selfColor);

        out.push_back(std::move(text));
    }
}

const std::vector<std::unique_ptr<Entity>>& InsertEntity::cached() const
{
    if (!m_cacheValid) {
        m_cache.clear();
        collectInto(m_cache, Transform2D::identity(), layerName(), 0, kMaxNestingDepth, true);
        m_cacheValid = true;
    }
    return m_cache;
}

std::vector<std::unique_ptr<Entity>> InsertEntity::explode(std::size_t maxDepth) const
{
    if (maxDepth >= kMaxNestingDepth) {
        // 常规路径走缓存：包围盒/拾取/渲染在同一帧里会反复查询
        const std::vector<std::unique_ptr<Entity>>& source = cached();
        std::vector<std::unique_ptr<Entity>> out;
        out.reserve(source.size());
        for (const std::unique_ptr<Entity>& entity : source) {
            if (entity) out.push_back(entity->clone());
        }
        return out;
    }

    std::vector<std::unique_ptr<Entity>> out;
    collectInto(out, Transform2D::identity(), layerName(), 0, maxDepth, true);
    return out;
}

const std::vector<std::unique_ptr<Entity>>& InsertEntity::expanded() const
{
    return cached();
}

std::size_t InsertEntity::expandedCount(std::size_t maxDepth) const
{
    if (maxDepth >= kMaxNestingDepth) return cached().size();
    std::vector<std::unique_ptr<Entity>> out;
    collectInto(out, Transform2D::identity(), layerName(), 0, maxDepth, true);
    return out.size();
}

// -----------------------------------------------------------------------------
//  属性
// -----------------------------------------------------------------------------
void InsertEntity::setAttributes(std::vector<Attribute> attributes)
{
    m_attributes = std::move(attributes);
    invalidateCache();
}

void InsertEntity::addAttribute(const Attribute& attribute)
{
    m_attributes.push_back(attribute);
    invalidateCache();
}

const Attribute* InsertEntity::findAttribute(const QString& tag) const
{
    const QString key = tag.trimmed();
    for (const Attribute& attribute : m_attributes) {
        if (attribute.tag.compare(key, Qt::CaseInsensitive) == 0) return &attribute;
    }
    return nullptr;
}

Attribute* InsertEntity::findAttribute(const QString& tag)
{
    const QString key = tag.trimmed();
    for (Attribute& attribute : m_attributes) {
        if (attribute.tag.compare(key, Qt::CaseInsensitive) == 0) return &attribute;
    }
    return nullptr;
}

bool InsertEntity::setAttributeValue(const QString& tag, const QString& value)
{
    Attribute* attribute = findAttribute(tag);
    if (!attribute) return false;
    if (attribute->value == value) return true;
    attribute->value = value;
    invalidateCache();
    return true;
}

void InsertEntity::setAttribute(const Attribute& attribute)
{
    if (Attribute* existing = findAttribute(attribute.tag)) {
        *existing = attribute;
    } else {
        m_attributes.push_back(attribute);
    }
    invalidateCache();
}

void InsertEntity::syncAttributesFromDefinitions()
{
    if (!m_block) return;

    // 已有的属性（从文件里读到的）按标签建索引，其余按定义补齐
    for (const AttributeDefinition& definition : m_block->attributeDefinitions()) {
        if (definition.isConstant()) continue;      // 常量属性不生成 ATTRIB
        if (findAttribute(definition.tag)) continue;

        Attribute attribute;
        attribute.tag = definition.tag;
        attribute.value = definition.defaultValue;
        attribute.height = std::max(definition.height, kGeomTol);
        attribute.rotation = definition.rotation;
        attribute.style = definition.style;
        attribute.flags = definition.flags;
        attribute.layer = layerName();

        // ATTDEF 的坐标在块坐标系；按插入参数换算到世界坐标（含字高与旋转）
        const Transform2D placement = blockToParentTransform();
        attribute.position = placement.apply(definition.position);
        const Vector2D direction = placement.applyVector(
            Vector2D(std::cos(definition.rotation), std::sin(definition.rotation)));
        const double length = direction.length();
        if (length > kGeomTol) {
            attribute.height *= length;
            attribute.rotation = normalizeAngle(direction.angle());
        }

        m_attributes.push_back(attribute);
    }

    invalidateCache();
}

// -----------------------------------------------------------------------------
//  Entity 接口
// -----------------------------------------------------------------------------
std::unique_ptr<Entity> InsertEntity::clone() const
{
    auto copy = std::make_unique<InsertEntity>();
    copyCommonPropertiesTo(*copy);

    // 共享同一份块定义（引用语义），而不是深拷贝块内容
    copy->m_block = m_block;
    copy->m_blockName = m_blockName;
    copy->m_point = m_point;
    copy->m_scaleX = m_scaleX;
    copy->m_scaleY = m_scaleY;
    copy->m_scaleZ = m_scaleZ;
    copy->m_rotation = m_rotation;
    copy->m_columns = m_columns;
    copy->m_rows = m_rows;
    copy->m_columnSpacing = m_columnSpacing;
    copy->m_rowSpacing = m_rowSpacing;
    copy->m_attributes = m_attributes;
    return copy;
}

Box2D InsertEntity::boundingBox() const
{
    Box2D box;
    for (const std::unique_ptr<Entity>& entity : cached()) {
        if (!entity) continue;
        // 无界图元（块内画了构造线/射线）没有有限范围，跳过其包围盒，
        // 否则整个块参照会被判成「无界」，范围缩放与出图都会失真。
        if (entity->isUnbounded()) continue;
        expandBy(box, entity->boundingBox());
    }
    return box;
}

double InsertEntity::distanceTo(const Point2D& p) const
{
    double best = -1.0;
    for (const std::unique_ptr<Entity>& entity : cached()) {
        if (!entity) continue;
        const double distance = entity->distanceTo(p);
        if (best < 0.0 || distance < best) best = distance;
    }
    return best < 0.0 ? std::numeric_limits<double>::max() : best;
}

Point2D InsertEntity::closestPointTo(const Point2D& p) const
{
    Point2D best = p;
    double bestDistance = -1.0;
    for (const std::unique_ptr<Entity>& entity : cached()) {
        if (!entity) continue;
        const Point2D candidate = entity->closestPointTo(p);
        const double distance = candidate.distanceTo(p);
        if (bestDistance < 0.0 || distance < bestDistance) {
            bestDistance = distance;
            best = candidate;
        }
    }
    return best;
}

bool InsertEntity::isInside(const Box2D& box) const
{
    const std::vector<std::unique_ptr<Entity>>& children = cached();
    if (children.empty() || !box.isValid()) return false;
    for (const std::unique_ptr<Entity>& entity : children) {
        if (!entity) continue;
        if (!entity->isInside(box)) return false;
    }
    return true;
}

bool InsertEntity::crosses(const Box2D& box) const
{
    for (const std::unique_ptr<Entity>& entity : cached()) {
        if (!entity) continue;
        if (entity->crosses(box)) return true;
    }
    return false;
}

double InsertEntity::length() const
{
    double total = 0.0;
    for (const std::unique_ptr<Entity>& entity : cached()) {
        if (entity) total += entity->length();
    }
    return total;
}

std::vector<Point2D> InsertEntity::snapPoints() const
{
    // 捕捉点直接取自展开后的子图元。上限用于挡住「块里几万个图元」的病态情形：
    // 捕捉本身是光标邻域查询，超出上限的部分距离光标只会更远。
    constexpr std::size_t kMaxSnapPoints = 512;

    std::vector<Point2D> points;
    for (const std::unique_ptr<Entity>& entity : cached()) {
        if (!entity) continue;
        const std::vector<Point2D> child = entity->snapPoints();
        points.insert(points.end(), child.begin(), child.end());
        if (points.size() >= kMaxSnapPoints) {
            points.resize(kMaxSnapPoints);
            break;
        }
    }
    return points;
}

std::vector<Point2D> InsertEntity::gripPoints() const
{
    // 块参照只有一个夹点：插入点。其余顶点由块定义决定，拖动它们没有意义。
    return { m_point };
}

bool InsertEntity::moveGrip(std::size_t index, const Point2D& newPos)
{
    if (index != 0) return false;
    setInsertionPoint(newPos);
    return true;
}

std::vector<Point2D> InsertEntity::flatten(double sagitta) const
{
    std::vector<Point2D> points;
    for (const std::unique_ptr<Entity>& entity : cached()) {
        if (!entity) continue;
        const std::vector<Point2D> child = entity->flatten(sagitta);
        points.insert(points.end(), child.begin(), child.end());
    }
    return points;
}

QString InsertEntity::describe() const
{
    if (!m_block) {
        return QStringLiteral("块参照（块定义缺失）  插入点 %1").arg(formatPoint(m_point));
    }
    return QStringLiteral("块参照 \"%1\"  插入点 %2  比例 %3×%4  旋转 %5")
        .arg(m_block->name(), formatPoint(m_point))
        .arg(m_scaleX, 0, 'g', 6)
        .arg(m_scaleY, 0, 'g', 6)
        .arg(formatAngle(m_rotation));
}

QString InsertEntity::geometrySummary() const
{
    if (!m_block) {
        return QStringLiteral("块名: (缺失)\n插入点: %1").arg(formatPoint(m_point));
    }

    QString summary = QStringLiteral("块名: %1\n插入点: %2\n比例: %3, %4, %5\n旋转: %6\n"
                                     "块内图元: %7\n展开后图元: %8")
                          .arg(m_block->name(), formatPoint(m_point))
                          .arg(m_scaleX, 0, 'g', 6)
                          .arg(m_scaleY, 0, 'g', 6)
                          .arg(m_scaleZ, 0, 'g', 6)
                          .arg(formatAngle(m_rotation))
                          .arg(m_block->entityCount())
                          .arg(expandedCount());

    if (isArray()) {
        summary += QStringLiteral("\n阵列: %1 行 × %2 列（列距 %3，行距 %4）")
                       .arg(m_rows)
                       .arg(m_columns)
                       .arg(m_columnSpacing, 0, 'g', 6)
                       .arg(m_rowSpacing, 0, 'g', 6);
    }

    if (!m_attributes.empty()) {
        summary += QStringLiteral("\n属性: %1 个").arg(m_attributes.size());
        for (const Attribute& attribute : m_attributes) {
            summary += QStringLiteral("\n  %1 = %2").arg(attribute.tag, attribute.value);
        }
    }
    return summary;
}

} // namespace lycad
