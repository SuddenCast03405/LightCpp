#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LightCpp 插件生态管理工具（lightcpp-pm）。

把仓库里的插件清单（plugins/*/plugin.json）与功能目录（catalog/features.json）
当成单一事实源，提供校验、依赖解析、安装顺序、依赖图与文档生成能力。

这是 C++ 版（core/src/cli_main.cpp）的等价实现，不是玩具：
全部 10 个子命令、参数、输出格式、stderr 与退出码逐字节一致，
`scripts/cli_parity.sh` 会对拍 161 组调用。`host` 用 ctypes 直接调插件
的 C ABI（lightcpp_plugin_entry），因此原生加载这条路径也有两份独立实现
互为交叉验证。

用法::

    python tools/lightcpp_pm.py validate
    python tools/lightcpp_pm.py resolve cpp-extension-pack
    python tools/lightcpp_pm.py graph --format mermaid
    python tools/lightcpp_pm.py matrix
    python tools/lightcpp_pm.py coverage
    python tools/lightcpp_pm.py engines
    python tools/lightcpp_pm.py info clang-core
    python tools/lightcpp_pm.py docs [--check] [--out <目录>]
    python tools/lightcpp_pm.py host [--require-native] [--only a,b]
"""
from __future__ import annotations

import argparse
import ctypes
import json
import re
import sys
import unicodedata
from collections import OrderedDict, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

LAYERS = ["core", "language", "build-framework", "build-system",
          "debugger", "tooling", "pack"]
LAYER_LABELS = {
    "core": "核心基础层",
    "language": "语言服务器与语言支持",
    "build-framework": "项目与构建系统核心框架",
    "build-system": "具体项目 / 构建系统支持",
    "debugger": "调试器",
    "tooling": "工具与集成",
    "pack": "扩展包",
}
KINDS = ["plugin", "pack"]
PLATFORMS = ["linux", "darwin", "win32"]
REQUIRED_FIELDS = ["id", "name", "nameZh", "version", "publisher", "kind", "layer", "description"]
CURRENT_PLATFORM = {"win32": "win32", "darwin": "darwin"}.get(sys.platform, "linux")

OPTIONAL_FIELDS = ["platforms", "engines", "dependencies", "recommends", "capabilitySets",
                   "activation", "contributes", "tags"]
# contributes 下各键的期望形状，用于类型规整
CONTRIBUTE_SHAPES = {
    "commands": "list", "languages": "list", "fileAssociations": "list",
    "buildSystems": "list", "debuggers": "list", "toolchain": "list",
    "compilers": "list", "settings": "list", "knownIssues": "list",
    "packageManager": "object", "testFramework": "object", "buildTaskEngine": "object",
    "debugSupport": "object", "scm": "object", "lsp": "object",
}
# 每层允许的成员；None 表示不限制。防止插件改个字段就冒充核心层。
LAYER_MEMBERS = {
    "core": {"cpp-base"},
    "build-framework": {"build-core"},
}
ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+$")


# --------------------------------------------------------------- 文本工具
def dwidth(text: str) -> int:
    """按终端显示宽度计算字符串宽度（CJK 记 2）。"""
    return sum(2 if unicodedata.east_asian_width(ch) in "WF" else 1 for ch in str(text))


def pad(text: str, width: int) -> str:
    text = str(text)
    return text + " " * max(0, width - dwidth(text))


def escape_cell(text) -> str:
    """Markdown 表格单元转义：| 会拆列，换行会断开表格。"""
    return str(text).replace("|", "\\|").replace("\n", " ")


def render_table(headers, rows) -> str:
    headers = [escape_cell(h) for h in headers]
    rows = [[escape_cell(c) for c in row] for row in rows]
    widths = [max(dwidth(h), 3) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], dwidth(cell))
    def line(cells):
        return "| " + " | ".join(pad(c, widths[i]) for i, c in enumerate(cells)) + " |"
    out = [line(headers), "|" + "|".join("-" * (w + 2) for w in widths) + "|"]
    out += [line(r) for r in rows]
    return "\n".join(out)


def render_long_table(headers, rows) -> str:
    """宽表用：先整体转义，再交给 render_table 排版。"""
    return render_table(headers, rows)


# --------------------------------------------------------------- 数据加载
class Normalized:
    """规整结果：data 为可安全消费的字典，bad 为类型错误说明。"""

    __slots__ = ("data", "bad")

    def __init__(self, data: dict, bad: list[str]):
        self.data = data
        self.bad = bad


def normalize_plugin(raw: dict) -> Normalized:
    """把插件清单规整成稳定形状。

    校验器本身不能因为"清单写错"而崩溃——那正是它要报告的情况。
    所有"应为对象/数组/字符串"的字段在这里统一纠正，并把原始类型错误记录下来，
    由 validate 转成 E001 输出。
    """
    data = dict(raw)
    bad: list[str] = []

    for key in ("dependencies", "recommends"):
        value = data.get(key)
        if value is None:
            continue
        if not isinstance(value, dict):
            bad.append(f"{key} 必须是对象，实际是 {type(value).__name__}")
            data[key] = {}
            continue
        cleaned = {}
        for dep, spec in value.items():
            if key == "dependencies":
                if not isinstance(spec, str):
                    bad.append(f"{key}.{dep} 的版本约束必须是字符串")
                    spec = "*"
            elif not isinstance(spec, dict):
                bad.append(f"{key}.{dep} 必须是对象")
                spec = {"version": "*"}
            cleaned[dep] = spec
        data[key] = cleaned

    for key in ("capabilitySets", "activation", "platforms", "tags"):
        value = data.get(key)
        if value is None:
            continue
        if isinstance(value, str):
            bad.append(f"{key} 必须是数组，实际是字符串（是否少写了方括号？）")
            data[key] = [value]
        elif not isinstance(value, list):
            bad.append(f"{key} 必须是数组，实际是 {type(value).__name__}")
            data[key] = []

    for key in ("id", "name", "nameZh", "version", "publisher", "kind", "layer", "description"):
        if key in data and not isinstance(data[key], str):
            bad.append(f"{key} 必须是字符串，实际是 {type(data[key]).__name__}")
            data[key] = str(data[key])

    contributes = data.get("contributes")
    if contributes is not None and not isinstance(contributes, dict):
        bad.append("contributes 必须是对象")
        data["contributes"] = {}
    elif isinstance(contributes, dict):
        fixed = dict(contributes)
        for key, shape in CONTRIBUTE_SHAPES.items():
            if key not in fixed:
                continue
            value = fixed[key]
            if shape == "list" and isinstance(value, str):
                bad.append(f"contributes.{key} 必须是数组，实际是字符串（是否少写了方括号？）")
                fixed[key] = [value]
            elif shape == "list" and not isinstance(value, list):
                bad.append(f"contributes.{key} 必须是数组")
                fixed[key] = []
            elif shape == "object" and not isinstance(value, dict):
                bad.append(f"contributes.{key} 必须是对象")
                fixed[key] = {}
        data["contributes"] = fixed

    return Normalized(data, bad)


class Repo:
    def __init__(self, root: Path = ROOT):
        self.root = Path(root)
        self.plugins: dict[str, dict] = {}
        self.catalog: dict = {}
        self.invariants: list[dict] = []
        self.load_errors: list[str] = []
        self.type_errors: dict[str, list[str]] = {}
        self._load()

    def _load(self):
        for path in sorted((self.root / "plugins").glob("*/plugin.json")):
            rel = path.relative_to(self.root)
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
            except json.JSONDecodeError as exc:
                self.load_errors.append(f"{rel}: JSON 解析失败 {exc}")
                continue
            except OSError as exc:
                self.load_errors.append(f"{rel}: 无法读取 {exc}")
                continue
            if not isinstance(data, dict):
                self.load_errors.append(f"{rel}: 顶层必须是 JSON 对象")
                continue
            n = normalize_plugin(data)
            # 用副本拼接，避免 extend 自引用（列表在迭代中被追加 -> 无限增长直到 OOM）
            issues = [f"{rel}: {m}" for m in n.bad]
            self.type_errors[path.parent.name] = issues
            plugin = n.data
            plugin["_dir"] = path.parent.name
            plugin["_path"] = path
            pid = plugin.get("id") if isinstance(plugin.get("id"), str) else path.parent.name
            if pid in self.plugins:
                self.load_errors.append(
                    f"{rel}: 与 plugins/{self.plugins[pid]['_dir']} 重复声明了同一个 id {pid}")
                continue
            self.plugins[pid] = plugin
        self.catalog = self._load_catalog(self.root / "catalog" / "features.json")
        inv_path = self.root / "catalog" / "invariants.json"
        raw_inv = json.loads(inv_path.read_text(encoding="utf-8"))
        rules = raw_inv.get("rules", []) if isinstance(raw_inv, dict) else []
        self.invariants = [r for r in rules if isinstance(r, dict)]

    def _load_catalog(self, path: Path) -> dict:
        """容错加载：结构不对时退化成空目录并记录错误，避免整个 CLI 崩掉。"""
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            self.load_errors.append(f"{path.name}: 无法解析 {exc}")
            return {"domains": [], "sets": []}
        catalog = raw if isinstance(raw, dict) else {}
        domains = [d for d in catalog.get("domains", []) if isinstance(d, dict) and "id" in d]
        sets = [s for s in catalog.get("sets", []) if isinstance(s, dict) and "id" in s]
        for s in sets:
            feats = [f for f in s.get("features", []) if isinstance(f, dict) and "id" in f]
            if len(feats) != len(s.get("features", [])):
                self.load_errors.append(f"功能集 {s['id']}: 存在缺少 id 的功能条目")
            s["features"] = feats
            if not isinstance(s.get("plugins", []), list):
                self.load_errors.append(f"功能集 {s['id']}: plugins 必须是数组")
                s["plugins"] = []
            for f in feats:
                for key in ("plugins", "except"):
                    if key in f and not isinstance(f[key], list):
                        self.load_errors.append(f"功能 {f['id']}: {key} 必须是数组")
                        f.pop(key)
        return {"domains": domains, "sets": sets}

    # -- 查询 ---------------------------------------------------------
    def by_layer(self, layer: str) -> list[dict]:
        return [p for p in self.plugins.values() if p.get("layer") == layer]

    def sets(self) -> dict[str, dict]:
        return {s["id"]: s for s in self.catalog["sets"]}

    def features(self):
        for s in self.catalog["sets"]:
            for f in s["features"]:
                yield s, f

    def feature_owners(self, s: dict, f: dict) -> list[str]:
        owners = list(f.get("plugins", s.get("plugins", [])))
        blocked = set(f.get("except", []))
        return [p for p in owners if p not in blocked]


