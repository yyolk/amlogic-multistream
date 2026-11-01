#!/usr/bin/env bash
# Stand-in for scripts/launch-coreelec.sh.
#
# The launch script requires GS-King X / CoreELEC and is not run against a
# device here. This harness substitutes PLAYER_BIN, a logging systemctl, and
# a type-2 runtime that forks a payload and does not forward SIGTERM (the
# same shape as the AppImage runtime). It runs under bash, dash, and
# busybox ash when that shell is installed.
#
# Pass --appimage PATH to also drive the real packaged AppImage (and, with
# --bare PATH, the unpackaged ELF) against a local mosquitto. That checks
# magic detection and --appimage-extract, which the stand-in cannot.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LAUNCH="$ROOT/scripts/launch-coreelec.sh"

die() {
  echo "FAIL $*" >&2
  exit 1
}

need() {
  command -v "$1" >/dev/null 2>&1 || die "required command '$1' is not installed"
}

need g++
need mktemp
need timeout

[[ -f "$LAUNCH" ]] || die "missing $LAUNCH"

ONLY_APPIMAGE=0
APPIMAGE=""
BARE=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --only-appimage) ONLY_APPIMAGE=1 ;;
    --appimage)
      shift
      [[ $# -gt 0 ]] || die "--appimage needs a path"
      APPIMAGE=$1
      ;;
    --bare)
      shift
      [[ $# -gt 0 ]] || die "--bare needs a path"
      BARE=$1
      ;;
    *) die "unknown argument: $1" ;;
  esac
  shift
done

# /proc/pid/exe is absolute. A relative --bare path never compares equal,
# which is what CI passed (build/amlogic-multistream).
if [[ -n "$APPIMAGE" ]]; then
  APPIMAGE=$(readlink -f "$APPIMAGE")
  [[ -x "$APPIMAGE" ]] || die "AppImage is not executable: $APPIMAGE"
fi
if [[ -n "$BARE" ]]; then
  BARE=$(readlink -f "$BARE")
  [[ -x "$BARE" ]] || die "bare ELF is not executable: $BARE"
fi

failures=0
pass() { echo "ok  $*"; }
fail() {
  echo "FAIL $*" >&2
  failures=$((failures + 1))
}

# shells: name, then the argv that execs a script path appended by the caller
declare -a SHELL_NAMES=()
declare -a SHELL_CMDS=()
add_shell() {
  SHELL_NAMES+=("$1")
  SHELL_CMDS+=("$2")
}
add_shell bash "bash"
if command -v dash >/dev/null 2>&1; then
  add_shell dash "dash"
elif [[ "${CI:-}" == "true" ]]; then
  die "CI requires dash"
else
  echo "skip dash (not installed)"
fi
if command -v busybox >/dev/null 2>&1; then
  add_shell busybox "busybox sh"
elif [[ "${CI:-}" == "true" ]]; then
  die "CI requires busybox"
else
  echo "skip busybox (not installed)"
fi

WORK=$(mktemp -d /tmp/amlogic-launch-test.XXXXXX)
ORIG_PATH=$PATH
REAL_EXTRACT=""
BROKER_PID=""
cleanup() {
  if [[ -n "$BROKER_PID" ]]; then
    kill "$BROKER_PID" 2>/dev/null || true
  fi
  rm -rf "$WORK"
  if [[ -n "$REAL_EXTRACT" ]]; then
    rm -rf "$REAL_EXTRACT"
  fi
}
trap cleanup EXIT

g++ -std=c++17 -Wall -Wextra -O2 -o "$WORK/launcher" -x c++ - <<'EOF'
#include <csignal>
#include <unistd.h>
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    if (setsid() < 0) return 3;
    execvp(argv[1], argv + 1);
    return 127;
}
EOF

g++ -std=c++17 -Wall -Wextra -O2 -pthread -o "$WORK/payload" -x c++ - <<'EOF'
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>
int main() {
    const char* ready_path = std::getenv("READY_FILE");
    const char* signal_path = std::getenv("SIGNAL_FILE");
    if (ready_path == nullptr || signal_path == nullptr) return 2;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &set, nullptr) != 0) return 3;
    {
        std::ofstream ready(ready_path);
        ready << getpid() << "\n";
    }
    int sig = 0;
    if (sigwait(&set, &sig) != 0) return 4;
    std::ofstream out(signal_path);
    out << sig << "\n";
    return 0;
}
EOF

# Ignores TERM, HUP, and INT so the launch script's stop window stays open
# long enough for a second signal. SIGKILL still ends it.
g++ -std=c++17 -Wall -Wextra -O2 -o "$WORK/payload-ignore" -x c++ - <<'EOF'
#include <csignal>
#include <fstream>
#include <cstdlib>
#include <unistd.h>
int main() {
    const char* ready_path = std::getenv("READY_FILE");
    if (ready_path == nullptr) return 2;
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    {
        std::ofstream ready(ready_path);
        ready << getpid() << "\n";
    }
    for (;;) pause();
}
EOF

