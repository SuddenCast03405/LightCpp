# LightCpp 项目长期约定

## 项目与铁律
LightC++ IDE；显示名 `LightC++`，代码标识 `LightCpp`，二进制 `lightcpp`。规范见 `要求.md`
（C++20、Qt 6.5+、目标态 QScintilla+ADS、CMake 3.24+CPM、spdlog、GTest/gcov）。
用户自称"用户"，称我为"AI/智能体"。**每次代码改动都要 git commit。**
- **纯逻辑/数据层一律单独成静态库 + GTest**（`LightCppCore` / `LightCppProject` ）。这些逻辑出错是静默的，
  必须在无显示环境下可测。
- CMake 目标统一 `LightCpp` 前缀；严格警告放 `LightCppWarnings` INTERFACE 目标，只链自有目标。
- C++20 基线，**不用 std::expected 等 C++23 特性**；第三方走 CPM.cmake（已本地化，可离线构建）。
- 纯逻辑层用 std::string/std::vector/std::optional；仅 Qt 交互层用 QString/QByteArray。
- 文件 I/O、解析放后台线程，UI 用 queued 信号槽更新。
- **改正则必须断言**：`QSyntaxHighlighter` 里 invalid 的 QRegularExpression 被**静默跳过**（只往 stderr 打一行），
  界面毫无异常——历史上数字高亮因此哑掉很久。用 `QRegularExpression::isValid()` 断言一次。

## 本机环境与工具链
- MSYS2 UCRT64 `E:\msys64\ucrt64\bin`（gcc 16.2 / cmake 4.3.3 / ninja 1.13）；Qt6 已装于此，无 ICU。
  另有 `clang-format.exe`(22.1.8) / `clangd.exe` / `git.exe` / `mingw32-make.exe`（**无 `make.exe`**）。
- Git：`D:\Program Files\Git For Windows\bin\git.exe`（2.55.0.windows.5，备选 msys64 版）；仓库身份 lenovo / lenovo@local。
- 跑 Qt 程序需 `E:\msys64\ucrt64\bin` 在 PATH；MinGW 测试可执行文件加 `-static-libgcc -static-libstdc++`。
- 残留 `dist/lightcpp.exe` 进程会让 CMake post-build 拷贝报 "Permission denied" 使 build exit=1（链接其实已成功）
  ——先 `Stop-Process` 再构建。

## 本机操作坑（必读）
- **构建用 PowerShell**：Bash 环境 PATH 坏（`ls`/`head`/`wc` not found）。先
  `$env:PATH="E:\msys64\ucrt64\bin;"+$env:PATH`。`build_msys2.ps1 [-Test] [-Clean]` 内部已处理。
- **PowerShell 工具不回显 stdout/stderr**：`$out = & ... 2>&1; $out | Out-File -Encoding utf8 file` 再 Read。
  `Out-File` 只挂到最后一条语句，多行输出要显式收集进数组。
- **禁止调用 `cmd.exe`**（`.bat` 里的 `if errorlevel` 也被拦）；`reg.exe` 在黑名单，一跑整条命令被拒。
- **`Remove-Item` 是静默无操作**！删目录用 `[System.IO.Directory]::Delete($p, $true)`，否则 `Test-Path` 仍 True 而 exit 0。
- 命令失败可能完全无输出；可执行文件不存在时 `$LASTEXITCODE` 为空——用 `Test-Path` 判断。
- git 提交信息含换行/引号时不要 `-m`：`[System.IO.File]::WriteAllText(path, msg, UTF8Encoding($false))` + `git commit -F`。
- **同一文件的多次编辑必须串行**：并行 Edit 是读-改-写，后写覆盖先写；症状 = 编译报"未声明"而源码里明明写了。
  不同文件之间并行没问题。
- **无头 UI 冒烟测试**：配方见 skill `lightcpp-build-verify`。
  **`MainWindow` 构造时会 `onNewFile()` 开一个 Untitled-1 标签**，"找那个编辑器"不能取
  `findChildren<CodeEditor*>().first()`，必须按文件路径匹配。

## 静态库模块
- `LightCppCore`（刻意无 Qt）：`Core::Text` UTF-8 解码/校验/读文件 + 自带 `Result<T,E>`（避开 C++23 expected）；
  `Core::Fold` 代码折叠区域计算（见下）。
