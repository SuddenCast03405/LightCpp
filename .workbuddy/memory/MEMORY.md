# LightCpp 项目长期约定

## 项目
LightC++ IDE；显示名 `LightC++`，代码标识 `LightCpp`，二进制 `lightcpp`。
规范见 `要求.md`（C++20、Qt 6.5+、以 QScintilla+ADS 为目标态、CMake 3.24+CPM、spdlog、GTest/gcov）。
用户自称"用户"，称呼我为"AI/智能体"。

## 模块
- `LightCppCore`（无 Qt）：`Core::Text` UTF-8 解码/校验/读文件；自带 `Result<T,E>`。
- `LightCppText`（Qt6::Core）：`detectEncoding()` = BOM → NUL 结构 → UTF-8 校验 → **本地 ANSI fallback**
  （zh-CN→GBK 等）；`convertToUtf8()` Unicode 走 QStringConverter、遗留编码走 Win32 MultiByteToWideChar；
  另有 `readDetectedTextFile()` / `readTextFileWithEncoding()` / `localAnsiEncoding()` / `localAnsiDisplayName()`。
  UTF-16/32 解码后须手工拒绝未配对代理（Qt decoder 太宽松）。
- `lightcpp-enc` CLI：检测编码 / `--out` 转 UTF-8 / `--list`。ctest 67 例全绿。

## GUI（LightCppApp，Qt6 Widgets，原生外观，不设 QPalette/StyleSheet）
- **布局：嵌套 QSplitter，无 QDockWidget**。外层垂直 = [ 水平(Explorer | 编辑标签页) , 底部 Output ]，
  均 `setChildrenCollapsible(false)`；View 菜单用自建 checkable QAction（Explorer Ctrl+B / Output Ctrl+J）。
  目的：面板不能被拖成独立浮动窗体。
- `CodeEditor`：QPlainTextEdit + 行号栏 + 当前行高亮 + `CppSyntaxHighlighter`，配色用 `palette()` 动态适配。
  **制表宽 = 4 个等宽字符**：`setTabStopDistance(QFontMetricsF(font).horizontalAdvance(' ') * 4.0)`（本机 32px）；
  Qt 默认 80px≈10 字符，且状态栏列号换算 `col += tabs*3` 一直假设 tab=4，现在两者一致。
  行号绘制：drawText 矩形高度必须 ≥ `fontMetrics().height()`，传 0 什么也不画。
- `FileExplorer` / `OutputPanel`：QWidget 子类，各带一行加粗小标题（EXPLORER / OUTPUT）。
- **OutputPanel 四通道**：`ChannelServer=0` Clangd Log / `ChannelBuild=1` Build Log（默认）/
  `ChannelProgram=2` Program Output（程序 stdout/stderr + `--- Running:`/`exited` 横幅）/ `ChannelProblems=3`。
  缓冲互不混淆。API `appendServerLog()` / `appendBuildOutput()` / `appendProgramOutput()` /
  `setProblems()` / `showChannel()` / `clearOutput()`——**`appendOutput()` 已删除**。
  combo 信号 `currentIndexChanged(int)` 须 lambda 转 Channel（int 不隐式转非作用域枚举）。
  Problems 下拉项带实时计数；空时显示不可选的 "No problems detected."。
- **Problems**：按文件分组 QTreeWidget（父=文件名，子=`severity: msg [code]` + `Ln x, Col y`）；
  行 data UserRole=路径 / +1=行(0基) / +2=列(0基)；单击 emit `problemActivated(path,line,col)`。
  `onProblemActivated()` 先按 `lintPath()` 匹配 tab（未保存缓冲是虚拟路径，不能从磁盘重开）再 `gotoLocation()`。
  `diagnosticsByFile_`(键=lintPath) → `rebuildProblems()` 排序展平（QHash 无序，不排序列表会跳）；
  关 tab / 保存换路径 / 关 clangd 时清理。
- 打开文件走 `readDetectedTextFile()`；状态栏显示路径/语言/编码/光标位置；编码下拉框在右下角
  （UTF-8 与 ANSI(<本地编码>) 排前两位），切换后用 `readTextFileWithEncoding()` 强制重读。
- windeployqt6 部署 Qt DLL 到 `dist/`。**QScintilla/ADS 未安装**（`pacman -S mingw-w64-ucrt-x86_64-qscintilla`）。

