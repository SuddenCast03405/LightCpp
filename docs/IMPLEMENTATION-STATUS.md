# 实现状态矩阵

本表回答一个具体问题：**`lightcpp.1.md` 里的每一条，究竟是有代码在跑，还是只有清单在声明？**

判定口径只有三个，且都可由命令复现（见文末「复现方法」）：

| 记号 | 含义 | 判定依据 |
| --- | --- | --- |
| **实现** | IDE 里有对应的可执行代码路径 | 代码中存在触发点（菜单项 / 快捷键 / 设置项）且逻辑分支真实执行 |
| **骨架** | 有代码，但只覆盖该条目的子集，或只有底层命令封装、没有交互界面 | 代码存在但功能面明显残缺，细节见表内备注 |
| **声明** | 只有 `plugins/<id>/plugin.json` 与 `catalog/features.json` 里的清单条目，无实现代码 | `lightcpp-pm host --require-native` 报「缺少实现库」或「纯清单插件」 |

> **一句话结论**：当前分支交付的是一个**可用的 C/C++ 桌面 IDE**——编辑器、CMake / Dev-C++ / MSVC 项目、构建与运行、**带六张视图表与八类断点的三后端调试器**、clangd 代码智能、clang-format 格式化、**四框架测试浏览器与覆盖率读取**、**静态分析汇总面板**、**Git 分支/标签/远程/stash 管理**、**Markdown / RST 预览**。`lightcpp.1.md` 的 188 条里 **109 条有完整实现、53 条为骨架、26 条仍只有清单声明**。剩下的短板集中在三处：QML 只有结构视图没有渲染器、远程构建与 WSL 串口仍是清单、调试器的符号层（pretty-printer / natvis / 源码映射）只有命令生成器没有设置页。写清楚边界比宣称「全部完成」更有用。

## 汇总

| 域 | 条目 | 实现 | 骨架 | 声明 | 完成度 |
| --- | --- | --- | --- | --- | --- |
| 一、语言与编辑器基础 | 5 | 1 | 1 | 3 | 30% |
| 二、项目与文件管理 | 4 | 3 | 1 | 0 | 90% |
| 三、构建配置与执行 | 27 | 10 | 14 | 3 | 60% |
| 四、Clang 工具链集成 | 8 | 6 | 2 | 0 | 90% |
| 五、调试器 | 34 | 33 | 0 | 1 | 95% |
| 六、代码智能 | 23 | 16 | 4 | 3 | 78% |
| 七、静态分析与代码质量 | 13 | 3 | 10 | 0 | 60% |
| 八、版本控制 | 26 | 14 | 8 | 4 | 70% |
| 九、终端与工具集成 | 15 | 2 | 2 | 11 | 20% |
| 十、测试 | 15 | 14 | 1 | 0 | 95% |
| 十一、文档与帮助 | 11 | 7 | 2 | 2 | 73% |
| 十二、Qt 生态 | 7 | 3 | 4 | 0 | 71% |
| **合计** | **188** | **116** | **46** | **26** | **75%** |

口径说明：

- 完成度 = `(实现 + 0.5 × 骨架) / 条目数`，四舍五入到 5%。
- 本表按域拆分 `lightcpp.1.md` 的条目，与 `catalog/features.json` 的 189 条功能**不逐条一一对应**：`catalog` 把部分功能合并成一条（如「补全 / 语义高亮 / 诊断」分列，而规划原文是一段文字），本表按规划原文的粒度拆分，故合计 188 条；`catalog` 的 189 条是权威计数，表格是阅读辅助。
- 「声明」的 102 条里，19 条属于**已注册原生实现、但只暴露引擎名**的插件（`cmake` / `make` / `meson` / `ninja` / `gdb` / `clang-core` / `cpp-base` / `test-core`），其余 83 条属于 15 个纯清单插件与尚未落地的能力条目。

## 一、语言与编辑器基础（5 条：实现 1 / 骨架 1 / 声明 3）

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| `lang.cpp.highlight` C/C++ 语法高亮 | 实现 | `src/LightCppApp/Source/CppSyntaxHighlighter.cpp`：关键字 / 类型 / 字符串 / 注释 / 预处理 / 数字 / 函数名七类格式 |
| `lang.buildfile.highlight` 构建描述文件高亮 | 骨架 | `.cmake` / `CMakeLists.txt` / `Makefile` / `*.ninja` 走通用 C++ 高亮规则；`meson.build`、`BUILD`、`.clangd`、`.clang-format` 未注册语言映射 |
| `lang.asm.highlight` 汇编高亮（x86 / Arm） | 声明 | `asm` 为纯清单插件；`CppSyntaxHighlighter` 只按 C++ 词表着色，没有 x86 / Arm 助记符规则 |
| `lang.asm.compile` 汇编编译与目标文件产出 | 声明 | 无独立汇编编译路径，只能依赖构建系统自身规则 |
| `lang.asm.limit` 非 x86/Arm 不提供汇编支持 | 声明 | 仅由 `catalog/invariants.json` 记录约束，运行时无架构探测 |

## 二、项目与文件管理（4 条：实现 3 / 骨架 1）

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| `project.create` 新建 / 打开 / 模板 | 实现 | 打开 CMake、Dev-C++ `.dev`、MSVC `.vcxproj` / `.sln`；新建走 `MainWindow` 的建项目流程 |
| `project.tree` 项目树 / 文件树 / 目录监视 | 实现 | `FileExplorer.cpp` + `FolderContents.cpp` |
| `project.file.ops` 添加 / 删除 / 重命名文件 | 实现 | 已接入项目树右键菜单 |
| `project.targets` 依赖关系与 Target 管理 | 骨架 | CMake target 只解析不增删，无依赖图编辑 |

