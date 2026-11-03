# 规划追溯表

本表把 `lightcpp.1.md` 的每一节映射到仓库中的落地位置，便于确认"规划里的每一条都有人接"。
功能条目的完整列表见 [FEATURES.md](FEATURES.md)（自动生成）。

## 一、核心基础层

| 规划条目 | 落地 |
| --- | --- |
| C/C++ 基础插件：语法高亮 + 基础编译器支持 | `plugins/cpp-base`，功能 `lang.cpp.highlight` |
| 支持 libclang / gcc / llvm / msvc（含 clang-cl） | `cpp-base.contributes.compilers` |
| 作为生态最底层强依赖 | `layerDependency` 不变量：build-system 与 debugger 层必须依赖 `cpp-base` |

## 二、语言服务器与格式化层

| 规划条目 | 落地 |
| --- | --- |
| clangd 提供补全 / 跳转 / 诊断 | `plugins/clang-core` → `intel.core`、`intel.nav` |
| 单文件格式化优先用 clangd | `clang.format.single` |
| 多文件 / 项目级格式化用独立 clang-format | `clang.format.project` |
| ccls 与 clangd 共存、择一启用 | `contributes.lsp.exclusiveGroup = cpp-lsp`，`lightcpp-pm engines` |
| 内置 clang-format 与 clang-tidy | `clang.tidy`、`clang.format`、`clang.settings` |

## 三、项目与构建系统核心框架

| 规划条目 | 落地 |
| --- | --- |
| 所有构建系统插件的前置核心框架 | `plugins/build-core`，`layerDependency` 不变量 |
| 统一项目管理接口 + 任务调度 | `build-core.contributes.buildTaskEngine` |
| 向下分发第五部分全部标准化能力 | `build-core.capabilitySets` 覆盖 `project.*` / `build.*` 共 32 条功能 |

## 四、具体项目 / 构建系统支持

| 规划条目 | 落地插件 |
| --- | --- |
| CMake | `cmake` |
| Meson | `meson` |
| Qt QMake | `qmake`（同时被 `qt-cpp-core` 依赖） |
| Bazel | `bazel` |
| Make & Makefile | `make` |
| Ninja（装 CMake/Meson 自动连带） | `ninja` + `cmake/meson.recommends[ninja].autoInstall` |
| Visual Studio Project（.vcxproj/.sln + MSBuild） | `vs-project` |
| Dev-C++ Project（额外强依赖 Make） | `devcpp` + `devcpp-requires-make` 不变量 |
| xcodebuild | `xcodebuild`（`platforms: darwin`） |
| compile_commands.json 导入 | `compile-commands` |
| 自定义命令 | `custom-command` |
| 汇编（仅 x86 系 / Arm 系） | `asm` |

## 五、核心框架支持的功能清单

| 规划小节 | 功能集 | 条数 | 归属 |
| --- | --- | --- | --- |
| 1. 项目与文件管理 | `project.core` | 4 | build-core |
| 2. 构建配置管理 | `build.config` | 3 | build-core |
| 3. 工具链与编译参数管理 | `build.toolchain` | 7 | build-core |
| 4. 构建执行操作（含远程构建 planned） | `build.exec` | 10 | build-core |
| 5. 构建反馈与诊断 | `build.diagnostics` | 6 | build-core |
| 6. 高级构建特性 | `build.advanced` | 2 | build-core |

## 调试器功能

| 规划条目 | 功能集 | 归属 |
| --- | --- | --- |
| 8 类断点 | `debug.breakpoint` | gdb / lldb / cdb / vs-debugger（逐条 `except` 标注平台差异） |
| 单步 / 继续 / 暂停 / 停止 | `debug.control` | 同上 |
| 变量、调用栈、线程、寄存器、内存、反汇编、控制台 | `debug.inspect` | 同上 |
| 进程附加 / core 文件 / 远程调试 | `debug.attach` | 同上 |
| 符号加载、源码映射、pretty-printer、STL、智能指针、枚举 | `debug.symbols` | gdb / lldb 为主，natvis 归 cdb / vs-debugger |
| 调试日志窗口 | `debug.log` | 四个调试器 |

## 代码智能

| 规划条目 | 功能集 | 归属 |
| --- | --- | --- |
| 补全、语义高亮、诊断、快速修复、悬停、签名、定义、引用、符号搜索、重命名、格式化、include、宏展开、类型提示 | `intel.core`（15 条） | clang-core / ccls |
| 导航、调用层级、类型层级、继承、类视图、调用图、大纲、文档注释预览 | `intel.nav`（8 条） | clang-core / ccls |

## 静态分析与代码质量

| 规划条目 | 功能集 | 归属 |
| --- | --- | --- |
| Cppcheck、Clang Static Analyzer、PVS-Studio、Coverity、编译器警告分析 | `analysis.engine`（5 条） | clang-core |
| 规范检查、复杂度、未使用变量、内存泄漏、空指针、越界、编码规范、安全漏洞 | `analysis.quality`（8 条） | clang-core |

