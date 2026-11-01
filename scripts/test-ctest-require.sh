#!/usr/bin/env bash
# Checks the CI guard that refuses a green run when ffmpeg_decoder,
# present_smoke, or stream_player_http was omitted or skipped.
# No compiler, device, or network.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=ctest-require.sh
source "$ROOT/scripts/ctest-require.sh"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

expect_ok() {
  if ! "$@"; then
    fail "expected success: $*"
  fi
}

expect_fail() {
  if "$@" >/dev/null 2>&1; then
    fail "expected failure: $*"
  fi
}

registered=$'1: Test command: /build/ffmpeg_decoder_tests\n  Test #7: ffmpeg_decoder\n\n8: Test command: /build/stream_player_http_test\n  Test #8: stream_player_http\n\n9: Test command: /build/present_smoke_test\n  Test #9: present_smoke\n'
expect_ok decoder_tests_registered "$registered"

# Ten tests: ctest pads single-digit indexes to "Test  #N".
padded=$'  Test  #8: ffmpeg_decoder\n  Test  #9: stream_player_http\n  Test #10: present_smoke\n'
expect_ok decoder_tests_registered "$padded"

verbose_only=$'7: Test #7: ffmpeg_decoder\n8: Test #8: stream_player_http\n9: Test #9: present_smoke\n'
expect_ok decoder_tests_registered "$verbose_only"

host_only=$'  Test #1: layout_engine\n  Test #2: stream_manager_test\n  Test #3: mqtt_smoke_test\n  Test #4: software_renderer\n'
expect_fail decoder_tests_registered "$host_only"

missing_one=$'  Test #7: ffmpeg_decoder\n  Test #8: stream_player_http\n'
expect_fail decoder_tests_registered "$missing_one"

log="$(mktemp)"
trap 'rm -f "$log"' EXIT

cat >"$log" <<'EOF'
7/9 Test #7: ffmpeg_decoder ....................   Passed    0.44 sec
8/9 Test #8: stream_player_http ...............   Passed    0.07 sec
9/9 Test #9: present_smoke ....................   Passed    0.17 sec
EOF
expect_ok decoder_tests_ran "$log"

cat >"$log" <<'EOF'
1/3 Test  #8: ffmpeg_decoder ...................   Passed    0.34 sec
2/3 Test  #9: stream_player_http ...............   Passed    0.07 sec
3/3 Test #10: present_smoke ....................   Passed    0.15 sec
EOF
expect_ok decoder_tests_ran "$log"

cat >"$log" <<'EOF'
1/3 Test  #8: ffmpeg_decoder ...................***Skipped   0.01 sec
2/3 Test  #9: stream_player_http ...............   Passed    0.07 sec
3/3 Test #10: present_smoke ....................   Passed    0.15 sec
EOF
expect_fail decoder_tests_ran "$log"

cat >"$log" <<'EOF'
7/9 Test #7: ffmpeg_decoder ....................***Skipped   0.01 sec
8/9 Test #8: stream_player_http ...............   Passed    0.07 sec
9/9 Test #9: present_smoke ....................   Passed    0.17 sec
EOF
expect_fail decoder_tests_ran "$log"

cat >"$log" <<'EOF'
8/9 Test #8: stream_player_http ...............***Skipped   0.01 sec
7/9 Test #7: ffmpeg_decoder ....................   Passed    0.44 sec
9/9 Test #9: present_smoke ....................   Passed    0.17 sec
EOF
expect_fail decoder_tests_ran "$log"

cat >"$log" <<'EOF'
7/9 Test #7: ffmpeg_decoder ....................   Passed    0.44 sec
8/9 Test #8: stream_player_http ...............   Passed    0.07 sec
9/9 Test #9: present_smoke ....................***Skipped   0.01 sec
EOF
expect_fail decoder_tests_ran "$log"

cat >"$log" <<'EOF'
1/4 Test #1: layout_engine ....................   Passed    0.01 sec
2/4 Test #2: stream_manager_test ..............   Passed    0.00 sec
3/4 Test #3: mqtt_smoke_test ..................   Passed    0.00 sec
4/4 Test #4: software_renderer ................   Passed    0.00 sec
EOF
expect_fail decoder_tests_ran "$log"

# A longer name that only starts with a required test must not count.
cat >"$log" <<'EOF'
7/9 Test #7: ffmpeg_decoder_extra .............   Passed    0.44 sec
8/9 Test #8: stream_player_http ...............   Passed    0.07 sec
9/9 Test #9: present_smoke ....................   Passed    0.17 sec
EOF
expect_fail decoder_tests_ran "$log"

echo "ctest require checks ok"