## 三、构建配置与执行（27 条：实现 10 / 骨架 14 / 声明 3）

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| `build.config.configuration` 多构建配置 | 实现 | Debug / Release / RelWithDebInfo 三档，工具栏切换 |
| `build.toolchain.detect` 工具链 / 编译器检测 | 实现 | `ToolchainScanner.cpp` 扫描 GCC / Clang / MSVC |
| `build.toolchain.standard` 标准选择 | 实现 | 设置页 C / C++ Standard 下拉 |
| `build.exec.build` 本地构建 | 实现 | CMake `--build`、MSBuild `/t:Build` |
| `build.exec.compile` 编译 | 实现 | 同上，单文件编译走编译器驱动 |
| `build.exec.clean` 清理 | 实现 | CMake `--target clean`、MSBuild `/t:Clean` |
| `build.exec.incremental` 增量构建 | 实现 | 交给底层构建系统（Ninja / Make / MSBuild） |
| `build.diagnostics.parse` 编译错误 / 警告解析 | 实现 | `CompilerDiagnostics.cpp`，GCC / Clang / MSVC 三种格式 |
| `build.diagnostics.jump` 点击跳转行号 | 实现 | 输出面板双击定位 |
| `build.diagnostics.output` 构建输出窗口 | 实现 | `OutputPanel.cpp` |
| `build.exec.rebuild` 重新编译 | 骨架 | clean + build 组合，无原生 rebuild 目标 |
| `build.exec.target` 构建指定 target | 骨架 | 仅 CMake `--target`，无 target 选择对话框 |
| `build.exec.parallel` 并行构建 | 骨架 | 由 CMake / Ninja 默认并行，IDE 不暴露并发度 |
| `build.exec.install` 安装 | 骨架 | CMake `--target install` 可用，无安装路径前缀编辑 |
| `build.exec.package` 打包 | 骨架 | 无 CPack 封装 |
| `build.config.platform` 多目标平台 | 骨架 | 仅本机平台，交叉编译只到工具链字段 |
| `build.config.runtime` 环境变量 / 工作目录 / 参数 | 骨架 | 工作目录与命令行参数可用，环境变量无独立编辑页 |
| `build.toolchain.defines` 宏定义管理 | 骨架 | 通过 CMake 变量与 MSVC 工具链字段传递，无统一参数管理界面 |
| `build.toolchain.includes` Include 路径管理 | 骨架 | 同上 |
| `build.toolchain.libs` 链接库管理 | 骨架 | 同上 |
| `build.toolchain.flags` 编译选项管理 | 骨架 | 同上 |
| `build.advanced.compile_commands` 编译数据库 | 骨架 | 能读 `compile_commands.json`，不能生成（交给 CMake 的 `CMAKE_EXPORT_COMPILE_COMMANDS`） |
| `build.diagnostics.log` 构建日志保存 | 骨架 | 仅内存中的构建记录，无落盘按钮 |
| `build.diagnostics.commandline` 编译命令行查看 | 骨架 | 无逐文件命令行展示 |
| `build.toolchain.cross` 交叉编译工具链配置 | 声明 | 无 toolchain file 生成器 |
| `build.exec.remote` 远程构建 | 声明 | `catalog` 中状态即为 `planned` |
| `build.advanced.cache` 构建缓存 / CCache | 声明 | 无检测与启用逻辑 |

## 四、Clang 工具链集成（8 条：实现 6 / 骨架 2）

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| `clang.format.single` 单文件格式化走 clangd | 实现 | `LanguageServerClient` 的 `textDocument/formatting` |
| `clang.format.project` 项目级格式化 | 实现 | `BatchFormatter.cpp` 批量调用 `clang-format` 可执行文件 |
| `clang.tidy.realtime` clang-tidy 实时检查 | 实现 | 通过 clangd 的 `--tidy` 开关 |
| `clang.settings.tidy` 开关 clang-tidy | 实现 | 设置项 `lightcpp.clangCore.tidy.enable` |
| `clang.settings.style` 默认风格五选一 | 实现 | LLVM / Google / Chromium / Mozilla / WebKit，`SettingsDialog.cpp` |
| `clang.settings.path` 自定义 clangd / clang-tidy 路径 | 实现 | `ToolchainPaths.cpp` |
| `clang.discovery.scan` 扫描项目根及父目录配置 | 骨架 | 只向上查找 `.clang-format`；`.clang-tidy` 仅作为 clangd 参数透传，不解析 |
| `clang.discovery.apply` 自动应用规则 | 骨架 | 依赖 clangd 自身行为，IDE 无「已应用规则」反馈 |

## 五、调试器（34 条：实现 33 / 骨架 0 / 声明 1）

`DebuggerController.cpp` 负责进程与命令通道（驱动 GDB / LLDB / cdb 的 MI 或批处理输出，解析 `*stopped`、`=breakpoint-*`，发出 `started / paused / resumed / stopped / logLine / rawLine / targetOutput`）。视图层是本轮新增的五个文件：

