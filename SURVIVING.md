# Survivors: `main` @ 7a0f243

Filtered from [REVIEW.md](REVIEW.md). That file is the unfiltered Opus pass.
This file is the list to act on. No production code was changed here, and no
GitHub issues were filed.

Scope of the filter: the near-term POC is software decode, then a renderer,
then the MQTT loop. Hardware-only ideas stay only when they stop the
documented CoreELEC / AppImage launch. Checked against the tree and, where
noted, re-run locally on Ubuntu 24.04 (glibc 2.39, FFmpeg 6.1).

---

## Survivors

### 1. blocker — SSH drop leaves Kodi stopped

- **Was:** A1. The SIGINT half of that writeup is wrong (see below).
- **Where:** `scripts/launch-coreelec.sh:11-18`, `src/mqtt_controller.cpp:106-110`
- **Evidence:** The script stops Kodi, runs the binary, then starts Kodi, with
  no trap. The player blocks only SIGINT and SIGTERM and `sigwait`s them, then
  exits 0. Reproduced with a stand-in that does the same:
  - SIGHUP to the shell (what an SSH disconnect delivers) kills the script
    after the "app is running" line and before the restart line. bash and a
    `setsid` session both did this.
  - SIGTERM to the shell alone does the same.
  - SIGINT does **not**. A child that handles SIGINT and exits 0 is followed
    by the restart line under bash. Ctrl+C on the real player takes that path.
- **Fix:** Before `systemctl stop kodi`, install a restart on `EXIT HUP TERM`.
  An `EXIT` trap is enough on bash (verified: it runs on SIGHUP). CoreELEC's
  `/bin/sh` may be dash-like; dash does not run `EXIT` on a fatal signal, but
  a `HUP` trap does run and then the script continues. Do not trap `INT`: the
  app is still shutting down when the shell sees Ctrl+C, and starting Kodi
  from that trap races the display. One restart path is enough. `|| true` can
  stay; it is not what strands Kodi.

### 2. blocker — The aarch64 AppImage is the wrong binary for this box, and the launch script cannot mount an AppImage

- **Was:** A2, corrected. The glibc-2.39 claim does not reproduce. The
  137-library slim-down is not a launch blocker.
- **Where:** `.github/workflows/ci.yml:4-7` and `:34-37` (artifact described as
  the S922X device build), `README.md:125-139`, `scripts/launch-coreelec.sh:16`,
  `scripts/build-appimage.sh:97-98` (extract-and-run is set only while
  packaging).
- **Evidence:**
  1. Beelink GS-King X is S922X on the CoreELEC **Amlogic-ng** image
     (`g12b_s922x_beelink_gs_king_x.dtb`). Current 21.3-Omega publishes that
     image as `CoreELEC-Amlogic-ng.arm-…`, 32-bit userspace. The CI device
     artifact is aarch64. The kernel will not load it. README already says a
     32-bit userspace cannot run the file; the workflow still calls that
     artifact the S922X build, and the launch script execs
     `/storage/amlogic-multistream` with no arch check.
  2. The launch script never sets `APPIMAGE_EXTRACT_AND_RUN`. The build script
     sets it only so linuxdeploy can run on the runner. CoreELEC's base image
     does not ship `fusermount` (it lives in an addon, same as current
     LibreELEC). A type-2 AppImage then fails in the runtime, before `main`.
  3. glibc, rechecked on this Ubuntu 24.04 host (the CI distro): walk of the
     player's `ldd` closure, undefined symbols only, 136 objects. Highest
     requirement is `GLIBC_2.38` (46 objects, including `libx265`, `libva`,
     `libvdpau`). Nothing needs `GLIBC_2.39`. The 2.39 versions sit on
     `libc.so.6` / `libm.so.6` as **exports**, which linuxdeploy does not
     bundle. CoreELEC 21.3-Omega pins glibc to `44f757a6` (`release/2.38`).
     That floor matches. Do not retarget the build at Debian bullseye over this.
- **Fix:** Produce a 32-bit ARM binary for the Amlogic-ng image (or stop
  calling the aarch64 AppImage the device build). In `launch-coreelec.sh`,
  export `APPIMAGE_EXTRACT_AND_RUN=1` before exec. Leave the FFmpeg build fat
  until a device run shows unpack cost.

### 3. major — A multi-variant HLS master fetches every variant and decodes the small one