# --------------------------------------------------------------- 版本
def parse_version(text: str) -> tuple[int, int, int]:
    if not VERSION_RE.match(text or ""):
        raise ValueError(f"非法版本号: {text!r}")
    parts = tuple(int(x) for x in text.split("."))
    if any(p > 65535 for p in parts):
        raise ValueError(f"版本号段超出范围 (0-65535): {text!r}")
    return parts  # type: ignore[return-value]


def satisfies(version: str, constraint: str) -> bool:
    """支持 *、x.y.z、^x.y.z、~x.y.z、>=x.y.z。"""
    constraint = (constraint or "*").strip()
    if constraint in ("*", "", "latest"):
        return True
    ver = parse_version(version)
    if constraint.startswith("^"):
        base = parse_version(constraint[1:])
        if base[0] > 0:
            return ver >= base and ver[0] == base[0]
        if base[1] > 0:
            return ver >= base and ver[:2] == base[:2]
        return ver == base
    if constraint.startswith("~"):
        base = parse_version(constraint[1:])
        return ver >= base and ver[:2] == base[:2]
    if constraint.startswith(">="):
        return ver >= parse_version(constraint[2:])
    return ver == parse_version(constraint)


# --------------------------------------------------------------- 校验
def validate(repo: Repo, strict: bool = False,
             platform: str | None = None) -> list[tuple[str, str, str]]:
    """返回 [(level, code, message)]，level 为 error / warning。

    platform 为空时取当前运行平台，仅用于 W003 平台可见性提示。
    """
    problems: list[tuple[str, str, str]] = [("error", "LOAD", m) for m in repo.load_errors]
    err = lambda code, msg: problems.append(("error", code, msg))
    warn = lambda code, msg: problems.append(("warning", code, msg))

    catalog_sets = repo.sets()

    for pid, p in repo.plugins.items():
        where = p["_dir"]
        for field in REQUIRED_FIELDS:
            if not p.get(field):
                err("E001", f"{where}: 缺少必填字段 {field}")
        for msg in repo.type_errors.get(where, []):
            err("E001", f"{where}: 字段类型错误 —— {msg}")
        known = set(REQUIRED_FIELDS) | set(OPTIONAL_FIELDS) | {"$schema"}
        for key in p:
            if key not in known and not key.startswith("_"):
                err("E026", f"{where}: 未知字段 {key}（拼写错误？）")
        if p.get("description") and len(p["description"]) < 8:
            err("E027", f"{where}: description 至少 8 个字符")
        if p.get("id") != where:
            err("E002", f"{where}: id ({p.get('id')}) 与目录名不一致")
        if p.get("id") and not ID_RE.match(p["id"]):
            err("E003", f"{where}: id 必须匹配 {ID_RE.pattern}")
        if p.get("kind") not in KINDS:
            err("E004", f"{where}: kind 必须是 {KINDS} 之一")
        if p.get("layer") not in LAYERS:
            err("E005", f"{where}: layer 必须是 {LAYERS} 之一")
        if p.get("kind") == "pack" and p.get("layer") != "pack":
            err("E006", f"{where}: 扩展包的 layer 必须为 pack")
        if p.get("kind") == "plugin" and p.get("layer") == "pack":
            err("E007", f"{where}: 非扩展包不能使用 pack 层级")
        allowed = LAYER_MEMBERS.get(p.get("layer"))
        if allowed is not None and p.get("id") not in allowed:
            err("E028", f"{where}: {p.get('layer')} 层只允许 {sorted(allowed)}")
        if p.get("version") and not VERSION_RE.match(p["version"]):
            err("E008", f"{where}: version 需为 x.y.z")
        for plat in p.get("platforms", []):
            if plat not in PLATFORMS:
                err("E009", f"{where}: 未知平台 {plat}")

        # 依赖
        for dep, constraint in (p.get("dependencies") or {}).items():
            target = repo.plugins.get(dep)
            if target is None:
                err("E010", f"{where}: 依赖的插件不存在: {dep}")
                continue
            try:
                if not satisfies(target.get("version", "0.0.0"), constraint):
                    err("E011", f"{where}: 依赖 {dep} 的版本 {target.get('version')} "
                                f"不满足约束 {constraint}")
            except ValueError as exc:
                err("E012", f"{where}: 依赖 {dep} 的版本约束非法 ({exc})")
            if target.get("kind") == "pack":
                err("E013", f"{where}: 不能把扩展包 {dep} 声明为依赖")

        # 推荐（含自动连带安装）
        for dep, spec in (p.get("recommends") or {}).items():
            if dep not in repo.plugins:
                err("E014", f"{where}: 推荐的插件不存在: {dep}")
                continue
            if isinstance(spec, dict):
                if spec.get("autoInstall") and not spec.get("reason"):
                    err("E015", f"{where}: autoInstall 推荐 {dep} 必须说明 reason")
                if dep in (p.get("dependencies") or {}):
                    warn("W001", f"{where}: {dep} 同时出现在 dependencies 与 recommends 中")

        # 能力集
        for sid in p.get("capabilitySets", []):
            if sid not in catalog_sets:
                err("E016", f"{where}: 未知功能集 {sid}")

        # 扩展包
        if p.get("kind") == "pack":
            if p.get("contributes"):
                err("E017", f"{where}: 扩展包不得声明 contributes（不含业务逻辑）")
            if not (p.get("dependencies") or {}):
                err("E018", f"{where}: 扩展包必须至少包含一个成员")
            if p.get("capabilitySets"):
                err("E019", f"{where}: 扩展包不得声明 capabilitySets")

    # 依赖环
    for cycle in find_cycles(repo):
        err("E020", "存在依赖环: " + " -> ".join(cycle))

    # 功能集引用与覆盖率
    used_sets: set[str] = set()
    for p in repo.plugins.values():
        used_sets.update(p.get("capabilitySets", []))
    for sid in catalog_sets:
        if sid not in used_sets:
            err("E021", f"功能集 {sid} 没有任何插件声明")

    domain_ids = {d["id"] for d in repo.catalog["domains"]}
    seen_sets: dict[str, int] = defaultdict(int)
    for s in repo.catalog["sets"]:
        seen_sets[s["id"]] += 1
    for sid, count in seen_sets.items():
        if count > 1:
            err("E029", f"功能集 id {sid} 重复出现 {count} 次（复制粘贴？）")

    for s in repo.catalog["sets"]:
        if s.get("domain") not in domain_ids:
            err("E030", f"功能集 {s['id']}: 未知功能域 {s.get('domain')!r}")
        if not s["features"]:
            err("E031", f"功能集 {s['id']} 没有任何功能")
        for f in s["features"]:
            for name in f.get("except", []):
                if name not in repo.plugins:
                    err("E032", f"功能 {f['id']}: except 指向不存在的插件 {name}")
            for owner in repo.feature_owners(s, f):
                if owner not in repo.plugins:
                    err("E022", f"功能 {f['id']} 归属的插件不存在: {owner}")
            if f.get("status", "required") == "required" and not repo.feature_owners(s, f):
                err("E023", f"必需功能 {f['id']} 没有归属插件")
            if f.get("status", "required") != "required" and not f.get("note"):
                warn("W002", f"功能 {f['id']} 状态为 {f['status']} 但缺少 note 说明")

    # LSP 互斥组
    groups: dict[str, list[tuple[str, str]]] = defaultdict(list)
    for p in repo.plugins.values():
        lsp = (p.get("contributes") or {}).get("lsp") or {}
        if lsp.get("exclusiveGroup"):
            groups[lsp["exclusiveGroup"]].append((p["id"], lsp.get("engine", "?")))
    for group, members in groups.items():
        if len(members) < 2:
            err("E024", f"互斥组 {group} 只有 {len(members)} 个成员")
        engines = [e for _, e in members]
        if len(set(engines)) != len(engines):
            err("E025", f"互斥组 {group} 内 engine 重名: {engines}")

    # 平台可见性：插件声明了平台，但当前目标平台不在其中，给出提示而非错误。
    target = platform or CURRENT_PLATFORM
    for pid, p in repo.plugins.items():
        declared = p.get("platforms") or []
        if declared and target not in declared:
            warn("W003", f"{pid} 声明平台 [{', '.join(declared)}]，"
                         f"当前目标平台 {target} 不可用（不影响校验结果）")

    # 计划不变量
    problems.extend(check_invariants(repo))

    if not strict:
        # 保持插入顺序（C++ 侧同样原地过滤），只摘掉 W002。
        problems = [p for p in problems if p[0] == "error" or p[1] != "W002"]
    return problems