## LSP（over stdio，clangd）
- `kClangdPath` = `E:/msys64/ucrt64/bin/clangd.exe`（硬编码在 `MainWindow.cpp`）；**默认开启**。
- **握手 response 不能写死 `id == 1`**：`stop()` + `start()` 后计数器已走到 3，写死 1 会让新进程
  永远收不到 `initialized` → 不发 didOpen → 诊断再也不出现（"语法检查关掉再打开就废了"就是这个）。
  用成员 `initializeRequestId_` 记住本次 initialize 的 id。
- **clangd 的头文件搜索路径跟随所选工具链**（见 MSVC 节 `clangdHeaders`）：通过
  `temp/lightcpp-clangd/compile_flags.txt` + `--compile-commands-dir` 实现；空配置表示用 clangd 自带默认。
- stderr 用 `setProcessChannelMode(SeparateChannels)` 按行转发，连同 `window/logMessage`/`showMessage` 进 Clangd Log。
- 300ms 防抖发 didChange（全文同步）→ `publishDiagnostics` 画波浪线 + 行号区红/黄点。诊断输出只在
  errors/warnings 计数变化时打印（防刷屏）。
- **补全前必须先 changeDocument 推最新全文**（lint 防抖滞后会让 clangd 位置错位返回空 ——
  "只有 include 补全有结果"的根因）。跳转定义同理。
- 补全触发字符 `. : > < "`；标识符输入与 Backspace 在 popup 开/关两分支都重启 120ms 空闲定时器
  （每次编辑都请求，这是奇偶交替 bug 的修复关键）。**过期响应守卫** `latestCompletionId_` 只认最新请求，
  旧响应（常为空）一律忽略，否则会关掉新弹窗。`pendingCompletions_` 用 `QPointer<CodeEditor>` 防悬空。
- `stop()` 顺序坑：先 `process_ = nullptr` 再 `waitForFinished` 会让 finished lambda 访问 nullptr；
  修复：stop 开头 `process_->disconnect(this)`，finished lambda 用 `sender()`。
- **零宽诊断归一化** `normalizedDiagnostic()`：clangd 对 `#include` 缺参数报零宽 range 且落在下一空行第 0 列，
  归一化归属到上一行行尾；波浪线/红点/悬停三处共用；零宽画波浪线取行尾最后一个可见字符。
- **诊断悬停 tooltip**：`QTextCharFormat::setToolTip` 对 extra selection 无效（覆盖层不参与文本布局）；
  改为重写 `event()` 拦 `QEvent::ToolTip`，`cursorForPosition` 与 `diagnostics_` 比较，零宽接受整行，重叠取最严重。
- 未保存文档用虚拟路径 `QDir::tempPath()/lightcpp-untitled/Untitled-N.cpp` 以便 lint；保存后 didClose 旧路径 +
  didOpen 真实路径。**该目录必须真实存在**：clangd 会把 VFS 工作目录设成文档父目录，否则每次 didOpen 报
  `VFS: failed to set CWD to ...`（不影响功能但刷日志）。`onNewFile()` 里 `QDir().mkpath()` 修复；
  本机 `QDir::tempPath()` = `E:/msys64/tmp`。

## 编辑器交互（CodeEditor）
- **跳转定义**：Ctrl+左键（`mousePressEvent` 判 `左键+ControlModifier`，先 `cursorForPosition()`
  取位置再交给基类，发信号放基类之后）或 F12（用光标位置）。信号 `definitionRequested(line, col)`。
  LSP 侧 `requestDefinition()` + `LspLocation{filePath,line,column; isValid()}`（空路径 = 没找到），
  响应兼容 Location / Location[] / LocationLink[]（后者取 targetSelectionRange，否则光标落在返回类型上）。
  MainWindow 走 `jumpToLocation(path,line,col)`——**与 Problems 列表共用**（先按 `lintPath()` 匹配已开 tab，
  匹配不到才从磁盘打开）；`latestDefinitionId_` 守卫慢响应。
  **Qt6 里 `QMouseEvent::pos()` 已弃用**，用 `event->position().toPoint()`（`-Werror` 会拦）。
