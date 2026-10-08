// =============================================================================
//  LyCAD - io/CadCodec.h
//  图形文件编解码器接口与注册表。
//
//  设计意图
//    文件格式以「编解码器」形式插件化：核心与界面层只面向 ICadCodec 与
//    CodecRegistry 编程，新增一种格式（如 DGN、SVG 输出）无需改动任何既有代码，
//    只需实现接口并注册。
//
//    这也是 DWG 支持得以工程化落地的关键：DWG 为闭源二进制格式，
//    本软件不重新实现其解析器，而是通过 DwgCodec 适配内置的 libdxfrw，
//    接口层保持不变。
// =============================================================================
#pragma once

#include "core/Types.h"

#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

class QIODevice;

namespace lycad {

class Document;

// -----------------------------------------------------------------------------
//  写出选项
// -----------------------------------------------------------------------------
struct WriteOptions {
    /// 目标格式版本。DXF 支持 R12 ~ 2018，取值见 availableDxfVersions()。
    /// 默认 AC1032（2018）：现代 CAD 都能打开，实体集完整，文本按 UTF-8 存放，
    /// 中文不再受代码页限制。DWG 的写出版本固定为 R2000，不受此项影响。
    QString version = QStringLiteral("AC1032");

    /// 输出可直接阅读的缩进版本（体积略增，便于人工排查）
    bool prettyPrint = false;

    /// 版本名称（含 AC 代号），供消息与提示展示
    static QString versionDisplayName(const QString& version);

    /// 版本短名（不含 AC 代号），用于「另存为」里逐版本的过滤器：
    /// 列表本身就是版本选择器，因此名字要短。
    static QString versionShortName(const QString& version);

    /// 可写出的 DXF 目标版本代号，按版本从低到高排列。
    /// 这是版本清单的唯一来源：界面、版本归一化、写出端都取自这里。
    static QStringList availableDxfVersions();
};

// -----------------------------------------------------------------------------
//  读写结果
// -----------------------------------------------------------------------------
struct IoResult {
    bool    ok = false;
    QString error;
    int     entityCount = 0;
    QStringList warnings;

    static IoResult success(int entityCount = 0);
    static IoResult failure(const QString& message);

    void addWarning(const QString& warning);
    bool hasWarnings() const { return !warnings.isEmpty(); }

    /// 供状态栏显示的一行摘要
    QString summary() const;
};

// -----------------------------------------------------------------------------
//  ICadCodec
// -----------------------------------------------------------------------------
class ICadCodec {
public:
    virtual ~ICadCodec();

    /// 短名，用于注册表查找，如 "DXF"
    virtual QString name() const = 0;

    /// 中文全名，用于文件对话框与关于信息
    virtual QString displayName() const = 0;

    /// 可读取的扩展名（小写，不含点）
    virtual QStringList readExtensions() const = 0;

    /// 可写出的扩展名（小写，不含点）
    virtual QStringList writeExtensions() const = 0;

    /// 是否具备读取能力（例如某个格式的解析器未编入时为 false）
    virtual bool canRead() const { return true; }

    /// 是否具备写出能力
    virtual bool canWrite() const { return true; }

    /// 能力与适用范围说明，界面据此向用户解释
    virtual QString capabilityNote() const { return QString(); }

    /// 扩展名匹配 **且** 当前具备读取能力
    bool canReadPath(const QString& path) const;
    /// 扩展名匹配 **且** 当前具备写出能力
    bool canWritePath(const QString& path) const;

    /// 仅判断扩展名是否属于本编解码器，不考虑当前是否可用。
    /// 用于「已知格式但暂时不可用」的场景——此时应向用户解释原因，
    /// 而不是把它当作完全未知的格式。
    bool hasReadExtension(const QString& path) const;
    bool hasWriteExtension(const QString& path) const;

    /// 基于文件头/内容快速判断是否为本格式。
    /// 实现须在返回前将 device 复位至起始位置（若可 seek）。
    virtual bool probe(QIODevice& device) const = 0;

    /// 读取整个文件并装配文档。失败时文档内容未定义，调用方应先备份。
    virtual IoResult read(const QString& path, Document& document) const = 0;

    /// 将文档写出至文件。
    virtual IoResult write(const QString& path, const Document& document,
                           const WriteOptions& options) const = 0;

    /// 供文件对话框使用的过滤器字符串。
    ///
    /// writeFilter 可被子类覆写：写出过滤器要能表达「写成什么」，
    /// 同一格式若有固定的目标版本（如 DWG 恒为 R2000），名字里就该点明，
    /// 否则用户只能看到「DWG 图形文件」而不知道写出的到底是哪一版。
    QString readFilter() const;
    virtual QString writeFilter() const;

protected:
    /// 由扩展名判定的辅助实现，供子类复用
    bool extensionMatches(const QString& path, const QStringList& extensions) const;
};

// -----------------------------------------------------------------------------
//  CodecRegistry
// -----------------------------------------------------------------------------
class CodecRegistry {
public:
    static CodecRegistry& instance();

    void registerCodec(std::unique_ptr<ICadCodec> codec);

    /// 按路径查找编解码器；forWriting 为 true 时只考虑具备写出能力者
    ICadCodec* codecForPath(const QString& path, bool forWriting) const;

    ICadCodec* codecByName(const QString& name) const;

    const std::vector<std::unique_ptr<ICadCodec>>& codecs() const { return m_codecs; }

    /// 形如 "DXF 图形交换文件 (*.dxf)" 的过滤器列表
    QStringList importFilters() const;
    QStringList exportFilters() const;

    /// 默认导入/导出过滤器（列表首项）
    QString defaultImportFilter() const;
    QString defaultExportFilter() const;

    /// 当前真正可读 / 可写的扩展名（受后端可用性影响）
    QStringList supportedReadExtensions() const;
    QStringList supportedWriteExtensions() const;

    /// 已注册的全部可读扩展名，不论当前是否可用
    QStringList knownReadExtensions() const;

    /// 是否已注册任何编解码器
    bool isEmpty() const { return m_codecs.empty(); }

private:
    CodecRegistry() = default;

    std::vector<std::unique_ptr<ICadCodec>> m_codecs;
};

/// 注册内置编解码器。幂等，可重复调用。
void registerBuiltinCodecs();

} // namespace lycad