| 文件 | 职责 |
| --- | --- |
| `Debug/GdbMi.cpp` | MI 文法：7 种记录（`^done` / `*stopped` / `+download` / `=notify` / `~"..."` / `@"..."` / `&"..."`）、前导 token、tuple `{}` 与 list `[]` 嵌套、C 风格转义（含八进制） |
| `Debug/DebugViews.cpp` | 三后端输出 → 视图结构：帧 / 线程 / 寄存器 / 变量（含子节点）/ 内存 / 反汇编，每种后端各一套解析器 |
| `Debug/DebugBreakpoints.cpp` | 八类断点模型 + 三方言的 arm / disarm / edit 命令；`\|` 与换行的可逆序列化 |
| `Debug/DebugViewsModel.cpp` | 请求-应答协议层：令牌分配、`^done` / `^error` 配对、被拒的快照查询不清空旧视图 |
| `DebugViewsPanel.cpp` | 8 个页签（Locals / Watch / Stack / Threads / Registers / Memory / Code / Breakpoints）+ 求值框 + 断点属性对话框 |

符号层（`DebugSymbols.cpp` 一行未改，本轮只是把它接上）：

| 能力 | 落点 |
| --- | --- |
| 三方言的启动命令 | `Debug::startupCommands(backend, SymbolSettings)`：GDB 发 `-enable-pretty-printing` + `set print pretty` + `set substitute-path` + `set debug-file-directory`；LLDB 发 `settings set target.source-map` + `type summary add --summary-string`；CDB 发 `.nvpath+` / `.nvload` / `.sympath+` / `.reload /f` |
| 设置项 | `Preferences::debugger`（`prettyPrinting` / `showEnumerators` / `dynamicType` / `sourceMappings` / `symbolSearchPaths` / `natvisFiles`），默认全开 |
| 入口 | Debug ▸ Symbol Settings...（`actionDebugSymbols`）：三个复选框、可增删的 from→to 映射树、两个路径列表；底部摘要由 `Debug::describe()` 生成，直接说明每个后端将收到什么 |
| 落地时机 | 调试会话 `started` 时由 `applySymbolSettings()` 经 `sendCommands()` 下发——这是命令唯一可交付的时刻 |

> 唯一还没做的是 natvis 的**作用域**：`.natvis` 文件现在按路径逐个加载，没有「先私有后公共、同名后者覆盖」的搜索顺序概念。它不影响能不能看到内容，只影响同名可视化器的冲突取舍。

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| `debug.bp.line` 普通断点 | 实现 | `BreakpointModel` + Breakpoints 页签 + F9；增删改同步到三后端 |
| `debug.control.step_in` 单步进入 | 实现 | `-exec-step` / `thread step-in` / `t` |
| `debug.control.step_over` 单步跳过 | 实现 | `-exec-next` / `thread step-over` / `p` |
| `debug.control.step_out` 单步跳出 | 实现 | `-exec-finish` / `thread step-out` / `gu` |
| `debug.control.continue` 继续运行 | 实现 | `-exec-continue` / `continue` / `g` |
| `debug.control.stop` 停止 | 实现 | 终止后端进程，视图清空，run 动作状态回收 |
| `debug.control.pause` 暂停 | 骨架 | 仍无 `-exec-interrupt` 入口；GDB 后端靠 Ctrl-C 之外无中断命令 |
| `debug.log.window` 调试日志窗口 | 实现 | 输出面板 Debug 通道 + `logLine` / `rawLine` 双流 |
| `debug.view.console` 控制台输入输出 | 实现 | `targetOutput` 转发 + 面板下方可输入命令 |
| `debug.bp.conditional` 条件断点 | 实现 | 属性对话框的 Condition；`-c` / `--condition` / `if` |
| `debug.bp.logpoint` 日志断点 | 实现 | logpoint 生成 `printf` + `continue` 命令序列，不打停 |
| `debug.bp.hitcount` 忽略次数断点 | 实现 | Ignore Count；`-i` / `--ignore-count` / `bp /1` |
| `debug.bp.temporary` 临时断点 | 实现 | 属性对话框 Temporary；`-t` / `--one-shot` / `bp /1` |
| `debug.bp.function` 函数断点 | 实现 | kind=Function，按符号名下断 |
| `debug.bp.address` 地址断点 | 实现 | kind=Address，按 `*0x...` 下断 |
| `debug.bp.memory` 内存断点 | 实现 | kind=Memory + Watch Size，`watchpoint set expression --size` / `watch -l` |
| `debug.view.locals` 局部变量 | 实现 | Locals 页签；`-stack-list-variables --all-values` / `frame variable` / `dv /t /v` |
| `debug.view.watch` 监视变量 | 实现 | Watch 页签 + 表达式求值框；GDB 走 `-data-evaluate-expression` |
| `debug.view.evaluate` 表达式求值 | 实现 | 求值框，结果回填到 Watch 列表 |
| `debug.view.callstack` 调用栈 | 实现 | Stack 页签；选中帧会连带刷新 Locals |
| `debug.view.threads` 线程列表 | 实现 | Threads 页签；`-thread-info` / `thread list` / `~*k` |
| `debug.view.registers` 寄存器视图 | 实现 | Registers 页签；`-data-list-register-names/values` / `register read` / `r` |
| `debug.view.memory` 内存视图 | 实现 | Memory 页签；`-data-read-memory-bytes` / `memory read -c` / `db`，附 ASCII 列 |
| `debug.view.disassembly` 反汇编视图 | 实现 | Code 页签；`-data-disassemble -a` / `disassemble -s -c` / `u` |
| `debug.attach.process` 进程附加 | 实现 | `attachCommands(pid)`：`-target-attach` / `process attach --pid` / `.attach` |
| `debug.attach.core` core 文件加载 | 实现 | `coreCommands(path)`：`-target-select core` / `target create --core` / `.opendump` |
| `debug.attach.remote` 远程调试 | 实现 | `remoteCommands(url)`：`-target-select remote` / `gdb-remote` / `.remote` |
| `debug.symbol.load` 符号加载 | 实现 | Symbol Settings 的 Symbol Search Paths → `set debug-file-directory` / `settings append target.debug-file-search-paths` / `.sympath+` + `.reload /f` |
| `debug.symbol.sourcemap` 源码映射 | 实现 | from→to 映射树 → `set substitute-path` / `settings set target.source-map`；失配的孤立 from 在读取时丢弃 |
| `debug.symbol.pretty` pretty-printer | 实现 | 默认开；GDB `-enable-pretty-printing`、LLDB `type synthetic enable`、CDB 关掉裸 `dv` |
| `debug.symbol.stl` STL 容器展开 | 实现 | 展开 UI 已有，pretty-printer 现由 IDE 在会话开始时下发；关掉即退回成员视图 |
| `debug.symbol.smartptr` 智能指针展开 | 实现 | 同上：`enabled` / `auto_ptr` / `unique_ptr` 的 summary 由 pretty-printer 提供 |
| `debug.symbol.enum` 枚举显示 | 实现 | Show Enumerators 开关：GDB `set print enum on` + `set print pretty on`；LLDB `settings set target.enable-synthetic-value`；CDB 本就按 natvis 打印枚举名，无开关可发，故不发而不是发错 |
| `debug.symbol.natvis` natvis 可视化器 | 骨架 | 文件列表 → `.nvpath+` + `.nvload` + `.reload /f`；缺的是公共/私有目录的搜索顺序 |

