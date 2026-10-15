#!/bin/sh
# The macOS build's tools (port/macos/README.md), put in ~/toolchains (or
# $HALO_TOOLCHAINS) for this user only:
#
# - wasi-sdk, the clang and C library the game is compiled with for wasm32
#   (an ILP32 target, as the game's data needs);
# - wabt's wasm2c, which turns that WebAssembly into C for the Mac's clang,
#   built from its source with this port's patch (wabt-1.0.42-opence.patch:
#   memory.atomic.wait/notify as calls into the host, SIMD loads and stores on
#   shared memory, and --unshared-memory, plain accesses for a memory every
#   thread shares);
# - CMake and ninja, for wabt and SDL 3 (uv's, when they are not on the PATH).
set -eu

ROOT=${HALO_TOOLCHAINS:-$HOME/toolchains}
WASI_SDK_VERSION=34
WABT_VERSION=1.0.42
HERE=$(cd "$(dirname "$0")" && pwd)

mkdir -p "$ROOT"
cd "$ROOT"

if ! command -v cmake >/dev/null 2>&1 || ! command -v ninja >/dev/null 2>&1; then
	if ! command -v uv >/dev/null 2>&1; then
		echo "cmake and ninja are needed (or uv, to install them): https://docs.astral.sh/uv/" >&2
		exit 1
	fi
	uv tool install cmake
	uv tool install ninja
	PATH=$HOME/.local/bin:$PATH
fi

WASI_SDK=wasi-sdk-$WASI_SDK_VERSION.0-arm64-macos
if [ ! -x "$WASI_SDK/bin/clang" ]; then
	curl -fsSL -o wasi-sdk.tar.gz \
		"https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-$WASI_SDK_VERSION/$WASI_SDK.tar.gz"
	tar xzf wasi-sdk.tar.gz
	rm wasi-sdk.tar.gz
	xattr -dr com.apple.quarantine "$WASI_SDK" 2>/dev/null || true
fi

if [ ! -d wabt-src ]; then
	git clone -q --depth 1 --branch "$WABT_VERSION" --recurse-submodules --shallow-submodules \
		https://github.com/WebAssembly/wabt.git wabt-src
	(cd wabt-src && git apply "$HERE/wabt-$WABT_VERSION-opence.patch")
fi
cmake -S wabt-src -B wabt-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DBUILD_LIBWASM=OFF \
	-DUSE_INTERNAL_SHA256=ON -DCMAKE_INSTALL_PREFIX="$ROOT/wabt" >/dev/null
ninja -C wabt-build install >/dev/null

echo "wasi-sdk: $ROOT/$WASI_SDK"
echo "wasm2c:   $ROOT/wabt/bin/wasm2c (runtime: $ROOT/wabt-src/wasm2c)"
