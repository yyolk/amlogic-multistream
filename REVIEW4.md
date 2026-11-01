# Fourth review: `main` @ 6641ca8 (after #17, #19, #20)

Reviewed commit: `6641ca8f6e647e39a32b544995f4bc1bd71ecdc1`
("Isolate a stalled stream so it cannot freeze the wall. (#20)").

This is the unfiltered pass. A separate adversarial pass will check these
claims and write `SURVIVING4.md`, so treat every item here as a claim to be
checked, not a task list. No production code was changed, and no GitHub
issues were filed.

Scope: the whole tree after #17 (CE21 armv7hf and CE22 aarch64 device
AppImages), #19 (SURVIVING3 minors), and #20 (B1: one decode thread per
stream, `DecoderBackend::abort()`, bounded SIGTERM). Measured against the
project goals: 1–9 concurrent HLS streams, MQTT control from Home
Assistant, fullscreen DRM/KMS with Kodi stopped, AppImage packaging, and a
RetroArch-style launch from Kodi.

**Severity.** **blocker** = defeats a project goal on a path this tree
already ships. **major** = wrong behavior on a shipped path, or a gap in a
stated goal. **minor** = real but contained. **nit** = polish.

**Evidence tags.** `[repro]` = reproduced on this VM (recipe in "How this
was checked"). `[code]` = from reading the code, not run. `[device]` =
needs a GS-King X running CoreELEC to confirm. Nothing here ran on a
GS-King X.

**Environment.** Ubuntu 24.04 x86_64 VM, g++ 13, FFmpeg 6.1 (libavformat
60.16.100), mosquitto 2.0.18, libdrm 2.4.125. `qemu-arm-static` for the
CE21 device AppImage. `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh`:
13/13 pass. CI on this commit is green: CI run 41275958370 and CoreELEC
device AppImages run 41275958346.

---

## Triage index

| Severity | Items |
| --- | --- |
| blocker | none |
| major | **T1** an uninterruptible DNS lookup still blocks the presenting thread on `remove`/`stop`/`restart` and delays SIGTERM; **K1** the Kodi add-on cannot pass MQTT credentials, so with Home Assistant's broker the launched player never connects; **P1** every stream is capped below 50 fps, so 50/60 fps live HLS falls behind and skips segments |
| minor | **H1** an HLS master with a packed-audio rendition is rejected outright; **M1** status has no per-stream health, and a trickling stream never fails; **O1** decode-thread log lines interleave; **CI1** the device workflow runs no tests on ARM and never decodes through MQTT; **CI2** no test for a backend that ignores `abort()`, for throughput, or for real-binary stall + SIGTERM |
| nit | **N1** `decoder_backend.h` says `close()` runs on the decode thread; **N2** a two-statement window in the launch script where HUP/TERM skips Kodi restore; **N3** `stream_player_stall` and `mqtt_reconnect` are not required tests |
| device-only notes | **I1–I3** (section 5): plane count vs per-tile present, hardware frame handles, DRM vs fbdev on CE21 |

---

## 1. Already fixed / verified on `6641ca8`

Do not re-raise these.

| Prior item | Status | Evidence on `6641ca8` |
| --- | --- | --- |
| SURVIVING3 #1 / REVIEW3 B1: one stalled, trickling, or live-edge stream freezes every tile and delays SIGTERM | **Fixed (#20), re-verified `[repro]`** | Each stream has its own `sessionLoop` thread with a one-frame slot (`src/stream_player.cpp:136-185`). `abort()` latches an `AVIOInterruptCB` (`src/ffmpeg_decoder.cpp:70-80`). `closeAll()` aborts every worker before joining any (`stream_player.cpp:225-233`). The real binary with a local broker and a healthy 10 fps MPEG-TS VOD stream presented 495 / 495 / 495 / 496 frames in 10 s with nothing / a blackhole URL / a trickle URL (1 byte every 5 s) / both added next to it. REVIEW3 measured 1 and 0 for the same cases. SIGTERM → exit took 0.05–0.10 s in every case, including an `https://` listener that accepts TCP and never answers the TLS handshake (0.104 s). REVIEW3 measured 35 s, and over 45 s for the trickle case. Live edge: 9 × 30 fps live HLS streams each presented all ~1,200 frames in 40 s. `stream_player_stall` logs `live+vod presents=152 … shutdownMs=17` (x86_64) and `shutdownMs=97` (aarch64) in CI. TSan: `stream_player_test`, `stream_player_stall_test`, `stream_player_http_test`, and an add/remove/restart/stop churn of the real binary reported 0 warnings. **The one remaining hole is T1 (DNS).** |
| SURVIVING3 #2: no device build for either CoreELEC target | **Fixed (#17)** | `.github/workflows/coreelec-appimage.yml` builds `coreelec21-ng-armv7hf` and `coreelec22-no-aarch64` against pinned CoreELEC sysroots. Run 41275958346 is green. The CE21 AppImage was rebuilt here with `scripts/build-coreelec-appimage.sh coreelec21-ng-armv7hf`, and `verify-coreelec-appimage.sh` passed under qemu-arm. |
| SURVIVING3 #3: an unclean exit loses the final status and buffered stdout | **Fixed (#19)** | Stdout is line-buffered (`src/main.cpp:9-10`), and the will is refreshed on each status publish (`docs/mqtt.md:54`). `mqtt_reconnect` covers SIGKILL keeping the last status line. Since B1 is fixed, SIGKILL is no longer the routine stop path, except under T1. |
| SURVIVING3 #4: a second HUP/TERM during restore skips `systemctl start kodi` | **Fixed (#19)** | `restore_kodi` ignores HUP/TERM/INT first (`scripts/launch-coreelec.sh:168-179`). `run_double_signal` in `scripts/test-launch-coreelec.sh:487` covers it. A two-statement window remains after a normal player exit (N2). |
| SURVIVING3 #5: `PLAYER_EXTRACT_DIR` is `rm -rf`'d | **Fixed (#19)** | `dangerous_extract_root` (`:72-90`) refuses `/`, `/storage`, `$HOME`, and any root that contains the script or the player. Only `squashfs-root` and `.source-stamp` are removed (`:229-231`). `run_extract_guards` (`:568`) covers it. |
| SURVIVING3 #6: MQTT `add` reaches every FFmpeg demuxer and decoder | **Fixed (#19)** | `kFormatWhitelist` / `kCodecWhitelist` (`src/ffmpeg_decoder.cpp:27-33`) are applied to open, the detected format, and the decoder (`:124-129`, `:158`, `:207`, `:234`). mp2 and opus audio in TS do not block the video open. One side effect is H1. |
| SURVIVING3 #7: nothing warns that Launch shows nothing on HDMI | **Fixed (#19)** | Startup message at `src/mqtt_controller.cpp:177-182`, plus `README.md:213` and `docs/kodi-addon.md:36`. |
| SURVIVING3 #8: exit 1 if the broker is unreachable at startup | **Fixed (#19)** | The first connect retries with 1–30 s backoff and still accepts signals (`mqtt_controller.cpp:184-214`). `mqtt_reconnect` covers it. K1 is the case where retrying cannot help. |
| SURVIVING3 #9: `signal_tree` overwrites the global `root` | **Fixed (#19)** | `local root sig dir pid pp` (`launch-coreelec.sh:131`). `run_signal_tree` (`:719`) covers it. |

**Not re-raised (dropped before, unchanged).** No TLS to the broker
(documented in `docs/mqtt.md:22`). `--mqtt-pass` on argv (the
`MQTT_PASSWORD` environment variable exists). `max_pixels` / 32-bit OOM
(REVIEW3 S2). SSRF (S3). VOD pacing (SURVIVING3 dropped P1 standalone).
One-core composite (P2). I1/I2 interface shape (deferred to the DRM PR;
section 5 only adds device constraints).

## 2. Intentional gates

These are tracked on purpose and are not findings.

- **Issue #14:** the Kodi add-on does not bundle the AppImage (by design;
  `docs/kodi-addon.md`, README).
