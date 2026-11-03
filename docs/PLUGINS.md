<!-- 由 tools/lightcpp_pm.py docs 生成，请勿手工编辑 -->

# 插件清单

共 **47** 个组件：42 个插件，5 个扩展包。

## 核心基础层

### `cpp-base` — C/C++ 基础

整个 C/C++ 插件生态的最底层强依赖：语法高亮、基础编译器支持（libclang / gcc / llvm / msvc 含 clang-cl）；仅当安装调试器插件时提供调试底层能力。

- 依赖：无
- 功能集：`lang.cpp`
- 平台：全平台

## 语言服务器与语言支持

### `asm` — 汇编支持

为 x86 系与 Arm 系汇编提供高亮与编译支持；其他架构不在支持范围内。

- 依赖：无
- 功能集：`lang.asm`
- 平台：全平台

### `ccls` — ccls

另一套 C/C++ LSP 实现，与 clangd 共存，用户可在设置中选择其一作为当前项目的 LSP 引擎。

- 依赖：`cpp-base`
- 功能集：`intel.core`, `intel.nav`
- 平台：全平台

### `clang-core` — Clang Core

基于 clangd 的 LSP 引擎，内置 clang-format 与 clang-tidy。单文件格式化优先调用 clangd 内置能力，多文件 / 项目级格式化调用独立 clang-format；打开项目时自动发现 .clang-format / .clang-tidy 并提供轻量设置页。

- 依赖：`cpp-base`
- 功能集：`lang.buildfile`, `clang.format`, `clang.tidy`, `clang.discovery`, `clang.settings`, `intel.core`, `intel.nav`, `analysis.engine`, `analysis.quality`
- 平台：全平台

### `qt-cpp-core` — Qt C++ Core

Qt C++ 项目核心支持，内含 QMake 构建能力。

- 依赖：`cpp-base`, `qmake`, `build-core`
- 功能集：`qt.build`
- 平台：全平台

### `qt-qml` — Qt QML

QML / JavaScript 语言支持与 Qt Quick 诊断。

- 依赖：`cpp-base`
- 功能集：`lang.qml`
- 平台：全平台

### `qt-ui` — Qt UI

.ui 设计文件预览与 .qrc 资源管理。

- 依赖：`cpp-base`
- 功能集：`lang.ui`
- 平台：全平台

## 项目与构建系统核心框架

### `build-core` — 项目与构建系统核心

所有具体构建系统插件的前置核心框架：提供统一的项目管理接口、构建任务调度引擎，并向下分发项目/配置/工具链/执行/诊断/高级特性等标准化构建能力。

- 依赖：`cpp-base`
- 功能集：`project.core`, `build.config`, `build.toolchain`, `build.exec`, `build.diagnostics`, `build.advanced`
- 平台：全平台

## 具体项目 / 构建系统支持

### `bazel` — Bazel

解析 BUILD / WORKSPACE 文件并调用 Bazel 构建。

- 依赖：`cpp-base`, `build-core`
- 功能集：`lang.buildfile`
- 平台：全平台

### `cmake` — CMake

解析 CMakeLists.txt 完成配置与构建；安装时自动连带安装 Ninja 作为默认生成器。

- 依赖：`cpp-base`, `build-core`
- 功能集：`lang.buildfile`
- 平台：全平台
- 推荐：`ninja`（自动安装）

### `compile-commands` — compile_commands.json 导入

选择一个现成的 compile_commands.json，以「无构建系统」方式打开、配置并索引 C/C++ 项目。

- 依赖：`cpp-base`, `build-core`
- 平台：全平台

### `custom-command` — 自定义命令

脱离标准构建系统，自定义编译/构建的 Shell 或批处理命令。

- 依赖：`cpp-base`, `build-core`
- 平台：全平台

### `devcpp` — Dev-C++ Project

解析 .dev 工程文件并构建；额外强依赖 Make & Makefile 插件。

- 依赖：`cpp-base`, `build-core`, `make`
- 平台：win32

### `make` — Make & Makefile

解析传统 Makefile 并执行构建；GNU 扩展包与 Dev-C++ 项目的必备依赖。

