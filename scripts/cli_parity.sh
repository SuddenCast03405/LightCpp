#!/usr/bin/env bash
# C++ 与 Python 两份 lightcpp-pm 实现的输出对拍。
#
# 覆盖全部 10 个子命令、7 个层级、3 种图格式、3 个目标平台、47 个组件，
# 逐组比较 stdout / stderr / exit code 三项。任何一项不同即以 1 退出，
# 并打印前几行差异，便于定位是哪一侧漂了。
#
# 用法:
#   scripts/cli_parity.sh                     # 用 build/native/bin/lightcpp-pm
#   scripts/cli_parity.sh path/to/lightcpp-pm
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${1:-$ROOT/build/native/bin/lightcpp-pm}"

# Windows 的 runner 上通常只有 python 没有 python3，两者都试。
PYTHON=""
for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1; then
        PYTHON="$candidate"
        break
    fi
done
if [[ -z "$PYTHON" ]]; then
    echo "找不到 python（需要 3.10+）" >&2
    exit 1
fi
PY=("$PYTHON" "$ROOT/tools/lightcpp_pm.py")

if [[ ! -x "$BIN" ]]; then
    echo "找不到可执行的 lightcpp-pm: $BIN" >&2
    echo "先构建（cmake --build build/native）或把路径作为第一个参数传入。" >&2
    exit 1
fi

cd "$ROOT"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

total=0
bad=0

# 单组对拍：参数按空格切分传入（与终端直觉一致）。
check() {
    total=$((total + 1))
    # shellcheck disable=SC2086
    "$BIN" $1 >"$TMP/c.out" 2>"$TMP/c.err"
    local rc_c=$?
    # shellcheck disable=SC2086
    "${PY[@]}" $1 >"$TMP/p.out" 2>"$TMP/p.err"
    local rc_p=$?

    if ! diff -q "$TMP/c.out" "$TMP/p.out" >/dev/null \
        || ! diff -q "$TMP/c.err" "$TMP/p.err" >/dev/null \
        || [[ $rc_c -ne $rc_p ]]; then
        echo "DIFF: lightcpp-pm $1  (exit $rc_c vs $rc_p)"
        diff "$TMP/c.out" "$TMP/p.out" | head -6
        diff "$TMP/c.err" "$TMP/p.err" | head -6
        bad=$((bad + 1))
    fi
}

# 全局开关：帮助、版本、未知命令、空参数
check "--help"
check "-h"
check "validate -h"
check "--version"
check "-v"
check "version"
check "bogus"
check ""

check "list"
for layer in core language build-framework build-system debugger tooling pack bogus; do
    check "list --layer $layer"
done

for fmt in text mermaid dot; do
    check "graph --format $fmt"
    for layer in core language build-framework build-system debugger tooling pack bogus; do
        check "graph --layer $layer --format $fmt"
    done
done

check "validate"
check "validate --strict"
check "validate --json"
for platform in linux darwin win32; do
    check "validate --platform $platform"
    check "validate --platform $platform --strict"
    check "validate --platform $platform --json"
done

check "matrix"
check "matrix --json"
check "coverage"
check "engines"
check "docs --check"

check "host"
check "host --require-native"
check "host --only cmake,ninja"
check "host --require-native --only cpp-base,ninja"
check "host --only nope"

for dir in "$ROOT"/plugins/*/; do
    id="$(basename "$dir")"
    check "info $id"
    check "info $id --json"
done
check "info nonexistent"

for pack in cpp-base gnu-pack llvm-pack msvc-pack qt-pack cpp-extension-pack; do
    check "resolve $pack"
done
check "resolve cpp-base cmake ninja --json"
check "resolve nope"

echo "对比 $total 组，差异 $bad 组"
[[ $bad -eq 0 ]]
