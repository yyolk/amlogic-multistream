# Code review: `main` @ 7a0f243 (after #1, #2, #3, #4)

Review only. No production code was changed. Findings are numbered so an
adversarial filter pass can accept or reject each one on its own. Do not file
issues from this file directly.

Severity: **blocker** = stops hardware bring-up or is unsafe to ship;
**major** = wrong behavior or an interface that has to change before the next
milestone; **minor** = real but contained; **nit** = polish.

`[HW]` = concern specific to GS-King X / S922X / CoreELEC that cannot be
fully verified without the device. `[confirmed]` = reproduced locally (see
"Verification performed" at the end).

---

## A. Must fix before hardware bring-up

### A1. blocker [HW] — Launch script can leave Kodi stopped permanently
- **Where:** `scripts/launch-coreelec.sh:11-18`
- **What:** `systemctl stop kodi`, run app, `systemctl start kodi` with no
  `trap`. If the script gets SIGHUP (SSH session drops), SIGTERM, or SIGINT,
  it dies before `systemctl start kodi`. The app process itself only blocks
  SIGINT/SIGTERM (`mqtt_controller.cpp:106-110`), so SIGHUP kills it with
  the default action too.
- **Why it matters:** The TV stays on a blank console with no UI. The only
  recovery is SSH or a power cycle, which is exactly what happens during
  bring-up.
- **Fix direction:** `trap 'systemctl start kodi' EXIT INT TERM HUP` set
  before stopping Kodi. Optionally run the app under `systemd-run --scope`
  or as a oneshot unit with `ExecStopPost=systemctl start kodi`. Propagate
  the app's exit code instead of `|| true`.

### A2. blocker [HW] (verify on device) — AppImage may not start on CoreELEC
- **Where:** `.github/workflows/ci.yml:34-37` (ubuntu-24.04 runners),
  `scripts/build-appimage.sh`, `scripts/launch-coreelec.sh:16`
- **What:**
  1. glibc floor. The binary needs `GLIBC_2.38`; the bundled Ubuntu libs
     need `GLIBC_2.39` (`objdump -T`, confirmed on a 24.04 build).
     linuxdeploy does not bundle glibc. If the device's glibc is older, the
     AppImage fails at load time.
  2. FUSE. CoreELEC normally has no libfuse2 or fusermount. The launch
     script runs `/storage/amlogic-multistream` without
     `--appimage-extract-and-run` or `APPIMAGE_EXTRACT_AND_RUN=1`. It is
     also unclear whether that path is meant to be the AppImage or the bare
     binary.
  3. Size and surface. `ldd` lists 137 shared objects, including
     `libva-x11`, `libvdpau`, `libOpenCL`, and `libx265`. Extract-and-run
     unpacks all of that into tmpfs (RAM) on every launch.
- **Why it matters:** The first launch on the device is likely to fail
  before `main()` runs.
- **Fix direction:** Run `ldd --version` on the target first. Build on the
  oldest glibc you need to support (for example a Debian bullseye or Ubuntu
  22.04 container on the arm runner) or use the CoreELEC toolchain. Make the
  launch script set `APPIMAGE_EXTRACT_AND_RUN=1`. Consider a minimal FFmpeg
  build for the device (see A9).

### A3. major — DecoderBackend has no cancellation; open/read can block 15 s+ [confirmed]
- **Where:** `src/decoder_backend.h:36-55`, `src/ffmpeg_decoder.cpp:75-82`
- **What:** No `AVFormatContext::interrupt_callback` and no `abort()` or
  `interrupt()` on the interface. `open()` against a black-hole host blocked
  for 15.2 s (the `rw_timeout`). HLS can chain several of these (master
  playlist, variant playlists, first segments in `avformat_find_stream_info`).
  The class is documented "not thread-safe", so `close()` from another
  thread is not allowed either.
- **Why it matters:** `remove`, `restart`, `stop`, layout changes, and
  SIGTERM all need to interrupt a stuck stream. Without this, shutdown can
  exceed systemd's stop timeout and one dead camera stalls the rest of the
  player.
