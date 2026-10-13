# LyCAD

基于 **C++20 + Qt6 + CMake** 的二维 CAD 绘图软件。功能定位对标 AutoCAD 的基础绘图与编辑能力，
支持 **DXF / DWG** 图形文件的导入导出，代码按「文件解析 / 几何模型 / 渲染显示 / 用户交互」等职责清晰分层。

---

**文档**（本 README 只保留简介、功能、构建说明与致谢，其余内容在 `docs/` 下）：

| 文档 | 内容 |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | 模块结构、依赖方向、各模块职责与关键设计 |
| [`docs/dwg.md`](docs/dwg.md) | DWG 支持：导入导出、文本编码、角度单位的实测定标、内容预检 |
| [`docs/verification.md`](docs/verification.md) | 验证：单元测试、ezdxf 交叉校验、界面链路、DWG 实样、复现验证、互操作性 |
| [`docs/packaging.md`](docs/packaging.md) | 打包与 CI：Debian 包、三条 GitHub 工作流、Windows(MinGW) 工具链 |
| [`docs/shortcuts.md`](docs/shortcuts.md) | 全部快捷键 |
| [`docs/limitations.md`](docs/limitations.md) | 已知限制：未实现的能力与刻意的取舍 |
| [`docs/libdxfrw-modify.md`](docs/libdxfrw-modify.md) | 随仓库分发的 libdxfrw 修改版：全部改动清单 |

---

## 功能

**绘图**：直线（连续绘制）、多段线（含圆弧段、**恒定/逐顶点宽度**——PCB 走线语义、闭合/放弃）、
圆（圆心半径/三点/两点/直径）、圆弧（三点/圆心起点终点）、**椭圆**（轴端点 / 中心点两种方式，
定轴方向时吸附到上下左右并给出追踪线与「水平 / 竖直」标记，定另一条半轴时标出该半轴及其端点）、
**构造线与射线**（定位点 + 通过点，构造线支持 H 水平 / V 垂直锁定方向，可连续绘制；
两图元共用一套工具状态机，命令 `XLINE / XL`、`RAY`）、
矩形、点、
**文字**（功能区「单行文字 / 多行文字」一格下拉，主体默认单行文字、箭头可切多行；
两种都是「**指定插入点 -> 弹出编辑框确认内容**」：单行文字用单行输入框，多行文字用可换行的
多行编辑框；单行支持字高/旋转/九种对齐组合，多行带附着点与参考栏宽）、
**填充**（ANSI31 图案/实体填充，边界取点闭合，支持凸度圆弧边界——「2 顶点 + 凸度 1」整圆焊盘）、
**尺寸标注**（对齐/线性/半径/直径/**坐标**/**弧长**，测量值实时推导，夹点拖拽重排几何）。
坐标标注按 AutoCAD 的习惯定度量轴：**引线偏竖直就量 X 坐标、偏水平就量 Y 坐标**，
移动鼠标时预览随之换轴，不必先选轴再点；弧长标注拾取一条圆弧后，尺寸弧的位置决定它的显示半径，
测量值始终是被拾取圆弧的弧长。
**表格**（`TABLE` / `TB`：先定插入点（左上角）再拖出另一角点定总尺寸，绘制前可输 `R`/`C`
改行数/列数，行高列宽按总尺寸均分；建成的是可直接编辑的原生表格，不是散落的线段）。

**编辑**：移动、复制（连续放置）、旋转、缩放、镜像、删除；夹点拖拽编辑；**双击文字编辑内容**（按文字类型走**不同的编辑框**——多行文字是多行编辑框：回车确认、**Alt+Enter 换行**；单行文字是单行输入框，不提供换行入口，避免把单行对象改成多行版面。编辑结果进撤销栈）；**特性面板改文字属性**（字高 / 旋转 / 水平对齐 / 垂直对齐，选中文字时才出现）；窗选（自左向右）与交叉选（自右向左）。

**选择与捕捉**：单击拾取、Shift 增减、Ctrl+A 全选、对象捕捉（9 类）、栅格吸附、正交、极轴追踪。

**图层**：新建/删除/重命名、颜色、线型、线宽、开关/冻结/锁定；当前图层；锁定图层不可选择。

**视图**：滚轮缩放（以光标为锚点）、中键平移、缩放至范围/选择集、网格与坐标轴。

**界面**：仿 AutoCAD 的功能区，按面板分组组织全部命令（文件 / 绘图 / 修改 / 注释 / 图层 / 特性 / 视图 / 帮助），
大按钮 + 小按钮两种密度，图层面板与特性面板内嵌真实下拉框；38 枚自绘 SVG 图标。
**深色主题**：菜单栏、功能区、停靠面板、表格、命令行、状态栏与画布整体切换
（「视图 → 深色主题」或功能区上的「深色主题」按钮，两处状态同步）。

**命令行**：命令补全、历史回溯、坐标与算式输入、`Esc` 取消、空格重复上一命令、`F1` 命令列表。