- **括号自动配对**：`autoCloseBracket(typed)` 插两半、光标居中；已有右半边时**跳过**；有选区时**包裹**（保留选区）。
  `backspaceEmptyPair()` 仅删真正的空对。**坑**：`movePosition(PreviousCharacter, KeepAnchor, 2)` 在文档起点只能
  左移 1 字符 → 剩半个括号；正确做法 `setPosition(pos-1)` + `setPosition(pos+1, KeepAnchor)` 再 `removeSelectedText()`。
  选区包裹也不能用 `selectedText()` 重插（段落符会变成字符 `U+2029`，多行选区写坏），要在 selectionStart/End 分别插。
  引号特例：前一字符是字母/数字/下划线且后面不是同种引号时**不配对**（`1'000`、`foo"`），
  但**有选区时一律包裹**（守卫须带 `!cursor.hasSelection()`）。`"` 配对后仍要 `requestCompletionAtCursor("\"")`。
  在 `keyPressEvent` 最前面拦截（早于补全逻辑）；`plainTextKey` 排除 Ctrl/Alt/Meta。
  `characterAfter()` 判结尾要用 `position() >= characterCount() - 1`（末尾有段落分隔符 `U+2029`）。

## 单文件编译 / 运行（Tools 菜单）
- Compile Current File(F7) / Compile && Run(F5) / Run(Ctrl+F5) / Stop(Shift+F5)；
  **Toolchain 子菜单 GCC / Clang / MSVC（cl.exe）三选一**，默认 GCC；切换会重启语言服务器。
- **build 与 run 共用 `resolveBuildTarget(editor)`**，exe 恒在源码同目录同名；Untitled 源码写
  `%TEMP%/lightcpp-compile/<tabstem>.cpp`。改路径规则只能改这个函数。
- GCC/Clang args `-Wall -Wextra -g <src> -o <exe>`（无 `-std=`）；子进程 PATH 前置工具链 bin。
- Compile & Run 在 `finished` 里 rc==0 才 `startProgram()`（`runAfterBuild_`）。
- Run 在 exe 不存在（弹 "Not Compiled"）、或 exe 比源码旧（弹 "Source Changed"）时 QMessageBox 询问是否重编。
- **输出分流**：编译器输出 → Build Log；程序输出 → Program Output（`--- Running:`/`exited` 横幅自成一份转录）。
  `compiler_` / `runner_` 懒创建复用，MergedChannels；PATH 前置各自 `toolDirs`。
- 被运行程序无终端：**必须在 `runner_->started()` 里 `closeWriteChannel()`** 给 stdin EOF，
  否则 scanf/getline 永久阻塞（`start()` 之后立刻 close 无效，那时通道未建立）。
- `updateRunActionStates()`：编译中禁用 Compile/Compile&Run/Run，运行中禁用 Run/Compile&Run、启用 Stop；
  `closeEvent` 里同时 kill 编译器与运行中程序。

### MSVC 支持（`LightCpp::App::Msvc`）
- `MsvcToolchain.hpp/.cpp`：`detect()` 定位工具链、`clangdHeaders()` 产出 clangd 配置。
  cl.exe **不能裸跑**，必须手工给 INCLUDE / LIB / PATH；**刻意不用 `vcvarsall.bat`**——它要 cmd.exe + reg.exe
  （本机沙箱把 reg.exe 列入黑名单），而且慢。
- 检测顺序：`vswhere.exe -all -prerelease -products * -format json`（**必须带 `-prerelease` 才看得到 18.x 预览版**；
  `-property A -property B` 只回最后一个，不可用）→ `HKLM\...\VisualStudio\SxS\VS7`（VS2002–2019）
  → VS6 的 `...\VisualStudio\6.0\Setup\VC\ProductDir` + `VC98`。按分数（VS 主版本×1000）取最新。
- 三种布局：VS2017+ `VC\Tools\MSVC\<ver>\bin\Hostx64\x64`；VS2002–2015 `VC\bin\amd64` + `VC\lib\amd64`；
  VC6 `VC98\Bin` + `VC98\Lib`（仅 x86）。
- Windows SDK 位置读 `HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots` 的 **`KitsRoot10`**
  （本机 `D:\Windows Kits\10`，**不在** `C:\Program Files (x86)\...`，别猜）。取版本最高的 `Include\10.*`，
  拼 `ucrt/shared/um` 与 `Lib\<ver>\{ucrt,um}\<arch>`。