def find_cycles(repo: Repo) -> list[list[str]]:
    color: dict[str, int] = {}
    stack: list[str] = []
    cycles: list[list[str]] = []

    def visit(node: str):
        color[node] = 1
        stack.append(node)
        for dep in (repo.plugins.get(node, {}).get("dependencies") or {}):
            if dep not in repo.plugins:
                continue
            if color.get(dep, 0) == 1:
                idx = stack.index(dep)
                cycles.append(stack[idx:] + [dep])
            elif color.get(dep, 0) == 0:
                visit(dep)
        stack.pop()
        color[node] = 2

    for pid in repo.plugins:
        if color.get(pid, 0) == 0:
            visit(pid)
    return cycles


def check_invariants(repo: Repo) -> list[tuple[str, str, str]]:
    out: list[tuple[str, str, str]] = []
    for rule in repo.invariants:
        rid, rtype = rule.get("id"), rule.get("type")
        if rtype == "dependency":
            p = repo.plugins.get(rule["plugin"], {})
            for target in rule["on"]:
                if target not in (p.get("dependencies") or {}):
                    out.append(("error", "E100",
                                f"[{rid}] {rule['plugin']} 必须依赖 {target}"))
        elif rtype == "autoInstall":
            p = repo.plugins.get(rule["plugin"], {})
            spec = (p.get("recommends") or {}).get(rule["target"], {})
            if not (isinstance(spec, dict) and spec.get("autoInstall")):
                out.append(("error", "E101",
                            f"[{rid}] {rule['plugin']} 必须自动连带安装 {rule['target']}"))
        elif rtype == "packMembers":
            p = repo.plugins.get(rule["pack"], {})
            actual = set((p.get("dependencies") or {}).keys())
            expected = set(rule["members"])
            if actual != expected:
                missing = sorted(expected - actual)
                extra = sorted(actual - expected)
                out.append(("error", "E102",
                            f"[{rid}] {rule['pack']} 成员不符：缺少 {missing}，多余 {extra}"))
        elif rtype == "layerDependency":
            for p in repo.by_layer(rule["layer"]):
                for target in rule["on"]:
                    if target not in (p.get("dependencies") or {}):
                        out.append(("error", "E103",
                                    f"[{rid}] {p['id']} 必须依赖 {target}"))
        elif rtype == "exclusiveGroup":
            members = {p["id"] for p in repo.plugins.values()
                       if ((p.get("contributes") or {}).get("lsp") or {}).get("exclusiveGroup")
                       == rule["group"]}
            if members != set(rule["members"]):
                out.append(("error", "E104",
                            f"[{rid}] 互斥组 {rule['group']} 成员不符: {sorted(members)}"))
        else:
            out.append(("error", "E105", f"[{rid}] 未知的不变量类型 {rtype}"))
    return out


