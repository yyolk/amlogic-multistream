# AUDIT9: `main` @ e53336a after AUDIT8 P1 (#54)

Audited commit: `e53336aebe049fcd1557a185c38872c0a5f99feb` (tip of `main`).
AUDIT8 base: `0518a7e` ([#52](https://github.com/yyolk/amlogic-multistream/pull/52) /
[#53](https://github.com/yyolk/amlogic-multistream/pull/53), both closed).
The only production change since that base is
[#54](https://github.com/yyolk/amlogic-multistream/pull/54) (`e53336a`), which
shares `SoftwareRenderer` size caps across copy, present, `--url`, and
`--viewport`. `git diff 0518a7e..e53336a -- src tests scripts CMakeLists.txt`
is those nine files. `docs/research/` was not used as findings.

No production code is changed in this PR. Nothing was fixed.

**Severity:** **must-fix** = breaks a guarantee the code or docs make, on a
reachable path, in a way that leaves the player hung, retained as online, or
unbounded. **should-fix** = real, contained, worth doing before hardware work.
**nit** = polish, a known trade-off, or a test/doc gap.
**Evidence:** `[repro]` = run on this VM. `[code]` = from reading the code.
**needs-GS-King-X** = needs the box.

**Environment.** Ubuntu 24.04 x86_64, g++ 13.3.0, FFmpeg 6.1.1
(libavcodec 60.31.102, libavformat 60.16.100, libswscale 7.5.100),
libmosquitto / mosquitto 2.0.18, libdrm 2.4.125.

**Suite.** `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` on `e53336a`:
**14/14 pass**, none skipped, ctest **120.41 s**. `mqtt_exit_test` (42.24 s):
sigterm 2011 ms, drain-cmd 2007 ms, gone-dns 20 ms, gone-blackhole 502 ms,
drop-drain 2010 ms, auth give-up rc 1 with `waitExitMs=2012`, startup auth
rc 1, **startup-dns 20 ms rc 0**. `present_smoke` 0.30 s.
`stream_player_http` 1.70 s (includes the wide-frame retry case).
`stream_player_test` 5.65 s (includes the fake-decoder wide-frame case).

---

## Survivors

Must-fix: **none**. Should-fix: **none**. **P1 is closed** by #54. B1 and L1
stay nits. This pass did not find new evidence that would move either one.
N2, N3, N4, F4, F5, F6, and F7 are unchanged in code and stay nits. They are
listed so they are not treated as new.

| ID | Severity | Finding | From |
| --- | --- | --- | --- |
| **B1** | nit | SIGTERM in the ~20 ms after the startup helper has written CONNECT, before `MQTT running`, still `_exit`s without DISCONNECT. The empty startup will can replace the previous retained status. `online` stays false. Not re-sampled; the window is the same code | #47, carried. Demoted in AUDIT7. Same rating here |
| **L1** | nit | `normalize_abs` in `scripts/launch-coreelec.sh` pathname-expands glob characters against the cwd. Not re-run | #53. Same rating here |
| N2 | nit | `publishRaw` returns true when `mosquitto_publish` queues, including while the socket exists and MQTT is not up. The header still says "a live connection" | #34, reconfirmed `[code]` |
| N3 | nit | A full retired-decoder cap also delays a healthy `restart` or a new open | #33, reconfirmed `[code]` |
| N4 | nit | `stream_player.h` still says process exit `_exit`s if a worker is inside the blocking call. `finishProcess` always `_exit`s | #34, reconfirmed `[code]` |
| F4 | nit | Re-adding an id whose lookup is in flight still waits that lookup out (`reaperBusy`) | carried `[code]` |
| F5 | nit | No `AV_DISPOSITION_ATTACHED_PIC` check after `av_find_best_stream`. A `mov` / HLS wrap of a cover can still present `mjpeg` | carried `[code]` |
| F6 | nit | Packed-audio fixtures in `ffmpeg_decoder_test` are still ADTS only (`testAdtsAudioMaster`) | carried `[code]` |
| F7 | nit | `mqtt_exit_test` and `mqtt_reconnect` still exit 77 without the daemon and are not required by `scripts/ctest-require.sh`. Both ran on this VM | carried `[code]`, suite `[repro]` |

---

## Verdict

#54 closes **P1**. A frame wider than 8192 and still inside the old copy caps
is rejected in `copyOwned` and takes the 1 s "will retry" path. `--url` of
that frame returns 1 at the frame check and does not init an oversize
viewport. A frame at the cap (8192×32) still presents. Height above 8192
was already on the retry path; it now uses the same predicate.

F1, X1, F3, A1, A2, A3, A4, A5, N1, and B2 still hold on the unit tests and
on the timings above. The silent-resolver, launch-script glob, and B1
post-CONNECT window were not rebuilt. Those lines did not move in a way
that adds evidence.

No new must-fix and no new should-fix on that first pass. [CROSSCHECK-A](#crosscheck-a) against [#55](https://github.com/yyolk/amlogic-multistream/pull/55) revises one drop: **D1** is an agreed should-fix. This file still has no must/should that #55 missed.

---

## 1. P1 — closed `[repro]` `[code]`

- **Where:** `SoftwareRenderer::kMaxExtent` is 8192 and `kMaxStride` is
  65536 (`src/software_renderer.h`). `frameAcceptable` requires yuv420p,
  both sides in `1..8192`, and each plane non-null with stride at least the
  plane width and at most `kMaxStride`
  (`src/software_renderer.cpp`). `present` uses that predicate.
  `StreamPlayer::copyOwned` returns false unless it passes, so the worker
  sets `WorkerFail::Read` and `tick` waits `kRetryDelay` (1 s) instead of
  logging `present failed` on every frame (`src/stream_player.cpp`).
  `--url` uses the same predicate in `frameLooksComplete` before viewport
  init (`src/app.cpp`). `--viewport` uses `kMaxExtent`
  (`src/mqtt_controller.cpp`).
- **Why the old hole is gone.** Before #54, copy allowed a width in
  `(8192, 65536]` when the stride and the row count were in range.
  `present` then rejected every frame and `tick` did not back off. Height
  above 8192 already failed the row check and retried. Both sides now fail
  the same function.

| Probe | Result |
| --- | --- |
| `--url` of h264 8192×32, `--frames 1` | **rc 0**. Stdout has `frame 0 8192x32`, `presented 0`, `Decoded 1 frame(s)`. Stderr empty |
| `--url` of h264 8224×32, `--frames 2` | **rc 1**. Stderr is `Decoded frame 0 8224x32 cannot be presented`. Stdout has the open line and `video 8224x32 yuv420p -> yuv420p`. No `presented`, no `Decoded N frame(s)`, no `viewport must be` |
| `--viewport 8193x10` | rc 1, `Invalid --viewport`, before broker connect |
| `--viewport 8192x32`, broker `127.0.0.1:1` | Init prints `StreamPlayer viewport 8192x32`. SIGTERM during the refused connect exits. The cap is accepted |
| `ffprobe` of the 8224 fixture | `h264,8224,32` |

`stream_player_test` `testWideFrameUsesRetryPath` (fake 8224×32) and
`stream_player_http` (real 8224×32 over HTTP into a 640×360 viewport) both
require zero presented frames, no `present failed`, and at least one
`will retry`. Both passed inside the suite. `software_renderer` checks
`frameAcceptable` is false at 8224 on either side and true at
`kMaxExtent`. `present_smoke` checks `--url` of the wide file fails with
`cannot be presented` and not with the viewport-init error.

The convert line on the 8224 `--url` run is expected. `publishYuv420`
still allocates and scales, then the caller rejects
(`src/ffmpeg_decoder.cpp`). That is the same order the tall path already
used. One rejected frame per open, then the 1 s retry. It is not the
per-frame present spam P1 measured.

---

## 2. Prior closed items on `e53336a`

#54 does not touch the protocol flag, the reaper, or the exit path. The
suite is the reconfirmation. The silent-resolver and launch-script
harnesses from AUDIT7 and AUDIT8 were not rebuilt.

| Item | Fixed by | Verdict | Evidence |
| --- | --- | --- | --- |
| **P1** wide frame copied, then `present` failed every tick | #54 | **Closed** | Section 1 |
| **B2** `--url http` playlist opens `file://` segments | #51 | **Closed** | `[repro]` `ffmpeg_decoder` log still has `Protocol 'file' not on whitelist 'http,https,tls,tcp,crypto,hls'!` for `http://…/file_seg.m3u8` and for `hls+http://…/file_seg.m3u8`. The flag-on open of the same playlist still decodes. `[code]` `urlIsLocalFileSource` is unchanged |
| **N1** pre-connect `getaddrinfo` holds SIGTERM | #47 | **Closed** | `[repro]` `startup-dns` 20 ms rc 0. `[code]` helper still calls `mosquitto_connect_async`; main stays in `sigtimedwait` (`src/mqtt_controller.cpp`) |
| **F1** command during the drain republishes `online:true` | #34 | **Closed** | `[repro]` drain-cmd 2007 ms, retained `online:false` with `cam` |
| **X1** exit joins the mosquitto thread | #34 | **Closed** | `[repro]` gone-dns 20 ms, gone-blackhole 502 ms, drop-drain 2010 ms. `[code]` `finishProcess` still never calls `mosquitto_loop_stop` |
| **F3** fresh-id churn stacks decode threads | #33 | **Closed** | `[repro]` `stream_player_test` passed, including the churn cases. `[code]` the busy-retired gate is unchanged |
| **A1** SIGTERM with a stuck worker keeps the table | #30 | **Closed** | `[repro]` sigterm 2011 ms, retained `online:false` |
| **A2** auth give-up exits 1 inside the same bound | #30 | **Closed** | `[repro]` `waitExitMs=2012`, rc 1. The 15.8 s before that is the three CONNACK attempts, not the drain |
| **A3** restart during a stuck lookup does not stack | #29 | **Closed** | `[code]` `reaperBusy` still runs before a replacement open |
| **A4** top-level packed audio rejected | #28 | **Closed** | `[repro]` `ffmpeg_decoder` rejects `song.mp3` with `demuxer is not allowed: mp3` |
| **A5** `mp3float` on the codec whitelist | #28 | **Closed** | `[code]` `kCodecWhitelist` still lists `mp3,mp3float`. Which name CoreELEC registers first remains **needs-GS-King-X** |

---

## 3. Carried nits — no new evidence

Not promoted. Checked only that the code is still the same shape.

- **B1** `[code]`. The startup helper still stores `connectReady` from
  `mosquitto_connect_async` and returns. A signal in the following
  `sigtimedwait` still `_exit`s without DISCONNECT
  (`src/mqtt_controller.cpp`). `online` stays false. Not re-sampled.
- **L1** `[code]`. `normalize_abs` still does `set -- $1` after setting
  `IFS=/` (`scripts/launch-coreelec.sh`). A glob in the value still expands.
  Not re-run.
- **N2** `[code]`. `publishRaw` returns true whenever `mosquitto_publish`
  returns success (`src/mqtt_controller.cpp`). `src/mqtt_controller.h`
  still says that means a live connection.
- **N3** `[code]`. `busyRetired >= kMaxBusyRetired` still runs before every
  new open, including `restart` (`src/stream_player.cpp`).
- **N4** `[code]`. `src/stream_player.h` still ties `_exit` to a worker
  inside the blocking call. `finishProcess` always `_exit`s.
  `exitIfBusy` is still test-only.
- **F4** `[code]`. `reaperBusy` still skips the replacement open until the
  retired worker returns.
- **F5** `[code]`. `av_find_best_stream` (`src/ffmpeg_decoder.cpp`) is still
  the only video pick. No attached-picture check.
- **F6** `[code]`. `testAdtsAudioMaster` is still the packed-audio fixture.
- **F7** `[code]`. `scripts/ctest-require.sh` still requires only
  `ffmpeg_decoder`, `present_smoke`, and `stream_player_http`. On this VM
  mosquitto was installed, so `mqtt_exit_test` and `mqtt_reconnect` ran
  and passed.

---

## 4. Considered, not findings

- **Reject the size before `av_frame_get_buffer`.** AUDIT8 named that as
  the preferred shape. #54 rejects at copy, after `publishYuv420` has
  scaled. The 8224 `--url` transcript shows the `video 8224x32` line and
  then `cannot be presented`. That thin frame matches the tall-frame path
  this cap was aligned to: one converted frame, then the 1 s retry, not a
  present loop. The first pass stopped there. A 12000×12000 MJPEG does
  not stay cheap. That case is **D1** in [CROSSCHECK-A](#crosscheck-a),
  agreed as a should-fix.
- **`present` failure still does not back off.** `tick` logs
  `present failed` and continues when `present` returns false
  (`src/stream_player.cpp`). After #54 a frame that fails
  `frameAcceptable` never gets there from the player. Layout cells for a
  positive viewport stay inside it. The leftover trigger is a PPM write
  error when `--render-dir` is set. Default MQTT mode does not write PPM.
  Not promoted.
- **8192×32 at the cap.** `--url` presents it (rc 0). `--viewport 8192x32`
  inits. The cap is inclusive on both paths.
- **B2's other shapes** (absolute-path segments, `FILE://`, `crypto+file`,
  `concat`, `subfile`, master variant, AES key, 302 to `file://`). Not
  re-run. `urlIsLocalFileSource` and the whitelist string did not change.
  The http and `hls+http` file-segment cases in `ffmpeg_decoder` still
  fail closed.
- **Scheme-less local playlist with a `file://` segment.** The allowance
  #51 kept. Not re-run.
- **B1's empty will versus `finishProcess`.** Already a nit. No new
  `online:true`.

---

## 5. Blocked on GS-King X

- Which MP3 decoder name CoreELEC's FFmpeg registers first (A5).
- F3 / N3: armv7 address space at the cap.
- Hardware decode and DRM/KMS present. `docs/research/` was not reviewed.
- The size cap was measured with FFmpeg 6.1.1 on this host. A device
  FFmpeg that reports a different linesize for an 8192-wide frame would
  still have to pass `frameAcceptable` before copy.

---

## 6. How this was checked

- **Suite:** `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` on
  `e53336a`. 14/14 passed, ctest 120.41 s. Host packages: g++ 13.3.0,
  FFmpeg 6.1.1, libmosquitto 2.0.18, libdrm 2.4.125, mosquitto 2.0.18.
- **P1 probes:** `ffmpeg` lavfi fixtures `edge.mp4` (h264 8192×32) and
  `wide.ts` (h264 8224×32). `build/amlogic-multistream --url … --frames`.
  `ffprobe` confirmed the 8224 geometry. `--viewport 8193x10` and
  `--viewport 8192x32` against a closed port.
- **Diff:** `git show e53336a` and
  `git diff 0518a7e..e53336a -- src tests scripts CMakeLists.txt`.
- **Not rebuilt:** silent nameserver, B1's post-CONNECT sample, L1's glob
  sample, the AUDIT7 reaper-cap RSS harness.

---

## CROSSCHECK-A

Independent rating of [#55](https://github.com/yyolk/amlogic-multistream/pull/55)
(`AUDIT9-B.md`, `origin/cursor/audit9-b-second-look-1eb2` @ `7b1f8c3`)
against this tree's `main` base `e53336a`. No production code changed.
B claims no must-fix. The only should-fix items B claims are **D1** and
**J1**. E1, H1, and U1 are B's nits and stay there.

### D1 — AGREE (should-fix)

`publishYuv420` calls `av_frame_get_buffer` and `sws_scale` at the decoded
size before `frameAcceptable` runs (`src/ffmpeg_decoder.cpp`). `open()`
never sets `max_pixels`. On this FFmpeg 6.1.1, `avcodec_alloc_context3`
for `mjpeg` and `h264` leaves `max_pixels` at `INT_MAX` (2147483647).
`mjpeg` is on `kCodecWhitelist`, and `mov`/`mp4` are top-level formats, so
an MQTT `http://` add of that file is in scope. A one-frame 12000×12000
MJPEG MP4 through `build/amlogic-multistream --url … --frames 1` on
`e53336a` printed `video 12000x12000 yuvj420p -> yuv420p`, then
`Decoded frame 0 12000x12000 cannot be presented`, and exited 1. Peak RSS
was **456700 KB**. Copy failure is `WorkerFail::Read`; `tick` retires the
worker and waits `kRetryDelay` (1 s) (`src/stream_player.cpp`). The table
holds `LayoutEngine::kMaxStreams` (9) workers, so nine such URLs overlap.
The 8224×32 path this audit dropped is still the cheap one. This square
is not, and it is worth bounding before a hardware backend copies the
same order. It is not a must-fix: the player exits the frame, frees the
decoder on retire, and does not stay retained-online.

### J1 — DEMOTE-TO-NIT

A throwaway driver of `parseFlatJsonObject` + `executeCommand` on
`{"id":"cam","url":"http://127.0.0.1/a\nMQTT status {\"online\":true}"}`
printed a following line that is exactly `MQTT status {"online":true}`
(`src/mqtt_json.cpp` decodes `\n`; `mqttUrlAllowed` rejects only NUL;
`StreamManager::addStream` logs the URL raw). The same run's
`buildStatusJson` kept the newline escaped (`\n` inside the URL string),
and a `\u0000` URL was rejected. The retained status document is the
escaped payload `publishRaw` logs. A reader that treats any
`MQTT status` line as that document can be fooled. That is a log footgun,
the same class as the existing SIGKILL-before-publish window, not a
wrong retained status.

### Must/should in AUDIT9.md that B missed

None. The first pass recorded no must-fix and no should-fix. Re-reading
`e53336a` does not promote B1, L1, N2–N4, or F4–F7. The pre-buffer drop
in §4 is the item D1 replaces; it was not a survivor B failed to list.

### AGREED SURVIVORS

Must-fix and should-fix only. J1 is not in this table.

| ID | Severity | Finding |
| --- | --- | --- |
| **D1** | should-fix | Extent cap runs after decode and `sws_scale`. A 12000×12000 MJPEG still converts, peaks at 456700 KB RSS, fails present, and the MQTT path retries that decode every 1 s, up to 9 streams. |
