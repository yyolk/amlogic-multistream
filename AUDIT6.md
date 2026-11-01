# AUDIT6: `main` @ 6d9d146 after the AUDIT5 fixes (#33, #34)

Audited commit: `6d9d146ba693d9b335dc13733d3ab146d542d45e` (merge of
[#34](https://github.com/yyolk/amlogic-multistream/pull/34)). AUDIT5 base:
`bf00f31` (closed drafts [#32](https://github.com/yyolk/amlogic-multistream/pull/32)
and [#31](https://github.com/yyolk/amlogic-multistream/pull/31)).

Fixes under audit:

- [#33](https://github.com/yyolk/amlogic-multistream/pull/33) F3: `kMaxBusyRetired = 9` global cap on retired decoders whose loop has not returned (`1a5cf55`).
- [#34](https://github.com/yyolk/amlogic-multistream/pull/34) F1 + X1: the `closing_` latch stops commands and reconnects during the exit wait. Every exit after the network thread starts publishes offline, drains for up to 2 s, sends DISCONNECT with a 500 ms wait only when that publish was queued, never calls `mosquitto_loop_stop`, and `_exit`s (`6d9d146`).

No production code is changed in this PR. Nothing was fixed. All scenarios ran against the `6d9d146` binary.

**Severity:** **must-fix** = breaks a guarantee the code or docs make, on a
reachable path. **should-fix** = real, contained, worth doing before hardware
work. **nit** = polish or test/doc gaps.
**Evidence:** `[repro]` = run on this VM (section 6). `[code]` = from reading
the code. **needs-GS-King-X** = needs the box.

**Environment.** Ubuntu 24.04 x86_64, g++ 13.3, glibc 2.39, FFmpeg 6.1.1
(libavformat 60.16.100), libmosquitto / mosquitto 2.0.18, libdrm 2.4.125.

**Suite.** `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` on `6d9d146`:
**14/14 pass**, none skipped (mosquitto and squashfs-tools installed).
`mqtt_exit_test`: sigterm 2014 ms, drain-cmd 2009 ms, gone-dns 20 ms,
gone-blackhole 503 ms, drop-drain 2014 ms, auth give-up 1992 ms rc 1, startup
auth rc 1. CI runs 41303997572 (CI) and 41303997457 (CoreELEC device AppImages)
on `6d9d146` are green.
ThreadSanitizer: `stream_player_test`, `stream_player_stall_test`, and every
`mqtt_exit_test` player child that wrote a log report **0 data races** (details in section 5).

---

## Verdict

#33 and #34 close **F1**, **X1** (Gemini G1 + G2 + my F2), and **F3**. I found
no regression in retained online status, the auth exit code, stall isolation,
restart gating, or the demuxer and codec whitelists. **No must-fix remains.**

The leftover the #34 agent flagged is real. If the broker hostname goes to a
resolver that never answers, SIGTERM before the first connect is queued
waits for `getaddrinfo` on the main thread. I rate it **should-fix** (N1): with
two nameservers, the launch script SIGKILLs the player and Kodi comes back at
11 s. The other new items are nits.

| ID | Severity | Finding | From |
| --- | --- | --- | --- |
| **N1** | **should-fix** | SIGTERM during the pre-connect broker lookup waits for `getaddrinfo`: 9.6 s with 1 silent nameserver, 19.6 s with 2. Through the launch script, Kodi returns at 9.1 s and 11.1 s (SIGKILL) | pre-existing (start-up retry loop). #34 left it out of scope |
| N2 | nit | `sent` is true while the network thread is in a reconnect's blocking `connect()`, so the exit waits the full 500 ms. The bound holds, but the "live connection" comment and README wording are wrong | #34 |
| N3 | nit | While 9 retired lookups are blocked, a **healthy** `restart` or read-failure retry also waits out a resolver timeout (8.5 s measured). Docs mention only new opens | #33 trade-off |
| N4 | nit | Stale docs after #34: the `stream_player.h` header says exit `_exit`s only "if a worker is still inside that call". `exitIfBusy()` is now used only by tests, and `docs/mqtt.md` still calls it "the process-exit wait" | #34 |
| F4 | nit | Re-adding an id with a corrected URL still waits out the old lookup (8.58 s) | carried from AUDIT5 |
| F5 | nit (wider) | Cover art still presents through `mov` (`.m4a`). **New:** a one-entry HLS playlist wrapping `song.mp3` or `song.m4a` also presents it, which gets around #28's top-level check | carried, widened |
| F6 | nit (partly closed) | #34 pins F1/X1 in `mqtt_exit_test`. Packed-audio fixtures are still ADTS only | carried |
| F7 | nit | `mqtt_exit_test` and `mqtt_reconnect` still exit 77 without the daemon and are not required. They are now the only guard for F1/X1 | carried |

AUDIT5 F8 (`~StreamPlayer` comment) is **closed** by #34.

---

## 1. AUDIT5 items on `6d9d146`

| Item | Fixed by | Verdict | Evidence |
| --- | --- | --- | --- |
| **F1** command during the drain leaves `"online":true` retained | #34 | **Closed** | `[repro]` Real binary, real silent resolver, `ok` + stuck `cam`. SIGTERM, then `list` and `add late` 0.5 s later: exit 2.02 s rc 0, **1** status message after SIGTERM, retained `online:false` with `[ok, cam]`, 2 × `ignored: shutting down`. `closing_` is set under `closeMu_` together with the offline publish (`src/mqtt_controller.cpp:305-314`). `handleMessage` (`:490-496`) and `handleConnect` (`:461-464`) stop on it. `publishStatus` has no other caller. |
| **X1** exit joins the mosquitto thread (G1, G2, F2) | #34 | **Closed** | `[repro]` See the table below. `finishProcess` is `[[noreturn]]` (`src/mqtt_controller.h:79`) and every loop exit calls it (`src/mqtt_controller.cpp:268-291`). Nothing after `loop_start` reaches `shutdown()`/`mosquitto_loop_stop`. |
| **F3** fresh-id churn stacks decode threads | #33 | **Closed** | `[repro]` `add cN` (stuck) / `remove cN` every 100 ms for 10 s (96 cycles): **peak 13 threads, 1.07 GB VmSize** (AUDIT5: 94 threads, 3.00 GB). One `9 retired decoders still blocked` log line. The bound `kMaxStreams + kMaxBusyRetired − 1 = 17` decode threads holds `[code]`: a start needs busy ≤ 8 and live ≤ 8, and retiring a worker keeps the sum. |
| F4 same-id URL fix latency | — | Open (nit) | 8.58 s (AUDIT5: 8.59 s). |
| F5 cover art via `mov` | — | Open (nit), wider | See F5 below. |
| F6 tests do not pin the gate / F1 / F2 | #34 | **Partly closed** | `mqtt_exit_test` now interposes `getaddrinfo` and blackholes `connect()`. It covers drain-cmd, gone-dns, gone-blackhole, and drop-drain. Packed-audio fixtures are still ADTS only. |
| F7 skippable MQTT tests | — | Open (nit) | `scripts/ctest-require.sh:5-9` is unchanged. |
| F8 destructor comment | #34 | **Closed** | `src/stream_player.cpp:63-67`. |

Exit timing, measured by this harness on the real binary (`--mqtt-host brokerhost`, `options timeout:5 attempts:2`):

| Scenario | AUDIT5 on `bf00f31` | `6d9d146` |
| --- | --- | --- |
| SIGTERM, stuck worker, broker up | 2.02 s, table kept | **2.02 s**, table kept, 1 status msg |
| SIGHUP, stuck worker, broker up | (not run) | **2.02 s**, rc 0, table kept |
| `list` + `add` during the drain | `"online":true` retained | **`online:false`**, table kept |
| No stuck worker, broker gone 3.5 s earlier, 1 / 2 silent nameservers | 7.53 s / 17.56 s | **0.02 s / 0.02 s** |
| No stuck worker, broker remapped to blackholed `10.255.255.1` | > 60 s | **0.51 s** |
| Stuck worker, broker drops 0.2 s after SIGTERM, 1 / 2 nameservers | 11.04 s / 21.07 s | **2.02 s / 2.02 s** |
| Stuck worker **and** blackholed broker (worst post-connect case) | — | **2.52 s** |
| Broker given as blackholed IP from launch, SIGTERM at 3 s | — | **0.52 s** |

All are rc 0 and inside `scripts/launch-coreelec.sh`'s 10 s poll. The 2.52 s
worst case matches `docs/mqtt.md`'s "about 2.5 seconds at most" for the
post-connect path.

## 2. Requested re-checks

- **Exit / online status** `[repro]`: closed (F1 row above). There is no path left that publishes `"online":true` without holding `closeMu_`.
  - QoS 1 resends after a reconnect keep queue order. libmosquitto resends in-flight messages in order from `message__retry_check`, after `on_connect` returns, so an `online:true` queued just before `closing_` is still followed by the offline publish.
  - **Lock order** `[code]`, against libmosquitto 2.0.18 source:
    - While holding `closeMu_`, the main thread takes only `msgs_out.mutex` and `out_packet_mutex` (`actions.c:158`, `packet__queue`).
    - The network thread takes `closeMu_` only from `on_connect`/`on_message`, while holding `callback_mutex` (`handle_connack.c:39`, `handle_publish.c:125/143`) and neither of those two mutexes.
    - `on_disconnect` takes only `disconnectMu_`.
    - No cycle.
- **Dead-broker exit vs the launch script's 10 s** `[repro]`: every post-connect case is ≤ 2.52 s (table above). The one remaining overrun is before the first connect is queued (N1).
- **Auth exit 1** `[repro]`:
  - Startup with a wrong password against a `password_file` broker: 3 × `connect rejected`, then rc 1 after 5.08 s. That is the 1 s + 2 s reconnect backoff plus CONNACKs.
  - Mid-session give-up with a stuck worker (unit test): rc 1, 1992 ms after the give-up line.
  - At give-up the socket is closed, so `sent` is false: no DISCONNECT, then `_exit(1)`.
- **Busy-retired cap and same-id `reaperBusy`** `[repro]`:
  - `restart cam` every 100 ms for 10 s (98 commands): peak **4 threads**, 418 MB. The per-id gate from #29 is intact.
  - Fresh-id rotation: 13 threads (F3 row).
  - The two gates compose. A pending restart waits on both (`src/stream_player.cpp:444-459`) and still runs once.
  - With the cap full, healthy opens wait too (N3).
- **Stall isolation** `[repro]`:
  - `--render-dir` PPMs per 0.5 s with `ok` + stuck `cam`: 15 before, 15 after `restart cam`, and 12 after `remove cam` (the layout goes from 1x2 to 1x1).
  - `stream_player_stall` passes, also under TSan: `blackhole+trickle presents=39`, `live+vod presents=146`, `maxTickMs ≤ 13`.
- **Demuxer / codec whitelists** (`--url http://127.0.0.1/… --frames 3`) `[repro]`:

  | Input | Result |
  | --- | --- |
  | HLS master + ADTS / AC-3 / E-AC-3 / MP3 audio rendition | rc 0, 3 frames each, 0 whitelist warnings |
  | `song.mp3` (ID3 APIC JPEG) | rc 1, `demuxer is not allowed: mp3` |
  | `plain.aac` | rc 1, `demuxer is not allowed: aac` |
  | `anim.gif` | rc 1 (format whitelist) |
  | `song.m4a` (JPEG `attached_pic`) | **rc 0, 1 frame, `codec=mjpeg`** (F5) |
  | `wrap_mp3.m3u8` (one `#EXTINF` → `song.mp3`) | **rc 0, 1 frame, `codec=mjpeg`** (F5, new path) |
  | master → `wrap_mp3.m3u8`; `song.mp3` renamed `.ts`; `wrap_m4a.m3u8` | **rc 0, 1 frame, `codec=mjpeg`** each |

---

## 3. Findings

### N1 — should-fix, pre-existing: SIGTERM during the pre-connect broker lookup waits for `getaddrinfo` `[repro]`

- **Where:**
  - The start-up retry loop calls `mosquitto_connect_async` on the main thread (`src/mqtt_controller.cpp:223-253`).
  - libmosquitto resolves the host synchronously inside that call.
  - SIGINT, SIGTERM, and SIGHUP are blocked process-wide (`:209-214`) and only taken by `sigtimedwait` (`:236`), which does not run while the lookup is in progress.
  - The comment at `:223-226` says this loop exists to "keep accepting SIGINT/SIGTERM/SIGHUP so the launch script can still start Kodi". That holds for a refused port, but not for a lookup that stalls.
- **What:** `--mqtt-host broker.unresolvable`, with the resolver pointed at UDP listeners that never answer (glibc defaults `timeout:5 attempts:2`):

  | Case | SIGTERM → exit |
  | --- | --- |
  | 1 nameserver, SIGTERM 0.5 s after launch | **9.59 s**, rc 0 |
  | 2 nameservers, SIGTERM 0.5 s after launch | **19.58 s**, rc 0 |
  | 1 nameserver, SIGTERM 3 s after launch | 7.09 s, rc 0 |

  End to end through `scripts/launch-coreelec.sh`, with a stub `systemctl`, a bare-ELF wrapper as `PLAYER_BIN`, and SIGTERM to the script only, as `KillMode=mixed` does:

  | Nameservers | SIGTERM → `systemctl start kodi` | Player |
  | --- | --- | --- |
  | 1 | **9.09 s** | exits by itself, just inside the poll |
  | 2 | **11.14 s** | **SIGKILLed** by `stop_player`, never logs the signal |

- **Why should-fix, not must-fix:**
  - It needs a broker configured **by hostname** (the add-on default is `localhost`, which `/etc/hosts` answers) and a resolver that **times out** rather than fails. It must also happen before the first connect is queued.
  - Once the resolver is silent, the process spends most of its time inside the lookup: 10–20 s of lookup against a 1–30 s `sigtimedwait` backoff. A stop request is also most likely exactly when the broker is unreachable.
  - Cost: Kodi returns at about 11 s through the script's SIGKILL fallback, not about 1 s.
  - The retained status is not damaged: this process never connected, so nothing was published and the previous session's offline status or will stays.
  - That is the same profile AUDIT5 gave F2 before it merged into X1. X1 was must-fix because the trigger was any broker loss after connect, and the docs promised that bound.
  - `README.md:65` ("…keeps the exit inside the launch script's poll when the broker is down, drops during the wait, does not resolve…") describes the post-connect exit, but readers will take "does not resolve" to cover start-up too.
- **Fix shape** (not implemented):
  - Run the start-up `connect_async` retry loop on a helper thread, and keep the main thread in `sigtimedwait`.
  - If a signal arrives before the helper has started the loop, flush and `_exit(0)`. Nothing has been published and nothing needs joining.
  - Once the helper has queued the connect and called `mosquitto_loop_start`, the main thread enters the present loop as today.
  - Pre-resolving with `getaddrinfo_a` and connecting by numeric address also works, but reconnects would then not follow a DHCP change of the broker's address.
  - Test: `mqtt_exit_test` already interposes `getaddrinfo`. Touch the stall file **before** launch, wait for the entered marker, then SIGTERM, and expect rc 0 within 1500 ms.
  - Also reword `:223-226` and README line 65.
- **needs-GS-King-X:** whether CoreELEC's resolver (and any `mdns` entry in `nsswitch.conf` for `*.local` broker names) times out or fails fast.

### N2 — nit, #34: `sent` is true while a reconnect is blocked in `connect()` `[repro]` `[code]`

- **What:**
  - libmosquitto 2.0.18 passes `&mosq->sock` straight into `net__try_connect` (`lib/net_mosq.c:916`), so the socket is valid while a blocking reconnect is in progress.
  - `send__publish` only checks `sock != INVALID_SOCKET`, so `publishRaw` succeeds and logs `MQTT status {"online":false…}`. Then `mosquitto_disconnect` also succeeds, and `finishProcess` waits the full `kDisconnectWait`.
  - Measured: blackholed broker, no stuck worker, 0.51 s; with a stuck worker, 2.52 s; blackholed IP at launch, 0.52 s.
- **Why nit:** the 500 ms cap holds, so the bound is intact. Nothing reaches the broker either way.
  - But the `publishRaw` comment ("True when libmosquitto queued the message on a live connection", `src/mqtt_controller.h:66-68`) and README:65 ("only when that publish was queued on a live broker") are wrong for this state.
  - The `MQTT status …` log line is also logged for a publish that never left the process.
- **Fix shape:** reword the comment, or track the session with an atomic set in `handleConnect(rc == 0)` and cleared in `onDisconnect`, and gate DISCONNECT on `sent && connected`.

### N3 — nit, #33 trade-off: a full cap also delays healthy reopens `[repro]`

- **What:** `ok` + stuck `cam`, then 12 stuck fresh ids added and removed within 1.2 s, giving 13 threads and a full cap. Then `add ok2` (healthy) and `restart ok` (healthy). Both opens waited **8.49 s**, until the first retired lookup returned. `ok` stays dark (last frame kept) for that time.
- **Why it matters:** the gate applies to every reopen, not just new ids. A healthy stream that hits EOF or a read error during such an episode also waits. `docs/mqtt.md:39` describes it for `add` ("That stream just shows no picture until one of the blocked lookups returns"), but not for `restart` or retries of tiles that were already presenting.
- **Why nit:** it is bounded by one resolver timeout, needs 9 blocked lookups at once, and is the price of F3's bound. The only alternative that preserves the thread bound is a per-id reservation, which adds complexity for little gain.
- **Fix shape:** document it, for example: "`restart` and the 1 s retry of an open tile wait too".

### N4 — nit, #34: stale exit wording `[code]`

- `src/stream_player.h:31-34`: "Process exit publishes offline status first, waits kExitJoinDeadline, then flushes and _exit if a worker is still inside that call." Since #34, MQTT mode always `_exit`s after the drain (`src/mqtt_controller.cpp:315-329`).
- `StreamPlayer::exitIfBusy` (`src/stream_player.cpp:310-318`) has no production caller. `tests/stream_player_test.cpp:280-308, 386-389` still pin it, and `docs/mqtt.md:119` describes it as "the process-exit wait". Either drop it or say it is a test helper. The real exit is pinned by `mqtt_exit_test`.

### F4 — nit, carried: re-adding an id with a corrected URL waits out the old lookup `[repro]`

`remove cam`, then `add cam` with the `ok` URL: open after **8.58 s**. Unchanged from AUDIT5, and documented. #33's global cap would now bound the (id, url) keying AUDIT5 proposed.

### F5 — nit, carried and widened: cover art still presents, now also through HLS `[repro]`

- `song.m4a` (`mov`) presents its JPEG cover, as in AUDIT5.
- **New path:** a media playlist with one `#EXTINF` pointing at `song.mp3` (or the same file renamed `song.ts`, or `song.m4a`), opened directly or behind a master, presents the cover with `codec=mjpeg`.
  - #28 checks only the caller's own demuxer (`hls`, allowed).
  - The segment demuxer is `mp3`, which must stay on `kFormatWhitelist` for packed-audio renditions (`src/ffmpeg_decoder.cpp:28-34`).
  - So #28 stops the bare file, but one extra playlist line gets past it.
  - `docs/mqtt.md:41` only claims that a *standalone* `song.mp3` is rejected, which is accurate.
- **Why nit:** AUDIT4 rated A4 minor, and this needs a crafted playlist. Over MQTT it is one still frame and then a retry every second, the same behavior as A4.
- **Fix shape (unchanged, now closes both):** after `av_find_best_stream` (`:187`), fail with `no video stream` when the stream has `AV_DISPOSITION_ATTACHED_PIC`. Add `song.m4a` and `wrap_mp3.m3u8` next to `testRejectCoverMp3`.

### F6 / F7 — nits, carried

- F6: `ffmpeg_decoder_test` builds only an ADTS rendition master. Dropping `ac3`, `eac3`, or `mp3` from `kFormatWhitelist` would stay green.
- F7: `mqtt_exit_test` and `mqtt_reconnect` exit 77 without `/usr/sbin/mosquitto` and are not in `DECODER_TESTS`. Since #34 they are the only tests for F1 and X1. CI installs mosquitto and both ran on `6d9d146`, but a CI image without it would skip them silently.

---

## 4. Considered, not findings

- **`mosquitto_disconnect` from the main thread while the network thread is reconnecting.** It sets state and queues DISCONNECT through the socket pair. Any race with `net__try_connect` writing `mosq->sock` lasts at most 500 ms, and the process `_exit`s.
- **`closeAll()` under `closeMu_`.** `reapFinished` only closes decoders whose loop has already returned and whose `abort()` is latched, so FFmpeg I/O during close returns `AVERROR_EXIT`. A command on the mosquitto thread waits at most that long.
- **Pre-thread-start exits** (signal in the start-up backoff, `loop_start` failure, will-set failure). They call `shutdown()`. `loopStarted_` is false there, so nothing is joined. Nothing was published, so no offline status is owed. #34's "every post-thread-start exit publishes offline" claim holds.
- **Offline status vs will.** When `sent` is false, the broker eventually publishes the will. It was built at the last `publishStatus`, so it carries the same table with `"online": false`.
- **Uncaught exception from `tick()`** (for example `bad_alloc` copying the stream list). `std::terminate` runs, the broker publishes the will, and the launch script's `wait` returns. This is not new.
- **TSan and `_exit`.** TSan reports the intentionally unjoined mosquitto thread as a "thread leak" and makes the process exit 66. If TSan is ever added to CI for `mqtt_exit_test`, it needs `TSAN_OPTIONS=report_thread_leaks=0`.
- **Status size.** 9 URLs of up to 64 KiB give a status of about 590 KB. MQTT allows it, and it is logged as one line.

## 5. Blocked on GS-King X

- N1: the CoreELEC resolver and `nsswitch.conf` (`mdns` for `*.local`), which decide whether a silent lookup lasts 10 s, 20 s, or fails fast.
- F3/N3: armv7 address-space headroom at the 17-thread bound, and `ulimit -s` inside the `amlogic-multistream-launch` unit.
- Which MP3 decoder name CoreELEC's FFmpeg registers first (both are allowed).
- Hardware decode (`multi_vdec` / V4L2 M2M) and DRM/KMS present: out of scope. Not started on this path.

## 6. How this was checked

- **Build and suite:**
  - `./scripts/run-tests.sh` with `AMS_REQUIRE_DECODER_TESTS=1` on `6d9d146`.
  - TSan build (`-fsanitize=thread -O1 -g`) of `stream_player_test`, `stream_player_stall_test`, and `mqtt_exit_test`, with `TSAN_OPTIONS=die_after_fork=0 log_path=…`, so forked player children write their own reports. Only two children (one per broker-up SIGTERM case) wrote a report. Both are the intended thread leak above, with no data race.
  - Under TSan, `mqtt_exit_test` itself fails: those two cases exit 66 instead of 0, and `gone-blackhole` takes 1509 ms against its 1500 ms limit. Both come from running under TSan.
- **Harness** (Python, run as root inside `unshare -m`, not committed, same shape as AUDIT5 section 7):
  - UDP sockets on `127.0.0.2:53` and `127.0.0.3:53` read queries and never answer. `/etc/resolv.conf` and `/etc/hosts` are bind-mounted and rewritten in place (`options timeout:5 attempts:2`). `brokerhost` is mapped, removed, or remapped to `10.255.255.1`, which hangs `connect()` on this VM.
  - A throwaway `mosquitto` per scenario (`persistence false`). `python3 -m http.server` serves a 300 s 10 fps MPEG-2 TS as `ok`. `cam` is `http://camera.lan/live/index.m3u8` (silent lookup).
  - Each MQTT scenario: `add ok`, `add cam`, `start`, 1.5 s, then the scenario.
    - Status messages are recorded with `mosquitto_sub -v`.
    - The retained status is read with `mosquitto_sub -C 1`.
    - Threads and VmSize come from `/proc/PID`.
    - PPM rates come from `--render-dir`.
  - Pre-connect: `--mqtt-host broker.unresolvable`, SIGTERM at 0.5 s or 3 s.
  - Launch e2e: `scripts/launch-coreelec.sh` with `PATH` stub `systemctl` logging timestamps, `PLAYER_BIN` a `sh` wrapper that `exec`s the player, SIGTERM to the script 2.5 s after start.
  - Cap: 12 stuck ids added and removed within 1.2 s, then `add ok2` + `restart ok`. Time until `StreamPlayer open ok2`.
- **Whitelist fixtures:**
  - An H.264 TS VOD video playlist, plus four masters with one `#EXT-X-MEDIA:TYPE=AUDIO` rendition each (`-f segment -segment_format adts|ac3|eac3|mp3`).
  - `song.mp3` (libmp3lame + JPEG `attached_pic`, ID3v2.3), `song.m4a` (AAC + JPEG `attached_pic`), `plain.aac`, and `anim.gif`.
  - `wrap_mp3.m3u8`, `wrap_m4a.m3u8`, `wrap_mp3ts.m3u8` (one `#EXTINF` each), and `master_wrap_mp3.m3u8`.
- **libmosquitto source:** v2.0.18 tag (`lib/net_mosq.c`, `lib/actions.c`, `lib/messages_mosq.c`, `lib/packet_mosq.c`, `lib/connect.c`, `lib/handle_*.c`), for lock order and the `sent` semantics in N2.
