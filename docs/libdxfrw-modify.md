# 对上游源码的修改（libdxfrw-modify）

> 本文件是 LyCAD 文档的一部分，入口见仓库根目录的 [README](../README.md)。

全部集中在下表这些位置，均带有说明注释，并有回归测试：

| 位置 | 修改 | 原因 |
|---|---|---|
| `intern/drw_textcodec.cpp` | 解码器只看当前文件的版本 | 上游用函数内 `static` 记录「进程见过的最低版本」来选解码器：先读老版本文件，之后 2007+ 文件的文本全被按老代码页解码。实测 173 个 2007+ DWG 中 59 个中招，汉字总数 156 → 20 |
| `drw_entities.cpp` | DXF 的 60 组码按规范取反；DWG 的 invisibleFlag 赋给 visible | 上游 DXF 路径把 60=1（不可见）当 visible 用、DWG 路径读出标志后丢弃，隐藏图元一律被读成可见 |
| `intern/dxfreader.cpp` | 二进制 DXF 组码按规范 1 字节读取（255 为转义符） | 上游按 2 字节读组码，任何合法二进制 DXF 都解析失败（最小样例读出 0 个图元） |
| `intern/drw_textcodec.cpp` | 双字节代码页（936/949/950/932）改为直接在码表上二分查找，不再按引导字节表划区间 | 引导字节表（`DRW_LeadTable*`）是手工维护的前缀和，与码表对不上：实测 936 有 3 处、932 有 29 处、949/950 各有几处错位。整段码位因此被划到邻居名下：落在缝里的字查不到，退化成 `?`（实测某张中文图纸里有 20 个字中招，如「个」「更」「各」；解码按完整码位比对，因此只会变 `?`、不会误命中别的字）。四张码表本身按码位严格升序且无重复，二分查找既准确又不依赖那份表 |
| `drw_interface.h` / `libdxfrw.cpp` | 新增 `addUnknownEntity` 回调 | 上游对不认识的实体类型（MLINE、MESH 等）静默跳过，应用层无从计数上报 |
| `libdxfrw.cpp` | 写出侧补齐：隐藏图元的 60 组码、填充的多段线路径边界、R12 的 DIMENSION 实体（本项目的 R12 写出已移除，这条分支不再被走到，保留是为了不偏离上游过远） | 上游写出路径分别是空实现/缺失，导致隐藏图元重现、多段线边界填充丢失、R12 标注被静默丢弃 |
| `drw_entities.h` / `libdxfrw.cpp` | DIMENSION 支持 42 组码（实测值） | 上游不写该组码，不具备标注重绘能力的旧软件无法显示测量值 |
| `drw_entities.h` / `drw_entities.cpp` / `libdxfrw.cpp` | 新增 `DRW_DimArc`：DXF 扩展实体 `ARC_DIMENSION`（子类 AcDbArcDimension）的读写，含 DWG 侧类对象解析 | 上游没有弧长标注（DIMARC）这个类型，读入时整条消失 |
| `drw_entities.cpp` | `DRW_DimArc::parseCode` 按 `100 AcDbArcDimension` 子类标记切换 70/71 的含义 | 弧长标注里有两组同号组码：AcDbDimension 段的 70 是标注类型、AcDbArcDimension 段的 70 是子类标志位。照抄 `DRW_Dimension::parseCode` 会把标志位当类型 |
| `intern/dwgreader.cpp` / `drw_classes.cpp` | 类对象 `ARC_DIMENSION` 接入分派（类名 → 内部类型号 200） | 类对象在 DWG 里只以 CLASSES 段的类号出现，不映射就落进「待处理对象」表被静默丢弃 |
| `intern/dwgbuffer.cpp` | `getBitLongLong` 改为**小端**读取 | DWG 的多字节整数一律小端，按大端拼接会把 2 字节长度读反：实测某类对象的预览数据长度 1412 被读成 33797，其后字段整体错位、对象解析失败 |
| `drw_entities.cpp` | 公共实体数据里预览数据的长度：2010+ 用 BLL，R13–2007 才是 RL | 上游不分版本一律按 RL 读。带预览/代理图形的对象（类对象几乎都带）在 2010+ 文件里会整体解析失败 |
| `drw_entities.cpp` | `DRW_Dimension::parseDwg` 把 42 组码（实测值）存进 `measurement` | 上游只读进局部变量就丢掉，上层拿不到图里写的测量值——坐标标注要靠它判断 X 型/Y 型 |
| `libdxfrw.cpp` | `writeDimension` 补 `DRW::DIMORDINATE` 分支（上游已有该分支，两侧齐了才写得出去）与新增 `writeArcDimension`；`ARC_DIMENSION` 需要 CLASSES 段声明 | 上游写出侧没有弧长标注，坐标为 6 的写法也只走默认分支（只写类型位、不写定义点） |
| `intern/dwgwriter15.cpp` | 新增 `dwgW::writeArcDimension`（类对象，对象头写类号 504）、`writeDimension` 补坐标标注（类型号 0x14） | 上游 DWG 写出侧没有这两类标注，保存为 DWG 后它们会整条消失 |
| `intern/dwgwriter15.cpp` | LTYPE 的句柄流里**每段线型各补一个** 340 句柄占位 |LTYPE 规范在非 DXF 分支里逐段无条件读一个「复杂线型形状样式」句柄，少了就读到对象末尾之外——实测 DASHED/CENTER 这类 2~4 段的线型在我们写出的 DWG 里都会让它报 `bit_read_RC buffer overflow`（文件仍能读出，但已越界） |
| `drw_header.cpp` | `$HANDSEED` 放宽到 0xFFFFFF | 规范要求句柄种子大于文件中全部句柄；上游写死 0x20000，余量太小 |
| `libdxfrw.h` / `libdxfrw.cpp` | 实体写出处补 `330` 组码（属主 BLOCK_RECORD 软指针） | 上游不写该组码，实体成"孤儿"——实测 AutoCAD 打开后整个图形空白；模型空间实体指向 0x1F，块内实体指向各自的块记录 |
| `drw_entities.*` / `drw_interface.h` / `libdxfrw.cpp` | 新增 `DRW_Attdef` / `DRW_Attrib` 与 `addAttdef` / `addAttrib` 回调，DXF 侧补齐 `ATTDEF`/`ATTRIB` 的解析与写出（`writeAttdef` / `writeAttrib` / `writeSeqEnd`，`INSERT` 的 66 组码） | 上游把这两种实体整块注释掉了：属性块读进来只剩几何，属性文字全丢；写出去也没有 `ATTRIB`，别的 CAD 打开看不到属性值 |
| `intern/dwgreader.cpp` | DWG 对象类型 2/3（ATTRIB/ATTDEF）接入派发；两者解析失败**不**把整张图判为损坏 | 上游把它们丢进"待处理对象"表后不再处理；而读取端只要有一条实体返回 false 就会让整张图打不开，拿属性换整张图不划算 |
| `intern/dwgwriter15.cpp` / `libdwgrw.h` | 新增 `dwgW::writeInsert`（对象类型 7/8）：插入点、比例、旋转、挤出、has_attribs 位、BLOCK RECORD 硬指针，以及属性存在时的 first_attrib / last_attrib / seqend 三个句柄 | 上游 DWG 写出器没有 INSERT，块参照保存为 DWG 后会整块消失；三个属性句柄不写，属性就成了没人认领的游离实体 |
| `intern/dwgwriter15.cpp` / `libdwgrw.h` | 新增 `dwgW::writeAttrib` / `writeAttdef` / `writeSeqEnd`（对象类型 2/3/6），与 TEXT 共用 `writeTextBody` | 上游没有属性对象的编码器，属性块保存为 DWG 后属性文字会散成模型空间里的独立 TEXT——块参照与它的属性就此脱钩 |
| `intern/dwgwriter15.cpp` | 属性对象的属主写成**块参照自己**（不是块记录），且不挂进块的 prev/next 实体链 | 实测 AutoCAD 2000 的图纸正是如此；照块内实体写会让别的实现把属性当成游离实体 |
| `intern/dwgwriter15.cpp` | `writeText` 的挤出/厚度顺序改为**挤出在前** | 读取端是 `getExtrusion()` 再 `getThickness()`，上游写出侧写反了。两者都以「1 位 = 取默认值」编码，默认时看不出来（所以一直没暴露），厚度或挤出一旦非默认就会张冠李戴 |
| `libdxfrw.cpp` | 写出文字样式时补写表记录的扩展数据（`writeExtData`） | 上游只给图元写扩展数据，表记录一律不写；而 AutoCAD 把中文图纸常用的 TrueType 字体族名**只**记在样式的 ACAD 扩展数据里（此时 3 组码为空），往返后字体丢失，汉字退化为 "?" |
| `intern/dwgreader21.cpp` | 2007+ 区段页的 RS 块数改用 `duint32`（上游写成 `duint8`） | 一页最多 257 块（65535 字节 / 255），写在 `duint8` 里 257 会截断成 1：整页只剩第一块被纠错，其余仍是 RS 编码，解压出来是乱码，表段解析失败 —— 整个文件被判「已损坏」而打不开。实测一个 2007 版 DWG（对象区 47MB）正是如此 |
| `libdxfrw.cpp`（`writeHatch`） | 写出填充的图案定义线（53/43/44/45/46/79/49），并把它们收进 `DRW_Hatch::deflist`（读取侧也照此保留） | 上游只写 78 组码的**条数**、内容一个字不写（那段代码是注释掉的），文件里的非实体填充于是成了「有图案名、无定义」：实测 AutoCAD 会把它画成一大片乱纹盖住整张图，点一下重生成才恢复。另外偏移必须换算到图案线坐标系、基点归零，直接照抄源数据同样出问题 |
| `libdxfrw.cpp`（`writeHatch`） | 填充边界带凸度时，**每个**顶点都写 42 组码（无凸度写 0） | 上游只给非零凸度的顶点写 42 的「稀疏」写法会让 AutoCAD 的填充边界解析错位：实测同一份文件只把零凸度的 42 组码删掉，打开就是一大片乱纹（属性面板显示面积为 0）。无凸度时（72=0）不写 42，与 AutoCAD/ezdxf 一致 |
| `drw_objects.cpp` | 文字样式的尾部句柄（父控制符、外部参照）仅在其后确实还有数据时才读 | 这些句柄在 2007+ 是可选的；上游无条件读，越界后流状态变坏，最后的 `isGood()` 为假 —— **一条样式读不出来就让整张图打不开**（实测同一个 DWG 里有 5 条样式如此） |
| `drw_entities.cpp`（`DRW_MText::parseDwg`） | 读入 11 组码后置位 `haveXAxis` | DWG 里 11 组码一定是「X 轴方向向量」，而 MTEXT 的旋转只存在这一个字段里（MTEXT 没有 50 组码）。不置位时 `updateAngle()` 是空操作、`hasXAxisDirection()` 恒为 false，调用方拿不到任何旋转依据：实测一张 2018 版 DWG（AC1032）的 272 条文字里，202 条旋转 90° 的 MTEXT 被画成水平，同一张图的 DXF 全部正确。DXF 路径早已在 `parseCode(11)` 里置位，DWG 路径漏了 |
| `drw_entities.cpp`（`DRW_MText::parseDwg`） | 把 72（书写方向）、73（行距样式）、44（行距系数）读进 `alignH` / `alignV` / `interlin` | 上游把这三个字段读出来就丢（`DRW_UNUSED`），多行文字的竖排方向与「固定行距」在往返中会被悄悄改成默认值（从左到右 / 至少 / 1.0）。DXF 路径本来就存进这三个字段，DWG 路径补齐后两条路径同义 |
| `intern/dwgwriter15.cpp`（`dwgW::writeMText`） | 绘制方向与行距样式按字段写出（原来是硬编码 1） | 同上的另一半：读取端填好了字段值，写出端还写死 1 就等于没读。缺省值仍是 1，与既有文件兼容 |
| `libdxfrw.cpp`（`dxfRW::writeMText`）+ `drw_entities.h` | 有 X 轴方向向量时写 11/21/31，否则才退回 50；为 `DRW_MText` 增加 `setXAxisDirection()` | MTEXT 的旋转本来就写在 11 组码里（50 只是 DXF 的输入便利写法，两者同时出现时后者胜出）。上游只写 50 且把 11 那行留着「RLZ … 11, 21, 31 needed?」的注释，旋转因此在 DXF 往返里走的是一条与 DWG 不同的路 |
| `drw_entities.*` / `drw_interface.h` / `libdxfrw.cpp` | 新增 `DRW_Table`（ACAD_TABLE）与 `addTable` 回调。DXF 解析在 `processTable` 里按 `AcDbBlockReference`/`AcDbTable` 两段子类标记推进（块名与表样式共用 2 组码、50 组码的旋转只在块参照段取、单元格按行优先逐格展开、合并值 173 低 16 位跨列/高 16 位跨行）；写出侧补齐本体（91/92 行列、141/142 行高列宽、单元格 171~1 序列、合并 173，缺省格也占位防错配） | 上游把 ACAD_TABLE 整块注释掉：表格读进来消失，写出去也消失 |
| `drw_entities.cpp`（`DRW_Table::parseDwg`） | DWG 侧按**实体公共头 + 双段位流**解析：数据流（插入点→比例/旋转→has_attribs→行高列宽）→ 按 objSize 重定位跳过解不动的 cells 数组 → 句柄流（块头 H2、表样式 H342）。|
| `intern/dwgwriter15.cpp` / `libdwgrw.h` | 新增 `dwgW::writeTable`（类号 505，CLASSES 段登记 `AcDbTable`）：AcDbBlockReference + AcDbTable 两段按实测位布局写出，extraHandles 追加块头/表样式句柄；cells 暂不写出（写 0 计数），几何与文字由 `*Tnn` 匿名几何块承载 | 上游 DWG 写出器没有表格，保存为 DWG 后表格会整条消失（或被降级成块参照） |
| `intern/dwgwriter18.*` / `dwgwriter15.*` / `libdwgrw.*` | 新增 **AC1018（R2004）原生写出**：分页容器（0x100 文件头 + 0x6C 内层头 CRC-32/幻数异或 + LZ77 分页 + 段/页映射 + 文件尾第二头部），节载荷与 AC15 共用（`buildSections`），R2004 对象层差异按版本镜像（颜色 CMC、xDictFlag 位、块记录实体表、INSERT/TABLE 的属主计数等） | 上游只有 R2000 写出；第三实现以「能被自家 dwgreader18 完整回读 + 与真实 R2004 文件逐结构比对」双标准验收 |
| `intern/dwgwriter15.cpp`（`buildSections`） | 节载荷构建抽成与容器无关的入口，头部/类节/对象层按 `dwgVer` 输出版本形态 | AC15/AC18 两种容器共用同一份内容，避免 3000 行复制 |
| `intern/dwgreader18.cpp`（`readMetaData`） | 代码页映射补齐（29/30=1252、38=932、39=936、40=949、41=950） | 上游只认 30：R2004 中文图纸（id=39/GBK）本来就会被按 1252 解码，中文全乱码 |
| `intern/dwgwriter18.cpp`（`writeSystemPage`） | 系统页头的压缩类型字段按 u32 写 2（此前 4 个字节各填 2，u32 = 0x02020202） | 本工程读取端忽略该字段所以往返正常；严格校验的第三方（OpenCADStudio / 浩辰）不等于 2 就把页数据当未压缩内容解析——页映射解不出，整文件判「损坏」 |
| `intern/dwgwriter15.cpp`（`writeHeaderVars`） | 2004+ 头部补齐 5 个缺失变量：OBSCUREDCOLOR / INTERSECTIONCOLOR（BS）、OBSCUREDLTYPE / INTERSECTIONDISPLAY（RC）、PROJECTNAME（T） | 此前整块漏写：读取端把这 5 个变量的位置当成其后的空间块/线型句柄来读，句柄流从 BLOCK PAPER_SPACE 起全部错位——本工程读取端宽容地"读出垃圾"继续跑（往返测试发现不了），浩辰则直接判文件损坏。用工程读取端的调试输出对同一张图逐变量比对定位 |
| `intern/dwgwriter15.cpp`（`writeTableCommon` / `writeEntityCommon` / `writeNamedHeadStart` + 各对象句柄流） | **R2004+** 的 `xDictFlag` 写 1（无 XDicObj），并省略 XDicObj 空句柄（实体/表记录/控制对象/字典/命名对象/布局，统一走 `Private::writeXdicObj`）。**R2000 及以下不受影响**：那些版本不写 `xDictFlag` 位，读取端按默认值 0 判定 XDicObj 存在而必然读取，故仍照写该句柄 | 我们的每个 R2004 对象句柄流都比真实 R2004 文件多一个空 XDicObj 句柄（46 个对象里 37 个）。修复后 CIRCLE 等实体与浩辰原版、OpenCADStudio 版三方逐字节一致，对象体尺寸精确对齐（CIRCLE 39→38、LAYER_CONTROL 14→13、LTYPE 291→290）。注意此处必须按版本区分——一刀切省略会让 R2000 输出少一个句柄而整体错位 |
| `intern/dwgwriter15.cpp`（`encodeBlockRecord`） | R2004+ 块记录的块内实体句柄码由 4（软指针）改为 3（硬属主）；R2000 分支不变 | 实测浩辰原版与 OpenCADStudio 版在此处均用码 3；用码 4 会被严格读取器判为引用类型不符 |
| `intern/dwgwriter15.cpp`（`writeHatch`） | R2004+ 补写 HATCH 数据段开头的**渐变填充字段块**（is_gradient / reserved / 角度 / 偏移 / 单色 / 色调 / 颜色数 / 名称，普通填充全 0 / 空串），写在 `basePoint.z` 之前 | 读取端 `DRW_Hatch::parseDwg` 的 `if (version > DRW::AC1015)` 块：漏写会把高程当成 is_gradient，其后 num_colors 与种子点数解析成天文数字（自家读取端直接死循环）。**这是既存缺陷**：含填充的图纸写出的 R2004 文件一直读不回，R2000 则正常 |
| `intern/dwgwriter15.cpp`（`layerHandle` / `ltypeHandle` / `styleHandle` / `dimstyleHandle`） | 名称查表改为**忽略大小写**（`Private::findName`：先精确查、再逐项忽略大小写比对） | 内建项登记为混合大小写（`Continuous`），而文档侧存的是大写规范名（`CONTINUOUS`），精确匹配落空后静默回退到默认值——实测图层线型 CONTINUOUS 被写成 ByLayer（图层线型引用错误） |
| `intern/dwgwriter18.cpp` / `dwgwriter21.cpp`（AppInfo 段） | 产品信息里的版本号不再写字面量，改由构建系统注入的 `LYCAD_VERSION` 宏提供（单独编译 libdxfrw 时退回 `"0.0.0"`） | 该段记录的是「谁写的、什么版本」，写死会在每次发版后与实际版本不符；实测把 VERSION 改成 9.9.9 后重建，两种写出器产出的 DWG 里 `build_version` / `registry_version` 同步变成 9.9.9 |

回归测试是 `tests/test_libdxfrw_codec.cpp`（解码器不变量）、`test_dxf_roundtrip`
的隐藏图元往返用例、`test_table`（表格 DXF/DWG 双链路逐字段往返），以及
`test_libdxfrw` 里由 DWG 实样驱动的
`dwgAndDxfAgree`（同一张图纸的两种格式必须读出同一份文字几何）——这些行为一旦
回退，测试会失败。
