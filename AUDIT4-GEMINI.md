# Gemini audit of `SURVIVING4` and PRs #23, #24, #25 (`main` @ `58a2f9c`)

Audited `main` commit: `58a2f9c114daf7835c7b2b3c8c3201b9a350705c`
("Stop a stuck DNS lookup from blocking the presenting thread (#25)").

Inputs compared:
- Unfiltered review: `REVIEW4.md` on `cursor/review4-unfiltered-9f4e` ([PR #21](https://github.com/yyolk/amlogic-multistream/pull/21), against `6641ca8`)
- Adversarial filter: `SURVIVING4.md` on `cursor/surviving4-filter-9dcd` ([PR #22](https://github.com/yyolk/amlogic-multistream/pull/22), against `6641ca8`)
- Merged fixes on `main`:
  - [PR #23](https://github.com/yyolk/amlogic-multistream/pull/23) (`9e5d696`) — H1 packed-audio HLS format whitelist
  - [PR #24](https://github.com/yyolk/amlogic-multistream/pull/24) (`6b41a9a`) — K1 Kodi add-on MQTT credentials + non-zero exit after 3 auth CONNACK refusals
  - [PR #25](https://github.com/yyolk/amlogic-multistream/pull/25) (`58a2f9c`) — T1 decoder worker reaper + 2 s shutdown join deadline (`_exit(0)`)

---

## Summary / Must-fix list

- **blocker:** none
- **major (fix regression in #25, plus dropped CI2 coverage gap):**
  1. **R1 (major, `[repro]`): PR #25 calls `player.exitIfBusy()` (`_exit(0)`) before `MqttController::shutdown()` (`mosquitto_disconnect()`), so on a DNS-stalled shutdown the broker fires the stale startup Will and overwrites the retained offline `<prefix>/status` with `"streams": []`.**
     - Dropping the real-binary + broker shutdown test from **REVIEW4 CI2** allowed this to slip past both `stream_player_test` (in-process `StreamPlayer`, no broker) and `test-mqtt-reconnect.sh` (`SIGHUP` with `playing=false`, so `exitIfBusy()` never takes the `_exit(0)` branch).
- **minor (fix edge case across #24 + #25):**
  2. **R2 (minor, `[code]`): `StreamPlayer::exitIfBusy()` hardcodes `_exit(0)` on the `sigtimedwait` error path (`return 1`), and `authGiveUp_` skips `exitIfBusy()` before `~StreamPlayer()` joins `reaper_`.**
     - K1's primary startup auth-refusal path is unaffected (`reaper_` is empty when the first connect is refused 3 times). A mid-session reconnect auth refusal while a stream is stuck in `getaddrinfo()` blocks in `~StreamPlayer()`'s `worker->thread.join()`, and a `sigtimedwait` error with a stuck worker exits `0` instead of `1`.

Everything else in the `SURVIVING4` filter and in #23 / #24 / #25 / #20 is clean.

---

## 1. Fix regressions on `main` (`58a2f9c`)

### R1. major — `exitIfBusy()` calls `_exit(0)` before `MqttController::shutdown()`, causing the broker to publish the stale startup Will and wipe out the retained offline stream list `[repro]`

- **Where:**
  - `src/mqtt_controller.cpp:261-268`, `:296-298`:
    ```cpp
    auto finishProcess = [&]() {
        player.closeAll();
        const std::string statusTopic = config_.prefix + "/status";
        const std::string offline = buildStatusJson(false, playing_.load(), manager_.layout(),
                                                    manager_.layoutSetting(), manager_.list());
        publishRaw(statusTopic, offline, true);
        player.exitIfBusy(StreamPlayer::kExitJoinDeadline);
    };
    ...
    finishProcess();
    shutdown();
    return 0;
    ```
  - `src/stream_player.cpp:281-289`: when `drain(deadline)` returns `false` after 2 s (`kExitJoinDeadline`), `exitIfBusy()` flushes stdio and calls `_exit(0)`.
  - `docs/mqtt.md:53`: claims that on SIGINT/SIGTERM/SIGHUP with a worker still inside `open` after 2 s, the final retained status from the live table with `"online": false` is preserved on the broker.
- **What happens:**
  1. On initial startup before any streams exist, `refreshWill(true)` (`src/mqtt_controller.cpp:189`) sets the broker Will to `{"online":false,"playing":false,"layout":"1x1","layout_setting":"auto","streams":[]}`, and `CONNECT` sends that Will to the broker.
  2. When streams are added and started, `publishStatus()` calls `refreshWill(false)` (`src/mqtt_controller.cpp:472`). In MQTT 3.1.1 and libmosquitto, `mosquitto_will_set()` only updates `mosq->will` in client memory for the *next* `CONNECT` packet (`docs/mqtt.md:53`); the broker still holds the empty-table Will from the initial `CONNECT`.
  3. When `SIGTERM` arrives while a stream worker is blocked in `getaddrinfo()`, `finishProcess()` calls `publishRaw(statusTopic, offline, true)` (which transmits the retained offline status containing the live stream table during the 2 s wait) and then `player.exitIfBusy(StreamPlayer::kExitJoinDeadline)`.
  4. At 2 s, `exitIfBusy()` calls `_exit(0)` **before** `shutdown()` (`mosquitto_disconnect()` + `mosquitto_loop_stop()`) is reached at line 297.
  5. Because the TCP socket closes without an MQTT `DISCONNECT` packet (`Client amlogic-multistream closed its connection` in mosquitto's log), the broker fires the stored startup Will, which is a retained publish to `<prefix>/status` with `"playing":false,"streams":[]`. That Will immediately overwrites the retained offline status that `publishRaw()` published 2 s earlier.
- **Reproduced on `58a2f9c`:**
  - In `unshare -m` with `/etc/resolv.conf` pointing at a silent UDP socket on `127.0.0.2:53` (`options timeout:5 attempts:2`), started `mosquitto` 2.0.18 and `build/amlogic-multistream`, published `add {"id":"cam","url":"http://camera.lan/live/index.m3u8"}` and `start {}`, waited 0.5 s for `cam` to enter `getaddrinfo()`, and sent `SIGTERM`.
  - The player exited `0` in `2003 ms`.
  - A subscriber on `amlogic-multistream/status` received two offline messages in sequence:
    ```text
    amlogic-multistream/status {"online":false,"playing":true,"layout":"1x1","layout_setting":"auto","streams":[{"id":"cam","url":"http://camera.lan/live/index.m3u8","muted":false}]}
    amlogic-multistream/status {"online":false,"playing":false,"layout":"1x1","layout_setting":"auto","streams":[]}
    ```
  - Querying the retained message on `amlogic-multistream/status` after exit returned `{"online":false,"playing":false,"layout":"1x1","layout_setting":"auto","streams":[]}` (`cam` was lost).
- **Fix:**
  - Call `shutdown()` (`mosquitto_disconnect(mosq_)` + `mosquitto_loop_stop(mosq_, false)`) inside `finishProcess()` *after* `publishRaw(statusTopic, offline, true)` and *before* `player.exitIfBusy(StreamPlayer::kExitJoinDeadline)`. `mosquitto_disconnect()` queues `DISCONNECT` after the queued QoS 1 offline `PUBLISH`, and `mosquitto_loop_stop(mosq_, false)` flushes both to the broker so the broker discards the Will before `exitIfBusy()` waits on `reaper_` or calls `_exit(0)`.

### R2. minor — `exitIfBusy()` hardcodes `_exit(0)` vs non-zero exit paths (`authGiveUp_` and `sigtimedwait` error) `[code]`

- **Where:**
  - `src/mqtt_controller.cpp:271-277` (`authGiveUp_` exit) and `:289-294` (`sigtimedwait` error exit)
  - `src/stream_player.cpp:62-75` (`~StreamPlayer()`) and `:281-289` (`exitIfBusy()`)
- **What happens:**
  - **Primary K1 startup case is clean:** When the broker refuses login at startup, no streams exist (`reaper_` is empty), `authGiveUp_` calls `player.closeAll()`, `shutdown()`, `return 1`, and does not call `finishProcess()`. The process exits `1` after 3 CONNACK refusals (`scripts/test-mqtt-reconnect.sh:323-402`).
  - **Edge case 1 (`sigtimedwait` error at `:289-294`):** Calls `finishProcess()` before `return 1`. If a worker is stuck in `getaddrinfo()`, `finishProcess()` calls `player.exitIfBusy()`, which calls `_exit(0)` instead of exiting `1`.
  - **Edge case 2 (mid-session `authGiveUp_` at `:271-277`):** If the client previously connected, started a stream that is currently stuck in `getaddrinfo()`, and the broker then disconnected and refused reconnect authentication 3 times, `authGiveUp_` calls `player.closeAll()`, `shutdown()`, and `return 1` without `exitIfBusy()`. Stack unwinding then runs `StreamPlayer::~StreamPlayer()` (`src/stream_player.cpp:62-75`), which joins every thread in `reaper_` unconditionally and blocks for the remainder of `getaddrinfo()`.
- **Fix:**
  - Add an exit code parameter `void exitIfBusy(std::chrono::milliseconds deadline, int exitCode = 0)` that calls `_exit(exitCode)`, and call `player.exitIfBusy(StreamPlayer::kExitJoinDeadline, 1)` after `shutdown()` on both `return 1` paths.

---

## 2. Other requested fix checks (#23, #24, #25, #20)

| Check | Verdict | Evidence on `58a2f9c` |
| --- | --- | --- |
| **#23 format whitelist width** (`src/ffmpeg_decoder.cpp:29-30`) | **Clean** | Added only `aac,ac3,eac3,mp3` to `kFormatWhitelist` (`"hls,mpegts,mov,mp4,mpeg,aac,ac3,eac3,mp3"`), matching the four audio codecs already in `kCodecWhitelist`. Image (`gif`, `image2`), playlist/concat, and device demuxers remain blocked (`testRejectGif` in `tests/ffmpeg_decoder_test.cpp` passes). Standalone audio URLs still fail `av_find_best_stream(AVMEDIA_TYPE_VIDEO)`. |
| **#24 credential `EnvironmentFile`** (`amlogic_launcher.py:37-113`, `default.py:38-73`) | **Clean** | `write_mqtt_env_file()` writes `mqtt.env` atomically (`tmp` + `os.open(..., 0o600)` + `os.fchmod(fd, 0o600)` + `os.replace` + `os.chmod(path, 0o600)`). `quote_systemd_env()` escapes `"`, `\`, `` ` ``, and `$` inside double quotes. Tested against Ubuntu 24.04's `libsystemd-shared-255.so` `load_env_file()` via `ctypes` across spaces, quotes, backslashes, `$VAR`, backticks, `#`, `;`, `=`, `%`, and newlines: all round-tripped identically. `default.py` does not `.strip()` the password, `argv` logs only the `EnvironmentFile=` path, and clearing the password removes `mqtt.env`. |
| **#20 stall isolation after #25** (`src/stream_player.cpp`, `tests/stream_player_stall_test.cpp`) | **Clean `[repro]`** | Moving per-thread state from `Session` to `Worker` and retiring stopped workers onto `reaper_` preserved #20's isolation (`stream_player_stall`: `blackhole+trickle shutdownMs=2`, `live+vod presents=152 shutdownMs=15`) and eliminated the presenting-thread freeze on `remove`/`stop`/`restart`/layout-hide during a blackholed DNS lookup (verified with the real binary + `127.0.0.2:53`: healthy neighbor continued presenting 20–49 frames/s across `remove`). |
| **Rebase fallout (#25 onto #23+#24)** (`README.md`, `docs/mqtt.md`, `src/mqtt_controller.cpp`) | **Clean** | `README.md:65,205` and `docs/mqtt.md:5,21,39,41,53` preserve #23's packed-audio demuxer allowlist, #24's 3-attempt CONNACK exit and mode-0600 `EnvironmentFile`, and #25's reaper semantics without dropped hunks or conflicting prose (save for R1 where `mqtt_controller.cpp` did not match `docs/mqtt.md:53`). |

---

## 3. Audit of `SURVIVING4` keep / downgrade / drop decisions vs `REVIEW4`

| `REVIEW4` item | `REVIEW4` sev | `SURVIVING4` action | Gemini audit verdict |
| --- | --- | --- | --- |
| **T1** uninterruptible DNS `getaddrinfo` blocks present thread on `remove`/`stop`/`restart` & delays SIGTERM | major | **Kept (major)** → fixed in #25 | **Correctly kept.** #25 fixed the presenting-thread freeze and bounded SIGTERM to 2 s, but introduced **R1** (and minor **R2**) on the `_exit(0)` path. |
| **K1** Kodi add-on lacks MQTT credentials + player loops forever on auth refusal | major | **Kept (major)** → fixed in #24 | **Correctly kept and cleanly fixed in #24.** |
| **H1** HLS master with packed-audio rendition (`.aac`/`.ac3`/`.eac3`/`.mp3`) fails format whitelist | minor | **Kept (minor)** → fixed in #23 | **Correctly kept and cleanly fixed in #23.** |
| **P1** 20 ms tick sleep + 1-frame blocking slot caps streams below ~50 fps and skips 60 fps live HLS segments | major | **Downgraded / deferred** until scanout exists | **Defensible deferral.** The cap (~49.5 fps at 64x36, ~43 fps at 1280x720) and 60 fps live segment skips are real on `main`, but software mode drives no display and documents the 20 ms loop (`docs/mqtt.md:39`). Must be addressed when the DRM/KMS present loop lands. |
| **M1** `<prefix>/status` has no per-stream health; trickle stream never fails | minor | **Dropped** | **Correctly dropped.** Status matches the documented `id`/`url`/`muted` contract (`docs/mqtt.md:94-106`), a watchdog is new feature scope, and omitting a per-read deadline in `interruptCallback` is required so live-edge HLS waits do not abort. |
| **O1** decode-thread `std::cout` / `std::cerr` lines interleave under `std::unitbuf` | minor | **Dropped** | **Defensible drop (cosmetic).** Chained `<<` calls on worker threads (`src/stream_player.cpp:176`, `src/ffmpeg_decoder.cpp:39-45,254-258`) can interleave when multiple streams open concurrently, whereas `MQTT status` uses a single string insertion (`src/mqtt_controller.cpp:506`). Diagnostic polish only. |
| **CI1** device AppImage workflow runs no ARM unit tests and does not decode via MQTT | minor | **Dropped** | **Correctly dropped.** Host CI runs all unit and stall tests on `x86_64` and `aarch64`, and `verify-coreelec-appimage.sh` matches its documented smoke scope (`README.md:187-194`). |
| **CI2** missing tests for (a) abort-ignoring backend, (b) throughput, (c) real-binary stall + SIGTERM | minor | **Dropped** ((a) folded into T1, (b) into P1, (c) dropped as covered in-process) | **Partially wrong drop on (c).** (a) landed in #25 (`StallingDecoder` in `tests/stream_player_test.cpp`) and (b) belongs with deferred P1, but dropping (c) — a real-binary + broker shutdown check with an uninterruptible worker — is why **R1** was not caught by `stream_player_test` or `test-mqtt-reconnect.sh`. |
| **N1** `decoder_backend.h` comment says `close()` runs on the decode thread | nit | **Dropped** | **Resolved on `main` anyway:** #25 updated `src/decoder_backend.h:45-47` (`close() runs after that call has returned, on the thread that owns the session`). |
| **N2** two-statement window after normal player exit before `restore_kodi` ignores HUP/TERM | nit | **Dropped** | **Correctly dropped.** Single shell function call boundary (`scripts/launch-coreelec.sh:270-271` → `:174`). |
| **N3** `stream_player_stall` and `mqtt_reconnect` not in `DECODER_TESTS` | nit | **Dropped** | **Correctly dropped.** Both run and pass in CI (`ci.yml` installs `ffmpeg` with `libx264` and `mosquitto`). |
| **I1–I3** plane count vs per-tile present, DMA-BUF handles, CE21 fbdev vs DRM | device notes | **Kept as device notes** | **Correctly handled.** Future DRM/hwaccel design constraints requiring a GS-King X. |

---

## 4. Cross-check against Opus `AUDIT4.md` (#27)

See also standalone [`CROSSCHECK-GEMINI.md`](CROSSCHECK-GEMINI.md). Cross-checked Opus's [`AUDIT4.md`](https://github.com/yyolk/amlogic-multistream/pull/27) (`cursor/audit4-filter-fixes-d168`) against `REVIEW4.md`, `SURVIVING4.md`, and `main` @ `58a2f9c`.

### Agree

- **`SURVIVING4` filter calls:** Keeping `T1`, `K1`, `H1`, deferring `P1` until scanout exists, and dropping `M1`, `O1` (real race in worker logging, but cosmetic), `CI1`, `N1` (fixed in #25), `N2`, `N3`, and `I1–I3` all hold up.
- **Core guarantees:** #20/#25 stall isolation holds (`remove` during a stuck DNS lookup freezes presents for 0 s; TSan clean); #24's startup auth refusal exits `1` (never `_exit(0)`); #24's `EnvironmentFile=` (`mode 0600`, `quote_systemd_env`) is sound.
- **Must-fix regressions:**
  - **Opus A1 == Gemini R1 (must fix, `[repro]`):** `finishProcess()` calls `player.exitIfBusy(kExitJoinDeadline)` (`_exit(0)`) before `MqttController::shutdown()` (`mosquitto_disconnect()`), so on a DNS-stalled shutdown the broker publishes the stale `CONNECT`-time Will (`"playing":false,"layout":"1x1","streams":[]`) over the final retained offline status.
  - **Opus A2 == Gemini R2 edge case 2 (must fix, `[code + repro]`):** `authGiveUp_` (`src/mqtt_controller.cpp:271-277`) calls `return 1` without a bounded drain/exit, so `~StreamPlayer()` joins `reaper_` with no deadline if a mid-session reconnect fails auth while a worker is blocked in `getaddrinfo()`.
  - **Opus A6 (part 1) == Gemini R2 edge case 1 (nit, `[code]`):** `sigtimedwait` error exit (`src/mqtt_controller.cpp:289-294`) calls `finishProcess()` before `return 1`, exiting `0` instead of `1` when a worker is blocked.

### Disagree / Nuances (with evidence)

1. **Severity of A1 / R1 (`minor` in Opus vs `major` in Gemini):**
   - Opus rates A1 `minor (regression)` (while elevating it to **must fix**) because `"online":false` is still true in the startup Will. Gemini rates R1 `major` because preserving the live stream table in the retained offline `<prefix>/status` on shutdown is a documented guarantee (`docs/mqtt.md:53`, `test-mqtt-reconnect.sh:264-266`) and is the entire reason #25 called `publishRaw()` before waiting on workers; on the T1 shutdown path, the startup Will overwrites `publishRaw()` 100% of the time after any `add`. Both audits mark it **must fix** with the same fix (`shutdown()` before `_exit`).
2. **`REVIEW4` CI2 recipe vs A1 / R1 (Opus is right):**
   - Opus (`AUDIT4.md:80`) rightly notes that `REVIEW4`'s literal CI2 recipe (`REVIEW4.md:188`: blackhole + trickle URLs, SIGTERM, assert `"online":false`) would **not** have caught A1/R1: TCP blackhole/trickle sockets honor `abort()` (so `drain()` finishes in `<100 ms` without calling `_exit(0)`), and asserting only `"online":false` passes even when the startup Will wipes out `"streams"`. Catching A1/R1 requires an uninterruptible worker plus asserting that `"streams"` is preserved in the retained status.

### Opus catches that Gemini missed (0 false positives in `AUDIT4.md`)

Every claim in Opus's `AUDIT4.md` (`A1`–`A6`) was verified on `58a2f9c`; Opus has **no false positives**. Opus caught three valid issues (`A3`, `A4`, `A5`, plus `--help` wording in `A6`) that Gemini missed:

- **Opus A3 (`minor`, new in #25 — verified `[code + repro]`):** `restart` (or `stop` + `start`) during a stuck lookup retires the blocked `Worker` onto `reaper_` and immediately spawns a replacement `std::thread` in `StreamPlayer::tick()` (`src/stream_player.cpp:400-428`) because `generationChanged` bypasses `session.backoff`. Command churn during a DNS outage stacks threads and UDP sockets on `reaper_` until `getaddrinfo()` times out (address-space hazard on 32-bit armv7).
- **Opus A4 (`minor`, from #23 — verified `[repro]`):** Widening `kFormatWhitelist` also widened the post-open top-level check `av_match_list(fmt_->iformat->name, kFormatWhitelist, ',')` at `src/ffmpeg_decoder.cpp:162-169`. Gemini missed that FFmpeg's `mp3` demuxer exposes an ID3v2 `APIC` cover-art JPEG as `AVMEDIA_TYPE_VIDEO` (`mjpeg`, which is on `kCodecWhitelist`): reproduced on `58a2f9c` with `amlogic-multistream --url song.mp3 --frames 1`, which opens `stream=1 codec=mjpeg`, presents the `64x64` cover art, and exits `0`. Keeping a narrow top-level allowlist (`"hls,mpegts,mov,mp4,mpeg"`) for `fmt_->iformat->name` at line 163 while passing the wider `kFormatWhitelist` in `opts` fixes this cleanly.
- **Opus A5 (`nit`, from #19 / #23 — verified `[repro]`):** In FFmpeg 6.1 (`libavcodec.so.60`), `avcodec_find_decoder(AV_CODEC_ID_MP3)->name` is `"mp3float"`, not `"mp3"`, so `avformat_find_stream_info` on an MP3-rendition HLS master logs `[mp3float @ ...] Codec (mp3float) not on whitelist ...` (video still opens and presents because audio is discarded).
- **Opus A6 part 2 (`nit` — verified `[code]`):** `MqttController::printUsage()` (`src/mqtt_controller.cpp:412`) still says `"stop closes them"` instead of `"stop retires them"`.