- **Issue #18:** an optional GS-King X self-hosted runner, for
  manual-release device smoke only.
- **Amlogic hardware decode and real DRM/KMS scanout:** device-only. This
  build refuses `isHardware()` backends (`stream_player.cpp:139-145`), and
  `DrmRenderer` does not modeset or commit a plane (`src/drm_renderer.h:10-13`).

---

## 3. Major

### T1. major — An uninterruptible DNS lookup still blocks the presenting thread: `remove`, `stop`, and `restart` freeze every healthy tile, and SIGTERM waits for the lookup `[repro]`

- **Where:**
  - `src/stream_player.cpp:115-118`: `finishWorker()` calls `requestStop()`, then `worker.join()`, on the main (presenting) thread. Main-thread callers include `remove` via `dropMissing()` (`:235-256`), `stop` via `closeAll()` (`:284-290`), a tile hidden by a layout override (`:338-340`), and `restart` or a URL change (`:346-348`). Shutdown calls `closeAll()` at `src/mqtt_controller.cpp:247`, before the offline status is published (`:249-252`).
  - `src/ffmpeg_decoder.cpp:64-76`: the `AVIOInterruptCB` only takes effect inside FFmpeg's interrupt-aware waits. FFmpeg's `tcp` protocol resolves the host name with a blocking `getaddrinfo()` before it connects, and that call never consults the callback.
  - `src/decoder_backend.h:40-46`: the `abort()` contract says "The in-flight open() or readFrame() then returns". That does not hold for a backend that is blocked in a libc call.