- 参数 ` /nologo /W4 /EHsc /MT [/TP] /Fe<exe> /Fo<obj> <src>`（**不传 `/std:`**，用编译器自己的默认）：
  `/MT` 静态 CRT；**`/TP`** 让 `.h` 按 C++ 编；**不加 `/Zi`/`/Z7`**（linker 必在 CWD 掉 6MB `.pdb` + 3MB `.ilk`，
  换来源码目录只剩 `.cpp` + `.exe`）。`/Fe`/`/Fo` 用**拼接式**（`/Fepath`）而非 `/Fe:path`。
  另：`cl <绝对路径源文件>` 而 CWD 在别处时 exe/.obj 落 CWD，所以 `/Fe` `/Fo` 必须显式给。
  **.obj 定向到 `%TEMP%/lightcpp-compile/msvc/`**。
- `Toolchain` 带 `targetTriple`（x64→`x86_64-pc-windows-msvc`，x86→`i686-pc-windows-msvc`）；
  `clMinor`（工具集目录 `14.30.30705` 取 30）只用来告诉 clangd 该用哪个 `_MSC_VER`。**`standardArgument()` 已删除**。
- 找不到工具链时菜单项 **disable + 原因写进 statusTip**；编译横幅带
  `[MSVC 19.30 (Visual Studio 17.0, toolset 14.30.30705)]`（装多个 VS 时只写 cl.exe 说不清）。
- **本机只有 VS 2022 Professional（17.0 / toolset 14.30.30705 / cl 19.30，Windows SDK 10.0.19041.0）**；
  没有 18.x 也没有 VC6 —— VC6/legacy 两条布局路径照文档写，**未实物验证**。

### 让 clangd 跟随所选编译器的头文件
- 症状：选了 MSVC，clangd 里 `<iostream>` 打开的还是 **MinGW/libstdc++** 的头，于是给 cl.exe 完全没意见的
  代码画波浪线。
- **`--query-driver` 这条路走不通**（实测）：没有编译数据库时 clangd 用的 fallback 命令 driver 恒为
  `clang`（默认 triple `x86_64-w64-windows-gnu`），根本没机会去跑 cl.exe；日志明确写
  `driver clang expanded to .../clang.exe` + `not allowed driver`。
- 可行机制只有一个：**编译数据库**。`LanguageServerClient::start()` 调 `writeClangdFlags()` 在 temp 下写
  **`lightcpp-clangd/compile_flags.txt`**（单文件数据库），并加 `--compile-commands-dir=<那个目录>`。
  **绝不写进用户源码树**。
- 内容：`--target=<toolchain.targetTriple>`、`-fms-compatibility`、`-fms-extensions`、
  `-fms-compatibility-version=19.<clMinor>`，再 4 个 `-isystem <目录>`（= 与 cl.exe 的 INCLUDE 完全同一批，
  构造上不可能不一致）。**不传 `-std:`**：clang 对 msvc target 的默认就是 c++14，正好等于 cl.exe 不带 `/std:`
  的默认，两边语言级别天然一致（日志里能看到 `Indexing c++14 standard library`）。
- `compile_flags.txt` **一行一个参数**，所以带空格的路径不用（也不能）加引号。
- GCC/Clang 工具链传空 `ToolchainHeaders` → 不加这个开关，保留 clangd 自带（= MSYS2 GCC）的默认。
- **`setToolchainHeaders()` 只在 `start()` 时被读**（clangd 启动就把 include path 定死）。
  `MainWindow::applyToolchainHeaders()` 在切换工具链时 `stop()` + `start()`（配置相同则不重启，
  故 GCC↔Clang 切换无动作），并在 Clangd Log 记一行 "Restarting the language server for …"。
  构造里在 `actSyntaxCheck_->setChecked(true)` **之前**调一次，clangd 一上来就是对的。重启后 `serverStarted`
  会自动把已打开的 tab 全部 didOpen 回来。
- `ToolchainHeaders.hpp`（`target` / `extraFlags` / `includeDirs` + `isEmpty()` + `operator==`）单独成头，
  是为了不让 `MsvcToolchain` 去依赖 LSP 客户端。

