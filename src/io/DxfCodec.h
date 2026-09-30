// =============================================================================
//  LyCAD - io/DxfCodec.h
//  DXF 编解码器：把 DxfReader / DxfWriter 适配到 ICadCodec 接口。
// =============================================================================
#pragma once

#include "io/CadCodec.h"

namespace lycad {

class DxfCodec : public ICadCodec {
public:
    DxfCodec() = default;
    ~DxfCodec() override = default;

    QString name() const override;
    QString displayName() const override;

    QStringList readExtensions() const override;
    QStringList writeExtensions() const override;

    bool probe(QIODevice& device) const override;

    IoResult read(const QString& path, Document& document) const override;
    IoResult write(const QString& path, const Document& document,
                   const WriteOptions& options) const override;
};

} // namespace lycad
