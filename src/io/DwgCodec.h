// =============================================================================
//  LyCAD - io/DwgCodec.h
//  DWG 编解码器。
//
//  关于 DWG 支持方式的说明（重要）
//
//    DWG 是 Autodesk 的闭源二进制格式：位压缩编码、分节映射、对象级 CRC 校验，
//    且各版本编码方式差异显著。从零实现一个可靠的 DWG 解析器属于独立工程，
//    不属于本项目的合理范围，硬做出来的半成品反而会静默损坏用户图纸。
//
//    因此本模块采用业界通行做法——「后端适配器」：
//      · 优先使用系统已安装的转换器（LibreDWG 的 dwg2dxf/dxf2dwg、ODA File
//        Converter），先转到 DXF，再由本软件自带的 DXF 编解码器完成实际装载；
//      · 支持用户在设置中指定自定义转换命令；
//      · 若一个后端都不可用，明确告知用户原因与解决办法，绝不静默降级。
//
//    这样做的直接收益：DWG 的读写路径完全复用已经过验证的 DXF 实现，
//    数据正确性只取决于转换器本身的保真度，而软件的其余部分无需为此承担风险。
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