- `LightCppProject`（仅 Qt6::Core，无 widgets，别名 `LightCpp::Project`）：`BuildConfiguration` / `CMakeProject` /
  `DevCppProject` / `GitStatus`。
- `LightCppText`（Qt6::Core）：`detectEncoding()` = BOM → NUL 结构 → UTF-8 校验 → **本地 ANSI fallback**
  （zh-CN→GBK、ja-JP→Shift-JIS、ko-KR→EUC-KR、en-US→Windows-1252），不再走 legacy 评分；
  `convertToUtf8()` Unicode 族走 QStringConverter、遗留编码走 Win32 MultiByteToWideChar；
  另有 `readDetectedTextFile()` / `readTextFileWithEncoding()` / `localAnsiEncoding()` / `localAnsiDisplayName()`。
  UTF-16/32 解码后须手工拒绝未配对代理（Qt decoder 太宽松）。
- `lightcpp-enc` CLI：检测编码 / `--out` 转 UTF-8 / `--list`。

## GUI 布局（LightCppApp，Qt6 Widgets，原生外观，不设 QPalette/StyleSheet）
- **嵌套 QSplitter，无 QDockWidget**：外层垂直 = [ 水平(Explorer | 编辑标签页) , 底部 Output ]，
  均 `setChildrenCollapsible(false)`；View 菜单用自建 checkable QAction（Explorer Ctrl+B / Output Ctrl+J）。
  目的：面板不能被拖成独立浮动窗体。
- `CodeEditor`：QPlainTextEdit + 行号栏 + 当前行高亮 + `CppSyntaxHighlighter`，配色用 `palette()` 动态适配。
  **制表宽 = 4 个等宽字符**：`setTabStopDistance(QFontMetricsF(font).horizontalAdvance(' ') * 4.0)`；
  Qt 默认 80px≈10 字符，而状态栏列号换算 `col += tabs*3` 一直假设 tab=4，现在两者一致。
  行号绘制：drawText 矩形高度必须 ≥ `fontMetrics().height()`，传 0 什么也不画。
- `FileExplorer` / `OutputPanel` 是 QWidget 子类，各带一行加粗小标题（EXPLORER / OUTPUT）。
- 打开文件走 `readDetectedTextFile()`；状态栏显示路径/语言/编码/光标位置；编码下拉框在右下角
  （UTF-8 与 ANSI(<本地编码>) 排前两位），切换后用 `readTextFileWithEncoding()` 强制重读。
- windeployqt6 部署 Qt DLL 到 `dist/`。**QScintilla/ADS 未安装**（`pacman -S mingw-w64-ucrt-x86_64-qscintilla`）。

### OutputPanel 通道
`ChannelServer=0` Clangd Log / `ChannelBuild=1` Build Log（默认）/ `ChannelProgram=2` Program Output
（stdout/stderr + `--- Running:`/`exited` 横幅）/ **`ChannelGit=3`** / **`ChannelProblems=4`**。缓冲互不混淆。
API `appendServerLog()` / `appendBuildOutput()` / `appendProgramOutput()` / `appendGitOutput()` / `setProblems()` /
`showChannel()` / `clearOutput()`——**`appendOutput()` 已删除**，新增输出按语义选通道。
combo 信号 `currentIndexChanged(int)` 须 lambda 转 Channel（int 不隐式转非作用域枚举）。
Problems 下拉项带实时计数；空时显示不可选的 "No problems detected."。

### Problems
按文件分组 QTreeWidget（父=文件名，子=`severity: msg [code]` + `Ln x, Col y`）；行 data UserRole=路径 / +1=行(0基) /
+2=列(0基)；单击 emit `problemActivated(path,line,col)`。`onProblemActivated()` 先按 `lintPath()` 匹配 tab
（未保存缓冲是虚拟路径，不能从磁盘重开）再 `gotoLocation()`。`diagnosticsByFile_`(键=lintPath) →
`rebuildProblems()` 排序展平（QHash 无序，不排序列表会跳）；关 tab / 保存换路径 / 关 clangd 时清理。

