# LightCpp 交付说明 —— 0.2.0 审查与修复

- **日期**：2026-11-04
- **对象**：`https://github.com/SuddenCast03405/LightCpp`（`master`）
- **产物**：`LightCpp-0.2.0-完整仓库-2026-11-04.zip`（源码树，含 vendored 依赖，不含构建产物与 `.git`）
- **未提交 GitHub**（按要求），全部改动只存在于交付包与本说明中

---

## 一、四个问题的结论

| 问题 | 结论 | 证据 |
| --- | --- | --- |
| **文件版本是否匹配** | 修复后**匹配**。修复前有 4 处不一致 | 见 §2.1；`lightcpp-pm --version` = `0.2.0`，CMake/IDE/About/CLI/文档/47 个清单全部 `0.2.0` |
| **接口是否符合** | 修复后**完全符合**。C++ 与 Python 两份 CLI 在 169 组调用上输出逐字节一致（含 stderr 与退出码） | 见 §2.2；`scripts/cli_parity.sh` 输出「对比 169 组，差异 0 组」 |
| **能否通过编译 & CTest** | **能**。根项目 633/633 通过；内核在 GCC 13、Clang 18、ASan+UBSan 三套严格模式下各 5/5 通过 | 见 §3 |
| **`lightcpp.1.md` 是否做完** | **没有**。189 条功能里实现 37、骨架 49、声明 102，完成度约 33%。测试域 0%，调试器无视图层 | 见 §4 与 `docs/IMPLEMENTATION-STATUS.md` |

---

## 二、修复的问题

### 2.1 版本不一致（4 处）

| 位置 | 修复前 | 修复后 |
| --- | --- | --- |
| 根 `CMakeLists.txt` `PROJECT_VERSION` | `0.1.0` | `0.2.0` |
| `core/src/cli_main.cpp` `kVersion` | 硬编码 `"0.1.0"` | `LIGHTC_PROJECT_VERSION` 宏，由 CMake 注入 |
| IDE About 对话框、`QApplication::setApplicationVersion` | 未设置 / 未使用 | 统一取 `LIGHTCPP_VERSION_STRING`（CMake 注入） |
| 47 个 `plugins/*/plugin.json` + 78 处依赖约束 + 8 个 `LIGHTC_PLUGIN_VERSION` | `0.1.0` / `^0.1.0` | `0.2.0` / `^0.2.0` |

现在版本只有**一个源头**：`CMakeLists.txt` 的 `PROJECT_VERSION`。C++ 侧通过编译定义读取，Python 侧跟随，`scripts/cli_parity.sh` 会比对 `--version` 输出，任何一处漂移都会让对拍失败。

> `docs/PLUGIN-AUTHORING.md` 里保留的 `0.1.0` / `^0.1.0` 是**版本约束语法示例**（说明 0.x 锁次版本），不是真实版本号，不应修改。

### 2.2 接口不符（11 项，全部修复）

