// =============================================================================
//  LyCAD - io/CadFile.h
//  文件读写门面。
//
//  core 模块刻意不了解任何文件格式；界面层也不应直接接触编解码器。
//  所有「打开/保存为」的操作统一经由本门面，由它负责：
//    · 确保内置编解码器已注册；
//    · 按扩展名与文件内容选择正确的编解码器；
//    · 在读取失败时保持调用方文档不被半成品污染；
//    · 读取成功后同步文档的文件路径与「已修改」标记。
// =============================================================================
#pragma once

#include "io/CadCodec.h"

#include <QString>

namespace lycad {

class Document;

namespace CadFile {

/// 读取文件并装配文档。
///
/// 实现细节：解析在文档副本之外的临时流程中进行是不可行的（图元持有大量状态），
/// 因此这里采用「先清空、再装载」的策略，并在失败时把文档恢复为空白状态，
/// 同时通过返回值把错误原因完整回传给界面层。
IoResult load(const QString& path, Document& document);

/// 写出文档。options.version 为空时使用目标格式的默认版本。
IoResult save(const QString& path, const Document& document,
              const WriteOptions& options = WriteOptions());

/// 是否存在能读取 / 写出该路径的编解码器（并已具备相应能力）
bool canLoad(const QString& path);
bool canSave(const QString& path);

/// 供界面提示：该扩展名已知但当前不可用（典型场景：未安装 DWG 后端）
QString capabilityNoteFor(const QString& path);

} // namespace CadFile

/// 注册内置编解码器。幂等，可重复调用。
void registerBuiltinCodecs();

} // namespace lycad
