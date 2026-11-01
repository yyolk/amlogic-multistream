# Sourced by scripts/run-tests.sh. scripts/test-ctest-require.sh exercises it.
# Do not enable set -e here; the caller owns the shell options.

# CTest names, not binary names. CI installs the libraries these need.
DECODER_TESTS=(
  ffmpeg_decoder
  present_smoke
  stream_player_http
)

# `ctest -N` prints "  Test #N: <name>". With 10 or more tests the number is
# padded, so single-digit entries are "  Test  #N: <name>" (two spaces).
# Verbose mode also prints "N: Test #N: <name>". Any of those counts.
ctest_lists_test() {
  local listing="$1"
  local name="$2"
  grep -E -q "(^|[[:space:]])Test[[:space:]]+#([0-9]+): ${name}([[:space:]]|$)" <<<"$listing"
}

# 0 when every decoder test is in a `ctest -N` listing.
decoder_tests_registered() {
  local listing="$1"
  local name
  for name in "${DECODER_TESTS[@]}"; do
    if ! ctest_lists_test "$listing" "$name"; then
      echo "required test '${name}' is not registered; CI installs libav, libmosquitto, libdrm, and ffmpeg, so a run without it is not green" >&2
      return 1
    fi
  done
  return 0
}

# 0 when the verbose ctest log shows each decoder test Passed and none Skipped.
decoder_tests_ran() {
  local log="$1"
  local name
  for name in "${DECODER_TESTS[@]}"; do
    if grep -E -q "Test[[:space:]]+#([0-9]+): ${name}[[:space:]].*\\*\\*\\*Skipped" "$log"; then
      echo "required test '${name}' was skipped (exit 77). CI installs ffmpeg, so this must run" >&2
      return 1
    fi
    if ! grep -E -q "Test[[:space:]]+#([0-9]+): ${name}[[:space:]].*Passed" "$log"; then
      echo "required test '${name}' did not pass" >&2
      return 1
    fi
  done
  return 0
}