# Type-2 stand-in. --appimage-extract writes squashfs-root/AppRun that execs
# STUB_PAYLOAD. Any other invocation is the buggy runtime: ignore TERM, fork
# the payload, and do not forward the signal.
g++ -std=c++17 -Wall -Wextra -O2 -o "$WORK/runtime" -x c++ - <<'EOF'
#include <sys/stat.h>
#include <sys/wait.h>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>
int main(int argc, char** argv) {
    const char* payload = std::getenv("STUB_PAYLOAD");
    if (payload == nullptr || payload[0] == '\0') return 2;
    if (argc > 1 && std::string(argv[1]) == "--appimage-extract") {
        if (mkdir("squashfs-root", 0755) != 0) return 3;
        std::ofstream app("squashfs-root/AppRun");
        const char* tree = std::getenv("STUB_TREE");
        if (tree != nullptr && std::string(tree) == "1") {
            // Fork two children, one of which has a grandchild. signal_tree
            // must signal the root and both branches.
            app << "#!/bin/sh\n"
                << "dir=${TREE_DIR:?}\n"
                << "echo $$ > \"$dir/root\"\n"
                << "(\n"
                << "  echo $$ > \"$dir/c1\"\n"
                << "  sleep 120 &\n"
                << "  echo $! > \"$dir/g\"\n"
                << "  wait\n"
                << ") &\n"
                << "echo $! > \"$dir/c1.pid\"\n"
                << "(\n"
                << "  echo $$ > \"$dir/c2\"\n"
                << "  exec sleep 120\n"
                << ") &\n"
                << "echo $! > \"$dir/c2.pid\"\n"
                << "wait\n";
        } else {
            app << "#!/bin/sh\nexec \"" << payload << "\" \"$@\"\n";
        }
        app.close();
        if (chmod("squashfs-root/AppRun", 0755) != 0) return 4;
        return 0;
    }
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    const pid_t child = fork();
    if (child < 0) return 5;
    if (child == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        execl(payload, payload, static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0) return 6;
    return 0;
}
EOF

printf 'AI\002' | dd of="$WORK/runtime" bs=1 seek=8 conv=notrunc status=none
printf 'AI\002' > "$WORK/magic-expect"
dd if="$WORK/runtime" bs=1 skip=8 count=3 of="$WORK/magic-got" status=none 2>/dev/null
cmp -s "$WORK/magic-expect" "$WORK/magic-got" || die "failed to stamp type-2 magic on the stand-in runtime"

cat > "$WORK/systemctl" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >> "$SYSTEMCTL_LOG"
if [ "$1" = "stop" ] && [ "${SYSTEMCTL_BLOCK_STOP:-}" = "1" ]; then
    printf '%s\n' stop > "$SYSTEMCTL_STATE"
    sleep 30
fi
exit 0
EOF
chmod +x "$WORK/systemctl"

# Background the launcher in THIS shell. A command substitution would run it
# in a subshell that exits and SIGHUPs the script under test.
start_shell() {
  local cmd=$1
  local out=$2
  local err=$3
  # cmd is "bash" or "busybox sh". The script path is the final argument.
  # shellcheck disable=SC2086
  env \
    SYSTEMCTL_LOG="$SYSTEMCTL_LOG" \
    SYSTEMCTL_STATE="${SYSTEMCTL_STATE:-}" \
    SYSTEMCTL_BLOCK_STOP="${SYSTEMCTL_BLOCK_STOP:-0}" \
    READY_FILE="${READY_FILE:-}" \
    SIGNAL_FILE="${SIGNAL_FILE:-}" \
    STUB_PAYLOAD="${STUB_PAYLOAD:-}" \
    STUB_TREE="${STUB_TREE:-}" \
    TREE_DIR="${TREE_DIR:-}" \
    PLAYER_BIN="$PLAYER_BIN" \
    PLAYER_EXTRACT_DIR="${PLAYER_EXTRACT_DIR:-}" \
    MQTT_HOST="${MQTT_HOST:-}" \
    MQTT_PORT="${MQTT_PORT:-}" \
    PATH="$WORK:$ORIG_PATH" \
    "$WORK/launcher" $cmd "$LAUNCH" >"$out" 2>"$err" &
  STARTED_PID=$!
}

wait_file() {
  local path=$1
  local i=0
  while [[ $i -lt 50 ]]; do
    if [[ -s "$path" ]]; then
      return 0
    fi
    sleep 0.1
    i=$((i + 1))
  done
  return 1
}

wait_pid_dead() {
  local pid=$1
  local i=0
  while [[ $i -lt 50 ]]; do
    if ! kill -0 "$pid" 2>/dev/null; then
      return 0
    fi
    sleep 0.1
    i=$((i + 1))
  done
  return 1
}

count_starts() {
  local log=$1
  grep -c '^start kodi$' "$log" || true
}

# Returns the shell pid via the global LAST_SHELL.
run_case() {
  local label=$1
  local shell_cmd=$2
  local player=$3
  local mode=$4
  local case_dir
  case_dir=$(mktemp -d "$WORK/case.XXXXXX")
  local log="$case_dir/systemctl.log"
  local state="$case_dir/state"
  local ready="$case_dir/ready"
  local sigfile="$case_dir/signal"
  local out="$case_dir/out"
  local err="$case_dir/err"
  local extract="$case_dir/extract"
  : >"$log"

  local shell_pid
  SYSTEMCTL_LOG=$log
  SYSTEMCTL_STATE=$state
  if [[ "$mode" == "pre-int" ]]; then
    SYSTEMCTL_BLOCK_STOP=1
  else
    SYSTEMCTL_BLOCK_STOP=0
  fi
  READY_FILE=$ready
  SIGNAL_FILE=$sigfile
  STUB_PAYLOAD=$WORK/payload
  STUB_TREE=
  TREE_DIR=
  PLAYER_BIN=$player
  PLAYER_EXTRACT_DIR=$extract
  MQTT_HOST=""
  MQTT_PORT=""
  start_shell "$shell_cmd" "$out" "$err"
  shell_pid=$STARTED_PID

  local status=0
  case "$mode" in
    pre-int)
      if ! wait_file "$state"; then
        fail "$label: systemctl stop did not start"
        kill -s TERM "$shell_pid" 2>/dev/null || true
        wait "$shell_pid" 2>/dev/null || true
        return
      fi
      kill -s INT -- "-$shell_pid"
      ;;
    term | hup | post-int)
      if ! wait_file "$ready"; then
        fail "$label: player did not become ready"
        echo "--- stdout ---" >&2
        cat "$out" >&2 || true
        echo "--- stderr ---" >&2
        cat "$err" >&2 || true
        kill -s TERM "$shell_pid" 2>/dev/null || true
        wait "$shell_pid" 2>/dev/null || true
        return
      fi
      local payload_pid parent
      payload_pid=$(cat "$ready")
      parent=$(sed -n 's/^PPid:[[:space:]]*//p' "/proc/$payload_pid/status" | head -n 1)
      # AppRun is exec'd by the launch script, so the payload's parent is
      # that shell. A type-2 runtime left in the middle would be the parent
      # instead, which is the SIGTERM orphan.
      if [[ "$parent" != "$shell_pid" ]]; then
        fail "$label: payload parent ${parent:-missing} is not the shell $shell_pid"
        kill -s TERM "$shell_pid" 2>/dev/null || true
        wait "$shell_pid" 2>/dev/null || true
        return
      fi
      case "$mode" in
        term) kill -s TERM "$shell_pid" ;;
        hup) kill -s HUP "$shell_pid" ;;
        post-int) kill -s INT -- "-$shell_pid" ;;
      esac
      ;;
    *) die "bad mode $mode" ;;
  esac

  local i=0
  while kill -0 "$shell_pid" 2>/dev/null && [[ $i -lt 150 ]]; do
    sleep 0.1
    i=$((i + 1))
  done
  if kill -0 "$shell_pid" 2>/dev/null; then
    fail "$label: shell still running"
    kill -s KILL -- "-$shell_pid" 2>/dev/null || true
    wait "$shell_pid" 2>/dev/null || true
    return
  fi
  wait "$shell_pid" || status=$?

  local starts
  starts=$(count_starts "$log")
  if [[ "$starts" != "1" ]]; then
    fail "$label: expected one 'start kodi', got ${starts:-0}"
    cat "$log" >&2 || true
    return
  fi

  case "$mode" in
    pre-int)
      if [[ -e "$ready" ]]; then
        fail "$label: player started before the signal"
        return
      fi
      if [[ "$status" -ne 130 ]]; then
        fail "$label: expected exit 130, got $status"
        return
      fi
      ;;
    term | hup)
      local sig
      payload_pid=$(cat "$ready")
      if ! wait_pid_dead "$payload_pid"; then
        fail "$label: payload $payload_pid still running (orphan)"
        return
      fi
      if [[ ! -s "$sigfile" ]]; then
        fail "$label: payload wrote no signal"
        return
      fi
      sig=$(cat "$sigfile")
      if [[ "$sig" != "15" ]]; then
        fail "$label: expected signal 15, got $sig"
        return
      fi
      if [[ "$status" -ne 0 ]]; then
        fail "$label: expected exit 0, got $status"
        return
      fi
      ;;
    post-int)
      local sig
      payload_pid=$(cat "$ready")
      if ! wait_pid_dead "$payload_pid"; then
        fail "$label: payload $payload_pid still running after SIGINT"
        return
      fi
      sig=$(cat "$sigfile" 2>/dev/null || true)
      if [[ "$sig" != "2" ]]; then
        fail "$label: expected signal 2, got ${sig:-empty}"
        echo "--- stdout ---" >&2
        cat "$out" >&2 || true
        return
      fi
      if [[ "$status" -ne 0 ]]; then
        fail "$label: expected exit 0 after Ctrl+C, got $status"
        return
      fi
      ;;
  esac
  pass "$label"
}

