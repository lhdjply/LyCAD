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
//!  dwgwriter18.h —— DWG **R2004 (AC1018)** 写出器入口。
//!
//!  与读取端 intern/dwgreader18.h 对称：数字 18 指 DWG 的**内部版本号**
//!  （= AC1018 = AutoCAD 2004），不是年份。
//!
//!  分工（与 dwgwriter15.h / dwgreader 三层一致）
//!    · dwgW（声明与实现均在 ../libdwgrw.h / ../libdwgrw.cpp）—— 对外接口，
//!      与 dxfRW 写出侧同构；版本分派在 libdwgrw.cpp 的 dwgW::write() 中完成。
//!    · 本模块（dwgwriter18.cpp）—— AC1018 的**分页文件格式**：
//!        0x100 文件头（含 0x6C 内层头，CRC-32 + 幻数异或）
//!        → 各节切页、逐页 LZ77 压缩、32 字节页头（XOR 掩码保护）
//!        → 段映射页（0x4163003b）与页映射页（0x41630e3b）
//!      节的**内容**（header 变量、classes、对象流、句柄流）与 AC15 一致，
//!      由 dwgW::buildSections() 统一构建后复用。
//!    · ../intern/dwgwriter.h —— 与版本无关的位级原语（dwgBitWriter / CRC16 /
//!      哨兵），两种容器共用。
//!
//!  格式差异（相对 R2000 线性容器）
//!    · 文件改为「分页」：段被切成 ≤0x7400 的页，页经 LZ77 压缩存放。
//!    · 文件头 0x80 处多出 0x6C 字节内层头（标识 AcFssFcAJMB），
//!      CRC-32 后整块与幻数序列异或；文件尾 0xEC 处为 20 字节幻数。
//!    · header / classes 节的载荷形态相对 AC15 有两处版本差异
//!      （见 libdwgrw.cpp::writeHeaderVars 与 drw_header.cpp::parseDwg）。
//!
//!  参考实现：opencadcodec 的 file_header_ac18.rs / compression.rs /
//!  checksum.rs（仅作算法参考，本文件为独立实现，不引用其代码）。
//!
//!  验证状态：容器与节载荷均已用工程自带的 dwgR（dwgreader18）回读闭环——
//!    文件头/内层头/CRC-32/幻数、页映射、段映射、LZ77、全部表记录与实体
//!    （LINE/CIRCLE/TEXT、含中文名称与样式的文字样式）逐字节往返一致。
//!    R2004 相对 R2000 的对象层差异已全部在 dwgwriter15.cpp 按 dwgVer 镜像：
//!      · 头部：4 处变量/句柄增删 + 颜色改 CMC + 节尾哨兵
//!      · 类节：类记录尾部 5 个 BitLong
//!      · 对象：numReactors 后的 xDictFlag 位、nolinks 位条件化、
//!              块记录的实体数与实体句柄表（取代 prev/next 链）
//!    另修复读取端 dwgReader18::readMetaData 的代码页映射缺失（R2004 中文）。
//!    容器装配与真实 AutoCAD R2004 文件逐结构对齐：规范的 13 条段记录
//!    （固定段号 + 固定顺序，空节登记为 0 页记录）、全部页 0x20 对齐、
//!    页映射页登记自身、文件尾 0x6C 字节第二头部、AcDb:Template /
//!    ObjFreeSpace / AuxHeader 元数据段（结构与真实文件逐字节核对）。
//!
#ifndef DWGWRITER18_H
#define DWGWRITER18_H

#include "../drw_base.h"            // DRW::Version

class dwgW;
class DRW_Interface;

namespace dwgWriter18 {

/// 本实现是否支持目标版本。当前仅 AC1018（AutoCAD 2004 / R2004）。
bool supports(DRW::Version ver) noexcept;

/// 写出整个文件（AC1018 分页容器）。
/// 前置条件：writer 已构造完毕；iface 非空；ver 已由 supports() 校验通过。
/// 返回 false 表示写出失败（文件不可写、内部状态不一致等）。
bool write(dwgW& writer, DRW_Interface* iface);

} // namespace dwgWriter18

#endif // DWGWRITER18_H