# --------------------------------------------------------------- 解析
def resolve(repo: Repo, roots: list[str]) -> tuple[OrderedDict, dict[str, str]]:
    """返回 (安装闭包, 来源说明)。闭包按依赖优先排序。"""
    unknown = [r for r in roots if r not in repo.plugins]
    if unknown:
        raise KeyError("未知插件: " + ", ".join(unknown))

    reason: dict[str, str] = {}
    queue: list[str] = []
    for r in roots:
        reason[r] = "直接安装"
        queue.append(r)

    while queue:
        cur = queue.pop(0)
        p = repo.plugins[cur]
        for dep in (p.get("dependencies") or {}):
            if dep not in reason:
                reason[dep] = f"由 {cur} 引入"
                queue.append(dep)
        for dep, spec in (p.get("recommends") or {}).items():
            if dep not in reason and isinstance(spec, dict) and spec.get("autoInstall"):
                reason[dep] = f"自动连带安装（{cur}）"
                queue.append(dep)

    order = topo_sort(repo, list(reason))
    return OrderedDict((pid, repo.plugins[pid]) for pid in order), reason


def topo_sort(repo: Repo, ids: list[str]) -> list[str]:
    result: list[str] = []
    seen: set[str] = set()

    def visit(pid: str, trail: tuple[str, ...]):
        if pid in seen:
            return
        if pid in trail:
            raise ValueError("依赖环: " + " -> ".join(trail + (pid,)))
        for dep in (repo.plugins[pid].get("dependencies") or {}):
            if dep in ids:
                visit(dep, trail + (pid,))
        seen.add(pid)
        result.append(pid)

    for pid in ids:
        visit(pid, ())
    return result


def exclusive_conflicts(repo: Repo, ids) -> list[str]:
    """只列 engine 名（clangd / ccls），与 C++ resolver 的 notes 同构。"""
    groups: dict[str, list[str]] = defaultdict(list)
    for pid in ids:
        lsp = (repo.plugins[pid].get("contributes") or {}).get("lsp") or {}
        if lsp.get("exclusiveGroup"):
            groups[lsp["exclusiveGroup"]].append(lsp.get("engine", ""))
    return [f"互斥组 {group} 包含 " + " / ".join(engines) + "，运行时仅启用其一"
            for group, engines in sorted(groups.items()) if len(engines) > 1]


# --------------------------------------------------------------- 输出
def cmd_validate(repo: Repo, args) -> int:
    problems = validate(repo, strict=args.strict, platform=args.platform)
    errors = [p for p in problems if p[0] == "error"]
    warnings = [p for p in problems if p[0] == "warning"]
    feature_count = sum(len(s["features"]) for s in repo.catalog["sets"])
    if args.json:
        print(json.dumps({
            "root": str(repo.root),
            "plugins": len(repo.plugins),
            "sets": len(repo.catalog["sets"]),
            "features": feature_count,
            "errors": len(errors),
            "warnings": len(warnings),
            "problems": [{"severity": level, "code": code, "message": msg}
                         for level, code, msg in problems],
        }, ensure_ascii=False, indent=2))
        return 1 if errors else 0

    for level, code, msg in problems:
        # 与 C++ 的 Problem::format 逐字符一致：两个空格分隔级别与 [code]。
        print(f"{'error  ' if level == 'error' else 'warning'}  [{code}] {msg}")
    print()
    line = (f"{len(repo.plugins)} 个组件 / {len(repo.catalog['sets'])} 个功能集 / "
            f"{feature_count} 条功能：{len(errors)} 个错误，{len(warnings)} 个警告")
    if not args.strict:
        line += "（非严格模式，W002 已省略；CI 使用 --strict）"
    print(line)
    return 1 if errors else 0


