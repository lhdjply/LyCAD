# LyCAD

基于 **C++20 + Qt6 + CMake** 的二维 CAD 绘图软件。功能定位对标 AutoCAD 的基础绘图与编辑能力，
支持 **DXF / DWG** 图形文件的导入导出，代码按「文件解析 / 几何模型 / 渲染显示 / 用户交互」等职责清晰分层。

---

## 1. 快速开始

```bash
# 依赖：CMake ≥ 3.21、支持 C++20 的编译器、Qt 6.5+ (Core/Gui/Widgets)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/bin/lycad                       # 启动
./build/bin/lycad drawing.dxf           # 启动并打开文件
ctest --test-dir build --output-on-failure   # 运行测试
```

### 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `LYCAD_BUILD_TESTS` | ON | 构建单元测试 |
| `LYCAD_BUILD_TOOLS` | ON | 构建开发期辅助工具（样例生成器、界面快照） |
| `LYCAD_WITH_LIBDXFRW` | OFF | 强制要求系统提供 libdxfrw 后端 |
| `LYCAD_ENABLE_ASAN` | OFF | 启用 AddressSanitizer / UBSan |
| `LYCAD_WARNINGS_AS_ERRORS` | OFF | 将编译告警视为错误 |

---

## 2. 模块结构

```
src/
├── core/       几何模型    4,935 行  —— 基础数学、图元、图层、文档、撤销栈
├── io/         文件解析    3,743 行  —— DXF 编解码器、DWG 后端适配、编解码器注册表
├── render/     渲染显示    1,812 行  —— 视图变换、图元绘制、画布控件
├── interact/   用户交互    4,250 行  —— 对象捕捉、工具状态机、命令注册表、坐标解析
└── ui/         界面装配    2,179 行  —— 主窗口、图层面板、属性面板、命令行
tests/          1,416 行
tools/            424 行
```

**依赖方向严格单向，无循环：**

```
ui ──> interact ──> render ──> core
          │            │         ^
          └──> io ─────┴─────────┘
```

每个模块编译为独立静态库（`LyCAD::Core` / `Io` / `Render` / `Interact` / `Ui`）。
`tools/lycad_sample` 只链接 `core` 与 `io`，因此在每次构建时充当
「下层模块不得反向依赖界面层」的编译期契约检查。

### 各模块职责与关键设计

**core —— 几何模型**

| 文件 | 职责 |
|---|---|
| `Types` | `Point2D` / `Vector2D` / `Box2D` / `Transform2D`（2×3 仿射变换）与线段求交等几何算法 |
| `Entity` | 图元抽象基类：标识、图层归属、颜色/线宽的随层解析、包围盒、拾取距离、夹点 |
| `PointEntity` `LineEntity` `CircleEntity` `ArcEntity` `PolylineEntity` | 五类图元 |
| `Layer` / `Linetype` / `AciPalette` | 图层表、标准线型、AutoCAD 颜色索引 |
| `Selection` | 选择集 |
| `Document` | 图元容器 + 图层表 + 选择集 + 撤销栈 |
| `Commands` | `QUndoCommand` 实现 |

设计要点：

- **`Document` 刻意不依赖 `io`**，因此不提供 `load/save`。文件读写由 `io::CadFile` 门面完成，
  它通过 `beginBatchLoad()` / `addEntity()` 装配文档。这让 core 处于依赖链最底层，可脱离文件格式独立测试。
- **撤销采用「前置快照」而非「施加逆变换」**。逆变换在非等比缩放下不可逆（圆会被近似为圆，无法还原原半径），
  而快照对任何变换都精确可逆。代价是一次深拷贝，换取的是撤销的绝对正确。
- **圆弧以「逆时针参数化」统一表达**（起始角 → 终止角，跨度 ∈ (0, 2π]），与 DXF 一致。
  镜像是唯一会反转绕行方向的变换，`ArcEntity::transform()` 与 `PolylineEntity::transform()` 对此有专门处理。
- **多段线支持凸度（bulge）**，即含圆弧段的多段线。凸度的符号决定绕行方向，
  因此包围盒、最近点、离散化都必须按符号分支——这是一个容易出错的点，也是测试重点。

**io —— 文件解析**

- `CadCodec`：`ICadCodec` 接口与 `CodecRegistry`。格式以插件形式注册，
  核心与界面只面向接口编程。注册表在按扩展名匹配时**分两轮**：先找可用的编解码器，
  若都不可用则返回「格式已知但能力不足」的那个，以便向用户说明具体原因
  （例如「DWG 需要安装 LibreDWG 或 ODA File Converter」），而不是一句无信息量的
  「不支持的文件类型 .dwg」。
