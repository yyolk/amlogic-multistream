#!/usr/bin/env bash
# Configure, build, and run CTest on a normal Linux host.
#
# layout_engine_tests is required: the script fails if that binary is not
# built or is not registered with CTest. Every other add_test() target runs
# in the same ctest invocation.
#
# AMS_REQUIRE_DECODER_TESTS=1 (set by GitHub Actions and the devcontainer)
# also fails the run when ffmpeg_decoder, present_smoke, or stream_player_http
# is missing from the CTest list or is reported skipped. Those three need the
# dev packages the workflow installs. Leave the variable unset on a host that
# does not have them: CMake omits the targets and this script still runs the
# tests it could build.
#
# scripts/launch-coreelec.sh requires GS-King X / CoreELEC. CTest runs
# scripts/test-launch-coreelec.sh, which drives that script with a stand-in
# runtime and does not boot a device.

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

# shellcheck source=ctest-require.sh
source "$ROOT/scripts/ctest-require.sh"

require_decoder_tests=0
if [[ "${AMS_REQUIRE_DECODER_TESTS:-}" == "1" ]]; then
  require_decoder_tests=1
fi

echo "CTest tests:"
listing="$(ctest --test-dir "$BUILD_DIR" -N -V 2>&1)"
printf '%s\n' "$listing"
if ! grep -q 'layout_engine_tests' <<<"$listing"; then
  die "CTest is not configured to run layout_engine_tests"
fi
if [[ "$require_decoder_tests" == 1 ]]; then
  decoder_tests_registered "$listing"
fi

echo "Running ctest (layout_engine_tests and any other registered tests)"
ctest_log="${CTEST_LOG:-$BUILD_DIR/ctest-verbose.log}"
set +e
timeout --foreground 300 ctest --test-dir "$BUILD_DIR" --output-on-failure --verbose | tee "$ctest_log"
ctest_rc=${PIPESTATUS[0]}
set -e
if [[ "$ctest_rc" -ne 0 ]]; then
  exit "$ctest_rc"
fi

if [[ "$require_decoder_tests" == 1 ]]; then
  decoder_tests_ran "$ctest_log"
  echo "Required decoder tests passed: ${DECODER_TESTS[*]}"
fi
