#!/usr/bin/env bash
# 一键构建 + 测试整个产品（IDE + 插件生态内核 + Python 参考实现）。
#
# 用途：本地或 CI 里用一条命令回答"现在这套东西到底能不能编译、能不能过测试"。
# 三个来源各自独立跑一遍，任何一步失败立即退出（set -e）。
#
# 用法：
#   bash scripts/build_all.sh                       # 全部（需要 Qt 6.5+ 与 clang-c 头文件）
#   bash scripts/build_all.sh --kernel-only         # 只跑内核（不需要 Qt）
#   CMAKE_PREFIX_PATH=/opt/Qt/6.8.3/gcc_64 bash scripts/build_all.sh
#   LLVM_ROOT=/usr/lib/llvm-18 bash scripts/build_all.sh
#
# 环境变量：
#   BUILD_DIR     根项目构建目录，默认 build/native
#   KERNEL_DIR    内核独立构建目录，默认 core/build
#   CMAKE_PREFIX_PATH   Qt 安装前缀（透传给 cmake）
#   LLVM_ROOT     clang-c/Index.h 所在前缀（透传给 cmake）
#   JOBS          并行度，默认 CPU 核数

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="${BUILD_DIR:-build/native}"
KERNEL_DIR="${KERNEL_DIR:-core/build}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
KERNEL_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --kernel-only) KERNEL_ONLY=1 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "未知参数：$arg" >&2; exit 2 ;;
    esac
done

step() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }

# ---------------------------------------------------------------- 第三方依赖
step "准备第三方依赖（Lua 5.4.7 / LuaBridge3）"
cmake -P _fetch_deps.cmake

# ---------------------------------------------------------------- 内核（独立构建）
# 独立构建是受支持的配置：CI 的 build.yml 就走这条路，不需要 Qt。
step "内核独立构建（-DLIGHTC_WERROR=ON）"
cmake -S core -B "$KERNEL_DIR" -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DLIGHTC_WERROR=ON
cmake --build "$KERNEL_DIR" --parallel "$JOBS"
ctest --test-dir "$KERNEL_DIR" --output-on-failure

step "内核清单与文档校验（C++ 实现）"
"$KERNEL_DIR/bin/lightcpp-pm" validate --strict
"$KERNEL_DIR/bin/lightcpp-pm" docs --check

# ---------------------------------------------------------------- Python 参考实现
step "Python 参考实现：validate / docs / unittest"
python3 tools/lightcpp_pm.py validate --strict
python3 tools/lightcpp_pm.py docs --check
python3 -m unittest discover -s tests

step "C++ 与 Python 两份实现输出对拍"
scripts/cli_parity.sh "$KERNEL_DIR/bin/lightcpp-pm"

if [ "$KERNEL_ONLY" = "1" ]; then
    step "完成（--kernel-only：跳过 IDE）"
    exit 0
fi

# ---------------------------------------------------------------- 整棵树
step "根项目构建（IDE + 内核，一次 configure 覆盖全部产物）"
EXTRA=()
[ -n "${CMAKE_PREFIX_PATH:-}" ] && EXTRA+=(-DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH")
[ -n "${LLVM_ROOT:-}" ] && EXTRA+=(-DLLVM_ROOT="$LLVM_ROOT")
cmake -S . -B "$BUILD_DIR" -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug \
      -DLIGHTCPP_DEPLOY_QT=OFF \
      "${EXTRA[@]}"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

step "全量 CTest（IDE + 内核）"
ctest --test-dir "$BUILD_DIR" --output-on-failure

step "全部通过"