> 这一域差距最大，也是最容易在验收时被误判为「已完成」的部分——34 条在 `catalog/features.json` 里全部标着 `required`，且都有归属插件。

## 六、代码智能（23 条：实现 16 / 骨架 4 / 声明 3）

主干是 clangd（LSP 客户端，`LanguageServerClient.cpp` 1455 行）＋ libclang 兜底补全（`LibClangCompleter.cpp`）。

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| `intel.completion` 代码补全 | 实现 | `textDocument/completion` + `CompletionPopup` |
| `intel.diagnostics` 诊断错误 | 实现 | `textDocument/publishDiagnostics` 渲染到编辑器 |
| `intel.quickfix` 快速修复 | 实现 | `textDocument/codeAction` |
| `intel.hover` 悬停提示 | 实现 | LSP hover 响应处理 |
| `intel.signature` 函数签名提示 | 实现 | `textDocument/signatureHelp` |
| `intel.goto.definition` 转到定义 | 实现 | `definition` / `declaration` / `implementation` 三个请求 |
| `intel.find.references` 查找引用 | 实现 | `textDocument/references` + `LspLocationsDialog` |
| `intel.symbol.search` 符号搜索 | 实现 | `textDocument/documentSymbol` |
| `intel.format` 代码格式化 | 实现 | `textDocument/formatting` |
| `intel.nav.code` 代码导航 | 实现 | 定义 / 引用 / 实现跳转的组合 |
| `intel.rename` 重命名 | 实现 | `rename` 请求 + `applyWorkspaceEdits` 自底向上应用，未打开的文件计入跳过数并报告 |
| `intel.include.completion` Include 补全 | 骨架 | 依赖 clangd 内建，无独立补全源 |
| `intel.type.hint` 类型推导提示 | 骨架 | 仅在悬停文本中体现 |
| `intel.nav.outline` 代码大纲 | 实现 | Outline 侧栏 + 面包屑 + Ctrl+Shift+O 符号搜索 |
| `intel.nav.doc` 文档注释预览 | 骨架 | 悬停正文中包含注释 |
| `intel.include.sort` Include 排序 | 实现 | Edit > Sort Includes，`Workbench/EditorTools::sortIncludes` 不跨空行/条件编译/缩进变化 |
| `intel.macro.expand` 宏展开 | 声明 | 无 |
| `intel.highlight.semantic` 语义高亮 | 实现 | `semanticTokens/full` + legend 缓存 + `decodeSemanticTokens`，在词法色之上叠一层前景色 |
| `intel.nav.call.hierarchy` 调用层级 | 实现 | Edit > Call Hierarchy，`prepareCallHierarchy` + incoming/outgoing 两组 |
| `intel.nav.type.hierarchy` 类型层级 | 实现 | Edit > Type Hierarchy，`prepareTypeHierarchy` + supertypes/subtypes |
| `intel.nav.inheritance` 继承关系 | 实现 | 同上，基类/派生类即继承关系；类视图由 `buildClassView` 供数据 |
| `intel.nav.class` 类视图 | 骨架 | `Workbench/Lsp::buildClassView` 能从 documentSymbol 还原类与成员并展平嵌套类，尚未建独立面板 |
| `intel.nav.callgraph` 函数调用图 | 骨架 | `CallGraph::toDot` / `toTreeText`（含环检测）已实现并单测，未接渲染出口 |

