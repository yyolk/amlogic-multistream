#!/usr/bin/env bash
# Configure, build, and run CTest on a normal Linux host.
#
# layout_engine_tests is required: the script fails if that binary is not
# built or is not registered with CTest. Every other add_test() target runs
# in the same ctest invocation.
#
# CoreELEC-only and not used here: scripts/launch-coreelec.sh
# (stops Kodi and expects /storage/amlogic-multistream on a GS-King X).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"

die() {
  echo "error: $*" >&2
  exit 1
}

need() {
  command -v "$1" >/dev/null 2>&1 || die "required command '$1' is not installed"
}

need cmake
need g++
need ctest
need timeout

CMAKE_ARGS=(
  -S "$ROOT"
  -B "$BUILD_DIR"
  -DCMAKE_CXX_COMPILER=g++
)
if [[ -n "${CMAKE_BUILD_TYPE:-}" ]]; then
  CMAKE_ARGS+=(-DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE")
fi
if command -v ccache >/dev/null 2>&1; then
  CMAKE_ARGS+=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
fi

echo "Configuring (g++: $(g++ --version | head -n 1))"
cmake "${CMAKE_ARGS[@]}"

echo "Building all targets, including layout_engine_tests"
cmake --build "$BUILD_DIR" --parallel

[[ -x "$BUILD_DIR/layout_engine_tests" ]] || die "build did not produce $BUILD_DIR/layout_engine_tests"

echo "CTest tests:"
listing="$(ctest --test-dir "$BUILD_DIR" -N -V 2>&1)"
printf '%s\n' "$listing"
if ! grep -q 'layout_engine_tests' <<<"$listing"; then
  die "CTest is not configured to run layout_engine_tests"
fi

echo "Running ctest (layout_engine_tests and any other registered tests)"
timeout --foreground 120 ctest --test-dir "$BUILD_DIR" --output-on-failure --verbose