- **Fix direction:** Add a thread-safe `requestAbort()` (atomic flag) to
  `DecoderBackend` and wire it to `interrupt_callback` in FfmpegDecoder.
  Document that it is the only method that may be called cross-thread.

### A4. major — `readFrame()` cannot tell EOF from error from "unsupported"
- **Where:** `src/decoder_backend.h:46-55`
- **What:** `false` means end of stream *or* a hard error. The default
  implementation also returns `false`, "so a DRM-prime hardware decoder
  stays linkable". To a caller, a backend that never yields CPU frames
  looks like a stream that hit EOF immediately.
- **Why it matters:** The future player loop needs a different reconnect
  policy for each case: EOF on a live stream means reconnect, a decode
  error means back off, and "no CPU frames" means use a different render
  path. With a hardware backend that keeps the default, the result is a
  tight reopen loop.
- **Fix direction:** Return an enum (`Frame`, `Again`, `Eof`, `Error`) and
  either make `readFrame` pure virtual or add a capability query
  (`outputKind()`: CPU planes, DMA-BUF, or direct-to-video-layer).

### A5. major [HW] — DecodedFrame ownership model does not fit zero-copy hardware output
- **Where:** `src/decoder_backend.h:9-35`, `README.md:17`
- **What:** Plane pointers are borrowed and only valid until the next
  `readFrame()` or `close()`. The README tells renderers to copy. The struct
  has no DMA-BUF fd, DRM fourcc/modifier, or refcount, and it is 8-bit
  YUV420P only.
- **Why it matters:** On S922X the useful hardware paths are V4L2 M2M with
  DMA-BUF (NV12/NV21) or the Amlogic amvideo/ionvideo path, which produces
  no CPU frames at all. Copying nine frames per vsync on the CPU defeats
  hardware decode. The borrowed lifetime also forces the decode thread and
  the render thread to run in lockstep.
- **Fix direction:** Before writing the hardware backend, change the frame
  to a refcounted handle (`std::shared_ptr<Frame>` wrapping an `AVFrame`
  ref or a DMA-BUF descriptor) with a tagged payload: CPU planes or a DMA-BUF.
  Add SAR, color range, and matrix while the struct is open (see B14).

### A6. major [HW] — Renderer interface cannot render anything yet; plane assumption is risky
- **Where:** `src/renderer.h:3-8`, `src/drm_renderer.cpp:4-11`
- **What:** `Renderer` has only `init()` and `present()`. There is no way to
  hand it frames or tiles, no layout input, no display-size query
  (`LayoutEngine::compute` needs the viewport), and no mode or vsync
  handling. `DrmRenderer::init()` prints "open /dev/dri/card0" and returns
  `true` without opening anything. The TODO says "atomic commit planes".
- **Why it matters:** Every caller written against this interface will
  break. `[HW]` The CoreELEC Amlogic-ng 4.9 vendor kernel drives the display
  through fbdev, OSD, and amvideo. KMS/atomic support and the number of
  overlay planes are unknown and are at best a handful (OSD1/2, VD1/2).
  One plane per tile (up to 9) will not exist. Tiles have to be composited
  into one buffer by GLES on Mali-G52 or by GE2D.
- **Fix direction:** First probe on the device what `/dev/dri` exposes
  (`modetest -p`), whether EGL works on GBM or fbdev, and whether GE2D is
  accessible. Then define `Renderer` around
  `submit(tileIndex, FrameHandle)`, `setLayout(Result)`,
  `displaySize()`, and `present()`. Make `init()` fail honestly until it
  is implemented.

### A7. major — HLS master playlists download every variant; variant choice is uncontrolled [confirmed]
- **Where:** `src/ffmpeg_decoder.cpp:96-99` (and no `discard` setup)
- **What:** Non-selected streams are never set to `AVDISCARD_ALL`, so the
  FFmpeg HLS demuxer keeps every variant playlist active. With a two-variant
  master playlist (320x240 and 640x480), every segment of *both* variants
  was fetched (`GET /lo/index0..4.ts` and `GET /hi/index0..4.ts`). In that
  test `av_find_best_stream` picked the **lower** variant (stream 0,
  320x240) while still downloading the higher one.