| # | 问题 | 影响 | 修复 |
| --- | --- | --- | --- |
| 1 | `plugins/` 混放清单与实现目录（`DevCppPlugin/`、`lua/` 等 10 个） | `validate --strict` 报 10 个 `[LOAD] 缺少 plugin.json`，exit 1，CTest `cli_validate` 红 | 实现目录迁到 `src/LightCppPlugins/`，`plugins/` 只留清单 |
| 2 | `README.md`、`.gitignore` 含未解决的冲突标记 | 仓库首页无法阅读 | 整体重写，保留双方有效内容 |
| 3 | Python `validate` 缺 `--platform` / `--json`，且不发 W003 | 两个实现结论不同（C++ 6 警告 / Python 0 警告） | 补齐参数与平台提示，输出逐字符对齐 |
| 4 | Python `info` 缺 `--json`，文本排版与 C++ 完全不同 | 47 个组件的人类可读输出全不一致 | 按 C++ 的字段顺序与措辞重写 |
| 5 | Python 缺 `list`、`host` 两个子命令 | 命令面缺 20% | `list` 补齐；`host` 用 `ctypes` 直接调插件的 C ABI 实现 |
| 6 | C++ `docs --out` 写在帮助里但从未实现 | 文档承诺了不存在的功能 | 打通 `docgen` → `cli_main`，真正支持输出目录 |
| 7 | `graph` 默认格式、排序、`--layer` 校验两侧不同 | 同一命令两种结果 | 统一为 C++ 语义（层级优先排序、非法层级 exit 2） |
| 8 | `resolve` 的 JSON schema 两侧完全不同 | 脚本无法互换实现 | Python 改用 `steps[]/notes[]` |
| 9 | `resolve` 互斥组提示措辞不同 | 文本对不上 | 统一为「互斥组 X 包含 clangd / ccls，运行时仅启用其一」 |
| 10 | `validate` 汇总行、错误前缀完全不同（`✗ [E0xx]` vs `error  [E0xx]`） | 无法对拍 | Python 改为 C++ 格式 |
| 11 | Python 缺 `--version` / `-v` / `version`，帮助文本完全不同 | 基本 CLI 契约不符 | 补齐全局开关，帮助文本与退出码完全一致 |

另外修复的两个**真缺陷**（不是接口对齐，是逻辑错误）：

- `core/src/fs.cpp`：`fs::absolute(".")` 返回 `/path/.`，污染 `validate --json` 的 `root` 字段与所有错误消息 → 增加尾随 `/.` 归一化。
- `core/tests/test_validator.cpp`：`std::find(codes(p).begin(), codes(p).end(), ...) != codes(p).end()` 对**三个不同的临时 vector** 取迭代器，属未定义行为，Clang 下直接编译失败 → 先存变量再比较，并补 `<algorithm>`。
- `core/CMakeLists.txt`：`LIGHTC_WITH_SANITIZERS` 选项声明了但从未生效（sanitizer 构建其实是普通构建）→ 真正接上 `-fsanitize=address,undefined`，MSVC 下显式报错。

### 2.3 构建系统

| 问题 | 修复 |
| --- | --- |
| `core/` 不在根构建内，`ctest` 只跑 IDE 的 628 个用例，内核 5 个用例需单独构建 | 新增 `LIGHTCPP_BUILD_KERNEL`（默认 ON），一次 configure+build+ctest 覆盖 633 个用例 |
| `_fetch_deps.cmake` 下载失败会留下 0 字节 tarball，下次仍当成功 | 重写为「下载到 `.part` → 校验 HTTP 状态 → 校验文件大小下限 → 才改名」；解压后校验目录结构，缺失则删 tarball 重下。实测 0 字节残留可自愈 |
| `scripts/package.sh` 的 `[ -f x ] && cp ...` 在条件为假时返回 1，`set -e` 下静默中止 | 改为 `if ... then ... fi` |
| CI 里 `build.yml` 有重复的 python job，`package.needs` 依赖已删除的 job | 删除重复 job，修正 `needs` |
| 没有一条命令能跑完全部验证 | 新增 `scripts/build_all.sh`（依赖 → 内核三模式 → IDE → CTest → Python → 对拍）；新增 `scripts/cli_parity.sh`（169 组对拍） |

### 2.4 其它

- `core/src/text.cpp` 表格最小列宽 3 → 与 Python 的 `render_table` 一致，修掉宽度为 0 的空列。
- `tests/test_pm.py` 中关于 `plugins/` 仍含实现目录的注释已过时，改写；新增 `test_validate_platform_filtering` 覆盖 W003 的平台切换。
- `docs/PLUGINS.md`、`docs/FEATURES.md` 重新生成，两侧 `docs --check` 均通过。

---

## 三、验证结果（全部可复现）

### 3.1 根项目（IDE + 内核）

```
cmake -S . -B build/ci -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=/opt/Qt/6.8.3/gcc_64 -DLLVM_ROOT=/usr/lib/llvm-18
cmake --build build/ci --parallel 8        # 246/246 目标
ctest --test-dir build/ci
→ 100% tests passed, 0 tests failed out of 633
```

