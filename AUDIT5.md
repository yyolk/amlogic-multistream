# AUDIT5: `main` @ bf00f31 after the AUDIT4 fixes (#28–#30)

Audited commit: `bf00f31598d1a170b5a17ccc6137b1766c265e10` (merge of
[#30](https://github.com/yyolk/amlogic-multistream/pull/30)). AUDIT4 base:
`58a2f9c` (closed drafts [#26](https://github.com/yyolk/amlogic-multistream/pull/26)
and [#27](https://github.com/yyolk/amlogic-multistream/pull/27)).

Fixes under audit:

- [#28](https://github.com/yyolk/amlogic-multistream/pull/28) A4 + A5: top-level demuxer allowlist, `mp3float` on the codec allowlist (`7f65ade`).
- [#29](https://github.com/yyolk/amlogic-multistream/pull/29) A3: `reaperBusy`, so `restart` or `stop`+`start` does not stack opens (`1a61dde`).
- [#30](https://github.com/yyolk/amlogic-multistream/pull/30) A1/R1 + A2 (+ A6): gated DISCONNECT, bounded auth give-up with exit code 1 (`bf00f31`).

No production code is changed in this PR. Candidate fixes were built in
throwaway worktrees only.

**Severity:** **must-fix** = breaks a guarantee the code or docs make, on a
reachable path. **should-fix** = real, contained, worth doing before hardware
work. **nit** = polish or test/doc gaps.
**Evidence:** `[repro]` = run on this VM (section 7). `[code]` = from reading
the code. **needs-GS-King-X** = needs the box.

**Environment.** Ubuntu 24.04 x86_64, g++ 13.3, glibc 2.39, FFmpeg 6.1.1,
mosquitto 2.0.18, `nsswitch.conf` `hosts: files dns`.

**Suite.** `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` on `bf00f31`:
**14/14 pass**. The first run skipped one `kodi_addon` case
(`test_device_appimage_marker`: squashfs-tools missing). With squashfs-tools
installed, all 19 add-on cases ran and passed. `mqtt_exit_test`: SIGTERM
2012 ms rc 0 with the table retained, auth give-up 1991 ms rc 1. CI run
41293021266 on `main` ran the same test on aarch64 (not skipped): 2009 ms and 1986 ms.
ThreadSanitizer (`-fsanitize=thread`, `TSAN_OPTIONS=die_after_fork=0`):
`stream_player_test` and `stream_player_stall_test` report 0 warnings.

---

## Verdict

#28–#30 close A1–A6. Restart stacking (A3) is closed only for `restart` and
`stop`/`start`. #20, #23, #24, and #25 behavior is intact.

#30's gated DISCONNECT exposes one new must-fix. A command that arrives during
the 2 s exit drain republishes a retained `"online":true`. DISCONNECT then
tells the broker to drop the will, so after the process exits, the broker
still says the player is online (F1). Before #30, the will overwrote that
status.

| ID | Severity | Finding | From |
| --- | --- | --- | --- |
| **F1** | **must-fix** | A command during the exit drain leaves a dead player retained as `"online":true` | #30 (regression vs `58a2f9c`) |
| F2 | should-fix | `sent` is read before the 2 s drain. A broker drop during the drain makes SIGTERM take 11 s or 21 s, past the launch poll | #30 residual (the race CROSSCHECK-OPUS §1 flagged but did not test) |
| F3 | should-fix | `add`/`remove` with fresh ids still stacks decode threads (94 threads, 3.0 GB VmSize) | A3 residual (#25; #29 gates per id only) |
| F4 | nit | Re-adding an id with a corrected URL waits out the old lookup (8.6 s vs 0.05 s) | #29 side effect (documented) |
| F5 | nit | An `.m4a` with JPEG cover art still opens via `mov` and presents the cover | pre-existing, same class as A4 |
| F6 | nit | Tests do not pin #30's `sent` gate, F1, or F2. Packed-audio fixtures cover ADTS only | #30, #23/#28 |
| F7 | nit | `mqtt_exit_test` and `mqtt_reconnect` exit 77 without the mosquitto daemon and are not required | AUDIT4 N3 carried over |
| F8 | nit | `~StreamPlayer` comment says every exit path calls `exitIfBusy()` and `_exit` | #30 |

---

## 1. AUDIT4 items on `bf00f31`

| Item | Fixed by | Verdict | Evidence |
| --- | --- | --- | --- |
| A1 stale will over final status | #30 | **Closed** | SIGTERM with a stuck lookup `[repro]`: `58a2f9c` takes 2.02 s, sends 2 status messages, and the retained status is the empty will. `bf00f31` takes 2.02 s, sends 1 message, and the retained status keeps `ok` and `cam`. With the broker dropped *before* SIGTERM (DNS name, 1 or 2 silent nameservers): 2.02 s. `mqtt_exit_test` asserts the stream id. |
| A2 auth give-up unbounded | #30 | **Closed** | `finishProcess(1)` at `src/mqtt_controller.cpp:283-289`. `exitIfBusy` takes the code (`src/stream_player.cpp:296-304`). Test: rc 1, 1991 ms after the give-up line. Startup refusal: rc 1 in 0 ms. |
| A3 restart stacks threads | #29 | **Closed for `restart` and `stop`/`start`** | 10 s of `restart {"id":"cam"}` at 10/s `[repro]`: `58a2f9c` ends at 100 threads and 3.05 GB VmSize. `bf00f31` ends at 4 threads and 423 MB. See F3 and F4 for what remains. |
| A4 top-level packed audio | #28 | **Closed** | `song.mp3` and `plain.aac` → `demuxer is not allowed: mp3` / `aac`, rc 1 (section 4). F5 is the same class through `mov`. |
| A5 `mp3float` | #28 | **Closed** | MP3-rendition master: 5 `not on whitelist` lines on `58a2f9c`, 0 on `bf00f31`. |
| A6 exit code / `--help` | #30 | **Closed** | The `sigtimedwait` error path uses `finishProcess(1)` (`:301-306`). `printUsage` says "stop retires them" (`:424`). |

## 2. Earlier fixes, regression check

- **#20 / #25 stall isolation** `[repro]`. `ok` plus a stuck `cam`, presents counted per 0.5 s: 24–25 before, 24–25 after `restart cam`, and 24–25 after `remove cam`. `stream_player_stall` passes (`blackhole+trickle shutdownMs=1`, `live+vod presents=151 shutdownMs=14`).
- **#23 packed-audio HLS** `[repro]`. All five rendition types still present 3 frames on `bf00f31`: ADTS, ADTS behind ID3v2, AC-3, E-AC-3, and MP3.
- **#24 Kodi MQTT credentials** `[repro]`. `kodi_addon` passes 19/19. #28–#30 do not touch the add-on. The auth exit stays non-zero (A2).
- **#25 reaper / bounded exit** `[repro]`. SIGTERM with a stuck worker exits in 2.02 s. The one exception is F2. TSan is clean on the reaper paths.

---

## 3. Findings

### F1 — must-fix, regression from #30: a command during the exit drain leaves a dead player retained as `"online":true` `[repro]`

- **Where:**
  - `handleMessage` (`src/mqtt_controller.cpp:453-478`) runs on the mosquitto thread. Nothing stops it once shutdown has begun.
  - Every mutating command and `list` set `publishStatus` (`src/mqtt_commands.cpp:80-227`). `publishStatus()` (`:480-485`) publishes a retained `"online":true` and refreshes the will.
  - `finishProcess` (`:268-280`) publishes the offline status before the drain. When `sent`, it then calls `shutdown()`, whose DISCONNECT makes the broker discard the will. So the last retained message is whatever the mosquitto thread published during the drain.
- **What:** the harness, with `ok` plus a stuck `cam`. SIGTERM, then `mosquitto_pub …/cmd/list {}` 0.5 s later:

  | Build | Status messages after SIGTERM | Retained after exit |
  | --- | --- | --- |
  | `58a2f9c` | offline (table), `online:true`, will (empty) | `online:false`, `streams:[]` |
  | `bf00f31` | offline (table), `online:true` | **`"online":true,"playing":true`, both streams** |
  | scratch fix below | offline (table) | `online:false`, both streams |

- **Why must-fix:** AUDIT4 rated A1 minor because `"online":false` stayed correct. F1 breaks exactly that field. Home Assistant shows the player online until the next launch.
  - The window is the full 2 s drain whenever a worker is stuck (T1 conditions). It is a few microseconds otherwise; that tiny race is older than #30.
  - `list` is a natural thing for a dashboard or automation to send.
  - Before #30, the empty will overwrote the status and so masked this.
- **Fix shape** (scratch-tested; F1 and F2 together are 33 insertions and 5 deletions in `mqtt_controller.{h,cpp}`):
  - Add `closing_` under a mutex. `finishProcess` takes the mutex, sets `closing_`, builds the offline status, and publishes it while holding the lock.
  - `handleMessage` takes the same mutex. If `closing_` is set, it publishes `{"ok":false,…,"message":"shutting down"}` on `/response` and returns without executing the command or publishing status.
  - Test: in `mqtt_exit_test`'s SIGTERM case, publish `list` after the signal. Assert that the retained status has `"online":false` and still lists `cam`.

### F2 — should-fix, #30 residual: a broker drop during the drain makes SIGTERM exceed the launch poll `[repro]`

- **Where:**
  - `sent` is set at `src/mqtt_controller.cpp:273` and read at `:275-277`, up to 2 s later.
  - `shutdown()` (`:524-538`) calls `mosquitto_loop_stop`, which joins libmosquitto's thread. If the connection dropped during the drain and `--mqtt-host` is a DNS name, that thread is inside the reconnect `getaddrinfo`.
- **What:** broker given as `brokerhost` (bind-mounted `/etc/hosts`). SIGTERM, then 0.2 s later stop the broker and remove the name:

  | Build | 1 silent nameserver | 2 silent nameservers |
  | --- | --- | --- |
  | `bf00f31` | **11.04 s** | **21.07 s** |
  | scratch fix below | 1.82 s | 1.82 s |

  Times are measured from 0.2 s after SIGTERM. `scripts/launch-coreelec.sh:154-163` SIGKILLs at 10 s. `README.md:65` ("keeps the exit inside the launch script's poll") and `docs/mqtt.md:54` promise the bound.
- **Why should-fix, not must-fix:** it needs three things at once: the broker lost inside the same 2 s window as a SIGTERM with a stuck worker, a broker configured by name, and a silent resolver. The add-on default `localhost` resolves from `/etc/hosts`. The cost is Kodi coming back at about 11 s instead of 2 s. CROSSCHECK-OPUS §1 named this race as untested.
- **Fix shape** (scratch-tested together with F1):
  - On the stuck path, do not call `shutdown()`. Call `mosquitto_disconnect()`. If it returns `MOSQ_ERR_SUCCESS`, wait up to 500 ms on a condition variable that `onDisconnect` signals, then `_exit`. libmosquitto 2.0 calls `on_disconnect(0)` after the DISCONNECT is written.
  - Never call `mosquitto_loop_stop` on that path.
  - Reset the flag before `mosquitto_disconnect()`, so an earlier drop cannot satisfy the wait.
  - Results: plain SIGTERM still exits in 2.02 s with one status message and the table kept. The broker-drop cases take about 2 s with one or two nameservers. The full suite passes 14/14 on that tree.

### F3 — should-fix, A3 residual: `add`/`remove` with fresh ids still stacks decode threads `[repro]`

- **Where:** `reaperBusy` is keyed by id (`src/stream_player.cpp:159-170`, gate at `:430-432`). Nothing caps the number of not-done workers on `reaper_`.
- **What:** stuck URL added as `c0`, `c1`, … and removed 50 ms later, for 10 s (97 cycles):
  - `bf00f31`: 94 threads, 3.00 GB VmSize. The same run on `58a2f9c`: 95 threads, 3.01 GB.
  - 12 s later: 3 threads, but VmSize stays at 2.28 GB (glibc per-thread arenas).
- **Why should-fix:**
  - `docs/mqtt.md:39` says churn "cannot stack decode threads". That holds for `restart` and `stop`/`start` only.
  - The CE21 armv7 AppImage is a 32-bit process, so a few hundred stacks exhaust its address space (**needs-GS-King-X**).
  - This is less likely than restart churn, because it needs a client that mints new ids.
- **Fix shape:** AUDIT4's alternative was a global cap. Before `startWorker`, count the not-done workers on `reaper_`. At `2 × LayoutEngine::kMaxStreams` (18) or more, treat the stream as in backoff. Test: `StallingDecoder` with 50 `add`/`remove` cycles under fresh ids, asserting that `maxInFlight` stays at or under the cap.

### F4 — nit, #29 side effect: re-adding an id with a corrected URL waits out the old lookup `[repro]`

- **What:** `remove cam`, then `add cam` with a working URL. The new open starts after **8.59 s** on `bf00f31` and after 0.05 s on `58a2f9c`.
- **Why it matters:** `add` refuses a different URL for an existing id (`src/stream_manager.cpp:55-62`). So `remove` plus `add` is the only way to fix a URL whose hostname hangs, and that is exactly the case where the old worker is stuck.
- **Why nit:** the delay is documented (`docs/mqtt.md:39`, "another open for the same id is not started until then") and bounded by the resolver timeout.
- **Fix shape:** key the gate on (id, url) by storing the URL in `Worker`, and let F3's global cap bound URL rotation.

### F5 — nit, pre-existing, same class as A4: an audio file with cover art still opens via `mov` `[repro]`

- **What:** `song.m4a` is AAC plus a JPEG `attached_pic`. `--url … --frames 3` gives rc 0 and 1 PPM of the cover (`codec=mjpeg`) on both `58a2f9c` and `bf00f31`.
- **Why it happens:**
  - `mov` must stay on `kTopLevelFormatWhitelist` (`src/ffmpeg_decoder.cpp:34`) for MP4 and fMP4.
  - `av_find_best_stream` (`:187`) returns the `attached_pic` stream when it is the only video stream.
  - Over MQTT this presents one still and then retries every second, like A4 did. `docs/mqtt.md:41` only claims `song.mp3`, so the doc is accurate.
- **Fix shape:** after `av_find_best_stream`, fail with `no video stream` when the chosen stream has `AV_DISPOSITION_ATTACHED_PIC`. That closes the class for every container. Add `song.m4a` next to `testRejectCoverMp3` (`tests/ffmpeg_decoder_test.cpp:573-631`).

### F6 — nit: tests do not pin #30's gate, F1, F2, or most packed-audio types `[repro]`

- **Gate:** `mqtt_exit_test` connects to `127.0.0.1` (`tests/mqtt_exit_test.cpp:286-287`), so `loop_stop` is never stuck. A scratch tree with the gate removed calls `shutdown()` unconditionally, which is the A1 fix AUDIT4 later rejected. On that tree, `mqtt_exit_test` still passes (table kept, auth 1990 ms rc 1), while the broker-dropped-before-SIGTERM case goes from 2.02 s to **7.53 s**.
- **Fix shape:**
  - Define `getaddrinfo` in `mqtt_exit_test.cpp`. The executable's symbol interposes libmosquitto's call. Forward to `dlsym(RTLD_NEXT, …)`, except for a magic name such as `stall.invalid`, which sleeps.
  - Add three cases: broker dropped before SIGTERM, broker dropped during the drain (F2), and a command during the drain (F1).
- **Packed audio:** `ffmpeg_decoder_tests` builds only an ADTS rendition master. The AC-3, E-AC-3, MP3, and ID3-ADTS renditions are checked only by audit harnesses. Dropping `ac3`, `eac3`, or `mp3` from `kFormatWhitelist` would stay green.

### F7 — nit, AUDIT4 N3 carried over: the MQTT regression tests are skippable `[code]`

- `mqtt_exit_test` (A1/A2) exits 77 without the mosquitto daemon (`tests/mqtt_exit_test.cpp:559-562`). `mqtt_reconnect` (K1) does the same.
- Neither is in `DECODER_TESTS` (`scripts/ctest-require.sh:5-9`).
- CI installs mosquitto, and both ran in run 41293021266. A CI image without the daemon would skip them silently.
- **Fix:** require both under the same `AMS_REQUIRE_DECODER_TESTS=1` gate.

### F8 — nit: `~StreamPlayer` comment overstates the exit paths `[code]`

`src/stream_player.cpp:64-67` says signal exit, a failed `sigtimedwait`, and auth give-up "all call exitIfBusy() and _exit before this destructor runs". In fact they call `drain()`, and `exitIfBusy()`/`_exit` only when that drain fails (`src/mqtt_controller.cpp:274-279`). On the normal path the destructor does run, with an empty reaper. The comment's second sentence is correct. Reword the first.

---

## 4. Requested re-checks

- **MQTT exit, will, and disconnect:** A1 is closed. F1 (must-fix) and F2 (should-fix) remain.
  - Broker down before SIGTERM: 2.02 s, so the gate works.
  - SIGHUP coverage in `scripts/test-mqtt-reconnect.sh:232-267` still has no live worker (AUDIT4 note). `mqtt_exit_test` now covers SIGTERM with one.
- **Auth give-up exit code:** rc 1 and bounded, both mid-session with a stuck worker (1991 ms) and at startup (0 ms).
  - At give-up the broker has already refused the connection, so `publishRaw` fails and `sent` is false.
  - The stuck path then `_exit(1)`s without `loop_stop` `[code]`.
- **Restart stacking:** closed for `restart` and `stop`/`start` (4 threads vs 100). Id rotation (F3) and same-id URL replacement latency (F4) remain.
- **Demuxer and codec whitelists** (`--url` over HTTP, 3 frames) `[repro]`:

  | Input | `58a2f9c` | `bf00f31` |
  | --- | --- | --- |
  | HLS + ADTS / ADTS-ID3 / AC-3 / E-AC-3 rendition | rc 0, 3 frames | rc 0, 3 frames |
  | HLS + MP3 rendition | rc 0, 3 frames, 5 `not on whitelist` | rc 0, 3 frames, 0 warnings |
  | `song.mp3` (ID3 APIC JPEG) | rc 0, cover presented | **rc 1, `demuxer is not allowed: mp3`** |
  | `plain.aac` | rc 1, `no video stream` | rc 1, `demuxer is not allowed: aac` |
  | `song.m4a` (JPEG cover) | rc 0, cover presented | rc 0, cover presented (F5) |

- **Stall isolation and reaper:** 24–25 presents per 0.5 s across `restart` and `remove` of a stuck stream. TSan is clean on the threaded player tests.
  - Restarts are deferred until the old lookup returns (#29).
  - The deferred restart runs once, not once per command (`testRestartDoesNotStackUninterruptibleOpen`).

## 5. Considered, not findings

- **`mosquitto_lib_cleanup()` with a decode thread still in `getaddrinfo`** (the gated `sent` path). That worker is aborted, so FFmpeg's next interruptible step fails before TLS, and the process `_exit`s right away. No effect was seen. F2's fix removes `shutdown()` from that path anyway.
- **`reaperBusy` reads `worker->id` without the lock.** The id is written before the thread starts and never changes.
- **`shutdown()` runs twice after `finishProcess` returns.** The second call is a no-op: `mosq_` is null and `libInitialized_` is false.
- **The A3 test's exact count** `entered == enteredAtStall + 1` matches the tick logic: one deferred replacement after release.

## 6. Blocked on GS-King X

- F3: armv7 address-space headroom, and `ulimit -s` inside the `amlogic-multistream-launch` unit.
- Whether CoreELEC's resolver waits out the glibc timeouts or fails fast. That decides how often F1–F4 can happen at all.
- Which MP3 decoder CoreELEC's FFmpeg registers first. A5 lists both `mp3` and `mp3float`, so either works.
- Hardware decode and DRM/KMS: out of scope.

## 7. How this was checked

- **Builds:**
  - `bf00f31` via `scripts/run-tests.sh`.
  - Worktrees at `58a2f9c` (AUDIT4 base) and at `bf00f31`, plus two scratch variants:
    - `nogate`: unconditional `shutdown()`.
    - `fix`: F1 + F2, 33 insertions and 5 deletions in `mqtt_controller.{h,cpp}`.
  - Each built with `cmake -DCMAKE_CXX_COMPILER=g++`. Nothing from the scratch trees is pushed.
- **Exit and churn harness** (run as root inside `unshare -m`, AUDIT4 section 7 style):
  - A Python UDP socket on `127.0.0.2:53` (and `127.0.0.3:53`) reads queries and never answers. `resolv.conf` lists them with `options timeout:5 attempts:2`. A bind-mounted `/etc/hosts` holds `brokerhost`, which the broker-drop cases rewrite.
  - A throwaway mosquitto on `127.0.0.1` (`persistence false`), and `python3 -m http.server` serving a 300 s 10 fps MPEG-2 TS clip as `ok`. `cam` is `http://camera.lan/live/index.m3u8`.
  - Each run: `add ok`, `add cam`, `start`, 1.5 s, then the scenario:
    - **sigterm:** SIGTERM; time the exit; count `<prefix>/status` messages; read the retained status with `mosquitto_sub -C 1`.
    - **sigterm_cmd:** as above, plus `list` 0.5 s after SIGTERM.
    - **brokerdrop_before / during_drain:** `--mqtt-host brokerhost`; stop the broker and drop the name 3.5 s before SIGTERM, or 0.2 s after it.
    - **restart_churn:** `restart cam` every 100 ms for 10 s. Then read `/proc/PID/task` and `VmSize`.
    - **id_rotation_churn:** `add cN` (stuck URL), 50 ms, `remove cN`, 50 ms, for 10 s.
    - **readd_fixed_url:** `remove cam`, then `add cam` with the `ok` URL. Time until `StreamPlayer open cam`.
    - **wall:** `--render-dir`; count PPMs per 0.5 s around `restart cam` and `remove cam`.
- **Whitelist fixtures:**
  - One H.264 TS video playlist. Five masters, each with one `#EXT-X-MEDIA:TYPE=AUDIO` rendition segmented with `-f segment -segment_format adts|ac3|eac3|mp3`; ID3 via `-segment_format_options write_id3v2=1`.
  - `song.mp3` (libmp3lame plus a JPEG `attached_pic`, ID3v2.3), `plain.aac` (ADTS), and `song.m4a` (AAC plus a JPEG `attached_pic`).
  - Run as `--url http://127.0.0.1:…/X --frames 3 --render-dir DIR` on both builds.
- **TSan:** `-fsanitize=thread -O1 -g` in a separate build dir. `stream_player_test` and `stream_player_stall_test` run with `TSAN_OPTIONS=die_after_fork=0`. `mqtt_exit_test` also passed under TSan, but its forked players log to temp files that are deleted on a pass, so those children are not counted as checked.