- **Why it matters:** Bandwidth is multiplied by the variant count, per
  stream, times nine streams. Picture quality is arbitrary. `[HW]` Nine
  1080p30 streams are about 560 Mpx/s, which is above a 4K60 decode budget
  (about 498 Mpx/s). A 3x3 wall has to pick variants that fit the tile size.
- **Fix direction:** After choosing the video stream, set `discard =
  AVDISCARD_ALL` on every other stream (and on audio until audio exists).
  Choose the variant explicitly by tile size and bitrate using
  `variant_bitrate` metadata or `AVProgram`s. Pass the target size through
  `open()` options, which affects A3/A4 interface work.

### A8. major — One corrupt packet permanently kills a stream [confirmed]
- **Where:** `src/ffmpeg_decoder.cpp:311-315`, `:279-284`
- **What:** `avcodec_send_packet` returning `AVERROR_INVALIDDATA` goes
  through `failRead`, which sets `failed_`. After that, `readFrame` returns
  false until the stream is reopened. A TS file with 400 corrupted bytes in
  the middle third stopped after 19 of 60 frames with "Invalid data found
  when processing input".
- **Why it matters:** Live HLS over Wi-Fi routinely produces damaged
  packets. Each one becomes a full reconnect, which costs seconds of black
  tile and a playlist reload.
- **Fix direction:** Treat `AVERROR_INVALIDDATA` (and similar per-packet
  errors) from send and receive as skip-and-continue, with a counter or
  rate limit. Only escalate to a hard failure on sustained errors or I/O
  errors.

### A9. major (security) — Any MQTT client can make the player open arbitrary FFmpeg URLs [confirmed: accepted into state]
- **Where:** `src/mqtt_commands.cpp:91-108`, `src/stream_manager.cpp:29-49`,
  `src/ffmpeg_decoder.cpp:75-82`
- **What:** `add` accepts any non-empty `url`. `pipe:0`, `file:///etc/shadow`,
  `tcp://…`, `concat:`, and `subfile:` are all stored and echoed back. The
  decoder sets no `protocol_whitelist`, `format_whitelist`, or
  `codec_whitelist`. MQTT is anonymous by default with no TLS
  (`docs/mqtt.md:20`), and on CoreELEC the process runs as root.
- **Why it matters:** This is harmless today because no decoder is opened
  from MQTT. Once the player loop wires `StreamManager` to `FfmpegDecoder`,
  anyone with LAN broker access gets SSRF, local file reads, stdin reads,
  and the full libavformat demuxer attack surface as root.
- **Fix direction:** Validate the scheme in `executeCommand("add")`
  (`http`/`https` only, plus `file` behind a CLI flag for tests). Set
  `protocol_whitelist=http,https,tls,tcp,crypto,hls` (plus `file` when
  allowed), and limit `format_whitelist` and `codec_whitelist` to what you
  support (for example `hls,mpegts,mov` and `h264,hevc`). Do this **before**
  connecting MQTT to the decoder.

### A10. major — `layout` accepts any string; invalid values are stored, echoed, and persist [confirmed]
- **Where:** `src/stream_manager.cpp:99-107`, `src/mqtt_commands.cpp:136-159`,
  `docs/mqtt.md:58`
- **What:** `{"layout":"banana"}`, `{"layout":4}` (numbers are stored as
  strings, B4), and `{"layout":"2X2"}` all return `ok:true` and become the
  effective `layout` in the retained status. `LayoutEngine::parseLayout`
  rejects all three, so the renderer's `compute()` call will fail and show
  nothing.
- **Why it matters:** One typo from Home Assistant gives a blank wall while
  status reports success. The docs describe this as intended behavior
  ("Any other non-empty string is stored"), so the contract itself needs to
  change.
