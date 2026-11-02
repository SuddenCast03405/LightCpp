# 架构说明

本文件描述 LightCpp 插件生态的结构与硬性规则。功能条目见 [FEATURES.md](FEATURES.md)，
组件清单见 [PLUGINS.md](PLUGINS.md)，与原始规划的对应关系见 [PLAN-TRACEABILITY.md](PLAN-TRACEABILITY.md)。

## 1. 分层与依赖方向

```
扩展包 (pack)                 ← 只声明依赖，无业务逻辑
    │
    ├── 语言层 (language)      clang-core / ccls / qt-qml / qt-ui / asm
    ├── 框架层 (build-framework) build-core
    ├── 构建系统层 (build-system) cmake / meson / qmake / bazel / make / ninja
    │                            vs-project / devcpp / xcodebuild
    │                            compile-commands / custom-command
    ├── 调试器层 (debugger)     gdb / lldb / cdb / vs-debugger
    └── 工具层 (tooling)        git / terminal / test-* / docs-* / pkg-*
                │
                └──────────────┴── 核心层 (core)  cpp-base
```

依赖只允许自上而下，`lightcpp-pm validate` 通过不变量 `layerDependency` 强制：

| 约束 | 规则 ID |
| --- | --- |
| 每个 `build-system` 插件必须依赖 `build-core` 与 `cpp-base` | `build-system-depends-core` |
| 每个 `debugger` 插件必须依赖 `cpp-base` | `debugger-depends-base` |
| `devcpp` 必须依赖 `make` | `devcpp-requires-make` |
| `cmake` / `meson` 必须自动连带安装 `ninja` | `cmake-pulls-ninja` / `meson-pulls-ninja` |
| `cpp-extension-pack` 成员固定（含 ninja） | `cpp-pack-members` |
| `qt-pack` 成员固定 | `qt-pack-members` |
| clangd 与 ccls 属于同一互斥组 | `lsp-engine-exclusive` |

## 2. 核心层：只有 `cpp-base`

`cpp-base` 提供三件事，其他插件不得重复实现：

1. C/C++ 语法高亮；
2. 编译器接入（libclang / gcc / llvm / msvc 含 clang-cl）；
3. 调试底层能力 —— 仅当安装了调试器插件时才会激活。

它是硬依赖链的终点，`dependencies` 为空。

## 3. 语言层：clangd 与 ccls 共存但互斥

两套 LSP 实现都声明 `contributes.lsp.exclusiveGroup = "cpp-lsp"`：

| 插件 | engine | 可执行文件 |
| --- | --- | --- |
| `clang-core` | `clangd` | `clangd`（可配置路径） |
| `ccls` | `ccls` | `ccls` |

- 安装层面：二者可以同时存在于 `cpp-extension-pack` 中，互不影响。
- 运行层面：同一项目只启用一个引擎，`lightcpp.lsp.engine`（clangd / ccls）由宿主设置提供。
- 工具层面：`lightcpp-pm resolve` 会打印 `互斥组 cpp-lsp … 运行时仅启用其一`；`lightcpp-pm engines` 可单独查看。

### 3.1 格式化策略（clang-core 专属）

| 场景 | 实现 |
| --- | --- |
| 单文件格式化 | 走 clangd 的 `textDocument/formatting`（内置 clang-format 能力） |
| 多文件 / 项目级格式化 | 调用独立 `clang-format` 可执行文件批量处理 |

配置发现：打开项目时扫描项目根目录及其父目录的 `.clang-format` 与 `.clang-tidy`；
存在则自动应用，不存在的部分回退到设置项（默认风格 LLVM，可选 Google / Chromium / Mozilla / WebKit）。

设置页只有三个开关，避免把 IDE 做成编译器前端配置器：

- `lightcpp.clangCore.tidy.enable`：clang-tidy 实时检查（默认关，大项目友好）
- `lightcpp.clangCore.format.style`：无 `.clang-format` 时的回退风格
- `lightcpp.clangCore.executable.clangd` / `.clangTidy`：自定义可执行文件路径

## 4. 框架层：能力由 `build-core` 定义，行为由构建系统插件提供