- 依赖：`cpp-base`, `build-core`
- 功能集：`lang.buildfile`
- 平台：全平台

### `meson` — Meson

解析 meson.build 完成配置与构建；安装时自动连带安装 Ninja 作为默认后端。

- 依赖：`cpp-base`, `build-core`
- 功能集：`lang.buildfile`
- 平台：全平台
- 推荐：`ninja`（自动安装）

### `ninja` — Ninja

直接读取并构建已有的 build.ninja。CMake / Meson 安装时自动连带安装，扩展包安装时一并安装，并作为推荐后端被联动调用。

- 依赖：`cpp-base`, `build-core`
- 功能集：`lang.buildfile`
- 平台：全平台

### `qmake` — Qt QMake

解析 .pro / .pri 工程文件并驱动 qmake 构建，是 Qt C++ Core 的构建基础。

- 依赖：`cpp-base`, `build-core`
- 功能集：`lang.buildfile`
- 平台：全平台

### `qt-cmake` — Qt CMake

Qt 6 的 CMake 集成（qt_add_executable / qt_add_library 识别）。

- 依赖：`cpp-base`, `build-core`, `cmake`
- 功能集：`qt.cmake`
- 平台：全平台

### `vs-project` — Visual Studio Project

解析 .vcxproj / .sln 并通过 MSBuild 构建，支持 Windows 平台目标。

- 依赖：`cpp-base`, `build-core`
- 平台：win32

### `xcodebuild` — xcodebuild

解析 Xcode 工程并调用 xcodebuild 构建，仅 macOS。

- 依赖：`cpp-base`, `build-core`
- 平台：darwin

## 调试器

### `cdb` — Windows Debugger (CDB / WinDbg)

Windows 原生调试后端，基于 CDB / WinDbg 引擎，使用 natvis 可视化类型。

- 依赖：`cpp-base`
- 功能集：`debug.breakpoint`, `debug.control`, `debug.inspect`, `debug.attach`, `debug.symbols`, `debug.log`
- 平台：win32

### `gdb` — GDB

Linux / MinGW 场景的调试器后端，支持 core 文件、远程调试与 pretty-printer。

- 依赖：`cpp-base`
- 功能集：`debug.breakpoint`, `debug.control`, `debug.inspect`, `debug.attach`, `debug.symbols`, `debug.log`
- 平台：linux, win32

### `lldb` — LLDB

LLVM 生态调试器后端，支持 core 文件、远程调试与 pretty-printer。

- 依赖：`cpp-base`
- 功能集：`debug.breakpoint`, `debug.control`, `debug.inspect`, `debug.attach`, `debug.symbols`, `debug.log`
- 平台：linux, darwin, win32

### `vs-debugger` — Visual Studio Debugger

复用 Visual Studio 调试引擎，适用于 MSVC 工具链；与 Visual Studio Project 插件配合最佳。

- 依赖：`cpp-base`
- 功能集：`debug.breakpoint`, `debug.control`, `debug.inspect`, `debug.attach`, `debug.symbols`, `debug.log`
- 平台：win32
- 推荐：`vs-project`（建议安装）

## 工具与集成

### `boost-test` — Boost.Test

Boost.Test 适配器：解析 boost 单元测试输出。

- 依赖：`test-core`
- 功能集：`test.framework`
- 平台：全平台

### `catch2` — Catch2

Catch2 适配器：按标签过滤、解析断言失败位置。

- 依赖：`test-core`
- 功能集：`test.framework`
- 平台：全平台

### `ctest` — CTest

CTest 适配器：驱动 ctest -N 发现用例并解析测试结果。

- 依赖：`test-core`
- 功能集：`test.framework`
- 平台：全平台

### `doc-preview` — 文档预览

Markdown / reStructuredText 实时预览与内置浏览器。

- 依赖：无
- 功能集：`doc.preview`
- 平台：全平台

### `docs-core` — 文档与帮助

快速帮助、快捷键、插件文档、C/C++ 标准与第三方库文档、Doxygen 注释解析与 API 浏览。

- 依赖：无
- 功能集：`doc.core`
- 平台：全平台

### `git` — Git