## 编辑器交互（CodeEditor）
- **括号自动配对**：`autoCloseBracket(typed)` 插两半、光标居中；已有右半边时**跳过**；有选区时**包裹**（保留选区）。
  `backspaceEmptyPair()` 仅删真正的空对。**坑**：`movePosition(PreviousCharacter, KeepAnchor, 2)` 在文档起点只能左移
  1 字符 → 剩半个括号；正确做法 `setPosition(pos-1)` + `setPosition(pos+1, KeepAnchor)` 再 `removeSelectedText()`。
  选区包裹不能用 `selectedText()` 重插（段落符会变成 `U+2029`，多行选区写坏），要在 selectionStart/End 分别插。
  引号特例：前一字符是字母/数字/下划线且后面不是同种引号时不配对（`1'000`、`foo"`），**但有选区时一律包裹**
  （守卫须带 `!cursor.hasSelection()`）。`"` 配对后仍要 `requestCompletionAtCursor("\"")`。
  在 `keyPressEvent` 最前面拦截（早于补全逻辑）；`plainTextKey` 排除 Ctrl/Alt/Meta。
  `characterAfter()` 判结尾要用 `position() >= characterCount() - 1`（末尾有段落分隔符 `U+2029`）。
- **跳转定义**：Ctrl+左键或 F12；信号 `definitionRequested(line,col)`；LSP 侧 `requestDefinition()` +
  `LspLocation{filePath,line,column; isValid()}`（空路径 = 没找到），响应兼容 Location / Location[] / LocationLink[]
  （后者取 targetSelectionRange，否则光标落在返回类型上）。MainWindow 走 `jumpToLocation()`——**与 Problems 共用**；
  `latestDefinitionId_` 守卫慢响应。
- **Qt6 里 `QMouseEvent::pos()` 已弃用**，用 `event->position().toPoint()`（`-Werror` 会拦）。
- **代码折叠**（`LightCppCore::Fold`）：逐行词法分析器在 `Normal/LineComment/BlockComment/String/Char/RawString`
  状态间切换找 `{}` 对，忽略注释/字面量里的花括号；`#if/#ifdef/#ifndef…#endif` 与 `#pragma region`/`#region`
  用栈追踪；指令只在 `Normal` 状态识别；只报告跨 ≥2 行的区域。
  - **折叠锚点 = `{regionIndex, headerText}`**（"第 N 个可折叠区域" + 其首行文本）：上方编辑会移动所有行号但序数不变；
    首行文本不同则丢弃折叠，不吸附到相似内容上。
  - Qt 隐藏块：`block.setVisible(false)` + `block.setLineCount(0)` + `document()->markContentsDirty(pos,len)`，
    再 `viewport()->update()` 与 `lineNumberArea_->update()`。
  - 行号栏左侧 12px 折叠列画 `+`/`−` 方块；点击转发 `handleFoldMarkerClick()`；`gotoLocation()` 先 `unfoldLine()`。

## LSP（over stdio，clangd）
- `kClangdPath` = `E:/msys64/ucrt64/bin/clangd.exe`（集中于 `ToolchainPaths.hpp`）；**默认开启**。
- **握手 response 不能写死 `id == 1`**：`stop()`+`start()` 后计数器已走到 3，写死 1 会让新进程永远收不到
  `initialized` → 不发 didOpen → 诊断再也不出现（"关掉再打开就废了"）。用成员 `initializeRequestId_`。
- stderr 用 `setProcessChannelMode(SeparateChannels)` 按行转发，连同 `window/logMessage`/`showMessage` 进 Clangd Log。
- 300ms 防抖发 didChange（全文同步）→ `publishDiagnostics` 画波浪线 + 行号区红/黄点。诊断输出只在
  errors/warnings 计数变化时打印（防刷屏）。
- **补全/跳转前必须先 changeDocument 推最新全文**（lint 防抖滞后会让 clangd 位置错位返回空——"只有 include
  补全有结果"的根因）。
- 补全触发字符 `. : > < "`；标识符输入与 Backspace 在 popup 开/关两分支都重启 120ms 空闲定时器（每次编辑都请求，
  这是奇偶交替 bug 的修复关键）。**过期响应守卫** `latestCompletionId_` 只认最新请求，旧响应（常为空）一律忽略，
  否则会关掉新弹窗。`pendingCompletions_` 用 `QPointer<CodeEditor>` 防悬空。