- **What:** The real binary, a local mosquitto, and a healthy 10 fps MPEG-TS stream `ok`, plus `cam` = `http://camera.lan/live/index.m3u8`. This ran in a mount namespace whose `/etc/resolv.conf` points at a UDP socket that never replies (`nameserver 127.0.0.2`, `options timeout:5 attempts:2`, which are glibc's defaults). Send `start`, wait 2 s, then:

  | Action | Healthy tile `ok` | Result |
  | --- | --- | --- |
  | `remove {"id":"cam"}` | **0 presents per second for 8 s**, then 49–50 per second | The command was acknowledged at once. The wall stayed frozen until `getaddrinfo` gave up. |
  | SIGTERM | n/a | **Exit after 8.11 s** (rc 0). Without `cam`: 0.05 s. |

  - While nothing is removed, `ok` keeps presenting, so B1's isolation holds for reads. The hole is the join on the presenting thread.
  - After a failed lookup the stream retries every 1 s (`kRetryDelay`, `stream_player.cpp:13`) and enters `getaddrinfo` again. So a `stop`, `remove`, or SIGTERM almost always lands inside a lookup (about 10 s of every 11 s).
  - `closeAll()` aborts every worker before joining, so N such streams cost the longest lookup, not the sum.
  - With two unreachable nameservers the glibc bound doubles to about 20 s (5 s × 2 attempts × 2 servers). That is past the launch script's 10 s poll (`launch-coreelec.sh:152-161`), so the script SIGKILLs the player and the clean offline status is never published. This is arithmetic from glibc's resolver behavior; only the one-server case was run.
- **Why major:** `README.md:65` and #20 promise that "one stalled read does not freeze the wall or the signal wait". A camera addressed by host name, plus a LAN resolver that drops queries (a router reboot, Pi-hole down, a VPN route gone), brings back a B1-shaped freeze on every control action and a multi-second SIGTERM. It is bounded by the resolver configuration, so it is not a blocker. `[device]`: whether CoreELEC's resolver path drops queries, or answers SERVFAIL quickly when upstream is down, is unchecked.
- **Fix:**
  1. Never join a possibly blocked worker on the presenting thread. On stop, remove, or restart, call `requestStop()` and move the `Session` (thread plus decoder, already a `unique_ptr`) into a reaper list. Each tick, join only the workers whose `sessionLoop` has returned (an atomic `done` flag set on every return path).
  2. At shutdown, publish the offline status first, then join with an overall deadline (for example 2 s). If a worker is still inside libc, flush stdout and `_exit(0)`.
  3. Optional: resolve in the worker with `getaddrinfo_a()` and a deadline that checks the abort flag. This only helps the first host, because HLS segments can name other hosts, so (1) is the real fix.
  4. Test: a `FakeDecoder` whose `open()` ignores `abort()` and sleeps for N s (a stand-in for any libc call). Assert that the other tile keeps presenting across a `remove`, and that shutdown honors the deadline.

### K1. major — The Kodi add-on cannot pass MQTT credentials, Home Assistant's Mosquitto add-on refuses anonymous clients, and the player retries forever, so Launch stops Kodi for a player that never connects `[repro + code]`

- **Where:**
  - `kodi-addon/plugin.program.amlogic-multistream/resources/lib/amlogic_launcher.py:32-52`: `build_command()` passes only `MQTT_HOST`, `MQTT_PORT`, and `MQTT_TOPIC_PREFIX`. `resources/settings.xml:30-53` has no username or password setting. The `systemd-run` transient unit does not inherit Kodi's environment, so nothing else reaches the player.
  - The player supports `MQTT_USERNAME` / `MQTT_PASSWORD` (`docs/mqtt.md:18-19`, `src/mqtt_controller.cpp:151-153`), and `docs/mqtt.md:22` says broker authentication "is expected" outside a lab LAN.
  - Home Assistant's official Mosquitto add-on, `mosquitto/DOCS.md`: "This app does not support anonymous logins; all connections must use a username/password to connect." Its changelog for 6.0.0 says "Support for anonymous logins has been removed".
- **What:** mosquitto with `allow_anonymous false` and a `password_file`. The player was started with host, port, and prefix only, which is exactly what the add-on produces. stderr loops `MQTT connect rejected: Connection Refused: not authorised.` and `MQTT connection lost: The connection was refused. (will retry)` until SIGTERM. No status is ever published and no command is ever received.

  On the add-on path, the script stops Kodi (`launch-coreelec.sh:253`), and the screen shows no player picture by design. The player can never be reached over MQTT, and has no quit command anyway (`docs/kodi-addon.md:44`). The only way back is `systemctl stop amlogic-multistream-launch` over SSH or a Home Assistant shell command (`docs/kodi-addon.md:44-47`). The only trace of the cause is in the journal.

  There is an undocumented workaround: set "Launch script override" to a wrapper that exports `MQTT_USERNAME` and `MQTT_PASSWORD`, then runs `exec /bin/sh <bundled launch-coreelec.sh>`.
- **Why major:** The add-on is the RetroArch-style launch path, and Home Assistant is the stated controller. With Home Assistant's own broker, the shipped add-on path cannot work at all. This holds even before DRM exists, and it will still hold once it does.
- **Fix:**
  1. Add `mqtt_username` and `mqtt_password` settings (`option="hidden"` for the password) and pass them like the other three, or through an `EnvironmentFile=` with mode 0600 in the add-on's profile directory, which keeps them out of `systemctl show`.
  2. Treat a CONNACK of "not authorised" or "bad username or password" as fatal after a few attempts: exit non-zero so the launch script starts Kodi again. Unlike an unreachable broker (SURVIVING3 #8), a refused login does not fix itself.
  3. Extend the `kodi_addon` test to assert that the credentials reach the unit.

### P1. major — Every stream is capped below 50 fps: the one-frame slot plus a fixed 20 ms wait makes 50/60 fps live HLS fall behind and skip segments `[repro]`

- **Where:**
  - `src/mqtt_controller.cpp:64`, `:229-235`: the loop is `tick()`, then `sigtimedwait` with a 20 ms timeout, whether or not frames are waiting.
  - `src/stream_player.cpp:376-381`: each tick takes at most one frame per stream. `:160`: the worker blocks until the slot is empty, so its demuxer stops reading.
  - `src/software_renderer.cpp:215`, `:231`: each `present()` copies the whole viewport (2.7 MB at 1280x720), which adds to every tick.
- **What:**
  - **Ceiling:** the healthy 10 fps VOD clip presented 495 frames in 10 s (49.5 per second) in every B1 run. That is the tick rate, not the clip's rate.
  - **60 fps live:** `ffmpeg -re` `testsrc` 320x240 at 60 fps, libx264, HLS with 1 s segments, a 6-entry playlist, and `delete_segments`. One stream, `--viewport 1280x720`, 40 s. About 1,680–1,740 frames were presented (about 43 fps). The source produced 2,400. Each 1 s segment took about 1.4 s of wall time to consume: `seg2` was fetched 1.36 s after `seg1`, and `seg3` 1.42 s after `seg2`. Segments 11, 15, 18, 22, 25, 29, 33, and 36 were never requested, because hls.c jumped ahead once they expired from the playlist. The FFmpeg warning that reports the skip is hidden by `av_log_set_level(AV_LOG_ERROR)` (`ffmpeg_decoder.cpp:115-117`), and the player logs nothing.
  - **Control:** 9 × 30 fps live streams on the same host each presented all ~1,200 frames in 40 s. On the host the cap only bites at 50 fps and above. On the device, tick cost (software composite of up to 9 tiles on A73/A53) comes out of the same budget: 30 fps keeps up only while a tick stays under about 13 ms. `[device]`
- **Why major:** 50 Hz and 60 Hz HLS (broadcast and IPTV, many NVR re-streams) is within "1–9 concurrent HLS streams". The steady state is "as far behind live as the playlist allows, jumping a segment now and then", with no log line. The cap is structural to the B1 loop that the DRM backend will inherit. Caveat: software mode scans nothing out yet, so today the effect shows only in PPM output and fetch logs.
- **Fix:**
  1. Wake the presenting thread when a frame arrives instead of sleeping a fixed 20 ms. For example, workers write to an `eventfd`, and the main loop `ppoll()`s that fd plus a `signalfd` for SIGINT, SIGTERM, and SIGHUP, so signals stay on one thread.
  2. For live streams, let the newest frame win: the worker overwrites an unpresented frame (and counts the drop) instead of blocking its demuxer. Decode then keeps pace with the network, and the display rate becomes the presenter's choice (vsync, once DRM exists).
  3. Test: a `FakeDecoder` that yields frames as fast as they are requested. Assert at least 55 presents per second for one stream over 2 s. Today this fails at 49.5.

---

## 4. Minor and nit

### H1. minor — An HLS master with a packed-audio rendition (`.aac`, `.ac3`, `.mp3` segments) fails to open, so the whole stream is rejected `[repro]`

- **Where:** `src/ffmpeg_decoder.cpp:27`: `kFormatWhitelist = "hls,mpegts,mov,mp4,mpeg"`, set at `:128`. hls.c copies the whitelist to every rendition's segment demuxer and opens each rendition inside `avformat_open_input`, before `AVDISCARD_ALL` (`:181-191`) can apply.
- **What:** A master playlist with `#EXT-X-MEDIA:TYPE=AUDIO,…,URI="audio.m3u8"` (ADTS `.aac` segments) and one TS video variant fails with `[aac @ …] Format not on whitelist 'hls,mpegts,mov,mp4,mpeg'`, then `FfmpegDecoder::open failed … avformat_open_input: Invalid argument`, and 0 frames. The same master without the `EXT-X-MEDIA` line presents. `ffprobe` opens the original (h264 + aac). In MQTT mode the stream retries every 1 s forever, the tile stays black, and status lists it as normal (M1).
- **Why minor:** Packed-audio renditions are valid HLS (RFC 8216 §3.4) and common in broadcast and Apple-tooling streams. Camera and NVR HLS usually muxes audio into TS or fMP4, so reach is limited. It is a side effect of the SURVIVING3 #6 fix.
- **Fix:** Add the elementary-stream demuxers hls.c uses for packed audio (`aac,ac3,eac3,mp3`) to `kFormatWhitelist`. Their decoders are already in `kCodecWhitelist`. Add a two-rendition master to `ffmpeg_decoder_tests`, which already builds an HLS fixture with libx264.

### M1. minor — Status has no per-stream health, and a trickling stream never fails or retries `[code + repro]`

- **Where:** `src/mqtt_json.cpp:411-439`: each stream carries only `id`, `url`, and `muted`. Worker failures go to stderr only (`stream_player.cpp:406-413`). A trickle never fails: `rw_timeout` (15 s) restarts with every byte, and `ffmpeg_decoder.cpp:64-69` deliberately installs no read deadline (an interrupted live-edge wait ends the HLS stream).
- **What:** In every B1 run, the trickle stream presented 0 frames for the whole run and never logged a failure. H1's rejected stream and T1's unresolvable host retry forever. In all of these, status says `"playing":true` and lists the stream like a healthy one.
- **Why minor:** Home Assistant is the stated controller and cannot show "camera 3 offline" or decide to restart it. No wrong action is taken.
- **Fix:** Add a per-stream `state` (`opening`, `playing`, `retrying`), `frames`, and `last_error` or `last_frame_age_ms` to status, published on state change rather than per frame. Add a watchdog in `StreamPlayer`, not in the interrupt callback: no decoded frame for, say, 20 s → `abort()`, then retry with backoff.

### O1. minor — Log lines from decode threads interleave mid-line `[repro]`

- **Where:** `src/main.cpp:10` sets `std::cout << std::unitbuf`, which flushes after every `<<`. Worker threads log with several insertions per line: `stream_player.cpp:155` and `ffmpeg_decoder.cpp:249-252`. Each insertion is atomic, but the line is not.
- **What:** Seen on the host (churn run) and on the CE21 AppImage under qemu-arm:

  ```text
  FfmpegDecoder::open FfmpegDecoder::open http://127.0.0.1:29701/clip.tshttp://127.0.0.1:29701/master_noaudio.m3u8 stream= stream=00 codec= codec=h264h264 software isHardware=0
  StreamPlayer open StreamPlayer open hlsts
  ```

- **Why minor:** The journal is the only diagnostic on the box, and a garbled line puts a stream under the wrong id (`hlsts`).
- **Fix:** Build each line in a `std::string` and write it with one `std::cout.write()` (or one `fputs` under a logging mutex). The `std::cerr` lines from workers (`logAvError`, `ffmpeg_decoder.cpp:160`, `:208`) need the same.

### CI1. minor — The device workflow runs no tests on 32-bit ARM, and its smoke never decodes through MQTT `[repro]`

- **Where:** `scripts/build-coreelec-appimage.sh:255` builds `--target amlogic-multistream` only. `scripts/verify-coreelec-appimage.sh:19-25`, `:190-255` checks `--help`, `--url` on a local fixture, and an MQTT connect + subscribe + SIGTERM with no streams.
- **What:**
  - The B1 threading code has never run on armv7 in CI. The five dependency-free tests (`layout_engine_tests`, `stream_manager_test`, `mqtt_smoke_test`, `software_renderer_tests`, and the threaded `stream_player_test`) cross-build with `arm-linux-gnueabihf-g++` and pass under `qemu-arm` here.
  - The MQTT decode path through CoreELEC's FFmpeg, including `https` (CE21's libavformat links `libssl.so.3`), is not exercised by CI. A manual run of the CE21 AppImage under qemu-arm: `add` a TS stream, an HLS stream, an `https` TS stream, and a blackhole URL, then `start`. All three healthy streams presented (1,911 PPM files), the blackhole was isolated, and SIGTERM → exit took 0.11 s. So the code works today, and CI would not notice if it stopped. CE22 was not run here.
  - `README.md:192` describes the smoke accurately. This is about coverage, not honesty.
- **Fix:** In the device job, build the five pure tests with the same toolchain and run them under `$CE_QEMU` with the sysroot. Extend the verify script's MQTT step: `add` a local HLS URL and a blackhole URL, `start`, require at least one present via `--render-dir`, and time SIGTERM → exit.

### CI2. minor — No test for a backend that ignores `abort()`, for throughput, or for a real-binary stall + SIGTERM `[code]`

- `tests/stream_player_test.cpp`'s `FakeDecoder` never blocks. `stream_player_stall_test` uses the real `FfmpegDecoder`, which honors `abort()`. Nothing models T1's case.
- No test asserts a frame rate above the tick rate (P1).
- `stream_player_stall_test` is in-process (`StreamPlayer` plus `closeAll()`). The controller's shutdown order (`closeAll()` before the offline publish, `mqtt_controller.cpp:247-252`) and the binary's exit time with stalled streams are not covered. `run_real_player_case` (`scripts/test-launch-coreelec.sh:801`) uses an idle player. SURVIVING3 #1 fix item 4 asked for the real-binary version.
- **Fix:** The T1 and P1 tests above, plus one `mqtt_reconnect`-style case: real binary, a blackhole and a trickle URL, `start`, SIGTERM, and assert the exit time and a retained `"online":false`.

### N1. nit — `decoder_backend.h` says `close()` runs on the decode thread; it runs on the main thread `[code]`

`src/decoder_backend.h:45-46`: "close() still runs on the decode thread". `finishWorker()` calls `decoder->close()` on the main thread after the join (`stream_player.cpp:127-132`). The join provides the ordering, so nothing breaks today. A hardware backend that ties handles to the opening thread would read the wrong promise. Fix: "close() runs after the decode thread has exited, on the thread that owns the session."

### N2. nit — After a normal player exit, a two-statement window lets HUP/TERM kill the script before Kodi is started `[code]`

`scripts/launch-coreelec.sh:268` resets HUP and TERM to their defaults (`trap - EXIT HUP TERM INT`). `restore_kodi` ignores them again only at `:172`. A HUP or TERM in between kills the shell, and `systemctl start kodi` never runs. The window is one function call, and this was not reproduced. Fix: clear only `EXIT` at `:268`. A signal there then runs `restore_kodi; exit 0`, which starts Kodi.

### N3. nit — The B1 regression test can be skipped silently `[code]`

`scripts/ctest-require.sh:5-9` requires `ffmpeg_decoder`, `present_smoke`, and `stream_player_http`. `stream_player_stall` and `mqtt_reconnect` can exit 77 and still pass CI, and the stall test's live case returns 0 when libx264 is missing (`tests/stream_player_stall_test.cpp:556-561`). CI 41275958370 does run both today, so this is latent. Fix: add both to `DECODER_TESTS`, and fail the live case when `AMS_REQUIRE_DECODER_TESTS=1`.

---

## 5. Hardware decode / DRM interface notes (device-only, not findings)

SURVIVING3 deferred I1 and I2 to the DRM PR. These notes add device
constraints that PR has to design around. They are not blockers, and each
one needs the box.

- **I1 `[device]` Plane count vs per-tile present.** `Renderer::present(frame, dest)` draws one stream into one cell (`src/renderer.h:12-14`). A natural DRM mapping is one scanout plane per tile. Amlogic VPUs expose few video planes: mainline meson has one OSD primary plane and one video overlay, and G12B BSP kernels have VD1/VD2. A 4- or 9-tile wall of decoded frames therefore needs a composition step (GE2D, GPU, or CPU) into one buffer and a single commit per tick. `Renderer` has no begin/commit step, and `StreamPlayer` holds a `SoftwareRenderer&` (`src/stream_player.h:39`, `:131`). Check with `modetest -p` on CE21 and CE22.
- **I2 `[device]` Hardware frame handles.** `StreamPlayer` refuses `isHardware()` backends (`stream_player.cpp:139-145`), `copyOwned()` copies CPU planes (`:19-46`), and `DecodedFrame` has no DMA-BUF fd, modifier, or release callback. Zero-copy needs a frame handle with explicit release that can cross the one-frame slot, and P1's newest-frame-wins option needs that release on overwrite.
- **I3 `[device]` DRM vs fbdev on CE21.** CE21 Amlogic-ng runs a 4.9 BSP kernel. Whether it exposes `/dev/dri/card0` with atomic KMS, or only fbdev plus `/dev/amvideo`, decides whether `DrmRenderer` (which opens `card0` and requires the atomic and universal-plane caps, `src/drm_renderer.cpp:24-57`) is the right backend for the armv7 image at all.

## 6. Needs a device (GS-King X / CoreELEC)

- **T1:** `cat /etc/resolv.conf`, then `time getent hosts camera.lan` with the upstream DNS unreachable. Does the box wait out the glibc timeouts, or fail fast?
- **P1:** tick time at 4 and 9 tiles in software mode on the S922X (the margin for 30 fps sources).
- **I1 / I3:** `ls -l /dev/dri /dev/amvideo /dev/video*`, and `modetest -p` plane list on CE21 and CE22.
- Hardware decoder instance count for 9 concurrent streams (amvdec or v4l2m2m).
- **K1:** whether any remote or CEC key path can stop the launch unit without SSH.

---

## 7. How this was checked

All runs used `build/amlogic-multistream` from `cmake -S . -B build && cmake --build build` on `6641ca8`, a throwaway mosquitto (`allow_anonymous true`, `persistence false`) on 127.0.0.1, and `mosquitto_pub` to `<prefix>/cmd/{add,remove,start}`. "Presents" are PPM files written by `--render-dir`, or `presented <id> frame N` lines (logged every 60 frames).

- **B1 re-verify:** a Python server on 127.0.0.1 with three handlers: a static 10 fps MPEG-TS file, a blackhole (accept, never reply), and a trickle (HTTP 200 headers, then 1 byte every 5 s). An `https` variant wraps the blackhole socket in a TLS listener that never completes the handshake. `--viewport 64x36 --render-dir DIR`, `add` the healthy URL plus 0, 1, or 2 bad URLs, `start`, count PPMs over 10 s, then `kill -TERM` and poll `kill -0` every 50 ms.
- **T1:** a root Python UDP socket bound to `127.0.0.2:53` that reads and never replies. Inside `sudo unshare -m`, bind-mount a file containing `nameserver 127.0.0.2` / `options timeout:5 attempts:2` over `/etc/resolv.conf`, and run the B1 setup with `cam` = `http://camera.lan/live/index.m3u8`. `start`, wait 2 s, then either `remove {"id":"cam"}` and count PPMs per second, or SIGTERM and time the exit. Run the binary directly as root inside the namespace; wrapping it in `su` adds su's own TERM handling.
- **K1:** mosquitto with `allow_anonymous false` and a `mosquitto_passwd` file. Start the player with `MQTT_HOST`/`MQTT_PORT`/`MQTT_TOPIC_PREFIX` only, read stderr for 10 s, then SIGTERM.
- **P1:** `ffmpeg -re -f lavfi -i testsrc=size=320x240:rate=60 -c:v libx264 -preset ultrafast -tune zerolatency -g 60 -f hls -hls_time 1 -hls_list_size 6 -hls_flags delete_segments+omit_endlist`, served by a logging HTTP server. Player `--viewport 1280x720`, one `add`, `start`, 40 s, SIGTERM. Compare the last `presented` count with 60 × 40, and list which `segN.ts` were never requested. The 9 × 30 fps control is the same with `rate=30` and nine `add`s of the same URL.
- **H1:** a master with `#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="aud",NAME="en",DEFAULT=YES,AUTOSELECT=YES,URI="audio.m3u8"` (ADTS `.aac` segments) and `#EXT-X-STREAM-INF:…,AUDIO="aud"` pointing at a TS video playlist, served by `python3 -m http.server`. `amlogic-multistream --url http://…/master.m3u8 --render-dir DIR` versus the same master without the `EXT-X-MEDIA` line, plus `ffprobe` on the original.
- **O1:** grep any multi-stream `out.log` for `StreamPlayer open StreamPlayer open`.
- **CI1:** `scripts/build-coreelec-appimage.sh coreelec21-ng-armv7hf`, then `scripts/verify-coreelec-appimage.sh coreelec21-ng-armv7hf dist/…AppImage`. For the MQTT decode run, `--appimage-extract`, run `squashfs-root/AppRun` under `qemu-arm-static -L <CE21 sysroot>` against the local broker, and `add` http TS, http HLS, https TS, and a blackhole. For the pure tests, configure a second build with `arm-linux-gnueabihf-g++`, build the five dependency-free test targets, and run each under `qemu-arm-static -L /usr/arm-linux-gnueabihf`.
- **TSan:** `-DCMAKE_CXX_FLAGS=-fsanitize=thread`, run the three `stream_player*` tests, and churn the real binary (add, remove, restart, stop, start in a loop with a blackhole URL present), then SIGTERM. 0 reports.