- **Fix direction:** In `setLayout`, accept `auto` or anything for which
  `LayoutEngine::parseLayout` returns true. Optionally lowercase the input
  first. Reply `ok:false` otherwise. Update the docs and add a test
  (currently none covers rejection).

### A11. major — No limit on stream count or id/url size [confirmed: 34 streams accepted]
- **Where:** `src/stream_manager.cpp:29-49`
- **What:** `addStream` has no cap. 34 streams were accepted, and status
  grows with every one. `LayoutEngine` silently places at most 9 tiles
  (fewer with an override, see B16). The only length bound on id and url is
  the 64 KiB payload cap, applied per message.
- **Why it matters:** Unbounded memory use and retained-status size driven
  from the network. Once decoders are wired, there is no backpressure
  against hardware decoder instance limits `[HW]`.
- **Fix direction:** Cap at `LayoutEngine::kMaxStreams` (or a configured
  hardware limit) and reply `ok:false, "stream limit reached"`. Cap id
  length (for example 64) and url length (for example 2048).

### A12. major — Player-loop integration points are missing
- **Where:** `src/mqtt_controller.h:53-54`, `src/mqtt_controller.cpp:131`,
  `src/app.cpp:187-190`, `src/stream_manager.h`
- **What:**
  1. The `start`/`stop` state (`playing_`) is a private field of
     `MqttController`, "touched only on the mosquitto network thread", so a
     player loop cannot read it.
  2. `MqttController::start()` blocks the main thread in `sigwait`, leaving
     nowhere to run a render loop.
  3. `StreamManager` has no change notification (version counter,
     condvar, or observer), so a player would have to poll `list()`
     copies.
  4. Commands run on the mosquitto thread. If decoder open/close work is
     added inside command handling, it stalls MQTT, including keepalive
     (A3).
- **Why it matters:** The next milestone has to restructure `App`,
  `MqttController`, and `StreamManager` anyway. Doing it before more code
  depends on the current shape is cheaper.
- **Fix direction:** Move `playing` into `StreamManager`. Add
  `snapshot()` (one lock, returns streams, setting, effective layout,
  playing, and a version) plus a wait-for-change primitive. Split
  `MqttController` into `start()` (non-blocking) and `stop()`, and let
  `App` own signal handling and the main loop.

### A13. major (missing test) — `MqttController` has zero automated coverage
- **Where:** `CMakeLists.txt:26-40`, `tests/mqtt_smoke_test.cpp`
- **What:** `mqtt_smoke_test` exercises only the pure parser and command
  functions; it never links libmosquitto. Nothing tests the threading,
  signal-mask inheritance, retained-command filtering, the 64 KiB cap,
  resubscribe on reconnect, the will message, or the offline status on
  SIGTERM. CI already installs dependencies with apt, and `mosquitto` is a
  single package.
- **Why it matters:** That controller logic is where races and lifetime
  bugs would live, and it is the part `[HW]` bring-up depends on to control
  the box.
- **Fix direction:** Add an opt-in (`MOSQUITTO_FOUND` plus broker found)
  ctest that starts `mosquitto -p <ephemeral>`, runs the player binary,
  publishes commands, and checks responses and status, including
  `kill -TERM` producing a retained `online:false`. Rename
  `mqtt_smoke_test` to something like `mqtt_commands_test`.

---

## B. Minor and nits (fine to defer past bring-up)

### B1. minor — `add` reports "added" vs "already present" with a race and two full copies
- **Where:** `src/mqtt_commands.cpp:97-106`
- **What:** It takes `list().size()` before and after `addStream()` under
  separate locks. With a second writer (the future player removing failed
  streams), the message can be wrong. It also copies the whole table twice.
- **Fix:** Have `addStream` return an enum (`Added`, `AlreadyPresent`,
  `Conflict`, `Invalid`, `LimitReached`).

### B2. minor — Status JSON is built from three separate locks (torn snapshot)
- **Where:** `src/mqtt_controller.cpp:280-282`, `:139-140`
- **What:** `layout()`, `layoutSetting()`, and `list()` each lock
  separately, so the effective layout can come from a different stream count
  than the list next to it. No other writer exists yet. → `snapshot()` (A12).

