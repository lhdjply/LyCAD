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
//!  dwgwriter24.h —— DWG **R2010 (AC1024)** 文件容器入口。
//!
//!  与 R2004（AC1018）**同族容器**：0x80 加密文件头（"AcFssFcAJMB"）、
//!  页映射 / 段映射、分页数据。与 R2007 的文件头页容器**不同族**——这一点由
//!  libredwg 的 decode.c 印证：`SINCE (R_2010b)` 走 decode_R2004，
//!  而 decode_R2007 只服务 R2007。详见 dwgwriter24.cpp 的说明。
//!
//!  因此本模块只是把 AC1024 转交给 dwgWriter18 的摆放代码。
//!
#ifndef DWGWRITER24_H
#define DWGWRITER24_H

#include "../drw_base.h"            // DRW::Version

class dwgW;
class DRW_Interface;

namespace dwgWriter24 {

/// 本实现是否支持目标版本（AC1024 = AutoCAD 2010）。
bool supports(DRW::Version ver) noexcept;

/// 写出整个文件（AC1024 分页容器）。
bool write(dwgW& writer, DRW_Interface* iface);

} // namespace dwgWriter24

#endif // DWGWRITER24_H
