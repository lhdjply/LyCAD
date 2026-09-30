// =============================================================================
//  LyCAD - io/LibDxfRwAdapter.h
//  内置 libdxfrw 的适配层。
//
//  职责
//    把 libdxfrw 解析出的数据结构（DRW_* ）转换成 LyCAD 的实体模型。
//
//  边界
//    本头文件刻意**不引用** libdxfrw：第三方类型与它带来的宏定义
//    （M_PI、DRW_POSIX、dint32 等）只出现在 LibDxfRwAdapter.cpp 里。
//    这样 io 的其它源文件、乃至整个上层，都不会被第三方库污染，
//    将来更换或升级 DWG 解析实现时，改动面被限制在一个 .cpp 内。
//
//  能力边界（重要）
//    · 读取：DWG（R14 ~ 2018）与 DXF，均由 libdxfrw-modify 完成。
//    · 写出：DXF 由 libdxfrw-modify 完成（见 writeDxfWithLibDxfRw）；
//      它**不提供 DWG 写出**，DWG 导出仍由外部转换器
//      （ODA File Converter / LibreDWG）承担。
//
//  许可证
//    libdxfrw-modify 为 GPL-2.0-or-later，与本项目一同分发时受其约束。
//    详见 third-party/libdxfrw-modify/COPYING 与 README 的「第三方组件」一节。
// =============================================================================
#pragma once

#include "io/CadCodec.h"

namespace lycad {

class Document;

/// 是否已编入 libdxfrw（构建期决定，见 LYCAD_WITH_LIBDXFRW）
bool libDxfRwAvailable();

/// 用内置 libdxfrw 原生读取 DWG。
///
/// 支持 R14 ~ 2018（AC1012 ~ AC1032）。超出该范围、或文件头不是 DWG 时
/// 明确报错，不做静默降级。
IoResult readDwgWithLibDxfRw(const QString& path, Document& document);

/// 用内置 libdxfrw 读取 DXF。
///
/// 这是 DXF 的**正式读取路径**（`DxfCodec::read` 直接调用它）：
/// ASCII 与二进制 DXF 都由 libdxfrw-modify 自动识别。
IoResult readDxfWithLibDxfRw(const QString& path, Document& document);

/// 用 libdxfrw-modify 写出 DXF（`DxfCodec::write` 的正式路径）。
///
/// 支持两个目标版本：AC1009 (R12) 与 AC1015 (R2000)。
/// 保真决策在该层完成：尺寸标注同时写出匿名几何块（*Dnn）、
/// R12 不支持的填充降级为闭合多段线并计数上报、
/// 文字以目标代码页（简中 GBK）编码写出。
IoResult writeDxfWithLibDxfRw(const QString& path, const Document& document,
                              const WriteOptions& options);

} // namespace lycad