run_synthetic() {
  local i name cmd
  for i in "${!SHELL_NAMES[@]}"; do
    name=${SHELL_NAMES[$i]}
    cmd=${SHELL_CMDS[$i]}
    run_case "stand-in $name SIGINT before player" "$cmd" "$WORK/runtime" pre-int
    run_case "stand-in $name SIGTERM AppImage runtime" "$cmd" "$WORK/runtime" term
    run_case "stand-in $name SIGINT after player" "$cmd" "$WORK/runtime" post-int
    run_case "stand-in $name SIGTERM bare ELF" "$cmd" "$WORK/payload" term
  done
  # One HUP of the shell alone, dash when present, else bash.
  local hup_name=bash
  local hup_cmd=bash
  local i
  for i in "${!SHELL_NAMES[@]}"; do
    if [[ ${SHELL_NAMES[$i]} == dash ]]; then
      hup_name=dash
      hup_cmd=${SHELL_CMDS[$i]}
    fi
  done
  run_case "stand-in $hup_name SIGHUP after player" "$hup_cmd" "$WORK/runtime" hup
}

# Two signals during the 10s stop window. The payload ignores TERM, so the
# first restore is still inside stop_player when the second arrives.
run_double_signal() {
  local name=$1
  local shell_cmd=$2
  local label="stand-in $name second TERM during Kodi restore"
  local case_dir
  case_dir=$(mktemp -d "$WORK/case.XXXXXX")
  local log="$case_dir/systemctl.log"
  local ready="$case_dir/ready"
  local out="$case_dir/out"
  local err="$case_dir/err"
  : >"$log"

  SYSTEMCTL_LOG=$log
  SYSTEMCTL_STATE=
  SYSTEMCTL_BLOCK_STOP=0
  READY_FILE=$ready
  SIGNAL_FILE=
  STUB_PAYLOAD=
  STUB_TREE=
  TREE_DIR=
  PLAYER_BIN=$WORK/payload-ignore
  PLAYER_EXTRACT_DIR=$case_dir/extract
  MQTT_HOST=
  MQTT_PORT=
  start_shell "$shell_cmd" "$out" "$err"
  local shell_pid=$STARTED_PID

  if ! wait_file "$ready"; then
    fail "$label: player did not become ready"
    cat "$err" >&2 || true
    kill -s TERM "$shell_pid" 2>/dev/null || true
    wait "$shell_pid" 2>/dev/null || true
    return
  fi
  local payload_pid
  payload_pid=$(cat "$ready")
  kill -s TERM "$shell_pid"
  sleep 2
  if ! kill -0 "$shell_pid" 2>/dev/null; then
    fail "$label: shell exited before the second signal"
    cat "$log" >&2 || true
    return
  fi
  kill -s TERM "$shell_pid" 2>/dev/null || true

  local i=0
  while kill -0 "$shell_pid" 2>/dev/null && [[ $i -lt 200 ]]; do
    sleep 0.1
    i=$((i + 1))
  done
  if kill -0 "$shell_pid" 2>/dev/null; then
    fail "$label: shell still running"
    kill -s KILL -- "-$shell_pid" 2>/dev/null || true
    wait "$shell_pid" 2>/dev/null || true
    return
  fi
  local status=0
  wait "$shell_pid" || status=$?

  local starts
  starts=$(count_starts "$log")
  if [[ "$starts" != "1" ]]; then
    fail "$label: expected one 'start kodi', got ${starts:-0}"
    cat "$log" >&2 || true
    cat "$err" >&2 || true
    return
  fi
  if [[ "$status" -ne 0 ]]; then
    fail "$label: expected exit 0, got $status"
    return
  fi
  if kill -0 "$payload_pid" 2>/dev/null; then
    fail "$label: payload $payload_pid still running"
    kill -s KILL "$payload_pid" 2>/dev/null || true
    return
  fi
  pass "$label"
}