- **Was:** A7, narrowed. No tile-size argument on `open()`.
- **Where:** `src/ffmpeg_decoder.cpp:96-99` (no `AVDISCARD_ALL` anywhere).
- **Evidence:** Local master playlist, 320x240 and 640x480, served by
  `http.server`. `--url http://127.0.0.1:8765/master.m3u8 --frames 3` logged
  `GET` of both media playlists and of `lo/index0.ts`, `hi/index0.ts`,
  `lo/index1.ts`, and `hi/index1.ts`. The decoder reported `stream=0` and
  `320x240`. Bandwidth scales with variant count on the path the POC already
  runs.
- **Fix:** Inside `FfmpegDecoder::open`, after `avformat_find_stream_info`,
  pick one video variant (highest `variant_bitrate`, else highest resolution)
  and set `AVDISCARD_ALL` on every other stream before the read loop. Keep
  variant choice local to the decoder until a renderer has tile sizes.

### 4. major — MQTT will hand the decoder any libavformat URL, as root

- **Was:** A9. Still true, and still not wired. Fix it before that wiring.
- **Where:** `src/mqtt_commands.cpp:91-108`, `src/stream_manager.cpp:29-49`,
  `src/ffmpeg_decoder.cpp:75-82`.
- **Evidence:** `executeCommand("add")` stored `pipe:0`, `file:///etc/hostname`,
  `tcp://127.0.0.1:9`, and `concat:file:///etc/hostname` with `ok=true`.
  Separately, `--url file:///tmp/clip-proof.mp4` decoded a real frame:
  `open` sets no `protocol_whitelist`. `src/mqtt_json.cpp` `appendUtf8` writes
  a raw `NUL` for `\u0000`, and `avformat_open_input` takes `c_str()`, so a
  later prefix check and the decoder can disagree on the string.
  Docs default the broker to anonymous with no TLS (`docs/mqtt.md:20`).
  `launch-coreelec.sh` calls `systemctl`, so on CoreELEC this process is root.
- **Fix:** In `add`, accept only `http://` and `https://`. Reject a `NUL` in
  the url in the same check. Pass
  `protocol_whitelist=http,https,tls,tcp,crypto,hls` into
  `avformat_open_input`. Keep `file` on the `--url` CLI path only, behind an
  explicit local-file allowance, so the existing fixture tests still run.
  Do this before `StreamManager` entries are passed to `FfmpegDecoder`.

### 5. major — The renderer has nowhere to put a decoded frame

- **Was:** A6, software half only.
- **Where:** `src/renderer.h:3-8`, `src/drm_renderer.cpp:4-11`,
  `src/app.cpp:118-120`.
- **Evidence:** `Renderer` is `init()` plus `present()`. `present()` takes no
  frame and no rectangle. `DrmRenderer::init` prints a line and returns
  `true` without opening a device. The `--url` path never calls it, because
  it cannot hand over the `DecodedFrame` it already built. `LayoutEngine::compute`
  already returns the tile rects a software blit needs.
- **Fix:** When the software present path is written, give `Renderer` one
  call that takes a `DecodedFrame` and a `LayoutEngine::Rect`, and make
  `init()` return false until a display is actually opened. CPU-frame copy
  is the POC. No DMA-BUF handle, no per-tile KMS plane, no GE2D.

### 6. major — The MQTT loop cannot run next to decode

- **Was:** A12, narrowed. No `snapshot()`, condvar, or version counter.
- **Where:** `src/mqtt_controller.h:53-54`, `src/mqtt_controller.cpp:131-136`,
  `src/app.cpp:187-190`.
- **Evidence:** `playing_` is private and documented as touched only on the
  mosquitto thread, so the process that will draw frames cannot see
  start/stop. `MqttController::start()` blocks the calling thread in
  `sigwait` after `mosquitto_loop_start`. `App::run` has no third place to
  decode or present: `--url` returns before MQTT, and the MQTT path never
  returns until the signal. Putting `FfmpegDecoder::open` inside the command
  callback would also stall the mosquitto thread (keepalive) for the 15s
  `rw_timeout` in `ffmpeg_decoder.cpp:19`.
- **Fix:** Move the playing flag onto `StreamManager` (the object `App`
  already owns). Split `start()` so it returns once the network loop is up,
  and let `App` own `sigwait` plus a decode/present loop that reads
  `StreamManager::list()`. Add an interrupt callback only if that loop has to
  block inside `open`/`readFrame`.

### 7. minor — CI stays green when the decoder tests never run

- **Was:** B11. Skip the ASan job; a local sanitizer run is not a CI gap.
- **Where:** `scripts/run-tests.sh:47-54`, `CMakeLists.txt:57-80`,
  `tests/ffmpeg_decoder_test.cpp:219-239`.
