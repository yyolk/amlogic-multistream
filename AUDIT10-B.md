# AUDIT10-B: host path on `main` @ 5c2c9d9 after #57

Independent second look of the host/software path. Audited commit:
`5c2c9d910efd5a0fe36d987695c5f717a7ebc1c1` (`main`, squash of
[#57](https://github.com/yyolk/amlogic-multistream/pull/57), AUDIT9 D1).

This pass did not read AUDIT10-A and did not change production code.
Nothing was fixed. Probes ran against this tree and the
`amlogic-multistream` binary built by the suite below.

**Severity:** **must-fix** = a reachable path hangs the process, leaves it
retained as online, or grows without the bound the code already claims.
**should-fix** = real, contained, worth doing before hardware work.
**nit** = polish, a known trade-off, or a test/doc gap. **drop** = checked,
not a finding.

**Environment.** Ubuntu 24.04 x86_64, g++ 13.3.0, FFmpeg 6.1.1-3ubuntu5
(libavformat 60.16.100), libmosquitto / mosquitto 2.0.18, libdrm 2.4.125.
`docs/research/**` was not in scope. Amlogic hardware decode and real DRM
were not exercised.

**Suite.** `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` on `5c2c9d9`:
**14/14 pass**, none skipped. CTest real time **123.03 s**. Required decoder
tests (`ffmpeg_decoder`, `present_smoke`, `stream_player_http`) ran and
passed. `stream_player_stall` 9.36 s, `mqtt_reconnect` 17.70 s,
`launch_coreelec` 39.02 s, `mqtt_exit_test` 42.26 s.

---

## Verdict

**No new must-fix.** The first pass also found no should-fix. **CROSSCHECK-B
revises that:** **D2** is a should-fix. #57 closes **D1** for a stream that
already exists when the probe starts. A frame whose pixel count is over
`8192²` is refused by FFmpeg `max_pixels` before a converted buffer exists,
and a frame that fails the extent or stride cap is refused in
`publishYuv420` before `av_frame_get_buffer` / `sws_scale`. Retries of that
open do not accumulate RSS. Copy, `--url`, and present still share
`SoftwareRenderer::frameAcceptable`.

AUDIT8 P1, AUDIT7 B2, AUDIT6 N1, AUDIT5 F1/X1/F3, and AUDIT4 A1–A5 still hold
on this tree. J1, E1, H1, U1, B1, and L1 stay nits. No new evidence moved
them.

| ID | Severity | Finding |
| --- | --- | --- |
| — | must-fix | None. |
| **D2** | should-fix | Revised in CROSSCHECK-B. Streams created during the probe miss `max_pixels`. |
| **D1** | closed | #57. Over-pixel stills fail closed. See below. |
| J1 | nit | Control characters in `id` / `url` still reach the add log. Retained JSON is still escaped. Not re-run. |
| E1 | nit | A decode-thread exception still sets `done` and leaves `failed` clear (`src/stream_player.cpp`). |
| H1 | nit | A bad `--mqtt-host` or topic prefix still falls back to `localhost` / `amlogic-multistream` (`src/mqtt_controller.cpp`). |
| U1 | nit | `scripts/build-appimage.sh` still fetches linuxdeploy and appimagetool from GitHub `continuous` with no checksum. |
| B1 | nit | SIGTERM in the startup window after CONNECT is written and before `MQTT running` still `_exit`s without DISCONNECT. `online` stays false. Not re-sampled. |
| L1 | nit | `normalize_abs` in `scripts/launch-coreelec.sh` still pathname-expands glob characters. Not re-run. |
| N2 | nit | `publishRaw` still returns true when `mosquitto_publish` queues. |
| N3 | nit | A full retired-decoder cap still delays a healthy open. |
| N4 | nit | `stream_player.h` still describes `_exit` as conditional on a blocked worker. `finishProcess` always `_exit`s. |
| F4 | nit | Re-adding an id whose lookup is in flight still waits on `reaperBusy`. |
| F5 | nit | No attached-picture check. A small cover can still be the selected video. A 12000×12000 cover is now the cheap D1 failure, not a new case. |
| F6 | nit | Packed-audio fixture is still ADTS. |
| F7 | nit | `mqtt_exit_test` and `mqtt_reconnect` still are not in `scripts/ctest-require.sh`. Both ran here because mosquitto is installed. |

---

## D1 — closed `[repro]` `[code]`

`open()` sets `max_pixels` to `SoftwareRenderer::kMaxExtent` squared
(67108864) on the playback context and on each `avformat_find_stream_info`
probe dictionary (`src/ffmpeg_decoder.cpp`). `publishYuv420` calls
`decodedFramePresentable` before `av_frame_get_buffer` and `sws_scale`.
That helper uses the same extent and stride limits as
`SoftwareRenderer::frameAcceptable`.

`ru_maxrss` from a throwaway driver of `FfmpegDecoder::open` + one
`readFrame` (local-file allowance on):

| Fixture | `open` | `readFrame` | Peak RSS | Notes |
| --- | --- | --- | --- | --- |
| MJPEG MP4 12000×12000 | true | false | **32180 KB** | stderr: `max_pixels` 67108864, then `avcodec_send_packet: Invalid argument`. No `-> yuv420p`. Six opens in one process stay at 32180 KB |
| HLS master and media playlist whose audio rendition is that MP3+APIC | true | false | **35656 KB** / **34044 KB** | Same `max_pixels` line. Selected stream is `mjpeg` (F5's pick) and still fails closed |
| Top-level `cover.mp3` | false | — | 30888 KB | `demuxer is not allowed: mp3` (A4) |
| MJPEG MP4 16384×4096 (exactly 8192² pixels, width over 8192) | true | false | 129920 KB, flat across six opens | `decoded frame 16384x4096 cannot be presented` before convert. Same pixel budget as an allowed frame; see considered |
| MJPEG MP4 4096×8192 | true | true, 4096×8192 | 130352 KB | In cap. `--url` not required; `readFrame` returned the frame |
| MJPEG MP4 2×2, 14×14 (from a 15×15 encode), 30×32, 8190×32 | true | true | under 33 MB | `--url --frames 1` of each exits 0 and prints `presented 0` |

The 8224×32 and 8192×32 H.264 cases are the suite's `ffmpeg_decoder` /
`present_smoke` checks (`8224x32 reject before convert`, `8192x32
presentable`). Both passed.

---

## Prior closed items on `5c2c9d9`

#57 touches the decoder convert path and the tests around it. The protocol
flag, the reaper, and the exit path are the same code the last rounds closed.
This suite re-ran them.

| Item | Fixed by | Verdict | Evidence |
| --- | --- | --- | --- |
| **D1** over-pixel frame converted, then retried | #57 | **Closed** | Section above |
| **P1** wide frame copied, then `present` failed every tick | #54 | **Closed** | `[code]` `copyOwned` still requires `frameAcceptable` (`src/stream_player.cpp`). Suite `stream_player_test` / `present_smoke` passed. In-cap odd MJPEG sizes present; the 16384-wide frame never reaches `present` |
| **B2** `--url http` playlist opens `file://` segments | #51 | **Closed** | `[code]` `urlIsLocalFileSource` (`src/app.cpp`) is unchanged. Suite `ffmpeg_decoder` still covers `http://` and `hls+http://` file segments |
| **N1** pre-connect `getaddrinfo` holds SIGTERM | #47 | **Closed** | `[repro]` `startup-dns` 20 ms, rc 0. `[code]` helper still calls `mosquitto_connect_async`; main stays in `sigtimedwait` |
| **F1** command during the drain republishes `online:true` | #34 | **Closed** | `[repro]` drain-cmd 2007 ms, retained `online:false` with `cam` |
| **X1** exit joins the mosquitto thread | #34 | **Closed** | `[repro]` gone-dns 20 ms, gone-blackhole 502 ms, drop-drain 2009 ms. `[code]` `finishProcess` still never calls `mosquitto_loop_stop` |
| **F3** fresh-id churn stacks decode threads | #33 | **Closed** | `[repro]` `stream_player_test` passed, including the cap cases. `[code]` `busyRetiredCount` gate unchanged |
| **A1** SIGTERM with a stuck worker keeps the table | #30 | **Closed** | `[repro]` sigterm 2010 ms, retained `online:false` and `cam` |
| **A2** auth give-up exits 1 inside the same bound | #30 | **Closed** | `[repro]` `waitExitMs=1990`, rc 1. Startup auth rc 1, `waitExitMs=0` |
| **A3** restart during a stuck lookup does not stack | #29 | **Closed** | `[code]` `reaperBusy` still runs before a replacement open. Suite passed |
| **A4** top-level packed audio rejected | #28 | **Closed** | `[repro]` `cover.mp3` → `demuxer is not allowed: mp3`. Suite still rejects `song.mp3` |
| **A5** `mp3float` on the codec whitelist | #28 | **Closed** | `[code]` `kCodecWhitelist` still lists `mp3,mp3float` |

---

## Carried nits — no new evidence

Not re-promoted. #57 does not change their code.

- **J1, E1, H1, U1, B1, L1.** Same sites as AUDIT9. B1 was not re-sampled;
  the pre-`mosquitto_loop_start` `_exit` is unchanged. L1's `set -- $1` in
  `normalize_abs` is unchanged.
- **N2, N3, N4, F4.** `[code]` `publishRaw`, the retired-decoder `continue`,
  the header comment, and `reaperBusy` are unchanged.
- **F5.** `[code]` `av_find_best_stream` is still the only video pick. The
  HLS wrap of a 12000×12000 APIC selects `mjpeg` and then takes the D1
  failure. A small cover is still presented. That does not get worse.
- **F6, F7.** `[code]` The packed-audio fixture is still the ADTS master.
  `scripts/ctest-require.sh` still requires only `ffmpeg_decoder`,
  `present_smoke`, and `stream_player_http`.

---

## Considered, not findings

- **Extent versus pixel product.** `max_pixels` is `8192*8192`, and FFmpeg
  rejects only when `width*height` is greater than that. A 16384×4096 MJPEG
  is equal to the product, so the decoder allocates, then
  `decodedFramePresentable` rejects it before `sws_scale`. Peak RSS
  **129920 KB**, and six opens in one process stay there. A 4096×8192 frame,
  which is inside both caps, peaks at **130352 KB** and is returned. The
  wide frame's cost is the in-cap frame the player already accepts, and the
  worker is a normal read failure with the 1 s retry. The source comment
  already says an 8224×32 frame is under the pixel cap and is rejected by
  extent before convert. Not a second D1.
- **HLS audio rendition probe.** FFmpeg 6.1.1 `hls.c` calls
  `avformat_find_stream_info` with a NULL options array for an ID3 or audio
  rendition. A 12000×12000 APIC on that path still logs `max_pixels`
  67108864 and stays under 36 MB. The parent playback context is capped.
- **MJPEG even sizes.** A 15×15 encode is returned as 14×14, 31×32 as 30×32,
  8191×32 as 8190×32. `--url --frames 1` presents each. That rounding is the
  codec, and the presented size is still inside the cap.
- **Retry thrash and thread cap.** The pixel-cap failure and the extent
  failure both end the worker. RSS does not climb across opens. The reaper
  gate is unchanged, and `stream_player_test` passed.
- **SIGTERM after a live session.** `mqtt_exit_test` sigterm / drain-cmd /
  auth give-up match the 2 s drain. A separate player against mosquitto,
  sent SIGTERM after `MQTT running`, retained `online:false` with the stream
  still listed.
- **Allowlist shapes already closed** (absolute-path segments, `FILE://`,
  `crypto+file`, `concat`, `subfile`, `hls+file`, 302 to `file://`). The
  whitelist string and `urlIsLocalFileSource` did not change. Not re-run.
- **Startup banner torn with `MQTT connected`.** The main thread's
  multi-insertion `MQTT running` line and the mosquitto thread's connect
  line can share `std::cout`. One launch spliced `MQTT connected` into that
  banner. Status publishes are a single insertion, and the retained payload
  from that run was intact. Log cosmetic. Not a retained-online break.
- **`~StreamPlayer` joins whatever is still on the reaper.** The header says
  a worker inside `getaddrinfo` is not joined there. MQTT shutdown never
  reaches the destructor: `finishProcess` `_exit`s. An exception from `tick`
  would. The throw in reach is `bad_alloc` on an already-failing allocation.
  Not promoted.
- **Playlist byte size, `tcp` on the protocol whitelist, PPM files under
  `--render-dir`.** Unchanged, and still the same class as "the URL the
  operator added is large" or an opt-in debug directory. HLS segment
  protocols other than `http` / `file` / `data` are still rejected inside
  FFmpeg 6.1.1 `open_url` before the whitelist.

---

## Blocked on GS-King X

- Which MP3 decoder name CoreELEC's FFmpeg registers first (A5).
- F3 / N3 in a 32-bit address space.
- Hardware decode and DRM/KMS present. `docs/research/` was not reviewed.
- `max_pixels` and `sws_scale` were measured with FFmpeg 6.1.1 on this host.

---

## How this was checked

- **Suite:** `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` on
  `5c2c9d9`. 14/14 passed. Host packages: g++ 13.3.0, FFmpeg 6.1.1,
  libmosquitto 2.0.18, libdrm 2.4.125, mosquitto 2.0.18.
- **D1 probes:** lavfi MJPEG fixtures (12000×12000, 16384×4096, 4096×8192,
  and the odd sizes above), plus an HLS master/media playlist whose segment
  is an MP3 with that 12000×12000 JPEG as an attached picture. Driver linked
  `FfmpegDecoder` and printed `getrusage` `ru_maxrss`. `--url --frames 1`
  on the in-cap odd files. `ffmpeg_decoder` / `present_smoke` cover 8224×32
  and 8192×32 H.264.
- **FFmpeg 6.1.1 source read for this pass:** `libavformat/demux.c`
  (`avformat_find_stream_info`, `orig_nb_streams` option bounds) and
  `libavformat/hls.c` (`open_url`, the audio-rendition
  `avformat_find_stream_info(..., NULL)` call).
- **Not re-built:** silent nameserver, B1's post-CONNECT sample, L1's glob
  sample, the fresh-id churn RSS harness. Those files did not change. The
  unit tests that stand in for them passed.

---

## Severity table

| ID | Severity | Status on `5c2c9d9` |
| --- | --- | --- |
| — | must-fix | None found. |
| D2 | should-fix | Revised in CROSSCHECK-B. Probe of a stream born inside `find_stream_info` peaks near 379 MB, then releases it. |
| D1 | closed by #57 | Over-pixel MJPEG (MP4 and HLS APIC) fails with `max_pixels` 67108864. Peak RSS about 32–36 MB, flat across retries. No convert line. |
| P1 | closed by #54 | Copy and present still share `frameAcceptable`. |
| B2 | closed by #51 | Local-file flag still limited to scheme-less and `file:` URLs. Suite file-segment cases passed. |
| N1 | closed by #47 | `startup-dns` 20 ms, rc 0. |
| F1, X1 | closed by #34 | drain-cmd retained `online:false`. Exit does not join the mosquitto thread. |
| F3 | closed by #33 | Reaper cap unchanged. `stream_player_test` passed. |
| A1, A2 | closed by #30 | sigterm 2010 ms rc 0 with the stream kept. Auth give-up rc 1, `waitExitMs=1990`. |
| A3 | closed by #29 | `reaperBusy` before a replacement open. |
| A4, A5 | closed by #28 | Top-level MP3 rejected. `mp3float` still listed. |
| J1, E1, H1, U1, B1, L1 | nit | Carried. Not elevated. |
| N2, N3, N4, F4, F5, F6, F7 | nit | Carried. Not elevated. |

---

## CROSSCHECK-B

Peer draft AUDIT10-A ([#59](https://github.com/yyolk/amlogic-multistream/pull/59))
files **D2** as should-fix. Checked here on the same tip (`5c2c9d9`), against
this tree's `amlogic-multistream --url … --frames 1` and a throwaway
`FfmpegDecoder` driver. **Agree: D2 is a should-fix.** It is not a must-fix.
The first pass did not open a stream that `avformat_find_stream_info` creates,
so it did not see this spike. No production change and no fix.

`open()` builds the per-stream `max_pixels` dictionary only when
`fmt_->nb_streams > 0` (`src/ffmpeg_decoder.cpp:216-229`). FFmpeg 6.1.1
snapshots `orig_nb_streams` at the start of `avformat_find_stream_info` and
applies those options only to indexes below that count ("new streams might
appear, no options for those" in `libavformat/demux.c`). A stream added
during the probe is decoded at the default `max_pixels` of `INT_MAX`.
`12000×12000` is under that default, so the probe allocates the picture.
The playback context still sets `codec_->max_pixels`
(`src/ffmpeg_decoder.cpp:296-303`), and `avcodec_open2` then fails. #57
still holds for a stream that exists before the probe: the same picture in
a normal single-program TS logs `get_buffer() failed` and peaks at
**89492 KB**.

| Input | After `open_input` | After the probe | Player | `ru_maxrss` |
| --- | --- | --- | --- | --- |
| PSI-less H.264. PES payloads of 16000 bytes, no PAT/PMT/SDT, 437100 bytes, PID 0x100, 12000×12000 | `nb_streams=0` | 1× h264 12000×12000 | rc 1, `avcodec_open2` invalid. No `get_buffer() failed` | **378944 KB** |
| HLS media playlist of that file | `nb_streams=0`, `iformat=hls` | 1× h264 12000×12000 | rc 1, same | **379296 KB** |
| PMT PID 0x100 is 320×240. PID 0x101 is that 12000×12000 picture and is not in the program. 438604 bytes | 1× h264 | stream 0 becomes 320×240, stream 1 is h264 12000×12000 | **rc 0**, presents 320×240 | **380448 KB** |

After the PSI-less probe, VmRSS was **33684 KB** and VmHWM **378556 KB**.
Three serial opens in one process stayed at VmRSS ~34 MB and VmHWM
**378908 KB**. Two overlapping opens reached VmHWM **728496 KB**, then
VmRSS **37020 KB**. The picture is not retained. A failed MQTT open is
`WorkerFail::Open`, and `tick` waits `kRetryDelay` (1 s) before the next
`open()` (`src/stream_player.cpp:16`, `:518-525`), so each retry repeats
the spike. That does not leave a retained `online:true`, and it is not a
hang. The extra-PID file stays open and presents the PMT stream, so it
spikes once per open or restart.

One PSI-less shape did not allocate. ffmpeg's mpegts muxer omits the video
PES length, and this demuxer's default `max_packet_size` is 204800. On
this solid-red frame the first packet is ~200 KB. Probing that whole
buffer ties `h264` with `gsm` (both score 51 once the buffer passes 60 KB),
so the codec stays unset and the player fails with "no video stream" at
**32872 KB**. A PES short enough to commit `h264` before that tie takes
the uncapped path. That is the table above, and it is D2.