def cmd_resolve(repo: Repo, args) -> int:
    if not args.plugins:
        print("错误: resolve 需要至少一个插件 id", file=sys.stderr)
        return 2
    try:
        closure, reason = resolve(repo, args.plugins)
    except KeyError as exc:
        print(f"错误: {exc.args[0]}", file=sys.stderr)
        return 1
    if args.json:
        # 与 C++ cmdResolve --json 同构：steps[{id,layer,version,origin}] + notes[]。
        print(json.dumps({
            "steps": [{"id": pid, "layer": p["layer"], "version": p["version"],
                       "origin": reason[pid]} for pid, p in closure.items()],
            "notes": exclusive_conflicts(repo, closure),
        }, ensure_ascii=False, indent=2))
        return 0
    rows = []
    for i, (pid, p) in enumerate(closure.items(), 1):
        rows.append([str(i), pid, LAYER_LABELS[p["layer"]], p["version"], reason[pid]])
    print(render_table(["#", "插件", "层级", "版本", "来源"], rows))
    packs = [pid for pid, p in closure.items() if p.get("kind") == "pack"]
    print(f"\n共 {len(closure)} 个组件（{len(packs)} 个扩展包，"
          f"{len(closure) - len(packs)} 个插件）")
    for line in exclusive_conflicts(repo, closure):
        print(f"\n提示：{line}")
    return 0


def cmd_info(repo: Repo, args) -> int:
    p = repo.plugins.get(args.plugin)
    if not p:
        print(f"错误: 未知插件: {args.plugin}", file=sys.stderr)
        return 2
    if args.json:
        # 与 C++ cmdInfo 的 --json 同构：只输出稳定字段，屏蔽 _dir 之类的内部键。
        print(json.dumps({
            "id": p["id"],
            "name": p.get("name", ""),
            "nameZh": p.get("nameZh", ""),
            "version": p["version"],
            "layer": p["layer"],
            "kind": p["kind"],
            "description": p.get("description", ""),
            "dependencies": p.get("dependencies") or {},
            "capabilitySets": p.get("capabilitySets") or [],
            "contributes": p.get("contributes") or {},
        }, ensure_ascii=False, indent=2))
        return 0
    # 文本形态逐字符对齐 C++ cmdInfo：头部、依赖/推荐/互斥组、功能集勾选、贡献点。
    def dump_compact(value) -> str:
        return json.dumps(value, ensure_ascii=False, separators=(",", ":"))

    print(f"{p['id']}（{p['nameZh']}）  {p['version']}  [{p['layer']} / {p['kind']}]")
    print(f"{p['description']}\n")

    deps = p.get("dependencies") or {}
    print("依赖: " + (", ".join(f"{k} {v}" for k, v in deps.items()) if deps else "无"))

    recs = p.get("recommends") or {}
    if recs:
        parts = []
        for rid, rec in recs.items():
            if isinstance(rec, dict):
                parts.append(f"{rid} {rec.get('version', '')}"
                             + ("（自动安装）" if rec.get("autoInstall") else ""))
            else:
                parts.append(f"{rid} {rec}")
        print("推荐: " + ", ".join(parts))
    else:
        print("推荐: 无")

    lsp = (p.get("contributes") or {}).get("lsp")
    group = lsp.get("exclusiveGroup", "") if isinstance(lsp, dict) else ""
    print(f"互斥组: {group or '无'}\n")

    print("功能集:")
    catalog_sets = repo.sets()
    for sid in p.get("capabilitySets") or []:
        s = catalog_sets.get(sid)
        if s is None:
            print(f"  - {sid}（未在 catalog 中定义）")
            continue
        print(f"  - {sid}（{s['label']}）")
        for f in s["features"]:
            mine = p["id"] in repo.feature_owners(s, f)
            print(f"      {'[x] ' if mine else '[ ] '}{f['id']}  {f['label']}")

    contributes = p.get("contributes") or {}
    if not contributes:
        print("\n贡献点: 无")
    else:
        print("\n贡献点:")
        for key, value in contributes.items():
            print(f"  - {key}: {dump_compact(value)}")
    return 0


def _select_plugins(repo: Repo, layer: str | None, by_layer: bool = True):
    """按 C++ 的命令挑选插件，并复刻它的排序。

    - `graph` 先按层级顺序、层内再按 id 排（C++ 用的 stable_sort + layerOrder）；
    - `list` 只按 id 排（C++ 直接遍历 load 顺序，即目录名字典序）。

    层级非法时返回 (None, 错误消息)，调用方原样打印并返回 1 —— 这样
    `list --layer test` 与 C++ 的 `错误: 未知层级: test` 完全一致。
    """
    if layer is not None and layer not in LAYERS:
        return None, f"未知层级: {layer}"
    chosen = [p for p in repo.plugins.values() if layer is None or p["layer"] == layer]
    if by_layer:
        chosen.sort(key=lambda p: (LAYERS.index(p["layer"]), p["id"]))
    else:
        chosen.sort(key=lambda p: p["id"])
    return chosen, None


def cmd_list(repo: Repo, args) -> int:
    chosen, error = _select_plugins(repo, args.layer, by_layer=False)
    if error:
        print(f"错误: {error}", file=sys.stderr)
        return 2
    rows = [[p["id"], p["nameZh"], p["layer"], p["version"]] for p in chosen]
    print(render_table(["插件", "名称", "层级", "版本"], rows))
    return 0


# ------------------------------------------------------- 原生宿主（ctypes）
API_VERSION = 1


class _PluginInfo(ctypes.Structure):
    _fields_ = [("apiVersion", ctypes.c_uint32),
                ("id", ctypes.c_char_p),
                ("version", ctypes.c_char_p),
                ("activate", ctypes.c_void_p),
                ("deactivate", ctypes.c_void_p)]


class _Api(ctypes.Structure):
    _fields_ = [("apiVersion", ctypes.c_uint32),
                ("host", ctypes.c_void_p),
                ("registerContribution", ctypes.c_void_p),
                ("log", ctypes.c_void_p)]


