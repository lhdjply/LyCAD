// =============================================================================
//  LyCAD - io/DwgCodec.h
//  DWG 编解码器。
//
//  DWG 支持方式
//
//    导入：内置 libdxfrw（third-party/libdxfrw-modify）在进程内原生解析，支持 R14 ~ 2018
//          的 DWG。不依赖外部程序、不产生中间文件。内置解析失败时自动回退到
//          外部转换器，并把两边的结果都如实告知用户。
//
//    导出：libdxfrw 只读 DWG，**不提供写出能力**，因此导出仍由外部转换器
//          （LibreDWG / ODA File Converter / 用户自定义命令）转成 DXF 后再输出。
//
//    外部转换器全部不可用时，明确说明原因与解决办法，绝不静默降级。
//
//  与 DXF 链路的关系
//
//    本软件的 DXF 读与写都走 libdxfrw-modify（见 io/LibDxfRwAdapter.h）。
//    DWG 的解析走同一个库；DWG 导出所需的 DXF 中转文件由同一条写出链路生成。
// =============================================================================
#pragma once

#include "io/CadCodec.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <vector>

namespace lycad {

// -----------------------------------------------------------------------------
//  DWG 版本签名识别
// -----------------------------------------------------------------------------
struct DwgVersionInfo {
    QString code;           ///< 形如 "AC1015"
    QString displayName;    ///< 形如 "AutoCAD 2000"
    bool    recognized = false;
};

/// 由文件头 6 字节签名解析版本
DwgVersionInfo dwgVersionFromSignature(const QByteArray& header);

/// 读取文件头并解析版本；文件不可读时返回 recognized = false
DwgVersionInfo dwgVersionOfFile(const QString& path);

// -----------------------------------------------------------------------------
//  DwgCodec
// -----------------------------------------------------------------------------
class DwgCodec : public ICadCodec {
public:
    DwgCodec();
    ~DwgCodec() override = default;

    QString name() const override;
    QString displayName() const override;

    QStringList readExtensions() const override;
    QStringList writeExtensions() const override;

    bool canRead() const override;
    bool canWrite() const override;
    QString capabilityNote() const override;

    bool probe(QIODevice& device) const override;

    IoResult read(const QString& path, Document& document) const override;
    IoResult write(const QString& path, const Document& document,
                   const WriteOptions& options) const override;

    /// 当前探测到的可用后端名称（用于关于对话框与故障排查）
    QStringList availableBackendNames() const;

    /// 自定义转换命令模板所用的 QSettings 键名
    static QString customCommandSettingKey();

private:
    /// 一个转换器后端
    struct Backend {
        QString     displayName;
        QString     program;          ///< 可执行文件名
        QString     resolvedPath;     ///< 实际解析到的绝对路径
        QStringList readArgs;         ///< 含 %IN% / %OUT% 占位
        QStringList writeArgs;
        bool        odaStyle = false; ///< ODA File Converter 采用「目录进目录出」的 CLI
        bool        found = false;
        QString     note;
    };

    void detectBackends() const;

    /// 按优先级返回可用于读/写的后端；无可用后端返回 nullptr
    const Backend* backendForRead() const;
    const Backend* backendForWrite() const;

    IoResult runConverter(const Backend& backend, bool forReading,
                          const QString& input, const QString& output) const;

    /// ODA File Converter 采用「目录进、目录出」的命令行接口，单独实现
    IoResult runOdaConverter(const Backend& backend, bool forReading,
                             const QString& input, const QString& output) const;

    /// 用户自定义后端（来自设置），未配置时返回空
    static Backend customBackend();

    /// 通过中间 DXF 完成 DWG 读取
    IoResult readViaDxf(const QString& path, Document& document, const Backend& backend) const;

    /// 通过中间 DXF 完成 DWG 写出
    IoResult writeViaDxf(const QString& path, const Document& document,
                         const WriteOptions& options, const Backend& backend) const;

    mutable std::vector<Backend> m_backends;
    mutable bool m_detected = false;
};

} // namespace lycad