- **Evidence:** `run-tests.sh` fails only if `layout_engine_tests` is missing
  from the ctest list. If the libav dev packages are absent, CMake omits
  `ffmpeg_decoder` with a STATUS line and the script still passes. If the
  `ffmpeg` CLI is missing, the test exits 77 and ctest treats that as skip,
  which is success. If libx264 is missing, HLS is skipped and the test
  returns 0. Today's workflow installs those packages, so current runs do
  execute `(mp4, hls)`. Nothing keeps the next image honest. The single-variant
  fixture also cannot see survivor 3.
- **Fix:** In CI, require the `ffmpeg_decoder` ctest to be present and to
  pass, and turn the libx264 miss into a failure instead of a quiet return 0.
  A two-variant fixture belongs with the discard fix, not a new job.

---

## Dropped

- **A1 SIGINT claim** — handled SIGINT that exits 0 still reaches `systemctl start kodi` (verified under bash).
- **A2 glibc 2.39 and the 137-lib diet** — undefined symbols top out at `GLIBC_2.38`; size is not why exec fails.
- **A3 cancellation** — nothing calls `open()` from MQTT yet; `rw_timeout` is already 15s. Add the interrupt in the loop from survivor 6.
- **A4 `readFrame` enum** — `--url` only needs stop-versus-frame. EOF versus error matters in a reconnect loop that does not exist.
- **A5 DMA-BUF `DecodedFrame`** — the POC copies CPU frames; README already says so.
- **A6 KMS / fbdev / GE2D / plane count** — not measured on the device, and it does not change the software blit.
- **A8 one bad packet kills the stream** — `failRead` on `AVERROR_INVALIDDATA` is real (`ffmpeg_decoder.cpp:300-314`), but a 400-byte hole in the middle of an MPEG-TS was concealed and all 30 frames came out. Reconnect policy waits for the loop.
- **A10 any layout string** — `setLayout` storing any non-empty token is the documented contract (`stream_manager.h:27`, `docs/mqtt.md:58`). `compute()` is not called with it yet; validate in the renderer PR.
- **A11 stream cap** — the table holds strings, and the layout engine already clamps tiles to 9. Cap when a decoder is created per entry.
- **A12 `snapshot()` / condvar / version** — one reader today. Survivor 6 is the smaller cut.
- **A13 broker-level MQTT test** — command functions are already covered; a live broker test is optional.
- **B1 add-message race** — the second writer does not exist.
- **B2 torn status** — same, one writer.
- **B3 restart-all on a missing id** — documented (`docs/mqtt.md:60`). `{"ID":"c"}` did restart every stream (`restarted 1` with no `id` match); that follows the contract. Fix the docs sentence that calls field names case-insensitive when someone next edits `docs/mqtt.md`.
- **B4 JSON edges** — `\u0000` is folded into survivor 4. Surrogates, raw UTF-8, and numeric ids are not on the POC path. No libFuzzer target.
- **B5 shared client id** — one box. Set `MQTT_CLIENT_ID` when a second process shares the broker.
- **B6 subscribe failure** — no failing subscribe in hand.
- **B7 unknown CLI flags** — `--mqtt-hots` is ignored (verified) and the process logs `MQTT broker localhost:1883`. The log is the check.
- **B8 credentials in `ps` and retained URLs** — TLS is already deferred. Home Assistant is the client that sent the URL.
- **B9 64 KiB cap after the broker buffer** — the broker's `message_size_limit` is the control that exists.
- **B10 CMake warnings / `Threads::Threads`** — the player links on glibc 2.39. pthread has been in libc since 2.34; CE 21.3 is 2.38.
- **B12 unpinned linuxdeploy** — supply-chain hygiene, not a POC failure.
- **B13 AppImage `--help` smoke** — `--help` does not catch survivor 2 (wrong arch, no FUSE). A deeper smoke test on the runner would not either.
- **B14 swscale copy, `thread_count`, color metadata** — unmeasured. Change it when one software stream misses frame rate.
- **B15 odd tile coordinates** — layout math is tested and correct. Alignment waits on a scaler that exists.
- **B16 hidden streams in status** — display-only. The table is still the source of truth.
- **B17 no persistence** — retained commands are ignored on purpose. Not a bug.
- **B18 slots reflow on remove** — insertion order is a fine grid until a hardware plane cares.
- **B19 stale docs** — `docs/mqtt.md` still calls `FfmpegDecoder` a stub and calls field names case-insensitive. Edit those sentences in the PR that touches the file.
- **B20 nits** — process-wide `av_log` level, the empty hwaccel hook, `GENPTS`, user-agent `0.1`, `mkdtemp` in the test, the gitignore pattern, `sleep 1`, `cancel-in-progress`, and log-line injection. None of them block decode, present, or MQTT.
