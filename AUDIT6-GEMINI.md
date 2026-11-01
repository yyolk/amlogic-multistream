# AUDIT6-GEMINI

Independent second-look audit of `yyolk/amlogic-multistream` at `main` tip `6d9d146ba693d9b335dc13733d3ab146d542d45e`, following AUDIT5 fixes #33 and #34.

## Executive Summary

The fixes from AUDIT5 (F1, X1, F3) are implemented correctly and effectively address the identified issues. No new must-fix bugs or race conditions were introduced by these changes. The exit paths, dead-broker timing, and reaper caps behave as intended.

**Note:** Hardware decode is blocked in this build (Host/CI only).

## Verification of AUDIT5 Claims

### F1: Closing Flag (Verified)
- **Claim:** A command delivered during the exit drain must not republish `"online": true`, which the DISCONNECT would then keep retained.
- **Implementation:** In `src/mqtt_controller.cpp:309`, `closing_ = true` is set under `closeMu_` before the final offline publish. Both `handleConnect` (line 461) and `handleMessage` (line 490) check this flag under `closeMu_` and return early if true.
- **Result:** This correctly prevents any command or reconnect during the drain from publishing `"online": true` or processing commands. The final retained status remains correctly offline.

### X1: Exit without `mosquitto_loop_stop` (Verified)
- **Claim:** The exit must not join libmosquitto's network thread while a reconnect is stuck in `getaddrinfo` or `connect()`.
- **Implementation:** In `src/mqtt_controller.cpp:328`, `mosquitto_loop_stop` is no longer called. Instead, if the offline publish was queued on a live connection, it sends DISCONNECT, waits up to 500ms for `onDisconnect` (`kDisconnectWait`), flushes stdio, and calls `_exit(exitCode)`.
- **Result:** This correctly avoids the hang. The process exits immediately without waiting for the network thread to finish a stuck DNS lookup or connection attempt. The total exit time is bounded by `drain` (up to 2s) + `kDisconnectWait` (500ms) = 2.5s, which is well within the 10s SIGTERM poll.

### F3: Busy-Retired Cap (Verified)
- **Claim:** Adding and removing streams under new ids must not stack one stuck lookup each without bound.
- **Implementation:** In `src/stream_player.cpp:172`, `busyRetiredCount()` counts the number of workers in `reaper_` that are not `done`. In `tick()` (line 450), if `busyRetired >= kMaxBusyRetired` (9), it skips starting new workers.
- **Result:** This correctly caps the number of stuck threads. The maximum number of decode threads running at once is bounded to 17 (9 active + 8 retired, or 9 retired + 8 active, etc.). The healthy tiles keep presenting, and new ids wait until a stuck open returns.

## Re-checks

- **Exit Paths & Auth Exit 1:** `authGiveUp_` correctly exits 1 after 3 failed attempts (`src/mqtt_controller.cpp:273`). The `exitCode` is correctly passed to `_exit` (`src/mqtt_controller.cpp:329`).
- **Dead-Broker Timing:** The exit path takes at most 2.5s, which is safe for the launch script's 10s SIGTERM poll.
- **Stall Isolation:** Stalled streams only block their own decode threads. The main thread continues to present frames for other streams.
- **Whitelists:** `protocol_whitelist`, `format_whitelist`, and `codec_whitelist` are correctly enforced in `src/ffmpeg_decoder.cpp`.

## Findings

### 1. Pre-connect main-thread DNS hang on SIGTERM (Should-Fix)
- **Location:** `src/mqtt_controller.cpp:229` (`mosquitto_connect_async`)
- **Description:** `mosquitto_connect_async` is called in the main thread before the mosquitto loop starts. It calls `getaddrinfo` synchronously. If the DNS server is unresponsive, the main thread hangs in this lookup. Because signals are blocked process-wide (`pthread_sigmask(SIG_BLOCK, ...)`), a SIGTERM delivered during this time remains pending, and the process will not exit until the DNS timeout expires (which can be minutes).
- **Severity:** Should-fix. This is the only remaining path where the process can hang on SIGTERM.

### 2. Minor race in `disconnectCv_.wait_for` (Nit)
- **Location:** `src/mqtt_controller.cpp:318`
- **Description:** If `onDisconnect` is called *before* `disconnected_ = false` is set in `finishProcess`, `disconnected_` is overwritten to `false`. However, because `mosquitto_disconnect` returns `MOSQ_ERR_NO_CONN` when the state is already disconnected, the `wait_for` block is skipped entirely. Thus, this race is benign and does not cause a hang.
- **Severity:** Nit. No action required.

### 3. `retiredLimitLogged_` reset logic (Nit)
- **Location:** `src/stream_player.cpp:459`
- **Description:** `retiredLimitLogged_` is reset to `false` only when `busyRetired < kMaxBusyRetired` *and* there is a session that needs to start a worker. If the limit drops but no new sessions are added, the flag remains `true`. If the limit is hit again later, the log message won't be printed.
- **Severity:** Nit. Minor logging detail.

## Conclusion

The codebase at `6d9d146` is robust against the previously identified issues. The exit path is now bounded, and the thread explosion issue is resolved. The only remaining notable issue is the pre-connect DNS hang, which should be addressed in a future PR.
