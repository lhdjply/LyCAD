// =============================================================================
//  LyCAD - io/DwgCodec.h
//  DWG 编解码器。
//
//  DWG 支持方式
//
//    导入：内置 libdxfrw（third-party/libdxfrw-modify）在进程内原生解析，
//          支持 R14 ~ 2018（AC1012 ~ AC1032）的 DWG。
//
//    导出：内置 dwgW（libdxfrw-modify 内的 DWG 写出器）原生写出
//          R2000 (AC1015)。
//
//    读与写都在进程内完成：不依赖任何外部程序、不产生中间文件。
//    解析失败时如实上报原因，绝不静默降级。
//
//  与 DXF 链路的关系
//
//    本软件的 DXF 读与写都走 libdxfrw-modify（见 io/LibDxfRwAdapter.h）。
//    DWG 的读与写走同一个库：读取走 libdwgrw 的 dwgR，写出走 dwgW；
//    两者共用同一套实体导出器（Exporter）。
// =============================================================================
#pragma once

#include "io/CadCodec.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

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

    /// 可读写的版本范围（界面据此向用户展示，不表示能力受限）
    QString capabilityNote() const override;

    bool probe(QIODevice& device) const override;

    IoResult read(const QString& path, Document& document) const override;
    IoResult write(const QString& path, const Document& document,
                   const WriteOptions& options) const override;
};

} // namespace lycad