- `DxfGroup`：组码层。支持 ASCII 与**二进制 DXF** 两种物理形态，并处理 `\U+XXXX` 转义，
  使中文图层名/文字在任意代码页下都能正确往返。
- `DxfReader`：R12 ~ 2018 全版本读取；支持 `POINT/LINE/CIRCLE/ARC/LWPOLYLINE/POLYLINE/SOLID/TRACE`；
  解析 `BLOCKS` 区并在 `INSERT` 处**按插入点/比例/旋转角展开为独立图元**，
  遵循「块内 0 层图元继承 INSERT 图层」的标准语义，支持嵌套块（含环引用保护）。
  未建模的实体（TEXT/HATCH/DIMENSION/SPLINE…）按类型汇总计数后跳过，**不静默丢弃**。
- `DxfWriter`：支持 `AC1009 (R12)` 与 `AC1015 (R2000)` 两个目标版本。
- `DwgCodec`：见第 5 节。

**render —— 渲染显示**

- `ViewTransform`：世界坐标（右手系，Y 轴向上）↔ 设备坐标（Y 轴向下）的换算。
  换算形式刻意只保留两个参数（原点的世界坐标、像素/单位），使所有视图操作都有闭式解，无需求逆矩阵。
- `EntityRenderer`：直线/圆/圆弧用**解析图元**直接绘制，不做折线近似，任意缩放下都精确；
  多段线的圆弧段按「像素矢高」自适应离散；屏幕半径超过 10⁵ 像素时曲率已低于一个像素，
  改用切线近似，既避免 Qt 光栅化定点坐标溢出，也省去无意义的海量分段。
- `CadCanvas`：网格、坐标轴、图元、选中高亮、夹点、捕捉标记、十字光标。
  **它不理解任何命令语义**——所有语义由 `CanvasInputHandler` 的实现（`interact::ToolManager`）承担。
  这样换一套交互方案（脚本驱动、自动化测试、触屏手势）只需替换处理器。

采用 QPainter 光栅后端而非 OpenGL：二维 CAD 的瓶颈在几何离散精度与图元数量，
而不在 GPU 填充率；光栅后端在远程桌面/虚拟机/无 GPU 环境下行为一致，对工程软件的可部署性更重要。
若需接入 GPU，只需替换 `EntityRenderer` 与画布的绘制循环。

**interact —— 用户交互**

- `SnapEngine`：对象捕捉（端点/交点/圆心/中点/象限点/垂足/切点/最近点/节点）+
  栅格吸附 + 正交 + 极轴追踪。捕捉带**优先级**：只要高优先级候选落在光圈内，
  更接近光标的低优先级候选也不会胜出——「端点」的工程意义远大于「最近点」。
- `ToolContext`：工具与文档/画布/捕捉/正交之间的唯一连接点，使工具构造函数保持单参数，
  新增交互选项无需改动任何工具。
- `Tool` / `ToolManager`：工具被建模为「由事件驱动的小型状态机」，命令行的提示文字就是当前状态的投影。
  **选择不是特例分支，而只是恰好作为兜底的那个工具**，消除了大量散布各处的 `if (无命令)` 判断。
- `CoordinateInput`：支持 `100,50` / `@50,0` / `50<45` 三种坐标形式，以及带括号的四则运算。
- 已实现命令：`LINE` `PLINE` `CIRCLE` `ARC` `RECTANGLE` `POINT` `MOVE` `COPY` `ROTATE` `SCALE` `MIRROR` `ERASE` `SELECT`。

**ui —— 界面装配**

`MainWindow` 的角色是**装配者而非实现者**：创建并连接文档、画布、工具管理器与各面板，
自身不持有任何几何或文件格式逻辑。图层面板与属性面板是纯视图——只发意图信号，
所有变更由主窗口落到 `Document` 的方法上，从而保证每次变更都进入撤销栈。

---

## 3. 功能一览

**绘图**：直线（连续绘制）、多段线（含圆弧段、闭合/放弃）、圆（圆心半径/三点/两点/直径）、
圆弧（三点/圆心起点终点）、矩形、点。

**编辑**：移动、复制（连续放置）、旋转、缩放、镜像、删除；夹点拖拽编辑；窗选（自左向右）与交叉选（自右向左）。

**选择与捕捉**：单击拾取、Shift 增减、Ctrl+A 全选、对象捕捉（9 类）、栅格吸附、正交、极轴追踪。

**图层**：新建/删除/重命名、颜色、线型、线宽、开关/冻结/锁定；当前图层；锁定图层不可选择。

**视图**：滚轮缩放（以光标为锚点）、中键平移、缩放至范围/选择集、网格与坐标轴、深色画布。

