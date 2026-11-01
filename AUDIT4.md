# Audit of the SURVIVING4 filter and fix PRs #23–#25: `main` @ 58a2f9c

Audited commit: `58a2f9c114daf7835c7b2b3c8c3201b9a350705c`
("Stop a stuck DNS lookup from blocking the presenting thread (#25)").
Pre-fix tip: `6641ca8f6e647e39a32b544995f4bc1bd71ecdc1` (#20).

Inputs:

- `REVIEW4.md`, unfiltered review of `6641ca8`: commit `33f302e` on
  `cursor/review4-unfiltered-9f4e` (closed draft
  [#21](https://github.com/yyolk/amlogic-multistream/pull/21)).
- `SURVIVING4.md`, the adversarial filter: commit `2cd75dd` on
  `cursor/surviving4-filter-9dcd` (closed draft
  [#22](https://github.com/yyolk/amlogic-multistream/pull/22)).
- Fixes on `main`: [#23](https://github.com/yyolk/amlogic-multistream/pull/23)
  H1 packed-audio whitelist (`9e5d696`),
  [#24](https://github.com/yyolk/amlogic-multistream/pull/24) K1 Kodi MQTT
  credentials and auth exit (`6b41a9a`),
  [#25](https://github.com/yyolk/amlogic-multistream/pull/25) T1 reaper
  and bounded exit (`58a2f9c`).

Scope: filter mistakes and fix regressions only. This is not a new
full-repo review. No production code was changed, and no GitHub issues
were filed. The candidate fixes below were tried in a scratch worktree
only. They are not committed anywhere.

**Severity** follows REVIEW4: **blocker** = defeats a project goal on a
shipped path. **major** = wrong behavior on a shipped path, or a gap in a
stated goal. **minor** = real but contained. **nit** = polish.
**Evidence:** `[repro]` = run on this VM (recipes at the end). `[code]` =
from reading the code. **needs-GS-King-X** = needs the box to confirm.
Nothing here ran on a GS-King X.

**Environment.** Ubuntu 24.04 x86_64, g++ 13.3, glibc 2.39, FFmpeg 6.1.1
(libavformat 60.16.100), mosquitto 2.0.18, `nsswitch.conf` `hosts: files dns`.
`AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` on `58a2f9c`:
13/13 pass.

---

## Verdict

No filter mistake changes what had to be fixed. Every keep, downgrade,
and drop holds up, and no dropped or downgraded item needs to come back.
One drop (O1) rested on weak evidence and still reproduces, but it is
cosmetic. The three fixes do what the survivors asked. B1 stall isolation
holds. The K1 auth exit stays non-zero and never becomes `_exit(0)`. The
H1 whitelist adds only the four demuxers HLS renditions need, though it
also admits them as a top-level format (A4).

The problems are in what the survivors prescribed, and in how #25 met #24
at the rebase:

| Priority | Item | Severity | Came from |
| --- | --- | --- | --- |
| **must fix** | **A1** after `_exit(0)` the broker publishes the stale CONNECT-time will over the final offline status | minor (regression) | #25, as prescribed by REVIEW4 and SURVIVING4 T1 |
| **must fix** | **A2** the auth give-up exit joins a stuck decode thread with no deadline, which the docs say cannot happen | minor (rebase gap) | #24 + #25 |
| should fix | **A3** `restart` / `stop`+`start` during a stuck lookup stacks decode threads | minor (new) | #25 |
| should fix | **A4** the packed-audio demuxers are also accepted as the top-level format | minor | #23, as prescribed by SURVIVING4 H1 |
| nit | **A5** `mp3` in `kCodecWhitelist` never matches FFmpeg's `mp3float` decoder | nit | #19, restated by SURVIVING4 H1 |
| nit | **A6** the `sigtimedwait` error path can exit 0; `--help` still says stop "closes" | nit | #25 |

A1 and A2 are marked must-fix even though each is minor. Each breaks a
guarantee the fix PR documents, and each fix is under 20 lines,
verified in a scratch build. A1's fix was corrected after the
cross-check with Gemini's audit (`CROSSCHECK-OPUS.md`).

---

## 1. Filter calls, re-checked on `58a2f9c`

| REVIEW4 item | SURVIVING4 call | Audit verdict |
| --- | --- | --- |
| T1 DNS lookup blocks the presenting thread | major, kept | **Correct.** Fixed by #25 `[repro]`. A1–A3 are gaps in that fix. |
| K1 add-on cannot pass MQTT credentials | major, kept | **Correct.** Fixed by #24. The add-on side is sound (section 4). A2 is on its exit path. |
| H1 packed-audio rendition rejected | minor, kept | **Correct.** Fixed by #23 for all five rendition types `[repro]`. The prescribed fix was broader than needed (A4), and one premise was wrong (A5). |
| P1 cap below 50 fps | major → deferred until scanout | **Defensible.** Software mode scans nothing out, and none of #23–#25 changed the tick loop. #25 adds two `reapFinished()` calls per tick, which walk an empty vector in steady state. |
| M1 no per-stream health | dropped | **Defensible.** It is a feature, and the reasoning was accurate. #23 removes one of its cases (H1). |
| O1 torn decode-thread log lines | dropped ("logs from this pass did not contain a torn line") | **The evidence for the drop was weak, but the outcome is fine.** It still reproduces: 2 torn lines out of 639 over 15 start/stop cycles of 9 streams, for example `FfmpegDecoder: video 160x120 yuv444pFfmpegDecoder::open  -> yuv420p` `[repro]`. Not seeing a race once does not refute REVIEW4's repro. The impact is cosmetic, so it stays a nit, not a must-fix. |
| CI1 no ARM unit tests in the device workflow | dropped | **Defensible.** The `ubuntu-24.04-arm` CI job runs the threaded tests natively on aarch64. armv7 differs mainly in address space, which matters only for A3. |
| CI2 no abort-ignoring / throughput / real-binary stall test | dropped | **Partly vindicated.** #25 added the abort-ignoring `StallingDecoder` test. But nothing tests `MqttController`'s shutdown with a blocked worker, and A1 and A2 are both there. REVIEW4's CI2 recipe would not have caught them either: a blackhole or trickle URL honors `abort()`, so it never reaches `_exit(0)`. |
| N1 `close()` comment | dropped | **Moot.** #25 rewrote `decoder_backend.h:42-47` correctly. |
| N2 HUP/TERM window before `restore_kodi` | dropped | **Defensible nit.** Still present (`scripts/launch-coreelec.sh:270-271`, then `:174`). The fixes did not touch it. The script ignores the player's exit status (`:267`, `wait … \|\| true`), so K1's non-zero exit affects only the unit status and the journal. |
| N3 `stream_player_stall` / `mqtt_reconnect` skippable | dropped | **Defensible, but it matters more now.** The K1 regression test lives in `mqtt_reconnect`, which still exits 77 without mosquitto and is not in `scripts/ctest-require.sh`. CI installs mosquitto, so this is latent. |
| I1–I3 device notes | not findings | Fine. |

---

## 2. Fix regressions and gaps

### A1. minor, regression from #25 — After `_exit(0)`, the broker publishes the stale CONNECT-time will over the final offline status `[repro]`

- **Where:**
  - `src/mqtt_controller.cpp:261-268`: `finishProcess` runs `closeAll()`, publishes the retained offline status, then calls `player.exitIfBusy(kExitJoinDeadline)`.
  - `src/stream_player.cpp:281-289`: when a worker is still blocked, `exitIfBusy` flushes stdio and `_exit(0)`s. It does not, and cannot, send an MQTT DISCONNECT, because `StreamPlayer` does not own the client.
  - The broker uses the will from the CONNECT packet. `refreshWill()` (`mqtt_controller.cpp:475-492`) only changes what the *next* CONNECT will carry, as `docs/mqtt.md:54` itself says. For a player that connected once and then received its `add`s, the will the broker holds is the startup one: an empty table. Dropping TCP without DISCONNECT makes the broker publish it, retained.
- **What:** real binary, local broker, a healthy MPEG-TS stream `ok`, and `cam` = `http://camera.lan/live/index.m3u8` with a resolver that never answers. SIGTERM about 1.5 s after `start`, with a subscriber on `<prefix>/status`:

  | Build | SIGTERM → exit | Status messages after SIGTERM | Retained status afterwards |
  | --- | --- | --- | --- |
  | `6641ca8` (pre-fix) | 8.54 s, rc 0 | 1: final status | `"online":false,"playing":true,"layout":"1x2"`, streams `ok`, `cam` |
  | `58a2f9c`, 1 nameserver | **2.02 s**, rc 0 | **2**: final status, then the will | **`{"online":false,"playing":false,"layout":"1x1","layout_setting":"auto","streams":[]}`** |
  | `58a2f9c`, 2 nameservers | 2.02 s, rc 0 | 2 | same empty will |
  | `58a2f9c`, no `cam` | 0.007 s, rc 0 | 1 | final status with `ok` |
  | scratch: DISCONNECT before `_exit(0)` | 2.02 s, rc 0 | 1 | final status, streams `ok`, `cam` |

- **Why it matters:** `docs/mqtt.md:54` says SIGINT, SIGTERM, and SIGHUP "are a clean shutdown: … publishes a final retained status from the live table". The T1 survivor fix ordered "publish the offline status before the shutdown join" so that status would survive a stuck lookup. `_exit(0)` without a DISCONNECT undoes that step on exactly the path it was written for. Before #25 this path was slow but clean whenever the lookup finished inside the launch script's 10 s poll. REVIEW4 (T1 fix 2) and SURVIVING4 (survivor 1 fix) both prescribed "flush and `_exit(0)`" without a disconnect, and #25 implemented that as written. So this is a gap in the filter's prescription that carried into the code.
- **Why minor:** `"online":false` is still right, so Home Assistant sees the player go offline. The wrong fields are the table, the layout, and `playing`, and only after a shutdown that found a worker still blocked 2 s in (the T1 conditions). The next launch starts with an empty table anyway.
- **Fix:**
  - **Corrected after the cross-check (`CROSSCHECK-OPUS.md` section 1).** The first version here called `shutdown()` unconditionally before `_exit(0)`. That keeps the table (last table row), but `mosquitto_loop_stop` then joins libmosquitto's reconnect thread. When the broker is configured by DNS name and has dropped, that thread is stuck in `getaddrinfo`, and SIGTERM took 8.54 s (one nameserver) or 18.56 s (two) `[repro]`.
  - Use the gated form instead: have `publishRaw` return whether the publish was queued, then `if (!player.drain(kExitJoinDeadline)) { if (sent) { shutdown(); } flush; _exit(0); }`. That kept the table and exited in 2.02 s in both cases, and the suite passed 13/13.
  - `MqttController::start` hard-codes `FfmpegDecoder`, so a controller-level test needs a decoder-factory seam to put `stream_player_test`'s `StallingDecoder` behind it. Then assert that the retained status still lists the streams after SIGTERM.

### A2. minor, rebase gap between #24 and #25 — The auth give-up exit joins a stuck decode thread with no deadline `[repro]`

- **Where:**
  - `src/mqtt_controller.cpp:270-277`: `player.closeAll(); shutdown(); return 1;`. There is no `drain` and no deadline. `return 1` runs `~StreamPlayer` (`src/stream_player.cpp:62-75`), which joins every reaper thread unconditionally.
  - That destructor's comment (`:63-65`) says "Process exit calls exitIfBusy() and _exit(0) before this destructor runs". That is false on this path. `src/stream_player.h:29-32` and `docs/mqtt.md:107` ("abandoned on process exit") make the same claim.
  - Signals stay blocked (`mqtt_controller.cpp:199-203`) and nothing calls `sigtimedwait` during the wait. A `systemctl stop` in that window goes unanswered until the launch script's 10 s poll SIGKILLs the player.
- **What:** the player connected anonymously and was playing `ok` and `cam` (silent resolver). The broker was then restarted with `allow_anonymous false`. Rejections came 1.0, 5.0, and 14.0 s after the drop (libmosquitto's quadratic backoff), then `MQTT authentication failed after 3 attempts; exiting`. The process exited **5.04 s** later with one nameserver and **4.04 s** later with two, rc 1 both times. Each exit landed right as the in-flight `getaddrinfo` returned. The wait is the remainder of the current lookup, so the bound is one full lookup: about 10 s with one silent nameserver, about 20 s with two. A scratch build with a bounded drain followed by `_exit(1)` exited in 2.04 s with rc 1.
- **Guarantee check:** the K1 auth exit did **not** become `_exit(0)`. This path never calls `exitIfBusy`, and rc was 1 in every run, including the existing `mqtt_reconnect` cases. The failure is the opposite one: the exit is not bounded.
- **How it happened:** #25 was opened (07:15:38Z) before #24 merged (07:16:32Z) and was rebased onto `6b41a9a`. `finishProcess` covers the signal and `sigtimedwait` exits that existed on #25's original base. It does not cover the `return 1` that #24 added, and #25's docs and comments were not updated for it.
- **Why minor:** it needs a mid-session credential change. Refusals on the first connect arrive before any `add`, so there is no worker yet. It also needs a silent resolver at the same time. Kodi still comes back, just up to one lookup late.
- **Fix:** give this path the same bounded drain, keeping exit status 1: `closeAll(); if (!drain(kExitJoinDeadline)) { shutdown(); flush; _exit(1); } shutdown(); return 1;`. If `exitIfBusy` stays, give it an exit-code parameter (that also fixes A6). Then correct `stream_player.cpp:63-65`, `stream_player.h:29-32`, and `docs/mqtt.md:107`.

### A3. minor, new with #25 — `restart` or `stop`+`start` during a stuck lookup stacks decode threads `[repro]`

- **Where:** `src/stream_player.cpp:400-405` retires the old worker on a generation or URL change, and `:407-428` starts a replacement in the same tick (`generationChanged` skips the backoff). The retired worker stays on `reaper_` until libc returns. Nothing caps retired-but-running workers per id or in total. Before #25, the join on the presenting thread meant there was never more than one worker per id, and repeated restarts collapsed into one.
- **What:** `cam` inside a silent-resolver lookup, then `restart {"id":"cam"}` ten times a second for 10 s:

  | Build | threads / fds / VmSize after 10 s | 12 s later |
  | --- | --- | --- |
  | `6641ca8` | 4 / 8 / 413 MB | 4 / 8 / 413 MB |
  | `58a2f9c` | **101 / 105 / 2,989 MB** | 4 / 8 / 2,229 MB |

  Each stuck `getaddrinfo` holds a UDP socket. The steady-state count is roughly command rate × lookup time × streams. Cycling `stop`/`start` does the same thing for every visible stream.
- **Why minor:** it needs a client that churns commands while the resolver is silent. The pile is bounded by the lookup timeout, and a failed `std::thread` is caught and backs off (`:243-250`). **needs-GS-King-X:** the CE21 armv7 AppImage is a 32-bit process. Each thread reserves a default 8 MB stack in an address space of at most 4 GB, so a few hundred stacked threads exhaust it. This was not run on armv7.
- **Fix:** do not start a replacement while the same id's previous worker is still on `reaper_` and not `done`, and treat that as backoff (a restart cannot beat the resolver anyway). Alternatively, cap `reaper_` at about 2 × `LayoutEngine::kMaxStreams`. Test: `StallingDecoder` plus N restarts, asserting that at most two `open()` calls are in flight per id.

### A4. minor, from #23 as prescribed by SURVIVING4 H1 — The packed-audio demuxers are also accepted as the top-level format `[repro]`

- **Where:** `src/ffmpeg_decoder.cpp:29-30` widened `kFormatWhitelist`. The same constant drives the post-open check of the detected demuxer (`:162-169`), which is the one place the top level could have stayed narrow. The open-time `format_whitelist` has to be wide, because hls.c copies it onto each rendition. The post-open check does not.
- **What:**
  - `--url http://…/song.mp3`, an MP3 with an ID3 APIC JPEG cover. `6641ca8`: `Format not on whitelist`, rc 1. `58a2f9c`: `FfmpegDecoder::open … stream=1 codec=mjpeg`, presents the cover art, rc 0.
  - MQTT `add` of the same URL plus `start`, 10 s. `6641ca8`: 10 open failures, 0 frames. `58a2f9c`: 10 opens, 10 presented cover frames, and `no frame from song (will retry)` each second. Status lists it like a camera. Each cycle reads the file to EOF, or up to the 8,192 skipped non-video packet cap (`:423-427`), before retrying `[code]`.
  - `plain.aac` (bare ADTS) now opens the `aac` demuxer and fails later at `no video stream`.
- **Why minor:** the decoder side is unchanged. The codec whitelist still gates it, and `mjpeg` was already allowed. What changed is that MQTT `add` now reaches four elementary audio demuxers, plus their ID3v2/APE tag parsing, as direct top-level targets. SURVIVING3 #6 set out to narrow exactly that. An audio URL also now "works" as a still image instead of failing. H1 needs these demuxers only inside HLS.
- **Fix:** add a top-level list (`hls,mpegts,mov,mp4,mpeg`) for the check at `:162-169`, and keep the wide list for `format_whitelist`. In a scratch build, all five packed-audio masters and plain TS still present, while `song.mp3` and `plain.aac` are refused with `demuxer is not allowed`. Add `song.mp3` to `ffmpeg_decoder_tests` as a rejection case.

### A5. nit, from #19 and restated by SURVIVING4 H1 — `mp3` in `kCodecWhitelist` never matches FFmpeg's MP3 decoder `[repro]`

`kCodecWhitelist` (`src/ffmpeg_decoder.cpp:35-36`) is matched against decoder names. On FFmpeg 6.1, `avcodec_find_decoder(AV_CODEC_ID_MP3)` returns `mp3float`; aac, ac3, and eac3 return their own names. The MP3-rendition master still opens and presents (audio is discarded), but every open logs `[mp3float] Codec (mp3float) not on whitelist` four times, and an MQTT stream retries every second. SURVIVING4 H1's "their decoders are already on `kCodecWhitelist`" and `docs/mqtt.md:41`'s "so a container probe can open the audio stream" are wrong for MP3. Fix: list `mp3float`, or reword the docs. **needs-GS-King-X:** which MP3 decoder name CoreELEC's FFmpeg registers first.

### A6. nit, from #25 — `exitIfBusy` hard-codes status 0, and `--help` still says stop "closes" `[code]`

- The `sigtimedwait` error path (`src/mqtt_controller.cpp:289-294`) calls `finishProcess()` and then `return 1`. With a blocked worker, `exitIfBusy` `_exit(0)`s first. `mqtt_controller.h:27-28` promises 1. This is practically unreachable, because `sigtimedwait` with a valid timeout fails only with `EAGAIN` or `EINTR`. The same exit-code parameter as A2 fixes it.
- `printUsage` (`mqtt_controller.cpp:412`) still says "stop closes them". `README.md:65` and `docs/mqtt.md:5` now say stop retires decoders and does not join a blocked one.

---

## 3. Guarantees in scope

- **B1 stall isolation holds.** `remove` during a stuck lookup: 0 s of frozen wall on `58a2f9c` (25 presents per 0.5 s throughout), against 8.0 s at 0 presents on `6641ca8` `[repro]`. `stream_player_stall` passes (`live+vod presents=151 shutdownMs=14`). ThreadSanitizer reports 0 warnings on `stream_player_test` and `stream_player_stall_test`. One note: TSan's default `die_after_fork=1` kills the new forked `stuckExitChild` case, so run that test with `TSAN_OPTIONS=die_after_fork=0`.
- **The K1 auth exit is not `_exit(0)`.** rc was 1 in all runs (A2). It is unbounded, though.
- **The H1 whitelist is not over-broad for renditions.** It adds exactly the four demuxers hls.c needs. It is over-broad at the top level (A4).

## 4. Clean

These parts of the fixes look sound and need no change:

- **#23 (H1):** HLS masters with a packed-audio rendition of each type present on `58a2f9c` and are refused on `6641ca8`: ADTS, ADTS behind an ID3v2 tag, AC-3, E-AC-3, and MP3 (SURVIVING4 only had ADTS fixtures). Variant selection and `AVDISCARD_ALL` are unchanged.
- **#24 (K1, add-on side):**
  - The username goes in `--setenv`. The password goes in `mqtt.env`, mode 0600, passed as `EnvironmentFile=`, and the file is removed when the password is cleared.
  - `quote_systemd_env` escapes exactly systemd 255's `SHELL_NEED_ESCAPE` set (`"`, `\`, `` ` ``, `$`) inside double quotes.
  - The password is not on argv, and not in the Kodi log (`default.py:73` logs argv only).
  - `tests/kodi_addon_test.py` covers all of the above.
  - Auth detection matches CONNACK 4/5 and MQTT 5 `0x86`/`0x87`. A broker that is down still retries (the existing `mqtt_reconnect` checks pass).
- **#25 (T1):** SIGTERM with a stuck lookup now exits in 2.02 s instead of 8.54 s, inside the launch script's 10 s poll, even with two silent nameservers. A healthy shutdown still takes 0.007 s. The reaper only joins `done` workers. `done` is set on every return path of the thread lambda (`stream_player.cpp:229-241`).
- **Docs after the rebase:** `README.md:65` and `:205`, `docs/kodi-addon.md:28-36` and `:60`, and `docs/mqtt.md:22`, `:39`, and `:41` match the code, except where A1, A2, A5, and A6 say otherwise.

## 5. Candidate fixes, scratch only

A scratch worktree at `58a2f9c` (not pushed) carried the fixes for A1, A2, and A4: 18 insertions and 2 deletions in `mqtt_controller.cpp` and `ffmpeg_decoder.cpp`. With them, SIGTERM keeps the retained table, the auth exit is bounded at 2 s with rc 1, the packed-audio masters still play, and top-level `mp3`/`aac` are refused. `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` on that tree: 13/13. That tree's A1 fix was the unconditional `shutdown()`. The gated form in A1 replaces it, and it was built and tested separately (`CROSSCHECK-OPUS.md` section 6).

## 6. Needs a GS-King X

- **A3:** thread and address-space headroom of the CE21 armv7 AppImage under command churn, and `ulimit -s` inside the `amlogic-multistream-launch` unit.
- **A5:** which MP3 decoder name CoreELEC's FFmpeg registers.
- **K1:** a transient unit started with `--property=EnvironmentFile=` on CE21 and CE22 systemd. This VM has no systemd as PID 1, so it was checked only against the env-file parser rules and the add-on test's model of them.
- **T1 (carried over):** whether CoreELEC's resolver waits out the glibc timeouts or fails fast. That decides how often A1–A3 can happen at all.

---

## 7. How this was checked

- **Builds:** `58a2f9c` via `scripts/run-tests.sh` (g++). `6641ca8` and the scratch tree in separate worktrees, each with `CXX=g++ cmake … && cmake --build`.
- **T1 / A1 / A2 / A3 harness (root):** a Python UDP socket on `127.0.0.2:53` (and `127.0.0.3:53` for two nameservers) that reads and never replies. A `resolv.conf` with those nameservers and `options timeout:5 attempts:2`, bind-mounted over `/etc/resolv.conf` inside `unshare -m`, where the player is `exec`'d. A throwaway mosquitto on 127.0.0.1 (`persistence false`, `user root`). `python3 -m http.server` serves a 300 s 10 fps MPEG-TS clip. The player runs with `--viewport 64x36 --render-dir DIR`. `add` `ok` (the clip) and `cam` (`http://camera.lan/live/index.m3u8`), then `start`.
  - **remove:** count PPM files every 0.5 s, 2 s before and 12 s after `remove {"id":"cam"}`.
  - **SIGTERM:** subscribe to `<prefix>/status`, send SIGTERM, time the exit, then read the retained status with `mosquitto_sub -C 1`.
  - **auth exit:** after 2 s, restart the broker on the same port with `allow_anonymous false` and a `password_file`. Time from the give-up line to the exit.
  - **churn:** `restart {"id":"cam"}` every 100 ms for 10 s. Sample `/proc/PID/task`, `/proc/PID/fd`, and `VmSize`.
- **A4 / A5 / H1 fixtures:** `song.mp3` (lavfi sine plus a JPEG cover as `attached_pic`, ID3v2.3), `plain.aac` (ADTS). Five HLS masters, each with an H.264 TS video playlist and one `#EXT-X-MEDIA:TYPE=AUDIO` rendition segmented with `-f segment -segment_format adts|ac3|eac3|mp3`, and `adts -write_id3v2 1` for the ID3 case. Run with `--url … --frames 3|5 --render-dir DIR` on each build. The MQTT case uses `add` + `start` for 10 s and counts GETs and PPMs. Decoder names come from `avcodec_find_decoder` via ctypes.
- **O1:** 9 streams (TS and HLS alternating), then 15 rounds of `start`, 0.6 s, `stop`, 0.2 s. Grep the log for two open/format messages on one line.
- **TSan:** `-fsanitize=thread`, `stream_player_test` (with `die_after_fork=0`) and `stream_player_stall_test`.