6 个跳过项是 Linux 下缺 CJK locale 的 GBK/Big5 用例，属预期。

### 3.2 内核 standalone（三种模式，均 `-DLIGHTC_WERROR=ON`）

| 配置 | 结果 |
| --- | --- |
| GCC 13.3.0, Release | 5/5 |
| Clang 18, Release | 5/5 |
| Clang 18, Debug + `-DLIGHTC_WITH_SANITIZERS=ON`（ASan+UBSan） | 5/5，无 sanitizer 报告 |

### 3.3 CLI 双实现对拍

```
scripts/cli_parity.sh build/ci/bin/lightcpp-pm
→ 对比 169 组，差异 0 组
```

覆盖：8 个全局开关（`--help`/`-h`/`--version`/`-v`/`version`/未知命令/空参数/子命令内 `-h`）、10 个子命令、7 个层级与 1 个非法层级、3 种图格式、3 个目标平台、47 个组件 × 2 种 `info` 形态、6 个扩展包的 `resolve`、`host` 的 5 种组合。逐组比较 **stdout + stderr + exit code**。

### 3.4 在交付包解压后的干净树上复验

```
cmake -S core -B /tmp/pkg-kernel -G Ninja -DLIGHTC_WERROR=ON && ... && ctest
→ 100% tests passed, 0 tests failed out of 5

cmake -P _fetch_deps.cmake
→ -- third-party deps ready          （vendored 依赖已在包内，不联网）

cmake -S . -B /tmp/pkg-root ... && cmake --build ... && ctest
→ 100% tests passed, 0 tests failed out of 633
```

### 3.5 Python

```
python3 -m unittest discover -s tests -p "test_*.py"
→ Ran 39 tests ... OK
```

（在无 `.git` 的交付包中为 `OK (skipped=1)`：`test_every_plugin_dir_is_tracked` 需要一个 git 工作区。该用例在有 `.git` 时已实测通过。）

---

## 四、`lightcpp.1.md` 的实际完成度

**没有做完，且差距不小。** 逐条证据见 `docs/IMPLEMENTATION-STATUS.md`，摘要：

| 域 | 条目 | 实现 | 骨架 | 声明 | 完成度 |
| --- | --- | --- | --- | --- | --- |
| 语言与编辑器基础 | 5 | 1 | 1 | 3 | 30% |
| 项目与文件管理 | 4 | 3 | 1 | 0 | 90% |
| 构建配置与执行 | 27 | 10 | 14 | 3 | 60% |
| Clang 工具链集成 | 8 | 6 | 2 | 0 | 90% |
| 调试器 | 34 | 0 | 9 | 25 | 15% |
| 代码智能 | 23 | 10 | 5 | 8 | 55% |
| 静态分析与代码质量 | 13 | 0 | 6 | 7 | 25% |
| 版本控制 | 26 | 5 | 6 | 15 | 30% |
| 终端与工具集成 | 15 | 1 | 2 | 12 | 15% |
| 测试 | 15 | 0 | 0 | 15 | 0% |
| 文档与帮助 | 11 | 1 | 3 | 7 | 25% |
| Qt 生态 | 7 | 0 | 0 | 7 | 0% |
| **合计** | **188** | **37** | **49** | **102** | **33%** |

三个最明显的空洞：

1. **调试器只有外壳**。`DebuggerController.cpp`（906 行）能启动 GDB/LLDB/cdb、下普通断点、单步、继续、停止，并发六个信号；但**变量、监视、求值、调用栈、线程、寄存器、内存、反汇编八个视图一个都没有**。条件/日志/忽略次数/临时/函数/地址/内存断点只有规格层（`core/plugins/GdbPlugin/` 有 MI 命令生成与单测），IDE 层未接。34 条里实现 0 条。
2. **测试域是零**。`test-core` 是纯清单插件，四个框架适配器（gtest/catch2/boost-test/ctest）只声明贡献点，`host --require-native` 因此把它们判为「缺少实现库」。IDE 里没有测试视图、没有运行入口、没有覆盖率。讽刺的是 LightCpp 自己的 633 个用例跑得很好。
3. **Git 只有最小面**。`GitClient.cpp` 只有 `refreshStatus()` 和通用 `run(args)`：状态、diff、log、commit、init 可用；分支视图、合并、rebase、cherry-pick、blame、冲突解决、子模块、worktree 全无。