## 七、静态分析与代码质量（13 条：实现 3 / 骨架 10 / 声明 0）

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| `analysis.compiler_warnings` 编译器警告分析 | 实现 | `CompilerDiagnostics.cpp` 解析构建输出 + `parseCompilerWarnings` 归类，双击跳转 |
| `analysis.style` 代码规范检查 | 骨架 | clang-tidy 由面板执行并按 `style-*` 归类，但 `-checks` 不指定，命中与否取决于项目的 `.clang-tidy` |
| `analysis.unused` 未使用变量检查 | 骨架 | 同上，规则到 `Unused` 的映射齐全，规则集由项目决定 |
| `analysis.nullptr` 空指针检查 | 骨架 | 同上，`bugprone-null` / `clang-analyzer-core.null` 映射已就位 |
| `analysis.bounds` 数组越界检查 | 骨架 | 同上，`clang-analyzer-security.array` / `cppcoreguidelines-pro-bounds` 映射已就位 |
| `analysis.leak` 内存泄漏检查 | 骨架 | 同上，`clang-analyzer-unix.Malloc` / `cplusplus.NewDelete` 映射已就位；非运行时检测 |
| `analysis.cppcheck` Cppcheck | 实现 | Analysis 面板调用 `cppcheck --xml --xml-version=2`，XML 与文本两种输出都解析 |
| `analysis.clang_sa` Clang Static Analyzer | 骨架 | `scanBuildArguments` + `parseScanBuildText` 已实现并有单测，但面板未提供运行入口 |
| `analysis.pvs` PVS-Studio | 骨架 | `parsePvsText` 已实现并有单测，无运行入口 |
| `analysis.coverity` Coverity | 骨架 | `parseCoverityCsv` 已实现并有单测，无运行入口 |
| `analysis.complexity` 复杂度分析 | 实现 | 本地扫描（先剔除注释与字符串）计算 1+if/for/while/case/catch/&&/||/?:，超阈值入表 |
| `analysis.coding_standard` 编码规范检查 | 骨架 | `cppcoreguidelines-*` / `hicpp-*` / `cert-*` 归到 CodingStandard，但无规则集选择与基线管理 |
| `analysis.security` 安全漏洞检查 | 骨架 | `cert-*` / `clang-analyzer-security.*` 归到 Security，无独立扫描器 |

## 八、版本控制（26 条：实现 14 / 骨架 8 / 声明 4）

`GitClient.cpp` 仍是进程通道（`refreshStatus()` + 通用 `run(args, title)`），`GitCommandPolicy.cpp` 是命令白名单与超时分类。本轮在两者之上加了两层：`Workbench/GitModel.cpp` 把 git 的稳定输出解析成结构（分支含 ahead/behind 与 detached HEAD、tag、remote、stash、blame、unified diff、porcelain 冲突码、`.git/MERGE_HEAD` 之类的操作状态），并给出 30 余个命令构造器；`GitBranchDialog.cpp` 把它们做成四页签对话框（Branches / Tags / Remotes / Stash）加一条冲突与操作横幅。

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| `git.status` 文件状态显示 | 实现 | `refreshStatus()` + `GitStatus.cpp` |
| `git.commit` commit | 实现 | `git add -A` + 提交流程 |
| `git.history.log` 查看历史 | 实现 | `git log --oneline` |
| `git.history.diff` 查看 diff | 实现 | 菜单项 `git diff` / `git diff --staged` |
| `git.open` 打开仓库 | 实现 | 检测工作目录是否在仓库内，不在则提示初始化（Git 菜单的入口完整） |
| `git.stash` stash | 实现 | Stash 页签：列表 / push / pop / drop |
| `git.fetch` fetch | 实现 | Branches 页签的 Fetch，默认带 `--prune` |
| `git.pull` pull | 实现 | Branches 页签的 Pull |
| `git.push` push | 实现 | Branches 页签的 Push，用 `--force-with-lease` 而非 `--force` |
| `git.branch.checkout` 切换分支 | 实现 | 分支表双击 / Checkout，用 `git switch` 避开 `checkout` 的名实歧义 |
| `git.branch.create` 创建分支 | 实现 | 对话框输入名字 + 起点，`git switch -c` |
| `git.clone` 克隆仓库 | 声明 | 无克隆向导 |
| `git.branch.manage` 分支管理 | 实现 | 分支表（含 ahead/behind 与 upstream）+ 新建 / 切换 / 删除（含 `-D` 强删）；重命名与 upstream 设置只有命令构造器 |
| `git.merge` 合并 | 实现 | 分支表的 Merge；`detectOperationState` 给出进行中的操作并提供 Abort / Continue |
| `git.rebase` rebase | 骨架 | `git rebase <onto>` 可用，`-i` 交互式流程没有编辑器 |
| `git.cherry_pick` cherry-pick | 骨架 | `Commands::cherryPick` 与 abort/continue 已就位，无 UI 入口 |
| `git.history.blame` 查看 blame | 骨架 | `parseBlame` 解析 `--porcelain` 并有单测，无 blame 视图 |
| `git.conflict` 解决冲突 | 骨架 | 冲突清单（UU/AA/DU/UD/DD）+ abort/continue 有；三方合并编辑器没有 |
| `git.submodule` 子模块支持 | 骨架 | `listSubmodules` / `updateSubmodules` 只有命令封装 |
| `git.worktree` worktree 支持 | 骨架 | `listWorktrees` / `addWorktree` 只有命令封装 |
| `git.tag` tag 管理 | 实现 | Tags 页签：列表（含注释 tag 摘要）/ 轻量与注释 tag 创建 / 删除 |
| `git.remote` remote 管理 | 骨架 | Remotes 页签可列出与新增，删除与 URL 修改只有命令封装 |
| `git.inline` 行内修改标记 | 声明 | 无编辑器行内 gutter |
| `git.commit.template` 提交模板 | 声明 | 交给系统 git 配置 |
| `git.commit.gpg` GPG 签名 | 声明 | 交给系统 git 配置 |
| `git.auth.ssh_https` SSH / HTTPS 认证 | 声明 | 交给系统 git 凭据助手 |

