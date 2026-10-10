/******************************************************************************
**  libDXFrw - Library to read/write DXF files (ascii & binary)              **
**                                                                           **
**  Copyright (C) 2011-2015 José F. Soriano, rallazz@gmail.com               **
**                                                                           **
**  This library is free software, licensed under the terms of the GNU       **
**  General Public License as published by the Free Software Foundation,     **
**  either version 2 of the License, or (at your option) any later version.  **
**  You should have received a copy of the GNU General Public License        **
**  along with this program.  If not, see <http://www.gnu.org/licenses/>.    **
******************************************************************************/
//!
//!  dwgwriter15.h —— DWG **R2000 (AC1015)** 文件容器入口。
//!
//!  与读取端 intern/dwgreader15.h 对称：数字 15 指 DWG 的**内部版本号**
//!  （= AC1015 = AutoCAD 2000），不是年份。
//!
//!  分工（与读取端 dwgR / dwgreader.h / dwgreader15.h 三层一致）
//!    · dwgW（声明与实现均在 ../libdwgrw.h / ../libdwgrw.cpp）—— 对外接口，
//!      与 dxfRW 写出侧同构；与版本无关的写出逻辑与节构建全在那一处。
//!    · 本模块（dwgwriter15.cpp）—— 仅 AC1015 的**线性节容器**：
//!      版本头、6 条节定位记录、AuxHeader、缩略图、header/classes 节、
//!      对象流、对象映射、第二头部、R14 模板节与各处 CRC。
//!    · ../intern/dwgwriter.h —— 与版本无关的位级原语（dwgBitWriter / CRC /
//!      哨兵），由各版本容器共用。
//!
//!  节载荷由 dwgW::buildSections() 统一构建（见 ../libdwgrw.cpp），
//!  本模块只消费 dwgW::SectionData，不直接触碰写出器的内部状态。
//!
//!  新增目标版本：仿照本文件与 dwgwriter18.h 建立 dwgwriterNN.h/.cpp，
//!  并在 libdwgrw.cpp 的 dwgW::write() 分派处追加一个分支。
//!
#ifndef DWGWRITER15_H
#define DWGWRITER15_H

#include "../drw_base.h"            // DRW::Version

class dwgW;
class DRW_Interface;

namespace dwgWriter15 {

/// 本实现是否支持目标版本。当前仅 AC1015（AutoCAD 2000 / R2000）。
bool supports(DRW::Version ver) noexcept;

/// 写出整个文件（AC1015 线性容器）。
/// 前置条件：writer 已构造完毕；iface 非空；ver 已由 supports() 校验通过。
/// 节载荷由 dwgW::buildSections() 构建，本函数只负责摆放、哨兵与 CRC 回填。
/// 返回 false 表示写出失败（文件不可写、内部状态不一致等）。
bool write(dwgW& writer, DRW_Interface* iface);

} // namespace dwgWriter15

#endif // DWGWRITER15_H
