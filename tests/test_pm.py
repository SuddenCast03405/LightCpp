#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""lightcpp_pm 的行为测试：既校验真实仓库，也用合成仓库覆盖各类错误分支。"""
import json
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
        problems = pm.validate(self.repo, strict=True)
        self.assertEqual([p for p in problems if p[0] == "error"], [])
        self.assertEqual([p for p in problems if p[0] == "warning"], [])

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


if __name__ == "__main__":
    unittest.main()