- `stop()` 顺序坑：先 `process_ = nullptr` 再 `waitForFinished` 会让 finished lambda 访问 nullptr；
  修复：stop 开头 `process_->disconnect(this)`，finished lambda 用 `sender()`。
- **零宽诊断归一化** `normalizedDiagnostic()`：clangd 对 `#include` 缺参数报零宽 range 且落在下一空行第 0 列，
  归一化归属到上一行行尾；波浪线/红点/悬停三处共用；零宽画波浪线取行尾最后一个可见字符。
- **诊断悬停 tooltip**：`QTextCharFormat::setToolTip` 对 extra selection 无效（覆盖层不参与文本布局）；
  改为重写 `event()` 拦 `QEvent::ToolTip`，`cursorForPosition` 与 `diagnostics_` 比较，零宽接受整行，重叠取最严重。
- 未保存文档用虚拟路径 `QDir::tempPath()/lightcpp-untitled/Untitled-N.cpp`；保存后 didClose 旧路径 + didOpen 真实路径。
  **该目录必须真实存在**：clangd 把 VFS 工作目录设成文档父目录，否则每次 didOpen 报 `VFS: failed to set CWD`。
  `onNewFile()` 里 `QDir().mkpath()` 修复；本机 `QDir::tempPath()` = `E:/msys64/tmp`。

## 单文件编译 / 运行（Tools 菜单）
- Compile Current File(F7) / Compile && Run(F5) / Run(Ctrl+F5) / Stop(Shift+F5)；
  **Toolchain 子菜单 GCC / Clang / MSVC（cl.exe）三选一**，默认 GCC；切换会重启语言服务器。
- **build 与 run 共用 `resolveBuildTarget(editor)`**，exe 恒在源码同目录同名；Untitled 源码写
  `%TEMP%/lightcpp-compile/<tabstem>.cpp`。改路径规则只能改这个函数。
- 编译标志来自 `flagsFor(buildConfiguration(), family)`，子进程 PATH 前置工具链 bin。
- Compile & Run 在 `finished` 里 rc==0 才 `startProgram()`（`runAfterBuild_`）。
- Run 在 exe 不存在（弹 "Not Compiled"）或 exe 比源码旧（弹 "Source Changed"）时 QMessageBox 询问是否重编。
- **输出分流**：编译器输出 → Build Log；程序输出 → Program Output。`compiler_`/`runner_` 懒创建复用，MergedChannels。
- 被运行程序无终端：**必须在 `runner_->started()` 里 `closeWriteChannel()`** 给 stdin EOF，
  否则 scanf/getline 永久阻塞（`start()` 之后立刻 close 无效，那时通道未建立）。
- `updateRunActionStates()`：编译中禁用 Compile/Compile&Run/Run，运行中禁用 Run/Compile&Run、启用 Stop；
  `closeEvent` 里同时 kill 编译器与运行中程序。

## 构建配置 / 项目 / 格式化 / Git
- `BuildConfiguration` 两取值，`toString`/`buildConfigurationFromName` 拼写与 `CMAKE_BUILD_TYPE` **完全一致**：
  `Debug` / `Release`。`flagsFor(config, family)`：GNU Debug `-g`、Release `-O2 -DNDEBUG`；
  MSVC Debug `/Od`、Release `/O2 /DNDEBUG`——**MSVC Debug 刻意不加 `/Zi`**（多 MB `.pdb` 会掉在用户源码旁，
  这条路径是"编译并运行"而非"附加调试器"）。工具栏最右侧 `QComboBox` 选 Debug/Release，单文件与项目构建共用。
- `CMakeProject`：从当前文件向上找 `CMakeLists.txt`（最大 6 层，遇 `.git` 停止）；构建树 `<source>/build/<config>`
  （每种配置一个缓存）；先 `-S -B -G Ninja -DCMAKE_BUILD_TYPE=<cfg>` 再 `--build <dir> --config <cfg>`；
  `builtExecutables()` 扫描**必须跳过 `CMakeFiles/`**（里面全是 `cmTC_*.exe` 探针）；`newestExecutable()` 按 mtime 选。
