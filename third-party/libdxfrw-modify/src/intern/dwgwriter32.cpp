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
//!  dwgwriter32.cpp —— R2018 (AC1032) 的容器摆放**复用 dwgWriter18**。
//!
//!  依据：libredwg 的 decode.c 主分派为 `SINCE (R_2010b) → decode_R2004`，
//!  即 R2010 / R2013 / R2018 全部走 R2004 一族容器
//!  （0x80 加密文件头 + 页映射/段映射 + 分页）；R2007 才是另一套 decode_R2007。
//!  AC1032 与 AC1018 / AC1024 / AC1027 在容器层的差别仅版本标识与几个版本常量，
//!  故转交 dwgWriter18::writeAs 按版本取值。
//!
//!  对象层的 R2018 专有差异由 dwgWriter18 的 `ver >= AC1032` 分支处理
//!  （辅助头 +6 字节、MTEXT 的 is_not_annotative 等）。
//!
#include "dwgwriter32.h"
#include "dwgwriter18.h"

namespace dwgWriter32 {

bool supports(DRW::Version ver) noexcept
{
    return ver == DRW::AC1032;
}

bool write(dwgW& writer, DRW_Interface* iface)
{
    return dwgWriter18::writeAs(writer, iface, DRW::AC1032);
}

} // namespace dwgWriter32