26 条「已声明但无实现」的清单里，19 条背后有原生 `.so`（cmake/make/meson/ninja/gdb/clang-core/cpp-base/test-core），只是这些插件只注册了「引擎名」这一层贡献点，真正的业务能力仍在 IDE 侧；另 15 个（asm/terminal/git/docs-core/pkg-*/wsl/serial/log/hex/media/qt-*）是纯清单插件。

**这不是缺陷隐瞒**：仓库里三份文档分工明确——`FEATURES.md` 说「这条归谁」，`PLAN-TRACEABILITY.md` 说「规划每一节有没有人接」，新的 `IMPLEMENTATION-STATUS.md` 说「接了之后代码跑不跑得起来」。任何一份单独拿出来都不能当作「已完成」的证明。

---

## 五、改动清单

### 新增

| 文件 | 作用 |
| --- | --- |
| `docs/IMPLEMENTATION-STATUS.md` | 188 条功能的「实现/骨架/声明」逐条判定与代码证据 |
| `scripts/build_all.sh` | 一键：依赖 → 内核三模式 → IDE → CTest → Python → 对拍 |
| `scripts/cli_parity.sh` | C++/Python 两份 CLI 的 169 组逐字节对拍 |
| `tests/test_pm.py::test_validate_platform_filtering` | W003 平台切换的回归用例 |

### 迁移

| 从 | 到 |
| --- | --- |
| `plugins/{DevCppPlugin,MsvcPlugin,GoPlugin,RustPlugin,PythonPlugin,ZigPlugin,QtCppPlugin,WxWidgetsPlugin,Support,lua}/` | `src/LightCppPlugins/` 同名目录 |
| `plugins/CMakeLists.txt` | `src/LightCppPlugins/CMakeLists.txt` |

### 主要修改

`CMakeLists.txt`、`core/CMakeLists.txt`、`core/src/{cli_main,docgen,fs,text,host}.cpp`、`core/include/lightcpp/{docgen,fs}.h`、`core/tests/test_runtime.cpp`、`core/tests/test_validator.cpp`、`tools/lightcpp_pm.py`、`tests/test_pm.py`、`_fetch_deps.cmake`、`scripts/package.sh`、`.github/workflows/{build,ci}.yml`、`README.md`、`.gitignore`、`docs/{PLUGIN-TRACEABILITY→PLAN-TRACEABILITY,README-PLUGIN-ECOSYSTEM,PLUGIN-AUTHORING}.md`、`docs/{PLUGINS,FEATURES}.md`（重新生成）、47 个 `plugins/*/plugin.json`、8 个 `core/plugins/*/CMakeLists.txt`。

---

## 六、遗留与建议

1. **`lightcpp.1.md` 的六个缺口**（调试器视图、测试域、静态分析三件套、Git 分支视图、QML/`.ui`/RST 预览、包管理器集成）按现有代码量估算约为仓库的 1.5 倍工作量，建议拆成独立里程碑而不是塞进 0.3.0。
2. **`.chatgpt/progress.md`** 是上一轮 AI 审查留下的过程记录（26 KB），随仓库发布意义不大，建议加入 `.gitignore` 或移出仓库。本次未删（属于上游既有内容）。
3. **`docs/PLUGIN-AUTHORING.md` 里的 `0.1.0`** 是版本约束语法示例，刻意保留，不要跟着版本号一起改。
4. **Windows/macOS 的对拍**：`scripts/cli_parity.sh` 已在 CI 的 Linux/macOS job 中启用；Windows 因 `.dll` 命名与换行翻译会让 diff 失去意义，只在 `build.yml` 的非 Windows 矩阵中运行。
