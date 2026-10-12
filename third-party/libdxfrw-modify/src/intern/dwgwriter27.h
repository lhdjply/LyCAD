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
//!  dwgwriter27.h —— DWG **R2013 (AC1027)** 文件容器入口。
//!
//!  与 R2004（AC1018）/ R2010（AC1024）**同族容器**：0x80 加密文件头
//!  （"AcFssFcAJMB"）、页映射 / 段映射、分页数据。依据来自 libredwg 的
//!  decode.c —— `SINCE (R_2010b)` 走 decode_R2004（注释写着 "for 2004 and
//!  2010+"），而 decode_R2007 只服务 R2007。也就是说 R2010 及以后（含 R2013）
//!  都用 R2004 族容器，R2007 的文件头页容器不适用。
//!
//!  因此本模块只是把 AC1027 转交给 dwgWriter18 的摆放代码，容器层的差异
//!  （版本串 "AC1027"、0x0B=0x7d / 0x11=0x1f、内部版本号 31）由 writeAs
//!  按版本取值。
//!
#ifndef DWGWRITER27_H
#define DWGWRITER27_H

#include "../drw_base.h"            // DRW::Version

class dwgW;
class DRW_Interface;

namespace dwgWriter27 {

/// 本实现是否支持目标版本（AC1027 = AutoCAD 2013）。
bool supports(DRW::Version ver) noexcept;

/// 写出整个文件（AC1027 分页容器）。
bool write(dwgW& writer, DRW_Interface* iface);

} // namespace dwgWriter27

#endif // DWGWRITER27_H
