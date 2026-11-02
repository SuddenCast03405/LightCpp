#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LightCpp 插件生态管理工具（lightcpp-pm）。

把仓库里的插件清单（plugins/*/plugin.json）与功能目录（catalog/features.json）
当成单一事实源，提供校验、依赖解析、安装顺序、依赖图与文档生成能力。

用法::

    python tools/lightcpp_pm.py validate
    python tools/lightcpp_pm.py resolve cpp-extension-pack
    python tools/lightcpp_pm.py graph --format mermaid
    python tools/lightcpp_pm.py matrix
    python tools/lightcpp_pm.py coverage
    python tools/lightcpp_pm.py engines
    python tools/lightcpp_pm.py info clang-core
    python tools/lightcpp_pm.py docs [--check]
"""
from __future__ import annotations

import argparse
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
ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")
VERSION_RE = re.compile(r"^\d+\.\d+\.\d+$")


# --------------------------------------------------------------- 文本工具
def dwidth(text: str) -> int:
    """按终端显示宽度计算字符串宽度（CJK 记 2）。"""
    return sum(2 if unicodedata.east_asian_width(ch) in "WF" else 1 for ch in str(text))


def pad(text: str, width: int) -> str:
    text = str(text)
    return text + " " * max(0, width - dwidth(text))


def render_table(headers, rows) -> str:
    widths = [dwidth(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], dwidth(cell))
    def line(cells):
        return "| " + " | ".join(pad(c, widths[i]) for i, c in enumerate(cells)) + " |"
    out = [line(headers), "|" + "|".join("-" * (w + 2) for w in widths) + "|"]
    out += [line(r) for r in rows]
    return "\n".join(out)


# --------------------------------------------------------------- 数据加载
class Repo:
    def __init__(self, root: Path = ROOT):
        self.root = Path(root)
        self.plugins: dict[str, dict] = {}
        self.catalog: dict = {}
        self.invariants: list[dict] = []
        self.load_errors: list[str] = []
        self._load()

    def _load(self):
        for path in sorted((self.root / "plugins").glob("*/plugin.json")):
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
            except json.JSONDecodeError as exc:  # pragma: no cover
                self.load_errors.append(f"{path.relative_to(self.root)}: JSON 解析失败 {exc}")
                continue
            data["_dir"] = path.parent.name
            data["_path"] = path
            self.plugins[data.get("id", path.parent.name)] = data
        catalog_path = self.root / "catalog" / "features.json"
        self.catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
        inv_path = self.root / "catalog" / "invariants.json"
        self.invariants = json.loads(inv_path.read_text(encoding="utf-8")).get("rules", [])

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
    return tuple(int(x) for x in text.split("."))  # type: ignore[return-value]


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
def validate(repo: Repo, strict: bool = False) -> list[tuple[str, str, str]]:
    """返回 [(level, code, message)]，level 为 error / warning。"""
    problems: list[tuple[str, str, str]] = [("error", "LOAD", m) for m in repo.load_errors]
    err = lambda code, msg: problems.append(("error", code, msg))
    warn = lambda code, msg: problems.append(("warning", code, msg))

    catalog_sets = repo.sets()

    for pid, p in repo.plugins.items():
        where = p["_dir"]
        for field in REQUIRED_FIELDS:
            if not p.get(field):
                err("E001", f"{where}: 缺少必填字段 {field}")
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
    for s in repo.catalog["sets"]:
        for f in s["features"]:
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

    # 计划不变量
    problems.extend(check_invariants(repo))

    if not strict:
        problems = [p for p in problems if p[0] == "error"] + \
                   [p for p in problems if p[0] == "warning" and p[1] != "W002"]
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
    groups: dict[str, list[str]] = defaultdict(list)
    for pid in ids:
        lsp = (repo.plugins[pid].get("contributes") or {}).get("lsp") or {}
        if lsp.get("exclusiveGroup"):
            groups[lsp["exclusiveGroup"]].append(f"{pid} ({lsp.get('engine')})")
    return [f"互斥组 {g}: " + " / ".join(members) + " —— 同时安装，运行时仅启用其一"
            for g, members in groups.items() if len(members) > 1]


# --------------------------------------------------------------- 输出
def cmd_validate(repo: Repo, args) -> int:
    problems = validate(repo, strict=args.strict)
    errors = [p for p in problems if p[0] == "error"]
    warnings = [p for p in problems if p[0] == "warning"]
    for level, code, msg in problems:
        print(f"{'✗' if level == 'error' else '!'} [{code}] {msg}")
    print()
    print(f"插件 {len(repo.plugins)} 个 · 功能集 {len(repo.catalog['sets'])} 个 · "
          f"功能 {sum(len(s['features']) for s in repo.catalog['sets'])} 条")
    print(f"错误 {len(errors)} 个，警告 {len(warnings)} 个")
    return 1 if errors else 0


