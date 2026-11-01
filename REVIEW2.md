# Second review: `main` @ f1ea360 (after #6 and #7)

Unfiltered pass. An adversarial filter runs next, so treat every item as a
claim to be checked, not a task list. No production code was changed, and no
GitHub issues were filed. [SURVIVING.md](https://github.com/yyolk/amlogic-multistream/blob/cursor/review-notes-c6b9/SURVIVING.md)
(draft PR #5) was the input. It is referenced here as S1–S7. It was not
taken as correct.

Scope: what #6 (software renderer) and #7 (launch traps, aarch64 docs, MQTT
URL allowlist, FFmpeg protocol whitelist) closed, what they introduced, what
is still missing for a decode ↔ MQTT ↔ present multi-stream POC, and test/CI
gaps.

Severity: **blocker** = stops hardware bring-up or strands the box;
**major** = wrong behavior, or an interface the next milestone has to change;
**minor** = real but contained; **nit** = polish. `[HW]` = depends on
GS-King X / CoreELEC facts that were not checked on a device.
`[repro]` = reproduced here (see "How this was checked").

## Triage index

| Bucket | Items |
| --- | --- |
| Must fix before hardware | **T1** (AppImage SIGTERM orphans the player, then Kodi starts), **T2** (pre-fork Ctrl+C strands Kodi on dash/busybox), **T3** (AppImage needs `libX11`), **O1** (HLS variants, S3), **O2** (no device binary) |
| Multi-stream POC loop (not hardware-gated) | L1–L8 major, L9–L10 minor |
| Test / CI | C1–C3 major, C4–C6 minor |
| Minor / nits | T4, T5, N1–N9 |

---

## 1. Closed by #6 / #7

| Survivor | Status | Evidence |
| --- | --- | --- |
| **S1** SSH drop leaves Kodi stopped | **Closed for a bare ELF. Reopened for the AppImage** (T1) **and for Ctrl+C before the fork** (T2) | Bare ELF under dash: SIGTERM to the shell gives `MQTT shutting down on signal 15`, then `systemctl start kodi` once, with no process left. SIGHUP to the process group restarts Kodi once under bash, dash, and busybox ash. `[repro]` |
| **S2** wrong binary, no FUSE | **Docs and `APPIMAGE_EXTRACT_AND_RUN` closed. Device build still open** (O2). New load-time blocker (T3) | `ci.yml:4-7`, README §AppImage, and `build-appimage.sh:4-7` no longer call aarch64 the device build. `launch-coreelec.sh:48` exports `APPIMAGE_EXTRACT_AND_RUN=1`, and the runtime extracts under it. The glibc claim holds: the highest `GLIBC_*` version across the AppDir is `2.38`. `[repro]` |
| **S4** MQTT hands any libav URL to the decoder | **Closed** on the MQTT path, with residuals (T4, T5, N3) | `mqtt_commands.cpp:27-32` accepts only lowercase `http://` / `https://` and rejects NUL. `ffmpeg_decoder.cpp:75-93` rejects NUL and sets `protocol_whitelist`. An HTTP playlist whose segment is `file:///…` fails to open with `allow=0`. `[repro]` |
| **S5** renderer has nowhere to put a frame | **Closed (software half)** | `Renderer::present(const DecodedFrame&, const LayoutEngine::Rect&)` (`renderer.h:22-24`). `--url` calls it (`app.cpp:245`). Default `DrmRenderer::init` returns false (`drm_renderer.cpp:77-80`). With `-DAMLOGIC_DRM_KMS=ON`, `init` returns true after opening `card0` with no modeset, but `present` always fails closed (`:97-110`). It compiles on 24.04 libdrm. `[repro]` |
| B19 (dropped) docs | **Partly closed** | `docs/mqtt.md:45` now says field names are case-sensitive. `docs/mqtt.md:134` still lists "HLS demux" as not in this build (N9). |

**Still open:**

| Survivor | Status | Evidence |
| --- | --- | --- |
| **S3** HLS master fetches every variant | **Open** (O1) | No `AVDISCARD` in `src/`. A two-variant master over `http.server` still logs `GET /lo/index{0,1,2}.ts` **and** `GET /hi/index{0,1,2}.ts`, and decodes `stream=0 320x240`. `[repro]` |
| **S6** MQTT `start()` blocks; `playing_` is private | **Open, unchanged** (L1, L2) | `mqtt_controller.cpp:131` still `sigwait`s. `mqtt_controller.h:53-54` is unchanged. `App::run` still returns from `--url` before MQTT and never returns from MQTT (`app.cpp:274-301`). |
| **S7** CI green with decoder tests skipped | **Open, and wider** (C1) | It now also hides #6's `present_smoke` and #7's whitelist assertions. With no libav/mosquitto/drm dev packages, `run-tests.sh` exits 0 with `4/4` passed. `[repro]` |
| README armv7 / CoreELEC AppImage TODO | **Open** (O2). #7 added the TODO only | `README.md:148` |

---

## 2. Must fix before hardware

### T1. blocker — Under the AppImage, the SIGTERM trap kills the runtime, and the real player keeps running while Kodi starts `[repro]`

- **Where:** `scripts/launch-coreelec.sh:20-31` (`restore_kodi` kills `$player_pid`), `:54-55`, `:48`.
- **What:** With `APPIMAGE_EXTRACT_AND_RUN=1` (which #7 added), `$!` is the
  type-2 runtime. The runtime forks the extracted `AppRun`, which is a
  symlink to the ELF (`AppDir/AppRun -> usr/bin/amlogic-multistream`), and
  `waitpid`s it. `kill "$player_pid"` sends SIGTERM to the runtime only. The
  runtime does not forward it and dies. The player is orphaned and keeps
  running. One second later the script runs `systemctl start kodi`.
  - bash, dash, and busybox ash: SIGTERM to the shell leaves
    `/tmp/appimage_extracted_<hash>/AppRun` alive, and the fake `systemctl`
    logs `stop kodi` then `start kodi`.
  - The bare ELF in the same slot exits cleanly (`signal 15`), so the trap
    logic is correct and only the process tree differs.
  - SIGHUP to the whole process group (what a real SSH hangup delivers) is
    fine, because every process gets HUP directly. Ctrl+C is also fine,
    because the runtime inherits SIG_IGN for INT and waits.
- **Why:** Today the orphan stays on MQTT with the default client id. The
  next launch then starts the client-id takeover loop already shown in B5
  of REVIEW.md. Once any renderer touches the display, Kodi and the
  orphan fight over it. `README.md:152` says "a trap stops the player", which
  is not true for the artifact type the script is written for.
- **Fix direction:** Pick one:
  - (a) `--appimage-extract` once into `/storage/amlogic-multistream.AppDir`
    and run `AppDir/AppRun` directly, so `$!` is the player. This also
    removes the 158 MB tmpfs unpack (N6).
  - (b) Signal the children of the runtime (`pkill -TERM -P "$player_pid"`)
    before the runtime itself.
  - (c) Start the player in its own process group and `kill -TERM -- -$pgid`.
    This option also needs an INT trap, because Ctrl+C would no longer reach
    the player's group.

  In every option, replace `sleep 1` with a bounded `kill -0` poll (for
  example 10 s), then SIGKILL, so Kodi never starts while the player is
  alive. That matters more once L4 exists, because `open()` can block 15 s.

### T2. major [HW] — Ctrl+C before the player is forked leaves Kodi stopped on dash / busybox `[repro]`

- **Where:** `scripts/launch-coreelec.sh:39-45` (INT untrapped during
  `systemctl stop kodi` and `sleep 1`), `:56` (`trap '' INT` is set only
  after the fork).
- **What:** I sent SIGINT to the group 0.5 s after start, during `sleep 1`,
  with SIGINT at its default disposition (a real terminal):
  - bash: the shell dies with rc −2, and the EXIT trap still runs
    `start kodi`.
  - dash and busybox ash: the shell dies with rc −2, `stop kodi` runs, and
    **no `start kodi` follows**.

  `systemctl stop kodi` blocks until Kodi exits, so this window is
  "the user changed their mind" time, not microseconds. #7 tested bash and
  dash in a harness. Busybox was not covered. LibreELEC/CoreELEC `/bin/sh`
  is believed to be busybox ash `[HW]`.
- **Why:** This is the same stranded-Kodi outcome S1 was about, reached by
  the keypress S1 said not to trap. S1's reasoning (do not race the
  player's shutdown) applies only after the fork.
- **Fix direction:** `trap 'restore_kodi; exit 130' INT` before
  `systemctl stop kodi`. Keep the existing `trap '' INT` after the fork.
  Verify `readlink -f /bin/sh` on the box.

### T3. blocker [HW] — The AppImage cannot load without `libX11` (and friends), which linuxdeploy excludes and CoreELEC does not ship `[repro]`

- **Where:** `scripts/build-appimage.sh:112-117` (linuxdeploy defaults),
  README `:144-146` (says the only mismatch is the 32-bit userspace).
- **What:** Ubuntu's `libavutil.so.58` has `NEEDED libX11.so.6`. So do the
  bundled `libva-x11`, `libvdpau`, and `libcairo`. linuxdeploy's exclude list
  drops `libX11`, `libxcb`, `libfontconfig`, `libfreetype`, `libharfbuzz`,
  `libfribidi`, `libgmp`, `libz`, `libdrm`, `libstdc++`, and `libgcc_s`
  (CI log: `Skipping deployment of blacklisted library …`). In a chroot with
  only glibc, libstdc++, libgcc_s, zlib, libdrm, and the AppDir:
  `error while loading shared libraries: libX11.so.6`. Adding
  `libX11`/`libxcb` gives `libfontconfig.so.1`. Adding the whole excluded
  set gives `amlogic-multistream starting...`.
- **Why:** CoreELEC Amlogic-ng has no X11 `[HW, verify: ls /usr/lib/libX11*]`.
  Fixing the architecture (O2) with the same packaging recipe and a distro
  FFmpeg (Ubuntu/Debian armhf) fails here too, before `main`. S2 treated the
  137-library size as not a blocker. The size is not the blocker; this
  dependency closure is.
- **Fix direction:** For the device build:
  - Link against the CoreELEC sysroot's own FFmpeg, or build FFmpeg with
    `--disable-xlib --disable-vaapi --disable-vdpau --disable-opencl` and
    only the hls/mpegts/mov demuxers and h264/hevc decoders. That also
    shrinks N6 and the parser surface behind T5.
  - Add the chroot smoke from C3 so this cannot regress silently.

### O1. major — HLS master still downloads every variant (S3, unchanged) `[repro]`

- **Where:** `src/ffmpeg_decoder.cpp:112` (`av_find_best_stream`). No
  `discard` is set anywhere.
- **What:** As S3 describes. Re-run on f1ea360 against a two-variant master:
  both variants' segments are fetched, and the 320x240 variant is the one
  decoded.
- **Fix direction:** As in S3: after `avformat_find_stream_info`, set
  `AVDISCARD_ALL` on every stream except the chosen video, and choose by
  `variant_bitrate`. Ship it with the two-variant HTTP fixture from C5.

### O2. major [HW] — No device AppImage exists, and the README's reason may be imprecise

- **Where:** `README.md:144-148`, `ci.yml:4-7`.
- **What:** The armv7 TODO is open, so there is still no binary for the
  documented target. The README also says "That kernel will not load an
  aarch64 userspace binary." I believe CoreELEC Amlogic-ng runs an
  **arm64 kernel with a 32-bit userspace**. If so, the kernel can exec
  aarch64. What is missing is `ld-linux-aarch64.so.1` and an aarch64 libc.
  Not verified.
- **Why:** If the kernel is arm64, a second route exists besides the armv7
  sysroot TODO: an aarch64 AppImage that bundles its own loader and glibc,
  or a static build. The choice changes which CI job to build next.
- **Fix direction:** On the box, compare `uname -m` with
  `file /usr/bin/kodi.bin` (or `/bin/busybox`), then pick armv7-sysroot or
  self-contained aarch64. T3 applies to both routes.

---

## 3. Decode ↔ MQTT ↔ present loop: what is still missing

None of these are bugs in shipped behavior. They are the pieces the
multi-stream POC still needs. The list is ordered as the smallest cut.

### L1. major — `App` has no third mode (S6, open)

- **Where:** `src/app.cpp:274-301`, `src/mqtt_controller.cpp:131-143`.
- **What:** `--url` is single-stream and never starts MQTT. MQTT mode sits
  in `sigwait` inside `MqttController::start()` until a signal, so nothing
  else can run on the main thread.
- **Fix direction:** Split `start()` into a non-blocking `start()` and a
  separate `stop()`. Let `App` own the signal mask, a `signalfd`/`sigwait`
  thread, and the present loop.

### L2. major — `playing` lives on the controller (S6, open)

- **Where:** `src/mqtt_controller.h:53-54`. `executeCommand(StreamManager&,
  bool& playing, …)` is in `mqtt_commands.cpp:74`.
- **Fix direction:** Move the flag into `StreamManager` under `mu_`.

### L3. major — `restart` is log-only, so no loop can act on it

- **Where:** `src/stream_manager.cpp:21-25`, `:80-97`.
- **What:** `restartStream` and `restartAll` print a line and return true.
  `StreamInfo` has no generation or flag, so MQTT replies
  `restart requested` and nothing can ever consume it. There is also no
  table version for a loop to diff `list()` against.
- **Fix direction:** Add a per-entry `uint64_t generation`, bumped on add
  and restart, plus a table `version()`. The loop then reopens the entries
  whose generation changed.

### L4. major — The decoder blocks, has no interrupt, and is not thread-safe

- **Where:** `src/ffmpeg_decoder.cpp:97` (`avformat_open_input`), `:107`,
  `:307`. `rw_timeout` is 15 s (`:19`). No `interrupt_callback`.
  `decoder_backend.h:38` says "Not thread-safe".
- **What:** With nine streams, one thread per stream is the simple shape.
  `remove`, `stop`, and shutdown then need to break a blocked `open` or
  `read`. S6 deferred this "until the loop exists". That time is now.
  T1's restore step also depends on bounded shutdown.
- **Fix direction:** An atomic abort flag read by
  `AVFormatContext::interrupt_callback`, set from the owner thread, and the
  only cross-thread call that is allowed.

### L5. major — `Renderer` has no frame boundary; software `present()` costs a full viewport per stream `[repro]`

- **Where:** `src/renderer.h:12-15` ("One present() draws one stream"),
  `src/software_renderer.cpp:214-233`.
- **What:** Each `present()` copies the entire RGB viewport
  (`next = pixels_`, 6.2 MB at 1080p) and, with `--render-dir`, writes a
  whole-viewport PPM. A nine-tile wall is therefore nine copies and nine
  partial-composite PPMs per wall frame. The bench measured 3.2 ms per
  present and **28.9 ms per 9-tile 1920x1080 wall** on this Xeon at `-O2`,
  before any decode. A73 cores will be several times slower.
- **Fix direction:** Add `beginFrame()` / `endFrame()` (or
  `presentTile` + `commit`). Copy-on-failure only the tile rect, and write
  one PPM per `endFrame`. The interface change matters more than the speed:
  the DRM path needs a commit point too.

### L6. major — The viewport comes from the first decoded frame

- **Where:** `src/app.cpp:227-233`
  (`renderer->init(frame.width, frame.height)` and `compute(frame.width, …, 1)`).
- **Fix direction:** A multi-stream wall needs a fixed output size
  (`--viewport WxH` now, display mode later) chosen before any stream opens.

### L7. major — Bad layout tokens still blank the wall while reporting `ok:true`

- **Where:** `src/stream_manager.cpp:99-107`, `:14-19`.
  `LayoutEngine::compute` rejects anything `parseLayout` rejects, including
  the literal `"auto"`.
- **What:** SURVIVING dropped A10 with "validate in the renderer PR". #6
  was that PR and did not validate. `{"layout":"banana"}` is still stored,
  and the first `compute()` will fail. A loop that passes
  `layoutSetting()` (`"auto"`) instead of `layout()` also fails.
- **Fix direction:** In `setLayout`, accept `auto` or a token for which
  `parseLayout` returns true, and reply `ok:false` otherwise. Document that
  the loop must pass `layout()`.

### L8. major — Frame lifetime does not cross threads

- **Where:** `src/decoder_backend.h:10-13`. Planes are valid only until the
  next `readFrame()`.
- **What:** With decode threads and a present thread, the present thread
  cannot read the decoder's planes.
- **Fix direction:** A per-stream single-slot mailbox. Copy the YUV into it
  (the decoder already does a `sws_scale` copy), or hold an `AVFrame` ref
  behind the existing interface. No DMA-BUF needed for the POC.

### L9. minor — No stream cap where decoders would be created

- **Where:** `src/stream_manager.cpp:29-49`.
- **What:** SURVIVING deferred the cap with "Cap when a decoder is created
  per entry." That becomes the loop PR.
- **Fix direction:** Cap at `LayoutEngine::kMaxStreams` and reply
  `ok:false`.

### L10. minor — No pacing or live reconnect

- **Where:** `app.cpp:207-261`.
- **What:** Frames are presented as fast as they decode. A live HLS EOF is
  simply the end of the stream.
- **Fix direction:** A pts or wall-clock pacer per tile, and reopen-with-
  backoff on EOF or error. This is also where the `readFrame` EOF-versus-
  error split (A4, dropped) starts to matter.

---

## 4. New risks from #6 / #7 (beyond T1–T3)

### T4. minor — The CLI `--url` local-file allowance also applies to remote playlists `[repro]`

- **Where:** `src/app.cpp:179-181`, `src/ffmpeg_decoder.cpp:90-93`.
- **What:** `setAllowLocalFile(true)` adds `file` to the whitelist for every
  `--url`, including `http://…`. With `allow=1`, a remote playlist whose
  segment is `file:///tmp/hlsmv/lo/index0.ts` opened and decoded a frame.
  FFmpeg's HLS extension filter allows `.ts`, `.mp4`, and similar.
- **Why:** This is operator-only today. It becomes a problem if `--url` is
  ever driven from automation, or if one decoder object is reused across
  paths, since `close()` does not reset `allow_local_file_`.
- **Fix direction:** Enable `file` only when the URL itself has no scheme or
  is `file:`. Reset the flag in `close()`, or take it as an `open()`
  argument.

### T5. minor (security) — `https://` is accepted but not authenticated, and every demuxer stays enabled

- **Where:** `src/ffmpeg_decoder.cpp:85-93`.
- **What:** FFmpeg `tls_verify` defaults to 0 (`ffmpeg -h protocol=tls`), and
  no `ca_file` is set. An `https` stream is therefore not verified. No
  `format_whitelist` is set, so any HTTP response is probed against every
  demuxer, running as root on CoreELEC. SSRF to LAN hosts through `http(s)`
  and redirects remains by design.
- **Fix direction:** Set `tls_verify=1` with a CA bundle path once the
  device has one. Set `format_whitelist=hls,mpegts,mov,mp4` (and an
  equivalent codec limit) alongside the T3 FFmpeg slim-down.

### N1. nit — The whitelist comment is misleading

- **Where:** `ffmpeg_decoder.cpp:88-89`.
- **What:** "hls is the protocol name libavformat registers": the HLS
  *demuxer* does not need the `hls` protocol. That entry only enables the
  legacy `hls+http:` protocol. Harmless.

### N2. nit — The launch script always exits 0

- **Where:** `launch-coreelec.sh:57`.
- **What:** `wait … || true` drops the player's exit status, so a crash on
  start looks like success to whoever ran the script.
- **Fix direction:** Keep the status and `exit` with it after
  `restore_kodi`.

### N3. nit — Uppercase schemes are rejected

- **What:** `HTTP://` and `HTTPS://` are rejected (fail-closed, and
  documented). A Home Assistant template that upper-cases the URL gets
  `ok:false`. Consider lowercasing the scheme only.

### N4. nit — SoftwareRenderer always uses BT.601

- **Where:** `software_renderer.cpp:145-153`.
- **What:** The BT.601 matrix is used for HD content as well. The PPM is
  test output, so this is mentioned only so nobody uses PPM checksums as a
  color reference.

### N5. nit — The pre-fork window has no Kodi-stop result check

- **Where:** `launch-coreelec.sh:42`.
- **What:** If `systemctl stop kodi` fails, the player starts on top of Kodi.

---

## 5. Test / CI gaps

### C1. major — Silent skip now hides #6 and #7 coverage (S7, wider) `[repro]`

- **Where:** `scripts/run-tests.sh:47-57`, `CMakeLists.txt:71-93`, `:100-149`.
- **What:** Missing FFmpeg dev packages drop `ffmpeg_decoder`. Missing any of
  FFmpeg, mosquitto, or **libdrm** drops the player and `present_smoke`.
  The libdrm requirement comes from `AMLOGIC_PLAYER_DEPS_FOUND`, even though
  the default build never calls libdrm. A missing `ffmpeg` CLI exits 77,
  which ctest counts as skipped. On this VM before installing packages:
  `Total Tests: 4`, `100% tests passed`, exit 0.
- **Fix direction:**
  - In CI, require `ffmpeg_decoder` and `present_smoke` to be registered
    (`ctest -N`) and not skipped.
  - Make the libx264 miss a failure in CI (`ffmpeg_decoder_test.cpp:243`).
  - Drop the libdrm requirement unless `AMLOGIC_DRM_KMS=ON`.

### C2. major — `launch-coreelec.sh` has no automated check

- **What:** T1 and T2 were found with a stand-in: a fake `systemctl` on
  PATH, the real AppImage at the binary path, a local mosquitto, and
  TERM/HUP/INT under bash, dash, and busybox ash. That runs on the x86_64
  runner in seconds.
- **Fix direction:** Add a CI step after "Build AppImage" that runs the
  stand-in and asserts three things: one `start kodi`, no surviving
  `AppRun`, and `start kodi` after a pre-fork INT. Also run
  `shellcheck scripts/*.sh`, which is clean today.

### C3. major — The AppImage smoke runs on a host that has every excluded library

- **Where:** `build-appimage.sh:122-123`.
- **What:** `--help` on the build runner cannot see T3.
- **Fix direction:** Add a chroot or `--library-path` smoke that offers
  only an allowlist of libraries the target ships (glibc, libstdc++,
  libgcc_s, zlib, libdrm), with `/proc` mounted for `$ORIGIN`. It fails
  today with `libX11.so.6`.

### C4. minor — `-DAMLOGIC_DRM_KMS=ON` is never compiled

- **Where:** `CMakeLists.txt:98`.
- **What:** It compiles on 24.04 libdrm today `[repro]`, but nothing keeps
  it compiling.
- **Fix direction:** A configure-and-build-only step. No device needed.

### C5. minor — The whitelist test proves "open failed", not why

- **Where:** `tests/ffmpeg_decoder_test.cpp:146-148`.
- **What:** `CHECK(!decoder.open(url))` passes for any failure. No test
  covers the real threat: an HTTP playlist pointing at `file:`.
- **Fix direction:** A local HTTP fixture (python `http.server` or an
  in-test socket) serving a playlist with a `file:` segment (expect failure
  at `allow=0`) and a two-variant master (expect one variant's GETs once O1
  lands).

### C6. minor — `MqttController` still has zero coverage

- **What:** A13 was dropped while `start()` was a leaf. L1 splits it, so a
  broker-backed ctest becomes the regression net for that refactor.

---

## 6. Other nits

- **N6 minor [HW]:** `APPIMAGE_EXTRACT_AND_RUN` leaves
  `/tmp/appimage_extracted_<hash>` behind, **158 MB** for the x86_64 build,
  even after `--help`. On CoreELEC `/tmp` is tmpfs (RAM), and each new
  build hash adds another tree. T1 option (a) removes this.
- **N7:** `restore_kodi` (`launch-coreelec.sh:25-29`) sleeps 1 s instead of
  confirming the exit. This is folded into the T1 fix direction.
- **N8:** `decodeUrl` logs the full URL (`app.cpp:177`, `ffmpeg_decoder.cpp:163`).
  Tokens in query strings go to stdout (B8 was dropped; unchanged).
- **N9:** `docs/mqtt.md:134` still lists "HLS demux" under "Not in this build".

---

## 7. Checked and fine

- The #6 software renderer: dest bounds and stride checks, letterbox math,
  odd chroma sizes, 8192 extent cap, PPM write failure rollback, and
  `shutdown()` idempotence. `software_renderer_tests` passes.
- `--renderer drm` fails closed with and without `AMLOGIC_DRM_KMS`.
  `present_smoke` asserts it.
- The #7 MQTT allowlist tests cover `file`, `pipe`, `tcp`, `concat`,
  uppercase, NUL, and accepted `https` (`tests/mqtt_smoke_test.cpp:200-235`).
- The FFmpeg whitelist propagates to HLS child playlists and segments
  (`allow=0` refuses a `file:` segment).
- glibc floor: the highest `GLIBC_*` version across the 24.04 AppDir is
  `2.38`.
- `shellcheck scripts/*.sh` is clean.
- CI on f1ea360: both jobs green, and the x86_64 log shows
  `ffmpeg_decoder_tests ok (mp4, hls)` and `present_smoke_test ok`.

---

## How this was checked

Ubuntu 24.04 VM, FFmpeg 6.1 (libavformat 60.16), mosquitto 2.0.18, and
libdrm 2.4.125. Nothing here ran on a GS-King X.

- **C1:** `./scripts/run-tests.sh` before installing the libav, mosquitto,
  and drm dev packages gave 4/4 and exit 0. After installing them it gave
  6/6. `-DAMLOGIC_DRM_KMS=ON` built the player.
- **O1:** two libx264 HLS variants (320x240 and 640x480, `-g 10 -hls_time 1`)
  under a master playlist, served by `python3 -m http.server`, then
  `--url http://127.0.0.1:8765/master.m3u8 --frames 3`. I read the GET
  lines from the server log.
- **T4 and S4:** a small harness linking `src/ffmpeg_decoder.cpp`, with
  `setAllowLocalFile(0|1)`, opening an HTTP playlist whose only segment is
  `file:///…/index0.ts`.
- **T1 and T2:** `scripts/build-appimage.sh` on x86_64 (type2-runtime). I ran
  a copy of `launch-coreelec.sh` with `/storage/amlogic-multistream`
  replaced by the AppImage (and by the bare ELF as a control), with a
  logging `systemctl` stub on PATH and a local mosquitto.
  - Signals: TERM to the shell, HUP to the group, and INT to the group, at
    0.5 s (pre-fork) and 3 s (player running).
  - Shells: bash, dash, and `busybox sh`.
  - The T2 runs used a Python launcher that resets SIGINT to SIG_DFL. A
    background-job launcher inherits SIG_IGN for INT, which a
    non-interactive shell cannot un-ignore, and that hides T2.
- **T3:** a chroot holding the AppDir plus only `libc`, `libm`, `libresolv`,
  `ld-linux`, `libstdc++`, `libgcc_s`, `libz`, and `libdrm` from the host,
  with `/proc` mounted, then `/app/AppRun --help`. I added libraries back
  in steps to find each missing one.
- **L5:** a `-O2` bench of `SoftwareRenderer::present` with a 640x360 YUV
  source into the nine `LayoutEngine::compute(1920,1080,9)` tiles, 20
  rounds.