## 九、终端与工具集成（15 条：实现 2 / 骨架 2 / 声明 11）

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| `tool.run` 运行程序 | 实现 | F5 / Run Project / Compile & Run |
| `tool.terminal` 内置终端 | 骨架 | `OutputPanel` 的终端通道 + `TerminalLaunch.cpp`（49 行）启动外部终端；**不是**真正的 PTY 终端。原文即标 `known-issue` |
| `tool.launch` 调试程序（插件） | 骨架 | 交给 `DebuggerController`，受第五节限制 |
| `tool.palette` 命令面板 | 实现 | `CommandPalette`：Ctrl+Shift+P，子序列模糊打分（连续命中 +8、词首 +6/+12、长度惩罚） |
| `tool.external` 外部工具配置 | 声明 | 无 |
| `tool.shell` Shell 集成 | 声明 | 无提示符解析、无命令历史同步 |
| `tool.pkg.vcpkg` vcpkg | 声明 | 全仓库无 `vcpkg` 引用 |
| `tool.pkg.conan` conan | 声明 | 无 `conan` 引用 |
| `tool.pkg.hunter` Hunter | 声明 | 无引用 |
| `tool.pkg.system` apt / brew / winget | 声明 | 无引用 |
| `tool.wsl` WSL 集成 | 声明 | `wsl` 纯清单插件 |
| `tool.serial` 串口终端 | 声明 | `serial-terminal` 纯清单插件 |
| `tool.log` 日志查看器 | 声明 | `log-viewer` 纯清单插件 |
| `tool.hex` Hex 查看器 | 声明 | `hex-viewer` 纯清单插件 |
| `tool.media` 图像 / 资源预览 | 声明 | `media-preview` 纯清单插件 |

## 十、测试（15 条：实现 14 / 骨架 1 / 声明 0）

本轮从零补齐：新库 `LightCppTesting`（探测框架 → 列举测试 → 构造命令行 → 解析输出 → 读覆盖率）加一个 `TestExplorerPanel`（Discover / Run All / Run Selected / Rerun Failed / Stop / Coverage）。四个框架的**输出格式**都按真实样本写解析器并有单测，这是本域最容易退化的部分——测试跑得起来但结果读错，比跑不起来更糟。

与调试器同样的分工：库只碰文本，`QProcess` 与超时留在面板里，所以每个解析器都能在没有可执行文件的环境下被测到。

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| `test.unit.integration` 单元测试集成 | 实现 | `TestExplorerPanel` + `LightCppTesting` 库；框架由 CMake 目标反查 |
| `test.explorer` 测试列表视图 | 实现 | Discover 按钮 + 按 suite 分组的树 |
| `test.run.single` 运行单个测试 | 实现 | Run Selected；gtest `--gtest_filter`、Catch2 位置参数、Boost `--run_test`、CTest `-R` |
| `test.run.all` 运行全部测试 | 实现 | Run All；`planRunAll` 按框架给出完整命令行 |
| `test.failure.jump` 测试失败跳转 | 实现 | `failureActivated` → `jumpToLocation`，失败行直接跳到编辑器 |
| `test.output.parse` 测试输出解析 | 实现 | 四种框架的 stdout 解析器（含 skipped 计数、Boost 的 `file(line): error:` 形式） |
| `test.timeout` 测试超时控制 | 实现 | 每框架默认超时（`defaultTimeoutMs`）+ 面板可调，超时记为失败并终止进程 |
| `test.log` 测试日志 | 实现 | 面板内日志框 + `logLine` 转发到输出面板 |
| `test.rerun` 测试重跑 | 实现 | Rerun Failed；`rerunFailedArguments` 用上一次失败的用例名重组过滤条件 |
| `test.coverage.report` 覆盖率统计 | 实现 | 读 `coverage.info` 或 gcov 输出，摘要 + 低于 80% 的文件清单 |
| `test.coverage.gcov_lcov` gcov / lcov 集成 | 骨架 | `parseLcov` / `parseGcov` / `lcovCaptureArguments` / `genhtmlArguments` 齐全，但 IDE 只读取既有报告，不自己发起 `lcov --capture` |
| `test.fw.gtest` GoogleTest 适配 | 实现 | `[       OK ]` / `[  FAILED  ]` / `--gtest_list_tests` / `--gtest_filter` |
| `test.fw.catch2` Catch2 适配 | 实现 | `test cases: N | P passed | F failed | S skipped` / `--list-tests` / 位置参数选测试 |
| `test.fw.boost` Boost.Test 适配 | 实现 | `file.cpp(12): error: in "suite/case":` / `*** No errors detected` / `--run_test` / `--list_content` |
| `test.fw.ctest` CTest 适配 | 实现 | `N/M Test #K: Name ... Passed` / `ctest -N` / `-R '^Name$'` / `--output-on-failure` |

> 现在这个面板也能跑 LightCpp 自己的 781 个 CTest 用例：打开 `CMakeLists.txt`、build 目录填 `/tmp/lc_base_build`，Discover 之后框架会自动认成 CTest。