- `DevCppProject`：两种方言（`[UnitN]` 节 / `[Project]` 里的 `UnitN=` 键）；列表分隔符 `_@@_`；
  单元索引从 0 开始**合法**（只有负数无效）；`Compile=0`/`Link=0` 的单元丢弃；头文件不传给编译器；
  **不强制 `-std=`**（用项目自己的 `CppCompiler=-std=c++11`）；所有单元在**单次 `g++` 调用**里编译
  （不生成 makefile ⇒ 源码树不留 `.o`）；可选 `windres` 图标步骤。构建队列走 `QProcess::MergedChannels`。
- **格式化** `CodeFormatter` 走 `clang-format.exe`。**实测结论（反直觉，别猜）**：`-style=Microsoft` 会**完全忽略**
  项目里的 `.clang-format`；`-style=file` 没有配置文件时**回退到 LLVM 而不是 Microsoft**。唯一同时满足两者的写法：
  **`-style=file --fallback-style=Microsoft`**。另加 `--assume-filename=<当前文件名>`；15 秒单次超时；
  请求被取代时断开 `finished`。`Edit ▸ Format Document`（`Shift+Alt+F` / `Ctrl+Shift+I`）；
  `applyFormattedText()` 保留光标行列。
- **Git** `GitClient` **一次只跑一个 git 命令**（git 会对索引加锁）；`refreshStatus()` / `run(args,title)` /
  `setWorkingDirectory()` / `isBusy()`；信号 `statusReady(GitStatusSummary)` / `commandFinished(title,text,exitCode)`。
  **`startNext()` 必须先判断进程是否已在跑再启动**：`run()` 每次都调它，不判断就会覆盖 `current_`
  并被 QProcess 拒绝第二次 `start()` —— 命令静默消失、正在跑的那条的输出被挂到新命令名下
  （症状：打开别的目录的文件后，状态栏一直报旧仓库）。排空靠 `finished`/`errorOccurred` 回调再次调 `startNext()`。
  子进程环境设 `LC_ALL=C`（解析器依赖 git 英文措辞）与 `GIT_TERMINAL_PROMPT=0`；**`git status` 失败只发空
  summary，不报错**（"不是仓库"就是靠这个判定的，所以它绝不能打成错误横幅）。`GitStatus` 解析
  `git status --porcelain=v1 -b -uall`；状态字母白名单 `" MADRCUT?!"`
  （否则 `"not porcelain at all"` 这类垃圾行会被当成条目）；ahead/behind 靠英文关键词，**无法识别的措辞让计数保持 0**
  （git 未承诺这些词稳定）；反引号路径做 C 反转义（八进制转义保留，路径只用于显示）。
  `MainWindow`：Git 菜单（刷新状态/暂存全部/取消暂存全部/提交…/显示日志/差异工作树/差异暂存/初始化仓库）
  + 状态栏 `statusGit_` 标签（objectName=`statusGit`，便于无头测试定位）；
  `syncGitWorkingDirectory()` 在构造、切 tab、打开项目、关闭项目时调用。
  冒烟测试 `_git_smoke.cpp`（43 项）在临时目录里跑完整链路：非仓库识别 → init → 队列 → stage → commit → log → diff。

## MSVC 支持（`LightCpp::App::Msvc`）
- `MsvcToolchain.hpp/.cpp`：`detect()` 定位工具链、`clangdHeaders()` 产出 clangd 配置。cl.exe **不能裸跑**，
  必须手工给 INCLUDE / LIB / PATH；**刻意不用 `vcvarsall.bat`**（要 cmd.exe + reg.exe，本机沙箱把 reg.exe 列黑名单，且慢）。
- 检测顺序：`vswhere.exe -all -prerelease -products * -format json`（**必须带 `-prerelease`** 才看得到 18.x 预览版；
  `-property A -property B` 只回最后一个，不可用）→ `HKLM\...\VisualStudio\SxS\VS7`（VS2002–2019）→ VS6 的
  `...\VisualStudio\6.0\Setup\VC\ProductDir` + `VC98`。按分数（VS 主版本×1000）取最新。
