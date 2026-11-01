#!/usr/bin/env bash
# Broker-down retry, SIGHUP clean status, a SIGKILL that still leaves the
# last retained status line, and a non-zero exit after repeated CONNACK
# authentication refusals. Requires the player binary and mosquitto.
# Exit 77 when either is missing so CTest can skip.

set -euo pipefail

die() {
  echo "FAIL $*" >&2
  if [[ -d "${WORK:-}" ]]; then
    local f
    for f in "$WORK"/*; do
      [[ -f "$f" ]] || continue
      [[ "$(basename "$f")" == "mqtt-helper" ]] && continue
      echo "--- $(basename "$f") ---" >&2
      cat "$f" >&2 || true
    done
  fi
  exit 1
}

PLAYER=${1:-}
if [[ -z "$PLAYER" || ! -x "$PLAYER" ]]; then
  echo "SKIP player binary not available"
  exit 77
fi
if ! command -v mosquitto >/dev/null 2>&1; then
  if [[ -x /usr/sbin/mosquitto ]]; then
    export PATH="/usr/sbin:$PATH"
  fi
fi
if ! command -v mosquitto >/dev/null 2>&1 || ! command -v g++ >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP mosquitto, g++, or python3 not available"
  exit 77
fi

WORK=$(mktemp -d /tmp/amlogic-mqtt-reconnect.XXXXXX)
BROKER_PID=
PLAYER_PID=
cleanup() {
  if [[ -n "${PLAYER_PID:-}" ]]; then
    kill -s KILL "$PLAYER_PID" 2>/dev/null || true
    wait "$PLAYER_PID" 2>/dev/null || true
  fi
  if [[ -n "${BROKER_PID:-}" ]]; then
    kill "$BROKER_PID" 2>/dev/null || true
    wait "$BROKER_PID" 2>/dev/null || true
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

g++ -std=c++17 -Wall -Wextra -O2 -o "$WORK/mqtt-helper" -x c++ - -lmosquitto <<'EOF'
#include <mosquitto.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <cstdlib>

static std::string g_log;

static void on_message(struct mosquitto*, void*, const struct mosquitto_message* msg) {
    if (msg == nullptr || msg->topic == nullptr) return;
    std::ofstream out(g_log, std::ios::app);
    out << msg->topic << " " << (msg->retain ? "1" : "0") << " ";
    if (msg->payload != nullptr && msg->payloadlen > 0) {
        out.write(static_cast<const char*>(msg->payload), msg->payloadlen);
    }
    out << "\n";
}

static int publish(int argc, char** argv) {
    if (argc != 7) return 2;
    const char* host = argv[2];
    const int port = std::atoi(argv[3]);
    const char* topic = argv[4];
    const char* payload = argv[5];
    const bool retain = std::string(argv[6]) == "1";
    mosquitto_lib_init();
    struct mosquitto* mosq = mosquitto_new("amlogic-mqtt-helper-pub", true, nullptr);
    if (mosq == nullptr) return 3;
    const int rc = mosquitto_connect(mosq, host, port, 30);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "helper connect " << mosquitto_strerror(rc) << "\n";
        return 4;
    }
    const int pr = mosquitto_publish(mosq, nullptr, topic, static_cast<int>(std::strlen(payload)),
                                     payload, 1, retain);
    if (pr != MOSQ_ERR_SUCCESS) {
        std::cerr << "helper publish " << mosquitto_strerror(pr) << "\n";
        return 5;
    }
    for (int i = 0; i < 20; ++i) {
        if (mosquitto_loop(mosq, 100, 1) != MOSQ_ERR_SUCCESS) break;
    }
    mosquitto_disconnect(mosq);
    mosquitto_loop(mosq, 100, 1);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}

static int subscribe(int argc, char** argv) {
    if (argc != 7) return 2;
    const char* host = argv[2];
    const int port = std::atoi(argv[3]);
    const char* topic = argv[4];
    const int seconds = std::atoi(argv[5]);
    g_log = argv[6];
    mosquitto_lib_init();
    struct mosquitto* mosq = mosquitto_new("amlogic-mqtt-helper-sub", true, nullptr);
    if (mosq == nullptr) return 3;
    mosquitto_message_callback_set(mosq, on_message);
    const int rc = mosquitto_connect(mosq, host, port, 30);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "helper connect " << mosquitto_strerror(rc) << "\n";
        return 4;
    }
    const int sr = mosquitto_subscribe(mosq, nullptr, topic, 1);
    if (sr != MOSQ_ERR_SUCCESS) {
        std::cerr << "helper subscribe " << mosquitto_strerror(sr) << "\n";
        return 5;
    }
    const int loops = seconds * 10;
    for (int i = 0; i < loops; ++i) {
        if (mosquitto_loop(mosq, 100, 1) != MOSQ_ERR_SUCCESS) break;
    }
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    const std::string cmd = argv[1];
    if (cmd == "pub") return publish(argc, argv);
    if (cmd == "sub") return subscribe(argc, argv);
    return 2;
}
EOF

wait_for() {
  local file=$1
  local needle=$2
  local tries=$3
  local i=0
  while [[ $i -lt $tries ]]; do
    if [[ -f "$file" ]] && grep -q "$needle" "$file"; then
      return 0
    fi
    sleep 0.1
    i=$((i + 1))
  done
  return 1
}

stop_player() {
  if [[ -n "${PLAYER_PID:-}" ]]; then
    kill -s KILL "$PLAYER_PID" 2>/dev/null || true
    wait "$PLAYER_PID" 2>/dev/null || true
    PLAYER_PID=
  fi
}

start_player() {
  local id=$1
  local out=$2
  local err=$3
  "$PLAYER" \
    --mqtt-host 127.0.0.1 \
    --mqtt-port "$PORT" \
    --mqtt-prefix "$PREFIX" \
    --mqtt-client-id "$id" \
    >"$out" 2>"$err" &
  PLAYER_PID=$!
}

PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
PREFIX="amlogic-reconnect-$$"

# TERM during the initial backoff must exit 0 without a broker.
start_player "retry-term-$$" "$WORK/term.out" "$WORK/term.err"
if ! wait_for "$WORK/term.err" "will retry" 30; then
  die "broker-down player did not log a retry"
fi
if ! grep -q "does not drive a display" "$WORK/term.err"; then
  die "stderr did not warn that software mode has no display"
fi
if ! kill -0 "$PLAYER_PID" 2>/dev/null; then
  die "player exited while the broker was down"
fi
kill -s TERM "$PLAYER_PID"
term_pid=$PLAYER_PID
PLAYER_PID=
i=0
while kill -0 "$term_pid" 2>/dev/null && [[ $i -lt 30 ]]; do
  sleep 0.1
  i=$((i + 1))
done
if kill -0 "$term_pid" 2>/dev/null; then
  die "SIGTERM during retry did not stop the player"
fi
term_status=0
wait "$term_pid" || term_status=$?
if [[ "$term_status" -ne 0 ]]; then
  die "SIGTERM during retry exited $term_status"
fi
if ! grep -q "MQTT shutting down on signal 15" "$WORK/term.out"; then
  die "SIGTERM during retry did not log signal 15"
fi

# First connect fails, then the broker appears and the same process connects.
start_player "retry-up-$$" "$WORK/up.out" "$WORK/up.err"
if ! wait_for "$WORK/up.err" "will retry" 30; then
  die "second player did not log a retry before the broker started"
fi
cat > "$WORK/mosquitto.conf" <<EOF
listener ${PORT} 127.0.0.1
allow_anonymous true
persistence false
log_dest stderr
EOF
mosquitto -c "$WORK/mosquitto.conf" >"$WORK/mosquitto.log" 2>&1 &
BROKER_PID=$!
if ! wait_for "$WORK/up.out" "MQTT connected" 200; then
  die "player did not connect after the broker started"
fi

# add, then SIGHUP publishes a retained offline status that still lists the stream.
if ! "$WORK/mqtt-helper" pub 127.0.0.1 "$PORT" "$PREFIX/cmd/add" \
  '{"id":"cam1","url":"http://127.0.0.1/live.m3u8"}' 0; then
  die "add publish failed"
fi
if ! wait_for "$WORK/up.out" "MQTT status .*cam1" 50; then
  die "running player did not flush a status line containing cam1"
fi
if ! kill -0 "$PLAYER_PID" 2>/dev/null; then
  die "player exited before SIGHUP"
fi
kill -s HUP "$PLAYER_PID"
hup_pid=$PLAYER_PID
PLAYER_PID=
i=0
while kill -0 "$hup_pid" 2>/dev/null && [[ $i -lt 50 ]]; do
  sleep 0.1
  i=$((i + 1))
done
if kill -0 "$hup_pid" 2>/dev/null; then
  die "SIGHUP did not stop the player"
fi
hup_status=0
wait "$hup_pid" || hup_status=$?
if [[ "$hup_status" -ne 0 ]]; then
  die "SIGHUP exited $hup_status"
fi
if ! grep -q "MQTT shutting down on signal 1" "$WORK/up.out"; then
  die "SIGHUP did not log signal 1"
fi
if ! "$WORK/mqtt-helper" sub 127.0.0.1 "$PORT" "$PREFIX/status" 2 "$WORK/status.log"; then
  die "status subscribe failed"
fi
if ! grep -q '"online":false' "$WORK/status.log" || ! grep -q 'cam1' "$WORK/status.log"; then
  die "retained status after SIGHUP did not keep cam1 offline"
fi

# SIGKILL cannot publish, but the last status line is already flushed.
start_player "kill-$$" "$WORK/kill.out" "$WORK/kill.err"
if ! wait_for "$WORK/kill.out" "MQTT connected" 50; then
  die "third player did not connect"
fi
if ! "$WORK/mqtt-helper" pub 127.0.0.1 "$PORT" "$PREFIX/cmd/add" \
  '{"id":"cam9","url":"http://127.0.0.1/other.m3u8"}' 0; then
  die "second add publish failed"
fi
if ! wait_for "$WORK/kill.out" "MQTT status .*cam9" 50; then
  die "status line was not flushed before SIGKILL"
fi
kill -s KILL "$PLAYER_PID"
kill_pid=$PLAYER_PID
PLAYER_PID=
wait "$kill_pid" 2>/dev/null || true
if ! grep -q "MQTT status .*cam9" "$WORK/kill.out"; then
  die "SIGKILL lost the last status line"
fi

# A CONNACK of not authorised must exit non-zero. A down broker keeps
# retrying; this one answers and refuses, which used to loop with Kodi
# stopped. Three attempts, then the process exits.
if [[ -n "${BROKER_PID:-}" ]]; then
  kill "$BROKER_PID" 2>/dev/null || true
  wait "$BROKER_PID" 2>/dev/null || true
  BROKER_PID=
fi
cat > "$WORK/mosquitto-auth.conf" <<EOF
listener ${PORT} 127.0.0.1
allow_anonymous false
persistence false
log_dest stderr
EOF
mosquitto -c "$WORK/mosquitto-auth.conf" >"$WORK/mosquitto-auth.log" 2>&1 &
BROKER_PID=$!
sleep 0.3
if ! kill -0 "$BROKER_PID" 2>/dev/null; then
  die "auth broker did not start"
fi

wait_player_exit() {
  local pid=$1
  local i=0
  while kill -0 "$pid" 2>/dev/null && [[ $i -lt 150 ]]; do
    sleep 0.1
    i=$((i + 1))
  done
  if kill -0 "$pid" 2>/dev/null; then
    return 1
  fi
  return 0
}

"$PLAYER" \
  --mqtt-host 127.0.0.1 \
  --mqtt-port "$PORT" \
  --mqtt-prefix "$PREFIX" \
  --mqtt-client-id "anon-$$" \
  >"$WORK/anon.out" 2>"$WORK/anon.err" &
PLAYER_PID=$!
if ! wait_player_exit "$PLAYER_PID"; then
  die "anonymous client kept running after not authorised"
fi
anon_pid=$PLAYER_PID
PLAYER_PID=
anon_status=0
wait "$anon_pid" || anon_status=$?
if [[ "$anon_status" -eq 0 ]]; then
  die "anonymous client exited 0 after not authorised"
fi
if ! grep -q "not authorised" "$WORK/anon.err"; then
  die "anonymous client did not log not authorised"
fi
if ! grep -q "MQTT authentication failed after 3 attempts; exiting" "$WORK/anon.err"; then
  die "anonymous client did not exit after 3 authentication attempts"
fi
if grep -q "MQTT connected" "$WORK/anon.out"; then
  die "anonymous client connected to a broker that refuses anonymous clients"
fi

# Wrong password is the other CONNACK the add-on has to stop on. Mosquitto
# may phrase it as "bad user name or password" or "not authorised".
if ! command -v mosquitto_passwd >/dev/null 2>&1; then
  if [[ "${CI:-}" == "true" ]]; then
    die "CI requires mosquitto_passwd"
  fi
  echo "skip bad-password check (mosquitto_passwd not installed)"
else
  if [[ -n "${BROKER_PID:-}" ]]; then
    kill "$BROKER_PID" 2>/dev/null || true
    wait "$BROKER_PID" 2>/dev/null || true
    BROKER_PID=
  fi
  mosquitto_passwd -b -c "$WORK/pwfile" player 'right-secret' >/dev/null
  chmod 0600 "$WORK/pwfile"
  cat > "$WORK/mosquitto-pw.conf" <<EOF
listener ${PORT} 127.0.0.1
allow_anonymous false
password_file ${WORK}/pwfile
persistence false
log_dest stderr
EOF
  mosquitto -c "$WORK/mosquitto-pw.conf" >"$WORK/mosquitto-pw.log" 2>&1 &
  BROKER_PID=$!
  sleep 0.3
  if ! kill -0 "$BROKER_PID" 2>/dev/null; then
    die "password broker did not start"
  fi
  "$PLAYER" \
    --mqtt-host 127.0.0.1 \
    --mqtt-port "$PORT" \
    --mqtt-prefix "$PREFIX" \
    --mqtt-client-id "badpw-$$" \
    --mqtt-user player \
    --mqtt-pass 'wrong-secret' \
    >"$WORK/badpw.out" 2>"$WORK/badpw.err" &
  PLAYER_PID=$!
  if ! wait_player_exit "$PLAYER_PID"; then
    die "wrong password kept the player running"
  fi
  bad_pid=$PLAYER_PID
  PLAYER_PID=
  bad_status=0
  wait "$bad_pid" || bad_status=$?
  if [[ "$bad_status" -eq 0 ]]; then
    die "wrong password exited 0"
  fi
  if ! grep -Eq "not authorised|bad user name or password|bad username or password" "$WORK/badpw.err"; then
    die "wrong password did not log an authentication refusal"
  fi
  if ! grep -q "MQTT authentication failed after 3 attempts; exiting" "$WORK/badpw.err"; then
    die "wrong password did not exit after 3 authentication attempts"
  fi

  "$PLAYER" \
    --mqtt-host 127.0.0.1 \
    --mqtt-port "$PORT" \
    --mqtt-prefix "$PREFIX" \
    --mqtt-client-id "goodpw-$$" \
    --mqtt-user player \
    --mqtt-pass 'right-secret' \
    >"$WORK/goodpw.out" 2>"$WORK/goodpw.err" &
  PLAYER_PID=$!
  if ! wait_for "$WORK/goodpw.out" "MQTT connected" 50; then
    die "correct password did not connect"
  fi
  kill -s TERM "$PLAYER_PID"
  good_pid=$PLAYER_PID
  PLAYER_PID=
  i=0
  while kill -0 "$good_pid" 2>/dev/null && [[ $i -lt 30 ]]; do
    sleep 0.1
    i=$((i + 1))
  done
  if kill -0 "$good_pid" 2>/dev/null; then
    die "SIGTERM after a correct login did not stop the player"
  fi
  good_status=0
  wait "$good_pid" || good_status=$?
  if [[ "$good_status" -ne 0 ]]; then
    die "correct password then SIGTERM exited $good_status"
  fi
fi

echo "mqtt reconnect checks ok"
