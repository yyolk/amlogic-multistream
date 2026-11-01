# Third review: `main` @ 49b4c8f (after #9, #10, #11)

Unfiltered pass. A separate adversarial pass will check these claims and
write `SURVIVING3.md`, so treat every item here as a claim to be checked,
not a task list. No production code was changed, and no GitHub issues were
filed.

Scope: the whole tree as it stands after #9 (AppImage SIGTERM, early
Ctrl+C, HLS variants, libX11 packaging), #10 (MQTT → decode → present
through `StreamPlayer`), and #11 (layout validation, 9-stream cap, CI
requires the decoder tests). Measured against the project goals:
multi-stream HLS, MQTT / Home Assistant control, AppImage packaging, a
RetroArch-style CoreELEC launch, and eventual Amlogic hardware decode.

Inputs: `REVIEW.md` / `SURVIVING.md` (draft PR #5) and `REVIEW2.md` /
`SURVIVING2.md` (draft PR #8). Items from those files are cited by their
original IDs (for example REVIEW2 L4, REVIEW B5). Each one was re-checked
against `49b4c8f` before it was listed as fixed or re-raised.

**Severity.** **blocker** = defeats a project goal on a path this tree
already ships, or strands the box. **major** = wrong behavior on a shipped
path, a security exposure, or an interface the next milestone has to
break. **minor** = real but contained. **nit** = polish.

**Evidence tags.** `[repro]` = reproduced on this VM (recipe in "How this
was checked"). `[code]` = from reading the code, not run. `[device]` =
requires GS-King X / CoreELEC to confirm. Nothing here ran on a GS-King X.

---

## Triage index

| Severity | Items |
| --- | --- |
| blocker | **B1** one stalled or slow stream freezes every tile and delays SIGTERM without bound |
| major | **L1** a second signal during Kodi restore exits without starting Kodi; **L2** SIGKILL is the routine shutdown path; **L3** `PLAYER_EXTRACT_DIR` is `rm -rf`'d; **P1** no frame pacing; **P2** whole wall decoded and composited on one core; **S1** MQTT reaches the full FFmpeg demuxer/decoder surface as root; **I1** `StreamPlayer` is bound to `SoftwareRenderer`; **I2** `DecoderBackend` contract blocks the hardware-decode goal; **G1** MQTT mode renders to a buffer nobody displays; **CI1** zero 32-bit ARM coverage |
| minor | L4–L7, P3–P7, S2–S5, C1–C3, C5, C6, I3, CI2–CI5 |
| nit | L8, L9, P8–P10, C4, C7, C8, I4, CI6–CI9, N1–N4 |

---

## 1. Already fixed since REVIEW2

These were checked against `49b4c8f` and should not be re-raised.

| Prior item | Status | Evidence on `49b4c8f` |
| --- | --- | --- |
| SURVIVING2 #1 / REVIEW2 T1: SIGTERM kills the AppImage runtime and orphans the player | **Fixed (#9)** | `scripts/launch-coreelec.sh:136-171` extracts a type-2 AppImage once and runs `squashfs-root/AppRun` directly, so `$!` is the player. `stop_player` (`:89-106`) polls for 10 s, then SIGKILLs. CTest `launch_coreelec` covers "SIGTERM AppImage runtime" under bash, dash, and busybox, and CI runs the same script against the packaged AppImage (`ci.yml:123-128`). Re-run here: pass. |
| SURVIVING2 #2 / REVIEW2 T2: Ctrl+C before the fork strands Kodi on dash/busybox | **Fixed (#9)** | `launch-coreelec.sh:127` traps INT before `systemctl stop kodi`. The "SIGINT before player" stand-in passes on dash and busybox. |
| SURVIVING2 #3 / REVIEW2 O1: HLS master downloads every variant | **Fixed (#9)** | `src/ffmpeg_decoder.cpp:122-129` sets `AVDISCARD_ALL` on every stream except the selected one. `ffmpeg_decoder_tests` serves a two-variant master and fails if the unselected variant's last segment is requested. Passes here and in CI. |
| SURVIVING2 #4 / REVIEW2 T3: AppImage needs `libX11` and the excluded closure | **Fixed for the smoke image (#9)** | `scripts/build-appimage.sh:139-212` copies the excluded closure into the AppDir. `:224-280` runs `--help` in a chroot that has only glibc, libstdc++, libgcc_s, zlib, and libdrm. The device build is still the README TODO and is not re-raised. |
| REVIEW2 L1 / SURVIVING S6: `start()` parks the main thread in `sigwait` | **Fixed differently (#10)** | `src/mqtt_controller.cpp:199-216` ticks `StreamPlayer` and then calls `sigtimedwait` with a 20 ms timeout. (The new problem with that shape is B1.) |
| REVIEW2 L3: `restart` only logs | **Fixed (#10)** | `StreamInfo::restartGeneration` (`src/stream_manager.h:13`), consumed in `StreamPlayer::ensureOpen` (`src/stream_player.cpp:121-141`). |
| REVIEW2 L6: viewport comes from the first frame | **Fixed for MQTT (#10)** | `--viewport WxH` (`src/mqtt_controller.cpp:276-283`). `--url` keeps a frame-sized viewport on purpose. |
| REVIEW2 L7 / REVIEW A10: bad layout tokens stored | **Fixed (#11)** | `src/stream_manager.cpp:26-33`, `:125-134`. `banana`, `4`, and `2X2` are rejected and leave the previous setting in place. Covered by `stream_manager_test`, `mqtt_smoke_test`, and `stream_player_test`. |
| REVIEW2 L9 / REVIEW A11 (count): no stream cap | **Fixed (#11)** | `src/stream_manager.cpp:64-68` caps at `LayoutEngine::kMaxStreams` (9). The id/url length half of A11 is still open (C7). |
| REVIEW2 L10: no live reconnect | **Partly fixed (#10)** | Open or read failure retries after 1 s (`stream_player.cpp:11`, `:234-239`). Pacing is still open (P1). |
| REVIEW2 C1 / SURVIVING #7: CI green with decoder tests skipped | **Fixed (#11)** | `AMS_REQUIRE_DECODER_TESTS=1` plus `scripts/ctest-require.sh`. The CI log for 49b4c8f shows `Required decoder tests passed: ffmpeg_decoder present_smoke stream_player_http`. |
| REVIEW2 C2: no launch-script CI | **Fixed (#7, #9)** | `scripts/test-launch-coreelec.sh` runs in CTest and against the packaged AppImage. |
| REVIEW2 C3: AppImage smoke on a host with every library | **Fixed (#9)** | Minimal chroot smoke, as above. |
| REVIEW2 N6 / N7: extract to `/tmp`; `sleep 1` instead of confirming exit | **Fixed (#9)** | Extracts to `/storage/amlogic-multistream.AppDir`, then a bounded poll. |
| REVIEW2 N9: docs list "HLS demux" as missing | **Fixed** | `docs/mqtt.md` "Not in this build" no longer lists it. |
| REVIEW B3: `restart` with a bad id restarts all streams | **Mostly fixed (#10)** | A present but empty, non-string, or unknown `id` now fails (`src/mqtt_commands.cpp:203-221`). Omitting `id` still restarts all, which is documented. A numeric id is still treated as a string (C4). |

**Still open from earlier reviews and re-raised below with new evidence:**
REVIEW2 L4 (now B1), REVIEW2 L5 (now P2), REVIEW2 L10 pacing (now P1),
REVIEW2 T5 (now S1, live since #10), REVIEW2 C4 (now CI5), REVIEW2 C6 /
REVIEW A13 (now CI3), REVIEW2 N2 (now L8), and REVIEW B4/B5/B6/B7/B8/B12
(now C3/C6/P6/C5/S4/S5).

**Still open but not re-raised.** These are unchanged and were deliberately
dropped before: REVIEW2 T4 (the `--url` local-file allowance applies to
remote playlists; operator-only path), N1 (the whitelist comment), N3
(uppercase schemes rejected), N4 (BT.601 PPM), N5 (unchecked
`systemctl stop`), and L8 (frame lifetime across threads, moot while there
is one thread).

---

## 2. Blocker

### B1. blocker — One stalled or slow stream freezes every tile, ignores `remove`/`stop`, and delays SIGTERM without bound `[repro]`

- **Where:**
  - `src/stream_player.cpp:223-253`: `ensureOpen` (`avformat_open_input` + `avformat_find_stream_info`) and `readFrame` run inline, one stream after another, on the main thread.
  - `src/mqtt_controller.cpp:199-205`: the signal check only runs after `tick()` returns.
  - `src/ffmpeg_decoder.cpp:86`: the only bound is `rw_timeout` (15 s per I/O operation).
  - `src/ffmpeg_decoder.cpp:97`: no `AVFormatContext::interrupt_callback` is set anywhere.
- **What:** With the real binary, a local mosquitto, and `http.server` serving a 10 fps MPEG-TS clip:

  | Scenario | Healthy tile, presents in 10 s | SIGTERM → exit |
  | --- | --- | --- |
  | healthy stream alone | 438 | 0.10 s |
  | + 1 URL whose server accepts and never answers | **1** | 5.2 s |
  | + 3 such URLs | **1** | **35.5 s** |
  | + 1 URL whose server sends HTTP headers, then 1 byte every 5 s | **0** after the add | **still running 45 s after SIGTERM** (`wchan=do_poll`), then SIGKILLed |

  - **Trickle case:** each byte resets `rw_timeout`, and the default probe size is 5,000,000 bytes, so `avformat_find_stream_info` effectively never returns.
  - **`remove` does not help:** `remove slow` was acknowledged, and the retained status no longer listed it. The healthy tile still stayed at 146 presents for the next 20 s, because the main thread was still inside FFmpeg for the removed stream. `stop` cannot help either, for the same reason.
- **Why blocker:**
  - Multi-stream HLS is the core goal. Today one flaky camera, or one `add` from anyone on the broker (anonymous by default), freezes the whole wall.
  - The only recovery is SIGKILL, which on the device is the launch script's fallback (L2).
  - `docs/mqtt.md:38` documents that one blocking read delays the tick. It does not say the delay is unbounded, or that it hits every tile.
- **Fix direction:**
  1. Install an `AVIOInterruptCB` on every `FfmpegDecoder`. It should check an atomic abort flag plus a deadline per open and per read. FFmpeg's network waits poll in roughly 100 ms slices and check that callback, so this bounds the trickle case. The flag must be settable from another thread (see I2).
  2. Cap the probe with `probesize`/`analyzeduration` and an open deadline (for example 10 s).
  3. Move open and read per stream off the main thread, with a single-slot frame mailbox per tile, so the main thread only composites. `stop`, `remove`, and shutdown set the abort flag.
  4. Add tests: a fake decoder that blocks (unit), and the real binary with a blackhole URL, asserting SIGTERM → exit in under the launch script's 10 s (CI2).

---

## 3. CoreELEC launch script: lifecycle and shell traps

### L1. major — A second HUP/TERM while Kodi is being restored exits the script without starting Kodi, and leaves the player running `[repro]`

- **Where:** `scripts/launch-coreelec.sh:110-117`. `restored=1` is set at `:114`, before `stop_player` and `systemctl start kodi`. The traps are at `:126` (`trap 'restore_kodi; exit 0' HUP TERM`).
- **What:** The first signal runs `restore_kodi`, which sets `restored=1` and enters `stop_player`'s 10 s poll. A second HUP or TERM in that window runs the trap again. The nested `restore_kodi` returns at once because `restored` is 1, and then the trap runs `exit 0`. Kodi is never started, and the player is not SIGKILLed.

  Repro: the payload is a script that ignores TERM and HUP (a stand-in for a player blocked inside FFmpeg, B1). `setsid <shell> launch-coreelec.sh` with a logging `systemctl`, then `kill -HUP` and, 2 s later, `kill -TERM`:

  | Shell | Exit | `systemctl` calls | Player afterwards |
  | --- | --- | --- | --- |
  | dash | 0 | `stop kodi` only | alive |
  | bash | 0 | `stop kodi` only | alive |
  | busybox sh | 0 | `stop kodi` only | alive |
  | dash, TERM then TERM | 0 | `stop kodi` only | alive |
  | dash, single TERM (control) | 0 | `stop kodi`, `start kodi` | gone |

- **Why:** This is the stranded-Kodi outcome the traps exist to prevent. It needs two signals inside the stop window, and B1 makes that window routinely the full 10 s. Realistic triggers are an impatient second `kill`, `killall` run twice, or a hangup followed by a supervisor's TERM.
- **Fix direction:**
  - Make the first line of `restore_kodi` `trap '' HUP TERM INT`, so later signals are ignored while restoring, or make the nested trap a no-op that does not `exit`.
  - Set `restored=1` only after `systemctl start kodi`.
  - Add a stand-in case: a TERM-ignoring payload, two signals, and assert exactly one `start kodi`.

### L2. major — SIGKILL is the normal shutdown path whenever a stream is stalled, and it loses the final status and every log line `[repro]`

- **Where:** `launch-coreelec.sh:95-105` (10 s poll, then KILL), combined with B1.
- **What:** Real binary under the real script (dash), with three blackhole URLs added and `start` sent. SIGTERM to the script reached `systemctl start kodi` **11.1 s** later through the KILL path.
  - The player never logged `MQTT shutting down`.
  - The retained status was the will message: `"online":false` with `"streams":[]`. The clean-shutdown status (`mqtt_controller.cpp:218-222`) carries the real table. The will does not, so Home Assistant loses the stream list.
  - The script's stdout file was **0 bytes**. Stdout is block-buffered when it is not a TTY, so SIGKILL discards everything, including `MQTT broker …` and the open failures. `test-launch-coreelec.sh:461-463` already notes the buffering.
- **Fix direction:** Fixing B1 is the real fix. Separately:
  - Make stdout line-buffered at startup (`setvbuf(stdout, nullptr, _IOLBF, 0)` plus `std::cout << std::unitbuf`, or send logs to stderr), so a KILL leaves a trail.
  - Once the shutdown time is bounded, size the poll above that bound.

### L3. major — `PLAYER_EXTRACT_DIR` is `rm -rf`'d with no guard; pointing it at `/storage` wipes the box's user data `[repro]`

- **Where:** `launch-coreelec.sh:138-152`. The removal at `:152` runs whenever `.source-stamp` is missing or different, which is always the case on the first run.
- **What:** With a stand-in type-2 AppImage at `storage/amlogic-multistream`, plus a `userdata.xml` and `.kodi/guisettings.xml` in the same directory, `PLAYER_EXTRACT_DIR=$PWD/storage` removed **everything**. That included the AppImage itself, and then extraction failed: `env: '/…/storage/amlogic-multistream': No such file or directory`, exit 1. A relative value such as `.` resolves to `$(pwd)/.` and deletes the current directory.
- **Why:** README `:158` describes the variable only as an override for the unpack directory. "Put it in `/storage/apps`" or "`/storage`" is a natural reading, and on CoreELEC `/storage` holds all Kodi configuration and add-ons. The impact is total data loss as root, and the guard is a few lines.
- **Fix direction:** Only remove a directory that is empty or already contains this script's `.source-stamp`. Never remove a directory that contains `$player_bin`. Refuse `/`, `/storage`, and `$HOME`. Better still, delete only `$extract_root/squashfs-root` and the stamp, never `$extract_root` itself. Add a stand-in test with a sentinel file.

### L4. minor (latent) — `signal_tree` clobbers the global `root`, so the root PID and later siblings are never signaled `[repro]`

- **Where:** `launch-coreelec.sh:72-87`. `root=$1` and `sig=$2` (`:73-74`) are globals. Only `dir pid pp` are `local` (`:75`). The function recurses at `:83`.
- **What:** After the first recursion, `root` holds the deepest child's PID. The parent's loop then compares the remaining `/proc` entries against the wrong PID, and the final `kill` (`:86`) signals the child again instead of the root. Repro under dash, bash, and busybox ash 1.36.1: with a root P that has a child C1 (which has a child G) and a second child C2, `signal_tree P TERM` leaves P and C2 running, and `$root` ends up equal to G's PID.
- **Why latent:** linuxdeploy's `AppRun` is a symlink to the ELF, and the player forks no processes. So today `player_pid` has no children and the bug is never reached. It triggers as soon as `AppRun` is a wrapper that forks, which is exactly the case the function's own comment (`:70-71`) is written for.
- **Fix direction:** `local root sig`, or pass the values through without globals. Add a stand-in whose `AppRun` forks two children.

### L5. minor — The extraction stamp is size plus mtime, so a same-size rebuild copied with `cp -p` or `rsync -a` reuses a stale tree `[code]`

- **Where:** `launch-coreelec.sh:143-148`.
- **Fix direction:** Use `cksum` (busybox has it) or the AppImage's embedded version string.

### L6. minor — There is no single-instance lock `[code]`

- **What:** Two concurrent launches (for example a Home Assistant script fired twice) both `rm -rf` and extract the same directory. They run two players with the same default client id (C6), which take each other's broker session. Whichever player exits first starts Kodi while the other is still running.
- **Fix direction:** A `mkdir`-style lock or `flock` around the whole script.

### L7. minor — SIGHUP kills the player with the default action, so the final status is the will with an empty table `[repro]`

- **Where:** `mqtt_controller.cpp:173-177` blocks only SIGINT and SIGTERM.
- **What:** `kill -HUP` gives exit 129 with no shutdown log, and the retained status becomes `{"online":false,…,"streams":[]}`. A terminal hangup delivers HUP to the whole foreground group, so on an SSH drop the player dies this way, not through the trap. README `:68` says HUP "stops the player", which is true, but it is not a clean stop.
- **Fix direction:** Add SIGHUP to the `sigtimedwait` set.

### L8. nit — The script exits 0 on TERM/HUP and on player failure (REVIEW2 N2, still open)

- **Where:** `launch-coreelec.sh:126`, `:186` (`wait … || true`).

### L9. nit — After SIGKILL, Kodi starts after a fixed `sleep 1` without confirming the player is gone `[code]`

- **Where:** `launch-coreelec.sh:103-105`.
- **What:** A player in uninterruptible sleep (a future DRM ioctl, NFS) can still hold the display when Kodi starts.
- **Fix direction:** Poll `proc_running` after the KILL as well.

---

## 4. StreamPlayer and MqttController lifecycle

### P1. major — No frame pacing: frames are presented as fast as the loop turns, not at the stream's rate `[repro]`

- **Where:** `src/stream_player.cpp:223-253`. `DecodedFrame::pts` and `time_base` are filled (`ffmpeg_decoder.cpp:273-278`) and never read.
- **What:** A 10 fps, 30 s clip over HTTP got 438 presents in 10 s (about 4.4× real time), reached EOF in under 10 s, and then replayed after the 1 s retry.
  - For live HLS, this reads ahead to the live edge and then blocks inside `av_read_frame` until the next segment. That is a stall of up to one target duration, and because of B1 it stalls every tile `[code]`.
  - A stream faster than the tick rate (50 Hz minus the decode and present time of every tile) falls behind live, and the HLS demuxer then skips expired segments `[code]`.
- **Fix direction:** Map pts to wall-clock time per session, keep the last frame, and read only when the next frame is due. This works best on top of B1's per-stream threads.

### P2. major — The whole wall is decoded and composited on one core `[repro]`

- **Where:**
  - `ffmpeg_decoder.cpp:163`: `avcodec_open2` uses the default `thread_count`, which is **1** (checked: `avcodec_alloc_context3(h264)->thread_count == 1`).
  - `stream_player.cpp:233`: every stream is decoded serially on the main thread.
  - `software_renderer.cpp:215`: `std::vector<uint8_t> next = pixels_;` copies the **entire viewport** on every `present()`, and `:220-229` writes a full-viewport PPM per present when `--render-dir` is set.
- **What:** Benchmark of 9 × 640x360 YUV420P presents into the `LayoutEngine::compute(…, 9)` tiles, `-O2`, on this 4-vCPU Xeon:

  | Viewport | 9-tile wall | Copied per present |
  | --- | --- | --- |
  | 1280x720 (MQTT default) | **13.6 ms** | 2.76 MB |
  | 1920x1080 | **31.7 ms** | 6.2 MB |

  That is before any decoding. A73/A53 cores are several times slower `[device]`.
- **Why:** The interim software path cannot reach a 9-tile wall at frame rate on the target, even for small streams. REVIEW2 L5 was dropped because "this tree does not draw nine tiles". #10 made it draw nine tiles.
- **Fix direction:**
  - Copy only `dest`, or validate first and then draw in place, since every failure check in `present()` runs before the first write.
  - Set `thread_count` to a small value per stream.
  - The real fix is B1's per-stream decode threads, and ultimately hardware decode (I2).

### P3. minor — The player exits 1 if the broker is not reachable at startup, contradicting "reconnects with backoff" `[repro]`

- **Where:** `mqtt_controller.cpp:179-184`. `docs/mqtt.md:22` says "reconnects with backoff from 1s to 30s".
- **What:** Nothing listening on the port gives `MQTT connect failed: Connection refused`, exit 1. An unresolvable host gives `Lookup error.`, exit 1. Through the launch script, Kodi stops, the player exits, and Kodi starts again. Backoff only applies after a first successful connect.
- **Fix direction:** On a failed `connect_async`, start the loop anyway and let libmosquitto reconnect (or retry in the tick loop). Exit only on configuration errors.

### P4. minor — Race: a command handled during shutdown can re-publish a retained `"online":true` after the final offline status `[code]`

- **Where:** The main thread publishes the offline status at `mqtt_controller.cpp:218-222`, then calls `shutdown()` → `mosquitto_disconnect` (`:401`). The mosquitto thread can still run `handleMessage` → `publishStatus` (`:375-377`) or `handleConnect` → `publishStatus` (`:352`) in between.
- **What:** A clean DISCONNECT makes the broker discard the will, so a retained `online:true` published in that window sticks. The window is microseconds, and this was not reproduced.
- **Fix direction:** Set an atomic `shuttingDown_` flag before the final publish and check it in `handleMessage` and `handleConnect`. Alternatively, unsubscribe, publish offline, then disconnect.

### P5. minor — SIGPIPE kills the player when stdout is a pipe that closes `[repro]`

- **What:** Running `amlogic-multistream … | head -n 1`, then sending 800 `list` commands, gives exit status 141. `launch-coreelec.sh | tee log` over SSH has the same shape when the session drops.
- **Fix direction:** `signal(SIGPIPE, SIG_IGN)` at startup.

### P6. minor — A failed subscribe is never retried (REVIEW B6, still open) `[code]`

- **Where:** `mqtt_controller.cpp:346-350`.
- **What:** The client stays connected and silently ignores commands until the next reconnect.

### P7. minor — A failed stream keeps its last frame on screen indefinitely, and nothing reports it `[code]`

- **Where:** `stream_player.cpp:234-239` closes the session but does not clear the tile. The status JSON (`mqtt_json.cpp:411-438`) has no per-stream state. See I3.

### P8. nit — `stop` then `start` within one 20 ms tick does not close and reopen decoders `[code]`

- **Where:** `stream_player.cpp:178-187`. `wasPlaying_` never sees `false`. `docs/mqtt.md:66` says `stop` closes the decoders.

### P9. nit — A removed stream can be presented once more after `remove` is acknowledged `[code]`

- **Where:** The tick works from a snapshot taken at `stream_player.cpp:189`. Because of B1, "once more" can arrive 15 s × N later.

### P10. nit — `restart` bypasses the backoff (`stream_player.cpp:130`) `[code]`

- **What:** A flood of `restart` commands reopens every stream on every tick. Any broker client can already do worse (B1).

---

## 5. Security

### S1. major — Any broker client can drive the full libavformat/libavcodec parser surface, as root, over plain HTTP `[repro]`

- **Where:**
  - `src/mqtt_commands.cpp:27-32` and `stream_player.cpp:49-54` check only the scheme.
  - `ffmpeg_decoder.cpp:85-93` sets `protocol_whitelist` (good: `file` is excluded on the MQTT path), but no `format_whitelist` or `codec_whitelist`, and leaves `tls_verify` at its default of 0.
- **What:** Via MQTT `add` plus `start` against `http.server`, the player opened and decoded a GIF (`codec=gif`), a Matroska/FFV1 file (`codec=ffv1`), and a raw PNG pipe (`codec=png`). Any demuxer that probes positive is reachable. MQTT is anonymous by default, without TLS (`docs/mqtt.md:20-22`), and CoreELEC runs the player as root. Because `tls_verify` is 0, `https://` URLs give confidentiality but no authenticity.
- **Why now:** REVIEW2 T5 was dropped while MQTT never opened a decoder. #10 made every `add` + `start` an `avformat_open_input` on a URL chosen by the sender. That is the condition REVIEW A9 warned about.
- **Fix direction:**
  - Set `format_whitelist=hls,mpegts,mov,mp4` (HLS needs mpegts and mov for its segments) and a `codec_whitelist` for the codecs you support (`h264,hevc`).
  - Set `max_pixels` (S2).
  - Set `tls_verify=1` with `ca_file` once the device has a CA bundle `[device]`.
  - Document broker authentication as required for anything beyond a lab LAN.

### S2. minor — No `max_pixels` cap: a hostile stream decodes at any size before `present()` rejects it `[code]`

- **Where:** `software_renderer.cpp:198` rejects sides above 8192 only after decode and `sws_scale` (`ffmpeg_decoder.cpp:221-264`) have allocated full-size buffers. FFmpeg's default `max_pixels` is `INT_MAX`.
- **What:** Nine such streams in a 32-bit userspace (about 3 GB of address space) is a plausible out-of-memory `[device]`. Not reproduced.

### S3. minor — SSRF to LAN and loopback through `http(s)` GET and redirects (by design, residual) `[code]`

- **What:** The allowlist is scheme-only, so any broker client can make the box fetch from internal hosts. Earlier reviews accepted this as POC transport.
- **Fix direction:** An optional `--allow-hosts` (or a deny list for loopback and link-local) is cheap to add before Home Assistant wiring.

### S4. minor — Credentials leak through retained status, logs, and argv (REVIEW B8, still open) `[code]`

- **Where:** URLs, including `user:pass@` and query tokens, are published in the **retained** status (`mqtt_json.cpp:431-432`) and logged (`stream_manager.cpp:71`, `ffmpeg_decoder.cpp:176`). `--mqtt-pass` is visible in `/proc/*/cmdline`.

### S5. minor (supply chain) — AppImage tools are unpinned and the cache never refreshes (REVIEW B12, still open) `[repro]`

- **Where:** `build-appimage.sh:29-31` downloads `continuous` builds with no checksum. `ci.yml:54-58` uses the cache key `appimage-tools-…-v1`. The 49b4c8f CI log shows `Using cached …/linuxdeploy-aarch64.AppImage`, so whatever was fetched first runs forever, on a runner with sudo.
- **Fix direction:** Pin release tags plus SHA-256 values, and put the hash in the cache key.

### Not a finding: path injection

MQTT-supplied strings never reach a filesystem path. Stream ids and URLs go only to JSON, logs, and `avformat_open_input` (with `file` excluded from the protocol whitelist). `--render-dir` is CLI-only, and PPM names come from a counter. The only path hazard found is L3 (an environment variable, operator-controlled).

### Nit: log injection

Ids and URLs are printed raw, so `\n` or ANSI escapes from MQTT can forge log lines (REVIEW B20, still open).

---

## 6. Other correctness and robustness

### C1. minor — Any `avcodec_send_packet` error still escalates to a full reopen on the shared thread (REVIEW A8, partially reproduced) `[code]`

- **Where:** `ffmpeg_decoder.cpp:341-345` → `failRead`, and `stream_player.cpp:234-239` closes and reopens after 1 s.
- **What:** Not reproduced on FFmpeg 6.1. Six seeds of 3 × 400-byte corruptions in an H.264 TS, and one in an MPEG-4 TS, all concealed (284–300 of 300 frames, with no hard error). The code path is unchanged, though, and a codec or build that returns `AVERROR_INVALIDDATA` turns each corrupt packet into an HLS reopen that stalls every tile (B1).
- **Fix direction:** Treat `AVERROR_INVALIDDATA` as skip-and-count.

### C2. minor — `--render-dir` in MQTT mode writes a full-viewport PPM on every present, unbounded `[repro]`

- **Where:** `software_renderer.cpp:220-229`.
- **What:** At 1280x720 that is 2.76 MB per file. One stream measured about 44 presents/s, so roughly 120 MB/s per stream, written into what will usually be `/storage` on eMMC or SD. Each file is also a partial composite: one tile updated, the rest stale.
- **Fix direction:** Write once per wall frame, add a frame limit or rotation, and document it as a debug-only flag.

### C3. minor — JSON: surrogate pairs are rejected and invalid UTF-8 is echoed (REVIEW B4, still open) `[code]`

- **Where:** `mqtt_json.cpp:143-145`.
- **What:** Python's `json.dumps` (`ensure_ascii=True` by default, as used by Home Assistant) escapes any non-BMP character as a surrogate pair. An emoji in a stream id therefore fails the whole command as `invalid json`. Raw invalid UTF-8 is stored and echoed into the retained status.

### C4. nit — A numeric `id` is accepted as a string `[code]`

- **Where:** `mqtt_json.cpp:305-312` stores numbers as strings, so `{"id":5}` is id `"5"`. `docs/mqtt.md:73` says a non-string id fails with `id is required`, and gives only `true` as the example.

### C5. minor — Unknown flags are silently ignored in MQTT mode (REVIEW B7, still open) `[repro]`

- **Where:** `mqtt_controller.cpp:246-292`.
- **What:** `--mqtt-hots 10.9.9.9` was ignored, and the player connected to `localhost`. Decode mode rejects unknown flags (`app.cpp:113`).

### C6. minor — The fixed default client id lets two boxes knock each other off (REVIEW B5, still open) `[code]`

- **Where:** `mqtt_controller.h:34`, `:304`. The topic prefix default is shared too, so both boxes act on the same commands.

### C7. nit — Id and url length is bounded only by the 64 KiB payload cap `[code]`

- **What:** Nine 64 KiB urls give a retained status of about 600 KB (the length half of REVIEW A11).

### C8. nit — Miscellaneous `[code]`

- `app.cpp:299` prints `App::run` (debug).
- `ffmpeg_decoder.cpp:87` hardcodes `user_agent` version 0.1.
- `ffmpeg_decoder.cpp:83` calls `av_log_set_level` on every open.

---

## 7. Interface stability against the project goals

### I1. major — `StreamPlayer` is bound to `SoftwareRenderer`, and `Renderer` has no clear or commit `[code]`

- **Where:** `src/stream_player.h:33`, `:77` (`SoftwareRenderer&`). `stream_player.cpp:41-45`, `:181`, `:208`, and `:216` call `ppmDirectory()` and `clearViewport()`, which `Renderer` (`src/renderer.h:19-25`) does not have.
- **Why:** The next display step, DRM/KMS, needs a per-wall-frame commit (atomic flip) and a clear. Today that means changing `StreamPlayer`'s constructor and the `Renderer` interface at the same time as bringing up hardware. REVIEW2 L5 asked for a frame boundary. #10 built the multi-tile loop without one.
- **Fix direction:** Add `clear()` and `beginFrame()` / `endFrame()` (or `commit()`) to `Renderer`, have `StreamPlayer` take `Renderer&`, and move the PPM write into `SoftwareRenderer::endFrame`.

### I2. major — The `DecoderBackend` contract blocks the hardware-decode goal `[code]`

- **Where:** `src/decoder_backend.h:38-56`. `stream_player.cpp:155-158` refuses any backend where `isHardware()` is true.
- **What:**
  - `readFrame` is a blocking `bool`. It has no thread-safe `abort()` (needed for B1), and it cannot tell EOF from error from "no frame yet" (needed for P1).
  - `DecodedFrame` is CPU planes only. An Amlogic or V4L2 M2M decoder produces DMA-BUF or DRM-PRIME buffers.
  - README `:18` plans a hardware decoder as "its own `DecoderBackend`", but `StreamPlayer` closes every such backend, and `Renderer` cannot accept anything other than CPU YUV420P.
- **Fix direction:** While there is still a single implementation:
  - Add `abort()` (the one cross-thread call).
  - Make the read result an enum (`Frame`, `Again`, `Eof`, `Error`).
  - Introduce a frame handle that is either CPU planes or a DMA-BUF descriptor.
  - Let `StreamPlayer` accept a hardware backend when the renderer can import its frames.

### I3. minor — The MQTT status schema has no per-stream state, no version, and responses have no correlation id `[code]`

- **Where:** `mqtt_json.cpp:411-454`.
- **What:**
  - Status cannot say whether a stream is opening, playing, failed, or hidden past the grid (REVIEW B16).
  - There is no `schema` or `version` field for Home Assistant templates to pin.
  - Two automations issuing commands concurrently cannot match their responses.
- **Fix direction:** Add these before Home Assistant discovery consumers depend on the current shape.

### I4. nit — Torn snapshots and split play state (REVIEW2 L2, REVIEW B2, still open; harmless today) `[code]`

- **What:** `tick()` reads `list()` and `layoutSetting()` under two separate locks (`stream_player.cpp:189-190`). This is harmless today because `compute()` clamps. The play flag lives in the controller's atomic plus a `bool&` parameter.

### G1. major (goal gap, documented) — MQTT mode presents into an RGB buffer that nothing displays `[code]` `[device]`

- **Where:** `mqtt_controller.cpp:122-130`. `SoftwareRenderer` has no output, and PPM writing is off by default. `DrmRenderer` is not used on this path (`docs/mqtt.md:36`).
- **What:** On a GS-King X, `launch-coreelec.sh` today stops Kodi, burns CPU decoding up to nine streams, and puts nothing on HDMI until SIGTERM. The README states that DRM is not implemented, but neither the launch-script header nor README "Launch on CoreELEC" (`:154-162`) warns that running it now gives a blank screen.
- **Fix direction:** Add a one-line warning in both places. Optionally, make the launch script refuse to stop Kodi unless the binary reports a display backend (for example `--print-presenter`).

---

## 8. CI honesty

### CI1. major — The device architecture gets no build or test at all, and a cheap first step exists `[repro]`

- **Where:** `ci.yml:35-39` builds x86_64 and aarch64. README `:144-148` is honest that neither is the device binary.
- **What:**
  - A green run says nothing about armv7 hard-float with 32-bit `long` and `size_t`.
  - The five dependency-free test binaries (`layout_engine`, `stream_manager_test`, `mqtt_smoke_test`, `software_renderer`, `stream_player_test`) cross-compile with Ubuntu's `arm-linux-gnueabihf-g++` (`-march=armv7-a -mfpu=neon -mfloat-abi=hard`) and **all pass under `qemu-arm`**, in about 12 s of build time. The only compiler output was the GCC 7.1 `-Wpsabi` parameter-passing notes, which would need `-Wno-psabi` if `-Werror` is ever enabled.
  - The aarch64 job mostly duplicates x86_64 coverage: same distro, same FFmpeg.
- **Fix direction:**
  - Add an armhf job: cross-compile plus `qemu-arm` for the pure tests.
  - Next, a multiarch armhf FFmpeg/mosquitto build of the player (not attempted here).
  - Whether GitHub's arm64 runners can execute AArch32 natively was not checked; plan on qemu.

### CI2. minor — The real-player launch tests only drive an idle player `[code]`

- **Where:** `test-launch-coreelec.sh:513-537` waits for the player to reach `sigtimedwait` with an empty stream table, then signals it.
- **What:** B1, L1, and L2 cannot appear in that setup.
- **Fix direction:** Add a case that adds a blackhole URL, sends `start`, then SIGTERMs, asserting exit in under 10 s and a retained status that contains the table.

### CI3. minor — No broker-backed test of the real binary's MQTT → `StreamPlayer` path (REVIEW A13 / REVIEW2 C6, still open) `[code]`

- **What:** `mosquitto` is already installed in CI (`ci.yml:84`), yet nothing publishes `add` + `start` to the built player and checks presents, `response` payloads, the retained offline status, or the will. `stream_player_test` and `stream_player_http` call `executeCommand` and `tick` in-process.

### CI4. minor — No sanitizer job `[code]`

- **What:** A ThreadSanitizer build was clean here (see section 11), but nothing keeps it that way.

### CI5. minor — `-DAMLOGIC_DRM_KMS=ON` is never compiled (REVIEW2 C4, still open) `[code]`

### CI6. nit — The `run-tests.sh` timeout is shorter than the sum of the per-test timeouts `[code]`

- **Where:** The wrapper uses `timeout 300` (`run-tests.sh:79`). The per-test `TIMEOUT`s are 180 + 120 + 60 + 60 = 420 s, so the wrapper can fire first with a less useful message.

### CI7. nit — `cancel-in-progress: true` also cancels pushes to `main` (`ci.yml:23-25`), so intermediate merges get no artifacts `[code]`

### CI8. nit — The player target still requires libdrm with `AMLOGIC_DRM_KMS=OFF` (`CMakeLists.txt:85`) `[code]`

### CI9. nit — `build-appimage.sh` requires passwordless sudo for the chroot smoke (`:42-45`), which README "AppImage" (`:143`) does not list `[code]`

---

## 9. Missing tests

These are not in the tree and would have caught the findings above:

1. **`StreamPlayer` with a blocking fake decoder.** Encodes B1's contract: other tiles keep presenting, and `closeAll()` returns within a bound.
2. **`StreamPlayer` read failure.** The fake decoder never fails `readFrame`, so the close → backoff → reopen path (`stream_player.cpp:234-239`) has no unit test.
3. **A hidden stream is closed.** The `1x1` case (`tests/stream_player_test.cpp:231-235`) checks that `opened` is unchanged, but not that the hidden stream's decoder was closed.
4. **Real binary, shutdown bound.** A blackhole and a trickle URL, then SIGTERM, assert exit in under 10 s (CI2).
5. **Broker integration (CI3).** Add, start, the response topic, the retained offline status carrying the table, the will on SIGKILL, retained-command ignore, and the payload cap.
6. **Launch script:**
   - two signals in the stop window (L1);
   - a TERM-ignoring payload that forces the KILL path;
   - `PLAYER_EXTRACT_DIR` pointing at a directory with a sentinel file (L3);
   - an `AppRun` that forks two children (L4).
7. **An HTTP playlist with a `file:` segment** at `allow=0` (REVIEW2 C5, still open). `tests/ffmpeg_decoder_test.cpp:162` only checks a local path.
8. **Pacing**, once implemented: N presents over T seconds within a tolerance of the stream's frame rate.
9. **A fuzz target for `parseFlatJsonObject`** (REVIEW B4).
10. **The armhf build** (CI1).

---

## 10. Nits

- **N1.** Tests use predictable `/tmp/…-<pid>` paths followed by `remove_all` (`tests/stream_player_test.cpp:289-291`, `tests/stream_player_http_test.cpp:39-42`, `tests/ffmpeg_decoder_test.cpp:62-63`). Use `mkdtemp` (REVIEW B20).
- **N2.** `.gitignore:8`: the unanchored `amlogic-multistream` pattern ignores any future file or directory with that name (REVIEW B20).
- **N3.** In `build-appimage.sh:215-222`, if `sudo umount "$CHROOT_DIR/app"` fails (busy), the following `rm -rf "$CHROOT_DIR"` descends into the bind-mounted AppDir. Use `rm -rf --one-file-system`.
- **N4.** `parseViewport` accepts leading whitespace and `+` through `std::stoi` (`" 1280x+720"`).

---

## 11. Checked and fine

- **Scheme allowlist, both sides.** It is enforced in `executeCommand("add")` and again in `StreamPlayer::urlAllowed`. `protocol_whitelist` excludes `file` on the MQTT path. Covered by `stream_player_test` (a `file:` URL and a NUL never reach the factory).
- **Data races.** A ThreadSanitizer build of the player (`-fsanitize=thread -O1`) went through three cycles of add ×4, `start`, `layout 3x3`, `restart`, `remove`, `layout auto`, and `stop`, then SIGTERM, with **0 reports**. libmosquitto was not instrumented. `StreamManager` is mutex-guarded, `playing_` is atomic, and the renderer and sessions are touched only by the main thread.
- **Signal masks.** SIGINT and SIGTERM are blocked before `mosquitto_loop_start` and before any FFmpeg thread exists, so every thread inherits the mask. Ctrl+C reaches `sigtimedwait` even though the shell starts the background player with SIGINT ignored: Linux does not discard a signal that is blocked. This is CI-tested ("SIGINT after player").
- **Single-signal traps.** Single TERM, HUP, and pre- and post-fork INT pass under bash, dash, and busybox (CTest `launch_coreelec`, re-run here).
- **#11 behavior.** Layout validation, the 9-stream cap, `restart` generation semantics, and "an unknown id restarts nothing" behave as documented.
- **The CI guard.** `AMS_REQUIRE_DECODER_TESTS` works: `test-ctest-require.sh` covers registration, skip, and prefix-name cases, and the 49b4c8f CI log shows all three required tests passing.
- **32-bit math.** Integer math in `LayoutEngine` and `SoftwareRenderer` uses `long long` and `size_t` where it matters, and the armhf `qemu-arm` run passes.
- **CI on `49b4c8f`.** Both jobs are green, and `run-tests.sh` gives 10/10 here with `AMS_REQUIRE_DECODER_TESTS=1`.

---

## 12. Needs a device (GS-King X / CoreELEC)

Not blockers. Each one is a fact that would change a severity above.

- **P2:** decode and composite cost on A73/A53 for 4 and 9 tiles at the 1280x720 default.
- **L1 / L7:** what a dropped SSH session actually delivers on CoreELEC (dropbear or OpenSSH): HUP only, or HUP then TERM. This decides how often L1 triggers.
- **G1:** what HDMI shows after `systemctl stop kodi` with this player running (black, the console, or a frozen Kodi frame).
- **S2:** the 32-bit address-space headroom with nine large streams.
- **S1:** whether CoreELEC ships a CA bundle the player can point `ca_file` at.
- **Carried over from SURVIVING2 "Needs a device":** `readlink -f /bin/sh` and `uname -m` next to `file /bin/busybox`.

---

## How this was checked

Ubuntu 24.04 VM (4 vCPU Xeon), g++ 13, FFmpeg 6.1 (libavformat 60.16),
mosquitto 2.0.18, dash 0.5.12, bash 5.2.21, and busybox 1.36.1
(`busybox-static`, used for the CTest run and the L1 and L4 repros). `g++-arm-linux-gnueabihf` 13 and
`qemu-user` were used for CI1. Tree: `origin/main` @ `49b4c8f`.
`AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` gave 10/10.

- **B1:** `python3 -m http.server` served `clip.ts` (`testsrc` 320x240,
  10 fps, 30 s, mpeg4, MPEG-TS). The servers used were:
  - a blackhole that `accept()`s and never writes;
  - a trickle server that sends `HTTP/1.1 200` headers with
    `Content-Length: 100000000`, then one `0x47` byte every 5 s.

  The player ran as `--mqtt-host 127.0.0.1 --mqtt-port 18830 --viewport
  64x36 --render-dir frames`. Streams were added with `mosquitto_pub …/cmd/add`,
  then `…/cmd/start`. Presents were counted as PPM files in `frames/`. Exit
  latency was measured from `kill -TERM` to the PID disappearing. The
  `remove` case published `…/cmd/remove {"id":"slow"}` and counted PPMs
  20 s later.
- **L1:** `stubborn.py` (`#!/usr/bin/python3`, SIG_IGN for TERM and HUP,
  `sleep(60)`) as `PLAYER_BIN`, a logging `systemctl` first on `PATH`, and
  `setsid <shell> scripts/launch-coreelec.sh`. Then `kill -HUP` followed
  2 s later by `kill -TERM` (or TERM, TERM).
- **L2:** the same harness with the real binary as `PLAYER_BIN`,
  `MQTT_HOST/PORT/TOPIC_PREFIX` in the environment, three blackhole URLs
  added, and `start`. Then SIGTERM to the script. `systemctl.log`
  timestamps: TERM at `…773.02`, `start kodi` at `…784.14`. The retained
  status was read with `mosquitto_sub -C 1`.
- **L3:** the stand-in type-2 runtime from `scripts/test-launch-coreelec.sh`
  (stamped `AI\x02` at offset 8) at `storage/amlogic-multistream`, with
  sentinel files next to it, run with `PLAYER_EXTRACT_DIR=$PWD/storage`.
- **L4:** lines 47-87 of `launch-coreelec.sh` sourced into a driver, with
  `sh -c 'sh -c "sleep 300 & wait" & sleep 300 & wait' &` as the root,
  then `signal_tree "$P" TERM` and a check of what survived.
- **L7 / P3 / P5 / C5:** direct runs of the binary (`kill -HUP`, a closed
  port, `no-such-host.invalid`, `| head -n 1` plus 800 `list` publishes, and
  `--mqtt-hots`).
- **P1:** the B1 setup with only the healthy stream: 438 PPMs in 10 s, and
  `no frame from s1 (will retry)` before 10 s had passed.
- **P2:** an `-O2` harness linking `src/software_renderer.cpp` and
  `src/layout_engine.cpp`: 30 rounds of 9 presents of a 640x360 YUV420P
  frame into `LayoutEngine::compute(W, H, 9)`. The same harness printed
  `avcodec_alloc_context3(h264)->thread_count`.
- **S1:** `x.gif`, `x.mkv` (ffv1), and `x.png` (`image2pipe`) served by
  `http.server` and added over MQTT. The player log shows
  `FfmpegDecoder::open … codec=gif|ffv1|png`.
- **C1:** `h264.ts` (libx264, 30 s) with 3 × 400 random bytes at seeded
  offsets in the middle half, then `--url … --frames 300`.
- **TSan:** `cmake -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1"`, then the
  command sequence in section 11.
- **CI1:** each pure test compiled with `arm-linux-gnueabihf-g++ -std=c++17
  -O2 -Wall -Wextra -Wpedantic -march=armv7-a -mfpu=neon -mfloat-abi=hard`,
  then run with `qemu-arm -L /usr/arm-linux-gnueabihf`.
- **CI on `49b4c8f`:** `gh run view 41269339964 --log` (both jobs green,
  `Using cached …appimage-tools…`, `Required decoder tests passed`).