- 三种布局：VS2017+ `VC\Tools\MSVC\<ver>\bin\Hostx64\x64`；VS2002–2015 `VC\bin\amd64` + `VC\lib\amd64`；
  VC6 `VC98\Bin` + `VC98\Lib`（仅 x86）。Windows SDK 读
  `HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots` 的 **`KitsRoot10`**（本机 `D:\Windows Kits\10`，
  **不在** `C:\Program Files (x86)\...`，别猜），取版本最高的 `Include\10.*`，拼 `ucrt/shared/um` 与
  `Lib\<ver>\{ucrt,um}\<arch>`。
- 参数 `/nologo /W4 /EHsc /MT [/TP] /Fe<exe> /Fo<obj> <src>`（**不传 `/std:`**，用编译器默认）：`/MT` 静态 CRT；
  **`/TP`** 让 `.h` 按 C++ 编；**不加 `/Zi`/`/Z7`**（linker 必在 CWD 掉 6MB `.pdb`+3MB `.ilk`）。`/Fe`/`/Fo`
  用**拼接式**（`/Fepath`）而非 `/Fe:path`；`cl <绝对路径源码>` 而 CWD 在别处时 exe/.obj 落 CWD，所以必须显式给。
  **.obj 定向到 `%TEMP%/lightcpp-compile/msvc/`**。
- `Toolchain` 带 `targetTriple`（x64→`x86_64-pc-windows-msvc`，x86→`i686-pc-windows-msvc`）；`clMinor`
  （`14.30.30705` 取 30）只用来告诉 clangd 该用哪个 `_MSC_VER`。**`standardArgument()` 已删除**。
  找不到工具链时菜单项 **disable + 原因写进 statusTip**；编译横幅带
  `[MSVC 19.30 (Visual Studio 17.0, toolset 14.30.30705)]`。
- **本机只有 VS 2022 Professional（17.0 / toolset 14.30.30705 / cl 19.30，SDK 10.0.19041.0）**；没有 18.x 也没有 VC6
  ——VC6/legacy 两条布局路径照文档写，**未实物验证**。

### 让 clangd 跟随所选编译器的头文件
- 症状：选了 MSVC，clangd 里 `<iostream>` 打开的还是 MinGW/libstdc++ 的头，于是给 cl.exe 完全没意见的代码画波浪线。
- **`--query-driver` 这条路走不通**（实测）：没有编译数据库时 clangd 的 fallback 命令 driver 恒为 `clang`
  （默认 triple `x86_64-w64-windows-gnu`），没机会跑 cl.exe；日志明确写 `driver clang expanded to .../clang.exe`
  + `not allowed driver`。
- 唯一可行机制：**编译数据库**。`LanguageServerClient::start()` 调 `writeClangdFlags()` 在 temp 下写
  **`lightcpp-clangd/compile_flags.txt`**，并加 `--compile-commands-dir=<那个目录>`。**绝不写进用户源码树**。
- 内容：`--target=<targetTriple>`、`-fms-compatibility`、`-fms-extensions`、`-fms-compatibility-version=19.<clMinor>`，
  再 4 个 `-isystem <目录>`（= 与 cl.exe 的 INCLUDE 完全同一批）。**不传 `-std:`**：clang 对 msvc target 默认 c++14，
  正好等于 cl.exe 不带 `/std:` 的默认，两边语言级别天然一致。`compile_flags.txt` **一行一个参数**，
  带空格的路径不用（也不能）加引号。GCC/Clang 传空 `ToolchainHeaders` → 不加这个开关，保留 clangd 自带默认。
- **`setToolchainHeaders()` 只在 `start()` 时被读**（clangd 启动就把 include path 定死）。
  `MainWindow::applyToolchainHeaders()` 在切换工具链时 `stop()`+`start()`（配置相同则不重启，故 GCC↔Clang 无动作），
  并在 Clangd Log 记 "Restarting the language server for …"。构造里在 `actSyntaxCheck_->setChecked(true)` **之前**
  调一次。重启后 `serverStarted` 会自动把已打开 tab 全部 didOpen 回来。
- `ToolchainHeaders.hpp`（`target`/`extraFlags`/`includeDirs` + `isEmpty()` + `operator==`）单独成头，
  避免 `MsvcToolchain` 依赖 LSP 客户端。