### B3. minor — `restart` with a missing or misspelled `id` restarts all streams [confirmed]
- **Where:** `src/mqtt_commands.cpp:182-192`
- **What:** `{"ID":"a"}` (wrong case) returned `restarted 2`. Field names are
  case-sensitive, but the docs imply otherwise (B19).
- **Fix:** Require an explicit `{"all":true}` or a separate command for
  restart-all.

### B4. minor — JSON edge cases accepted or rejected unexpectedly [confirmed]
- **Where:** `src/mqtt_json.cpp:106-111`, `:138-147`, `:305-313`
- **What:**
  1. `\u0000` is accepted and puts a NUL inside `id`. That id was stored and
     echoed as `"b\u0000x"`. `url.c_str()` would truncate silently at the NUL.
  2. Valid surrogate pairs (`\ud83d\ude00`) reject the **whole payload** as
     "invalid json".
  3. Raw bytes ≥ 0x20 are not UTF-8-validated and are echoed verbatim, so
     status can contain invalid UTF-8 that strict consumers (Python `json`
     in Home Assistant) fail to decode.
  4. Numbers are stored as strings: `{"id":5}` gives id `"5"` and
     `{"layout":4}` gives `"4"` (feeds A10).
- **Fix:** Reject control characters, including decoded `\u0000`, in
  ids and urls. Decode surrogate pairs. Validate UTF-8. Keep numbers in a
  separate map or reject them for string fields. Add a libFuzzer target for
  `parseFlatJsonObject`. Depth limiting and truncation handling looked
  correct, and the parser is ASan/UBSan-clean on the existing tests.

### B5. minor — Fixed default client ID makes two boxes knock each other off [confirmed]
- **Where:** `src/mqtt_controller.h:30`, `:75`
- **What:** Two instances using the defaults each logged "connection lost" 3
  times in 6 s, an endless takeover loop. The prefix default is shared too,
  so both would also act on the same commands.
- **Fix:** Default the client ID to `amlogic-multistream-<hostname>` (or add
  a random suffix). Log the effective client ID at startup.

### B6. minor — Subscribe failure is logged once and never retried
- **Where:** `src/mqtt_controller.cpp:244-249`
- **What:** If `mosquitto_subscribe` fails, the client stays connected and
  ignores every command until the next reconnect. It still published
  `online:true` on an earlier connect.
- **Fix:** Disconnect so that reconnect retries the subscribe, or retry the
  subscribe directly. Check the granted QoS in `on_subscribe`.

### B7. minor — CLI: unknown flags silently ignored; bad port falls back
- **Where:** `src/mqtt_controller.cpp:166-196`, `:16-26`
- **What:** `--mqtt-hots 10.0.0.5` is ignored and the player connects to
  localhost. An invalid port warns and uses 1883 instead of failing. Decode
  mode, by contrast, rejects unknown arguments (`app.cpp:68`).
- **Fix:** Set `configError_` for unknown flags and invalid values.

### B8. minor (security) — Credentials exposure
- **Where:** `src/mqtt_controller.cpp:192`, `src/mqtt_json.cpp:431`,
  `src/stream_manager.cpp:47`, `src/ffmpeg_decoder.cpp:21`
- **What:** `--mqtt-pass` shows up in `ps` and `/proc/*/cmdline`. Stream URLs,
  which often carry tokens or `user:pass@`, are published in the
  **retained** status and logged to stdout and stderr. There is no TLS.
- **Fix:** Prefer the environment variable or a password file. Redact
  userinfo and query strings in logs and status, or make URLs in status
  opt-in. TLS is already listed as future work.

### B9. minor — The 64 KiB payload cap is applied after libmosquitto buffered the full message
- **Where:** `src/mqtt_controller.cpp:325`
- **What:** libmosquitto has already allocated the full packet before the
  check runs. Only the broker's `message_size_limit` protects the device's
  RAM.
