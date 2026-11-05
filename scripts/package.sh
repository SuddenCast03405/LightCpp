#!/usr/bin/env bash
# LightCpp 交付包构建脚本
#
# 作用：从干净状态完整构建 C++ 内核（严格模式 + 测试），然后把
#   二进制、源码、清单、文档、许可打包成 dist/lightcpp-<版本>-<平台>.tar.gz
#
# 用法：
#   bash scripts/package.sh              # 构建 + 测试 + 打包
#   bash scripts/package.sh --skip-tests # 跳过测试（不推荐）
#   SKIP_BUILD=1 bash scripts/package.sh # 复用已有 core/build，只打包
#
# 环境要求：CMake 3.16+、C++17 编译器、Ninja（或改用 -G "Unix Makefiles"）、tar

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="${BUILD_DIR:-core/build}"
DIST_DIR="${DIST_DIR:-dist}"
SKIP_TESTS=0
for arg in "$@"; do
    case "$arg" in
        --skip-tests) SKIP_TESTS=1 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "未知参数：$arg" >&2; exit 2 ;;
    esac
done

# 版本号以 CMakeLists.txt 为准，避免两处不一致
VERSION="$(sed -n 's/^ *VERSION  *\([0-9][0-9.]*\)$/\1/p' core/CMakeLists.txt | head -1)"
if [ -z "$VERSION" ]; then
    echo "无法从 core/CMakeLists.txt 读取版本号" >&2
    exit 1
fi

HOST_OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
HOST_ARCH="$(uname -m)"
PKG_NAME="lightcpp-${VERSION}-${HOST_OS}-${HOST_ARCH}"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

# ---------------------------------------------------------------- 1. 构建
if [ "${SKIP_BUILD:-0}" != "1" ]; then
    log "配置（严格模式）"
    GENERATOR=()
    if command -v ninja >/dev/null 2>&1; then
        GENERATOR=(-G Ninja)
    fi
    cmake -S core -B "$BUILD_DIR" "${GENERATOR[@]}" \
          -DCMAKE_BUILD_TYPE=RelWithDebInfo \
          -DLIGHTC_WERROR=ON

    log "构建"
    cmake --build "$BUILD_DIR"

    if [ "$SKIP_TESTS" != "1" ]; then
        log "运行测试"
        ctest --test-dir "$BUILD_DIR" --output-on-failure
    fi

    log "校验清单（严格模式）"
    "$BUILD_DIR/bin/lightcpp-pm" validate --strict

    log "校验文档一致性"
    "$BUILD_DIR/bin/lightcpp-pm" docs --check
fi

# ---------------------------------------------------------------- 2. 组装
STAGE="$DIST_DIR/$PKG_NAME"
rm -rf "$STAGE"
mkdir -p "$STAGE"/{bin,plugins,include/lightcpp,catalog,docs,schemas,core,scripts}

log "拷贝可执行文件"
cp "$BUILD_DIR/bin/lightcpp-pm" "$STAGE/bin/"
if [ -f "$BUILD_DIR/bin/lightcpp_tests" ]; then
    cp "$BUILD_DIR/bin/lightcpp_tests" "$STAGE/bin/"
fi

log "拷贝插件动态库"
if [ -d "$BUILD_DIR/bin/plugins" ]; then
    cp "$BUILD_DIR/bin/plugins/"* "$STAGE/plugins/" 2>/dev/null || true
fi

log "拷贝头文件与源码"
cp -R core/include/lightcpp/. "$STAGE/include/lightcpp/"
cp core/CMakeLists.txt "$STAGE/core/"
cp -R core/src "$STAGE/core/"
cp -R core/plugins "$STAGE/core/"
cp -R core/cmake "$STAGE/core/"
cp -R core/tests "$STAGE/core/"

log "拷贝清单、文档、脚本"
cp -R plugins/. "$STAGE/plugins/" 2>/dev/null || true
cp -R catalog/. "$STAGE/catalog/"
cp -R schemas/. "$STAGE/schemas/"
cp -R docs/. "$STAGE/docs/"
cp -R tools "$STAGE/tools"
cp scripts/package.sh "$STAGE/scripts/"
for f in README.md LICENSE .gitignore; do
    if [ -f "$f" ]; then cp "$f" "$STAGE/"; fi
done

# 附一份校验和与构建信息，便于核对交付物一致性
log "写入构建信息"
{
    echo "版本: $VERSION"
    echo "平台: $HOST_OS-$HOST_ARCH"
    echo "构建时间: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "编译器: $("$BUILD_DIR/bin/lightcpp-pm" --version 2>/dev/null || echo '见 CMakeCache.txt')"
    echo ""
    echo "插件清单校验:"
    "$BUILD_DIR/bin/lightcpp-pm" validate --strict 2>&1 | tail -3
} > "$STAGE/BUILD-INFO.txt"

# ---------------------------------------------------------------- 3. 打包
log "打包"
ARCHIVE="$DIST_DIR/$PKG_NAME.tar.gz"
rm -f "$ARCHIVE"
tar -czf "$ARCHIVE" -C "$DIST_DIR" "$PKG_NAME"

log "生成校验和"
( cd "$DIST_DIR" && sha256sum "$PKG_NAME.tar.gz" > "$PKG_NAME.tar.gz.sha256" )

log "完成：$ARCHIVE ($(du -h "$ARCHIVE" | cut -f1))"
echo
echo "解包后可直接运行："
echo "  tar -xzf $(basename "$ARCHIVE")"
echo "  cd $PKG_NAME && ./bin/lightcpp-pm validate --strict"
