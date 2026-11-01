# AUDIT12: `main` @ a0c18ff after AUDIT11 D3 (#62)

Audited commit: `a0c18ff58e4f25c2d845e8309cab5cf650732f5a` (squash of
[#62](https://github.com/yyolk/amlogic-multistream/pull/62) onto `b29b44d`).
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
commit: **14/14 pass**, 122.75 s, exit 0. None skipped. Required decoder
tests ran: `ffmpeg_decoder` (2.19 s), `present_smoke` (1.84 s),
`stream_player_http` (1.65 s). The other tests: layout_engine,
stream_manager, mqtt_smoke, software_renderer (0.01 s), stream_player
(5.63 s), ctest_require (0.05 s), launch_coreelec (38.39 s), kodi_addon
(3.66 s), stream_player_stall (9.33 s), mqtt_reconnect (17.77 s),
mqtt_exit_test (42.22 s). `mqtt_exit_test`: sigterm 2011 ms rc 0, drain-cmd
2007 ms, gone-dns 20 ms, gone-blackhole 502 ms, drop-drain 2012 ms, auth
give-up rc 1 with the exit 1991 ms after the give-up line (that line was
15.8 s after the refusing broker started, which is reconnect backoff, not
the drain), startup auth rc 1, startup-dns 20 ms rc 0. Retained status on
sigterm and drain-cmd was `online:false` with the stream table kept.

---

## Verdict

**No must-fix.** AUDIT11 D3's jump-then-extra fixture is closed on this
binary (about 35 MB, not ~381). AUDIT10 D2, AUDIT9 D1, AUDIT8 P1, AUDIT7
B2, AUDIT6 N1, AUDIT5 F1 / X1 / F3, and AUDIT4 A1–A5 still hold on the
evidence below. Carried nits stay nits.

One new **should-fix**, **D4**. `#62` clamps the outer MPEG-TS probe after
a timestamp gap. It does not run for a decode that finishes inside
`avformat_open_input`. FFmpeg 6.1.1's HLS demuxer calls
`avformat_find_stream_info` on an audio-rendition playlist with a NULL
options pointer, so that segment's probe decoder keeps
`max_pixels == INT_MAX`. A master whose audio group points at the same
12000×12000 H.264 TS used since AUDIT10 peaks at about 380 MB during open
and then fails `avcodec_open2`. RSS after that open is ~34 MB. A video-only
media playlist of the same TS stays at ~90 MB with `get_buffer() failed`.

| ID | Severity | Finding |
| --- | --- | --- |
| **D4** | **should-fix** | An HLS audio rendition is probed inside `avformat_open_input` with no `max_pixels`. Local `--url` of that master: VmHWM **380176 KB**, rc 1. The same URL over HTTP: **380208 KB**, rc 1. `open_input` alone, before this player's probe cap: **379476 KB**, and stream 0 is already h264 12000×12000; VmRSS after that call is **34168 KB**. Two overlapping HTTP opens summed to **760248 KB**. A media playlist of the same TS, with no audio rendition, stays at **89568 KB**. |
| V1, J1, E1, H1, U1, B1, L1, N2–N4, F4–F7 | nit | Carried. Same code shape. No new evidence, so not elevated. |

---

## 1. Prior fixes on `a0c18ff`

| Item | Verdict | Evidence |
| --- | --- | --- |
| **D3** timestamp-gap stop keeps the queue and clamps `probesize` | **Closed** on the AUDIT11 fixture | `[repro]` Same jump-then-extra file (440484 bytes: 160×120 PMT, second video PES PTS +10 s, undeclared 12000×12000 PID 0x101). Local `--url --frames 1`: rc 0, presents 160×120, VmHWM **34724 KB**. HTTP on 127.0.0.1 (200, `Accept-Ranges: none`): rc 0, presents 160×120, VmHWM **35708 KB**. HLS media playlist of that TS: rc 0, presents 160×120, VmHWM **35604 KB**. AUDIT11's uncapped number was ~381 MB. `present_smoke` also pins `ru_maxrss < 100000` KB and passed. |
| **D2** probe `max_pixels` for streams born inside the usual probe window | **Closed** on the fixtures `#60` names | `[repro]` PSI-less 12000×12000 TS: rc 1, VmHWM **89908 KB**, `exceeds specified max pixel count 67108864` and `get_buffer() failed`; VmRSS after the probe **36176 KB**. PMT 160×120 plus that picture appended as PID 0x101, no PTS edit: rc 0, presents 160×120, VmHWM **92280 KB**, same `get_buffer() failed`. Normal-PMT 12000×12000 TS: rc 1, VmHWM **89940 KB**, `get_buffer() failed`. |
| **D1** reject before `sws_scale` | **Closed** | `[code]` `decodedFramePresentable` is still in front of `sws_scale` (`src/ffmpeg_decoder.cpp`). `[repro]` `present_smoke` passed, including its 300000 KB ceiling on the MJPEG still and the PSI-less TS. The normal-PMT row above is the same reject at probe time. |
| **P1** copy and present caps | **Closed** | `[code]` `StreamPlayer::copyOwned` still refuses `!frameAcceptable`. Suite `software_renderer` passed. |
| **B2** HTTP `--url` must not enable `file` | **Closed** | `[code]` `urlIsLocalFileSource` is unchanged. `present_smoke` still rejects an HTTP playlist whose segment is `file://` (that test passed). |
| **N1** DNS off the main thread | **Closed** | `[repro]` `mqtt_exit_test` startup-dns **20 ms**, gone-dns **20 ms**. |
| **F1 / X1** exit drain, retained offline, no `loop_stop` on that path | **Closed** | `[repro]` sigterm 2011 ms, drain-cmd 2007 ms, drop-drain 2012 ms, gone-blackhole 502 ms, retained `online:false`. `[code]` `finishProcess` still sets `closing_` under `closeMu_` with the offline publish and `_exit`s. |
| **F3** retired-decoder cap | **Closed** | `[code]` `kMaxBusyRetired = kMaxStreams` (9). Suite `stream_player_test` passed. Not re-measured with a fresh-id churn harness. |
| **A1–A5** | **Closed** | Exit, auth, and the demuxer/codec allowlists are what `mqtt_exit_test`, `ffmpeg_decoder`, and `present_smoke` still pin. Not re-opened. |

---

## 2. D4 — HLS audio rendition probes with no pixel cap

`FfmpegDecoder::open` passes `max_pixels` into the outer
`avformat_find_stream_info` only after `avformat_open_input` has returned
(`src/ffmpeg_decoder.cpp`, the call then `discoverStreamsForProbeOptions`).
`#60` and `#62` both sit in that later window. They never see a decode that
the demuxer's `read_header` already finished.

`[code]` FFmpeg 6.1.1 `libavformat/hls.c` `hls_read_header` opens every
playlist. For one that is ID3-timestamped, or whose first rendition is
audio, it then does:

```c
avformat_find_stream_info(pls->ctx, NULL);
```

(around lines 2167–2172). A NULL options pointer leaves every probe decoder
at the default `max_pixels` (`INT_MAX`). `ff_copy_whiteblacklists` copies
the player's codec whitelist onto that inner context, and `h264` is on it,
so the inner probe is allowed to decode the picture. This runs from
`avformat_open_input`, which is the HLS `read_header`.

`[repro]` Master playlist, files in one directory:

- `audio.m3u8` — one segment, the AUDIT10 12000×12000 one-frame libx264
  MPEG-TS (`ultrafast`, `stillimage`, CRF 51, 432588 bytes).
- `video.m3u8` — the 160×120 `small.ts`.
- `master-audio.m3u8` — `#EXT-X-MEDIA:TYPE=AUDIO,...URI="audio.m3u8"` and
  one `#EXT-X-STREAM-INF` with `AUDIO=` that group, pointing at
  `video.m3u8`.

| Input | rc | VmHWM | After | What the probe saw |
| --- | --- | --- | --- | --- |
| Player `--url master-audio.m3u8 --frames 1` | 1 | **380176 KB** | VmRSS **70708 KB** | stderr then hits `max pixel count 67108864` and `avcodec_open2: Invalid argument` |
| `http://127.0.0.1/master-audio.m3u8` (ordinary static file server) | 1 | **380208 KB** | VmRSS **34804 KB** | same failure |
| `avformat_open_input` only, same whitelists, no player probe cap | 0 | **379476 KB** | VmRSS **34168 KB** | stream 0 is h264 **12000×12000** before `open_input` returns |
| Player `--url huge-media.m3u8` (that TS as the only media playlist, no audio rendition) | 1 | **89568 KB** | still in the capped band | `get_buffer() failed` |
| `avformat_open_input` only of `huge-media.m3u8` | 0 | **29340 KB** | **29340 KB** | stream 0 is h264 **0×0** (not decoded yet) |
| `avformat_open_input` of a master whose extra group is `TYPE=SUBTITLES` and whose segment is the same huge TS | 0 | **29152 KB** | **29152 KB** | width stays 0. The inner `find_stream_info(NULL)` is the audio branch, not subtitles |

Two player processes opening the HTTP master at the same time: the sum of
their VmRSS peaked at **760248 KB**, both rc 1. Peaks add while they
overlap. They do not stick. `open_input` itself returns with the picture
buffer released and the dimensions left on the stream (34168 KB, width
already 12000). The player's later `avcodec_open2` is what prints the
`max_pixels` error: by then the audio playlist's video is stream 0, so the
playback context refuses it. That refusal does not undo the inner alloc.

This is the same shape AUDIT10 called should-fix for D2, and the same
reasons it is not must-fix still hold. The process stays up, the buffer is
released before open returns, nothing republishes `online:true`, and a
second open does not add another 380 MB on top of a live one. MQTT `add`
accepts `http://` and `https://` and uses this `open`. A failed open is
retried, so a broker can repeat the spike; each attempt releases it.

It is not a re-open of D2 or D3. Those fixtures stay in the 35–92 MB band
on this binary, and the single-playlist HLS of the huge TS never enters
`find_stream_info(NULL)`. The ID3-timestamped arm of the same `if` was not
given its own fixture. The audio-rendition arm was.

A fix was not implemented and not tested. The inner `find_stream_info`
needs the same `max_pixels` the outer probe already passes, or this player
has to stop an audio rendition from being probe-decoded before that call.
Clamping the outer `probesize` does not reach it.

---

## 3. Checked, and not filed as must or should

- **D3 clamp versus the next packet.** `[repro]` The jump file truncated at
  the jumped PES, then the huge PID immediately after: rc 0, presents
  160×120, VmHWM **90628 KB**, `get_buffer() failed`. A PSI-less private
  PES with a 10 s PTS gap and then that picture: rc 1, VmHWM **89780 KB**,
  same cap. The pre-pass's first `av_read_frame` already creates the late
  PID, so it is in the `max_pixels` dictionary. Not a bypass of `#62`.
- **Allowlist.** `[repro]` `--url` of `crypto+file:///etc/passwd`, a
  `data:` URL, `concat:file:///etc/passwd`, and a `subfile` URL all fail
  in `avformat_open_input` (rc 1, no present). An HTTP 302 whose
  `Location` is `file:///etc/passwd` also fails open (rc 1, no present);
  the process does not demux that file. `present_smoke` still covers the
  HTTP playlist with a `file://` segment. Not re-opened.
- **Present path, layout, MQTT command parsing.** Read against the closed
  items above. `software_renderer`, `layout_engine`, `mqtt_smoke`, and
  `stream_player` passed. No new present-spam loop and no new retained
  `online:true`.
- **Exit timings** are inside `scripts/launch-coreelec.sh`'s 10 s poll.
  `launch_coreelec` passed (38.39 s).
- **GS-King X, Amlogic hardware decode, real KMS/DRM.** Not exercised.
  `DrmRenderer` stays off unless `AMLOGIC_DRM_KMS` is set. Seen only as
  out of scope.

Carried nits, confirmed only as the same code, and not elevated: **V1**
(first video track is the contract `#60` wrote down), **J1** (`Adding
stream:` prints id and url raw), **E1** (worker `catch` sets `done` and
not `failed`), **H1** (invalid host or prefix falls back to localhost /
`amlogic-multistream`), **U1** (`scripts/build-appimage.sh` still fetches
linuxdeploy, the AppImage plugin, and appimagetool from `continuous` with
no sha256), **B1** (SIGTERM in the startup wait still `_exit`s without
DISCONNECT once `connect_async` has queued CONNECT), **L1** (`set -- $1`
in `scripts/launch-coreelec.sh`), **N2** (exit `sent` is the publish
return), **N3** (a full reaper also delays a healthy reopen), **N4**
(`finishProcess` always `_exit`s; the header text is the old conditional
wording), **F4** (same-id replace waits out the old lookup), **F5**
(attached picture through `mov` / HLS), **F6** (packed-audio fixtures
still ADTS), **F7** (`scripts/ctest-require.sh` still does not require
`mqtt_exit_test`).
