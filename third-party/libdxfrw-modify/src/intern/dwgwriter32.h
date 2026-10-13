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
//!  dwgwriter32.h —— DWG **R2018 (AC1032)** 文件容器入口。
//!
//!  与 R2004 / R2010 / R2013 **同族容器**：0x80 加密文件头（"AcFssFcAJMB"）、
//!  页映射 / 段映射、分页数据。libredwg 的 decode.c 对 `SINCE (R_2010b)`
//!  一律走 decode_R2004，故本模块只是把 AC1032 转交给 dwgWriter18 的摆放代码。
//!
//!  版本常量（libredwg 版本表 R_2018）：dwg_version = 0x21、maint_rel_version = 0x1d、
//!  内部版本号 33。
//!
#ifndef DWGWRITER32_H
#define DWGWRITER32_H

#include "../drw_base.h"            // DRW::Version

class dwgW;
class DRW_Interface;

namespace dwgWriter32 {

/// 本实现是否支持目标版本（AC1032 = AutoCAD 2018）。
bool supports(DRW::Version ver) noexcept;

/// 写出整个文件（AC1032 分页容器）。
bool write(dwgW& writer, DRW_Interface* iface);

} // namespace dwgWriter32

#endif // DWGWRITER32_H