# PLAYER_EXTRACT_DIR must not be rm -rf'd, and a dangerous value is refused
# before anything is removed.
run_extract_guards() {
  local shell_cmd=$1
  local label_shell=$2

  local safe
  safe=$(mktemp -d "$WORK/safe.XXXXXX")
  local extract="$safe/extract"
  mkdir -p "$extract"
  echo keep > "$extract/keep-me"
  local log="$safe/systemctl.log"
  local ready="$safe/ready"
  local out="$safe/out"
  local err="$safe/err"
  : >"$log"
  SYSTEMCTL_LOG=$log
  SYSTEMCTL_STATE=
  SYSTEMCTL_BLOCK_STOP=0
  READY_FILE=$ready
  SIGNAL_FILE=
  STUB_PAYLOAD=$WORK/payload
  STUB_TREE=
  TREE_DIR=
  PLAYER_BIN=$WORK/runtime
  PLAYER_EXTRACT_DIR=$extract
  MQTT_HOST=
  MQTT_PORT=
  start_shell "$shell_cmd" "$out" "$err"
  local shell_pid=$STARTED_PID
  if ! wait_file "$ready"; then
    fail "$label_shell extract sentinel: player did not become ready"
    cat "$err" >&2 || true
    kill -s TERM "$shell_pid" 2>/dev/null || true
    wait "$shell_pid" 2>/dev/null || true
    return
  fi
  if [[ ! -f "$extract/keep-me" ]]; then
    fail "$label_shell extract sentinel: keep-me was removed"
  elif [[ ! -x "$extract/squashfs-root/AppRun" ]]; then
    fail "$label_shell extract sentinel: AppRun missing"
  else
    pass "$label_shell extract sentinel kept"
  fi
  kill -s TERM "$shell_pid" 2>/dev/null || true
  wait "$shell_pid" 2>/dev/null || true

  refuse_extract() {
    local why=$1
    local dir_value=$2
    local case_dir
    case_dir=$(mktemp -d "$WORK/refuse.XXXXXX")
    local sentinel="$case_dir/sentinel"
    echo keep > "$sentinel"
    local rlog="$case_dir/systemctl.log"
    local rout="$case_dir/out"
    local rerr="$case_dir/err"
    : >"$rlog"
    SYSTEMCTL_LOG=$rlog
    SYSTEMCTL_STATE=
    SYSTEMCTL_BLOCK_STOP=0
    READY_FILE=
    SIGNAL_FILE=
    STUB_PAYLOAD=
    STUB_TREE=
    TREE_DIR=
    PLAYER_BIN=$WORK/runtime
    PLAYER_EXTRACT_DIR=$dir_value
    MQTT_HOST=
    MQTT_PORT=
    start_shell "$shell_cmd" "$rout" "$rerr"
    local pid=$STARTED_PID
    local i=0
    while kill -0 "$pid" 2>/dev/null && [[ $i -lt 50 ]]; do
      sleep 0.1
      i=$((i + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
      fail "$label_shell refuse $why: shell still running"
      kill -s KILL "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
      return
    fi
    local status=0
    wait "$pid" || status=$?
    if [[ "$status" -eq 0 ]]; then
      fail "$label_shell refuse $why: expected non-zero exit"
      cat "$rerr" >&2 || true
      return
    fi
    if ! grep -q 'refusing' "$rerr"; then
      fail "$label_shell refuse $why: stderr did not say refusing"
      cat "$rerr" >&2 || true
      return
    fi
    if grep -q 'stop kodi' "$rlog"; then
      fail "$label_shell refuse $why: stopped Kodi"
      cat "$rlog" >&2 || true
      return
    fi
    if [[ ! -f "$sentinel" || ! -x "$WORK/runtime" ]]; then
      fail "$label_shell refuse $why: sentinel or runtime was removed"
      return
    fi
    pass "$label_shell refuse $why"
  }

  local ancestor
  ancestor=$(mktemp -d "$WORK/ancestor.XXXXXX")
  mkdir -p "$ancestor/bin"
  cp "$WORK/runtime" "$ancestor/bin/player"
  echo keep > "$ancestor/keep-me"
  local alog="$ancestor/systemctl.log"
  local aout="$ancestor/out"
  local aerr="$ancestor/err"
  : >"$alog"
  SYSTEMCTL_LOG=$alog
  PLAYER_BIN=$ancestor/bin/player
  PLAYER_EXTRACT_DIR=$ancestor
  start_shell "$shell_cmd" "$aout" "$aerr"
  local apid=$STARTED_PID
  local i=0
  while kill -0 "$apid" 2>/dev/null && [[ $i -lt 50 ]]; do
    sleep 0.1
    i=$((i + 1))
  done
  if kill -0 "$apid" 2>/dev/null; then
    fail "$label_shell refuse ancestor: shell still running"
    kill -s KILL "$apid" 2>/dev/null || true
    wait "$apid" 2>/dev/null || true
  else
    local status=0
    wait "$apid" || status=$?
    if [[ "$status" -eq 0 ]] || ! grep -q 'refusing' "$aerr"; then
      fail "$label_shell refuse ancestor: expected refusal"
      cat "$aerr" >&2 || true
    elif [[ ! -f "$ancestor/keep-me" || ! -x "$ancestor/bin/player" ]]; then
      fail "$label_shell refuse ancestor: player or sentinel removed"
    elif grep -q 'stop kodi' "$alog"; then
      fail "$label_shell refuse ancestor: stopped Kodi"
    else
      pass "$label_shell refuse ancestor"
    fi
  fi

  refuse_extract "empty" ""
  refuse_extract "root" "/"
  refuse_extract "storage" "/storage"
  refuse_extract "storage-parent" "/storage/.."
}

# AppRun forks a child with a grandchild and a second child. All of them
# must die on one TERM, including under dash, bash, and busybox ash.
run_signal_tree() {
  local name=$1
  local shell_cmd=$2
  local label="stand-in $name signal_tree forks"
  local case_dir
  case_dir=$(mktemp -d "$WORK/tree.XXXXXX")
  local tree="$case_dir/pids"
  mkdir -p "$tree"
  local log="$case_dir/systemctl.log"
  local ready="$tree/root"
  local out="$case_dir/out"
  local err="$case_dir/err"
  : >"$log"
  SYSTEMCTL_LOG=$log
  SYSTEMCTL_STATE=
  SYSTEMCTL_BLOCK_STOP=0
  READY_FILE=
  SIGNAL_FILE=
  STUB_PAYLOAD=$WORK/payload
  STUB_TREE=1
  TREE_DIR=$tree
  PLAYER_BIN=$WORK/runtime
  PLAYER_EXTRACT_DIR=$case_dir/extract
  MQTT_HOST=
  MQTT_PORT=
  start_shell "$shell_cmd" "$out" "$err"
  local shell_pid=$STARTED_PID

  local i=0
  while [[ $i -lt 50 ]]; do
    if [[ -s "$tree/root" && -s "$tree/c1" && -s "$tree/c2" && -s "$tree/g" ]]; then
      break
    fi
    sleep 0.1
    i=$((i + 1))
  done
  if [[ ! -s "$tree/root" || ! -s "$tree/c1" || ! -s "$tree/c2" || ! -s "$tree/g" ]]; then
    fail "$label: process tree did not record pids"
    cat "$err" >&2 || true
    kill -s TERM "$shell_pid" 2>/dev/null || true
    wait "$shell_pid" 2>/dev/null || true
    return
  fi
  local root c1 c2 g
  root=$(cat "$tree/root")
  c1=$(cat "$tree/c1")
  c2=$(cat "$tree/c2")
  g=$(cat "$tree/g")
  local started=$SECONDS
  kill -s TERM "$shell_pid"
  i=0
  while kill -0 "$shell_pid" 2>/dev/null && [[ $i -lt 80 ]]; do
    sleep 0.1
    i=$((i + 1))
  done
  local elapsed=$((SECONDS - started))
  if kill -0 "$shell_pid" 2>/dev/null; then
    fail "$label: shell still running after ${elapsed}s"
    kill -s KILL -- "-$shell_pid" 2>/dev/null || true
    wait "$shell_pid" 2>/dev/null || true
    return
  fi
  local status=0
  wait "$shell_pid" || status=$?
  local starts
  starts=$(count_starts "$log")
  local alive=""
  for pid in "$root" "$c1" "$c2" "$g"; do
    if kill -0 "$pid" 2>/dev/null; then
      alive="$alive $pid"
      kill -s KILL "$pid" 2>/dev/null || true
    fi
  done
  if [[ "$starts" != "1" || "$status" -ne 0 || -n "$alive" || "$elapsed" -ge 8 ]]; then
    fail "$label: starts=${starts:-0} status=$status elapsed=${elapsed}s alive=${alive:-none}"
    cat "$log" >&2 || true
    cat "$err" >&2 || true
    return
  fi
  pass "$label"
}

run_real_player_case() {
  local label=$1
  local shell_cmd=$2
  local player=$3
  local mode=$4
  local case_dir
  case_dir=$(mktemp -d "$WORK/real.XXXXXX")
  local log="$case_dir/systemctl.log"
  local out="$case_dir/out"
  local err="$case_dir/err"
  : >"$log"
  local shell_pid
  SYSTEMCTL_LOG=$log
  SYSTEMCTL_STATE=
  SYSTEMCTL_BLOCK_STOP=0
  READY_FILE=
  SIGNAL_FILE=
  STUB_PAYLOAD=
  STUB_TREE=
  TREE_DIR=
  PLAYER_BIN=$player
  PLAYER_EXTRACT_DIR=$REAL_EXTRACT
  MQTT_HOST=127.0.0.1
  start_shell "$shell_cmd" "$out" "$err"
  shell_pid=$STARTED_PID

  # stdout is block-buffered when it is not a tty, so the "Waiting for
  # SIGINT" line may not hit the log until the player exits. Readiness is
  # the player process itself: AppRun (or the bare ELF) parented by the shell.
  local player_pid="" exe
  local i=0
  while [[ $i -lt 300 ]]; do
    if ! kill -0 "$shell_pid" 2>/dev/null; then
      fail "$label: shell exited before the player was ready"
      echo "--- stdout ---" >&2
      cat "$out" >&2 || true
      echo "--- stderr ---" >&2
      cat "$err" >&2 || true
      return
    fi
    player_pid=""
    # Direct children only. Scanning every /proc entry made this loop take
    # minutes on a GitHub runner when the exe path did not match.
    local child
    for child in $(cat "/proc/$shell_pid/task/$shell_pid/children" 2>/dev/null || true); do
      exe=$(readlink -f "/proc/$child/exe" 2>/dev/null || true)
      [[ -n "$exe" ]] || continue
      case "$exe" in
        "$REAL_EXTRACT"/*)
          player_pid=$child
          break
          ;;
      esac
      # --appimage-extract is the type-2 runtime. A subshell may exec it,
      # so it shows up as a child, then exits when unpack finishes.
      if [[ "$exe" == "$player" && "$player" != "$APPIMAGE" ]]; then
        player_pid=$child
        break
      fi
    done
    if [[ -n "$player_pid" ]]; then
      break
    fi
    sleep 0.1
    i=$((i + 1))
  done
  if [[ -z "$player_pid" ]]; then
    fail "$label: player is not a direct child of the launch script"
    echo "--- stdout ---" >&2
    cat "$out" >&2 || true
    echo "--- stderr ---" >&2
    cat "$err" >&2 || true
    kill -s TERM "$shell_pid" 2>/dev/null || true
    wait "$shell_pid" 2>/dev/null || true
    return
  fi

  # Library load of the bundled FFmpeg is not instant. Wait until the
  # player is blocked in sigwait so SIGTERM is the shutdown path, not an
  # early default kill that skips the log line.
  local wchan=""
  i=0
  while [[ $i -lt 100 ]]; do
    wchan=$(cat "/proc/$player_pid/wchan" 2>/dev/null || true)
    case "$wchan" in
      *sigwait* | *sigtimedwait*) break ;;
    esac
    if ! kill -0 "$player_pid" 2>/dev/null; then
      break
    fi
    sleep 0.1
    i=$((i + 1))
  done
  case "$wchan" in
    *sigwait* | *sigtimedwait*) ;;
    *)
      fail "$label: player $player_pid did not reach sigwait (wchan=${wchan:-empty})"
      kill -s TERM "$shell_pid" 2>/dev/null || true
      wait "$shell_pid" 2>/dev/null || true
      return
      ;;
  esac

  case "$mode" in
    term) kill -s TERM "$shell_pid" ;;
    post-int) kill -s INT -- "-$shell_pid" ;;
  esac

  i=0
  while kill -0 "$shell_pid" 2>/dev/null && [[ $i -lt 200 ]]; do
    sleep 0.1
    i=$((i + 1))
  done
  if kill -0 "$shell_pid" 2>/dev/null; then
    fail "$label: shell still running"
    kill -s KILL -- "-$shell_pid" 2>/dev/null || true
    wait "$shell_pid" 2>/dev/null || true
    return
  fi
  local status=0
  wait "$shell_pid" || status=$?

  local starts
  starts=$(count_starts "$log")
  if [[ "$starts" != "1" ]]; then
    fail "$label: expected one 'start kodi', got ${starts:-0}"
    cat "$log" >&2 || true
    return
  fi

  # The player binary, AppRun, or a leftover runtime must be gone.
  # Init reaps a zombie the shell has not waited for; give that a moment.
  local leftover=0
  local n=0 pid exe
  while [[ $n -lt 10 ]]; do
    n=$((n + 1))
    leftover=0
    for pid_path in /proc/[0-9]*; do
      pid=${pid_path#/proc/}
      [[ "$pid" =~ ^[0-9]+$ ]] || continue
      exe=$(readlink -f "/proc/$pid/exe" 2>/dev/null || true)
      case "$exe" in
        "$REAL_EXTRACT"/*|"$player")
          leftover=1
          echo "leftover pid $pid exe $exe" >&2
          ;;
      esac
    done
    if [[ "$leftover" -eq 0 ]]; then
      break
    fi
    sleep 0.1
  done
  if [[ "$leftover" -ne 0 ]]; then
    fail "$label: player still running after cleanup"
    return
  fi
  if [[ "$mode" == "post-int" && "$status" -ne 0 ]]; then
    fail "$label: expected exit 0, got $status"
    return
  fi
  if [[ "$mode" == "term" && "$status" -ne 0 ]]; then
    fail "$label: expected exit 0, got $status"
    return
  fi
  if ! grep -q 'MQTT shutting down on signal' "$out"; then
    fail "$label: player did not log shutdown"
    cat "$out" >&2 || true
    return
  fi
  if [[ "$mode" == "term" ]] && ! grep -q 'MQTT shutting down on signal 15' "$out"; then
    fail "$label: expected signal 15"
    cat "$out" >&2 || true
    return
  fi
  if [[ "$mode" == "post-int" ]] && ! grep -q 'MQTT shutting down on signal 2' "$out"; then
    fail "$label: expected signal 2"
    cat "$out" >&2 || true
    return
  fi
  pass "$label"
}

run_appimage() {
  [[ -n "$APPIMAGE" ]] || return 0
  [[ -x "$APPIMAGE" ]] || die "AppImage is not executable: $APPIMAGE"
  if ! command -v mosquitto >/dev/null 2>&1 && [[ -x /usr/sbin/mosquitto ]]; then
    export PATH="/usr/sbin:$PATH"
  fi
  need mosquitto
  local port=$((20000 + RANDOM % 20000))
  MQTT_PORT=$port
  cat > "$WORK/mosquitto.conf" <<EOF
listener ${port} 127.0.0.1
allow_anonymous true
persistence false
log_dest stderr
EOF
  mosquitto -c "$WORK/mosquitto.conf" >"$WORK/mosquitto.log" 2>&1 &
  BROKER_PID=$!
  local i=0
  while [[ $i -lt 50 ]]; do
    if (echo >/dev/tcp/127.0.0.1/"$port") 2>/dev/null; then
      break
    fi
    if ! kill -0 "$BROKER_PID" 2>/dev/null; then
      cat "$WORK/mosquitto.log" >&2 || true
      die "mosquitto failed to start"
    fi
    sleep 0.1
    i=$((i + 1))
  done
  if ! (echo >/dev/tcp/127.0.0.1/"$port") 2>/dev/null; then
    cat "$WORK/mosquitto.log" >&2 || true
    die "mosquitto did not accept connections on $port"
  fi
  REAL_EXTRACT=$(mktemp -d /tmp/amlogic-launch-extract.XXXXXX)

  local i name cmd
  # Extract is cached in REAL_EXTRACT. One shell is enough to lock the
  # real runtime; dash matches CoreELEC's /bin/sh most closely on Debian,
  # and bash plus busybox cover the other two traps.
  for i in "${!SHELL_NAMES[@]}"; do
    name=${SHELL_NAMES[$i]}
    cmd=${SHELL_CMDS[$i]}
    run_real_player_case "AppImage $name SIGTERM" "$cmd" "$APPIMAGE" term
    run_real_player_case "AppImage $name SIGINT after player" "$cmd" "$APPIMAGE" post-int
  done
  if [[ -n "$BARE" ]]; then
    [[ -x "$BARE" ]] || die "bare ELF is not executable: $BARE"
    run_real_player_case "bare ELF ${SHELL_NAMES[0]} SIGTERM" "${SHELL_CMDS[0]}" "$BARE" term
    local i
    for i in "${!SHELL_NAMES[@]}"; do
      if [[ ${SHELL_NAMES[$i]} == dash ]]; then
        run_real_player_case "bare ELF dash SIGTERM" "${SHELL_CMDS[$i]}" "$BARE" term
      fi
    done
  fi
  kill "$BROKER_PID" 2>/dev/null || true
  wait "$BROKER_PID" 2>/dev/null || true
  BROKER_PID=
}

if [[ "$ONLY_APPIMAGE" -eq 0 ]]; then
  run_synthetic
  for i in "${!SHELL_NAMES[@]}"; do
    run_double_signal "${SHELL_NAMES[$i]}" "${SHELL_CMDS[$i]}"
    run_signal_tree "${SHELL_NAMES[$i]}" "${SHELL_CMDS[$i]}"
  done
  # Extract guards are shell-independent aside from the trap syntax. Dash
  # when present, otherwise bash.
  guard_cmd=bash
  guard_name=bash
  for i in "${!SHELL_NAMES[@]}"; do
    if [[ ${SHELL_NAMES[$i]} == dash ]]; then
      guard_cmd=${SHELL_CMDS[$i]}
      guard_name=dash
    fi
  done
  run_extract_guards "$guard_cmd" "$guard_name"
fi
if [[ -n "$APPIMAGE" ]]; then
  run_appimage
fi

if [[ "$failures" -ne 0 ]]; then
  echo "$failures launch-script check(s) failed" >&2
  exit 1
fi
echo "launch-coreelec checks ok"
