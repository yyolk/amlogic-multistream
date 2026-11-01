# CROSSCHECK-GEMINI

Crosscheck of Gemini's `AUDIT6-GEMINI.md` and Opus's `AUDIT6.md` (PR #44).

## Verdict on Must-Fixes
**No must-fix issues remain at `main` @ `6d9d146` after #33 and #34.** Both audits confirm that F1, X1, and F3 are correctly resolved. The exit paths are bounded, the thread explosion is capped, and the retained status is protected.

## Evaluation of Findings

### Gemini's Findings

1. **Pre-connect main-thread DNS hang on SIGTERM (Gemini #1 / Opus N1)**
   - **Rating:** AGREE (should-fix).
   - **Rationale:** Opus's measurements show the launch script's SIGKILL fallback ensures the system recovers (Kodi restarts at ~11s) and no retained status is corrupted since no connection was made, confirming this is a `should-fix` rather than a `must-fix`.

2. **Minor race in `disconnectCv_.wait_for` (Gemini #2)**
   - **Rating:** AGREE (nit).
   - **Rationale:** It's a benign race that doesn't cause a hang because `mosquitto_disconnect` returns an error if already disconnected, safely bypassing the wait.

3. **`retiredLimitLogged_` reset logic (Gemini #3)**
   - **Rating:** AGREE (nit).
   - **Rationale:** It's a minor logging edge case where the warning might be missed if the limit is hit again without a successful worker start in between.

### Opus's Findings

1. **N1: SIGTERM during pre-connect broker lookup waits for `getaddrinfo`**
   - **Rating:** AGREE (should-fix).
   - **Rationale:** (Same as Gemini #1) The process is SIGKILLed by the launch script, preventing a permanent hang, but it delays Kodi's return and violates the clean exit expectation.

2. **N2: `sent` is true while a reconnect is blocked in `connect()`**
   - **Rating:** AGREE (nit).
   - **Rationale:** The 500ms exit bound still holds, so it's not a functional bug, but the comments and README are technically inaccurate for this specific state.

3. **N3: A full cap also delays healthy reopens**
   - **Rating:** AGREE (nit).
   - **Rationale:** The global cap correctly bounds threads, but as a trade-off, it delays healthy reopens until a stuck lookup returns, which should be documented.

4. **N4: Stale exit wording in docs/headers**
   - **Rating:** AGREE (nit).
   - **Rationale:** `exitIfBusy` is no longer used in production code, so the docs and headers should be updated to reflect the new exit path.

5. **F4: Re-adding an id with a corrected URL waits out the old lookup**
   - **Rating:** AGREE (nit).
   - **Rationale:** It's a known minor issue carried over from AUDIT5, bounded by the resolver timeout.

6. **F5: Cover art still presents, now also through HLS**
   - **Rating:** AGREE (nit).
   - **Rationale:** Wrapping an audio file with cover art in an HLS playlist bypasses the top-level demuxer check, but it only results in a single still frame.

7. **F6: Tests do not pin the gate / F1 / F2 (Packed-audio fixtures are ADTS only)**
   - **Rating:** AGREE (nit).
   - **Rationale:** Packed-audio fixtures should ideally cover more formats than just ADTS to ensure robust testing.

8. **F7: Skippable MQTT tests**
   - **Rating:** AGREE (nit).
   - **Rationale:** The tests guarding F1/X1 can be silently skipped if mosquitto is missing, which weakens CI guarantees.

## Survivors Table

Only findings that both audits effectively keep as `must-fix` or `should-fix`.

| ID | Severity | Finding |
| --- | --- | --- |
| **N1 / Gemini #1** | **should-fix** | SIGTERM during the pre-connect broker lookup waits for `getaddrinfo` on the main thread. The launch script's 10s poll SIGKILLs the player, delaying Kodi's return to ~11s. |