## 十一、文档与帮助（11 条：实现 7 / 骨架 2 / 声明 2）

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| `doc.shortcuts` 快捷键帮助 | 实现 | 帮助菜单的快捷键列表 |
| `doc.inline` 代码提示文档 | 骨架 | 悬停注释 |
| `doc.api` API 文档浏览 | 骨架 | `Docs::search` 统一索引关键字/标准/库/插件；未接浏览面板 |
| `doc.doxygen` Doxygen 注释解析 | 实现 | `parseDoxygenAbove` + `doxygenToHtml`：\brief / \param / \return / \note / 代码块 |
| `doc.quickhelp` 快速帮助 | 骨架 | `Workbench/Docs::quickHelp` + 86 个关键字一句话说明 + `isKnownKeyword`，未接面板 |
| `doc.stdlib` C/C++ 标准文档 | 实现 | `Docs::cppreferenceUrl` 映射 60+ 标准库名到 cppreference 页面，未知名走站内搜索；`standardDocuments` 列草案 |
| `doc.thirdparty` 第三方库文档 | 实现 | `Docs::libraryDocuments`（Qt 6 / Boost / fmt / nlohmann-json / GoogleTest / CMake / clangd） |
| `doc.plugin` 插件文档 | 实现 | `Docs::pluginDocuments` 由插件 id 生成锚点链接 |
| `doc.preview.markdown` Markdown 预览 | 实现 | `MarkdownPreviewDialog` + `renderMarkdown`（标题、列表、引用、代码块、表格、链接） |
| `doc.preview.rst` reStructuredText 预览 | 实现 | `renderRestructuredText`：段标题、强调、列表、字面块 |
| `doc.preview.browser` 内置浏览器 | 声明 | 未链接 QtWebEngine，无内置浏览器 |

## 十二、Qt 生态（7 条：实现 3 / 骨架 4 / 声明 0）

本轮新增 `Workbench/QtProjectFiles.cpp`：`.pro` / `.pri` 的解析（含 `$$PWD` 展开与 `include()` 递归）、`.ui` 的 widget 树与 `<connections>` 还原、`.qrc` 的 prefix/alias 还原、QML 关键字表。**没有的是渲染端**——这台机器上的 Qt 6.8.3 不带 WebEngine，`.ui` 与 QML 的所见即所得预览需要自绘或换 Qt 构建，那是独立工作量。

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| `qt.project` Qt 项目识别与 `.pro` / `.pri` 解析 | 骨架 | `parseQmakeProject` / `parseQmakeText` 解析 `+= -= *= =`、`$$PWD`、`include()`、续行、注释，有单测；IDE 主流程未接 `.pro` 打开 |
| `qt.qmake.build` 基于 qmake 构建 | 骨架 | `qmakeArguments` 生成器可用，无 IDE 侧 qmake 构建入口 |
| `qt.cmake.integration` Qt CMake 目标识别与构建 | 声明 | `qt-cmake` 是纯清单插件 |
| `lang.qml.highlight` QML 语法高亮 | 骨架 | `qmlKeywords` / `qmlBuiltinTypes` 词表已就位；`qmlObjectTree` 可还原对象树与 id，未注册成高亮器 |
| `lang.qml.diagnostics` QML 诊断与补全 | 声明 | 无 QML 语言服务 |
| `lang.ui.preview` `.ui` 文件预览 | 实现 | `Workbench/Qt::uiPreviewHtml` 按 geometry 绝对定位渲染，无 geometry 的 widget 退化为带标签的行；`<connections>` 一并列出 |
| `lang.ui.resource` `.qrc` 资源预览 | 实现 | `qrcPreviewHtml` 按 prefix 分组，标出 `:/prefix/alias` 与磁盘上缺失的条目 |

## 复现方法

```bash
# 1. 哪些插件有原生实现、哪些只有清单
lightcpp-pm host                     # 表格：已激活 / 缺少实现库 / 纯清单
lightcpp-pm host --require-native    # 严格模式：缺原生库即 exit 1

# 2. 每个功能归属到哪个插件
lightcpp-pm coverage                 # 按功能集统计
lightcpp-pm info <plugin-id>         # 单个插件的贡献点与能力集

# 3. 构建与测试（本机实测）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_PREFIX_PATH=/opt/Qt/6.8.3/gcc_64 -DLLVM_ROOT=/usr/lib/llvm-18
cmake --build build -j"$(nproc)"     # 0 error
ctest --test-dir build -j"$(nproc)"  # 781/781 passed（6 个 GBK 用例因 locale 跳过）

# 4. 代码侧证据（下列文件都在仓库里，可直接 wc -l）
wc -l src/LightCppApp/Source/Debug/GdbMi.cpp                 # MI 文法
wc -l src/LightCppApp/Source/Debug/DebugViews.cpp            # 三后端视图解析
wc -l src/LightCppApp/Source/Debug/DebugBreakpoints.cpp      # 八类断点
wc -l src/LightCppApp/Source/DebugViewsPanel.cpp             # 8 页签面板
wc -l src/LightCppTesting/Source/*.cpp                       # 测试发现 / 输出 / 计划 / 覆盖率
wc -l src/LightCppWorkbench/Source/*.cpp                     # 分析 / Git / markup / Qt 文件

# 5. 仍然没有的（这几条 grep 应当为空，若不为空说明本表过时了）
grep -rn "callHierarchy\|typeHierarchy" src/LightCppApp/Source/LanguageServerClient.cpp
grep -rn "QtWebEngine\|QWebEngineView" src
grep -rn "natvis" src/LightCppApp
grep -rn "vcpkg\|conan" src apps --include=*.cpp
```