**命令行**：命令补全、历史回溯、坐标与算式输入、`Esc` 取消、空格重复上一命令、`F1` 命令列表。

**文件**：DXF 导入导出（R12 / R2000，ASCII 与二进制读取）、拖放打开、DXF 目标版本可选。

---

## 4. 验证

验证分三层，均已在当前环境实测通过。

### 4.1 单元测试（4 个测试程序，全部通过）

| 测试 | 覆盖内容 |
|---|---|
| `test_geometry` | 变换复合次序与求逆、镜像自逆、包围盒、点到线段距离、五类图元的几何与夹点、圆弧三点定弧与镜像方向反转、凸度多段线的圆心/弧长/包围盒/镜像符号、坐标解析与算式求值 |
| `test_document` | id 稳定性、增删与绘制次序的撤销还原、变换撤销、夹点替换、图层重命名/删除的图元归属联动、选择集剪除、锁定的可选性、范围统计排除隐藏图层、批量装载不污染撤销栈 |
| `test_dxf_roundtrip` | R12 与 R2000 的写出→读回逐图元比对（类型/图层/颜色/可见性/几何）、二进制 DXF 解析、未支持实体的计数上报、DWG 版本签名识别 |
| `test_dwg_backend` | DWG 版本签名表、内容探测、无后端时的可执行指引、外部转换器的完整读写链路、转换器失败的上报 |

```
100% tests passed, 0 tests failed out of 4
```

### 4.2 DXF 互操作性交叉校验（用 ezdxf 独立实现）

自身往返测试只能证明「自己写、自己读」自洽，无法发现「双方都偏离规范」的情形。
因此用第三方严格实现 ezdxf 做双向交叉校验：

**正向（LyCAD 写出 → ezdxf 读入）**：`tools/verify_dxf_with_ezdxf.py`

- R12 与 R2000 两份输出**均通过 ezdxf 严格审计器的全部检查，零错误、零修复**
- 版本号、图层表（含中文名 `\U+XXXX` 转义还原）、线型表、坐标、角度（30°/250° 精确）、
  颜色索引、不可见标志（60 组码）、多段线正负凸度全部正确
- ezdxf 可无损回写我们的文件

```
结论：全部 75 项检查通过——LyCAD 导出的 DXF 可被独立实现正确读取
```

**反向（ezdxf 写出 → LyCAD 读入）**：`tools/verify_lycad_reads_ezdxf.py`

刻意覆盖需要语义转换的内容：块定义 + INSERT（含缩放/旋转）、嵌套块、
旧式 `POLYLINE/VERTEX`、中文图层名、以及 TEXT/HATCH 等未建模实体的混入。
期望值由脚本从源文档按同一规则推导，而非写死数字。

- 图元数量与图层分布**与推导值完全一致**
- 块展开的几何正确：半径 3 按 xscale=2 缩放为 6（周长 37.699）、嵌套块按 0.5 缩放为 1.5
- 块内 0 层图元正确继承 INSERT 的图层
- 含凸度多段线周长 111.9093 与手算值一致；闭合 `POLYLINE` 周长 120（含收尾段）
- TEXT/HATCH 被计数上报而非静默丢失

```
结论：全部 20 项检查通过——LyCAD 能正确读取 ezdxf 写出的文件
```

### 4.3 界面与交互链路验证

`tools/lycad_ui_snapshot` 在进程内渲染主窗口为 PNG，并用 `QTest` 注入**真实鼠标事件**
驱动完整交互链路（画布 → ToolManager → 工具状态机 → 文档）。它的成功运行本身就是一次端到端功能验证：

```
已保存 ui_overview.png  (1500×950)
交互链路：已创建图元总数 12        ← 10 个来自文件 + 2 条由模拟点击实际绘制
已保存 ui_draw_preview.png  (1500×950)   ← 含命令执行中的橡皮筋预览
已保存 ui_selection.png  (1500×950)      ← 含选中高亮与夹点
```

该工具还暴露并促成了两个真实缺陷的修复：状态栏图元计数不随增删刷新、选中高亮过重掩盖图元原色。

### 4.4 复现验证

```bash
cmake --build build -j && ctest --test-dir build --output-on-failure

build/bin/lycad_sample /tmp/out                    # 生成参考 DXF
python3 tools/verify_dxf_with_ezdxf.py /tmp/out    # 正向交叉校验
python3 tools/verify_lycad_reads_ezdxf.py build/bin/lycad_sample /tmp/rev   # 反向交叉校验

QT_QPA_PLATFORM=offscreen build/bin/lycad_ui_snapshot /tmp/out/lycad_sample_2000.dxf /tmp/shots
```

