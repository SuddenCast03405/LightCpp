# LightCpp 插件生态改造 · 交付说明

> 交付时间：2026-11-02 22:10（GMT+8）
> 依据：`lightcpp.1.md`（C/C++ 插件生态规划）
> 目标仓库：`GitHub/SuddenCast03405/LightCpp`（默认分支 `main`，仓库内无 `master`）

## 一、先说结论

| 项目 | 结果 |
| --- | --- |
| 代码改动 | ✅ 已完成并在本地提交（5 个 commit） |
| 推送到 GitHub | ❌ **失败**：`403 Resource not accessible by integration`，当前 GitHub 连接器签发的是只读令牌（可读仓库、无 contents:write），`git push` 与 REST 建 ref 均被拒 |
| 兜底方案 | ✅ 已按你的要求存入项目资产（本说明 + 交付包 zip + 5 个 git 补丁） |
| 改动质量 | `validate --strict` 0 错 0 警；25 个用例全绿；生成文档与清单一致；补丁在「干净基线 + git am」上端到端复验通过 |

## 二、这次到底改了什么

仓库原本只有 `LICENSE`、`README.md`（一行）、`.gitignore`，没有任何实现。本次把规划里的**插件生态**整层落成机器可读、可校验、可解析的资产，而不是再写一份文档。

### 1. 47 个组件清单（`plugins/<id>/plugin.json`）

42 个插件 + 5 个扩展包，每个都有层级、强依赖、推荐/自动安装、功能集、贡献点、激活事件：

- **核心层**：`cpp-base`（libclang / gcc / llvm / msvc-clang-cl，调试底层仅在装了调试器时生效）
- **语言层**：`clang-core`（clangd + clang-format + clang-tidy）、`ccls`、`asm`（仅 x86/Arm）、`qt-qml`、`qt-ui`
- **框架层**：`build-core`（独占 32 条项目/构建能力）
- **构建系统层**：`cmake` `meson` `qmake` `bazel` `make` `ninja` `vs-project` `devcpp` `xcodebuild` `compile-commands` `custom-command` `qt-cmake`
- **调试器层**：`gdb` `lldb` `cdb` `vs-debugger`
- **工具层**：`git` `terminal` `pkg-vcpkg` `pkg-conan` `pkg-hunter` `pkg-native` `wsl` `serial-terminal` `log-viewer` `hex-viewer` `media-preview` `test-core` `gtest` `catch2` `boost-test` `ctest` `docs-core` `doc-preview` `qt-cpp-core`
- **扩展包**：`cpp-extension-pack`（12 成员，含 Ninja）`gnu-pack` `llvm-pack` `msvc-pack` `qt-pack`

### 2. 189 条功能目录（`catalog/features.json`）

12 个功能域、40 个功能集，逐条标注归属插件与状态。这样规划里那几页"应当具备的功能"清单变成了可查询、可统计、可验收的数据：

| 域 | 条数 | 域 | 条数 |
| --- | --- | --- | --- |
| 调试器 | 34 | 版本控制 | 26 |
| 构建配置与执行 | 28 | 代码智能 | 23 |
| 终端与工具集成 | 15 | 测试 | 15 |
| 静态分析与代码质量 | 13 | 文档与帮助 | 11 |
| Clang 工具链集成 | 8 | Qt 生态 | 7 |
| 语言与编辑器基础 | 5 | 项目与文件管理 | 4 |

规划里的三处"坑"被显式标记，而不是假装已完成：远程构建 `planned`、内置终端 `known-issue`（原文"目前有问题"）、汇编仅 x86/Arm。

### 3. 可执行的不变量（`catalog/invariants.json`）

规划里的硬条款变成 CI 会拦的规则：

- 所有 `build-system` 插件必须依赖 `cpp-base` + `build-core`；所有调试器必须依赖 `cpp-base`
- 装 CMake / Meson **自动连带安装 Ninja**；装 Dev-C++ **强依赖 Make**
- `cpp-extension-pack` 成员固定为 12 项（含 Ninja），`qt-pack` 固定 4 项
- clangd 与 ccls 同属 `cpp-lsp` 互斥组：共存安装、同项目只启用其一

