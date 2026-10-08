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
//      DWG 亦由 libdxfrw-modify 完成（见 writeDwgWithLibDxfRw，dwgW，
//      目标版本 R2000/AC1015）。DWG 的读与写都不依赖外部程序。
//
//  许可证
//    libdxfrw-modify 为 GPL-2.0-or-later，与本项目一同分发时受其约束。
//    详见 third-party/libdxfrw-modify/COPYING 与 README 的「第三方组件」一节。
// =============================================================================
#pragma once

#include "io/CadCodec.h"

namespace lycad {

class Document;

/// libdxfrw 是否可用。其源码随项目一起编译，因此恒为 true——
/// 保留该查询只为让「解析器是否在位」在调用处显式可见。
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
/// 目标版本取自 `WriteOptions::availableDxfVersions()`：
/// AC1015/AC1018/AC1021/AC1024/AC1027/AC1032
/// （AutoCAD 2000 / 2004 / 2007 / 2010 / 2013 / 2018）。
/// **R12 (AC1009) 不在其中**：它缺 HATCH/SPLINE/ELLIPSE/XLINE/RAY，也没有
/// TrueType 与 MTEXT，写出时得整体降级改写，这条通道已移除——R12 只读不写。
/// 保真决策在该层完成：尺寸标注同时写出匿名几何块（*Dnn）；
/// 2000 ~ 2004 的文字以目标代码页（简中 GBK）编码写出，2007 及以后一律 UTF-8。
IoResult writeDxfWithLibDxfRw(const QString& path, const Document& document,
                              const WriteOptions& options);

/// 用 libdxfrw-modify 的 dwgW 原生写出 DWG（`DwgCodec::write` 的首选路径）。
///
/// 目标版本固定为 R2000 (AC1015)：与 DXF 中间格式一致，也是开放资料最
/// 充分、兼容面最广的写出版本。实体复用与 DXF 相同的导出器（Exporter），
/// 表记录、匿名标注块、模型空间块结构由 dwgW 负责组织。
IoResult writeDwgWithLibDxfRw(const QString& path, const Document& document,
                              const WriteOptions& options);

} // namespace lycad
