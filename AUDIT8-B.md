# AUDIT8-B: host player on `main` @ 0518a7e (after AUDIT7 B2 / #51)

Audited commit: `0518a7ee2746d6a835841c87548a468e42b4809c`
(“Reject HTTP --url playlists with file:// segments (AUDIT7 B2) (#51)”).

Independent second look of the host player. `docs/research/**` was not in
scope. No production code is changed in this PR. Nothing was fixed. Probes
ran against the `0518a7e` binary built by the suite below.

**Severity:** **must-fix** = breaks a guarantee the code or docs make, on a
reachable path. **should-fix** = real, contained, worth doing before hardware
work. **nit** = polish or a narrow footgun. **drop** = checked, and it does
not hold.
**Evidence:** `[repro]` = run on this VM. `[code]` = from reading the code.

**Environment.** Ubuntu 24.04 x86_64, g++ 13.3.0, FFmpeg 6.1.1 (libavformat
60.16.100), libmosquitto 2.0.18, libdrm 2.4.125. On-device Amlogic hw/DRM was
not exercised.

**Suite.** `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` on `0518a7e`:
**14/14 pass**, 0 failed, real time 132.61 s. Required decoder tests passed
and were not skipped: `ffmpeg_decoder`, `present_smoke`, `stream_player_http`.
`mqtt_exit_test` (mosquitto present): sigterm 2013 ms rc 0, drain-cmd 2008 ms
rc 0, gone-dns 20 ms rc 0, gone-blackhole 502 ms rc 0, drop-drain 2013 ms rc 0,
auth give-up `waitExitMs=1991` rc 1 (`observedMs=15799` is time until give-up),
startup auth rc 1, startup-dns 20 ms rc 0.

---

## Verdict

**No must-fix.** #51 still holds: an HTTP `--url` playlist cannot open a
`file://` segment, and the same whitelist string is what MQTT uses. The new
item worth doing before hardware work is **P1**: a source frame wider than
8192 and still inside the copy caps is fully decoded, then
`SoftwareRenderer::present` rejects every frame and `StreamPlayer::tick`
leaves the worker running. Height above 8192 takes the one-second retry
instead. **L1** is a nit in `normalize_abs`.

Prior closed rounds stay closed. AUDIT7 B1 (SIGTERM in the startup window
after CONNECT is written and before the MQTT loop is running) was not
re-opened: this pass found no new evidence that the retained status becomes
`online:true`. N2–N4 and F4–F7 were not promoted.

| ID | Severity | Finding |
| --- | --- | --- |
| **P1** | **should-fix** | Width in (8192, 65536] with height ≤ 8192 is decoded and copied, then present fails on every tick with no backoff. Height > 8192 fails the copy and retries after 1 s. `--url` exits at viewport init instead. |
| L1 | nit | `normalize_abs` pathname-expands glob characters in `PLAYER_EXTRACT_DIR` and the player path against the current directory. |

---

## P1 — oversized source width hot-fails present

`SoftwareRenderer` caps the viewport at 8192 on each side, and `docs/mqtt.md`
says the same for `--viewport`. The present path applies that cap to the
**source** frame as well:

```198:200:src/software_renderer.cpp
    if (frame.width <= 0 || frame.height <= 0 || frame.width > kMaxExtent || frame.height > kMaxExtent) {
        std::cerr << "SoftwareRenderer::present: frame size must be 1.." << kMaxExtent << "\n";
        return false;
```

`kMaxExtent` is 8192 (`src/software_renderer.cpp:12`). The blit itself is
bounded by the destination cell: `fitInside` then `blitYuv420` walks
`drawn.width` × `drawn.height` (`src/software_renderer.cpp:217-218`,
`122-141`). A frame that has already been copied could be scaled into a
640×360 tile.

The copy cap is wider than the present cap on **width** and equal on
**height**:

```16:18:src/stream_player.cpp
constexpr std::chrono::milliseconds kRetryDelay{1000};
constexpr int kMaxPlaneStride = 65536;
constexpr int kMaxPlaneRows = 8192;
```

`copyOwned` rejects a plane whose `linesize` exceeds 65536 or whose row count
exceeds 8192 (`src/stream_player.cpp:36-38`). A Y plane of width 8224 and
height 32 passes. A height of 8224 fails the row check, the worker sets
`WorkerFail::Read` and returns (`src/stream_player.cpp:219-228`), and `tick`
retires it with a 1 s backoff (`src/stream_player.cpp:524-529`).

Present failure does not retire and does not set `retryAfter`:

```499:505:src/stream_player.cpp
        if (haveFrame) {
            session.worker->cv.notify_all();
            const LayoutEngine::Rect& dest = layout.tiles[i].rect;
            const DecodedFrame view = taken.view();
            if (!renderer_.present(view, dest)) {
                std::cerr << "StreamPlayer: present failed for " << info.id << "\n";
                continue;
```

The worker is woken, reads the next frame, and the present thread (20 ms
tick, `kTickWaitNs` in `src/mqtt_controller.cpp:98`) logs two lines per frame
for as long as the stream keeps producing. There is no `loggedRefusal` for
this path. The decoder has already allocated and converted: `publishYuv420`
calls `av_frame_get_buffer` and `sws_scale` with only a positive-size check
(`src/ffmpeg_decoder.cpp:288-317`). FFmpeg’s default `max_pixels` is
`INT_MAX`, and this player does not set it.

`--url` is a different failure. `decodeUrl` sizes the viewport from the first
frame (`src/app.cpp:249-254`), so an oversized frame fails `init` and the
process returns 1. It does not loop.

**Why should-fix.** The player stays up, and other tiles still tick, so this
is not a broken exit or allowlist guarantee. It is a permanent miss plus a
per-frame stderr flood and a full decode/copy of a frame the renderer will
not draw. A live input in this size band never hits EOF, so it never takes
the “will retry” path. Worth closing before a DRM presenter copies the same
source-size check.

**Fix shape (not done).** Reject width or height above the renderer cap inside
`FfmpegDecoder` before `av_frame_get_buffer` (and cap the codec’s
`max_pixels` so the decoder does not allocate first). Treat a present failure
like a read failure: one log line, retire, 1 s backoff. Keep the 8192
viewport cap.

### `[repro]`

Broker `127.0.0.1:18883`, viewport **640×360**, media from
`http://127.0.0.1:8765/`. `wide_long.mp4` is 8224×32, 20 frames, 2 s.
`tall_long.mp4` is 32×8224, 20 frames. Both are h264, served with 200, opened
twice (the retry).

| Stream | Present failures | “will retry” | What was logged |
| --- | --- | --- | --- |
| wide | **25** | 1 (between the two opens) | `SoftwareRenderer::present: frame size must be 1..8192` and `StreamPlayer: present failed for wide`, then `FfmpegDecoder: video 8224x32 yuv420p -> yuv420p` on each open. The 20-frame file was rejected frame by frame, then reopened. |
| tall | **0** | 2 | `FfmpegDecoder: video 32x8224 yuv420p -> yuv420p` and `StreamPlayer: no frame from tall (will retry)`. |

`--url` on the same binary, `--frames 2`:

- `/tmp/audit8b/wide2.mp4` (8224×16): prints `frame 0 8224x16`, then
  `SoftwareRenderer::init: viewport must be 1..8192 on each side`, **rc 1**.
  No present line.
- `/tmp/audit8b/wide3.mp4` (16384×64): same, `frame 0 16384x64`, **rc 1**.

A 768 MiB upper bound follows from the caps already in the tree: stride
65536 × 8192 rows × 1.5 for 4:2:0 is the largest `copyOwned` will accept, and
any width in that buffer above 8192 is then discarded by present. This pass
did not allocate a frame that large.

---

## L1 — `normalize_abs` globs against the cwd

`scripts/launch-coreelec.sh` is `#!/bin/sh` and documents dash. `normalize_abs`
splits on `/` and then runs unquoted `set -- $1` (`scripts/launch-coreelec.sh:46-51`,
shellcheck SC2086 disabled). Both `PLAYER_EXTRACT_DIR` and the player path go
through it (`:198`, `:216`). Pathname expansion runs on those fields against
the process cwd. `.` and `..` are still collapsed afterwards, so a normal
`/storage/../etc` becomes `/etc`.

### `[repro]`

dash, busybox ash, and bash, cwd `/tmp/normglob/cwdfiles` containing `aaa` and
`bbb`:

| Input | Result |
| --- | --- |
| `/tmp/normglob/*` | `/tmp/normglob/aaa/bbb` |
| `/tmp/does-not-exist/foo*` | `/tmp/does-not-exist/foo*` (no match, left literal) |
| `/storage/../etc` | `/etc` |
| `/*` | `/aaa/bbb` (the `*` field expands in the cwd, not in `/`) |

`dangerous_extract_root` then sees the rewritten path. A glob has to be
present in the environment value, and the result is a joined path rather than
`/`. Low impact. Quoting `set -- "$1"` is not the fix (that would undo the
split); the split fields need to be protected from pathname expansion.

**Dropped adjacent claim.** `rm -rf "$extract_root/squashfs-root"`
(`scripts/launch-coreelec.sh:232`) does not follow a symlink to a directory.
GNU `rm` and busybox 1.36.1 `rm -rf` on a symlink named `squashfs-root`
pointing at a directory removed the symlink. The file inside the target
(`keep`) was still there. A nested symlink to the victim directory behaved
the same way under busybox.

---

## #51 still holds (not a new finding)

HTTP `--url` does not call `setAllowLocalFile`. `urlIsLocalFileSource`
requires a `file:` prefix, no colon, or a slash before the first colon
(`src/app.cpp:178-203`). `open` then sets
`protocol_whitelist=http,https,tls,tcp,crypto,hls`
(`src/ffmpeg_decoder.cpp:136-138`). MQTT never sets the flag.

### `[repro]`

`amlogic-multistream --url … --frames 1` against a local HTTP server. Strace
`openat` of the canary `/tmp/audit8b/clip.mp4` stayed at zero for the
fail-closed cases (fresh trace on `file-seg` included).

| Case | Result |
| --- | --- |
| `http://…/file-seg.m3u8` with a `file:///tmp/audit8b/clip.mp4` segment | rc 1. libav: `Protocol 'file' not on whitelist 'http,https,tls,tcp,crypto,hls'`. Canary not opened. |
| `hls+http://…/file-seg.m3u8` with the same segment | rc 1. Same whitelist line. |
| Relative segment `clip.mp4` | rc 0. `presented 0`, 160×120. |
| Direct `http://…/clip.mp4` | rc 0. `presented 0`, 160×120. |
| `FILE://` of the local canary, even as a local `--url` | fail closed (`Protocol not found` on the earlier pass). `file://` and a scheme-less path still present, which is the allowed local path. |

The same fail-closed result (no canary `openat`) was recorded for playlist
segments written as `file:` without a slash, `FILE://`, a bare absolute path,
`crypto+file://`, `crypto:`, `data:file://`, `concat:file://`, `subfile,,…`,
a master playlist whose variant is `file://`, an AES-128 `URI="file://…"`,
an HTTP 302 `Location` of `file://`, and a 302 to the absolute path. The
absolute-path and `subfile` forms showed up as ordinary HTTP GETs of that
string, not as filesystem opens. A query containing CR LF was sent as
`GET /clip.mp4?x=1%0D%0AX-Injected:%20pwned`; the bytes were not a second
header. **Drop** the header-injection reading.

---

## Checked, not promoted

- **B1.** Left as the known nit. No new measurement of a retained
  `online:true` from the pre-`loop_start` window.
- **Reaper cap.** `kMaxBusyRetired = kMaxStreams` (`src/stream_player.h:55`).
  The start/retire accounting that keeps live + busy retired ≤ 17 is
  unchanged. No new churn bug.
- **N2, N3, N4, F4, F5, F6, F7.** No new evidence. F5’s `mov` / wrapped-mp3
  cover path and F7’s skippable MQTT tests are as AUDIT6 left them.
- **Exit timings** in the suite match the #34 bounds (about 2 s when a
  publish is queued, about 20 ms when the broker name never resolves, rc 1
  on auth give-up).
