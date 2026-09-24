# LightCpp 项目长期约定

## 项目
LightC++ IDE；显示名 `LightC++`，代码标识 `LightCpp`，二进制 `lightcpp`。
规范见 `要求.md`（C++20、Qt 6.5+、QScintilla+ADS 为目标态、CMake 3.24+CPM、spdlog、GTest/gcov）。

## 模块
- `LightCppCore`（无 Qt 静态库）：`Core::Text` UTF-8 解码/校验与文件读取；自带 `Result<T,E>`。
- `LightCppText`（依赖 Qt6::Core）：编码识别 + 转 UTF-8。
  - `detectEncoding()`：BOM → NUL 结构 → UTF-8 校验 → **本地 ANSI fallback**（zh-CN→GBK / ja-JP→Shift-JIS / ko-KR→EUC-KR / en-US→CP1252）。
  - `convertToUtf8()`：Unicode 族走 QStringConverter；遗留编码（GBK/Big5/ShiftJIS/EUC-KR/CP1252）走 Win32 MultiByteToWideChar。
  - `readDetectedTextFile()` / `readTextFileWithEncoding(path,enc)` / `localAnsiEncoding()` / `localAnsiDisplayName()`。
  - UTF-16/32 解码后须手工拒绝未配对代理（Qt decoder 过于宽松）。
- `lightcpp-enc` CLI：检测编码 / `--out` 转 UTF-8 / `--list`。
- 测试 `LightCppCoreTests` + `LightCppTextTests`，ctest 67 例全绿（2026-09-23）。

## GUI（LightCppApp，Qt6 Widgets，原生外观无自定义主题）
- **布局：嵌套 QSplitter，无 QDockWidget**。外层垂直 = [ 水平(Explorer | 编辑器标签页) , 底部 Output ]，
  均 `setChildrenCollapsible(false)`；View 菜单用自建 checkable QAction（Explorer Ctrl+B / Output Ctrl+J）。
  目的：面板不能被拖成独立浮动窗体。
- `CodeEditor`：QPlainTextEdit + 行号栏 + 当前行高亮 + `CppSyntaxHighlighter`，配色用 `palette()` 动态适配。
- `FileExplorer` / `OutputPanel`：QWidget 子类，各带一行加粗小标题（EXPLORER / OUTPUT）替代 dock 标题栏。
- **OutputPanel 三通道**：`ChannelServer=0` Clangd Log / `ChannelBuild=1` Build Log（默认）/ `ChannelProblems=2` Problems，
  缓冲互不混淆。API `appendServerLog()` / `appendBuildOutput()` / `setProblems()` / `showChannel()` / `clearOutput()`
  ——**`appendOutput()` 已删除**。combo 信号 `currentIndexChanged(int)` 须 lambda 转 Channel（int 不隐式转非作用域枚举）。
  Problems 下拉项带实时计数；空时显示不可选的 "No problems detected."。
- **Problems**：按文件分组 QTreeWidget（父=文件名，子=`severity: msg [code]` + `Ln x, Col y`）；
  行 data UserRole=路径 / +1=行(0基) / +2=列(0基)；单击 emit `problemActivated(path,line,col)`。
  `MainWindow::onProblemActivated()` 先按 `lintPath()` 匹配 tab（未保存缓冲是虚拟路径，不能从磁盘重开）再 `gotoLocation()`。
  `diagnosticsByFile_`(键=lintPath) → `rebuildProblems()` 排序展平；关 tab / 保存换路径 / 关 clangd 时清理。
- 打开文件走 `readDetectedTextFile()`；状态栏显示路径/语言/编码/光标位置；编码下拉框在状态栏右下角（UTF-8 与
  ANSI(<本地编码>) 排前两位），切换后用 `readTextFileWithEncoding()` 强制重读。
- windeployqt6 自动部署 Qt DLL 到 `dist/`。**QScintilla/ADS 未安装**（`pacman -S mingw-w64-ucrt-x86_64-qscintilla`）。