`build-core` 独占 28 条构建功能（项目模型、构建配置、工具链参数、执行、诊断、高级特性）。
具体构建系统插件只声明"我能构建什么"（`contributes.buildSystems`）与文件关联，
不重复声明这些能力 —— 这样新增构建系统时不需要复制 28 条功能描述。

例外是**离群能力**：`compile-commands` 与 `custom-command` 以"无构建系统"方式工作，
它们仍然依赖 `build-core` 以复用任务调度与输出解析。

## 5. 调试器层的平台边界

| 插件 | 后端 | 平台 | 典型工具链 |
| --- | --- | --- | --- |
| `gdb` | GDB | linux, win32 | GCC / MinGW |
| `lldb` | LLDB | linux, darwin, win32 | Clang / LLVM |
| `cdb` | CDB / WinDbg | win32 | MSVC / MSBuild |
| `vs-debugger` | Visual Studio Debugger | win32 | MSVC / Visual Studio |

能力差异通过功能条目的 `plugins` / `except` 表达：pretty-printer、STL 容器展开、智能指针展开只归 GDB / LLDB；
natvis 可视化器只归 CDB / VS Debugger；日志断点在 CDB 与 VS Debugger 上不提供。
这样"功能目录"既不会假装人人都有，也不会因为个别平台缺失而整体缺项。

推荐组合由扩展包承担：

- `gnu-pack` = `cpp-base` + `make` + `gdb`
- `llvm-pack` = `cpp-base` + `clang-core` + `make` + `lldb`
- `msvc-pack` = `cpp-base` + `vs-project` + `cdb` + `vs-debugger`

## 6. 扩展包：零逻辑，只声明依赖

扩展包 `kind = "pack"`，Schema 层面禁止 `contributes` 与 `capabilitySets`，校验器另有 `E017` / `E018` / `E019` 兜底：

| 扩展包 | 成员 |
| --- | --- |
| `cpp-extension-pack` | cpp-base, clang-core, ccls, build-core, cmake, make, meson, bazel, ninja, compile-commands, custom-command, asm |
| `gnu-pack` | cpp-base, make, gdb |
| `llvm-pack` | cpp-base, clang-core, make, lldb |
| `msvc-pack` | cpp-base, vs-project, cdb, vs-debugger |
| `qt-pack` | qt-cpp-core, qt-cmake, qt-qml, qt-ui |

**Qt 生态刻意不进入 `cpp-extension-pack`**：Qt 相关插件按规划属于"特定生态插件"，需用户按需手动安装。

## 7. 依赖的三个等级

| 等级 | 字段 | 语义 | 例子 |
| --- | --- | --- | --- |
| 硬依赖 | `dependencies` | 缺失则无法安装，解析时自动补齐 | `cmake` → `build-core` |
| 自动连带 | `recommends[].autoInstall = true` | 随插件一并安装，`resolve` 会带入并标注 | `cmake` → `ninja` |
| 建议安装 | `recommends[].autoInstall = false` | 仅提示，不自动安装 | `vs-debugger` → `vs-project` |

## 8. 状态标记

功能条目支持三种状态，`coverage` 命令按域统计：

- `required`：v0.1 验收范围内，必须至少有一个归属插件（否则 `E023`）；
- `planned`：规划中但明确暂不支持（远程构建），不参与验收；
- `known-issue`：已有归属但当前不可用（内置终端），需在 `note` 里写明兜底方案。

## 9. 校验规则索引

| 代码 | 含义 |
| --- | --- |
| `E001`–`E009` | 字段完整性、ID 与目录一致、kind/layer/platform 取值、版本格式 |
| `E010`–`E015` | 依赖不存在、版本不满足、约束非法、依赖扩展包、推荐目标非法、autoInstall 缺 reason |
| `E016` | 引用未定义的功能集 |
| `E017`–`E019` | 扩展包声明 contributes / 无成员 / 声明 capabilitySets |
| `E020` | 依赖环 |
| `E021`–`E023` | 功能集无人使用、功能归属插件不存在、必需功能无归属 |
| `E024`–`E025` | 互斥组成员不足、engine 重名 |
| `E100`–`E105` | 规划不变量未满足（依赖 / 自动安装 / 扩展包成员 / 层级依赖 / 互斥组） |