def _library_name(pid: str) -> str:
    """与 C++ Host::libraryName 一致：`cpp-base` -> `libcpp_base.so`。"""
    if sys.platform == "win32":
        return pid.replace("-", "_") + ".dll"
    if sys.platform == "darwin":
        return "lib" + pid.replace("-", "_") + ".dylib"
    return "lib" + pid.replace("-", "_") + ".so"


def _topological_order(repo: Repo, ids: list[str]) -> tuple[list[str], str]:
    """复刻 C++ topologicalOrder：DFS 后序，候选按传入顺序（字典序）访问。"""
    wanted = set(ids)
    done: set[str] = set()
    result: list[str] = []
    trail: list[str] = []

    def visit(node: str) -> str:
        if node in done:
            return ""
        if node in trail:
            return "依赖环: " + " -> ".join(trail + [node])
        trail.append(node)
        for dep in (repo.plugins.get(node, {}).get("dependencies") or {}):
            if dep in wanted:
                err = visit(dep)
                if err:
                    return err
        trail.pop()
        if node not in done:
            done.add(node)
            result.append(node)
        return ""

    for node in ids:
        err = visit(node)
        if err:
            return [], err
    return result, ""


def cmd_host(repo: Repo, args) -> int:
    """加载原生插件并汇报状态。

    与 C++ core/src/host.cpp 同构：同样的库查找顺序、同样的状态措辞、同样的
    ABI 校验（apiVersion / 清单 id 一致性）。用 ctypes 直接调 C ABI，
    因此这条命令在两个实现里都能跑，也顺带成了 ABI 的第二份交叉验证。
    """
    require_native = getattr(args, "require_native", False)
    lib_dir = getattr(args, "lib_dir", None) or str(repo.root / "core" / "build" / "bin" / "plugins")
    only = [s for s in (getattr(args, "only", "") or "").replace(" ", "").split(",") if s]

    candidates = [pid for pid, p in repo.plugins.items()
                  if p.get("kind") != "pack" and (not only or pid in only)]
    order, order_error = _topological_order(repo, candidates)
    if order_error:
        print(f"错误: {order_error}", file=sys.stderr)
        return 1

    states = []          # [{id, version, status, contributions}]
    registered: list[tuple[str, str]] = []   # [(type, id)]，按注册顺序
    errors: list[str] = []
    warnings: list[str] = []
    handles = []         # 保活：库与回调一旦回收，激活的插件就成了悬空指针

    def register(host, plugin_id, type_name, contrib_id):
        registered.append((type_name.decode(), contrib_id.decode()))
        return 0

    register_cb = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p,
                                   ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p)(register)
    log_cb = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p)(
        lambda host, level, message: None)

    native_types = ("buildSystems", "debuggers", "lsp", "testFramework",
                    "packageManager", "testExplorer")

    for pid in order:
        p = repo.plugins[pid]
        plugin_dir = repo.root / "plugins" / p["_dir"]
        name = _library_name(pid)
        found = None
        for cand in (plugin_dir / name, plugin_dir / "lib" / name,
                     plugin_dir / "bin" / name, Path(lib_dir) / name):
            if cand.is_file():
                found = cand
                break

        native_required = any((p.get("contributes") or {}).get(k) for k in native_types)
        if found is None:
            if native_required and require_native:
                errors.append(f"{pid} 声明了 原生贡献点，但未找到实现库 {name}")
                status = "缺少实现库"
            elif native_required:
                status = "缺少实现库（当前为清单级自检）"
            else:
                status = "纯清单插件，无原生实现"
            states.append({"id": pid, "version": p["version"], "status": status, "n": 0})
            continue

        try:
            lib = ctypes.CDLL(str(found), mode=ctypes.RTLD_LOCAL)
            entry = lib.lightcpp_plugin_entry
            entry.restype = ctypes.POINTER(_PluginInfo)
            info = entry().contents
        except (OSError, AttributeError) as exc:
            errors.append(f"{pid} 加载失败：{exc}")
            states.append({"id": pid, "version": p["version"], "status": "dlopen 失败", "n": 0})
            continue

        if info.apiVersion != API_VERSION:
            errors.append(f"{pid} 的 ABI 版本不匹配（期望 {API_VERSION}）")
            states.append({"id": pid, "version": p["version"], "status": "ABI 不匹配", "n": 0})
            continue
        declared = info.id.decode() if info.id else ""
        if declared and declared != pid:
            errors.append(f"{pid} 的实现自称 {declared}，与清单不一致")
            states.append({"id": pid, "version": p["version"],
                           "status": "实现与清单 id 不一致", "n": 0})
            continue

        api = _Api()
        api.apiVersion = API_VERSION
        api.host = None
        api.registerContribution = ctypes.cast(register_cb, ctypes.c_void_p).value
        api.log = ctypes.cast(log_cb, ctypes.c_void_p).value

        before = len(registered)
        rc = 0
        if info.activate:
            activate = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(_Api))(info.activate)
            rc = activate(ctypes.byref(api))
        if rc != 0:
            errors.append(f"{pid} 激活返回错误码 {rc}")
            states.append({"id": pid, "version": p["version"], "status": "activate 失败", "n": 0})
            continue

        handles.append(lib)
        states.append({"id": pid, "version": p["version"], "status": "已激活",
                       "n": len(registered) - before})

    # 互斥组：同组内多个引擎同时激活时给告警。
    group_engine: dict[str, str] = {}
    for pid, p in repo.plugins.items():
        lsp = (p.get("contributes") or {}).get("lsp") or {}
        group = lsp.get("exclusiveGroup") or ""
        if not group:
            continue
        if not any(s["id"] == pid and s["status"] == "已激活" for s in states):
            continue
        engine = lsp.get("engine", "")
        if group not in group_engine:
            group_engine[group] = engine
        elif group_engine[group] != engine:
            warnings.append(f"互斥组 {group} 中 {pid}（{engine}）不会被启用，"
                            f"当前使用 {group_engine[group]}")

    rows = [[s["id"], s["version"], s["status"], str(s["n"])] for s in states]
    print(render_table(["插件", "版本", "状态", "注册贡献点"], rows))

    for type_name in ("buildSystems", "debuggers", "lsp", "testFramework"):
        ids = [cid for t_name, cid in registered if t_name == type_name]
        if ids:
            print(f"\n{type_name}: " + ", ".join(ids))
    for warning in warnings:
        print(f"\n提示：{warning}")
    for error in errors:
        print(f"错误: {error}", file=sys.stderr)

    missing = [s["id"] for s in states if s["status"] != "已激活"]
    if missing:
        print(f"\n未激活（{len(missing)}）：" + ", ".join(missing))
    activated = sum(1 for s in states if s["status"] == "已激活")
    tail = "（严格：缺少原生实现即失败）" if require_native else "（清单级自检模式）"
    print(f"\n已激活 {activated} / {len(states)} 个插件。{tail}")
    return 0 if not errors else 1