## 语法检查 / 补全（LSP over stdio，clangd）
- `kClangdPath` = `E:/msys64/ucrt64/bin/clangd.exe`（硬编码在 `MainWindow.cpp`）；**默认开启**。
- stderr 用 `setProcessChannelMode(SeparateChannels)` 按行转发，连同 `window/logMessage`/`showMessage` 进 Clangd Log 通道。
- 300ms 防抖发 didChange（全文同步）→ `publishDiagnostics` 画波浪线 + 行号区红/黄点。
- **补全前必须先 changeDocument 推最新全文**（lint 防抖滞后会使 clangd 位置错位返回空 —— "只有 include 补全有结果"的根因）。
- 触发字符 `. : > < "`；标识符输入与 Backspace 在 popup 开/关两分支都重启 120ms 空闲定时器（每次编辑都请求，
  这是奇偶交替 bug 的修复关键）。
- **过期响应守卫**：`latestCompletionId_` 只认最新请求，旧响应（常为空）一律忽略，否则会关掉新弹窗。
- `pendingCompletions_` 用 `QPointer<CodeEditor>` 防 tab 关闭后悬空。
- **stop() 顺序坑**：先 `process_ = nullptr` 再 `waitForFinished` 会让 finished lambda 访问 nullptr；
  修复：stop 开头 `process_->disconnect(this)`，finished lambda 用 `sender()`。
- **零宽诊断归一化** `normalizedDiagnostic()`：clangd 对 `#include` 缺参数报零宽 range 且落在下一空行第 0 列，
  归一化归属到上一行行尾；波浪线/红点/悬停三处共用。
- **诊断悬停 tooltip**：`QTextCharFormat::setToolTip` 对 extra selection 无效（覆盖层不参与文本布局）；
  改为重写 `event()` 拦 `QEvent::ToolTip`，`cursorForPosition` 与 `diagnostics_` 比较，零宽接受整行，重叠取最严重。
- 未保存文档分配虚拟路径 `QDir::tempPath()/lightcpp-untitled/Untitled-N.cpp` 以便 lint；保存后 didClose 旧路径 + didOpen 真实路径。

## 括号自动配对（CodeEditor，2026-09-24）
- `autoCloseBracket(typed)` 插两半、光标居中；已有右半边时**跳过**它；有选区时**包裹**选区（保留选区）。
- `backspaceEmptyPair()`：仅删真正的空对（`(|)` 整体消失，`(x|)` 保留）。**坑**：
  `movePosition(PreviousCharacter, KeepAnchor, 2)` 在文档起点只能左移 1 字符 → 会剩半个括号；
  正确做法 `setPosition(pos-1)` + `setPosition(pos+1, KeepAnchor)` 再 `removeSelectedText()`。
- 引号特例：前一字符是字母/数字/下划线且后面不是同种引号时**不配对**（`1'000`、`foo"`），
  但**有选区时一律包裹**（守卫须带 `!cursor.hasSelection()`）。
- `"` 配对后仍需 `requestCompletionAtCursor("\"")` 触发 clangd include 补全（原路径不能丢）。
- 在 `keyPressEvent` 最前面拦截（早于补全逻辑）；`plainTextKey` 排除 Ctrl/Alt/Meta。
- 验证脚本 `%TEMP%\lightcpp_smoke\bracket_smoke.cpp`，31 项断言。

## 单文件编译 / 运行（Tools 菜单）
- Compile Current File(F7) / Compile && Run(F5) / Run(Ctrl+F5) / Stop(Shift+F5)；Toolchain 子菜单 GCC/Clang 互斥，默认 GCC。
- **build 与 run 共用 `MainWindow::BuildTarget resolveBuildTarget(editor)`**，exe 恒在源码同目录同名；
  Untitled 源码写 `%TEMP%/lightcpp-compile/<tabstem>.cpp`。改路径规则只能改这个函数，否则"运行"找不到"编译"产物。
