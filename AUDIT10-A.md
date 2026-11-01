# AUDIT10-A: `main` @ 5c2c9d9 after AUDIT9 D1 (#57)

Audited commit: `5c2c9d910efd5a0fe36d987695c5f717a7ebc1c1` (squash of
[#57](https://github.com/yyolk/amlogic-multistream/pull/57), AUDIT9 D1).

Host and software path only: decoder, MQTT, present and renderer, CLI,
launch scripts, and the CI and AppImage scripts that run on CI. Amlogic
hardware decode, real DRM, and GS-King X stay gated. No production code is
changed in this PR. Nothing was fixed.

**Severity:** **must-fix** = breaks a guarantee the code or docs make, on a
reachable path. **should-fix** = real, contained, worth doing before hardware
work. **nit** = polish or test/doc gaps.
**Evidence:** `[repro]` = run on this VM. `[code]` = from reading the code.
**needs-GS-King-X** = needs the box.

**Environment.** Ubuntu 24.04.5 x86_64, g++ 13.3.0, glibc 2.39, FFmpeg 6.1.1
(libavformat 60.16.100, libavcodec 60.31.102, libavutil 58.29.100),
libmosquitto / mosquitto 2.0.18, libdrm 2.4.125.

**Suite.** `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` on this
commit: **14/14 pass**, 137.83 s, exit 0. Required decoder tests were not
skipped: `ffmpeg_decoder`, `present_smoke`, `stream_player_http`.
`mqtt_exit_test` passed (42.23 s): sigterm 2012 ms, drain-cmd 2009 ms,
gone-dns 20 ms, gone-blackhole 502 ms, drop-drain 2013 ms, auth give-up
1992 ms rc 1, startup auth rc 1, startup-dns 20 ms.

---

## Verdict

**No must-fix.** AUDIT9 D1, AUDIT8 P1, AUDIT7 B2, AUDIT6 N1, AUDIT5 F1 / X1 /
F3, and AUDIT4 A1–A5 still hold on the evidence below. Carried nits stay nits.

One new **should-fix**, **D2**. `#57` passes `max_pixels` into
`avformat_find_stream_info` only for streams that already exist when that
call starts. FFmpeg 6.1.1 applies that per-stream dictionary only to indexes
below the entry count (`orig_nb_streams` in `libavformat/demux.c`). A video
stream created during the probe is decoded with the default `max_pixels`
(`INT_MAX`). The playback `AVCodecContext` still has the cap, so the oversize
stream is rejected at `avcodec_open2`, and the probe buffer is released
before `find_stream_info` returns. The spike itself is about 379 MB from a
423 KB file, and two overlapping opens add.

| ID | Severity | Finding |
| --- | --- | --- |
| **D2** | **should-fix** | Streams born inside `avformat_find_stream_info` miss `max_pixels`. PSI-less MPEG-TS, HLS of that TS, and a PMT program plus an extra video PID each peak at ~379–381 MB during the probe. VmRSS after the probe is ~33 MB. Two concurrent opens of the PSI-less file reached 726 MB VmHWM. |
| J1, E1, H1, U1, B1, L1, N2–N4, F4–F7 | nit | Carried. Same code shape. No new evidence, so not elevated. |

---

## 1. Prior fixes on `5c2c9d9`

| Item | Verdict | Evidence |
| --- | --- | --- |
| **D1** reject before `sws_scale`, and `max_pixels` on streams that exist before the probe | **Closed** | `[repro]` Real binary `--url` of a 12000×12000 MJPEG in MOV (844809 bytes): rc 1, `ru_maxrss` **34632 KB**, `avcodec_send_packet` invalid, "Opened but decoded 0 frames", no `-> yuv420p`. A normal-PMT 12000×12000 H.264 MPEG-TS (433340 bytes) and an HLS playlist of that TS: rc 1, **89428 KB** and **89872 KB**. Probe logs `get_buffer() failed` and then `avcodec_open2` fails. `present_smoke` still enforces the 300000 KB ceiling on its own 12000×12000 MJPEG. |
| **P1** copy and present caps | **Closed** | `[code]` `decodedFramePresentable` (`src/ffmpeg_decoder.cpp:80-90`) and `SoftwareRenderer::frameAcceptable` share `sizeAcceptable` (1..8192) and `kMaxStride` 65536. Suite `software_renderer` and `present_smoke` passed, including the 8192×32 and 8192×8192 extent checks. No new present-spam loop. |
| **B2** `--url` local-file flag, including `hls+http://` | **Closed** | `[code]` `urlIsLocalFileSource` (`src/app.cpp:169-179`) is unchanged: `file:`, no colon, or a slash before the first colon. `present_smoke` still covers an HTTP playlist whose segment is `file://`. No new allowlist bypass was reproduced. |
| **N1** DNS off the main thread | **Closed** | `[repro]` `mqtt_exit_test` startup-dns **20 ms**, gone-dns **20 ms**. The startup `connect_async` still runs on the helper (`src/mqtt_controller.cpp:230-278`). |
| **F1 / X1** exit drain, retained offline, no `loop_stop` | **Closed** | `[repro]` sigterm 2012 ms, drain-cmd 2009 ms, drop-drain 2013 ms, gone-blackhole 502 ms, retained `online:false`. `[code]` `finishProcess` still sets `closing_` under `closeMu_` with the offline publish and `_exit`s (`src/mqtt_controller.cpp:331-355`). |
| **F3** retired-decoder cap | **Closed** | `[code]` `kMaxBusyRetired = kMaxStreams` (9). Suite `stream_player_test` passed. Not re-measured with a fresh-id churn harness. |
| **A1–A5** | **Closed** | The exit, auth, and whitelist cases those rounds added are still what `mqtt_exit_test` and `ffmpeg_decoder` / `present_smoke` pin (auth rc 1, startup-dns 20 ms, top-level demuxer list `hls,mpegts,mov,mp4,mpeg`, codec list includes `mp3float`). Not re-opened. |

---

## 2. D2 — probe decoders for streams created during `find_stream_info`

`open()` builds a per-stream `max_pixels` dictionary only when
`fmt_->nb_streams > 0`, then calls `avformat_find_stream_info`
(`src/ffmpeg_decoder.cpp:216-229`). The comment says that cap stops the
probe from materializing an oversize still. On FFmpeg 6.1.1 that dictionary
does not apply to a stream added after the call starts:

- `avformat_find_stream_info` snapshots `orig_nb_streams = ic->nb_streams`
  and notes that new streams get no options (`demux.c` around the snapshot).
- `try_decode_frame` and the later probe `avcodec_open2` pass
  `options[i]` only when `i < orig_nb_streams`. Otherwise the probe codec
  keeps the default `max_pixels` of `INT_MAX`.
- The playback context is separate and does set `codec_->max_pixels`
  (`src/ffmpeg_decoder.cpp:296-303`).

`mpegts` turns `auto_guess` on after the PAT scan and sets
`AVFMTCTX_NOHEADER`, so a video PID that was not in the PMT (or a file with
no PAT/PMT) becomes a stream during the probe. `hls` of that file leaves
the parent at `nb_streams == 0` after `avformat_open_input` and hits the
same `NULL` options pointer. Both names are on `kTopLevelFormatWhitelist`.
MQTT `add` accepts `http://` and `https://` (`src/mqtt_commands.cpp:27-31`).

`[repro]` Real binary `/workspace/build/amlogic-multistream --url … --frames 1`
(`ru_maxrss` is the peak). A throwaway probe that mirrors this `open()`
sequence also printed `/proc/self/status` VmRSS and VmHWM. Fixtures are
local files under the `--url` file allowance; the same `open()` runs for an
MQTT `http://` URL.

| Input | After `open_input` | After `find_stream_info` | Player rc | Player `ru_maxrss` | VmRSS after the probe |
| --- | --- | --- | --- | --- | --- |
| PSI-less H.264 TS, 432776 bytes, PID 0x100, 12000×12000 | `nb_streams=0` | 1× h264 12000×12000 | 1, `avcodec_open2` invalid. No `get_buffer() failed` line | **379364 KB** | 33264 KB (VmHWM 378688) |
| HLS media playlist of that TS | `nb_streams=0`, `iformat=hls` | 1× h264 12000×12000 | 1, same | **379716 KB** | 33920 KB (VmHWM 379240) |
| PMT program PID 0x100 is 320×240; PID 0x101 is 12000×12000 and is not in the program. 441424 bytes | 1× h264 0×0 | stream 0 becomes 320×240, stream 1 is h264 12000×12000 | **0**, presents stream 0 at 320×240 | **381236 KB** | 34468 KB (VmHWM 380924) |

The capped controls are the D1 row: the same 12000×12000 picture in MOV, and
the same H.264 in a normal single-program TS, stay at 35 MB and 89 MB because
those streams exist before the probe and `get_buffer` refuses the picture.

Three serial probes of the PSI-less file in one process stay at VmRSS
~33–35 MB and VmHWM ~379 MB. The frame is not retained and serial retries do
not accumulate. Two threads probing that file at the same time reached
**VmHWM 726312 KB**, so the peaks add while they overlap. A failed MQTT open
is `WorkerFail::Open` and `tick` waits `kRetryDelay` (1 s) before the next
`open()` (`src/stream_player.cpp:16`, `:518-525`). That repeats the spike.
It does not grow the retained set. The extra-PID file opens successfully, so
it spikes once per open or restart and then presents the PMT stream. This
round did not run a live broker; the retry interval is the code path above.
`--url` itself exits after one open.

This is should-fix, in the same family as D1, and it is not a re-open of D1.
D1's playback cap and the pre-`sws_scale` check still hold. `#57` already
described a stream created during the probe as an FFmpeg API gap whose
playback context stays capped. The new measurement is that the probe
materializes the frame (about 379 MB peak) before that rejection. The
process stays up, other tiles are not stopped by this open, there is no
retained `online:true`, and the spike is not a hang.

A fix was not implemented and not tested. FFmpeg 6.1.1 has no default
codec-option field for streams born inside `find_stream_info`, so a larger
per-stream array cannot cover an index that does not exist yet. A demux-only
pre-pass (`av_read_frame` until the streams in the first window exist, seek
back to 0, then `find_stream_info` with one dictionary per stream that now
exists) would cover the two shapes measured here. A PID that starts only
after that window can still be uncapped. Shrinking `probesize` does not help
when the frame sits inside the window. Playback `max_pixels` already stops
the second decode.

---

## 3. Checked, and not filed

- **8192×8192 stays legal.** `presentableMaxPixels()` is `kMaxExtent` squared
  and FFmpeg rejects `width * height > max_pixels`. The suite still accepts
  8192 on a side. In-extent frames are not a regression.
- **Under the area cap and over one side** (the 8224×32 case `#57` called
  out) still decodes, then `decodedFramePresentable` rejects before
  `sws_scale`. Cost is bounded by one legal frame. Left as `#57` left it.
- **~89 MB** on the capped normal-PMT H.264 probe is `get_buffer` refusing
  the picture. It is under the 300 MB `present_smoke` ceiling and under one
  legal 8192×8192 frame.
- **Allowlist.** No new bypass. Not re-tested this round, and not re-opened:
  an HTTP 302 to `file://`, a `data:` segment, a CRLF in the HTTP path,
  `crypto+file`, `concat`, `subfile`. Nested HLS `file://` opens are still
  `EPERM` in current FFmpeg, and the remote whitelist still omits `file` and
  `data`.
- **Retained online while the broker is dead.** The will is `online:false`.
  No new lie. F1 lock order is unchanged.
- **`authGiveUp_` stays set after a later CONNACK success.** The main loop
  samples it every tick and reconnect backoff is at least 1 s, so the
  process exits on the give-up before that matters.
- **In-cap decoder memory** (an 8192-wide H.264 DPB, a large packet that is
  still inside `max_pixels`) was not shown to amplify past the cap. The
  423 KB → 379 MB case is the missing probe `max_pixels`, which is D2.
- **Present-failure retry with no `--render-dir`.** Still closed by the
  shared caps. A zero-size layout cell returns success on an empty blit.
- **GS-King X, Amlogic hw decode, real KMS/DRM.** Not exercised. `DrmRenderer`
  still stays off unless `AMLOGIC_DRM_KMS` is set.

Carried nits, confirmed only as the same code, and not elevated: **J1**
(`Adding stream:` prints id and url raw, `src/stream_manager.cpp:71`; status
JSON still escapes), **E1** (worker `catch` sets `done` and not `failed`,
`src/stream_player.cpp:257-266`), **H1** (invalid host or prefix falls back
to localhost / `amlogic-multistream` after one stderr line,
`src/mqtt_controller.cpp:427-434`), **U1** (`scripts/build-appimage.sh`
still fetches linuxdeploy, the AppImage plugin, and appimagetool from
`continuous` with no sha256), **B1** (SIGTERM in the startup wait still
`_exit`s without DISCONNECT once `connect_async` has queued CONNECT and
before `connectReady` is visible), **L1** (`normalize_abs` still `set -- $1`
with pathname expansion, `scripts/launch-coreelec.sh:46-52`), **N2** (exit
`sent` is the publish return while a reconnect can be inside `connect`),
**N3** (a full reaper also delays a healthy reopen), **N4** (`stream_player.h`
still describes `_exit` as conditional on a worker inside the blocking
call; `finishProcess` always `_exit`s), **F4** (same-id replace waits out
the old lookup), **F5** (attached picture through `mov` / HLS), **F6**
(packed-audio fixtures still ADTS), **F7** (`scripts/ctest-require.sh` still
does not require `mqtt_exit_test`).

---

## CROSSCHECK-A

Peer: [AUDIT10-B](https://github.com/yyolk/amlogic-multistream/pull/58)
on the same tip, `5c2c9d9`. They report no must-fix and no should-fix.
This pass re-read that file and re-ran the D2 `--url` cases on
`/workspace/build/amlogic-multistream`.

**D2 stays should-fix.** It is not a must-fix, and it is not a nit.

AUDIT10-B's D1 numbers match this tree. Their 12000×12000 MJPEG MP4 peaks
around 32 MB; a re-run here is **34412 KB**, with `max_pixels` 67108864 and
no `-> yuv420p`. Their 16384×4096 case (product equal to 8192², then rejected
by extent at about 130 MB, flat across opens) is the in-cap cost already
left out of section 3. Their HLS MP3+APIC case stays under 36 MB. None of
those inputs are D2.

D2 is a stream that does not exist when `avformat_find_stream_info` starts,
so `src/ffmpeg_decoder.cpp:219-228` passes no `max_pixels` dictionary for it.
FFmpeg 6.1.1 applies the per-stream dictionary only while
`i < orig_nb_streams`. Re-measured on this binary:

| Input | rc | `ru_maxrss` |
| --- | --- | --- |
| PSI-less 12000×12000 H.264 TS | 1 (`avcodec_open2`, no probe `get_buffer() failed`) | **379320 KB** |
| HLS playlist of that TS | 1, same | **379744 KB** |
| PMT 320×240 plus extra PID 0x101 at 12000×12000 | 0, presents 320×240 | **381300 KB** |
| Normal-PMT 12000×12000 H.264 TS (control) | 1, probe `get_buffer() failed` | **89352 KB** |

`hls.c` calls `avformat_find_stream_info(pls->ctx, NULL)` only for an
ID3-timestamped playlist or an audio rendition. That is the path AUDIT10-B
measured, and a JPEG attached picture can be rejected by the parent playback
context without a 379 MB frame. A video HLS playlist does not take that
nested call. The parent's probe decodes the H.264, and with `nb_streams == 0`
that decode is uncapped. The stderr difference is the evidence: the normal
PMT logs `get_buffer() failed` during the probe; the PSI-less file logs only
the later playback `max_pixels` line, after the peak.

Their "retries do not accumulate" holds here too, which is why D2 is not
must-fix. Serial probes still return to ~33 MB VmRSS. Two overlapping opens
of the PSI-less file, re-run for this cross-check, reached **VmHWM 726176
KB**. AUDIT10-B's must-fix bar (hang, retained online, or growth past a
claimed bound) is not met. The peak is still the allocation `#57` said the
probe dictionary prevents, on a whitelisted `mpegts` / `hls` URL.

Nothing else in AUDIT10-B moves. The torn `MQTT running` / `MQTT connected`
line, the destructor join comment, and MJPEG even-size rounding stay as they
rated them. No carried nit is elevated.
