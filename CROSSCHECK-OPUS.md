# Cross-check of Gemini's `AUDIT4-GEMINI.md` against REVIEW4, SURVIVING4, and `main` @ 58a2f9c

Checked: `AUDIT4-GEMINI.md` at commit `cbacbf5` on `cursor/audit4-gemini-b345`
(draft [#26](https://github.com/yyolk/amlogic-multistream/pull/26)), against
`REVIEW4.md` (`33f302e`, [#21](https://github.com/yyolk/amlogic-multistream/pull/21)),
`SURVIVING4.md` (`2cd75dd`, [#22](https://github.com/yyolk/amlogic-multistream/pull/22)),
and `main` @ `58a2f9c114daf7835c7b2b3c8c3201b9a350705c`. My own findings
(A1–A6) are in `AUDIT4.md` in this PR. No production code was changed.
Candidate fixes were built only in throwaway worktrees.

Same VM and harness as `AUDIT4.md` section 7, plus one new scenario
(section 6 here). `[repro]` = run on this VM. `[code]` = from reading the code.

## Bottom line

- **R1 (will wipe) is real.** It is the same defect as A1, and my re-run
  matches Gemini's numbers. **Gemini's fix is not safe as written**, and
  neither was my A1 scratch fix. Calling `shutdown()` before `_exit(0)`
  joins libmosquitto's network thread. If the broker connection is down
  and the broker is configured by DNS name, that thread is inside a
  blocking `getaddrinfo`. SIGTERM then takes 8.53 s with one silent
  nameserver and 18.56 s with two, against 2.02 s on `main`. 18.56 s is past
  the launch script's 10 s poll, so the player gets SIGKILLed, which is
  T1's original symptom. A gated fix keeps 2.02 s in both cases (section 1).
  I corrected A1's fix in `AUDIT4.md` to match.
- **R2 is real.** Its auth half is A2. Its fix for the `sigtimedwait` half
  cannot run: `finishProcess()` already `_exit(0)`s first (section 2).
- **Disagree:**
  - R1's severity (major vs minor). Both audits call it must-fix.
  - Gemini's claim that dropping CI2(c) is why R1 slipped.
  - "#23 clean".
  - "Rebase fallout clean".
  - "K1 and H1 cleanly fixed".
- **Gemini misses:** A3, A4, A5, A2's stale docs and comments, A2's blocked
  signals, and O1 still reproducing. It also repeats SURVIVING4's
  `mp3`-decoder premise.
- **Gemini false positives:** none. Every finding Gemini raises is real.
  The errors are in supporting details: two off-by-one line citations, one
  doc paraphrase, the R1 causal claim, and the R2 fix.

## 1. R1 (Gemini: major) — `_exit(0)` before `mosquitto_disconnect()` lets the stale will wipe the table

**Agree:**

- **Defect and mechanism.**
  - The broker holds the will from the first CONNECT, which has an empty table.
  - `refreshWill(false)` (`src/mqtt_controller.cpp:472`) only changes what the next CONNECT carries.
  - `exitIfBusy` (`src/stream_player.cpp:281-289`) `_exit(0)`s before `shutdown()` at `:298` can send DISCONNECT.
- **Same numbers.** Gemini measured 2003 ms, rc 0, two status messages, and the retained empty will. I re-ran `main` today: 2.018 s, rc 0, two messages, and the retained status is `{"online":false,"playing":false,"layout":"1x1","layout_setting":"auto","streams":[]}` `[repro]`.
- **The SIGHUP test cannot catch it.** `scripts/test-mqtt-reconnect.sh:233-235` adds `cam1` but never sends `start`, so no worker exists when the drain runs. Its URL (`http://127.0.0.1/live.m3u8`) is refused at once and honors `abort()` in any case. The check at `:265-267` therefore passes on `58a2f9c` `[code]`.

**Disagree:**

1. **Severity.** I keep it at minor.
   - `"online":false` is still correct, so Home Assistant's availability is right.
   - What is wrong is the table, the layout, and `playing` of a player that is already offline, and only under T1's conditions.
   - Gemini's "major" partly rests on the CI2 claim below.
   - Both audits list it as must-fix, so the label does not change the action.
2. **"Dropping REVIEW4 CI2(c) allowed this to slip" (AUDIT4-GEMINI.md:20-21, :107) is not supported.**
   - REVIEW4's CI2 fix (`REVIEW4.md:188`) asked for a "real binary, a blackhole and a trickle URL, `start`, SIGTERM, and assert the exit time and a retained `"online":false`".
   - REVIEW4 measured that exact pair exiting in 0.05–0.10 s on `6641ca8` (`REVIEW4.md:53`), because FFmpeg's connect and read honor `abort()`.
   - On `58a2f9c` they still drain within 2 s. So `exitIfBusy` returns, `shutdown()` sends DISCONNECT, and the `_exit(0)` branch is never reached.
   - The assertion would not catch it either: the stale will is also `"online":false`. A test written as REVIEW4 specified would pass on `58a2f9c`.
   - Catching R1 takes a worker that ignores `abort()` behind the controller (a DNS stall, or `StallingDecoder` behind a decoder-factory seam) and an assertion on the stream list. That is my CI2 verdict ("partly vindicated"): the gap is a controller-level test, not the dropped recipe.
3. **The doc paraphrase.** Gemini says `docs/mqtt.md:53` claims the live-table status "is preserved on the broker" on this path.
   - The paragraph is at `:54`. It says the process "publishes a final retained status", and "When every worker has returned, the process disconnects and the broker discards the will".
   - It never promises the status survives `_exit`. What the code breaks is the paragraph's "clean shutdown" framing.

**Gemini's fix: call `shutdown()` in `finishProcess()` after `publishRaw` and before `exitIfBusy`.**

- **It works while connected** `[repro]`: 2.019 s, rc 0, one status message, and the retained status keeps `ok` and `cam`. With it, `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` passes 13/13.
- **It makes SIGTERM unbounded when the broker is unreachable by name.**
  - `shutdown()` (`src/mqtt_controller.cpp:511-525`) calls `mosquitto_loop_stop(mosq_, false)`, which joins libmosquitto's network thread.
  - After a dropped connection, that thread sits in `loop_forever`'s reconnect (`reconnect_delay_set` at `:196`). `mosquitto_reconnect` does a blocking `getaddrinfo` and `connect` on `--mqtt-host`.
  - With a silent resolver (T1's conditions) and a broker given by DNS name, the join waits out that lookup. On `main`, `_exit(0)` skips `shutdown()`, so #25's 2 s bound holds.

| Build (`58a2f9c` + patch) | Stuck `cam`, broker up | Stuck `cam`, broker stopped and its name not resolving |
| --- | --- | --- |
| `main` | 2.018 s, rc 0, **retained empty will** | 2.018 s, rc 0 |
| Gemini: `shutdown()` before `exitIfBusy` | 2.019 s, table kept | **8.532 s** (1 nameserver), **18.560 s** (2 nameservers) |
| My A1 scratch fix: drain, then `shutdown()` + `_exit(0)` | 2.018 s, table kept | **8.537 s** (1 nameserver), **18.563 s** (2 nameservers) |
| Gated: `shutdown()` only if the final publish was queued | 2.019 s, table kept | 2.019 s (1 nameserver), 2.018 s (2 nameservers) |

- **The gated fix.** `publishRaw` returns `bool`, then:
  `const bool sent = publishRaw(...); if (!player.drain(kExitJoinDeadline)) { if (sent) { shutdown(); } player.exitIfBusy(0ms); }`.
  - The diff is 12 insertions and 6 deletions in `mqtt_controller.{h,cpp}`. The suite still passes 13/13.
  - Skipping DISCONNECT when not connected loses nothing. The final publish has already failed (`MQTT publish failed (audit4/status): The client is not currently connected.`), and the broker publishes the will on its own once it notices the dropped connection.
  - One race remains: the connection can drop during the 2 s drain. A version that is fully bounded would call `mosquitto_disconnect()`, wait up to a short deadline for `onDisconnect(rc == 0)`, and then `_exit` without `mosquitto_loop_stop`. That version is untested.
- **The same delay on the no-stuck-worker path is older than #25, not a regression.**
  - With no `cam` at all, both `6641ca8` and `58a2f9c` take 8.536 s to exit in the broker-dropped case, in the tail `shutdown()` `[repro]`.
  - The add-on's default `mqtt_host` is `localhost` (`resources/settings.xml:30-32`). That resolves from `/etc/hosts`, so the default setup never hits this. Only a broker set by DNS name does.

## 2. R2 (Gemini: minor) — the non-zero exit paths vs `exitIfBusy`'s hard-coded `_exit(0)`

**Agree:**

- **The mid-session auth give-up joins a stuck worker with no deadline** (`src/mqtt_controller.cpp:270-277` → `~StreamPlayer` at `src/stream_player.cpp:62-75`). This is A2. Gemini gives only `[code]`. My measurements `[repro]`:
  - After the give-up line, exit took 5.04 s with one nameserver and 4.04 s with two, both rc 1.
  - The bound is the rest of one lookup: about 10 s with one nameserver, about 20 s with two.
- **Startup refusals are unaffected:** no `add` can arrive before the first CONNACK.
- **The `sigtimedwait` error path exits 0** (A6). Gemini does not say it is practically unreachable: with a valid timeout, `sigtimedwait` fails only with `EAGAIN` or `EINTR`. So that half is a nit.
- **Minor overall.**

**Disagree / missed:**

- **Gemini's fix does not reach the `sigtimedwait` case** `[code]`.
  - That path is `finishProcess(); shutdown(); return 1;` (`:291-293`). `finishProcess()` calls `exitIfBusy(kExitJoinDeadline)` with the default code 0 and `_exit(0)`s before returning.
  - So the proposed `exitIfBusy(kExitJoinDeadline, 1)` after `shutdown()` never runs while a worker is stuck. That stays true with Gemini's R1 fix applied.
  - `finishProcess` itself has to take the exit code.
- **Gemini's auth-path fix** (`closeAll(); shutdown(); exitIfBusy(deadline, 1)`) is sound `[code]`.
  - It matches my A2 scratch fix, which exited 2.04 s after the give-up with rc 1.
  - `shutdown()` is fast here: the existing path already calls it and exits as soon as the lookup returns.
- **Missed: signals are blocked during that join** (`mqtt_controller.cpp:199-203`, and nothing calls `sigtimedwait`). A `systemctl stop` in that window waits for the launch script's SIGKILL.
- **Missed: three comments/docs say this path cannot happen.**
  - `src/stream_player.cpp:63-65`: "Process exit calls exitIfBusy() and _exit(0) before this destructor runs".
  - `src/stream_player.h:29-32`.
  - `docs/mqtt.md:107`: "abandoned on process exit".
  - This is exactly the rebase fallout Gemini rated clean.

## 3. Gemini's other verdicts

**Agree, with evidence:**

- **#24 add-on side, clean.** I re-ran Gemini's `ctypes` check: 18 values through `quote_systemd_env` and this VM's `libsystemd-shared-255.so` `load_env_file()`, with 0 mismatches `[repro]`. The values covered spaces, `"`, `\`, `$HOME`, `${X}`, backticks, `#`, `;`, `=`, `%`, newline, tab, leading and trailing spaces, `'`, and non-ASCII. This is stronger than the rule-based check in `AUDIT4.md` section 4. CE21/CE22 systemd still needs-GS-King-X.
- **#20 stall isolation intact.** My count was 25 presents per 0.5 s across `remove`; Gemini saw 20–49 per second.
- **The other filter calls:** T1, K1, and H1 kept, the P1 deferral, M1, CI1, N1 (moot after #25), N2, and I1–I3.

**Disagree:**

- **"#23 clean" (AUDIT4-GEMINI.md:88).** Two claims in it are false on `58a2f9c` `[repro]`:
  - **"Standalone audio URLs still fail `av_find_best_stream(AVMEDIA_TYPE_VIDEO)`."**
    - Not for an MP3 with cover art. FFmpeg exposes an ID3 APIC picture as an `attached_pic` video stream.
    - `song.mp3` is refused on `6641ca8` (`Format not on whitelist`). On `58a2f9c` it opens `stream=1 codec=mjpeg`, presents the cover, and exits rc 0.
    - Over MQTT: 10 opens and 10 presented frames in 10 s (A4).
    - Only bare ADTS (`plain.aac`) fails at `no video stream`.
  - **"matching the four audio codecs already in `kCodecWhitelist`."** That list is matched by decoder name, and FFmpeg 6.1's MP3 decoder is `mp3float`, so `mp3` never matches (A5). `testRejectGif` shows the image demuxers stay blocked. It does not cover the four new demuxers at the top level.
- **"Rebase fallout clean (save for R1)."** The three stale A2 comments/docs above contradict the code. So does `printUsage`'s "stop closes them" (`mqtt_controller.cpp:412`), against `README.md:65` and `docs/mqtt.md:5` (A6).
- **"K1 cleanly fixed" and "H1 cleanly fixed" (AUDIT4-GEMINI.md:100-101).**
  - For K1, Gemini's own R2 shows the auth exit is unbounded mid-session.
  - For H1, see A4 and A5.
- **O1, "defensible drop."** I agree with the outcome (cosmetic). SURVIVING4's reason ("logs from this pass did not contain a torn line") was weak, though, and Gemini did not re-test. The bug still reproduces: 2 torn lines out of 639 `[repro]`.
- **N3, "correctly dropped; both run and pass in CI."** True today. But #24's K1 regression cases live in `mqtt_reconnect`, which exits 77 (skip) without mosquitto and is not in `scripts/ctest-require.sh`. A CI image without mosquitto would skip the K1 test silently. It stays a nit.

## 4. What Gemini missed

- **A3:** `restart` churn during a stuck lookup stacks decode threads. After 10 s of 10 restarts per second: 101 threads, 105 fds, and 2,989 MB VmSize, against 4, 8, and 413 MB before #25. **needs-GS-King-X** for the 32-bit armv7 address space.
- **A4** and **A5** (section 3).
- **A2's measurements, stale docs, and blocked signals** (section 2).
- **The unbounded `shutdown()` in the R1 fix** (section 1). My A1 missed this too.

## 5. Must-fix list, both audits combined

1. **R1 = A1:**
   - Disconnect before `_exit(0)` only when the final status was queued on a live connection. Do not call `shutdown()` unconditionally.
   - Add a controller-level test with a worker that ignores `abort()`. Assert the stream list in the retained status after SIGTERM, not just `"online":false`.
2. **R2 = A2:**
   - Give the auth give-up path a bounded drain, keeping exit status 1.
   - Have `finishProcess` take the exit code, so the `sigtimedwait` error path keeps its 1 as well.
   - Correct `stream_player.cpp:63-65`, `stream_player.h:29-32`, and `docs/mqtt.md:107`.

Should fix: A3 and A4. Nits: A5, A6, O1, and N3.

## 6. How this was checked

- **Worktrees at `58a2f9c`, built with `CXX=g++`:**
  - Gemini's R1 fix (one added `shutdown();` line).
  - My `AUDIT4.md` A1 scratch fix.
  - The gated fix above.
  - `6641ca8`, for the pre-#25 baseline.
- **Suite results:** Gemini's tree and the gated tree each passed `AMS_REQUIRE_DECODER_TESTS=1 ./scripts/run-tests.sh` 13/13.
- **New `brokerdrop` scenario:** the `AUDIT4.md` harness, plus a bind-mounted `/etc/hosts` mapping `brokerhost` to 127.0.0.1. The player runs with `--mqtt-host brokerhost`.
  - 1.2 s after `start`, stop the broker and rewrite the hosts file without `brokerhost`.
  - Wait 2.5 s after `MQTT connection lost`. By then libmosquitto's 1 s reconnect delay has passed, and its thread is in the broker lookup (`/proc/…/wchan` = `do_poll`).
  - Then send SIGTERM and time the exit. `NOCAM=1` drops the stuck `cam`, and `NAMESERVERS=2` adds the second silent resolver.
- **`ctypes`:** `load_env_file(NULL, path, &env)` from `libsystemd-shared-255.so`, on a one-line file `AMS_MQTT_PASSWORD=<quote_systemd_env(v)>`.
