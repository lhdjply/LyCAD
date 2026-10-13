# 打包与 CI

> 本文件是 LyCAD 文档的一部分，入口见仓库根目录的 [README](../README.md)。

`debian/` 下的文件就是全部打包逻辑，`debuild -us -uc` 即可出包：

```bash
sudo apt install build-essential devscripts dh-make equivs
mk-build-deps -i -t 'apt-get -y' ./debian/control   # 按 control 装齐构建依赖
debuild -us -uc                                     # 产物落在仓库的上一级目录
```

| 文件 | 内容 |
|---|---|
| `debian/control` | 构建依赖（CMake + Qt6 Core/Gui/Widgets/Svg/PrintSupport）与运行时依赖（`${shlibs:Depends}` 自动带上实际链接到的 Qt6 库）；界面文案是中文，最小系统上一个中文字体都没有时整屏是豆腐块，故 `Recommends: fonts-wqy-zenhei`；离屏渲染类用例需要字体，故构建依赖里有 `fonts-dejavu-core` |
| `debian/rules` | `dh` 默认流程 + 两处覆盖：构建时关掉不随包发布的开发工具；出包前真的跑一遍单元测试 |
| `debian/changelog` | 版本号。**必须与根 `VERSION` 一致**（`CMakeLists.txt` 的 `project(VERSION)` 也是从 `VERSION` 读的）——包名与界面显示的版本各取一处，不同步就会做出「包里写 0.1.0、界面显示 0.2.0」的东西 |
| `debian/copyright` | 项目自身（含 `debian/`）为 **Apache-2.0**；`third-party/libdxfrw-modify` 是 GPL-2.0-or-later 且链接进同一个二进制，所以分发二进制时整体按 GPL 处理（许可与分发的说明见根目录 README 的「致谢」一节） |
| `debian/source/format` | `3.0 (native)`——本包不带 Debian 修订号，`debuild` 直接对工作树出包，不需要上游 orig.tar.gz |
| `debian/watch` | 只跟踪上游 tag（native 包不需要下载上游源码包） |

两处刻意的取舍：

- **没有 `debian/*.install`**。可执行文件、`.desktop` 条目、hicolor 图标主题里的 PNG
  全部由根 `CMakeLists.txt` 的安装规则安装，`dh_auto_install` 只是去调它。再写一份
  `.install` 等于把同一份清单抄第二遍，两边迟早跑偏。
- **测试真的会跑**。LyCAD 的测试目标是 `EXCLUDE_FROM_ALL` 的（日常构建只编主程序），
  而 `ctest` 不会自己编译缺失的二进制，所以 `debian/rules` 先显式构建 `lycad_tests`
  再跑；构建 chroot 里没有显示服务，用例自带 `QT_QPA_PLATFORM=offscreen`。
  嫌慢或构建环境不合适时，用 `DEB_BUILD_OPTIONS=nocheck debuild -us -uc` 跳过。
  这一步是整条 deb 流程里最重的（11 个测试目标各自编入整套 headless 源码，约 520 次
  编译，原因见 [architecture.md](architecture.md) 里「每个可执行文件各编一次」那段），所以并行度必须显式给数字：
  `cmake --build --parallel` 不带数字时，Make 生成器下会变成 `make -j`（**无限并发**），
  几百个 `g++` 一起抢内存，在构建容器里看着就像卡住。`debian/rules` 取
  `DEB_BUILD_OPTIONS` 里的 `parallel=N`（CI 传的是容器自身的 `nproc`），没给就退回
  `nproc`；`debuild` 不带 `-j` 时主程序那份本是单线程编的，同样靠这个值并行起来。
  构建目录固定为 `build/deb`（而不是让 debhelper 取 `obj-<triplet>`）——上面的测试步骤
  要按名字构建一个 `EXCLUDE_FROM_ALL` 目标，目录名自己拿在手里最省事；该目录由
  `debian/clean` 负责清理。

`.github/workflows/` 下的三条工作流：

| 文件 | 内容 |
|---|---|
| `build.yaml` | Linux 与 Windows(MinGW) 各编一遍主程序；Linux 另外显式编一遍开发工具，免得工具失修 |
| `test.yaml` | 两平台各构建 `lycad_tests` 再跑 `ctest`；失败时用 `--rerun-failed -V` 把完整输出打进 CI 日志 |
| `release.yaml` | 四架构 deb（QEMU + Deepin 容器；非 amd64 因仿真过慢用 `DEB_BUILD_OPTIONS=nocheck` 跳过测试）+ Windows 的 zip 与 setup.exe，汇总后发 Release |

Windows 侧统一走 **MSYS2 / MinGW** 而不是 MSVC：DWG 的文本编码转换依赖 iconv
（`third-party/libdxfrw-modify/src/intern/drw_textcodec.cpp`），MSVC 侧没有现成的
iconv 可链接，MinGW 侧由 MSYS2 的 libiconv 提供（链接期 `-liconv`，运行时需要
`libiconv-2.dll`，打包时会随 `bin/` 一起带走）。Windows 产物有两种形态：`zip`
解压即用；`setup.exe` 由 Inno Setup 6 编译 `resources/win/lycad.iss` 得到（带
`resources/win/ChineseSimplified.isl` 简体中文语言包，要求 Inno Setup 6.5.0+；
CI 里若缺 Inno 会自行安装）。

Windows 侧踩过的坑（这几条**只在 Windows 上出现**，Linux/macOS 的测试覆盖不到）：

- **测试步骤必须在 msys2 shell 里跑，不要给它加 `shell: bash`**。Git Bash 启动时会
  把 Git for Windows 自带的 `/mingw64/bin` 插到 PATH 前面，那里有 `libstdc++-6.dll`、
  `libgcc_s_seh-1.dll`、`libwinpthread-1.dll`；但那套是 **msvcrt** 运行时，而测试是
  **MSYS2 UCRT64** 编的。加载到另一套运行时上属于未定义行为：实测 11 条用例全部挂死、
  零输出（ctest 只报 8 秒超时），而同一个二进制在 msys2 shell 里 `-functions` 正常
  退出 0、逐条 slot 也跑得动。`运行测试` 那一步现在会先打印 PATH 与 `ldd` 解析到的
  运行时 DLL 路径，若它们指向 Git 的 `mingw64` 就说明又踩回来了。
- **iconv 的编码名也只有 Windows 会踩**：glibc 认 `"UTF8"`、GNU libiconv 只认
  `"UTF-8"`，传错名字时上游会把 `(iconv_t)-1` 当描述符用而段错误。见
  [libdxfrw-modify.md](libdxfrw-modify.md) 里 `DRW_ExtConverter` 那条。
- **用例设了 `TIMEOUT`**（`tests/CMakeLists.txt`）：挂死必须变成失败，不能一路拖到
  作业的 6 小时上限。失败时 `定位挂死的用例` 那一步会逐条 slot、再逐个参数
  （`-o`、环境变量）重跑，把「卡在哪一步」直接打进日志。
