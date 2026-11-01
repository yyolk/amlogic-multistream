# Gemini Cross-Check of Opus `AUDIT4.md` (#27)

Cross-checked Opus's [`AUDIT4.md`](https://github.com/yyolk/amlogic-multistream/pull/27) (`cursor/audit4-filter-fixes-d168`) against `REVIEW4.md` ([PR #21](https://github.com/yyolk/amlogic-multistream/pull/21)), `SURVIVING4.md` ([PR #22](https://github.com/yyolk/amlogic-multistream/pull/22)), `AUDIT4-GEMINI.md` ([PR #26](https://github.com/yyolk/amlogic-multistream/pull/26)), and `main` @ `58a2f9c114daf7835c7b2b3c8c3201b9a350705c`.

---

## 1. Agree

- **`SURVIVING4` filter verdict (`REVIEW4` → `SURVIVING4`):**
  - Keeping **T1** (major), **K1** (major), and **H1** (minor), deferring **P1** until DRM/KMS scanout exists, and dropping **M1**, **O1**, **CI1**, **N1**, **N2**, **N3**, and **I1–I3** all hold up on `58a2f9c`. No dropped or downgraded item needs to be restored as a blocker/major.
  - **O1** (torn decode-thread log lines) still exists in the code (`SURVIVING4`'s single-run absence of a torn line was weak evidence against a race), and remains cosmetic/nit.
- **Core guarantees on `58a2f9c`:**
  - **B1 stall isolation (#20 + #25)** holds: `remove`, `stop`, `restart`, and layout-hide during a silent-resolver `getaddrinfo()` no longer block the presenting thread.
  - **K1 startup auth exit (#24)** is never turned into `_exit(0)` by #25 (`rc == 1` after 3 CONNACK refusals).
  - **K1 `EnvironmentFile=` handling (#24)** (`mode 0600`, `quote_systemd_env` escaping `"`, `\`, `` ` ``, `$`, no password on `systemd-run` argv or in `kodi.log`, file removed when cleared) is sound.
- **Must-fix regressions in #24 + #25:**
  - **Opus A1 == Gemini R1 (must fix, `[repro]`):** In `src/mqtt_controller.cpp:261-268`, `finishProcess()` calls `player.exitIfBusy(StreamPlayer::kExitJoinDeadline)` (`_exit(0)`) before `MqttController::shutdown()` (`mosquitto_disconnect()`). When SIGTERM/SIGINT/SIGHUP lands while a worker is blocked in `getaddrinfo()`, `_exit(0)` drops the TCP connection without an MQTT `DISCONNECT`, causing the broker to publish the stale `CONNECT`-time Will (`"playing":false,"layout":"1x1","streams":[]`) over the retained offline `<prefix>/status` that `publishRaw()` had just published. Both audits independently reproduced this against `mosquitto` 2.0.18 with a silent UDP listener on `127.0.0.2:53` and proposed the same fix (`shutdown()` before `_exit`).
  - **Opus A2 == Gemini R2 edge case 2 (must fix, `[code + repro]`):** In `src/mqtt_controller.cpp:271-277`, the `authGiveUp_` path calls `player.closeAll(); shutdown(); return 1;` without a bounded drain/exit, so a mid-session reconnect auth failure while a stream is blocked in `getaddrinfo()` runs `StreamPlayer::~StreamPlayer()` (`src/stream_player.cpp:62-75`), which joins `reaper_` with no deadline (blocking up to 10–20 s with signals masked) and contradicts `src/stream_player.cpp:63-65`, `src/stream_player.h:29-32`, and `docs/mqtt.md:107`.
  - **Opus A6 (first half) == Gemini R2 edge case 1 (nit, `[code]`):** The `sigtimedwait` error branch (`src/mqtt_controller.cpp:289-294`) calls `finishProcess()` before `return 1`, which exits `0` via `exitIfBusy()` if a worker is blocked instead of exiting `1`.

---

## 2. Disagree / Nuances (with evidence)

1. **Severity of A1 / R1 (`minor` in Opus vs `major` in Gemini):**
   - Opus rates **A1** `minor (regression)` (while still marking it **must fix**) because `"online":false` is still published by the startup Will and only `"streams"`, `"layout"`, and `"playing"` are clobbered.
   - Gemini rates **R1** `major` because preserving the stream list in the retained offline `<prefix>/status` on shutdown is an explicit contract (`docs/mqtt.md:53`, `scripts/test-mqtt-reconnect.sh:264-266` from SURVIVING3 #3) and is the sole reason #25 reordered `publishRaw()` ahead of the worker wait (`src/mqtt_controller.cpp:257-260`). On the exact T1 shutdown path that #25 targeted, the startup Will overwrites `publishRaw()` 100% of the time after the first `add`.
   - **Practical impact:** None — both audits place A1/R1 at the top of the **must-fix** list with the same fix.

2. **`REVIEW4` CI2's relationship to A1 / R1 (Opus is right, Gemini overstated CI2):**
   - In `AUDIT4-GEMINI.md`, Gemini attributed R1 slipping through to `SURVIVING4` dropping `REVIEW4` **CI2** (c) (real-binary stall + SIGTERM test).
   - Opus (`AUDIT4.md:80`) rightly points out that `REVIEW4`'s literal CI2 recipe (`REVIEW4.md:188`: *"real binary, a blackhole and a trickle URL, `start`, SIGTERM, and assert the exit time and a retained `\"online\":false`"*) would **not** have caught A1/R1 either: TCP blackhole/trickle sockets honor `AVIOInterruptCB` (`abort()`), so `drain()` finishes in `<100 ms` without taking the `_exit(0)` branch, and asserting only `"online":false` passes even when the startup Will clobbers `"streams"`. Catching A1/R1 requires an uninterruptible worker (`getaddrinfo` or `StallingDecoder`) **plus** asserting that `"streams"` survives in the retained `<prefix>/status`.

---

## 3. Opus catches that Gemini missed & False-Positive Check

Every claim in Opus's `AUDIT4.md` (`A1`–`A6`) was tested against `main` @ `58a2f9c`. **Opus has zero false positives.**

Opus caught three real issues (`A3`, `A4`, `A5`, plus the `--help` wording nit in `A6`) that Gemini missed:

1. **Opus A3 (`minor`, new in #25 — verified `[code + repro]`): `restart` or `stop`+`start` during a stuck DNS lookup stacks unbounded threads on `reaper_`.**
   - In `src/stream_player.cpp:400-428`, a `restart` (or `stop` + `start` via `closeAll()`) calls `retireWorker(session)` (moving the blocked `Worker` to `reaper_`) and immediately calls `startWorker(info, session)` in the same `tick()` because `generationChanged` bypasses `session.backoff`. Each command spawns another `std::thread` and UDP socket that blocks in `getaddrinfo()` until the resolver times out. On 32-bit `coreelec21-ng-armv7hf` (8 MB default stack per thread), high-rate command churn during a DNS outage can exhaust virtual address space.
   - **Verdict:** Valid `minor` (should fix) — either treat an still-running retired worker for the same stream `id` on `reaper_` as in-progress/backoff, or cap `reaper_`.

2. **Opus A4 (`minor`, from #23 — verified `[repro]`): Widening `kFormatWhitelist` also widened the post-open top-level demuxer check (`src/ffmpeg_decoder.cpp:162-169`).**
   - Gemini checked that standalone audio files fail `av_find_best_stream(AVMEDIA_TYPE_VIDEO)` and concluded #23's whitelist was clean (`AUDIT4-GEMINI.md:88`), missing that FFmpeg's `mp3` demuxer exposes an ID3v2 `APIC` cover-art tag as `AVMEDIA_TYPE_VIDEO` with `AV_CODEC_ID_MJPEG` (`mjpeg` is in `kCodecWhitelist`).
   - Reproduced on `58a2f9c`: running `amlogic-multistream --url song.mp3 --frames 1` on an MP3 with an attached JPEG cover opens `stream=1 codec=mjpeg`, decodes and presents the `64x64` cover frame, and exits `0`.
   - While `av_dict_set(&opts, "format_whitelist", ...)` must include `aac,ac3,eac3,mp3` so `hls.c` copies them to child rendition demuxers, `fmt_->iformat->name` at `src/ffmpeg_decoder.cpp:163` only checks the top-level container and can stay `"hls,mpegts,mov,mp4,mpeg"`.
   - **Verdict:** Valid `minor` (should fix).

3. **Opus A5 (`nit`, from #19 / #23 — verified `[repro]`): `mp3` in `kCodecWhitelist` never matches FFmpeg 6.1's `mp3float` decoder.**
   - Confirmed via `libavcodec.so.60` (`avcodec_find_decoder(AV_CODEC_ID_MP3)->name` returns `"mp3float"`) and by running `amlogic-multistream` on an HLS master with an MP3 audio rendition: `avformat_find_stream_info` logs `[mp3float @ ...] Codec (mp3float) not on whitelist 'h264,hevc,mpeg4,mpeg2video,mpeg1video,mjpeg,aac,mp3,ac3,eac3'` six times per open (video still opens and decodes because audio is discarded).
   - **Verdict:** Valid `nit`.

4. **Opus A6 second half (`nit` — verified `[code]`): `--help` wording.**
   - `MqttController::printUsage()` (`src/mqtt_controller.cpp:412`) still says `"stop closes them"`, whereas `README.md:65` and `docs/mqtt.md:5` were updated in #25 to say `stop` retires/aborts decoders without joining a blocked worker.
   - **Verdict:** Valid `nit`.