## 工程约定
- CMake 目标统一 `LightCpp` 前缀；严格警告放 `LightCppWarnings` INTERFACE 目标，只链自有目标。
- C++20 基线，**不用 std::expected 等 C++23 特性**；第三方走 CPM.cmake（已本地化，可离线构建）。
- 纯逻辑层用 std::string/std::vector/std::optional；仅 Qt 交互层用 QString/QByteArray。
- 文件 I/O、解析放后台线程，UI 用 queued 信号槽更新。
- **改正则必须断言**：`QSyntaxHighlighter` 里 invalid 的 QRegularExpression 会被**静默跳过**（只往 stderr 打一行），
  界面毫无异常 —— 历史数字高亮因此哑掉很久。用 `QRegularExpression::isValid()` 断言一次。
- **每次代码改动都 git commit 一次**。

## 本机环境与工具链
- MSYS2 UCRT64 `E:\msys64\ucrt64\bin`（gcc 16.2 / cmake 4.3.3 / ninja 1.13）；Qt6 已装于此，无 ICU。
- Git：`D:\Program Files\Git For Windows\bin\git.exe`（备选 msys64 版）；仓库级身份 lenovo / lenovo@local。
- **用 PowerShell 构建**（Bash 环境 PATH 坏，`ls`/`head` not found）：先 `$env:PATH="E:\msys64\ucrt64\bin;"+$env:PATH`。
- **PowerShell 工具坑**：
  - **不回显 stdout/stderr**：`$out = & ... 2>&1; $out | Out-File -Encoding utf8 file` 再 Read。
    注意 `Out-File` 只挂到最后一条语句上，多行输出要显式收集进数组。
  - `cmd *> file` 写成 UTF-16（Read 判定为二进制）。**禁止调用 `cmd.exe`**（连 `.bat` 里 `if errorlevel` 都被拦）；
    `reg.exe` 在程序黑名单里，一跑整条命令被拒。`vcvarsall.bat` 不可行。
  - **`Remove-Item` 是静默无操作**！删目录要用 `[System.IO.Directory]::Delete($p, $true)`；
    否则 `Test-Path` 仍为 True 而命令 exit 0。
  - 命令失败可能没有任何输出，`$LASTEXITCODE` 在可执行文件不存在时为空 —— 查 `Test-Path` 判断。
  - git 提交信息含换行/引号时不要 `-m`：用 `[System.IO.File]::WriteAllText(..., UTF8Encoding($false))`
    写消息文件 + `git commit -F`。
- **同一文件的多次编辑必须串行**：并行 Edit 是读-改-写，后写覆盖先写。症状 = 编译报"未声明"而源码里
  明明写了。不同文件之间并行没问题。
- 跑 Qt 程序需 `E:\msys64\ucrt64\bin` 在 PATH；MinGW 测试可执行文件加 `-static-libgcc -static-libstdc++`。
- 残留的 `dist/lightcpp.exe` 进程会让 CMake 的 post-build 拷贝报 "Permission denied" 使 build exit=1
  （实际链接早已成功）——先 `Stop-Process` 再构建。
- **无头 UI 冒烟测试配方**（改 GUI / clangd 后跑，见 skill `lightcpp-build-verify`）：写自带 `main()` 的 cpp
  （QApplication + 真实 MainWindow/OutputPanel），用 `E:/msys64/ucrt64/bin/g++.exe` 编译，`-I` 上
  `src/LightCppApp/Include`、`src/LightCppText/Include`、`src/LightCppCore/Include`、
  `E:/msys64/ucrt64/include/qt6{,/QtCore,/QtGui,/QtWidgets}`；链接 `build/msys2-ucrt64/src/LightCppApp/CMakeFiles/lightcpp.dir/`
  下除 `main.cpp.obj` 外全部 .obj（含 `lightcpp_autogen/mocs_compilation.cpp.obj`，它提供 moc 故不必自己跑 moc）
  + `libLightCppText.a` + `libLightCppCore.a` + `-L E:/msys64/ucrt64/lib -lQt6Widgets -lQt6Gui -lQt6Core
  -static-libgcc -static-libstdc++`；`QT_QPA_PLATFORM=offscreen` 运行，`QTimer::singleShot` 分阶段断言；
  驱动私有槽用 `QMetaObject::invokeMethod(&w, "onFileActivated", Q_ARG(QString, path))`。
  测试源码放 `%TEMP%\lightcpp_smoke\`（**临时目录会被系统清理，脚本不是长期资产，用前先看还在不在**）。
