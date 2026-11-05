#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""lightcpp_pm 的行为测试：既校验真实仓库，也用合成仓库覆盖各类错误分支。"""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import lightcpp_pm as pm  # noqa: E402


# --------------------------------------------------------------- 合成仓库
def make_repo(tmp: Path, plugins: dict, sets: list | None = None, rules: list | None = None) -> pm.Repo:
    for pid, data in plugins.items():
        path = tmp / "plugins" / pid
        path.mkdir(parents=True)
        body = {"id": pid, "name": pid, "nameZh": pid, "version": "0.1.0",
                "publisher": "test", "kind": "plugin", "layer": "tooling",
                "description": "测试用插件，说明足够长。", **data}
        (path / "plugin.json").write_text(json.dumps(body, ensure_ascii=False), encoding="utf-8")
    (tmp / "catalog").mkdir(exist_ok=True)
    catalog = {"schemaVersion": 1, "domains": [{"id": "tool", "label": "工具"}],
               "sets": sets or []}
    (tmp / "catalog" / "features.json").write_text(
        json.dumps(catalog, ensure_ascii=False), encoding="utf-8")
    (tmp / "catalog" / "invariants.json").write_text(
        json.dumps({"rules": rules or []}, ensure_ascii=False), encoding="utf-8")
    return pm.Repo(tmp)


def codes(problems):
    return [p[1] for p in problems]


# --------------------------------------------------------------- 单元
class VersionTests(unittest.TestCase):
    def test_caret(self):
        self.assertTrue(pm.satisfies("0.1.5", "^0.1.0"))
        self.assertFalse(pm.satisfies("0.2.0", "^0.1.0"))

    def test_tilde(self):
        self.assertTrue(pm.satisfies("0.1.9", "~0.1.0"))
        self.assertFalse(pm.satisfies("0.2.0", "~0.1.0"))

    def test_wildcard_and_floor(self):
        self.assertTrue(pm.satisfies("9.9.9", "*"))
        self.assertTrue(pm.satisfies("1.2.3", ">=1.0.0"))
        self.assertFalse(pm.satisfies("0.9.0", ">=1.0.0"))

    def test_invalid(self):
        with self.assertRaises(ValueError):
            pm.satisfies("abc", ">=1.0.0")


class RealRepoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repo = pm.Repo(ROOT)

    def test_validate_clean(self):
        # strict=True 也会保留 W003：它是"这个插件不适用于当前平台"的
        # 事实陈述，不是待修的缺陷。其余任何警告都算不干净。
        problems = pm.validate(self.repo, strict=True)
        self.assertEqual([p for p in problems if p[0] == "error"], [])
        self.assertEqual([p[1] for p in problems if p[0] == "warning" and p[1] != "W003"], [])

    def test_validate_platform_filtering(self):
        """W003 只针对"声明了平台但当前目标不在其中"的插件，换平台应当换名单。"""
        def flagged(platform):
            return {p[2].split(" ")[0]
                    for p in pm.validate(self.repo, platform=platform) if p[1] == "W003"}

        linux, win32, darwin = flagged("linux"), flagged("win32"), flagged("darwin")
        # win32 专属插件在 win32 上不该被提示，在 linux 上必须被提示。
        self.assertNotIn("wsl", win32)
        self.assertIn("wsl", linux)
        self.assertIn("wsl", darwin)
        # darwin 专属插件反之。
        self.assertIn("xcodebuild", linux)
        self.assertNotIn("xcodebuild", darwin)
        # 全平台插件永远不进名单。
        self.assertNotIn("cpp-base", linux | win32 | darwin)

    def test_every_required_feature_covered(self):
        for s, f in self.repo.features():
            if f.get("status", "required") == "required":
                self.assertTrue(self.repo.feature_owners(s, f), f["id"])

    def test_pack_resolves_transitively(self):
        closure, reason = pm.resolve(self.repo, ["cpp-extension-pack"])
        self.assertEqual(len(closure), 13)
        for pid in ("cpp-base", "clang-core", "build-core", "ninja", "asm"):
            self.assertIn(pid, closure)
        self.assertEqual(reason["ninja"], "由 cpp-extension-pack 引入")

    def test_cmake_auto_installs_ninja(self):
        closure, reason = pm.resolve(self.repo, ["cmake"])
        self.assertIn("ninja", closure)
        self.assertTrue(reason["ninja"].startswith("自动连带安装"))

    def test_meson_auto_installs_ninja(self):
        closure, _ = pm.resolve(self.repo, ["meson"])
        self.assertIn("ninja", closure)

    def test_devcpp_pulls_make(self):
        closure, _ = pm.resolve(self.repo, ["devcpp"])
        self.assertIn("make", closure)

    def test_install_order_respects_dependencies(self):
        for pid in self.repo.plugins:
            closure, _ = pm.resolve(self.repo, [pid])
            seen = []
            for _, p in closure.items():
                for dep in p.get("dependencies") or {}:
                    self.assertIn(dep, seen, f"{pid}: {dep} 应在 {p['id']} 之前安装")
                seen.append(p["id"])

    def test_every_plugin_installable(self):
        for pid in self.repo.plugins:
            closure, _ = pm.resolve(self.repo, [pid])
            self.assertIn(pid, closure)

    def test_pack_has_no_payload(self):
        for p in self.repo.plugins.values():
            if p["kind"] == "pack":
                self.assertFalse(p.get("contributes"))
                self.assertFalse(p.get("capabilitySets"))

    def test_lsp_engines_exclusive(self):
        groups = {}
        for p in self.repo.plugins.values():
            lsp = (p.get("contributes") or {}).get("lsp") or {}
            if lsp:
                groups.setdefault(lsp["exclusiveGroup"], []).append(lsp["engine"])
        self.assertEqual(sorted(groups["cpp-lsp"]), ["ccls", "clangd"])

    def test_every_plugin_dir_is_tracked(self):
        """防止 .gitignore 规则（如 build-*/）把插件目录吞掉。"""
        if not (ROOT / ".git").exists():
            self.skipTest("非 git 工作区")
        out = subprocess.run(["git", "-C", str(ROOT), "ls-files", "plugins"],
                             capture_output=True, text=True, check=True).stdout
        tracked = {Path(line).parts[1] for line in out.splitlines()
                   if line.endswith("plugin.json")}
        # plugins/ now holds manifests only. The compiled QPlugin sources moved
        # to src/LightCppPlugins/ precisely so that this invariant can stay
        # simple: every directory under plugins/ that exists on disk must carry
        # a plugin.json, and every such directory must be tracked by git. A
        # manifest directory swallowed by an ignore rule still shows up in
        # on_disk and still fails here.
        on_disk = {p.name for p in (ROOT / "plugins").iterdir()
                   if p.is_dir() and (p / "plugin.json").is_file()}
        self.assertEqual(on_disk - tracked, set(), "存在未被 git 跟踪的插件目录")

    def test_docs_up_to_date(self):
        self.assertEqual(pm.build_plugins_doc(self.repo),
                         (ROOT / "docs" / "PLUGINS.md").read_text(encoding="utf-8"))
        self.assertEqual(pm.build_features_doc(self.repo),
                         (ROOT / "docs" / "FEATURES.md").read_text(encoding="utf-8"))


class SyntheticRepoTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def test_detects_missing_dependency(self):
        repo = make_repo(self.tmp, {"a": {"dependencies": {"ghost": "*"}}})
        self.assertIn("E010", codes(pm.validate(repo)))

    def test_detects_version_mismatch(self):
        repo = make_repo(self.tmp, {
            "a": {"dependencies": {"b": "^0.2.0"}},
            "b": {},
        })
        self.assertIn("E011", codes(pm.validate(repo)))

    def test_detects_cycle(self):
        repo = make_repo(self.tmp, {
            "a": {"dependencies": {"b": "*"}},
            "b": {"dependencies": {"a": "*"}},
        })
        self.assertIn("E020", codes(pm.validate(repo)))
        with self.assertRaises(ValueError):
            pm.resolve(repo, ["a"])

    def test_pack_must_not_contribute(self):
        repo = make_repo(self.tmp, {
            "a": {},
            "pack": {"kind": "pack", "layer": "pack",
                     "dependencies": {"a": "*"}, "contributes": {"commands": []}},
        })
        self.assertIn("E017", codes(pm.validate(repo)))

    def test_pack_must_have_members(self):
        repo = make_repo(self.tmp, {
            "pack": {"kind": "pack", "layer": "pack", "dependencies": {}},
        })
        self.assertIn("E018", codes(pm.validate(repo)))

    def test_uncovered_required_feature(self):
        repo = make_repo(self.tmp, {"a": {"capabilitySets": ["s"]}}, sets=[{
            "id": "s", "domain": "tool", "label": "集", "plugins": [],
            "features": [{"id": "f1", "label": "功能", "status": "required"}],
        }])
        self.assertIn("E023", codes(pm.validate(repo)))

    def test_except_removes_owner(self):
        repo = make_repo(self.tmp, {
            "a": {"capabilitySets": ["s"]},
            "b": {"capabilitySets": ["s"]},
        }, sets=[{
            "id": "s", "domain": "tool", "label": "集", "plugins": ["a", "b"],
            "features": [{"id": "f1", "label": "功能", "except": ["b"]}],
        }])
        s = repo.sets()["s"]
        f = s["features"][0]
        self.assertEqual(repo.feature_owners(s, f), ["a"])

    def test_invariant_pack_members(self):
        repo = make_repo(self.tmp, {
            "a": {}, "b": {},
            "pack": {"kind": "pack", "layer": "pack", "dependencies": {"a": "*"}},
        }, rules=[{"id": "r", "type": "packMembers", "pack": "pack",
                   "members": ["a", "b"], "description": "成员固定"}])
        self.assertIn("E102", codes(pm.validate(repo)))

    def test_invariant_layer_dependency(self):
        repo = make_repo(self.tmp, {"a": {"layer": "build-system"}},
                         rules=[{"id": "r", "type": "layerDependency",
                                 "layer": "build-system", "on": ["build-core"]}])
        self.assertIn("E103", codes(pm.validate(repo)))


