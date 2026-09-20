# LightCpp 项目长期约定

## 项目
LightC++ IDE：显示名 "LightC++"，代码标识 `LightCpp`，最终二进制 `lightcpp`。
规范见仓库根 `要求.md`（C++20、Qt 6.5 + QScintilla + ADS、CMake 3.24 + CPM.cmake、spdlog、GTest/gcov）。

## 已落地模块
- `LightCppCore`（静态库，刻意无 Qt）：`LightCpp::Core::Text` 下的 UTF-8 解码/校验与文件读取。
  含通用 `Result<T,E>` sum type（C++20，避免 std::expected）。
- `LightCppText`（静态库，依赖 Qt6::Core）：编码识别 + 转 UTF-8。
  - `detectEncoding()`：BOM 检测 → NUL 结构 → UTF-8 校验 → **本地 ANSI 编码 fallback**（不再走 legacy 评分）。
    在 zh-CN Windows 上 fallback 返回 GBK；ja-JP 返回 Shift-JIS；ko-KR 返回 EUC-KR；en-US 返回 Windows-1252。
    用户可通过 GUI 编码下拉框强制选择其他编码（调用 `readTextFileWithEncoding()`）。
  - `convertToUtf8()`：Unicode 族走 Qt QStringConverter；遗留编码（GBK/Big5/ShiftJIS/EUC-KR/CP1252）走 Win32 MultiByteToWideChar。
  - `readDetectedTextFile()`：端到端管线（读文件→检测编码→转 UTF-8）。
  - `readTextFileWithEncoding(path, encoding)`：用指定编码强制读取文件（用户从下拉框选择时调用）。
  - `localAnsiEncoding()` / `localAnsiDisplayName()`：查询本地 ANSI 代码页及显示名。
  - UTF-16/32 解码后额外检测未配对代理并拒绝（Qt decoder 宽松，需手动校验）。
- `lightcpp-enc` CLI 工具：检测文件编码、`--out` 转 UTF-8、`--list` 列出支持的编码。
- `lightcpp` GUI 应用（Qt6 Widgets，纯文本阶段）：暗色主题 IDE 外壳。
  - `MainWindow`：菜单栏(File/View/Help)、工具栏(New/Open/Save/SaveAll/编码选择器)、
    左侧文件树(QFileSystemModel + QTreeView)、中央编辑器标签页(QTabWidget)、
    底部输出面板、状态栏(文件路径/语言/编码/光标位置)。
  - `CodeEditor`：QPlainTextEdit 子类 + 行号栏 + 当前行高亮 + `CppSyntaxHighlighter`。
  - `CppSyntaxHighlighter`：QSyntaxHighlighter 子类，C/C++ 关键字/类型/字符串/注释/数字/函数/预处理指令。
  - `FileExplorer` / `OutputPanel`：QDockWidget 子类。
  - 打开文件时调用 `readDetectedTextFile()` 自动检测编码并转换，状态栏显示编码信息。
  - **无自定义主题**：使用 Qt 默认原生外观，不设 QPalette / StyleSheet。
    CodeEditor 行号栏/当前行高亮使用 `palette().color()` 动态适配。
    编码选择器在状态栏右下角（QComboBox，display-only），默认 UTF-8。
  - 编码下拉框：UTF-8 + ANSI(<本地编码>)排在前两位，其他按名称字母排序。
    下拉框已启用，用户切换后用 `readTextFileWithEncoding()` 强制以所选编码重新读取文件。
  - windeployqt6 自动部署 Qt DLL 到 `dist/` 目录，可独立运行。
  - **QScintilla/ADS 未安装**，用 QPlainTextEdit + QDockWidget 替代；如需安装 QScintilla：
    `pacman -S mingw-w64-ucrt-x86_64-qscintilla`。
- `LightCppApp` 语法检查：通过 `LanguageServerClient` 用 LSP over stdio 跟 clangd 通信。
  - clangd 路径硬编码在 `MainWindow.cpp` 常量 `kClangdPath` = `E:/msys64/ucrt64/bin/clangd.exe`。
  - Tools 菜单"语法检查"开关启动/停止 LSP。
  - 文本变化 800ms 防抖后发 `textDocument/didChange`（全文同步）。
  - 接收 `textDocument/publishDiagnostics` → 在 CodeEditor 上画波浪下划线 + 行号区红/黄点。
  - 路径在 PATH 上找：`QProcessEnvironment` 把 clangd 所在目录加到子进程 PATH。