def cmd_engines(repo: Repo, args) -> int:
    groups: dict[str, list[tuple[str, str]]] = defaultdict(list)
    for p in repo.plugins.values():
        lsp = (p.get("contributes") or {}).get("lsp") or {}
        if lsp.get("exclusiveGroup"):
            groups[lsp["exclusiveGroup"]].append((p["id"], lsp.get("engine", "?")))
    if not groups:
        print("没有声明互斥引擎组")
        return 0
    for group, members in groups.items():
        print(f"互斥组 {group}:")
        for pid, engine in members:
            print(f"  - {pid} -> {engine}")
        print("  同一项目只启用其中一个，另一个保持安装但处于停用状态。")
    return 0


def cmd_matrix(repo: Repo, args) -> int:
    rows = []
    for layer in LAYERS:
        for p in sorted(repo.by_layer(layer), key=lambda x: x["id"]):
            rows.append([p["id"], p["nameZh"], str(len(p.get("dependencies") or {})),
                         str(len(p.get("capabilitySets", []))),
                         ",".join(p.get("platforms", ["all"])) or "all"])
    if args.json:
        print(json.dumps([dict(zip(["id", "name", "deps", "sets", "platforms"], r))
                          for r in rows], ensure_ascii=False, indent=2))
        return 0
    print(render_table(["插件", "名称", "依赖数", "功能集", "平台"], rows))
    return 0


def cmd_coverage(repo: Repo, args) -> int:
    stats: dict[str, dict[str, int]] = defaultdict(lambda: defaultdict(int))
    for s, f in repo.features():
        stats[s["domain"]][f.get("status", "required")] += 1
    labels = {d["id"]: d["label"] for d in repo.catalog["domains"]}
    rows = []
    for domain in labels:
        d = stats.get(domain, {})
        total = sum(d.values())
        rows.append([labels[domain], str(total), str(d.get("required", 0)),
                     str(d.get("planned", 0)), str(d.get("known-issue", 0))])
    print(render_table(["功能域", "合计", "已实现目标", "规划中", "已知问题"], rows))
    total = sum(len(s["features"]) for s in repo.catalog["sets"])
    print(f"\n合计 {total} 条功能。规划中 / 已知问题项不计入 v0.2 验收范围；"
          "其余条目的「已覆盖」指有归属插件（清单声明），不代表引擎已实现。")
    return 0


def cmd_graph(repo: Repo, args) -> int:
    chosen, error = _select_plugins(repo, args.layer)
    if error:
        print(f"错误: {error}", file=sys.stderr)
        return 2
    ids = {p["id"] for p in chosen}

    if args.format == "text":
        # 与 C++ 版一致：按层级打印依赖表，而不是图。终端里看得更清楚。
        for layer in LAYERS:
            rows = [[p["id"], p["nameZh"],
                     ", ".join((p.get("dependencies") or {})) or "—"]
                    for p in chosen if p["layer"] == layer]
            if rows:
                print(LAYER_LABELS[layer])
                print(render_table(["插件", "名称", "依赖"], rows))
                print()
        return 0

    if args.format == "dot":
        print("digraph lightcpp {")
        print("  rankdir=LR; node [shape=box];")
        for p in chosen:
            print(f'  "{p["id"]}";')
        for p in chosen:
            for dep in (p.get("dependencies") or {}):
                if dep in ids:
                    print(f'  "{p["id"]}" -> "{dep}";')
        print("}")
        return 0

    print("```mermaid")
    print("graph LR")
    for layer in LAYERS:
        members = [p for p in chosen if p["layer"] == layer]
        if not members:
            continue
        print(f'  subgraph {layer}["{LAYER_LABELS[layer]}"]')
        for p in members:
            print(f'    {p["id"]}["{p["nameZh"]}"]')
        print("  end")
    for p in chosen:
        for dep in (p.get("dependencies") or {}):
            if dep in ids:
                print(f'  {p["id"]} --> {dep}')
    print("```")
    return 0


# --------------------------------------------------------------- 文档生成
DOC_HEADER = "<!-- 由 lightcpp-pm docs 生成，请勿手工编辑 -->\n"


def build_plugins_doc(repo: Repo) -> str:
    out = [DOC_HEADER, "# 插件清单", "",
           f"共 **{len(repo.plugins)}** 个组件："
           f"{sum(1 for p in repo.plugins.values() if p['kind'] == 'plugin')} 个插件，"
           f"{sum(1 for p in repo.plugins.values() if p['kind'] == 'pack')} 个扩展包。",
           ""]
    for layer in LAYERS:
        members = sorted(repo.by_layer(layer), key=lambda x: x["id"])
        if not members:
            continue
        out += [f"## {LAYER_LABELS[layer]}", ""]
        for p in members:
            out += [f"### `{p['id']}` — {p['nameZh']}", "", p["description"], ""]
            deps = ", ".join(f"`{d}`" for d in (p.get("dependencies") or {}))
            sets = ", ".join(f"`{s}`" for s in p.get("capabilitySets", []))
            out.append(f"- 依赖：{deps or '无'}")
            if sets:
                out.append(f"- 功能集：{sets}")
            out.append(f"- 平台：{', '.join(p.get('platforms', [])) or '全平台'}")
            recs = p.get("recommends") or {}
            if recs:
                out.append("- 推荐：" + "；".join(
                    f"`{k}`（{'自动安装' if isinstance(v, dict) and v.get('autoInstall') else '建议安装'}）"
                    for k, v in recs.items()))
            out.append("")
    return "\n".join(out) + "\n"