class RobustnessTests(unittest.TestCase):
    """校验器本身不能因为清单写错就崩溃 —— 那是它唯一的工作。"""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def test_dependencies_as_list_does_not_crash(self):
        repo = make_repo(self.tmp, {"a": {}, "b": {"dependencies": ["a"]}})
        problems = pm.validate(repo)          # 修复前抛 AttributeError
        self.assertIn("E001", codes(problems))
        self.assertIn("b", pm.resolve(repo, ["b"])[0])

    def test_capability_sets_as_string_is_reported(self):
        repo = make_repo(self.tmp, {"a": {"capabilitySets": "tool.pkg"}},
                         sets=[{"id": "tool.pkg", "domain": "tool", "label": "集",
                                "plugins": ["a"],
                                "features": [{"id": "f1", "label": "功能"}]}])
        self.assertIn("E001", codes(pm.validate(repo)))
        self.assertEqual(repo.plugins["a"]["capabilitySets"], ["tool.pkg"])

    def test_platforms_as_string_does_not_emit_per_char_errors(self):
        repo = make_repo(self.tmp, {"a": {"platforms": "linux"}})
        codes_ = codes(pm.validate(repo))
        self.assertIn("E001", codes_)
        self.assertNotIn("E009", codes_)

    def test_contributes_commands_as_string(self):
        repo = make_repo(self.tmp, {"a": {"contributes": {"commands": "a.run"}}})
        self.assertIn("E001", codes(pm.validate(repo)))

    def test_unknown_field_is_rejected(self):
        repo = make_repo(self.tmp, {"a": {"dependecies": {"b": "*"}}, "b": {}})
        self.assertIn("E026", codes(pm.validate(repo)))

    def test_short_description_is_rejected(self):
        repo = make_repo(self.tmp, {"a": {"description": "太短"}})
        self.assertIn("E027", codes(pm.validate(repo)))

    def test_layer_membership_is_enforced(self):
        repo = make_repo(self.tmp, {"a": {"layer": "core"}})
        self.assertIn("E028", codes(pm.validate(repo)))

    def test_duplicate_plugin_id(self):
        make_repo(self.tmp, {"a": {}})
        other = self.tmp / "plugins" / "a-copy"
        other.mkdir()
        (other / "plugin.json").write_text(
            json.dumps({"id": "a", "name": "a", "nameZh": "a", "version": "0.1.0",
                        "publisher": "t", "kind": "plugin", "layer": "tooling",
                        "description": "测试用插件，说明足够长。"}, ensure_ascii=False))
        repo = pm.Repo(self.tmp)
        self.assertTrue(any("重复声明" in m for m in repo.load_errors))

    def test_catalog_structure_errors(self):
        repo = make_repo(self.tmp, {"a": {"capabilitySets": ["s"]}}, sets=[{
            "id": "s", "domain": "nope", "label": "集", "plugins": ["a"],
            "features": [{"id": "f1", "label": "功能", "except": ["ghost"]}],
        }])
        codes_ = codes(pm.validate(repo))
        self.assertIn("E030", codes_)   # 未知 domain
        self.assertIn("E032", codes_)   # except 指向不存在插件

    def test_duplicate_set_id(self):
        repo = make_repo(self.tmp, {"a": {"capabilitySets": ["s"]}}, sets=[
            {"id": "s", "domain": "tool", "label": "集", "plugins": ["a"],
             "features": [{"id": "f1", "label": "功能"}]},
            {"id": "s", "domain": "tool", "label": "集2", "plugins": ["a"],
             "features": [{"id": "f2", "label": "功能2"}]},
        ])
        self.assertIn("E029", codes(pm.validate(repo)))

    def test_empty_set(self):
        repo = make_repo(self.tmp, {"a": {"capabilitySets": ["s"]}}, sets=[
            {"id": "s", "domain": "tool", "label": "集", "plugins": ["a"], "features": []},
        ])
        self.assertIn("E031", codes(pm.validate(repo)))

    def test_table_escapes_pipe_and_newline(self):
        table = pm.render_table(["a"], [["x|y"], ["m\nn"]])
        self.assertIn(r"x\|y", table)
        # 换行被压平成空格，行数 = 表头 + 分隔行 + 2 行数据
        self.assertEqual(len(table.splitlines()), 4)

    def test_version_segment_overflow(self):
        with self.assertRaises(ValueError):
            pm.satisfies("1.0.0", "~99999.0.0")


if __name__ == "__main__":
    unittest.main()