- **Fix:** Document that the broker limit must be set, or use MQTT v5 with a
  `maximum-packet-size` CONNECT property.

### B10. minor — CMake: player target is built differently from the tests
- **Where:** `CMakeLists.txt:82-108`
- **What:** No `-Wall -Wextra` on `amlogic-multistream`. No
  `target_link_directories(${FFMPEG_LIBRARY_DIRS})`, which the test target
  has. No `Threads::Threads`, even though the code uses `pthread_sigmask`
  and threaded mosquitto. `[HW]` Cross-compiling against a CoreELEC sysroot
  or an older glibc (before pthread moved into libc in 2.34) is where this
  fails.
- **Fix:** Use `pkg_check_modules(... IMPORTED_TARGET)` and link
  `PkgConfig::FFMPEG` etc. Add `find_package(Threads)` and the warning
  flags.

### B11. minor — CI can go green while silently skipping the decoder tests
- **Where:** `scripts/run-tests.sh:47-54`, `CMakeLists.txt:57-80`,
  `tests/ffmpeg_decoder_test.cpp:200-240`
- **What:** Missing libav dev packages mean the target is not created.
  Missing ffmpeg CLI gives exit 77, which ctest reports as SKIP, not
  failure. Missing libx264 skips HLS with exit 0. `run-tests.sh` only
  requires `layout_engine_tests`. Today's CI logs show "(mp4, hls)", so all
  of this currently runs, but nothing enforces it.
- **Fix:** Add `-DAMS_REQUIRE_ALL_TESTS=ON` in CI that turns each skip into
  `FATAL_ERROR`, or fail on SKIP. Also consider an ASan/UBSan job; a local
  run was clean.

### B12. minor (supply chain) — AppImage tooling is unpinned and unverified; cache never refreshes
- **Where:** `scripts/build-appimage.sh:23-25`, `:83-95`, `.github/workflows/ci.yml:52-56`
- **What:** It downloads `continuous` builds of linuxdeploy, the plugin, and
  appimagetool with no checksum. The cache key `appimage-tools-…-v1` never
  changes, so whichever binary was downloaded first is used indefinitely.
- **Fix:** Pin release tags or SHA256 values and include them in the cache
  key.

### B13. minor — AppImage smoke test is only `--help`
- **Where:** `scripts/build-appimage.sh:120-121`
- **What:** `--help` never loads libav codecs or the network stack. A missing
  or broken bundled library (out of 137) would not show up until the device.
- **Fix:** Also run `--appimage-extract-and-run --url <generated fixture>
  --frames 1` inside the AppImage.

### B14. minor — FfmpegDecoder performance and fidelity
- **Where:** `src/ffmpeg_decoder.cpp:191-235`, `:115-136`, `src/decoder_backend.h:24-35`
- **What:** It always runs `sws_scale` into a second buffer, even when the
  source is already `yuv420p`, which is a full frame copy per frame for no
  reason. `thread_count` stays at the FFmpeg default of 1, so software
  decode is single-threaded. `DecodedFrame` has no SAR (anamorphic
  streams), color range, or matrix (BT.601 vs 709), so a renderer cannot
  draw colors correctly.
- **Fix:** Pass `decoded_` planes through when the format already matches.
  Set `thread_count` within a budget. Add the metadata fields during the A5
  redesign.

### B15. minor [HW] — Tile rectangles can have odd x and width
- **Where:** `src/layout_engine.cpp:12-30`
- **What:** 1280/3 gives x=853 and width=427. YUV420 chroma and Amlogic
  scalers (GE2D, VPP) commonly need 2-pixel or coarser alignment.
- **Fix:** Add an optional alignment parameter to `compute()` once the
  hardware constraints are known. The current math (partition, no overlap,
  ±1 px) is correct and well tested.

### B16. minor — Hidden streams are invisible in status
- **Where:** `src/layout_engine.cpp:83-90`, `src/mqtt_json.cpp:411-438`
- **What:** With `layout:"1x1"` and 4 streams, streams 2-4 get no tile, but
  status does not say which streams are on screen.