def build_features_doc(repo: Repo) -> str:
    labels = {d["id"]: d["label"] for d in repo.catalog["domains"]}
    out = [DOC_HEADER, "# 功能目录", "",
           "规划中的每一条能力都登记在此，并用 `plugins` / `except` 标注归属，"
           "`lightcpp-pm validate` 会强制要求所有 `required` 功能有归属。", ""]
    total = 0
    for domain_id, domain_label in labels.items():
        sets = [s for s in repo.catalog["sets"] if s["domain"] == domain_id]
        if not sets:
            continue
        out += [f"## {domain_label}", ""]
        rows = []
        for s in sets:
            for f in s["features"]:
                total += 1
                owners = repo.feature_owners(s, f)
                status = f.get("status", "required")
                rows.append([f"`{f['id']}`", f["label"],
                             "规划中" if status == "planned" else
                             ("已知问题" if status == "known-issue" else "目标"),
                             ", ".join(f"`{o}`" for o in owners) or "—"])
        out += [render_table(["功能 ID", "说明", "状态", "归属插件"], rows).rstrip(), ""]
    out += [f"合计 **{total}** 条功能。"]
    return "\n".join(out) + "\n"


def cmd_docs(repo: Repo, args) -> int:
    out_dir = Path(args.out) if args.out else repo.root / "docs"
    targets = {
        out_dir / "PLUGINS.md": build_plugins_doc(repo),
        out_dir / "FEATURES.md": build_features_doc(repo),
    }
    failed = False
    for path, content in targets.items():
        if args.check:
            if not path.exists():
                print(f"文档与清单不一致：缺少文件 {path.name}", file=sys.stderr)
                failed = True
                continue
            if path.read_text(encoding="utf-8") != content:
                print(f"文档与清单不一致：{path.name} 与清单不一致，请运行 `lightcpp-pm docs`",
                      file=sys.stderr)
                failed = True
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
            print(f"已生成 {path}")
    if args.check and not failed:
        print("文档与清单一致。")
    return 1 if failed else 0


# --------------------------------------------------------------- 入口
USAGE_TEXT = """lightcpp-pm —— LightCpp 插件生态工具链

用法:
  lightcpp-pm <命令> [参数] [--root <目录>]

命令:
  validate [--strict] [--platform <linux|darwin|win32>] [--json]
                校验清单、依赖、功能覆盖与规划不变量
  resolve <插件>... [--json]
                解析安装闭包与安装顺序（含 autoInstall 连带安装）
  info <插件> [--json]
                查看单个插件的依赖、推荐、功能集与贡献点
  graph [--layer <层级>] [--format <text|mermaid|dot>]
                输出依赖图
  matrix [--json]
                插件 x 能力矩阵
  coverage      按功能域统计功能状态
  engines       互斥引擎组（clangd / ccls）
  docs [--check] [--out <目录>]
                生成 docs/PLUGINS.md 与 docs/FEATURES.md；--check 只比对
  host [--require-native] [--only <插件,插件>] [--lib-dir <目录>]
                加载插件宿主：读清单、按依赖顺序加载原生库、注册贡献点
  list [--layer <层级>]
                列出全部组件

全局参数:
  --root <目录>   仓库根目录，默认从当前目录向上查找
  --version, -v   打印版本
  --help, -h      显示本帮助
"""

COMMANDS = ["validate", "resolve", "info", "engines", "matrix", "coverage",
            "graph", "list", "host", "docs"]

# 版本只在 CMakeLists.txt 的 PROJECT_VERSION 里定义一次；这里跟随它，由
# scripts/cli_parity.sh 与 C++ 的 `--version` 输出对拍来保证不漂。
VERSION = "0.2.0"


def print_usage(stream=None) -> None:
    print(USAGE_TEXT, file=stream or sys.stdout, end="")


def main(argv=None) -> int:
    raw = list(sys.argv[1:] if argv is None else argv)

    # 全局开关先于子命令：C++ 版把 -h/--help、-v/--version 当成"任意位置都生效"，
    # 且 --version 优先于 --help（脚本里常把两者一起写）。
    if "--version" in raw or "-v" in raw or (raw and raw[0] == "version"):
        print(f"lightcpp-pm {VERSION}")
        return 0
    if "--help" in raw or "-h" in raw:
        print_usage()
        return 0
    if not raw:
        print_usage()
        return 1
    if raw[0] not in COMMANDS:
        print(f"未知命令: {raw[0]}\n", file=sys.stderr)
        print_usage()
        return 2

    parser = argparse.ArgumentParser(
        prog="lightcpp-pm", add_help=False,
        description="LightCpp 插件生态管理工具")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("validate", help="校验插件清单、依赖关系与功能覆盖")
    p.add_argument("--strict", action="store_true", help="把提示类警告视为问题")
    p.add_argument("--platform", choices=["linux", "darwin", "win32"],
                   help="目标平台（默认取当前运行平台），仅影响 W003 提示")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_validate)

    p = sub.add_parser("resolve", help="解析安装闭包与安装顺序")
    p.add_argument("plugins", nargs="+")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_resolve)

    p = sub.add_parser("info", help="查看单个插件详情")
    p.add_argument("plugin")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("engines", help="列出互斥引擎组（clangd / ccls）")
    p.set_defaults(func=cmd_engines)

    p = sub.add_parser("matrix", help="输出插件矩阵")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_matrix)

    p = sub.add_parser("coverage", help="功能覆盖统计")
    p.set_defaults(func=cmd_coverage)

    p = sub.add_parser("graph", help="输出依赖图")
    p.add_argument("--format", choices=["text", "mermaid", "dot"], default="text")
    p.add_argument("--layer")
    p.set_defaults(func=cmd_graph)

    p = sub.add_parser("list", help="列出全部组件")
    p.add_argument("--layer")
    p.set_defaults(func=cmd_list)

    p = sub.add_parser("host", help="加载原生插件并汇报状态")
    p.add_argument("--require-native", action="store_true",
                   help="缺少原生实现即视为失败")
    p.add_argument("--only", default="", help="只加载列出的插件（逗号分隔）")
    p.add_argument("--lib-dir", default="", help="实现库所在目录")
    p.set_defaults(func=cmd_host)

    p = sub.add_parser("docs", help="生成或检查 docs/PLUGINS.md 与 docs/FEATURES.md")
    p.add_argument("--check", action="store_true")
    p.add_argument("--out", default="docs", help="输出目录（默认 docs/）")
    p.set_defaults(func=cmd_docs)

    args = parser.parse_args(argv)
    repo = Repo()
    try:
        return args.func(repo, args)
    except KeyError as exc:
        print(str(exc).strip("'\""), file=sys.stderr)
        return 2
    except ValueError as exc:
        print(f"解析失败: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