**文件**：DXF 导入导出（可写目标版本 R2000 ~ 2018 六档；**R12 只读不写**）、**DWG 导入导出**（内置 libdxfrw / dwgW 原生，导入 R14 ~ 2018、导出 R2004（默认）/R2000）、拖放打开。
**多标签页**：每张图占用一个标签页，可同时打开多份图纸独立编辑——「新建 / 打开 / 拖入文件」都会开进新标签页
（当前是没动过的空图时直接复用）；同一文件重复打开会自动切换到已开的标签页；标签可关闭、可拖动重排，
关闭或退出时逐个询问未保存的修改；`Ctrl+W` 关闭标签页，`Ctrl+Tab` / `Ctrl+Shift+Tab` 来回切换。
表格（ACAD_TABLE）在两种格式下都按**原生实体**读写：DXF 本体携带完整行列结构与单元格文字；
DWG 按 AcDbTable 类实体写出（CLASSES 段登记），行列数、行高列宽、插入点、旋转逐项保真，
几何与文字烘进 `*Tnn` 匿名几何块（AutoCAD 打开即为可编辑的表格对象，不会变成块参照）。
普通保存（Ctrl+S）用默认目标版本 **AC1032 (2018)**；**「另存为」的文件类型列表就是版本选择器**——
`AutoCAD 2000 DXF 文件` … `AutoCAD 2018 DXF 文件` 各一条（由旧到新排列，
默认选中 2018），末尾再跟一条 `AutoCAD 2000 DWG 文件`（DWG 写出版本固定 R2000，名字同样点明版本），
选哪条就等于选哪个目标格式与版本，不再需要单独的「DXF 目标版本」菜单项。
菜单里的**「导出」只导出 PDF**（一步到位；带纸张/比例/预览的 PDF 通路在「打印」对话框里）。

**打印**：打印（Ctrl+P）一屏完成——左侧设置（打印机或**导出为 PDF**、纸张 A0~A4/B 系列/
Letter 等、横向纵向、**出图比例**：适应图纸 / 1:1 / 自定义 1:n、份数），右侧实时打印预览；
出图范围铺满可打印区、白底、按毫米落线宽，纸面上只画图纸内容（无边框、无标题栏）；
固定比例下图纸超出一张纸时**自动分页**（先行后列，预览带页码栏可翻页）；
**「不打印」图层的图元不进纸面**；预览与打印共用同一份 `render/PlotRenderer`，
预览即所的是结构保证而不是巧合。
文字/填充/尺寸标注在导出时按规范写出（TEXT / MTEXT / HATCH / DIMENSION + 匿名几何块）。

---

## 构建说明

```bash
# 依赖：CMake ≥ 3.21、支持 C++20 的编译器、Qt 6.5+ (Core/Gui/Widgets/Svg/PrintSupport)
#       Debian/Ubuntu 上 Qt 开发包为 qt6-base-dev qt6-svg-dev
#       libdxfrw-modify（DWG/DXF 原生解析，libdxfrw 修改版）已随仓库内置，
#       不需要单独安装；它依赖 iconv，
#       glibc 自带、无需额外安装，macOS 等平台由 CMake 自动链接
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j              # 默认只编译主程序

./build/lycad                       # 启动
./build/lycad drawing.dxf           # 启动并打开文件

# 测试与开发工具按需构建（ctest 不会自己编译缺失的二进制）：
cmake --build build --target lycad_tests -j
ctest --test-dir build --output-on-failure   # 运行测试
```

### 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `LYCAD_BUILD_TESTS` | ON | 定义单元测试目标（不在默认构建里，用 `--target lycad_tests` 按需编译） |
| `LYCAD_BUILD_TOOLS` | ON | 定义开发工具目标（同上，用 `--target lycad_tools` 按需编译） |
| `LYCAD_ENABLE_ASAN` | OFF | 启用 AddressSanitizer / UBSan |
| `LYCAD_WARNINGS_AS_ERRORS` | OFF | 将编译告警视为错误 |

Windows 走 MSYS2 / MinGW（DWG 的文本编码转换依赖 iconv），三条 GitHub 工作流与出包流程见 [docs/packaging.md](docs/packaging.md)。

---

## 致谢

### 本项目

本项目代码为原创实现，采用 **Apache-2.0**（许可全文见仓库根目录的 `LICENSE`）。
DXF 格式依据 Autodesk 公开发布的 *DXF Reference* 实现。

### 第三方组件

| 组件 | 版本 | 许可 | 用途 |
|---|---|---|---|
| [libdxfrw](https://github.com/LibreCAD/libdxfrw) 0.6.3 的修改版（`third-party/libdxfrw-modify`） | 基于 0.6.3 | **GPL-2.0-or-later** | DWG / DXF 原生解析 |

`third-party/libdxfrw-modify` 是 libdxfrw 0.6.3 的**永久修改版**，不再跟随上游。
采用 GPL-2.0-or-later 且已随本项目一起编译链接，这意味着 `lycad` 二进制的分发
整体受 GPL 约束：分发二进制时需一并提供对应源码（本仓库即是）。许可全文见
`third-party/libdxfrw-modify/COPYING`。

本项目自身是 Apache-2.0，而 Apache-2.0 与 GPL-2.0 并不兼容；但 libdxfrw 是
**GPL-2.0-or-later**，因此含它的 `lycad` 二进制可以按 GPL-3.0 这一档分发。

> 早期版本的 README 曾把 libdxfrw 的许可写作 LGPL-2.1，那是**错误的**。
> 核对 `COPYING`（GPL 第 2 版全文）与其源码文件头的声明后已更正。

它不是「可选的运行时依赖」：源码在编译期编入，不存在「不装就没有」的情形。
若需以非 GPL 方式分发，必须移除 `third-party/libdxfrw-modify`、`io/LibDxfRwAdapter.*`
与根 `CMakeLists.txt` 里编 libdxfrw-modify 的那一段（`lycad_dxfrw` 目标）——届时 `io/DwgCodec.*` 也要一并移除，
因为 DWG 的读写全部由它承担，移除后不再有任何 DWG 支持（DXF 链路不受影响）。

对上游源码的全部修改（逐条原因与对应回归测试）见 [docs/libdxfrw-modify.md](docs/libdxfrw-modify.md)。