## 与其它文档的关系

- [FEATURES.md](FEATURES.md) —— 189 条功能的**清单视图**（归属、平台差异、状态字段），由 `lightcpp-pm docs` 自动生成，回答「这条归谁」。
- [PLAN-TRACEABILITY.md](PLAN-TRACEABILITY.md) —— 规划章节到**落地位置**的映射，回答「规划里每一节有没有人接」。
- 本文 —— 回答「接了之后，代码到底跑不跑得起来」。

三份文档刻意分工，任何一份都不能单独当作「已完成证明」。

## 到「lightcpp.1.md 全部完成」还差什么

`0.2.0` 的定位是「插件生态内核 + 可用的 C/C++ IDE」。当前分支把调试器（含符号设置页）、测试、静态分析、Git、Markdown、**语义高亮与层级导航**、**包管理器**、**二进制/日志/图像查看器**几块从「声明」推到了「能跑」，剩下的差距集中在两处：

1. **Qt 渲染端**：`.ui` / `.qrc` 已有 HTML 预览（`Workbench/QtPreview`），QML 有对象树结构视图但没有渲染器——那需要 QML 引擎与场景图，是这个仓库刻意不链接的东西。真正的所见即所得（拖动 widget 改 geometry）也还没有。
2. **远程与终端**：远程构建（distcc/icecc）、真实 PTY 终端、WSL 与串口集成仍是清单。终端这一块在 §一 就标着「目前有问题」，本轮没有改动。

调试器只剩一条边角：natvis 的**搜索顺序**（先私有后公共、同名后者覆盖）尚未建模，`.natvis` 现在按列表逐个加载。除此之外的符号层能力——pretty-printer、STL / 智能指针展开、枚举显示、动态类型、源码映射、符号路径、`reload /f`——都已经有界面、有落盘、有断言，并在会话开始时下发给后端。

按现有代码量（`src/` + `apps/` 约 5.9 万行）估算，以上合计约为现有仓库的 0.4 倍工作量。

## 本轮改了什么（增量记录）

| 位置 | 内容 | 用例数 |
| --- | --- | --- |
| `src/LightCppApp/{Include,Source}/Debug/` | `GdbMi` / `DebugViews` / `DebugBreakpoints` / `DebugViewsModel` | 59 |
| `src/LightCppApp/Source/DebugViewsPanel.cpp` | 8 页签调试面板 + 断点属性对话框 | 14 |
| `src/LightCppTesting/` | 框架探测、四种输出解析、运行计划、覆盖率 | 33 |
| `src/LightCppApp/Source/TestExplorerPanel.cpp` | 测试浏览器（含超时与重跑失败） | — |
| `src/LightCppWorkbench/` | 分析报告 / Git 模型 / markup 渲染 / Qt 工程文件 | 42 |
| `src/LightCppApp/Source/AnalysisPanel.cpp` | 三工具分析面板，按类别分组 | — |
| `src/LightCppApp/Source/GitBranchDialog.cpp` | 分支 / 标签 / 远程 / stash + 冲突横幅 | — |
| `src/LightCppApp/Source/CommandPalette.cpp` | Ctrl+Shift+P 模糊搜索 | — |
| `src/LightCppApp/Source/MarkdownPreviewDialog.cpp` | Markdown / RST 预览 | — |
| `src/LightCppWorkbench/{Include,Source}/LspModel` | 语义令牌解码、调用/类型层级、类视图、调用图、workspace edit | 26 |
| `src/LightCppWorkbench/{Include,Source}/EditorTools` | include 排序、行 diff | 14 |
| `src/LightCppWorkbench/{Include,Source}/DebugSymbols` | 三方言符号设置命令生成 | 12 |
| `src/LightCppWorkbench/{Include,Source}/GitExtra` | blame 分块、三方冲突、提交模板、子模块、worktree | 24 |
| `src/LightCppWorkbench/{Include,Source}/PackageManager` | vcpkg / Conan / Hunter / apt / brew / winget | 12 |
| `src/LightCppWorkbench/{Include,Source}/Viewers` | hex、日志分级与过滤、二进制嗅探 | 16 |
| `src/LightCppWorkbench/{Include,Source}/BuildOptions` | 编译命令解析、交叉编译 toolchain、ccache、target 列表 | 14 |
| `src/LightCppWorkbench/{Include,Source}/QtPreview` | `.ui` / `.qrc` / QML 预览 | 4 |
| `src/LightCppWorkbench/{Include,Source}/DocsCatalog` | 关键字、标准草案、库文档、搜索 | 6 |
| `src/LightCppApp/Source/LanguageServerClient.cpp` | `requestRaw` 通用通道 + 语义令牌能力声明 | — |
| `src/LightCppApp/Source/MainWindow.cpp` | Sort Includes、Call/Type Hierarchy、Git 侧栏、Blame、冲突解决、包管理器、查看器 | 9 |
| `src/LightCppTesting/{TestModel,TestRunner,CoverageReport}.hpp` | 补回分支上缺失、导致无法编译的三个头 | — |
| `src/LightCppSettings/` | `Preferences::debugger` + `"debugger"` 块的读写（含孤立 from 丢弃） | 3 |
| `MainWindow.cpp` Debug ▸ Symbol Settings | 映射树 + 路径列表 + `Debug::describe()` 摘要；会话 `started` 时下发 | — |

单元测试从 781 增至 **885**（本轮新增 104 个），`100% tests passed`。