## Git 插件

| 规划条目 | 功能集 | 条数 |
| --- | --- | --- |
| 克隆、打开、分支、切换、创建、合并、rebase、cherry-pick、stash、commit、push、pull、fetch | `git.core` | 13 |
| 历史、blame、diff | `git.history` | 3 |
| 冲突、子模块、worktree、tag、remote | `git.advanced` | 5 |
| 文件状态、行内标记 | `git.ui` | 2 |
| 提交模板、GPG 签名、SSH/HTTPS 认证 | `git.auth` | 3 |

## 终端与工具集成

| 规划条目 | 功能集 / 插件 | 备注 |
| --- | --- | --- |
| 内置终端 | `tool.workbench` / `terminal` | 状态 `known-issue`，原文即标注"目前有问题" |
| 命令面板、外部工具、运行、调试程序、Shell 集成 | `tool.workbench` / `terminal` | — |
| vcpkg / conan / Hunter / apt / brew / winget | `tool.pkg` / `pkg-vcpkg`、`pkg-conan`、`pkg-hunter`、`pkg-native` | 系统包管理器合并为 `pkg-native`，按 os 分支 |
| WSL、串口、日志、Hex、图像预览 | `tool.runtime` | 一个功能集，五条功能分别归属五个插件 |

## 测试功能

| 规划条目 | 功能集 | 归属 |
| --- | --- | --- |
| 单元测试集成、列表、运行、跳转、解析、超时、日志、重跑 | `test.core`（9 条） | test-core |
| 覆盖率、gcov / lcov | `test.coverage`（2 条） | test-core |
| GoogleTest / Catch2 / Boost.Test / CTest | `test.framework`（4 条） | 对应适配器插件 |

## 文档和帮助

| 规划条目 | 功能集 | 归属 |
| --- | --- | --- |
| 快速帮助、API 文档、Doxygen、标准文档、第三方库、提示文档、快捷键、插件文档 | `doc.core`（8 条） | docs-core |
| Markdown / reStructuredText 预览、内置浏览器 | `doc.preview`（3 条） | doc-preview |

## Clang Core 插件功能要求

| 规划条目 | 落地 |
| --- | --- |
| 打开项目时扫描项目根及父目录的 `.clang-format` / `.clang-tidy` | `clang.discovery.scan` |
| 存在则自动应用，无需手动配置 | `clang.discovery.apply` |
| 设置页：clang-tidy 实时检查开关 | `clang.settings.tidy` → `lightcpp.clangCore.tidy.enable` |
| 设置页：默认 clang-format 风格（5 种） | `clang.settings.style` → `lightcpp.clangCore.format.style` |
| 设置页：自定义 clangd / clang-tidy 路径 | `clang.settings.path` → 两个 executable 设置项 |

## 扩展包

| 规划 | 落地 |
| --- | --- |
| C/C++ Extension Pack（12 项） | `cpp-extension-pack`，成员由 `cpp-pack-members` 不变量锁定 |
| 包内文件至少高亮（*.cmake / *.clangd / Makefile / *.ninja / CMakeLists.txt） | `lang.buildfile.highlight`，7 个插件共同声明 |
| GNU 拓展包（Core + Make + GDB） | `gnu-pack` |
| LLVM 拓展包（Base + Clang Core + Make + LLDB） | `llvm-pack` |
| 微软 Windows 拓展包（Base + VS Project + CDB + VS Debugger） | `msvc-pack` |
| Qt C++ Extension Pack（不在 C++ 包内，按需安装） | `qt-pack` |

## 建模时的取舍

1. **`qmake` 独立成插件**：规划中它既出现在"元构建系统"，又写在"Qt C++ Core（含 QMake）"里。
   落地上 `qmake` 是独立插件，`qt-cpp-core` 依赖它，两个入口都指向同一实现。
2. **系统包管理器合并**：apt / brew / winget 归入一个 `pkg-native` 插件，按操作系统分支，
   避免为三个命令行工具造三个插件、三份重复的检测逻辑。
3. **汇编插件不设硬依赖**：规划未要求它依赖 `cpp-base`，因此保持独立，只声明 x86 / Arm 的架构限制。
4. **调试能力按平台裁剪**：规划给出的是"调试器应当具备"的总清单，未区分平台。
   落地时用 `except` 标注 CDB / VS Debugger 缺失的日志断点、地址断点、内存断点，以及 GDB / LLDB 之外的 pretty-printer。
   这属于对规划的细化，不是删减 —— 差异在 `docs/FEATURES.md` 的归属列可见。
5. **远程构建、内置终端**：按原文保留，但状态分别标为 `planned` 与 `known-issue`，避免在 v0.1 验收时被当成已完成。
