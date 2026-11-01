# AUDIT7-B: independent second look of `main` @ c597b16 after #47

Audited commit: `c597b16bfdb9456d5a38e64890ae5a9d6255c12a` (merge of
[#47](https://github.com/yyolk/amlogic-multistream/pull/47), AUDIT6 N1).
Parent: `6d9d146` (merge of [#34](https://github.com/yyolk/amlogic-multistream/pull/34)).
This pass did not read the other AUDIT7 review.

No production code is changed in this PR. Nothing was fixed.

**Severity:** **must-fix** = breaks a guarantee the code or docs make, on a
reachable path, in a way that leaves the wrong retained status or blows the
launch-script poll. **should-fix** = real, contained, worth doing before
hardware work. **nit** = polish, a doc/comment mismatch, or a test gap.
**Evidence:** `[repro]` = run on this VM. `[code]` = from reading the code.

**Environment.** Ubuntu 24.04 x86_64, g++ 13.3, glibc 2.39, FFmpeg 6.1.1
(libavformat 60.16.100), libmosquitto / mosquitto 2.0.18.

**Suite.** `ctest --test-dir build --output-on-failure` on `c597b16`:
**14/14 pass**, none skipped (mosquitto and squashfs-tools installed).
`mqtt_exit_test`: sigterm 2011 ms, drain-cmd 2007 ms, gone-dns 20 ms,
gone-blackhole 502 ms, drop-drain 2011 ms, auth give-up exit 2010 ms rc 1,
startup auth rc 1, **startup-dns 20 ms rc 0**.

---

## Survivors

No must-fix and no should-fix remains. N1 is closed. F1, X1, F3, and A1–A5
stay closed. One new nit (N5) comes from #47. The carried nits are unchanged
in code; F5 was re-opened on this tree and still presents a cover.

| ID | Severity | Finding | From |
| --- | --- | --- | --- |
| N5 | nit | A SIGTERM in the startup `sigtimedwait`, after `connect_async` has already written CONNECT, `_exit`s without DISCONNECT. The broker publishes the empty startup will and replaces the previous retained status. The player log has no `MQTT status` line. `online` stays false | #47 |
| N2 | nit | `sent` is true while the socket exists but MQTT is not up, so a blackhole broker still pays the 500 ms DISCONNECT wait. Measured again: 515 ms | #34, reconfirmed |
| N3 | nit | A full retired-decoder cap also delays a healthy `restart` or retry. The gate is unchanged | #33, reconfirmed `[code]` |
| N4 | nit | `stream_player.h` still says process exit `_exit`s only when a worker is stuck. MQTT mode always `_exit`s | #34, reconfirmed `[code]` |
| F4 | nit | Re-adding an id whose lookup is stuck still waits out that lookup | carried `[code]` |
| F5 | nit | `song.m4a` and a one-entry HLS playlist of `song.mp3` or `song.m4a` still present the JPEG cover | carried, reconfirmed `[repro]` |
| F6 | nit | Packed-audio fixtures in `ffmpeg_decoder_test` are still ADTS only | carried `[code]` |
| F7 | nit | `mqtt_exit_test` and `mqtt_reconnect` still exit 77 without the daemon and are not required by `scripts/ctest-require.sh` | carried `[code]` |

---

## Closed on this tree

| Item | Fixed by | Verdict | Evidence |
| --- | --- | --- | --- |
| **N1** startup `getaddrinfo` blocks SIGTERM | #47 | **Closed** | `[repro]` below. Unit test 20 ms. Real silent resolver 3 ms, while a control lookup was still blocked |
| **F1** command during the drain leaves `"online":true` | #34 | **Closed** | `[repro]` drain-cmd 2007 ms, retained `online:false` with `cam`. `closing_` is still set under `closeMu_` with the offline publish (`src/mqtt_controller.cpp:337-343`). `handleMessage` (`:519-524`) and `handleConnect` (`:490-492`) still stop on it |
| **X1** exit joins the mosquitto thread | #34 | **Closed** | `[repro]` gone-dns 20 ms, gone-blackhole 502 ms, drop-drain 2011 ms. `finishProcess` (`:334-358`) still never calls `mosquitto_loop_stop`. #47 does not touch it |
| **F3** fresh-id churn stacks decode threads | #33 | **Closed** | `[repro]` `stream_player_test` passed, including the cap case (`entered == kMaxBusyRetired`). `src/stream_player.cpp` was not changed by #47 |
| **A1** stuck exit keeps the table, no stale empty will | #30 | **Closed** | `[repro]` sigterm 2011 ms, retained `online:false` and `cam` |
| **A2** auth give-up exits 1 inside the drain bound | #30 | **Closed** | `[repro]` mid-session rc 1, `waitExitMs=2010`. Startup refusal rc 1 |
| **A3** `restart` / `stop`+`start` stacks opens | #29 | **Closed** | `[repro]` `stream_player_test` passed (`maxInFlight <= 1` on the per-id cases). Gate still at `src/stream_player.cpp:444-446` |
| **A4** top-level packed audio | #28 | **Closed** | `[repro]` `song.mp3`, real ADTS `plain.aac`, and `plain.ac3` reject with `demuxer is not allowed` |
| **A5** `mp3float` on the codec allowlist | #28 | **Closed** | `[code]` `kCodecWhitelist` still lists `mp3float` (`src/ffmpeg_decoder.cpp:42-43`). The cover-art opens below did not log a whitelist miss |
| **F8** destructor comment | #34 | **Closed** | `[code]` `src/stream_player.cpp:62-67` matches `finishProcess` |

---

## N1 — closed `[repro]`

#47 runs the first `mosquitto_connect_async` on a helper and leaves the main
thread in `sigtimedwait` (`src/mqtt_controller.cpp:233-281`). libmosquitto
2.0.18 still calls blocking `getaddrinfo` inside that call even when the TCP
`connect` is non-blocking (`net__try_connect_tcp`). Moving the call off the
main thread is the right fix.

Two checks:

1. **Unit test** (`mqtt_exit_test` `testStartupStalledLookup`). The stall file
   exists before spawn, so the interposed `getaddrinfo` sleeps 60 s.
   SIGTERM after the entered marker: **20 ms, rc 0**. The log contains
   `MQTT shutting down on signal 15` and does not contain `MQTT running` or
   `MQTT status`.

2. **Production binary, real resolver.** Network namespace, `lo` addresses
   `192.0.2.2` and `192.0.2.3`, a UDP socket on each port 53 that reads and
   never answers, `resolv.conf` with those two nameservers and
   `options timeout:5 attempts:1` (about 10 s if the caller waits). A control
   `getaddrinfo("broker.audit7b.invalid")` was still blocked when the player
   exited.

   | Thread | `wchan` |
   | --- | --- |
   | main | `do_sigtimedwait` |
   | helper | `do_poll` (inside `getaddrinfo`) |

   SIGTERM: **3 ms, rc 0**. Stdout ends at `MQTT shutting down on signal 15`.
   No `MQTT running`, no `MQTT status`. The control lookup was still blocked
   afterward, so the process did not wait out the resolver.

Same path for the fast-failure backoff, which is the other place the old loop
slept on the main thread. `--mqtt-host 127.0.0.1 --mqtt-port 1`, SIGTERM /
SIGHUP / SIGINT at 350 ms, while stderr already shows
`MQTT connect failed: Connection refused (will retry)`:

| Signal | Exit |
| --- | --- |
| SIGTERM | 3 ms, rc 0, log `signal 15` |
| SIGHUP | 1 ms, rc 0, log `signal 1` |
| SIGINT | 3 ms, rc 0, log `signal 2` |

Post-connect exit is unchanged. `--mqtt-host 203.0.113.5` reaches
`MQTT running` (numeric address, non-blocking `connect`) and SIGTERM takes
**515 ms**, inside the 2.5 s post-connect bound. That 515 ms is N2, not N1.

---

## N5 — nit, #47: pre-loop SIGTERM can still publish the startup will `[repro]`

`connect_async` on libmosquitto 2.0.18 writes MQTT CONNECT, including the
will, before it returns when TCP connects immediately (`mosquitto__reconnect`
calls `send__connect` unless `rc > 0`). The helper stores `connectReady` only
after that return. The main thread does not look at the flag again once
`sigtimedwait` has returned a signal (`src/mqtt_controller.cpp:262-272`).

A signal that arrives during that 20 ms wait is taken even if the helper has
already finished CONNECT (`src/mqtt_controller.cpp:262-272` calls `_exit(0)`
without reading `connectReady` again). The comments and `docs/mqtt.md` say a
signal before `mosquitto_loop_start` has not published yet. That is true
while the helper is still inside `getaddrinfo` (N1). It is not true for an
immediately reachable broker.

**What:** local mosquitto. Retained `audit7b/status` seeded as `playing:true`
with stream `old` (139 bytes). Player started, SIGTERM 20 ms later.

| | Result |
| --- | --- |
| Player log | `MQTT shutting down on signal 15`. No `MQTT running`, no `MQTT status`, no `MQTT disconnected`. Exit **1 ms**, rc 0 |
| Broker log | `Will message specified (84 bytes)`, CONNACK, then `Client a7-live closed its connection` with no DISCONNECT |
| Retained after | `{"online":false,"playing":false,...,"streams":[]}` |

The same tree, SIGTERM at 150 ms, has already logged `MQTT connected` and
`MQTT status {"online":true...}`. It then takes `finishProcess`: offline
status, `MQTT disconnected`, retained `online:false`. That path is the #34
behavior and is fine.

**Why nit, not should-fix:** the will written at startup is the empty offline
snapshot (`refreshWill(true)` runs before any command). A clean
`finishProcess` at that same moment stores the same payload. `online` does
not stick at true, and the exit is faster than the launch poll, not slower.
The DNS-stall path does not send CONNECT at all (no `MQTT status`, and the
helper is still in `poll`). The damage is the false "has not published"
claim, a missing status line, and an unclean disconnect instead of
DISCONNECT.

**Fix shape** (not applied): if `sigtimedwait` returns a signal and
`connectReady` is already true, join the helper, `loop_start`, and
`finishProcess` instead of `_exit`. The leftover window is CONNECT already on
the wire but the flag not stored yet, which is the tail of `send__connect`,
not the 20 ms tick.

---

## Prior nits, reconfirmed

### N2 `[repro]`

`--mqtt-host 203.0.113.5`, SIGTERM at 0.25 s and at 1.2 s. Both have reached
`MQTT running` and both exit in **515 ms** after logging
`MQTT status {"online":false,...,"streams":[]}`. Nothing is listening on that
address, so the publish did not reach a broker. `publishRaw` still returns
true (`src/mqtt_controller.h:69-71` still says "a live connection"), `sent`
stays true, and `finishProcess` waits `kDisconnectWait` (500 ms). The cap
holds. Same rating as AUDIT6.

### N3, N4, F4 `[code]`

#47 does not change `src/stream_player.cpp`. The cap check
(`busyRetired >= kMaxBusyRetired`, `:450-458`) runs for every new open,
including the open that follows `retireWorker` on `restart`. `reaperBusy`
(`:444-446`) still blocks a second open for that id until the stuck lookup
returns (F4). `src/stream_player.h:31-34` still says exit `_exit`s only "if a
worker is still inside that call". `finishProcess` always `_exit`s.
`exitIfBusy` is still only called from `tests/stream_player_test.cpp`.

### F5 `[repro]`

`FfmpegDecoder::open` with `setAllowLocalFile(false)`, the MQTT allowlist.
HTTP server on `127.0.0.1`.

| Input | Result |
| --- | --- |
| `song.mp3` (ID3 APIC JPEG) | rejected, `demuxer is not allowed: mp3` |
| ADTS `plain.aac` | rejected, `demuxer is not allowed: aac` |
| `plain.ac3` | rejected, `demuxer is not allowed: ac3` |
| `clip.gif` | rejected, format not on the whitelist |
| `ok.ts` (H.264 MPEG-TS) | opened, `codec=h264`, one 64x48 frame |
| `song.m4a` (AAC + JPEG `attached_pic`) | **opened, `codec=mjpeg`, 32x24** |
| `wrap_mp3.m3u8` (one `#EXTINF` → `song.mp3`) | **opened, `codec=mjpeg`, 16x16** |
| `wrap_m4a.m3u8` | **opened, `codec=mjpeg`, 32x24** |
| HLS segment `file:///tmp/audit7b-secret.ts` | rejected, `Protocol 'file' not on whitelist` |
| HLS AES key `file:///tmp/...` | rejected, same whitelist line |
| `pipe:` segment, ffconcat body, `plain.m3u` of a `file://` entry | rejected |
| HTTP 302 from `/redir-ok` to `ok.ts` | opened as H.264 |
| HTTP 302 to `file:///tmp/audit7b-secret.ts` | rejected, local file not opened |

There is still no `AV_DISPOSITION_ATTACHED_PIC` check after
`av_find_best_stream` (`src/ffmpeg_decoder.cpp:187-189`). The top-level list
allows `hls` and `mov`, and `mp3` stays on `kFormatWhitelist` for packed-audio
renditions, so one playlist line still gets around #28. `docs/mqtt.md` only
claims a standalone `song.mp3` is rejected, which remains accurate.

### F6, F7 `[code]`

`tests/ffmpeg_decoder_test.cpp` builds an ADTS rendition master and a
standalone `song.mp3`. It does not build AC-3, E-AC-3, or MP3 renditions.
`scripts/ctest-require.sh` still requires only `ffmpeg_decoder`,
`present_smoke`, and `stream_player_http`. `mqtt_exit_test` is the test that
pins N1, F1, and X1, and it still skips with status 77 when `mosquitto` is
absent. On this VM the daemon was installed and the test ran.

---

## #47 regression check

- **Helper vs main on `mosq_`.** Only the helper calls into libmosquitto until
  `connector.join()` (`src/mqtt_controller.cpp:282`). A signal before that
  `_exit`s and does not destroy the client. `mosquitto_loop_start` runs after
  the join, so it does not race `connect_async`. `mosquitto_threaded_set` is
  still implied by `loop_start`, which is the right point: there is no second
  caller before then.
- **Signals are blocked before the helper is created** (`:211-216`), and the
  new thread inherits the mask. The real-resolver probe shows the helper in
  `do_poll` and the main thread in `do_sigtimedwait`, so SIGTERM is not
  delivered to the lookup thread.
- **`_exit` does not run the joinable `std::thread` destructor.** Returning
  from `start()` with the helper still running would `std::terminate`. The
  signal and `sigtimedwait` error paths `_exit`. The constructor-failure path
  returns 1 only when the thread was not started (`:256-259`).
- **Post-connect `finishProcess` is the #34 function.** Blackhole, drain
  command, broker-gone DNS, and auth give-up timings match AUDIT6.
- **Allowlists.** `file`, `pipe`, and a `file://` redirect did not open a
  local file. A4/A5 rejections still hold. F5 is the remaining hole and is
  the one AUDIT6 already tracked.
- **stdio lock during the stall.** Not hit. During the real lookup the helper
  is in `poll`, not in `operator<<`. A refused-port SIGTERM, which does
  overlap the helper's `MQTT connect failed` line with the main thread's
  flush, still exited in 3 ms.

## Considered, not findings

- **Pre-connect `_exit` while `getaddrinfo` holds `malloc`.** The long part of
  a silent lookup is `poll`. The 3 ms exit on a live stall is that path. A
  kill in the middle of the short allocate/parse window is not the hang N1
  was.
- **Startup SIGTERM preserving the previous stream list.** Once CONNECT has
  been sent, the empty will replaces it (N5). A clean shutdown before any
  command publishes that same empty table, so this is not an online-status
  regression.
- **Auth time-to-give-up.** `observedMs=15803` in `mqtt_exit_test` is the
  reconnect backoff until the third refusal plus the drain, measured from
  before the give-up line. The exit after that line is `waitExitMs=2010`,
  rc 1. The bound A2 cares about still holds.
- **Reaper ceiling.** Unchanged: a start requires fewer than 9 busy retired
  workers and the table holds at most 9 live sessions, so the running decode
  threads stay at or under 17. The churn test still stops at 9 stuck opens.

---

## CROSSCHECK-B

Peer: [AUDIT7.md](https://github.com/yyolk/amlogic-multistream/pull/50) on
draft [#50](https://github.com/yyolk/amlogic-multistream/pull/50), audited
`main` @ `25bfa8d`. This file was written against `c597b16`. `git diff
c597b16..25bfa8d` has no production changes (audit write-ups and
`docs/research/` only), so the host-side code is the same tree. Nothing
below is a fix.

The N5 paragraph above stands as the first-pass rating. This section revises
it.

### Their findings

| ID | Their rating | This pass | Reason |
| --- | --- | --- | --- |
| **B1** | should-fix | **Agree, should-fix.** Same defect as N5. Revise N5 from nit to should-fix | Both runs saw SIGTERM before `MQTT running` arm the empty startup will and replace a seeded retained status, with no `MQTT status` line. Their 40-launch sample is 29 kept / 3 replaced / 8 already in `finishProcess`, and all 3 replacements were the 30 ms delay (3 of 4). My 20 ms localhost run is one of those replacements. Not must-fix: `online` stays false, the exit is a few milliseconds, and a signal after `MQTT running` publishes the same empty object |
| **B2** | should-fix | **Agree, should-fix.** I missed it | `[repro]` on `c597b16`: `--url http://127.0.0.1:8766/file_seg.m3u8` whose only segment is `file:///tmp/audit7b-secret.mp4` exits **rc 0, `codec=mpeg4`**. The same URL with `setAllowLocalFile(false)` is rejected (`Protocol 'file' not on whitelist`). `decodeUrl` sets the flag for every `--url` (`src/app.cpp:181-183`), including `http://`. `docs/mqtt.md` says `file` is only for the `--url` local-file path. Not must-fix: MQTT `add` never sets the flag, and a 302 to `file://` still fails |

No other findings. Their carried-nit list is N2, N3, N4, F4, F5, F6, F7.

**Refine B1's fix, and drop mine.** Re-checking `connectReady` and then
calling `finishProcess` does not keep the previous status: the new process's
table is empty, so the explicit offline publish wipes the sentinel too. The
preserving fix is to leave `getaddrinfo` on the helper and not call
`mosquitto_connect_async` until the main thread has committed to
`loop_start`. A signal before that call still `_exit`s, and CONNECT has not
been written. Rewording the "has not published yet" sentence without moving
CONNECT leaves the window in place.

### My findings

| ID | First pass | Peer | After this pass |
| --- | --- | --- | --- |
| **N5** | nit | Filed as **B1**, should-fix. Not missed | **Revise to should-fix.** Same item as B1. The first-pass comparison to `finishProcess` was the wrong exit: the pre-loop `_exit` is supposed to leave the broker alone, and the lookup path does |
| **N2** | nit | Kept nit. Blackhole from launch **516 ms** (my 515 ms) | **Keep nit** |
| **N3** | nit | Kept nit. Healthy `add` under a full cap waited **6.95 s**. `restart` of an already-open tile was not re-timed; it uses the same `continue` | **Keep nit** |
| **N4** | nit | Kept nit. Same header lines | **Keep nit** |
| **F4** | nit | Kept nit. Re-add opened **5.61 s** later (remainder of their 10 s stall; AUDIT6 was 8.58 s) | **Keep nit** |
| **F5** | nit | Kept nit, and confirmed it on the MQTT path as well as `--url` | **Keep nit.** Not B2: the cover is an allowed `http`/`mov`/`mjpeg` open, not a `file` segment |
| **F6** | nit | Kept nit | **Keep nit** |
| **F7** | nit | Kept nit. Notes `mqtt_exit_test` is now also the N1 guard | **Keep nit** |

### Agreed survivors

No must-fix. Both passes keep two should-fix items:

| ID | Severity | Summary |
| --- | --- | --- |
| **B1** (N5) | should-fix | SIGTERM in the ~20 ms after the startup helper has written CONNECT, and before `MQTT running`, `_exit`s without DISCONNECT. The broker publishes the empty startup will and drops the previous retained status. Delay CONNECT until the main thread commits to `loop_start`. |
| **B2** | should-fix | `--url http://…` turns on the `file` protocol allowlist, so a playlist segment of `file:///…` decodes a local file. MQTT `add` of that URL does not. Set the flag only for a non-HTTP `--url`. |