完整的 Git 集成：克隆/打开仓库、分支与历史、合并与变基、冲突解决、子模块与 worktree、签名与认证。

- 依赖：无
- 功能集：`git.core`, `git.history`, `git.advanced`, `git.ui`, `git.auth`
- 平台：全平台

### `gtest` — GoogleTest

GoogleTest 适配器：发现 TEST/TEST_F 用例并解析 gtest 输出。

- 依赖：`test-core`
- 功能集：`test.framework`
- 平台：全平台

### `hex-viewer` — Hex 查看器

二进制文件的十六进制查看与编辑。

- 依赖：无
- 功能集：`tool.runtime`
- 平台：全平台

### `log-viewer` — 日志查看器

结构化日志查看、过滤与跟随（tail -f）。

- 依赖：无
- 功能集：`tool.runtime`
- 平台：全平台

### `media-preview` — 图像 / 资源预览

在编辑器内预览图像、字体与常见资源文件。

- 依赖：无
- 功能集：`tool.runtime`
- 平台：全平台

### `pkg-conan` — Conan 集成

集成 Conan 2 的 profile 与依赖解析，生成构建系统可消费的配置。

- 依赖：`build-core`
- 功能集：`tool.pkg`
- 平台：全平台

### `pkg-hunter` — Hunter 集成

基于 CMake 的 Hunter 包管理集成。

- 依赖：`build-core`, `cmake`
- 功能集：`tool.pkg`
- 平台：全平台

### `pkg-native` — 系统包管理器

对接 apt / brew / winget 的系统级依赖安装与检测。

- 依赖：`build-core`
- 功能集：`tool.pkg`
- 平台：全平台

### `pkg-vcpkg` — vcpkg 集成

检测并驱动 vcpkg 清单模式，向构建系统注入工具链文件与依赖信息。

- 依赖：`build-core`
- 功能集：`tool.pkg`
- 平台：全平台

### `serial-terminal` — 串口终端

串口连接、收发与日志记录，面向嵌入式场景。

- 依赖：无
- 功能集：`tool.runtime`
- 平台：全平台

### `terminal` — 终端与工具集成

命令面板、外部工具配置、运行/调试程序入口与 Shell 集成；内置终端在当前版本存在已知问题。

- 依赖：无
- 功能集：`tool.workbench`
- 平台：全平台

### `test-core` — 测试核心

测试列表视图、运行/重跑、超时控制、输出解析与覆盖率（gcov / lcov）的统一框架。

- 依赖：无
- 功能集：`test.core`, `test.coverage`
- 平台：全平台

### `wsl` — WSL 集成

在 Windows 上以 WSL 发行版作为工具链与运行环境。

- 依赖：`cpp-base`
- 功能集：`tool.runtime`
- 平台：win32

## 扩展包

### `cpp-extension-pack` — C/C++ 扩展包

面向现代 C/C++ 开发的一键安装集合，本身无业务逻辑，仅声明依赖。

- 依赖：`cpp-base`, `clang-core`, `ccls`, `build-core`, `cmake`, `make`, `meson`, `bazel`, `ninja`, `compile-commands`, `custom-command`, `asm`
- 平台：全平台

### `gnu-pack` — GNU 拓展包

GNU 工具链组合：C/C++ Core + Make + GDB。

- 依赖：`cpp-base`, `make`, `gdb`
- 平台：全平台

### `llvm-pack` — LLVM 拓展包

LLVM 工具链组合：C/C++ Base + Clang Core + Make + LLDB。

- 依赖：`cpp-base`, `clang-core`, `make`, `lldb`
- 平台：全平台

### `msvc-pack` — 微软 Windows 系统开发拓展包

Windows 原生开发组合：C/C++ Base + Visual Studio Project + CDB/WinDbg + Visual Studio Debugger。

- 依赖：`cpp-base`, `vs-project`, `cdb`, `vs-debugger`
- 平台：全平台

### `qt-pack` — Qt C++ 扩展包

Qt 生态组合，需用户按需手动安装（不包含在 C/C++ 扩展包内）。

- 依赖：`qt-cpp-core`, `qt-cmake`, `qt-qml`, `qt-ui`
- 平台：全平台

