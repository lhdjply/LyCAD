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
//!  dwgwriter21.h —— DWG **R2007 (AC1021)** 文件容器入口。
//!
//!  与读取端 intern/dwgreader21.h 对称：数字 21 指 DWG 的**内部版本号**
//!  （= AC1021 = AutoCAD 2007），不是年份。
//!
//!  分工（与 dwgwriter15.h / dwgwriter18.h 一致）
//!    · dwgW（声明与实现均在 ../libdwgrw.h / ../libdwgrw.cpp）—— 对外接口；
//!      与版本无关的写出逻辑、节载荷构建（buildSections）都在那一处。
//!    · 本模块（dwgwriter21.cpp）—— AC1021 的**RS + 分页文件格式**：
//!        0x0000  0x80 字节元数据块（版本串、各节定位地址…）
//!        0x0080  0x400 字节文件头页（RS(255,239)×3 + 0x110 元数据 + 0x28 校验数据）
//!        0x0480  数据页（逐页 LZ77 + RS(255,251) 交织编码，0x20 对齐）
//!        …       段映射页、段映射副本、页映射页、页映射副本
//!        末尾    文件头页副本（0x400）
//!      节的**内容**（header 变量、classes、对象流、句柄流）由
//!      dwgW::buildSections() 统一构建后复用。
//!    · ../intern/dwgwriter.h —— 与版本无关的位级原语（dwgBitWriter / CRC /
//!      哨兵），各版本容器共用。
//!
//!  格式差异（相对 R2004 分页容器）
//!    · **RS 纠错**：所有页都做 Reed-Solomon 编码。系统页（页映射/段映射/文件头）
//!      用 RS(255,239)（pp=0x96, tt=8），数据页用 RS(255,251)（pp=0xB8, tt=2），
//!      码字按 factor 交织存放。实测真实 R2007 文件的校验字节与本工程读取端
//!      使用的 RScodec 完全一致（见 tests 的 RS 约定验证）。
//!    · **LZ77 变体**：AC1021 的压缩流语法与 R2004 不同——操作码高 4 位定指令
//!      类型（0x0_/0x1_ 短中匹配、0x2_ 16 位偏移、0x3_~0xE_ 紧凑匹配、0x11 结束），
//!      字面量按长度做**字节重排**（长度 1~31 各一套置换、≥32 每 32 字节组倒序）。
//!    · **CRC-64**：文件头元数据 CRC、页 CRC、页校验和全部改用 64 位；
//!      元数据里还有 CRC 随机编码器（LCG + MT 混合）产生的随机种子字段。
//!    · **段名 UTF-16LE**：段映射里的段名按 UTF-16LE + NUL 存放，长度字段是字节数。
//!
//!  参考实现：opencadcodec 的 file_headers/file_header_ac21.rs、dwg21_metadata.rs、
//!  crc.rs、reed_solomon.rs（仅作格式参考，本文件为独立实现，不引用其代码）。
//!
//!  写入策略（与参考实现的差异，均为「可被读取端与第三方按规范读回」的前提）：
//!    · 段按规范给的 encoding 字段写出：读取端会解析的段（Header / Classes /
//!      Handles / AcDbObjects / ObjFreeSpace / Template / AuxHeader / RevHistory）
//!      用 encoding=4（LZ77 + RS(255,251) 交织）；元数据段（SummaryInfo / Preview /
//!      AppInfo / FileDepList）用 encoding=1（原文 + RS 校验，数据在文件里连续，
//!      预览图的绝对偏移因此有效）——与真实 R2007 文件的编码取值一致。
//!    · LZ77 采用「只发字面量」的合法流：每段 ≤22 字节的字面量 run，run 之间用
//!      长度为 0 的 0x2_ 匹配操作码隔开（读取端的指令语法允许）。压缩率为 1 附近，
//!      但流本身完全合法，任何按规范实现的解码器都能还原原文。真正的匹配搜索
//!      压缩尚未实现（不影响可读性，只影响体积）。
//!
#ifndef DWGWRITER21_H
#define DWGWRITER21_H

#include "../drw_base.h"            // DRW::Version

class dwgW;
class DRW_Interface;

namespace dwgWriter21 {

/// 本实现是否支持目标版本。当前仅 AC1021（AutoCAD 2007 / R2007）。
bool supports(DRW::Version ver) noexcept;

/// 写出整个文件（AC1021 RS + 分页容器）。
/// 前置条件：writer 已构造完毕；iface 非空；ver 已由 supports() 校验通过。
/// 返回 false 表示写出失败（文件不可写、内部状态不一致等）。
bool write(dwgW& writer, DRW_Interface* iface);

} // namespace dwgWriter21

#endif // DWGWRITER21_H
