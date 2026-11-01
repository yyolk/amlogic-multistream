# AUDIT7: `main` @ 25bfa8d after the N1 fix (#47)

Audited commit: `25bfa8d6862671edbb6ba208b14bc703deba4dd3` (tip of `main`).
AUDIT6 base: `6d9d146` ([#44](https://github.com/yyolk/amlogic-multistream/pull/44)).
The only production change since that base is
[#47](https://github.com/yyolk/amlogic-multistream/pull/47) (`c597b16`), which moves
the first `mosquitto_connect_async` off the main thread. Later commits on this
tip are audit write-ups and `docs/research/`. Research notes are out of scope
and were not used as findings.

No production code is changed in this PR. Nothing was fixed.

**Severity:** **must-fix** = breaks a guarantee the code or docs make, on a
reachable path, in a way that leaves the player hung, retained as online, or
unbounded. **should-fix** = real, contained, worth doing before hardware work.
**nit** = polish, a known trade-off, or a test/doc gap.
**Evidence:** `[repro]` = run on this VM (section 6). `[code]` = from reading
the code. **needs-GS-King-X** = needs the box.

**Environment.** Ubuntu 24.04 x86_64, g++ 13.3, glibc 2.39, FFmpeg 6.1.1
(libavformat 60.16.100), libmosquitto / mosquitto 2.0.18, libdrm 2.4.125.
`hosts: files dns` (no `resolve` / `mdns` module).

**Suite.** `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` on `25bfa8d`:
**14/14 pass**, none skipped, 118.7 s. `mqtt_exit_test` (42.3 s): sigterm 2011 ms,
drain-cmd 2008 ms, gone-dns 20 ms, gone-blackhole 502 ms, drop-drain 2010 ms,
auth give-up rc 1 with `waitExitMs=2010`, startup auth rc 1, **startup-dns 20 ms rc 0**.

---

## Survivors

Must-fix: **none**. N1 is closed. F1, X1, F3, and A1–A5 still hold.

| ID | Severity | Finding | From |
| --- | --- | --- | --- |
| **B1** | **should-fix** | SIGTERM in the ~20 ms after the startup helper has written CONNECT, and before `MQTT running`, `_exit`s without DISCONNECT. The broker then publishes the empty startup will and replaces the previous retained status. 3 of 4 localhost launches signaled at 30 ms. A signal during the lookup itself does not do this | #47. libmosquitto 2.0.18 writes CONNECT inside `connect_async`, on the helper, before `connectReady` |
| **B2** | **should-fix** | `--url http://…` still puts `file` on the protocol allowlist, so an HLS playlist whose segment is `file:///…/secret.mp4` decodes that local file (rc 0, `codec=mpeg4`). MQTT `add` of the same URL is rejected | pre-existing. `setAllowLocalFile(true)` is unconditional on the `--url` path |

Nits left off this table, still open: N2, N3, N4, F4, F5, F6, F7. None of them
is a hang, a retained `online:true`, or an unbounded thread pile.

---

## Verdict

#47 closes **N1**. With one silent nameserver, `getaddrinfo` of the broker
name takes **10.01 s** if nothing interrupts it. SIGTERM, SIGHUP, and SIGINT
during that lookup exit in **3 ms**, rc 0, without reaching `MQTT running`,
and the previous retained status is unchanged. The same SIGTERM through
`scripts/launch-coreelec.sh` starts Kodi in **0.06 s** with one or two silent
nameservers. AUDIT6 measured 9.6 s / 19.6 s for the process and 9.1 s / 11.1 s
(SIGKILL) to Kodi.

F1, X1, F3, A1, A2, A3, A4, and A5 still hold, on the unit tests and on the
real binary. The reaper cap still stops at 9 blocked lookups (peak **12**
threads, **1.0 GB** VmSize, one `9 retired decoders still blocked` line). A
healthy `add` issued while that cap was full waited **6.95 s**.

Two should-fix items are new relative to AUDIT6. **B1** is a #47 hole in the
sentence "a signal before `mosquitto_loop_start` has not published yet": the
helper's `connect_async` writes the CONNECT packet, will included, before the
main thread has left `sigtimedwait`. **B2** is a pre-existing allowlist hole on
`--url` only. The MQTT command path does not have it.

---

## 1. N1, and the AUDIT5 items, on `25bfa8d`

| Item | Fixed by | Verdict | Evidence |
| --- | --- | --- | --- |
| **N1** pre-connect `getaddrinfo` holds SIGTERM | #47 | **Closed** | `[repro]` Below. The main thread stays in `sigtimedwait` (`src/mqtt_controller.cpp:262-272`). The helper calls `mosquitto_connect_async` (`:239`). The stall log has `MQTT shutting down on signal 15` and no `MQTT connect failed`, so the helper had not returned. `[code]` libmosquitto 2.0.18 `net__try_connect_tcp` calls `getaddrinfo` on the caller (`lib/net_mosq.c:405`). `WITH_ADNS` / `getaddrinfo_a` is broker-only. |
| **F1** command during the drain republishes `online:true` | #34 | **Closed** | `[repro]` `mqtt_exit_test` drain-cmd: exit 2008 ms rc 0, retained `online:false` with `cam`. `[code]` `closing_` is still set under `closeMu_` before the offline publish (`src/mqtt_controller.cpp:337-342`). `handleConnect` (`:490-492`) and `handleMessage` (`:519-524`) still return before `publishStatus`. No new caller. |
| **X1** exit joins the mosquitto thread | #34 | **Closed** | `[repro]` gone-dns 20 ms, gone-blackhole 502 ms, drop-drain 2010 ms, all rc 0. A blackholed IP from launch (`10.255.255.1`), SIGTERM at 0.5 s: **516 ms**, and the log already contains `MQTT running`, so this is `finishProcess`, not the new `_exit`. `[code]` `finishProcess` (`:334-358`) still never calls `mosquitto_loop_stop`. |
| **F3** fresh-id churn stacks decode threads | #33 | **Closed** | `[repro]` Real silent resolver, `camera.lan`, 12 ids added and removed with a 250 ms gap so each open starts: threads climb 3→12 and **stop**. One log line `StreamPlayer: 9 retired decoders still blocked; new opens wait`. VmSize 355 MB → **1021 MB**. 12 threads = main + mosquitto + 1 live decoder + 9 retired. The bound `kMaxStreams + kMaxBusyRetired − 1 = 17` decode threads still holds `[code]` (`src/stream_player.cpp:450-458`, `src/stream_player.h:44-50`). `stream_player_test` still pins `entered == kMaxBusyRetired`. |
| **A1** offline publish is queued before the drain | #25 / #34 | **Closed** | `[repro]` sigterm 2011 ms, retained offline status still lists `cam`. Same ordering as F1. |
| **A2** auth give-up exits 1, and does not join a stuck worker | #24 / #34 | **Closed** | `[repro]` Mid-session give-up with the stalling decoder: rc 1, `waitExitMs=2010` (the 2 s drain, not the 120 s stall). Startup refusal: rc 1. |
| **A3** `restart` does not stack opens on one stuck id | #29 | **Closed** | `[repro]` `remove cam` / `add cam` with the healthy URL, while `cam` was inside the 10 s lookup: `StreamPlayer open cam` **5.61 s** later. One in-flight open, then the replacement. `stream_player_test` still requires `maxInFlight <= 1`. |
| **A4** top-level `mp3` / `aac` rejected | #28 | **Closed** | `[repro]` `song.mp3` and `plain.aac` over HTTP: rc 1, `demuxer is not allowed`, both via `--url` and via MQTT `add`. `anim.gif`: format whitelist. |
| **A5** `mp3float` stays on the codec allowlist | #28 | **Closed** | `[code]` `kCodecWhitelist` still lists `mp3float` (`src/ffmpeg_decoder.cpp:42-43`). `[repro]` MQTT open of `wrap_mp3.m3u8` logs `codec=mjpeg` and no codec-whitelist rejection. |

N1 timings. Resolver options `timeout:5 attempts:2`. A UDP socket on
`127.0.0.2:53` (and `.3` for the two-nameserver case) reads queries and does
not answer. `/etc/resolv.conf` and `/etc/hosts` are bind-mounted in a mount
namespace. An uninterrupted `getaddrinfo("broker.unresolvable")` takes
**10.01 s** (one nameserver).

| Case | AUDIT6 on `6d9d146` | `25bfa8d` |
| --- | --- | --- |
| 1 silent nameserver, SIGTERM 0.5 s after launch | 9.59 s | **0.003 s**, rc 0, retained sentinel kept |
| 2 silent nameservers, SIGTERM 0.5 s after launch | 19.58 s | **0.003 s**, rc 0, retained sentinel kept |
| SIGHUP / SIGINT, 1 or 2 silent nameservers | — | **0.003 s**, rc 0, sentinel kept |
| `startup-dns` in `mqtt_exit_test` (60 s interposed sleep) | not present | **20 ms**, rc 0, no `MQTT running` |
| Launch script, 1 / 2 silent nameservers, SIGTERM to the script | Kodi at 9.09 s / **11.14 s (SIGKILL)** | Kodi **0.06 s / 0.06 s**, player logs signal 15, no SIGKILL |
| Refused port, SIGTERM during the 1 s helper backoff | (main-thread sleep, already prompt) | **0.003 s**, rc 0 |
| Blackholed IP from launch, SIGTERM at 0.5 s | 0.52 s | **0.52 s** |

The post-connect rows from `mqtt_exit_test` are unchanged from AUDIT6's table:
2.01 s with a stuck worker and the broker up, 0.02 s when the broker is gone
and the reconnect is inside the interposed lookup, 0.50 s when `connect()`
blocks, 2.01 s when the broker drops during the drain.

A refused port that later accepts a connection still connects. The player
logged `MQTT connected`, applied `add`, and on SIGTERM published
`online:false` with that stream kept, then `MQTT disconnected`.

---

## 2. Findings

### B1 — should-fix, #47: a startup signal after CONNECT is written publishes the empty will `[repro]` `[code]`

- **Where:**
  - The helper calls `mosquitto_connect_async` and only then stores
    `connectReady` (`src/mqtt_controller.cpp:239-243`).
  - The main thread `_exit`s if `sigtimedwait` returns a signal while
    `connectReady` is still false (`:262-272`). It does not look again after
    the wait returns.
  - libmosquitto 2.0.18 `mosquitto__reconnect(..., blocking=false)` calls
    `net__socket_connect` and, unless that returns `> 0`, sets
    `mosq_cs_connected` and `send__connect` (`lib/connect.c:205-226`).
  - `MOSQ_ERR_CONN_PENDING` is **-1**, so an `EINPROGRESS` connect does not
    take the `rc>0` branch.
  - `packet__queue` calls `packet__write` whenever `threaded == mosq_ts_none`
    (`lib/packet_mosq.c:202-203`). `loop_start` has not set threaded mode yet.
    The write of CONNECT, will included, happens on the helper.
- **What:** strace of a localhost connect, before any signal:
  - tid 8726 (the helper): `connect()` returns `EINPROGRESS`, then `sendto` of
    136 bytes starting `10 85 … MQTT` — the CONNECT packet, carrying
    `audit7s/status` and `{"online":false,"playing":fa…`.
  - 20 ms later, tid 8727 (the loop thread) writes SUBSCRIBE and the online
    status.
  - So CONNECT is on the wire before `MQTT running` is printed, and before the
    main thread has left the pre-loop wait.

  Forty localhost launches, SIGTERM at 0, 1, 2, 5, 8, 12, 20, 30, 50, or 80 ms,
  four each. A sentinel retained status `{"online":true,"sentinel":"KEEPME"}`
  was published first. `MQTT running` is printed only after `loop_start`
  (`src/mqtt_controller.cpp:284-294`), so its absence means the pre-loop
  `_exit` ran.

  | Result | Count |
  | --- | --- |
  | No `MQTT running`, sentinel kept | 29 |
  | No `MQTT running`, sentinel replaced by the empty startup will | **3** |
  | `MQTT running`, then the explicit offline status (the `finishProcess` path) | 8 |
  | Hung | 0 |

  All 3 replacements were the 30 ms delay: **3 of the 4** launches signaled
  at 30 ms. The replacement was exactly
  `{"online":false,"playing":false,"layout":"1x1","layout_setting":"auto","streams":[]}`.
  The log line was `MQTT shutting down on signal 15` and did not contain
  `MQTT running` or `MQTT status`. Nothing in-process published that payload.
  The broker did, from the will inside the CONNECT the helper had already
  written. Exit was still a few milliseconds. No sample retained `online:true`.

  The stall runs are the other side of the same race. SIGTERM while the helper
  is still inside `getaddrinfo` keeps `KEEPME`, because CONNECT has not been
  written. That is the N1 case, and it is closed.
- **Why should-fix, not must-fix:**
  - The window is the `sigtimedwait` (20 ms) that overlaps the helper's write
    of CONNECT. It needs a broker that completes TCP, not a resolver that
    never answers. The add-on default `localhost` is the case that hits it.
  - The bytes are the empty startup will. A signal a few milliseconds later,
    after `MQTT running`, runs `finishProcess` and publishes the same empty
    object, because the new process has not applied a command yet. This is not
    F1: the player is not left looking online.
  - It does not blow the launch script's 10 s poll.
  - It is still a break of the #47 claim. `docs/mqtt.md` says a signal before
    `mosquitto_loop_start` "has not published yet", and the comment at
    `src/mqtt_controller.cpp:230` says the same. For a third of the launches
    that logged that shutdown *without* `MQTT running`, the previous retained
    status was gone. 29 of 40 kept it. The log line does not say which.
- **Fix shape** (not implemented): keep `getaddrinfo` on the helper, and do
  not call `mosquitto_connect_async` until the main thread has committed to
  `loop_start`. A signal before that call still `_exit`s, and CONNECT has not
  been written, so the will is not armed. Calling `finishProcess` once
  `connectReady` is set does **not** preserve the previous status: the table
  is empty, so the explicit offline publish wipes it too. The preserving fix
  is to delay CONNECT, not to change which exit runs after it.
  - Test: sentinel retained status, localhost broker, SIGTERM at 30 ms (and
    once the broker has logged the new client, if that is sooner). The
    sentinel must still be the retained payload, and the player log must not
    contain `MQTT running`.
  - Reword `docs/mqtt.md` and the comment at `:230` if CONNECT stays where it
    is: the previous status survives a signal during the lookup, not every
    signal before the present loop.

### B2 — should-fix, pre-existing: `--url http(s)://` allows `file` segment URIs `[repro]` `[code]`

- **Where:** `decodeUrl` calls `setAllowLocalFile(true)` for every `--url`
  (`src/app.cpp:181-183`). `FfmpegDecoder::open` then puts `file` on
  `protocol_whitelist` (`src/ffmpeg_decoder.cpp:136-138`). HLS copies that
  list onto segment opens. MQTT never calls `setAllowLocalFile`, so its list
  has no `file`.
- **What:** `file_seg.m3u8` is an HTTP playlist with one segment,
  `file:///tmp/audit7/media/secret.mp4` (mpeg4, 64x64).

  | Path | Result |
  | --- | --- |
  | `--url http://127.0.0.1/file_seg.m3u8` | **rc 0, `codec=mpeg4`** |
  | MQTT `add` of that URL, then `start` | `Protocol 'file' not on whitelist 'http,https,tls,tcp,crypto,hls'`, open fails |
  | `--url` 302 to `file:///…/secret.mp4` | rc 1, `Connection to tcp://:80 failed` (not opened) |
  | HTTP playlist whose segment is the absolute path `/tmp/…/secret.mp4`, or a `../` path | rc 1, invalid data, no open of the mp4, on both `--url` and MQTT |

  `docs/mqtt.md` says `file` is on the allowlist "only on the `--url` local-file
  path". An `http://` URL is not a local path. The comment in `app.cpp` says
  the same thing and then sets the flag unconditionally.
- **Why should-fix, not must-fix:** the MQTT command path, which is the remote
  surface, rejects it. `--url` is the operator's process. It still matters:
  CoreELEC runs this binary as root, and a network playlist is enough. The
  file is demuxed, not executed. A redirect and a raw absolute path did not
  get through.
- **Fix shape** (not implemented): call `setAllowLocalFile(true)` only when
  the `--url` string is not `http://` or `https://`. Add `file_seg.m3u8` next
  to the existing HTTP cases and expect the same protocol rejection `--url`
  already produces for a bare `file` URL when the flag is off.

### Carried nits

| ID | Severity | This pass |
| --- | --- | --- |
| N2 | nit | Unchanged. Blackholed IP from launch exits in **516 ms**, which is the 500 ms `kDisconnectWait`, not a hang. `publishRaw`'s "live connection" comment (`src/mqtt_controller.h:69-70`) is still wrong for a socket stuck in `connect()`. |
| N3 | nit | The cap is the same gate for every new open. While it was full, a healthy `add ok2` waited **6.95 s** (one in-flight lookup of a 10.01 s stall). `docs/mqtt.md` already says a new id shows no picture until a blocked lookup returns. A `restart` of a tile that was already open takes the same `continue` (`src/stream_player.cpp:444-458`); that case was not re-timed. |
| N4 | nit | Unchanged since `6d9d146`. `src/stream_player.h:31-34` still says exit `_exit`s only "if a worker is still inside that call". `exitIfBusy()` is still test-only. |
| F4 | nit | Re-adding `cam` with a working URL while its old lookup was in flight opened **5.61 s** later. Same behavior as AUDIT6's 8.58 s; the clock here is the remainder of a 10 s stall. |
| F5 | nit | Still open, and it is on the MQTT path, not only `--url`. `song.m4a` and `wrap_mp3.m3u8` / `wrap_m4a.m3u8` present `codec=mjpeg` (one frame, then the 1 s retry). `song.mp3` itself is rejected. |
| F6 | nit | `ffmpeg_decoder_test` still builds only an ADTS audio rendition. `ac3` / `eac3` / `mp3` can leave `kFormatWhitelist` without that test going red. |
| F7 | nit | `scripts/ctest-require.sh` still requires `ffmpeg_decoder`, `present_smoke`, and `stream_player_http` only. `mqtt_exit_test` (now also the N1 guard) and `mqtt_reconnect` still skip with 77 when `mosquitto` is missing. This host had the daemon; CI's image does too. |

---

## 3. Considered, not findings

- **`_exit` while the helper is inside `getaddrinfo`.** Forty startup signals
  plus the stall runs: 0 hangs. glibc `_exit` is `exit_group`. The helper is
  not joined, which is what makes N1 stay under 20 ms. The unit test allows
  1500 ms; it took 20 ms.
- **`fflush` against the helper's log line.** The refused-port path logs
  `MQTT connect failed` on the helper and can race the main thread's flush.
  The measured exits were a few milliseconds, including a SIGTERM during that
  backoff. Not observed as a deadlock.
- **Retry loop moved off-thread.** A closed port, then a broker on that port,
  still reaches `MQTT connected` and applies `add`. `mqtt_reconnect` passes
  (17.9 s).
- **`mosquitto_threaded_set`.** Only the helper touches `mosq_` until
  `connector.join()` (`:282`). `loop_start` then sets `mosq_ts_self`. There is
  no concurrent library call on the success path. Startup auth and the
  reconnect test both get a session.
- **Lock order of `closeMu_` and `StreamManager::mu_`.** Unchanged. Commands
  take `closeMu_` then the table mutex. `onDisconnect` takes only
  `disconnectMu_`. The #47 helper takes neither.
- **Allowlist probes that did not open a local file.** MQTT `file://` segment
  (rejected). HTTP 302 to `file://` (`tcp://:80`, connection refused). A
  segment whose URI is an absolute filesystem path, or a `../` path
  (invalid data, no decode of `secret.mp4`). `song.mp3`, `plain.aac`, and
  `anim.gif` rejected. `secret.mp4` itself plays, as a normal HTTP object.
- **Stall isolation.** `stream_player_stall`: `blackhole+trickle presents=39
  maxTickMs=0`, `live+vod presents=152 maxTickMs=0`, shutdown 85 ms and 16 ms.
  With a real stuck `camera.lan` next to `ok.ts`, `--render-dir` wrote 468
  PPMs before exit.
- **Status size, uncaught `tick()` exception, TSan thread-leak on `_exit`.**
  Unchanged from AUDIT6 section 4. #47 adds a second abandoned thread on the
  pre-loop `_exit` (the helper). That is intentional. A TSan run of
  `mqtt_exit_test` would report it, and the startup-dns child would exit
  non-zero under TSan's thread-leak check unless `report_thread_leaks=0`.

---

## 4. Blocked on GS-King X

- N1's trigger no longer depends on how long CoreELEC's resolver waits. The
  lookup is not on the thread that calls `sigtimedwait`. This was measured
  with glibc 2.39 and libmosquitto 2.0.18. It remains true on the box if that
  libmosquitto still calls `getaddrinfo` inside `connect_async`.
- F3 / N3: armv7 address space at the cap. On this x86_64 host the cap was
  **1021 MB** VmSize at 9 blocked lookups plus one live decoder.
- Which MP3 decoder name CoreELEC's FFmpeg registers first.
- Hardware decode and DRM/KMS present. Not started on this path. `docs/research/`
  was not reviewed.

---

## 5. How this was checked

- **Suite:** `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` on `25bfa8d`.
  14/14 passed, 118.7 s. Host packages: `libmosquitto-dev` 2.0.18,
  `libdrm-dev` 2.4.125, FFmpeg 6.1.1, `mosquitto` 2.0.18, g++ 13.3.
- **Resolver stalls:** `unshare --mount` bind-mounts a resolv.conf
  (`nameserver 127.0.0.2` / `.3`, `options timeout:5 attempts:2`) and an
  `/etc/hosts` with no `broker.unresolvable` and no `camera.lan`. Python UDP
  sockets on those addresses read and do not answer. A control
  `getaddrinfo` in that namespace takes 10.01 s.
- **Launch script:** `scripts/launch-coreelec.sh` with `PLAYER_BIN` exec'ing
  the built player, `PATH` stub `systemctl` appending timestamps, inside the
  same mount namespace. SIGTERM to the script 2.4 s after start (after its
  own `sleep 1`).
- **B1 window:** throwaway `mosquitto` (`persistence false`), sentinel
  retained publish, 40 player processes, `mosquitto_sub -C 1 -W 2` for the
  retained payload. strace `-f -e trace=network,write` of one unsignaled
  localhost connect.
- **Reaper:** 20 s 10 fps MPEG-2 TS on `127.0.0.1`, broker on `127.0.0.1` so
  MQTT itself does not use the silent resolver, `camera.lan` does. Thread
  counts from `/proc/<pid>/task`, VmSize from `/proc/<pid>/status`.
- **Allowlists:** `ffmpeg` fixtures (`song.mp3` with an ID3 APIC JPEG,
  `song.m4a` with `attached_pic`, `plain.aac`, `anim.gif`, `secret.mp4`,
  `wrap_mp3.m3u8`, `wrap_m4a.m3u8`, `file_seg.m3u8`). Opened with
  `--url --frames` and, for the ones that matter, with MQTT `add` / `start`
  against the built player.
- **libmosquitto source:** v2.0.18 tag (`lib/connect.c`, `lib/net_mosq.c`,
  `lib/packet_mosq.c`, `lib/thread_mosq.c`) for B1 and for the claim that the
  client library resolves on the calling thread.