- **Fix:** Add `visible: bool` or a `slot` to each status stream entry.

### B17. minor (design question) — Stream table is not persisted
- **Where:** `src/stream_manager.*`, `src/mqtt_controller.cpp:321-324`
- **What:** Retained commands are deliberately ignored and the table lives
  only in memory, so every relaunch (and every Kodi round trip through the
  launch script) starts with an empty wall.
- **Fix:** Decide on persistence: a retained desired-state topic or a local
  config file. Not a code bug.

### B18. minor — Removing a stream reflows every later tile
- **Where:** `src/stream_manager.cpp:59`
- **What:** Slots come from insertion order, so removing slot 0 moves every
  other stream to a new cell. `[HW]` That means reconfiguring every
  decoder's output and scaler instead of one.
- **Fix:** Consider stable slot assignment.

### B19. nit — Docs are stale or inaccurate
- `docs/mqtt.md:45` says "Field names and command names are case-insensitive
  for the command". Only command names are case-insensitive; field names are
  case-sensitive (see B3).
- `docs/mqtt.md:133,137` lists "HLS demux" as not in this build and calls
  `FfmpegDecoder` an "unchanged stub". Both are out of date since #2.

### B20. nit — Miscellaneous
- `src/ffmpeg_decoder.cpp:73`: `av_log_set_level` is process-wide and runs on
  every `open()`. Move it to startup.
- `src/ffmpeg_decoder.cpp:38-43`, `:125-131`: `prepareAmlogicHwaccel` nulls a
  field that is always null, and the following `!= nullptr` check is dead
  code.
- `src/ffmpeg_decoder.cpp:89`: `AVFMT_FLAG_GENPTS` can read ahead and add
  latency on live streams. Confirm it is needed.
- `src/ffmpeg_decoder.cpp:77`: `user_agent` hardcodes `0.1`. Use
  `PROJECT_VERSION`.
- `tests/ffmpeg_decoder_test.cpp:45-49`: predictable `/tmp/…-<pid>` path
  followed by `remove_all` (symlink race on shared `/tmp`). Use `mkdtemp`.
- `.gitignore:8`: the unanchored `amlogic-multistream` pattern ignores any
  future file or directory with that name. Use `/amlogic-multistream`.
- `scripts/launch-coreelec.sh:14`: `sleep 1` as a display-release heuristic.
- `.github/workflows/ci.yml:21-23`: `cancel-in-progress` also cancels pushes
  to `main`, so intermediate commits get no artifacts.
- Logs print raw ids and urls, so newlines or ANSI escapes from MQTT can
  forge log lines.

---

## Checked and looks correct
- Layout math: integer splitting, exact cover, no overlap, ±1 px neighbors,
  override validation, negative and large counts. Tests are thorough.
- JSON parser: depth limit (32), trailing garbage, truncated input, and
  escape handling (apart from B4). Response and status escaping is correct.
- Retained command deliveries are filtered. The signal mask is set before
  `mosquitto_loop_start`, so the network thread inherits it. The offline
  retained status was delivered on SIGTERM in 15 of 15 local runs.
- FfmpegDecoder lifecycle: double `close()`, reopen after close, failed open
  leaving the object closed, mid-stream resolution change (320→640→320
  decoded correctly), and `yuvj420p` input. All ASan/UBSan clean.

## Verification performed
- `cmake` Debug with `-fsanitize=address,undefined -Wall -Wextra` on all
  targets: no warnings, 4/4 ctest pass.
- Local `mosquitto` 2.x and the ASan player: published the payloads in A9,
  A10, A11, B3, B4, and B5 and recorded the responses and retained status.
- `python3 -m http.server` serving a two-variant master playlist generated
  with `ffmpeg -g 10 -hls_time 1`; request log for A7.
- Corrupted TS (400 random bytes in the middle third) for A8. Black-hole
  host `http://10.255.255.1/` for A3 (15.2 s).
- `objdump -T` and `ldd` on a Release build from Ubuntu 24.04 for A2.
