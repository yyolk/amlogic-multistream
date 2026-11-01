# AUDIT11: `main` @ b29b44d after AUDIT10 D2 (#60)

Audited commit: `b29b44d59bdba85bde1595e845adcc87557c6db3` (squash of
[#60](https://github.com/yyolk/amlogic-multistream/pull/60) onto `5c2c9d9`).
Single auditor pass. No peer cross-check.

Host and software path only: decoder, StreamManager, LayoutEngine, MQTT
controller, CLI, present path, allowlists, and the CI scripts that run on
x86_64. Amlogic hardware decode, real DRM, and GS-King X stay gated. No
production code is changed in this PR. Nothing was fixed.

**Severity:** **must-fix** = breaks a guarantee the code or docs make, on a
reachable path, in a way this series has treated as blocking (hang, wrong
exit that sticks a retained lie, or growth that does not come back).
**should-fix** = real, contained, worth doing before hardware work.
**nit** = polish, a test gap, or a documented tradeoff.
**Evidence:** `[repro]` = run on this VM. `[code]` = from reading the code.
**needs-GS-King-X** = needs the box.

**Environment.** Ubuntu 24.04 x86_64, g++ 13.3.0, glibc 2.39, FFmpeg 6.1.1
(libavformat 60.16.100, libavcodec 60.31.102, libavutil 58.29.100,
libswscale 7.5.100), libmosquitto / mosquitto 2.0.18, libdrm 2.4.125.
Player binary is `build/amlogic-multistream` from this commit.

**Suite.** `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` on this
commit: **14/14 pass**, 139.20 s, exit 0. None skipped. Required decoder
tests ran: `ffmpeg_decoder` (2.41 s), `present_smoke` (2.00 s),
`stream_player_http` (1.67 s). The other tests: layout_engine, stream_manager,
mqtt_smoke, software_renderer, stream_player (5.64 s), ctest_require,
launch_coreelec (54.38 s), kodi_addon (3.85 s), stream_player_stall (9.42 s),
mqtt_reconnect (17.56 s), mqtt_exit_test (42.20 s). `mqtt_exit_test`: sigterm
2012 ms rc 0, drain-cmd 2007 ms, gone-dns 20 ms, gone-blackhole 503 ms,
drop-drain 2011 ms, auth give-up rc 1 with the exit 2011 ms after the give-up
line (that line was 15.8 s after the refusing broker started, which is
reconnect backoff, not the drain), startup auth rc 1, startup-dns 20 ms rc 0.
Retained status on sigterm and drain-cmd was `online:false` with the stream
table kept.

---

## Verdict

**No must-fix.** AUDIT10 D2's measured fixtures are closed on this binary.
AUDIT9 D1, AUDIT8 P1, AUDIT7 B2, AUDIT6 N1, AUDIT5 F1 / X1 / F3, and AUDIT4
A1–A5 still hold on the evidence below. Carried nits stay nits.

One new **should-fix**, **D3**. `#60` demuxes streams into existence before
`avformat_find_stream_info` and then seeks back when the seek reports
success. The pre-pass stops on a 7 s timestamp gap. FFmpeg 6.1.1's probe
does not. A video PID that is still unread at that gap is created during
`find_stream_info` and decoded with the default `max_pixels` (`INT_MAX`).
A 440484-byte MPEG-TS peaks at about 381 MB and still presents the 160×120
program (`rc 0`). The buffer is released (RSS back to ~34 MB) and a second
open in the same process does not add another 381 MB. The same file over
HTTP, against a server that ignores `Range`, takes the same path. An HLS
playlist of that file does not (the pre-pass keeps the queue and sets
`probesize` to 1).

| ID | Severity | Finding |
| --- | --- | --- |
| **D3** | **should-fix** | A timestamp gap stops the D2 pre-pass before an undeclared video PID exists. Seek-back then lets `find_stream_info` decode that PID with `max_pixels == INT_MAX`. Local `--url` VmHWM **381052 KB**, HTTP **381560 KB**, both `rc 0` presenting 160×120. RSS after the probe is ~34 MB. Two overlapping players summed to **760624 KB**. The in-window D2 fixtures stay under 87 MB. |
| V1 | nit | `open()` now keeps the first video track of every container. `tiny-first.mp4` presents 16×16; `av_find_best_stream` on the same file returns the 320×240 track. The pre-pass does not run for this MP4. The header states first-video order, so this is the new contract. |
| J1, E1, H1, U1, B1, L1, N2–N4, F4–F7 | nit | Carried. Same code shape. No new evidence, so not elevated. |

---

## 1. Prior fixes on `b29b44d`

| Item | Verdict | Evidence |
| --- | --- | --- |
| **D2** probe `max_pixels` for streams born inside the usual probe window | **Closed** on the fixtures `#60` names | `[repro]` Same 12000×12000 H.264 picture as AUDIT10. PSI-less TS (432024 bytes): player `rc 1`, VmHWM **86832 KB**, stderr `exceeds specified max pixel count 67108864` and `get_buffer() failed`, then `avcodec_open2` invalid. PMT program 160×120 plus that picture appended as PID 0x101 (440484 bytes, no timestamp edit): player `rc 0`, presents 160×120, VmHWM **72516 KB**, same `get_buffer() failed` during the probe. A normal-PMT 12000×12000 TS (432588 bytes): `rc 1`, VmHWM **72204 KB**, `get_buffer() failed`. AUDIT10's uncapped number on these shapes was ~379–381 MB. |
| **D1** reject before `sws_scale` | **Closed** | `[code]` `decodedFramePresentable` is still in front of `sws_scale` (`src/ffmpeg_decoder.cpp:566-571`). `[repro]` The normal-PMT row above, and `present_smoke` (passed), which still enforces a 300000 KB ceiling on the MJPEG still and the PSI-less TS. |
| **P1** copy and present caps | **Closed** | `[code]` `StreamPlayer::copyOwned` still refuses `!frameAcceptable` (`src/stream_player.cpp:20-24`). Suite `software_renderer` passed. |
| **B2** HTTP `--url` must not enable `file` | **Closed** | `[code]` `urlIsLocalFileSource` is unchanged (`src/app.cpp:169-179`). `present_smoke` still rejects an HTTP playlist whose segment is `file://` (that test passed). |
| **N1** DNS off the main thread | **Closed** | `[repro]` `mqtt_exit_test` startup-dns **20 ms**, gone-dns **20 ms**. |
| **F1 / X1** exit drain, retained offline, no `loop_stop` on that path | **Closed** | `[repro]` sigterm 2012 ms, drain-cmd 2007 ms, drop-drain 2011 ms, gone-blackhole 503 ms, retained `online:false`. `[code]` `finishProcess` still sets `closing_` under `closeMu_` with the offline publish and `_exit`s (`src/mqtt_controller.cpp:331-355`). |
| **F3** retired-decoder cap | **Closed** | `[code]` `kMaxBusyRetired = kMaxStreams` (9) (`src/stream_player.h:55`). Suite `stream_player_test` passed. Not re-measured with a fresh-id churn harness. |
| **A1–A5** | **Closed** | Exit, auth, and the demuxer/codec allowlists are what `mqtt_exit_test`, `ffmpeg_decoder`, and `present_smoke` still pin (auth rc 1, startup-dns 20 ms, top-level demuxers `hls,mpegts,mov,mp4,mpeg`). Not re-opened. |

---

## 2. D3 — the pre-pass stops short of a PID the probe still decodes

`#60` reads packets before `avformat_find_stream_info` so late MPEG-TS and
HLS PIDs exist when the per-stream `max_pixels` dictionary is built
(`src/ffmpeg_decoder.cpp:406-422`). For `mpegts` and `mpeg` the pre-pass
stops when a packet timestamp is at least 7 s after the first timestamp it
saw, which is FFmpeg 6.1.1's default `max_analyze_duration` for those
formats (`src/ffmpeg_decoder.cpp:127-135` and `:261-273`). If the format is
not HLS and `rewindAfterDiscovery` returns success, the queued packets are
freed and `probesize` is left at the default (`:283-294`). HLS always keeps
the queue and sets `probesize` and `max_analyze_duration` to 1.

That 7 s check is not how `avformat_find_stream_info` stops. In FFmpeg
6.1.1 `libavformat/demux.c` the duration break runs only once the packet's
stream already has `codec_info_nb_frames > 1`, and the value it compares is
accumulated packet duration (or, failing that, a DTS span after more than
30 frames). A single PES whose PTS jumps by 10 s does not trip it. The
probe then keeps reading. `try_decode_frame` is called with
`options[i]` only when `i < orig_nb_streams`. That `i` is the index of the
first stream that still looks incomplete, not `pkt->stream_index`. A stream
created during the call has an index at or past `orig_nb_streams`, so the
probe decoder keeps `max_pixels == INT_MAX`. The playback context is a
different object and still sets the cap (`src/ffmpeg_decoder.cpp:492-495`).

`[repro]` Fixture, built on this VM:

1. `testsrc=size=160x120:rate=10:duration=0.4`, libx264, MPEG-TS. The PMT
   program is PID 0x100. The second video PES is PTS-only; its PTS field was
   rewritten from 126000+9000 = 135000 to 1035000 (plus 10 s at 90 kHz).
2. A 12000×12000 one-frame libx264 MPEG-TS (`ultrafast`, `stillimage`,
   CRF 51). Its PID 0x100 packets were remapped to 0x101 and appended. That
   PID is not in the PMT.

The file is 440484 bytes. A packet dump of `av_read_frame` (no
`find_stream_info`) shows `nb_streams == 1` after the jumped PES and
`nb_streams == 2` only on the following read, which is also the read that
scans to EOF. The pre-pass stops on the jumped PES, so the second stream
does not exist yet. `av_seek_frame` then returns 0.

Player `--url <file> --frames 1`, VmHWM sampled from `/proc/<pid>/status`:

| Input | rc | Presents | VmHWM | Probe stderr |
| --- | --- | --- | --- | --- |
| Local `jump-then-extra.ts` | 0 | 160×120, one frame | **381052 KB** | no `max pixel count`, no `get_buffer() failed` |
| `http://127.0.0.1:8766/jump-then-extra.ts` (server answers every GET with 200 and `Accept-Ranges: none`) | 0 | 160×120 | **381560 KB** | same, uncapped |
| HLS media playlist of that TS | 0 | 160×120 | **32704 KB** | no spike |
| PSI-less 12000×12000 (D2 control) | 1 | no | 86832 KB | `get_buffer() failed` |
| Extra PID appended with no PTS edit (D2 control) | 0 | 160×120 | 72516 KB | `get_buffer() failed` |
| Normal-PMT 12000×12000 (D1 control) | 1 | no | 72204 KB | `get_buffer() failed` |

A separate harness that uses the same stop (`ts - first_ts >= 7s`) and then
calls `avformat_find_stream_info` with `max_pixels` set only for streams
that already exist:

- pre-pass `read_size=4930`, `stopped_early=1`, `nb_streams=1`, `seek_rc=0`
- after `find_stream_info`: stream 0 is h264 160×120, stream 1 is h264
  **12000×12000**, VmHWM **380392 KB**, VmRSS **33928 KB**
- after `avformat_close_input`: VmRSS **33572 KB** (the picture is not kept)
- a second open in that same process: VmHWM **382000 KB**, not ~760 MB

Two player processes opening the HTTP URL at the same time: the sum of
their VmRSS peaked at **760624 KB**, both `rc 0`. Peaks add while they
overlap. They do not stick.

This is the same shape AUDIT10 called should-fix for D2, and the same
reasons it was not must-fix still hold. The process stays up, the other
tiles are not stopped by this open, nothing republishes `online:true`, and
serial retries do not accumulate. It is not a re-open of D2: the PSI-less
file and the in-window extra PID, which are what `#60` and `present_smoke`
pin, are capped (72–87 MB, and `get_buffer() failed`). `present_smoke`'s
300000 KB ceiling passes because those fixtures sit inside the pre-pass
window. The HLS extra-PID case in that test also takes the `probesize = 1`
path, which this file shows does not decode the late picture.

Why HTTP hits it: the player does not special-case MPEG-TS the way it
special-cases HLS. `av_seek_frame` returned 0 against the no-`Range` server
(FFmpeg re-reads the URL from the start), the queue was dropped, and
`find_stream_info` ran with the default `probesize`. MQTT `add` accepts
`http://` and `https://` (`src/mqtt_commands.cpp:27-31`). The open used
here is `FfmpegDecoder::open`.

A fix was not implemented and not tested. The HLS branch is the one that
already refuses to trust seek and refuses to let `find_stream_info` read
past the pre-pass; that is why the playlist row stays at 33 MB. The
seekable MPEG-TS branch needs the same refusal whenever the pre-pass
stopped early, or an equivalent way to put `max_pixels` on a decoder whose
stream index did not exist at the start of `find_stream_info`. Matching the
pre-pass stop to FFmpeg's real stop (do not treat a 7 s PTS delta as the
end of the window) would also cover this file. Playback `max_pixels` is
what still limits the 160×120 decoder. It does not run for the late PID,
because `open()` keeps the first video stream and sets `AVDISCARD_ALL` on
the rest (`src/ffmpeg_decoder.cpp:432-451`).

---

## 3. Checked, and not filed as must or should

- **HLS of the D3 file.** VmHWM 32704 KB, `rc 0`, one 160×120 frame. One
  `decode_slice_header error` on stderr, then a normal present. The
  `probesize = 1` path is doing the job `#60` added for HLS. Not a second
  finding.
- **V1, first video track.** `[repro]` `tiny-first.mp4` is two copied H.264
  tracks, 16×16 then 320×240. The player opens `stream=0` and presents
  16×16. `av_find_best_stream` on that file returns **1**. `[code]`
  `firstVideoStreamIndex` replaced `av_find_best_stream` for every container
  (`src/ffmpeg_decoder.cpp:432-436`). For this MP4, `nb_streams > 0` and the
  format is not `mpegts` or `hls`, so `formatMayAddStreams` is false and the
  pre-pass never runs. The header at `src/ffmpeg_decoder.h:19-24` now says
  open keeps the first video. That is the contract `#60` wrote down, which
  is why this is a nit and not a should-fix. A poster-first attempt with
  ffmpeg's muxer stored the attached picture as stream 1
  (`DISPOSITION:attached_pic=1`) and the feature as stream 0; the player
  presented 320×240. F5 (cover art as the only video) was not re-opened.
- **Allowlist.** No new bypass was reproduced. `present_smoke` still covers
  the HTTP playlist with a `file://` segment. Not re-tested this round, and
  not re-opened: an HTTP 302 to `file://`, a `data:` segment, `crypto+file`,
  `concat`, `subfile`.
- **Packet-queue growth.** `pushPacketQueue` reallocates to `count + 1` on
  every packet (`src/ffmpeg_decoder.cpp:187-200`). A 20000-packet PSI-less
  file of one-byte PES payloads (3760000 bytes) returned in well under a
  second with `no video stream`. That is not a hang. A probesize-sized
  queue of real PES packets was not shown to stall open, so this is not
  filed.
- **Present path, layout, MQTT command parsing.** Read against the closed
  items above. `software_renderer`, `layout_engine`, `mqtt_smoke`, and
  `stream_player` passed. No new present-spam loop and no new retained
  `online:true`.
- **Exit timings** are inside `scripts/launch-coreelec.sh`'s 10 s poll.
  `launch_coreelec` passed (54.38 s).
- **GS-King X, Amlogic hardware decode, real KMS/DRM.** Not exercised.
  `DrmRenderer` stays off unless `AMLOGIC_DRM_KMS` is set. Seen only as
  out of scope.

Carried nits, confirmed only as the same code, and not elevated: **J1**
(`Adding stream:` prints id and url raw, `src/stream_manager.cpp:71`),
**E1** (worker `catch` sets `done` and not `failed`,
`src/stream_player.cpp:257-266`), **H1** (invalid host or prefix falls back
to localhost / `amlogic-multistream`, `src/mqtt_controller.cpp:427-434`),
**U1** (`scripts/build-appimage.sh` still fetches linuxdeploy, the AppImage
plugin, and appimagetool from `continuous` with no sha256), **B1** (SIGTERM
in the startup wait still `_exit`s without DISCONNECT once `connect_async`
has queued CONNECT), **L1** (`set -- $1` in `scripts/launch-coreelec.sh:51`),
**N2** (exit `sent` is the publish return), **N3** (a full reaper also
delays a healthy reopen), **N4** (`finishProcess` always `_exit`s; the
header text is the old conditional wording), **F4** (same-id replace waits
out the old lookup), **F5** (attached picture through `mov` / HLS), **F6**
(packed-audio fixtures still ADTS), **F7** (`scripts/ctest-require.sh` still
does not require `mqtt_exit_test`).
