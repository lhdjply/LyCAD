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
//!  dwgwriter24.cpp —— R2010 (AC1024) 的容器摆放**复用 dwgWriter18**。
//!
//!  依据：libredwg 的 decode.c 把 R2010 交给 decode_R2004
//!  （`SINCE (R_2010b) → decode_R2004`，该函数的注释也写着 "for 2004 and 2010+"），
//!  而 R2007 走的是另一套 decode_R2007。也就是说 AC1024 与 AC1018 同属
//!  「0x80 加密文件头 + 页映射/段映射 + 分页」这一族容器，AC1021 的文件头页
//!  容器**不适用**于 AC1024。
//!  两者在容器层的差别只有版本标识与几个版本常量，故全部复用 dwgWriter18，
//!  由 writeAs 按版本取值。
//!
#include "dwgwriter24.h"
#include "dwgwriter18.h"

namespace dwgWriter24 {

bool supports(DRW::Version ver) noexcept
{
    return ver == DRW::AC1024;
}

bool write(dwgW& writer, DRW_Interface* iface)
{
    return dwgWriter18::writeAs(writer, iface, DRW::AC1024);
}

} // namespace dwgWriter24
