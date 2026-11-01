# CROSSCHECK-OPUS: AUDIT6 vs AUDIT6-GEMINI

Checked: `AUDIT6-GEMINI.md` on `cursor/audit6-gemini-7871` (draft
[#35](https://github.com/yyolk/amlogic-multistream/pull/35), commit `b958a33`),
against `AUDIT6.md` in this PR and `main` @ `6d9d146`. No production code is changed.

Gemini's doc reads the code. It reports no measurements. Where a verdict below
cites `[repro]`, the run is the one in `AUDIT6.md` section 6.

## Bottom line

- **Both audits keep one finding at should-fix: SIGTERM during the pre-connect broker lookup waits for `getaddrinfo`.** That is my N1 and Gemini's finding 1. Neither audit claims a must-fix.
- **N1 stays should-fix, not must-fix.** See the judgement below.
- **No must-fix remains at `main` @ `6d9d146`.** Both audits verify F1, X1, and F3 as closed. My runs measured every exit after the network thread starts at 2.52 s or less, and found no retained `"online":true`.
- Gemini's other two findings are nits. I agree with their severity, but Gemini's mechanism is wrong for the race in finding 2, and the trigger for finding 3 is narrower than Gemini states.
- Gemini did not report my nits N2–N4 or the wider F5. None of them is must-fix or should-fix, so the survivors are unchanged.

## N1 / Gemini 1: must-fix or should-fix?

**Should-fix.** The case for must-fix:

- With two silent nameservers, `scripts/launch-coreelec.sh` SIGKILLs the player and Kodi returns at 11.14 s `[repro]`.
- `README.md:65` lists "does not resolve" among the cases the exit stays inside the poll for.
- The comment at `src/mqtt_controller.cpp:223-226` says the start-up loop keeps accepting signals so the launch script can start Kodi.

It falls short of must-fix because:

1. **Narrow trigger.** It needs a broker set by hostname (the add-on default `localhost` resolves from `/etc/hosts`), a resolver that times out rather than fails, and a stop before the first connect is queued. A broker given as an IP, even a blackholed one, exits in 0.52 s, because `connect_async` does not block on an IP `[repro]`.
2. **Bounded, with a working fallback.** glibc defaults are 5 s × 2 tries per nameserver. With one nameserver, Kodi returns at 9.09 s, inside the poll and without SIGKILL `[repro]`. With two, the script's SIGKILL fallback runs as designed and Kodi is back at about 11 s.
3. **No state damage.** This process never connected, so nothing was published. The broker keeps the previous session's offline status or will. That is the opposite of F1, which left a dead player retained as online.
4. **The README claim is about the exit after connect.** The sentence describes the DISCONNECT / `_exit` sequence. It is misleading for start-up and should be reworded, but it is not a broken mechanism.

X1 was must-fix because its trigger was any broker loss after connect, and the docs promised that exact bound. N1 matches the profile AUDIT5 gave F2 (should-fix) before it was merged into X1.

## My AUDIT6 findings

| ID | My severity | Gemini | Verdict | Rationale |
| --- | --- | --- | --- | --- |
| N1 pre-connect DNS | should-fix | Finding 1, should-fix | **AGREE** | Same bug and same severity. It is measured at 9.59 s / 19.58 s (9.09 s / 11.14 s to Kodi), and the reasons above keep it below must-fix. |
| N2 `sent` true during a blocking reconnect | nit | Missed (finding 2 is next to it) | **AGREE (nit)** | libmosquitto sets `mosq->sock` inside `net__try_connect` (`lib/net_mosq.c:916`), so the exit waits the 500 ms cap. The 2.52 s bound holds, so only the comment and README wording are wrong. |
| N3 full cap also delays healthy reopens | nit | Partly covered: says "new ids wait", but not healthy restarts | **AGREE (nit)** | It is the stated price of F3's 17-thread bound, limited to one resolver timeout (8.49 s measured). |
| N4 stale `stream_player.h` exit text, test-only `exitIfBusy()` | nit | Missed | **AGREE (nit)** | Documentation drift only. The real exit is pinned by `mqtt_exit_test`. |
| F4 same-id URL fix latency | nit | Missed | **AGREE (nit)** | Documented and bounded (8.58 s). |
| F5 cover art via `mov` and HLS-wrapped `song.mp3` | nit | Missed: says the whitelists are "correctly enforced" | **AGREE (nit)** | `[repro]` `wrap_mp3.m3u8` presents `codec=mjpeg`. Gemini's whitelist line is true for what the lists contain, but misses this way around #28's top-level check. AUDIT4 rated this class minor. |
| F6 packed-audio fixtures are ADTS only | nit | Missed | **AGREE (nit)** | Test gap, with no runtime effect. |
| F7 MQTT regression tests are skippable | nit | Missed | **AGREE (nit)** | CI installs mosquitto, so the risk is a silent skip on another image. |

## Gemini's findings and claims

| Gemini item | Gemini severity | Verdict | Rationale |
| --- | --- | --- | --- |
| F1 closed | verified | **AGREE** | `[repro]`: `list` + `add` during the drain leave `online:false` retained with the table kept, and only one status message is published after SIGTERM. |
| X1 closed, exit ≤ 2.5 s | verified | **AGREE** | `[repro]`: 0.02 s, 0.51 s, and 2.02 s, and 2.52 s worst case with a blackholed broker plus a stuck worker. The 2.5 s figure covers only the exit after connect, not N1. |
| F3 closed, ≤ 17 decode threads | verified | **AGREE** | `[repro]`: fresh-id churn peaks at 13 threads (AUDIT5: 94), and the bound proof holds. |
| Auth exit 1, stall isolation | verified | **AGREE** | `[repro]`: startup refusal gives rc 1. Mid-session give-up with a stuck worker gives rc 1 at 1992 ms. The healthy tile keeps presenting across `restart`/`remove` of the stuck one. |
| Whitelists "correctly enforced" | verified | **PARTIAL** | The lists are applied as written, but an HLS playlist wrapping an audio file with cover art still presents the cover (F5). |
| 1. Pre-connect main-thread DNS hang on SIGTERM | should-fix | **AGREE on bug and severity. PARTIAL on size.** | "Can be minutes" needs non-default `resolv.conf` options. glibc defaults give 10 s per nameserver, 30 s at most with the 3-nameserver limit, measured at 9.6 s / 19.6 s. "The only remaining path that can hang on SIGTERM" matches what I found. |
| 2. `disconnected_` reset race | nit, no action | **PARTIAL** | Agree it is benign, but the reason is wrong. `mosquitto_disconnect` checks `mosq->sock`, not state. A drop closes the socket before `on_disconnect` runs (`lib/net_mosq.c:246`), and that is why it returns `NO_CONN`. During a blocking reconnect it returns success instead and waits the 500 ms cap (N2), which is still bounded. |
| 3. `retiredLimitLogged_` reset logic | nit | **AGREE (nit), narrower trigger** `[code]` | The flag resets when any pending session passes the cap check (`src/stream_player.cpp:459`). The session that hit the cap normally does that once the reaper drains. The log is only suppressed when that session is removed, stopped, or hidden while capped, and a later episode then hits the cap. It is a cosmetic log gap. |
| Line citations | — | **Mostly right** | `:309`, `:461`, `:490`, `:172`, `:450`, `:459` match. `:318` is `disconnected_ = false` at `:319`. `:328` is the `fflush` before `_exit`. Neither error changes a conclusion. |

## Survivors (kept as must-fix or should-fix by both)

| # | Severity | Finding | Where | Fix shape (not implemented) |
| --- | --- | --- | --- | --- |
| 1 | **should-fix** | SIGTERM, SIGINT, or SIGHUP during the pre-connect broker lookup waits for `getaddrinfo`, because the signal is blocked and only `sigtimedwait` takes it. Measured: 9.6 s with 1 silent nameserver and 19.6 s with 2. Through the launch script, Kodi returns at 9.1 s, or 11.1 s via SIGKILL | `src/mqtt_controller.cpp:223-253` (`connect_async` at `:229`, `sigtimedwait` at `:236`) | Run the start-up `connect_async` retry loop on a helper thread, and keep the main thread in `sigtimedwait`. On a signal before `mosquitto_loop_start`, flush and `_exit(0)`: nothing has been published and nothing needs joining. Test: in `mqtt_exit_test`, touch the stall file before launch, then SIGTERM, and expect rc 0 within 1500 ms. Reword `:223-226` and `README.md:65`. |

**Must-fix:** none.

Nits left off this table: N2, N3, N4, F4, F5, F6, F7, and Gemini 2 and 3. None of them blocks GS-King X hardware work.
