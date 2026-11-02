# 插件开发指南

## 1. 新增一个插件

1. 建目录 `plugins/<id>/`，`<id>` 用小写字母、数字与连字符，且必须与清单里的 `id` 一致。
2. 写 `plugin.json`，最少包含：

```json
{
  "$schema": "../../schemas/plugin.schema.json",
  "id": "my-plugin",
  "name": "My Plugin",
  "nameZh": "我的插件",
  "version": "0.1.0",
  "publisher": "lightcpp",
  "kind": "plugin",
  "layer": "build-system",
  "description": "一句话说明这个插件解决什么问题。",
  "dependencies": { "cpp-base": "^0.1.0", "build-core": "^0.1.0" },
  "capabilitySets": ["build.exec"],
  "activation": ["workspaceContains:foo.build"],
  "contributes": { "buildSystems": ["foo"] }
}
```

3. 如果它带来规划里没有的能力，先在 `catalog/features.json` 里加功能集与功能条目，
   再在 `capabilitySets` 里引用 —— 不允许先写插件、后补功能登记。
4. 跑 `python tools/lightcpp_pm.py validate`、`python tools/lightcpp_pm.py docs`、`python -m unittest discover -s tests`。
5. 需要进入扩展包时，同步修改 `catalog/invariants.json` 里对应的 `packMembers` 规则（成员是锁定集合，不是"至少包含"）。

## 2. 字段速查

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `id` | ✅ | 与目录名一致 |
| `name` / `nameZh` | ✅ | 英文名 / 中文名 |
| `version` | ✅ | `x.y.z` |
| `publisher` | ✅ | 发布者 |
| `kind` | ✅ | `plugin` 或 `pack` |
| `layer` | ✅ | `core` / `language` / `build-framework` / `build-system` / `debugger` / `tooling` / `pack` |
| `description` | ✅ | 至少 8 个字符 |
| `platforms` | | `linux` / `darwin` / `win32`，缺省表示全平台 |
| `dependencies` | | 硬依赖，值支持 `*`、`x.y.z`、`^x.y.z`、`~x.y.z`、`>=x.y.z` |
| `recommends` | | `{ "<id>": { "version": "...", "reason": "...", "autoInstall": true } }` |
| `capabilitySets` | | 引用 `catalog/features.json` 中定义的功能集 |
| `activation` | | `onLanguage:cpp` / `workspaceContains:CMakeLists.txt` / `onStartup` |
| `contributes` | | 贡献点，扩展包禁止使用 |
| `tags` | | 自由标签，如 `recommended` / `gnu` / `optional` |

## 3. 版本约束语义

| 写法 | 含义 |
| --- | --- |
| `*` | 任意版本 |
| `0.1.0` | 精确匹配 |
| `^0.1.0` | `>=0.1.0 <0.2.0`（0.x 视为不稳定，锁次版本） |
| `^1.2.0` | `>=1.2.0 <2.0.0` |
| `~0.1.0` | `>=0.1.0 <0.2.0` |
| `>=1.0.0` | 下限约束 |

## 4. 能力的两种归属写法

```json
{
  "id": "debug.symbols",
  "domain": "debug",
  "label": "符号与可视化",
  "plugins": ["gdb", "lldb", "cdb", "vs-debugger"],
  "features": [
    { "id": "debug.symbol.load", "label": "符号加载" },
    { "id": "debug.symbol.pretty", "label": "pretty-printer", "plugins": ["gdb", "lldb"] },
    { "id": "debug.bp.logpoint", "label": "日志断点", "except": ["cdb", "vs-debugger"] }
  ]
}
```

- `plugins`（功能集级）是默认归属；功能级 `plugins` 覆盖默认；
- `except` 从归属里排除，用于如实表达平台差异；
- `status` 取 `required` / `planned` / `known-issue`，非 `required` 必须写 `note`。

## 5. 校验规则要记住的几条

- 必需功能必须有人认领（`E023`）；功能集必须被至少一个插件引用（`E021`）。
- 依赖环会被拒绝（`E020`），安装顺序按依赖拓扑排序。
- 扩展包不得有 `contributes` / `capabilitySets`，且必须至少一个成员（`E017`–`E019`）。
- 同一 `exclusiveGroup` 至少两个成员，engine 不得重名（`E024` / `E025`）。
- 规划不变量（`catalog/invariants.json`）任何一条不满足都会失败（`E100`–`E105`）。

## 6. 文档

`docs/PLUGINS.md` 与 `docs/FEATURES.md` 是**生成产物**，不要手改。改完清单后运行：

```bash
python tools/lightcpp_pm.py docs
```

CI 会跑 `docs --check`，文档与清单不一致就失败。