---

## 5. 关于 DWG 支持

DWG 是 Autodesk 的闭源二进制格式：位压缩编码、分节映射、对象级 CRC 校验，
且各版本编码方式差异显著。从零实现一个可靠的 DWG 解析器属于独立工程，
本项目不这样做——**硬做出来的半成品会静默损坏用户图纸，这比不支持更糟**。

采用业界通行的**后端适配器**方案：`DwgCodec` 负责版本识别与流程编排，
实际转换交给外部工具，转换后的 DXF 再交由本项目自带、已经过交叉验证的 DXF 编解码器处理。

后端按优先级自动探测：

1. **自定义转换命令**（设置项 `dwg/converterCommand`，含 `%IN%` / `%OUT%` 占位符）
2. **LibreDWG** —— `dwg2dxf` / `dxf2dwg`
3. **ODA File Converter** —— Autodesk 官方免费工具（目录进目录出式 CLI）

当前构建环境未安装任何转换器，因此 DWG 路径退化为**明确的能力提示**而非静默失败：

```
当前系统未检测到可用的 DWG 转换后端，因此 DWG 导入导出不可用。
DWG 为闭源二进制格式，本软件通过外部转换器完成读写，请任选其一安装：
  · LibreDWG  —— 提供 dwg2dxf / dxf2dwg 命令
  · ODA File Converter —— Autodesk 官方免费转换工具
  · 或在 设置 → DWG 转换命令 中填写自定义转换命令模板（含 %IN% 与 %OUT% 占位符）
在未安装前，DXF 的导入导出完全可用。
```

这条链路本身是被测试覆盖的：`test_dwg_backend` 注册一个行为确定的替代转换器脚本，
走完**真实的进程调用、真实的临时目录编排、真实的失败分支**，验证读取、写出与错误上报。

> 若需要进程内 DWG 解析，`.gitignore` 外的接入点已经就绪：CMake 会自动探测 libdxfrw
> （`LYCAD_LIBDXFRW_FOUND`），`io/CMakeLists.txt` 会定义 `LYCAD_HAVE_LIBDXFRW`，
> 只需实现一个 `DwgCodec::Backend` 即可，接口层无需改动。

---

## 6. 已知限制

| 限制 | 说明 |
|---|---|
| 圆/圆弧在非等比缩放下被近似 | 内部不表达椭圆，非等比缩放按等效均匀缩放处理。等比缩放与镜像下为精确结果。曲线上表现为近似，直线上无影响。 |
| 块引用导入即展开 | 当前不建模块定义体系，`INSERT` 在导入时按变换展开为独立图元。图形内容零丢失，代价是文件往返后块被打散（导入结果中会明确告知）。 |
| 未实现逆向编辑 | 不支持 OFFSET / TRIM / EXTEND / FILLET / CHAMFER / STRETCH；未实现文字、标注、图案填充、样条曲线、椭圆。刻意保留为未实现状态而非半成品。 |
| 线型为内置标准集 | 支持 CONTINUOUS / DASHED / DASHDOT / DASHDOTDOT / DOT / HIDDEN / CENTER / PHANTOM / BORDER；文件中引用的自定义线型会回退为 CONTINUOUS。 |
| 单图纸空间 | 无布局/视口体系，仅在模型空间工作。 |
| 不支持对象编组与属性块 | — |

---

## 7. 快捷键

| 按键 | 功能 | 按键 | 功能 |
|---|---|---|---|
| `Esc` | 取消当前命令 | `F1` | 命令列表 |
| `空格` / `回车` | 重复上一命令 | `F3` | 对象捕捉开关 |
| `Delete` | 删除选中对象 | `F7` | 网格显示开关 |
| `Ctrl+A` | 全选 | `F8` | 正交模式开关 |
| `Ctrl+Z` / `Ctrl+Y` | 撤销 / 重做 | `F9` | 栅格吸附开关 |
| `中键拖拽` | 平移视图 | `F10` | 极轴追踪开关 |
| `滚轮` | 缩放视图（以光标为锚点） | `左→右框选` | 窗选（完全包含） |
| `Tab`（命令行） | 命令补全 | `右→左框选` | 交叉选（接触到） |
| `↑` / `↓`（命令行） | 历史回溯 | | |

---

## 8. 许可与致谢

本项目代码为原创实现。DXF 格式依据 Autodesk 公开发布的 *DXF Reference* 实现；
互操作性验证使用 [ezdxf](https://ezdxf.mozman.at/)（MIT 许可）。
DWG 支持依赖可选的第三方转换器，它们的许可与分发条款由各自项目决定，本项目不打包分发。