def cmd_resolve(repo: Repo, args) -> int:
    closure, reason = resolve(repo, args.plugins)
    if args.json:
        print(json.dumps({
            "roots": args.plugins,
            "order": list(closure),
            "reason": reason,
            "conflicts": exclusive_conflicts(repo, closure),
        }, ensure_ascii=False, indent=2))
        return 0
    print(f"安装计划：{', '.join(args.plugins)}")
    rows = []
    for i, (pid, p) in enumerate(closure.items(), 1):
        rows.append([str(i), pid, LAYER_LABELS[p["layer"]], p["version"], reason[pid]])
    print(render_table(["#", "插件", "层级", "版本", "来源"], rows))
    packs = [pid for pid, p in closure.items() if p.get("kind") == "pack"]
    print(f"\n共 {len(closure)} 个组件（{len(packs)} 个扩展包，"
          f"{len(closure) - len(packs)} 个插件）")
    for line in exclusive_conflicts(repo, closure):
        print(f"注意：{line}")
    return 0


def cmd_info(repo: Repo, args) -> int:
    p = repo.plugins.get(args.plugin)
    if not p:
        print(f"未知插件: {args.plugin}", file=sys.stderr)
        return 1
    print(f"{p['nameZh']} ({p['id']})  v{p['version']}  [{p['kind']} / {p['layer']}]")
    print(f"  {p['description']}")
    if p.get("platforms"):
        print(f"  平台: {', '.join(p['platforms'])}")
    if p.get("dependencies"):
        print("  依赖: " + ", ".join(f"{k} {v}" for k, v in p["dependencies"].items()))
    if p.get("recommends"):
        for k, v in p["recommends"].items():
            flag = "自动连带安装" if isinstance(v, dict) and v.get("autoInstall") else "推荐"
            print(f"  {flag}: {k} —— {v.get('reason', '')}")
    if p.get("capabilitySets"):
        print("  功能集: " + ", ".join(p["capabilitySets"]))
        catalog_sets = repo.sets()
        for sid in p["capabilitySets"]:
            s = catalog_sets[sid]
            feats = [f["label"] for f in s["features"] if p["id"] in repo.feature_owners(s, f)]
            print(f"    - {s['label']}（{len(feats)} 条）")
    return 0


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
    print(f"\n合计 {total} 条功能。规划中 / 已知问题项不计入 v0.1 验收范围。")
    return 0


def cmd_graph(repo: Repo, args) -> int:
    ids = [pid for pid, p in repo.plugins.items()
           if args.layer is None or p["layer"] == args.layer]
    if args.format == "dot":
        print("digraph lightcpp {")
        print('  rankdir=LR; node [shape=box, fontname="Helvetica"];')
        for pid in ids:
            print(f'  "{pid}";')
        for pid in ids:
            for dep in (repo.plugins[pid].get("dependencies") or {}):
                if dep in ids:
                    print(f'  "{pid}" -> "{dep}";')
        print("}")
        return 0

    print("```mermaid")
    print("graph LR")
    for layer in LAYERS:
        members = [p for p in repo.by_layer(layer) if p["id"] in ids]
        if not members:
            continue
        print(f'  subgraph {layer}["{LAYER_LABELS[layer]}"]')
        for p in sorted(members, key=lambda x: x["id"]):
            print(f'    {p["id"]}["{p["nameZh"]}"]')
        print("  end")
    for pid in ids:
        for dep in (repo.plugins[pid].get("dependencies") or {}):
            if dep in ids:
                print(f"  {pid} --> {dep}")
    print("```")
    return 0


# --------------------------------------------------------------- 文档生成
DOC_HEADER = "<!-- 由 tools/lightcpp_pm.py docs 生成，请勿手工编辑 -->\n"


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
           "`tools/lightcpp_pm.py validate` 会强制要求所有 `required` 功能有归属。", ""]
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
    targets = {
        repo.root / "docs" / "PLUGINS.md": build_plugins_doc(repo),
        repo.root / "docs" / "FEATURES.md": build_features_doc(repo),
    }
    failed = False
    for path, content in targets.items():
        if args.check:
            current = path.read_text(encoding="utf-8") if path.exists() else ""
            if current != content:
                print(f"文档已过期: {path.relative_to(repo.root)}（请运行 docs 重新生成）")
                failed = True
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
            print(f"已生成 {path.relative_to(repo.root)}")
    return 1 if failed else 0


# --------------------------------------------------------------- 入口
def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="lightcpp-pm", description="LightCpp 插件生态管理工具")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("validate", help="校验插件清单、依赖关系与功能覆盖")
    p.add_argument("--strict", action="store_true", help="把提示类警告视为问题")
    p.set_defaults(func=cmd_validate)

    p = sub.add_parser("resolve", help="解析安装闭包与安装顺序")
    p.add_argument("plugins", nargs="+")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_resolve)

    p = sub.add_parser("info", help="查看单个插件详情")
    p.add_argument("plugin")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("engines", help="列出互斥引擎组（clangd / ccls）")
    p.set_defaults(func=cmd_engines)

    p = sub.add_parser("matrix", help="输出插件矩阵")
    p.add_argument("--json", action="store_true")
    p.set_defaults(func=cmd_matrix)

    p = sub.add_parser("coverage", help="功能覆盖统计")
    p.set_defaults(func=cmd_coverage)

    p = sub.add_parser("graph", help="输出依赖图")
    p.add_argument("--format", choices=["mermaid", "dot"], default="mermaid")
    p.add_argument("--layer")
    p.set_defaults(func=cmd_graph)

    p = sub.add_parser("docs", help="生成或检查 docs/PLUGINS.md 与 docs/FEATURES.md")
    p.add_argument("--check", action="store_true")
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
