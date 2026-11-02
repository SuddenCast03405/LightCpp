<!-- 由 tools/lightcpp_pm.py docs 生成，请勿手工编辑 -->

# 功能目录

规划中的每一条能力都登记在此，并用 `plugins` / `except` 标注归属，`tools/lightcpp_pm.py validate` 会强制要求所有 `required` 功能有归属。

## 语言与编辑器基础

| 功能 ID                    | 说明                                                                                                              | 状态 | 归属插件                                                          |
|----------------------------|-------------------------------------------------------------------------------------------------------------------|------|-------------------------------------------------------------------|
| `lang.cpp.highlight`       | C/C++ 语法高亮                                                                                                    | 目标 | `cpp-base`                                                        |
| `lang.asm.highlight`       | 汇编语法高亮（x86 / Arm）                                                                                         | 目标 | `asm`                                                             |
| `lang.asm.compile`         | 汇编文件编译与目标文件产出                                                                                        | 目标 | `asm`                                                             |
| `lang.asm.limit`           | 非 x86/Arm 架构不提供汇编支持（规划约束）                                                                         | 目标 | `asm`                                                             |
| `lang.buildfile.highlight` | 构建描述文件高亮（CMakeLists.txt / *.cmake / Makefile / *.ninja / meson.build / BUILD / .clangd / .clang-format） | 目标 | `cmake`, `meson`, `make`, `ninja`, `bazel`, `qmake`, `clang-core` |

## 项目与文件管理

| 功能 ID            | 说明                              | 状态 | 归属插件     |
|--------------------|-----------------------------------|------|--------------|
| `project.create`   | 新建项目 / 打开项目 / 项目模板    | 目标 | `build-core` |
| `project.tree`     | 项目树 / 文件树 / 目录监视        | 目标 | `build-core` |
| `project.file.ops` | 添加文件 / 删除文件 / 重命名文件  | 目标 | `build-core` |
| `project.targets`  | 项目依赖关系 / 目标 (Target) 管理 | 目标 | `build-core` |

## 构建配置与执行

| 功能 ID                           | 说明                                                     | 状态   | 归属插件     |
|-----------------------------------|----------------------------------------------------------|--------|--------------|
| `build.config.configuration`      | 项目配置 / 多构建配置 (Debug / Release / RelWithDebInfo) | 目标   | `build-core` |
| `build.config.platform`           | 多目标平台支持                                           | 目标   | `build-core` |
| `build.config.runtime`            | 环境变量 / 工作目录 / 命令行参数配置                     | 目标   | `build-core` |
| `build.toolchain.cross`           | 交叉编译配置 / 交叉编译工具链配置                        | 目标   | `build-core` |
| `build.toolchain.detect`          | 工具链检测 / 编译器检测                                  | 目标   | `build-core` |
| `build.toolchain.standard`        | 标准选择 (C / C++ Standard)                              | 目标   | `build-core` |
| `build.toolchain.defines`         | 宏定义管理                                               | 目标   | `build-core` |
| `build.toolchain.includes`        | Include 路径管理                                         | 目标   | `build-core` |
| `build.toolchain.libs`            | 链接库管理                                               | 目标   | `build-core` |
| `build.toolchain.flags`           | 编译选项管理                                             | 目标   | `build-core` |
| `build.exec.build`                | 本地构建                                                 | 目标   | `build-core` |
| `build.exec.compile`              | 编译                                                     | 目标   | `build-core` |
| `build.exec.rebuild`              | 重新编译                                                 | 目标   | `build-core` |
| `build.exec.clean`                | 清理                                                     | 目标   | `build-core` |
| `build.exec.install`              | 安装 (Install)                                           | 目标   | `build-core` |
| `build.exec.package`              | 打包 (Package)                                           | 目标   | `build-core` |
| `build.exec.target`               | 构建指定 target                                          | 目标   | `build-core` |
| `build.exec.parallel`             | 并行构建                                                 | 目标   | `build-core` |
| `build.exec.incremental`          | 增量构建                                                 | 目标   | `build-core` |
| `build.exec.remote`               | 远程构建                                                 | 规划中 | `build-core` |
| `build.diagnostics.output`        | 构建输出窗口                                             | 目标   | `build-core` |
| `build.diagnostics.log`           | 构建日志保存                                             | 目标   | `build-core` |
| `build.diagnostics.error`         | 编译错误解析                                             | 目标   | `build-core` |
| `build.diagnostics.warning`       | 编译警告解析                                             | 目标   | `build-core` |
| `build.diagnostics.jump`          | 点击错误 / 警告跳转至对应文件及行号                      | 目标   | `build-core` |
| `build.diagnostics.commandline`   | 编译命令查看（具体执行的编译命令行）                     | 目标   | `build-core` |
| `build.advanced.cache`            | 构建缓存支持 / CCache 支持                               | 目标   | `build-core` |
| `build.advanced.compile_commands` | 生成 / 管理编译数据库 (compile_commands.json)            | 目标   | `build-core` |