- 编译 args `-Wall -Wextra -g <src> -o <exe>`；Compile & Run 在 `finished` 里 rc==0 才 `startProgram()`（`runAfterBuild_`）。
- Run 在 exe 不存在时弹 QMessageBox 询问是否立即编译；exe 比源码旧只提醒仍照跑。
- `compiler_` / `runner_` 懒创建复用，MergedChannels → Build Log；两者 PATH 都前置 `kMingwBin`。
- 被运行程序无终端：**必须在 `runner_->started()` 里 `closeWriteChannel()`** 给 stdin EOF，否则 scanf/getline 永久阻塞
  （`start()` 之后立刻 close 无效，那时通道未建立）。

## 工程约定
- CMake 目标统一 `LightCpp` 前缀；严格警告放 `LightCppWarnings` INTERFACE 目标，只链自有目标。
- C++20 基线，**不用 std::expected 等 C++23 特性**；第三方走 CPM.cmake（CPM 与 googletest 已本地化，可离线构建）。
- 纯逻辑层用 std::string/std::vector/std::optional；仅 Qt 交互层用 QString/QByteArray。
- 文件 I/O、解析放后台线程，UI 用 queued 信号槽更新（核心层保持无状态可测）。
- **改正则必须断言**：`QSyntaxHighlighter` 里 invalid 的 QRegularExpression 会被**静默跳过**（只往 stderr 打一行），
  界面毫无异常 —— 历史数字高亮因此哑掉很久（2026-09-24 修复）。
- **每次代码改动都 git commit 一次**（用户要求）。

## 本机环境与工具链
- MSYS2 UCRT64 `E:\msys64\ucrt64\bin`（gcc 16.2 / cmake 4.3.3 / ninja 1.13）；Qt6 已装于此，无 ICU。
- Git：`D:\Program Files\Git For Windows\bin\git.exe`（备选 msys64 版）；仓库级身份 lenovo / lenovo@local。
- **用 PowerShell 构建**（Bash 环境 PATH 坏，`ls`/`head` not found）：先 `$env:PATH="E:\msys64\ucrt64\bin;"+$env:PATH`。
  **坑**：PowerShell 工具不回显 stdout/stderr；`cmd *> file` 写成 UTF-16（Read 判定为二进制）。
  正确姿势：`$out = & cmd ... 2>&1; $out | Out-File -Encoding utf8 file` 再 Read。
  git 提交信息含换行/引号时不要 `-m`：用 `[System.IO.File]::WriteAllText(..., UTF8Encoding($false))` 写消息文件 + `git commit -F`。
- 跑 Qt 程序需 `E:\msys64\ucrt64\bin` 在 PATH；MinGW 测试可执行文件加 `-static-libgcc -static-libstdc++`（否则 ctest 派生进程缺 DLL）。
- **无头 UI 冒烟测试配方**（改 GUI / clangd 后跑）：写自带 `main()` 的 cpp（QApplication + 真实 MainWindow/OutputPanel），
  用 `E:/msys64/ucrt64/bin/g++.exe` 编译，`-I` 上 `src/LightCppApp/Include`、`src/LightCppText/Include`、`src/LightCppCore/Include`、
  `E:/msys64/ucrt64/include/qt6{,/QtCore,/QtGui,/QtWidgets}`；链接 `build/msys2-ucrt64/src/LightCppApp/CMakeFiles/lightcpp.dir/`
  下除 `main.cpp.obj` 外全部 .obj（含 `lightcpp_autogen/mocs_compilation.cpp.obj`，它提供 moc 故不必自己跑 moc）
  + `libLightCppText.a` + `libLightCppCore.a` + `-L E:/msys64/ucrt64/lib -lQt6Widgets -lQt6Gui -lQt6Core
  -static-libgcc -static-libstdc++`；`QT_QPA_PLATFORM=offscreen` 运行，`QTimer::singleShot` 分阶段断言；
  驱动私有槽用 `QMetaObject::invokeMethod(&w, "onFileActivated", Q_ARG(QString, path))`。
