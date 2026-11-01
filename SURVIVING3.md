# Survivors: `main` @ 32b7d0d

Filtered from [REVIEW3.md](REVIEW3.md), which is the unfiltered pass over
`49b4c8f`. This file is the list to act on. No production code was changed,
and no GitHub issues were filed.

`origin/main` is now `32b7d0d`: `49b4c8f` plus the Kodi launcher add-on
(#12). #12 did not touch `src/` or `scripts/launch-coreelec.sh`, so
REVIEW3's line numbers still hold. Where the add-on changes how a finding
plays out, that is noted. The add-on starts the launch script as a
`systemd-run` unit with `KillMode=mixed`, refuses to launch twice, does not
pass `PLAYER_EXTRACT_DIR`, and sends player output to the journal.

Re-checked on Ubuntu 24.04, g++ 13.3, FFmpeg 6.1 (libavformat 60.16),
mosquitto 2.0.18, dash, bash, and busybox ash 1.36.1. The player was built
from `32b7d0d`, and `AMS_REQUIRE_DECODER_TESTS=1 ctest` gives 11/11. CI run
41271964070 on `32b7d0d` is green. Nothing here ran on a GS-King X.

Severity follows REVIEW3. **blocker** = defeats a project goal on a
shipped path. **major** = wrong behavior on a shipped path, or a gap in a
stated goal. **minor** = real but contained.

---

## Survivors

### 1. blocker — One slow or dead stream, or a healthy live HLS stream at its live edge, freezes every tile and delays SIGTERM (REVIEW3 B1, with P1's live case folded in)

- **Where:** `src/stream_player.cpp:223-253` opens and reads every stream
  in turn on the main thread. `src/mqtt_controller.cpp:199-205` checks for
  signals only after `tick()` returns. `src/ffmpeg_decoder.cpp:85-97`:
  the only bound is `rw_timeout` (15 s per I/O operation), and no
  `interrupt_callback` is set.
- **Evidence (re-run on `32b7d0d`):** The real binary, a local broker,
  `--viewport 64x36 --render-dir`, and a healthy 10 fps MPEG-TS clip served
  by `http.server`. Presents were counted as PPM files.

  | Added next to the healthy stream | Presents in 10 s | SIGTERM → exit |
  | --- | --- | --- |
  | nothing | 439 | 0.06 s |
  | 1 URL that accepts and never answers | 1 | 5.2 s |
  | 3 such URLs | 1 | 35.4 s |
  | 1 URL that sends headers, then 1 byte every 5 s | 1 | still alive at 45 s (`wchan=do_poll`), then SIGKILLed |

  **New case, no faulty stream at all:** one healthy VOD stream next to one
  healthy live HLS stream (`ffmpeg -re`, 2 s segments). Over 20 s, the gaps
  between PPM writes were about 1.0 s every 2 s, at t = 5.1, 7.1, 9.2,
  11.2, 15.3, and 17.3 s. The VOD tile froze too. With no pacing, the loop
  reads to the live edge, then waits inside `av_read_frame` for the next
  segment, and every tile waits with it. Multi-stream live HLS is the
  project's core case, so this is reachable with only working cameras.
- **Add-on path:** `systemctl stop amlogic-multistream-launch` → the
  script TERMs the player → the script's 10 s poll → SIGKILL. Kodi returns
  after about 11 s, and survivor 3's losses apply.
- **Fix:**
  1. Set an `AVIOInterruptCB` on each `FfmpegDecoder`. It checks an atomic
     abort flag and a deadline for each open and read. This bounds the
     trickle case and shutdown. It is also the one cross-thread call that
     REVIEW3 I2 asked `DecoderBackend` to expose (`abort()`).
  2. Cap `probesize` and `analyzeduration` for the open.
  3. Decode each stream on its own thread with a one-frame slot per tile,
     so the main thread only composites and handles signals. This is what
     removes the live-edge stall. `stop`, `remove`, and shutdown set the
     abort flag.
  4. Add a test that adds a blackhole and a trickle URL to the real binary,
     sends `start`, then SIGTERM, and asserts exit in well under the launch
     script's 10 s. Add the live-edge case if a cheap fixture exists.

  PTS-based pacing (the rest of REVIEW3 P1) is not needed for live streams
  once each one blocks on its own thread. Revisit it for VOD when a display
  backend exists.

### 2. major — No device build for either CoreELEC target (REVIEW3 CI1, reframed)

- **Where:** `.github/workflows/ci.yml` builds Ubuntu x86_64 and aarch64
  AppImages. README "AppImage" TODO. `scripts/install-kodi-addon.sh:93-107`
  refuses any player that is not 32-bit ARM unless `--allow-non-arm32` is
  passed.
- **What:** The target is now two device AppImages: CoreELEC 21 armv7
  (Amlogic-ng, the current GS-King X image) and CoreELEC 22 aarch64.
  Neither is built. A green run says nothing about 32-bit `long` and
  `size_t`. The Ubuntu aarch64 artifact is not a CE22 device image either,
  because it ships Ubuntu's X11/VAAPI-linked FFmpeg stack (SURVIVING2 #4).
  REVIEW3's "the aarch64 job duplicates x86_64" no longer holds, since
  aarch64 is now a device target. Its "add an armv7 job" is a first step,
  not the packaging answer.
- **Evidence:** The five dependency-free tests (`layout_engine`,
  `stream_manager_test`, `mqtt_smoke_test`, `software_renderer`,
  `stream_player_test`) build with `arm-linux-gnueabihf-g++ -march=armv7-a
  -mfpu=neon -mfloat-abi=hard` in 11 s and all pass under `qemu-arm`.
- **Fix:**
  - Cheap first step: an armhf cross plus `qemu-arm` job for those five
    tests.
  - Deliverable: device AppImages for CE21 armv7 and CE22 aarch64, each
    linked against that CoreELEC sysroot's FFmpeg, or an FFmpeg built
    without X11, VAAPI, VDPAU, and OpenCL.
  - When the CE22 image exists, make the `install-kodi-addon.sh` guard and
    the "GS-King X is 32-bit" wording in README and `docs/kodi-addon.md`
    accept aarch64 for CE22.

### 3. minor — An unclean exit loses the final retained status and buffered stdout (REVIEW3 L2 + L7, downgraded from major)

- **Where:** `launch-coreelec.sh:95-105` (10 s, then SIGKILL).
  `mqtt_controller.cpp:173-177` blocks only SIGINT and SIGTERM, so SIGHUP
  takes the default action.
- **Evidence:** The real binary under the real script (dash), with one
  healthy and one trickle stream. A single TERM to the script reached
  `start kodi` 11.1 s later through the KILL path. The retained status was
  the will, `{"online":false,…,"streams":[]}`, and stdout was 0 bytes.
  `kill -HUP` on the player directly gave exit 129, no shutdown log, and the
  same empty-table will.

  Correction to REVIEW3: not every log line is lost. `std::cerr` is tied to
  `std::cout`, so any stderr write flushes stdout. What is lost is
  everything since the last stderr write. In the trickle case nothing was
  ever written to stderr, so that was everything. In the add-on path this
  output is the journal, so `journalctl -u amlogic-multistream-launch` has
  the same gap.
- **Why minor:** Fixing survivor 1 removes the routine KILL. What remains
  is two one-line fixes.
- **Fix:** Add SIGHUP to the `sigtimedwait` set, and make stdout
  line-buffered at startup.

### 4. minor — A second HUP/TERM during Kodi restore exits without starting Kodi (REVIEW3 L1, downgraded from major)

- **Where:** `launch-coreelec.sh:110-117` sets `restored=1` before
  `stop_player`. The trap at `:126` is `restore_kodi; exit 0`.
- **Evidence:** A stand-in player that ignores TERM and HUP. HUP then TERM
  2 s later, under dash, bash, and busybox, and TERM then TERM under dash:
  each time the script exited 0, `systemctl` logged only `stop kodi`, and
  the player was still alive. A single TERM gave `stop kodi` then
  `start kodi`. With the **real player** stuck on a trickle stream (survivor
  1), two TERMs 2 s apart gave the same result: no `start kodi`, and the
  player still running.
- **Why minor:** It needs two signals inside the stop window. The add-on's
  `systemctl stop` sends one SIGTERM, and a repeated stop on a unit that is
  already deactivating does not send another (systemd semantics, read but
  not run here). That leaves a manual `kill` repeated over SSH. Fixing
  survivor 1 shrinks the window to well under a second.
- **Fix:** Make the first line of `restore_kodi` `trap '' HUP TERM INT`. Add
  a stand-in test with a TERM-ignoring payload and two signals that asserts
  exactly one `start kodi`.

### 5. minor — `PLAYER_EXTRACT_DIR` is `rm -rf`'d without a guard (REVIEW3 L3, downgraded from major)

- **Where:** `launch-coreelec.sh:137-152`.
- **Evidence:** A type-2 stand-in at `storage/amlogic-multistream`, with
  `storage/userdata.xml` and `storage/.kodi/guisettings.xml` beside it, run
  with `PLAYER_EXTRACT_DIR=$PWD/storage`. The directory was emptied,
  AppImage included. Extraction then failed (`env: …: No such file or
  directory`), and the script exited 1.
- **Why minor:** The value is an operator-set environment variable on the
  SSH path only. The add-on does not expose or pass it, so it extracts to
  `$PLAYER_BIN.AppDir`. The damage when it is misused is total, though, and
  the guard is small.
- **Fix:** Remove only `$extract_root/squashfs-root` and the stamp, never
  `$extract_root` itself. Add a stand-in test with a sentinel file.

### 6. minor — MQTT `add` reaches every FFmpeg demuxer and decoder (REVIEW3 S1, downgraded from major)

- **Where:** `ffmpeg_decoder.cpp:85-93` sets `protocol_whitelist` (and
  excludes `file` on the MQTT path) but no `format_whitelist` or
  `codec_whitelist`.
- **Evidence:** An MQTT `add` of an HTTP GIF, then `start`, logged
  `FfmpegDecoder::open … codec=gif` and presented a frame. SURVIVING2
  dropped T5 because MQTT never opened a decoder. #10 changed that.
- **Why minor:** The trust boundary is the broker, and any broker client
  can already freeze the wall (survivor 1). Kodi on the same box already
  runs FFmpeg's parsers as root on whatever media it is pointed at. This
  path only adds a way to trigger them without a person.
- **Fix:** A `format_whitelist` for HLS and its segment demuxers, and a
  `codec_whitelist` for the codecs the player intends to support. Prove
  them against the existing HLS and MPEG-TS fixtures, since `hls` passes
  its whitelists to segment demuxers. In `docs/mqtt.md`, say that broker
  authentication is expected beyond a lab LAN.

### 7. minor — Nothing warns that launching shows nothing on HDMI, and since #12 that is one click from the Kodi UI (REVIEW3 G1, downgraded from major)

- **Where:** `mqtt_controller.cpp:122-130`: `SoftwareRenderer` has no
  output. The add-on's Launch entry, `docs/kodi-addon.md`, and README
  "Launch on CoreELEC" and "Kodi plugin install" do not say the screen
  will be blank.
- **What:** Launch from the remote stops Kodi, and nothing comes back on
  screen. Getting back needs SSH or Home Assistant (`systemctl stop
  amlogic-multistream-launch`), which `docs/kodi-addon.md` "Returning to
  Kodi" does document.
- **Fix:** One sentence in the Launch label or its notification, and in
  both README sections. What HDMI actually shows needs a device.

### 8. minor — The player exits 1 if the broker is unreachable at startup, contradicting "reconnects with backoff" (REVIEW3 P3)

- **Where:** `mqtt_controller.cpp:179-184`. `docs/mqtt.md:22`.
- **Evidence:** Connection refused gave `MQTT connect failed: Connection
  refused` and exit 1. `no-such-host.invalid` gave `Lookup error.` and
  exit 1. Through the add-on, that is Kodi stopping and starting again
  with nothing explained on screen. The error is on stderr, so it reaches
  the journal.
- **Fix:** Start the loop anyway and let libmosquitto retry. Failing that,
  correct the doc to say backoff applies only after the first connect.

### 9. minor (latent) — `signal_tree` overwrites the global `root`, so the root and later siblings are not signaled (REVIEW3 L4)

- **Where:** `launch-coreelec.sh:72-87`. Only `dir pid pp` are `local`.
- **Evidence:** A root with children C1 (which has child G) and C2, then
  `signal_tree P TERM`, under dash, bash, and busybox. The root and one
  child survived, and `$root` ended up as the deepest PID.
- **Why latent:** Today the player forks nothing, and linuxdeploy's
  `AppRun` is the ELF. The bug fires in exactly the forking-`AppRun` case
  that the function's comment (`:70-71`) describes.
- **Fix:** `local root sig` (or no globals). Add a stand-in whose `AppRun`
  forks two children.

---

## Already fixed since REVIEW2

REVIEW3 section 1 holds on `32b7d0d`: the AppImage SIGTERM orphan, early
Ctrl+C, HLS variant discard, the libX11 closure plus minimal-chroot smoke,
the tick loop, `restart` generation, `--viewport`, layout validation, the
9-stream cap, the required decoder tests, and launch-script CI. Local
CTest is 11/11, including `launch_coreelec`, `ffmpeg_decoder`, and the new
`kodi_addon`. CI 41271964070 logs `Required decoder tests passed` and the
chroot `--help` smoke. None of these is re-raised.

---

## Needs a device (GS-King X / CoreELEC)

Not blockers. Do them when the box is on the bench.

- **Survivor 7:** What HDMI shows after `systemctl stop kodi` with the
  player running: black, the console, or a frozen Kodi frame.
- **Survivor 4:** That CoreELEC's systemd sends one SIGTERM for
  `systemctl stop amlogic-multistream-launch`, including when it is
  repeated, and what happens to a surviving player when the script exits.
- **Survivor 2:** CE22 aarch64: `uname -m`, `file /bin/busybox`, and the
  glibc version on the CE22 image, before its sysroot is chosen.
- **Software decode cost (dropped P2):** Only if software decode stays on
  the device path: 4- and 9-tile CPU on A73/A53 at 1280x720.

---

## Dropped

- **P1 pacing (standalone)** — 4.4× real time on VOD is reproduced (439 presents in 10 s at 10 fps). The live consequence is survivor 1. VOD pacing waits for a display backend.
- **P2 one-core decode and composite** — `thread_count` 1 and the full-viewport copy per `present()` are real (code). It is a software stand-in with no output, the 13.6 ms figure was not remeasured, and device cost needs a GS-King X.
- **P4 shutdown publish race** — microsecond window, not reproduced.
- **P5 SIGPIPE through `| head`** — contrived. The add-on writes to the journal, and an SSH drop delivers HUP first.
- **P6 failed subscribe not retried** — no failing subscribe in hand (dropped as REVIEW B6).
- **P7 a failed tile keeps its last frame** — nothing is displayed yet.
- **P8–P10 stop/start in one tick, one extra present, `restart` bypasses backoff** — nits. Any broker client can do worse (survivor 1).
- **L5 size+mtime stamp** — needs a same-size rebuild copied with preserved mtime. Speculative.
- **L6 no single-instance lock** — the add-on refuses an active unit (`amlogic_launcher.py:70-71`). Two concurrent SSH launches are speculative.
- **L8 exit 0 on signal** — cosmetic (dropped as REVIEW2 N2).
- **L9 `sleep 1` after SIGKILL** — "a future DRM ioctl or NFS" is speculative.
- **S1 TLS-verify and root sub-claims** — no device CA bundle, LAN cameras are often self-signed, and CoreELEC runs Kodi as root anyway.
- **S2 `max_pixels`** — 32-bit OOM is device guesswork, not reproduced.
- **S3 SSRF** — by design for a URL-taking player. Accepted before.
- **S4 credentials in retained status and argv** — Home Assistant sent the URL. No change since REVIEW B8 was dropped.
- **S5 unpinned AppImage tools** — supply-chain hygiene. No change since REVIEW B12 was dropped.
- **Log injection** — dropped as part of REVIEW B20.
- **C1 `AVERROR_INVALIDDATA` reopen** — REVIEW3 itself did not reproduce it (FFmpeg 6.1 concealed every corruption).
- **C2 `--render-dir` PPM flood** — CLI-only debug flag.
- **C3 surrogate pairs and raw UTF-8** — not on the POC path (dropped as REVIEW B4).
- **C4 numeric id accepted as a string** — nit.
- **C5 unknown MQTT-mode flags ignored** — the add-on passes environment variables, not flags, and the log shows the host (dropped as REVIEW B7).
- **C6 shared default client id** — one box, and `MQTT_CLIENT_ID` exists (dropped as REVIEW B5).
- **C7 id/url length** — bounded by the 64 KiB payload cap and the 9-stream cap.
- **C8 `App::run` print, user agent 0.1, repeated `av_log_set_level`** — nits.
- **I1 `StreamPlayer` takes `SoftwareRenderer&`** — change the interface in the PR that adds DRM. Nothing is broken now.
- **I2 `DecoderBackend` contract** — `abort()` is part of survivor 1. DMA-BUF handles and enum read results wait for the hardware-decode work.
- **I3 status schema version and correlation id** — no consumer depends on them yet.
- **I4 torn snapshots** — harmless, as REVIEW3 says.
- **CI2 / CI3 idle-player launch test, no broker-backed test** — folded into the tests for survivors 1 and 4.
- **CI4 no sanitizer job** — the TSan run was clean, and there is no regression to guard.
- **CI5 `AMLOGIC_DRM_KMS=ON` never compiled** — it compiles cleanly on `32b7d0d` (11 objects, `AMLOGIC_DRM_KMS=1` in the flags).
- **CI6–CI9 wrapper timeout, `cancel-in-progress` on main, libdrm required with KMS off, sudo for the chroot smoke** — nits. The latest main run still uploads artifacts.
- **N1–N4 test tmp paths, `.gitignore` pattern, `rm -rf --one-file-system`, `parseViewport` whitespace** — nits.