## Clang 工具链集成

| 功能 ID                 | 说明                                                                  | 状态 | 归属插件     |
|-------------------------|-----------------------------------------------------------------------|------|--------------|
| `clang.format.single`   | 单文件格式化优先调用 clangd 内置能力                                  | 目标 | `clang-core` |
| `clang.format.project`  | 多文件 / 项目级格式化调用独立 clang-format 可执行文件                 | 目标 | `clang-core` |
| `clang.tidy.realtime`   | clang-tidy 实时检查（可关闭以保证大项目性能）                         | 目标 | `clang-core` |
| `clang.discovery.scan`  | 打开项目时扫描项目根目录及父目录的 .clang-format / .clang-tidy        | 目标 | `clang-core` |
| `clang.discovery.apply` | 存在配置时自动应用规则，无需手动配置                                  | 目标 | `clang-core` |
| `clang.settings.tidy`   | 开关 clang-tidy 实时检查                                              | 目标 | `clang-core` |
| `clang.settings.style`  | 默认 clang-format 风格（LLVM / Google / Chromium / Mozilla / WebKit） | 目标 | `clang-core` |
| `clang.settings.path`   | 自定义 clangd / clang-tidy 可执行文件路径                             | 目标 | `clang-core` |

## 调试器

| 功能 ID                   | 说明            | 状态 | 归属插件                            |
|---------------------------|-----------------|------|-------------------------------------|
| `debug.bp.line`           | 断点            | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.bp.conditional`    | 条件断点        | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.bp.logpoint`       | 日志断点        | 目标 | `gdb`, `lldb`                       |
| `debug.bp.hitcount`       | 忽略次数断点    | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.bp.temporary`      | 临时断点        | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.bp.function`       | 函数断点        | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.bp.address`        | 地址断点        | 目标 | `gdb`, `lldb`, `cdb`                |
| `debug.bp.memory`         | 内存断点        | 目标 | `gdb`, `lldb`, `cdb`                |
| `debug.control.step_in`   | 单步进入        | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.control.step_over` | 单步跳过        | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.control.step_out`  | 单步跳出        | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.control.continue`  | 继续运行        | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.control.pause`     | 暂停            | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.control.stop`      | 停止            | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.view.locals`       | 局部变量查看    | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.view.watch`        | 监视变量        | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.view.evaluate`     | 表达式求值      | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.view.callstack`    | 调用栈          | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.view.threads`      | 线程列表        | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.view.registers`    | 寄存器视图      | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.view.memory`       | 内存视图        | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.view.disassembly`  | 反汇编视图      | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.view.console`      | 控制台输入输出  | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.attach.process`    | 进程附加        | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.attach.core`       | core 文件加载   | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.attach.remote`     | 远程调试        | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.symbol.load`       | 符号加载        | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.symbol.sourcemap`  | 源码映射        | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.symbol.pretty`     | pretty-printer  | 目标 | `gdb`, `lldb`                       |
| `debug.symbol.stl`        | STL 容器展开    | 目标 | `gdb`, `lldb`                       |
| `debug.symbol.smartptr`   | 智能指针展开    | 目标 | `gdb`, `lldb`                       |
| `debug.symbol.enum`       | 枚举显示        | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |
| `debug.symbol.natvis`     | natvis 可视化器 | 目标 | `cdb`, `vs-debugger`                |
| `debug.log.window`        | 调试日志窗口    | 目标 | `gdb`, `lldb`, `cdb`, `vs-debugger` |

## 代码智能

| 功能 ID                    | 说明         | 状态 | 归属插件             |
|----------------------------|--------------|------|----------------------|
| `intel.completion`         | 代码补全     | 目标 | `clang-core`, `ccls` |
| `intel.highlight.semantic` | 语义高亮     | 目标 | `clang-core`, `ccls` |
| `intel.diagnostics`        | 诊断错误     | 目标 | `clang-core`, `ccls` |
| `intel.quickfix`           | 快速修复     | 目标 | `clang-core`, `ccls` |
| `intel.hover`              | 悬停提示     | 目标 | `clang-core`, `ccls` |
| `intel.signature`          | 函数签名提示 | 目标 | `clang-core`, `ccls` |
| `intel.goto.definition`    | 转到定义     | 目标 | `clang-core`, `ccls` |
| `intel.find.references`    | 查找引用     | 目标 | `clang-core`, `ccls` |
| `intel.symbol.search`      | 符号搜索     | 目标 | `clang-core`, `ccls` |
| `intel.rename`             | 重命名       | 目标 | `clang-core`, `ccls` |
| `intel.format`             | 代码格式化   | 目标 | `clang-core`, `ccls` |
| `intel.include.completion` | Include 补全 | 目标 | `clang-core`, `ccls` |
| `intel.include.sort`       | Include 排序 | 目标 | `clang-core`, `ccls` |
| `intel.macro.expand`       | 宏展开       | 目标 | `clang-core`, `ccls` |
| `intel.type.hint`          | 类型推导提示 | 目标 | `clang-core`, `ccls` |
| `intel.nav.code`           | 代码导航     | 目标 | `clang-core`, `ccls` |
| `intel.nav.call.hierarchy` | 调用层级     | 目标 | `clang-core`, `ccls` |
| `intel.nav.type.hierarchy` | 类型层级     | 目标 | `clang-core`, `ccls` |
| `intel.nav.inheritance`    | 继承关系     | 目标 | `clang-core`, `ccls` |
| `intel.nav.class`          | 类视图       | 目标 | `clang-core`, `ccls` |
| `intel.nav.callgraph`      | 函数调用图   | 目标 | `clang-core`, `ccls` |
| `intel.nav.outline`        | 代码大纲     | 目标 | `clang-core`, `ccls` |
| `intel.nav.doc`            | 文档注释预览 | 目标 | `clang-core`, `ccls` |

## 静态分析与代码质量

| 功能 ID                      | 说明                  | 状态 | 归属插件     |
|------------------------------|-----------------------|------|--------------|
| `analysis.cppcheck`          | Cppcheck              | 目标 | `clang-core` |
| `analysis.clang_sa`          | Clang Static Analyzer | 目标 | `clang-core` |
| `analysis.pvs`               | PVS-Studio 集成       | 目标 | `clang-core` |
| `analysis.coverity`          | Coverity 集成         | 目标 | `clang-core` |
| `analysis.compiler_warnings` | 编译器警告分析        | 目标 | `clang-core` |
| `analysis.style`             | 代码规范检查          | 目标 | `clang-core` |
| `analysis.complexity`        | 复杂度分析            | 目标 | `clang-core` |
| `analysis.unused`            | 未使用变量检查        | 目标 | `clang-core` |
| `analysis.leak`              | 内存泄漏检查          | 目标 | `clang-core` |
| `analysis.nullptr`           | 空指针检查            | 目标 | `clang-core` |
| `analysis.bounds`            | 数组越界检查          | 目标 | `clang-core` |
| `analysis.coding_standard`   | 编码规范检查          | 目标 | `clang-core` |
| `analysis.security`          | 安全漏洞检查          | 目标 | `clang-core` |

## 版本控制

| 功能 ID               | 说明             | 状态 | 归属插件 |
|-----------------------|------------------|------|----------|
| `git.clone`           | 克隆仓库         | 目标 | `git`    |
| `git.open`            | 打开仓库         | 目标 | `git`    |
| `git.branch.manage`   | 分支管理         | 目标 | `git`    |
| `git.branch.checkout` | 切换分支         | 目标 | `git`    |
| `git.branch.create`   | 创建分支         | 目标 | `git`    |
| `git.merge`           | 合并             | 目标 | `git`    |
| `git.rebase`          | rebase           | 目标 | `git`    |
| `git.cherry_pick`     | cherry-pick      | 目标 | `git`    |
| `git.stash`           | stash            | 目标 | `git`    |
| `git.commit`          | commit           | 目标 | `git`    |
| `git.push`            | push             | 目标 | `git`    |
| `git.pull`            | pull             | 目标 | `git`    |
| `git.fetch`           | fetch            | 目标 | `git`    |
| `git.history.log`     | 查看历史         | 目标 | `git`    |
| `git.history.blame`   | 查看 blame       | 目标 | `git`    |
| `git.history.diff`    | 查看 diff        | 目标 | `git`    |
| `git.conflict`        | 解决冲突         | 目标 | `git`    |
| `git.submodule`       | 子模块支持       | 目标 | `git`    |
| `git.worktree`        | worktree 支持    | 目标 | `git`    |
| `git.tag`             | tag 管理         | 目标 | `git`    |
| `git.remote`          | remote 管理      | 目标 | `git`    |
| `git.status`          | 文件状态显示     | 目标 | `git`    |
| `git.inline`          | 行内修改标记     | 目标 | `git`    |
| `git.commit.template` | 提交模板         | 目标 | `git`    |
| `git.commit.gpg`      | GPG 签名         | 目标 | `git`    |
| `git.auth.ssh_https`  | SSH / HTTPS 认证 | 目标 | `git`    |

## 终端与工具集成

| 功能 ID           | 说明                | 状态     | 归属插件          |
|-------------------|---------------------|----------|-------------------|
| `tool.terminal`   | 内置终端            | 已知问题 | `terminal`        |
| `tool.palette`    | 命令面板            | 目标     | `terminal`        |
| `tool.external`   | 外部工具配置        | 目标     | `terminal`        |
| `tool.run`        | 运行程序            | 目标     | `terminal`        |
| `tool.launch`     | 调试程序（插件）    | 目标     | `terminal`        |
| `tool.shell`      | Shell 集成          | 目标     | `terminal`        |
| `tool.pkg.vcpkg`  | vcpkg               | 目标     | `pkg-vcpkg`       |
| `tool.pkg.conan`  | conan               | 目标     | `pkg-conan`       |
| `tool.pkg.hunter` | Hunter              | 目标     | `pkg-hunter`      |
| `tool.pkg.system` | apt / brew / winget | 目标     | `pkg-native`      |
| `tool.wsl`        | WSL 集成（插件）    | 目标     | `wsl`             |
| `tool.serial`     | 串口终端            | 目标     | `serial-terminal` |
| `tool.log`        | 日志查看器          | 目标     | `log-viewer`      |
| `tool.hex`        | Hex 查看器          | 目标     | `hex-viewer`      |
| `tool.media`      | 图像 / 资源预览     | 目标     | `media-preview`   |

## 测试

| 功能 ID                   | 说明             | 状态 | 归属插件     |
|---------------------------|------------------|------|--------------|
| `test.unit.integration`   | 单元测试集成     | 目标 | `test-core`  |
| `test.explorer`           | 测试列表视图     | 目标 | `test-core`  |
| `test.run.single`         | 运行单个测试     | 目标 | `test-core`  |
| `test.run.all`            | 运行全部测试     | 目标 | `test-core`  |
| `test.failure.jump`       | 测试失败跳转     | 目标 | `test-core`  |
| `test.output.parse`       | 测试输出解析     | 目标 | `test-core`  |
| `test.timeout`            | 测试超时控制     | 目标 | `test-core`  |
| `test.log`                | 测试日志         | 目标 | `test-core`  |
| `test.rerun`              | 测试重跑         | 目标 | `test-core`  |
| `test.coverage.report`    | 覆盖率统计       | 目标 | `test-core`  |
| `test.coverage.gcov_lcov` | gcov / lcov 集成 | 目标 | `test-core`  |
| `test.fw.gtest`           | GoogleTest 适配  | 目标 | `gtest`      |
| `test.fw.catch2`          | Catch2 适配      | 目标 | `catch2`     |
| `test.fw.boost`           | Boost.Test 适配  | 目标 | `boost-test` |
| `test.fw.ctest`           | CTest 适配       | 目标 | `ctest`      |

## 文档与帮助

| 功能 ID                | 说明                  | 状态 | 归属插件      |
|------------------------|-----------------------|------|---------------|
| `doc.quickhelp`        | 快速帮助              | 目标 | `docs-core`   |
| `doc.api`              | API 文档浏览          | 目标 | `docs-core`   |
| `doc.doxygen`          | Doxygen 注释解析      | 目标 | `docs-core`   |
| `doc.stdlib`           | C/C++ 标准文档        | 目标 | `docs-core`   |
| `doc.thirdparty`       | 第三方库文档          | 目标 | `docs-core`   |
| `doc.inline`           | 代码提示文档          | 目标 | `docs-core`   |
| `doc.shortcuts`        | 快捷键帮助            | 目标 | `docs-core`   |
| `doc.plugin`           | 插件文档              | 目标 | `docs-core`   |
| `doc.preview.markdown` | Markdown 预览         | 目标 | `doc-preview` |
| `doc.preview.rst`      | reStructuredText 预览 | 目标 | `doc-preview` |
| `doc.preview.browser`  | 内置浏览器            | 目标 | `doc-preview` |

## Qt 生态

| 功能 ID                | 说明                                       | 状态 | 归属插件      |
|------------------------|--------------------------------------------|------|---------------|
| `lang.qml.highlight`   | QML 语法高亮                               | 目标 | `qt-qml`      |
| `lang.qml.diagnostics` | QML 诊断与补全                             | 目标 | `qt-qml`      |
| `lang.ui.preview`      | .ui 文件预览与编辑器                       | 目标 | `qt-ui`       |
| `lang.ui.resource`     | .qrc 资源预览                              | 目标 | `qt-ui`       |
| `qt.project`           | Qt 项目识别与 .pro / .pri 解析             | 目标 | `qt-cpp-core` |
| `qt.qmake.build`       | 基于 qmake 的 Qt 项目构建                  | 目标 | `qt-cpp-core` |
| `qt.cmake.integration` | Qt CMake 目标识别与构建（前置 CMake 插件） | 目标 | `qt-cmake`    |

合计 **189** 条功能。