### 4. 工具链（`tools/lightcpp_pm.py`，零第三方依赖）

```
validate     25 类错误码：字段/ID/版本/依赖/环/扩展包纯度/功能覆盖/互斥组/不变量
resolve      安装闭包 + 拓扑安装顺序，自动带入 autoInstall，提示互斥引擎
info/engines/matrix/coverage/graph/docs    单插件详情、引擎组、矩阵、覆盖统计、mermaid/dot 依赖图、文档生成
```

`resolve cmake` 的真实输出：

```
| 1 | cpp-base   | 核心基础层              | 由 cmake 引入         |
| 2 | build-core | 项目与构建系统核心框架  | 由 cmake 引入         |
| 3 | cmake      | 具体项目 / 构建系统支持 | 直接安装              |
| 4 | ninja      | 具体项目 / 构建系统支持 | 自动连带安装（cmake） |
```

### 5. 文档与 CI

- `docs/ARCHITECTURE.md`：分层与依赖方向、clangd/ccls 互斥、格式化双策略、调试器平台矩阵、依赖三级语义、错误码索引
- `docs/PLAN-TRACEABILITY.md`：规划每一节 → 插件/功能集的逐条映射，含 5 处建模取舍说明
- `docs/PLUGIN-AUTHORING.md`：新增插件的完整流程与字段速查
- `docs/PLUGINS.md` / `docs/FEATURES.md`：**自动生成**，CI 用 `docs --check` 防止与清单漂移
- `.github/workflows/validate.yml`：`validate --strict` + `docs --check` + 单元测试

## 三、过程中发现并修掉的一个真问题

`.gitignore` 里原有的 `build-*/` 会匹配**任意层级**的 `build-*` 目录，把 `plugins/build-core/` 一起吞掉，首次提交里因此缺了构建核心框架插件（46 而非 47）。
已改为只锚定仓库根（`/build/`、`/build-*/`），并补了一条回归用例：**磁盘上的插件目录必须全部被 git 跟踪**。这也是补丁集里第 4 个 commit 的由来。

## 四、怎么落到 GitHub（拿到可写权限后）

三选一：

```bash
# 方式 A：应用补丁（保留 5 个提交与提交信息，推荐）
git clone https://github.com/SuddenCast03405/LightCpp.git && cd LightCpp
git am /path/to/LightCpp-补丁/*.patch
git push origin main
git branch feat/plugin-ecosystem && git push origin feat/plugin-ecosystem

# 方式 B：整目录覆盖（不保留提交历史）
unzip LightCpp-交付包.zip && cp -r LightCpp/* <repo>/ && cd <repo> && git add -A && git commit -m "feat: 落地插件生态清单与工具链"

# 方式 C：只取补丁核对
git apply --stat /path/to/LightCpp-补丁/0001-plugin-manifests.patch
```

> 注：仓库默认分支是 `main`，没有 `master`。按你的要求，建议先落 `main`，再从同一提交拉 `feat/plugin-ecosystem` 继续迭代。

## 五、验收自检

```bash
python tools/lightcpp_pm.py validate --strict   # 47 个组件 / 40 功能集 / 189 条功能，0 错 0 警
python tools/lightcpp_pm.py docs --check        # 生成文档与清单一致
python -m unittest discover -s tests -v         # 25 passed
python tools/lightcpp_pm.py resolve cpp-extension-pack
```

在「基线 1931264 + 5 个补丁」的干净副本上复跑，以上四项全部通过。

## 六、边界（没有做的事）

- **没有写插件宿主运行时**：加载器、贡献点注册、UI 宿主属于 v0.2，规划里也没定义宿主接口，硬编会变成拍脑袋。
- **没有写构建/调试的真实适配器**：本阶段只交付"生态模型 + 校验工具"，CMake/Ninja/clangd 闭环在 v0.3、GDB/LLDB DAP 在 v0.4。
- **未推送任何远端改动**：GitHub 侧零变更，你自己 push 时不会冲突。