- `LightCppApp` 语法补全：基于 LSP `textDocument/completion`。
  - LanguageServerClient::requestCompletion() 返回 JSON-RPC id；handleMessage 用
    `completionRequests_` map 跟踪，emit `completionReady(id, filePath, items)`。
  - CompletionPopup（QListWidget 子类，Qt::Popup）：行首类型字母（f/M/T/v...）+
    label + detail/documentation tooltip；屏幕边缘自动重定位；按 label 前缀过滤。
  - CodeEditor 检测触发字符 `.` `:` `>` 自动开 popup；Ctrl+Space 手动触发；
    popup 打开时拦截 Enter/Tab/Esc/方向键/PageUp/PageDown/Backspace；Enter/Tab 接受条目。
  - MainWindow 用 `QMap<int, QPointer<CodeEditor>> pendingCompletions_` 路由异步响应到
    发出请求的 editor；QPointer 防 tab 关闭后悬空指针。
  - **补全前必须先 changeDocument 推最新全文**（lint didChange 是 300ms 防抖的，
    否则 clangd 拿到旧文本位置错位返回空——这是"只有 include 补全有结果"的根因）。
  - 触发字符 `. : > < "`（`<`/`"` 走 clangd include 补全）；标识符输入与 Backspace
    在 popup 开/关两分支都重启 120ms 空闲定时器重新请求（每次编辑都请求，
    奇偶交替 bug 的修复关键）；refreshPopupFilter 仅作即时反馈。
  - **过期响应守卫**：MainWindow `latestCompletionId_` 只认最新请求；
    旧响应（可能是空）一律忽略——否则旧空响应会关掉新弹窗。
  - mousePressEvent：编辑器内点击关闭 popup 并停定时器。
- **诊断悬停 tooltip**：QTextCharFormat::setToolTip 对 extra selection 无效
  （覆盖层不参与文本布局）；CodeEditor 重写 event() 拦截 QEvent::ToolTip，
  cursorForPosition 匹配 diagnostics_（坐标系一致可直接比较），零宽范围接受
  整行，重叠取最严重，QToolTip::showText 显示 `[Severity] msg (code)`。
- **零宽诊断归一化** normalizedDiagnostic()：clangd 对 `#include` 缺参数类错误
  报零宽 range 且位置在下一空行第 0 列；归一化把这种诊断归属到上一行行尾。
  波浪线/红点/悬停三处共用。零宽区间画波浪线取行尾最后一个可见字符。
  - **未保存文档也能 lint**：Untitled-N 分配虚拟路径 `QDir::tempPath()/lightcpp-untitled/Untitled-N.cpp`
    （CodeEditor::lintPath()，空时回退 filePath()）；保存后 didClose 旧路径 + didOpen 真实路径；
    changeDocument 对未 open 的文档自动补 didOpen。
  - **防抖 300ms** + 诊断输出节流（仅在 errors/warnings 计数变化时打印，避免刷屏）。
  - **stop() 顺序坑**：`process_ = nullptr` 后再 `waitForFinished` 会触发 finished lambda
    访问 nullptr。修复：stop 开头 `process_->disconnect(this)` 断开所有信号；
    finished lambda 用 `sender()` 不依赖成员指针。
- 测试目标 `LightCppCoreTests`（28 例）+ `LightCppTextTests`（34 例），共 62 例全部通过。

## 工程约定（已在本仓库建立）
- CMake 目标统一 `LightCpp` 前缀；严格警告放在 `LightCppWarnings` INTERFACE 目标上，只链接自有目标。
- C++20 基线，**不使用 std::expected 等 C++23 特性**。
- 第三方依赖走 CPM.cmake；本仓库把 CPM.cmake 与 googletest 源码本地化（`cmake/`、`third_party/`），可离线构建。
- 纯逻辑/数据层用 std::string/std::vector/std::optional；与 Qt 交互层才用 QString/QByteArray（待 Qt 就位）。
- 文件 I/O、解析等一律放到后台线程，UI 用 queued 信号槽更新（核心层保持无状态可测）。

## 本机环境
- 工具链 MSYS2 UCRT64：`E:\msys64\ucrt64\bin`（gcc 16.2 / cmake 4.3.3 / ninja 1.13）。
  **Qt6 已安装**（qt6-base 等，位于 `E:\msys64\ucrt64\lib\cmake\Qt6`），无 ICU。
  遗留编码（GBK/Big5/ShiftJIS/EUC-KR）通过 Win32 API 实现，不依赖 ICU。
- **Git**：`D:\Program Files\Git For Windows\bin\git.exe`（MSVC 编译，无需其他组件）；
  备选 `E:\msys64\ucrt64\bin\git.exe`。仓库级身份 lenovo / lenovo@local。
- **工作流约定（用户要求）：每完成一次代码改动就 git commit 一次。**
- 用 PowerShell 构建（Bash 环境 PATH 损坏），先 `$env:PATH = "E:\msys64\ucrt64\bin;" + $env:PATH`；
  需要命令输出时写文件再读。
- MinGW 下测试可执行文件静态链接 GCC 运行时（`-static-libgcc -static-libstdc++`），否则 ctest 派生进程会缺 DLL。
- 运行 Qt 程序需 `E:\msys64\ucrt64\bin` 在 PATH 中（Qt6Core.dll 等依赖）。
