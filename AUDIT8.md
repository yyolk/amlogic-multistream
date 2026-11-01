# AUDIT8: `main` @ 0518a7e after AUDIT7 B2 (#51)

Audited commit: `0518a7ee2746d6a835841c87548a468e42b4809c` (tip of `main`).
AUDIT7 base: `25bfa8d` ([#50](https://github.com/yyolk/amlogic-multistream/pull/50), closed).
The only production change since that base is
[#51](https://github.com/yyolk/amlogic-multistream/pull/51) (`0518a7e`), which sets the
local-file protocol flag only for a scheme-less path or a `file:` / `file://`
`--url`. `git diff 25bfa8d..0518a7e -- src tests scripts CMakeLists.txt` is
those four files (`src/app.cpp`, `src/ffmpeg_decoder.h`,
`tests/ffmpeg_decoder_test.cpp`, `tests/present_smoke_test.cpp`).
`docs/research/` was not used as findings.

No production code is changed in this PR. Nothing was fixed.

**Severity:** **must-fix** = breaks a guarantee the code or docs make, on a
reachable path, in a way that leaves the player hung, retained as online, or
unbounded. **should-fix** = real, contained, worth doing before hardware work.
**nit** = polish, a known trade-off, or a test/doc gap.
**Evidence:** `[repro]` = run on this VM. `[code]` = from reading the code.
**needs-GS-King-X** = needs the box.

**Environment.** Ubuntu 24.04 x86_64, g++ 13.3, glibc 2.39, FFmpeg 6.1.1
(libavformat 60.16.100), libmosquitto / mosquitto 2.0.18, libdrm 2.4.125.
Built with `g++-13`. FFmpeg 6.1.1 `url_find_protocol` / HLS `open_url` /
`mov_open_dref` were read from the `n6.1.1` tag.

**Suite.** `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` on `0518a7e`:
**14/14 pass**, none skipped, ctest **117.0 s**. `mqtt_exit_test` (42.2 s):
sigterm 2011 ms, drain-cmd 2007 ms, gone-dns 20 ms, gone-blackhole 503 ms,
drop-drain 2013 ms, auth give-up rc 1 with `waitExitMs=1992`, startup auth
rc 1, **startup-dns 20 ms rc 0**. `present_smoke` 0.22 s (includes the B2
`--url` cases).

---

## Survivors

Must-fix: **none**. Should-fix: **none**. **B2 is closed** by #51. B1 stays a
nit. The window is the same code as `25bfa8d`; this pass did not find a worse
case. N2, N3, N4, F4, F5, F6, and F7 are unchanged in code.

| ID | Severity | Finding | From |
| --- | --- | --- | --- |
| **B1** | nit | SIGTERM in the ~20 ms after the startup helper has written CONNECT, before `MQTT running`, still `_exit`s without DISCONNECT. The empty startup will can replace the previous retained status. `online` stays false. Not re-sampled; the lines did not move | #47, carried. Demoted in AUDIT7 CROSSCHECK-A. Same rating here |
| N2 | nit | `publishRaw` returns true when `mosquitto_publish` queues, including while the socket exists and MQTT is not up, so a blackhole broker from launch still pays `kDisconnectWait`. The header still says "a live connection" | #34, reconfirmed `[code]` |
| N3 | nit | A full retired-decoder cap also delays a healthy `restart` or a new open. The gate is the same `continue` | #33, reconfirmed `[code]` |
| N4 | nit | `stream_player.h` still says process exit `_exit`s if a worker is inside the blocking call. `finishProcess` always `_exit`s | #34, reconfirmed `[code]` |
| F4 | nit | Re-adding an id whose lookup is in flight still waits that lookup out (`reaperBusy`) | carried `[code]` |
| F5 | nit | No `AV_DISPOSITION_ATTACHED_PIC` check after `av_find_best_stream`. A `mov` / HLS wrap of a cover can still present `mjpeg` | carried `[code]` |
| F6 | nit | Packed-audio fixtures in `ffmpeg_decoder_test` are still ADTS only | carried `[code]` |
| F7 | nit | `mqtt_exit_test` and `mqtt_reconnect` still exit 77 without the daemon and are not required by `scripts/ctest-require.sh`. Both ran on this VM | carried `[code]`, suite `[repro]` |

---

## Verdict

#51 closes **B2**. `--url http://…/file_seg.m3u8` and `--url hls+http://…`
whose only segment is `file:///…/secret.mp4` exit 1. strace shows no
`openat` of that file. stderr is `Protocol 'file' not on whitelist
'http,https,tls,tcp,crypto,hls'!`. The same URL shape with the flag forced
on still decodes (`ffmpeg_decoder_test`), so the playlist is a real file
segment. A scheme-less path and `file://` of the fixture still present.
A scheme-less local playlist that names a `file://` segment still decodes
that file. That is the allowance #51 kept.

F1, X1, F3, A1, A2, A3, A4, and A5 still hold on the unit tests and on the
timings above. N1's startup-dns child is still 20 ms, rc 0. This pass did
not rebuild the silent-nameserver harness. Those files did not change.

No new must-fix and no new should-fix. The B2 classifier matches FFmpeg
6.1.1's scheme scan: a remote `scheme://` URL has its colon before the first
slash, so the flag stays off, and segment opens go through
`protocol_whitelist`.

---

## 1. B2 — closed `[repro]` `[code]`

- **Where:** `urlIsLocalFileSource` (`src/app.cpp:178-188`) returns true only
  when the URL starts with `file:`, contains no `:`, or has a `/` before the
  first `:`. `decodeUrl` calls `setAllowLocalFile(true)` only then
  (`src/app.cpp:201-203`). Otherwise the whitelist is
  `http,https,tls,tcp,crypto,hls` (`src/ffmpeg_decoder.cpp:136-138`).
- **Why that matches FFmpeg.** `url_find_protocol` (`libavformat/avio.c` on
  `n6.1.1`) takes a scheme only from `URL_SCHEME_CHARS` followed by `:`.
  `/` is not a scheme character, so a slash before the first colon is the
  file protocol on both sides. `file:` is the file protocol on both sides.
  A string this function treats as remote is not opened as `file` by
  FFmpeg, and a string it treats as local is not fetched as HTTP.
  `file:http://127.0.0.1:…/file.m3u8` is a local filename: rc 1, no TCP
  connection, no `openat` of `secret.mp4`.
- **Nested opens use the same whitelist.** `io_open_default` passes
  `s->protocol_whitelist` into `ffio_open_whitelist`. HLS `open_url` allows
  `file` only after the extension check, then calls `io_open`. `mov`
  external `dref` stays off unless `enable_drefs` / `use_absolute_path`
  (both default 0). The player does not set either option.

Fixture: 64×48 MPEG-4 `secret.mp4`, playlist served on `127.0.0.1` with one
segment URL. Player: `--url <url> --frames 1`. `strace -f -e trace=openat,connect`.

| `--url` | Result |
| --- | --- |
| `http://` playlist, segment `file:///…/secret.mp4` | rc 1, no `openat`. Log: `Protocol 'file' not on whitelist 'http,https,tls,tcp,crypto,hls'!` |
| `hls+http://` same playlist | rc 1, no `openat`, same whitelist line |
| segment `file:/…` (one slash), `FILE://`, `file://localhost/…`, `crypto+file://`, `crypto:file://`, `hls+file://`, `subfile,,…`, `concat:file://`, `pipe:0`, `data:`, leading space, `file://…?.mp4`, percent-encoded `file%3A%2F%2F…` | rc 1, no `openat` of `secret.mp4` |
| segment is the absolute path `/tmp/…/secret.mp4`, or a long `../` chain to that path | rc 1, no `openat`. The player HTTP-GETs that path on the playlist host (404). It is an HTTP URL, not a local open |
| `#EXT-X-MAP` URI `file://…/secret.mp4` | rc 1, `Invalid argument`, no `openat` |
| `#EXT-X-KEY` URI `file://…/secret.mp4`, media segment `ok.ts` | rc 1, no `openat`. The key open is refused. `ok.ts` is fetched |
| master variant URI `file://…/secret.mp4` | rc 1, `Invalid argument`, no `openat` |
| `#EXT-X-MEDIA` URI `file://…` plus a real `ok.m3u8` variant | rc 0, decodes the HTTP variant. Whitelist line for `file`. No `openat` of `secret.mp4` |
| HTTP 302 `Location: file://…/secret.mp4` | rc 1, no `openat` |
| `http://` and `hls+http://` playlist of `ok.ts` | rc 0, `codec` open, `Decoded` |
| `file://…/secret.mp4`, scheme-less path, scheme-less path whose name contains `:` (`se:cret.mp4`) | rc 0, `openat`, `Decoded` |
| scheme-less `local.m3u8` whose segment is `file://…/secret.mp4` | rc 0, `openat`, `Decoded`. Local playlist, flag on. The allowance #51 kept |
| `FILE:///…/secret.mp4` and `HTTP://…/file.m3u8` | rc 1, `Protocol not found`. Fail closed. FFmpeg's protocol match is case-sensitive, same as MQTT's `http://` / `https://` check |

`https://` and `hls+https://` were not given a TLS listener. Both have the
colon before the first slash, so `urlIsLocalFileSource` is false, and the
whitelist string does not gain `file`. The `hls+https://` probe died in the
handshake (`End of file`) before a playlist was read.

`ffmpeg_decoder_test` opens the same HTTP and `hls+http` playlist with the
flag left off (`CHECK(!open)`) and again with the flag forced on (one
YUV420P frame). `present_smoke` rejects `--url` for `http://` and
`hls+http://` and still presents `file://` of the fixture. Those tests
passed inside the suite.

---

## 2. Prior closed items on `0518a7e`

#51 does not touch MQTT, the reaper, or the demuxer/codec lists. The suite
is the reconfirmation. The silent-resolver, launch-script, and reaper-cap
harnesses from AUDIT7 were not rebuilt.

| Item | Fixed by | Verdict | Evidence |
| --- | --- | --- | --- |
| **B2** `--url http` playlist opens `file://` segments | #51 | **Closed** | Section 1 |
| **N1** pre-connect `getaddrinfo` holds SIGTERM | #47 | **Closed** | `[repro]` `startup-dns` 20 ms rc 0. `[code]` helper still calls `mosquitto_connect_async` (`src/mqtt_controller.cpp:239`); main stays in `sigtimedwait` (`:262-272`) |
| **F1** command during the drain republishes `online:true` | #34 | **Closed** | `[repro]` drain-cmd 2007 ms, retained `online:false` with `cam` |
| **X1** exit joins the mosquitto thread | #34 | **Closed** | `[repro]` gone-dns 20 ms, gone-blackhole 503 ms, drop-drain 2013 ms. `[code]` `finishProcess` still never calls `mosquitto_loop_stop` (`src/mqtt_controller.cpp:334-358`) |
| **F3** fresh-id churn stacks decode threads | #33 | **Closed** | `[repro]` `stream_player_test` passed. `[code]` cap still `kMaxBusyRetired` (`src/stream_player.cpp:450-458`) |
| **A1** offline publish is queued before the drain | #25 / #34 | **Closed** | `[repro]` sigterm 2011 ms, retained offline status still lists `cam` |
| **A2** auth give-up exits 1 inside the drain bound | #24 / #34 | **Closed** | `[repro]` `waitExitMs=1992` rc 1. Startup refusal rc 1 |
| **A3** `restart` does not stack opens on one stuck id | #29 | **Closed** | `[repro]` `stream_player_test` passed. `[code]` `reaperBusy` still at `src/stream_player.cpp:444-446` |
| **A4** top-level `mp3` / `aac` rejected | #28 | **Closed** | `[code]` `kTopLevelFormatWhitelist` unchanged. `ffmpeg_decoder_test` passed, including `testRejectCoverMp3` |
| **A5** `mp3float` stays on the codec allowlist | #28 | **Closed** | `[code]` `kCodecWhitelist` still lists `mp3float` (`src/ffmpeg_decoder.cpp:42-43`) |

---

## 3. Carried nits

Not re-promoted. #51 does not touch them. No new measurement moved any of
them.

- **B1** `[code]`. After `mosquitto_connect_async` returns, the helper stores
  `connectReady` (`src/mqtt_controller.cpp:239-242`). A signal during the
  20 ms `sigtimedwait` (`kTickWaitNs`, `:98`, `:262-272`) `_exit`s without
  reading the flag again and without DISCONNECT. libmosquitto 2.0.18 still
  writes CONNECT, will included, inside that `connect_async` when TCP
  connects immediately. The lookup path still has not written CONNECT
  (`startup-dns` has no `MQTT running`). The post-CONNECT window was not
  re-sampled. It is the same size as AUDIT7. `online` stays false. The
  preserving fix is still to delay `mosquitto_connect_async` until the main
  thread commits to `loop_start`.
- **N2** `[code]`. `publishRaw` (`src/mqtt_controller.cpp:564-579`) returns
  true whenever `mosquitto_publish` returns success. `src/mqtt_controller.h:69-71`
  still says that means a live connection. `finishProcess` waits
  `kDisconnectWait` (500 ms) when `sent` is true (`:345-352`).
- **N3** `[code]`. `busyRetired >= kMaxBusyRetired` runs before every new
  open, including `restart` (`src/stream_player.cpp:450-458`).
- **N4** `[code]`. `src/stream_player.h:31-34` still ties `_exit` to a worker
  that is inside the blocking call. `finishProcess` always `_exit`s
  (`src/mqtt_controller.cpp:358`). `exitIfBusy` is still test-only.
- **F4** `[code]`. `reaperBusy` (`src/stream_player.cpp:444-446`) still skips
  the replacement open until the retired worker returns.
- **F5** `[code]`. `av_find_best_stream` (`src/ffmpeg_decoder.cpp:187`) is
  still the only video pick. No attached-picture check.
- **F6** `[code]`. `testAdtsAudioMaster` is the packed-audio fixture. No
  AC-3, E-AC-3, or MP3 rendition.
- **F7** `[code]`. `scripts/ctest-require.sh:5-9` still requires only
  `ffmpeg_decoder`, `present_smoke`, and `stream_player_http`. On this VM
  mosquitto was installed, so `mqtt_exit_test` and `mqtt_reconnect` ran and
  passed.

---

## 4. Considered, not findings

- **Scheme-less local playlist with a `file://` segment.** Decodes. The user
  passed a local path. `docs/mqtt.md` says `file` is only on the `--url`
  local-file path. That sentence matches #51.
- **Absolute-path and `../` segments of an HTTP playlist.** Resolved onto the
  HTTP origin and 404. They do not become `openat`.
- **`FILE://` and `HTTP://`.** Unknown protocol. Fail closed.
- **`data:`, `pipe:`, `concat:`, `subfile`, `crypto+file://`, `hls+file://`.**
  Not on the whitelist, and HLS `open_url` rejects a segment protocol other
  than `file`, `http`, and `data` before `io_open`. None opened `secret.mp4`.
- **MOV `dref`.** Defaults keep external tracks closed. Not exercised with a
  crafted atom. The option is off, so this is not a bypass of #51.
- **`tcp` / `tls` on the whitelist.** Unchanged. A remote playlist can still
  name `http://127.0.0.1:…`. That was true before #51 and is the point of
  playing a remote URL.
- **B1's empty will versus `finishProcess`.** Already settled as a nit. No
  new timing, no new retained `online:true`.

---

## 5. Blocked on GS-King X

- Which MP3 decoder name CoreELEC's FFmpeg registers first (A5).
- F3 / N3: armv7 address space at the cap.
- Hardware decode and DRM/KMS present. `docs/research/` was not reviewed.
- B2's whitelist behavior was measured with FFmpeg 6.1.1 on this host. The
  same `protocol_whitelist` string is what CoreELEC's libavformat will see.
  A different FFmpeg that ignores the option, or that opens a segment with
  `open()` instead of `io_open`, would need a run on the box.

---

## 6. How this was checked

- **Suite:** `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` on
  `0518a7e`. 14/14 passed, ctest 117.0 s. Host packages: `libmosquitto-dev`
  2.0.18, `libdrm-dev` 2.4.125, FFmpeg 6.1.1, `mosquitto` 2.0.18, g++ 13.3.
- **B2 probes:** `ffmpeg` lavfi fixture `secret.mp4` (mpeg4, 64×48, 0.4 s)
  and `ok.ts`. A one-shot HTTP server on `127.0.0.1`. `strace -f -e
  trace=openat,connect` of `build/amlogic-multistream --url … --frames 1`.
  `openat` of `secret.mp4` is the local-file signal. HTTP access logs
  separate a path-absolute segment from a filesystem open.
- **FFmpeg 6.1.1 source** (`n6.1.1`): `libavformat/avio.c`
  (`url_find_protocol`, `ffurl_connect`), `libavformat/options.c`
  (`io_open_default`), `libavformat/hls.c` (`open_url`,
  `allowed_extensions`), `libavformat/mov.c` (`mov_open_dref`,
  `enable_drefs` default 0).
- **Diff:** `git diff 25bfa8d..0518a7e -- src tests scripts CMakeLists.txt`.

---

## Agreed survivors

This pass has not been cross-checked. The survivors table above is the
rating before that. Nothing here was fixed.

| ID | Severity | Summary |
| --- | --- | --- |
| — | — | Pending cross-check |