- **Symlink `rm -rf`** and **CRLF request smuggling** as above. **Drop.**

---

## CROSSCHECK-B

Peer: [AUDIT8.md](https://github.com/yyolk/amlogic-multistream/pull/52) on
draft [#52](https://github.com/yyolk/amlogic-multistream/pull/52)
(`cursor/audit8-host-8992`). Same tip, `0518a7e`. No production code changed.
Their line cites checked here still match the tree (`urlIsLocalFileSource`
`src/app.cpp:178-188`, whitelist `src/ffmpeg_decoder.cpp:136-138`,
`finishProcess` `_exit` `src/mqtt_controller.cpp:358`, `publishRaw`
`src/mqtt_controller.cpp:564-579`, reaper gate `src/stream_player.cpp:444-458`,
`av_find_best_stream` `src/ffmpeg_decoder.cpp:187`, header `src/stream_player.h:31-34`,
`scripts/ctest-require.sh:5-9`). Both suites are 14/14. Wall time differs
(theirs 117.0 s, this draft 132.61 s). That is not a finding.

### Their findings

| Their item | Their severity | This cross-check | Reason |
| --- | --- | --- | --- |
| B2 HTTP `--url` `file://` segment | closed | **agree** / **drop** | #51 holds on both probes. Whitelist line and no canary `openat` match. |
| B1 startup CONNECT window, `online` stays false | nit | **agree** / **nit** | Same code. No new retained `online:true`. |
| N2 `sent` true without a live socket | nit | **agree** / **nit** | `publishRaw` returns true on `MOSQ_ERR_SUCCESS`. Exit bound still holds. |
| N3 full reaper cap delays healthy opens | nit | **agree** / **nit** | The `busyRetired >= kMaxBusyRetired` `continue` is unchanged. |
| N4 header ties `_exit` to a stuck worker | nit | **agree** / **nit** | `stream_player.h:31-34` still says that. `finishProcess` always `_exit`s. |
| F4 same-id replace waits out `reaperBusy` | nit | **agree** / **nit** | `src/stream_player.cpp:444-446` still skips the replacement open. |
| F5 cover art via `av_find_best_stream` | nit | **agree** / **nit** | No attached-picture check after `:187`. Not re-promoted. |
| F6 packed-audio fixtures are ADTS only | nit | **agree** / **nit** | Test gap. No runtime change in #51. |
| F7 `mqtt_exit_test` / `mqtt_reconnect` skippable | nit | **agree** / **nit** | `ctest-require.sh` still lists only the three decoder tests. |
| N1, F1, X1, F3, A1–A5 closed | closed | **agree** / **drop** | Suite timings and the unchanged gates match. |
| Local playlist may open `file://`; absolute-path segment is an HTTP GET; `FILE://` / `HTTP://` fail closed; `data` / `pipe` / `concat` / `subfile` / `crypto+file` / `hls+file` do not open the canary; MOV `dref` default off | not a finding | **agree** / **drop** | Same results on the cases this draft ran. Their extra playlist forms (`#EXT-X-MAP`, `#EXT-X-MEDIA`, `pipe:`, `hls+file://`, `file://localhost`, percent-encoded `file`, leading space, `se:cret.mp4`) follow the same classifier. |
| “No new should-fix” | verdict | **disagree** | Misses **P1**. Re-verified below. Their other ratings stay nits. |

### This draft’s findings

| ID | Was | Peer | After cross-check | Reason |
| --- | --- | --- | --- | --- |
| **P1** wide source, present fails every tick | should-fix | **missed** (their verdict is “should-fix: none”) | **keep should-fix** | Re-ran on the `0518a7e` binary. Viewport 640×360, broker `127.0.0.1:18883`, about 2.2 s of play. `wide_long.mp4` (8224×32): **40** `present failed` lines, **2** “will retry”, decoder log `8224x32` on each open. `tall_long.mp4` (32×8224): **0** present failures, **3** “will retry”, `32x8224`. Code is unchanged: present returns false and `tick` `continue`s (`src/stream_player.cpp:503-505`); height fails `copyOwned` (`kMaxPlaneRows` 8192) and takes the 1 s path. Not must-fix: the process stays up, other tiles still tick, and nothing is retained as online. |
| L1 `normalize_abs` pathname expansion | nit | **missed** (they did not rebuild the launch-script harness) | **keep nit** | dash / busybox ash / bash still rewrite `/tmp/normglob/*` to `/tmp/normglob/aaa/bbb`. Needs a glob in the path. Not should-fix. |

### Agreed survivors

**None.** AUDIT8-A rates no must-fix and no should-fix. This draft’s only
should-fix is **P1**, which that draft does not rate, so it is not a finding
both audits keep. It stays should-fix in this file. L1 and every carried item
above stay nits.

| ID | Severity | Summary |
| --- | --- | --- |
| — | — | No must-fix or should-fix that both drafts rated and this cross-check keeps. |
