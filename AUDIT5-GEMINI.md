# AUDIT5-GEMINI

Independent second-look audit of `yyolk/amlogic-multistream` at `bf00f31598d1a170b5a17ccc6137b1766c265e10`.

## Findings

| Severity | Finding | File:Line |
| :--- | :--- | :--- |
| **Must-Fix** | `SIGTERM` hangs if the broker is down and `drain()` returns true | `src/mqtt_controller.cpp:177` |
| **Nit** | `exitIfBusy` returning can trigger the same hang | `src/mqtt_controller.cpp:182` |

### 1. `SIGTERM` hangs if the broker is down and `drain()` returns true (Must-Fix)

**Context:** PR #30 (A1/R1) intended to skip `shutdown()` (and its blocking `mosquitto_loop_stop`) when the broker is down, to avoid hanging on a stuck `getaddrinfo` during process exit.

**Bug:** The logic only skips `shutdown()` if `player.drain()` returns `false` (i.e., a decoder worker is stuck). If `player.drain()` returns `true` (e.g., there are no streams, or all streams stopped cleanly), `finishProcess()` returns normally. Then `MqttController::start()` unconditionally calls `shutdown()`. If the broker is down, `mosquitto_loop_stop` will block on the MQTT thread's `getaddrinfo`, causing the process to hang and miss the 10s `SIGTERM` poll.

**Evidence:**
```cpp
// src/mqtt_controller.cpp:177
        if (!player.drain(StreamPlayer::kExitJoinDeadline)) {
            if (sent) {
                shutdown();
            }
            player.exitIfBusy(std::chrono::milliseconds(0), exitCode);
        }
    }; // finishProcess returns if drain() is true

    // ... later in start() ...
    finishProcess(0);
    shutdown(); // Unconditionally called, blocks on mosquitto_loop_stop if broker is down
```

**Recommendation:** If `sent` is false, the process must `_exit(exitCode)` to avoid the blocking `shutdown()`, regardless of whether `drain()` returned true or false.

### 2. `exitIfBusy` returning can trigger the same hang (Nit)

**Context:** In `finishProcess`, `player.exitIfBusy` is called with a 0ms deadline.

**Bug:** `exitIfBusy` internally calls `drain(deadline)`. If the stuck worker finishes in the tiny window between the first `drain()` and `exitIfBusy()`, `drain(0)` will return `true`, and `exitIfBusy` will return instead of calling `_exit()`. This causes `finishProcess` to return, and `start()` will then unconditionally call `shutdown()`. If `sent` was false, this accidentally executes the blocking `shutdown()` that was intentionally skipped.

**Evidence:**
```cpp
// src/mqtt_controller.cpp:182
            player.exitIfBusy(std::chrono::milliseconds(0), exitCode);
```

## Verification of Claims (A1-A5)

- **A1 (Gated MQTT disconnect):** Incomplete. It correctly gates disconnect when a decoder worker is stuck, but fails to gate it when no decoder workers are stuck (see Must-Fix finding).
- **A2 (Auth exit 1):** Verified. `authGiveUp_` triggers `finishProcess(1)`, which correctly propagates the exit code to `_exit(1)` or `return 1`.
- **A3 (reaperBusy no restart pile-up):** Verified. `session.appliedGeneration` is updated *after* the `reaperBusy` check, ensuring `generationChanged` remains true and the new worker is not started until the old one finishes.
- **A4 (dual demuxer allowlist):** Verified. `kTopLevelFormatWhitelist` correctly rejects a top-level `mp3` (even with a cover art that probes as `mjpeg`), while `kFormatWhitelist` allows HLS to open packed-audio renditions.
- **A5 (mp3float):** Verified. `mp3float` is added to `kCodecWhitelist`, fixing the FFmpeg 6.1 probe warning.
