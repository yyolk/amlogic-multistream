# Survivors: `main` @ 6641ca8

Filtered from [REVIEW4.md](REVIEW4.md) on `cursor/review4-unfiltered-9f4e`
(`33f302e`), the unfiltered pass over `6641ca8`. This file is the list to
act on. No production code was changed, and no GitHub issues were filed.

Checked commit: `6641ca8f6e647e39a32b544995f4bc1bd71ecdc1`
("Isolate a stalled stream so it cannot freeze the wall. (#20)").
`origin/main` was still that commit when this pass ran.

Re-checked on Ubuntu 24.04 x86_64, g++ 13.3, glibc 2.39, FFmpeg 6.1.1
(libavformat 60.16.100), mosquitto 2.0.18. `nsswitch.conf` on this VM is
`hosts: files dns`, so `getaddrinfo` uses `/etc/resolv.conf` directly.
The player was built from this commit with `g++`.
`stream_player_stall_test` passed (`blackhole+trickle shutdownMs=88`,
`live+vod presents=152 shutdownMs=17`). Nothing here ran on a GS-King X.

Severity follows REVIEW4. **blocker** = defeats a project goal on a
shipped path. **major** = wrong behavior on a shipped path, or a gap in a
stated goal. **minor** = real but contained.

---

## Survivors

### 1. major — An unresolved hostname still blocks the presenting thread on `remove`, `stop`, `restart`, and SIGTERM (REVIEW4 T1)

- **Where:** `finishWorker()` calls `requestStop()` and then `worker.join()`
  on the thread that presents (`src/stream_player.cpp:115-118`). That
  caller is `dropMissing()` on `remove` (`:249-256`), `closeAll()` on
  `stop` and shutdown (`:225-233`, `:284-286`, `src/mqtt_controller.cpp:247`),
  a tile a layout override hides (`stream_player.cpp:338-340`), and a
  `restart` or URL change (`:346-348`). Shutdown publishes the offline
  status only after that join (`mqtt_controller.cpp:247-252`).
  `FfmpegDecoder::abort()` only latches an `AVIOInterruptCB`
  (`src/ffmpeg_decoder.cpp:70-80`). FFmpeg 6.1.1 `tcp_open` calls
  `getaddrinfo()` before `ff_connect_parallel`, and that libc call does
  not consult the callback. `decoder_backend.h:40-46` says the in-flight
  `open()` or `readFrame()` then returns. That return waits out
  `getaddrinfo`.
- **Evidence:** A mount+network namespace, one nameserver
  `127.0.0.2` with `options timeout:5 attempts:2`, and a UDP socket on
  that address that reads and never replies. glibc `getaddrinfo("camera.lan")`
  returned `EAI_AGAIN` in **10.015 s**. The same call with two such
  servers took **20.023 s**. An NXDOMAIN reply took **0.003 s**.
  `127.0.0.1` took **0.003 s**.

  The real binary, a local broker, a healthy MPEG-TS clip at
  `http://127.0.0.1/…`, and `cam` = `http://camera.lan/live/index.m3u8`,
  viewport `64x36`, `--render-dir`:

  | Action | Result |
  | --- | --- |
  | Leave `cam` resolving for 311 s | The healthy tile stayed at about **49.5 presents/s**. The gaps were about 1 s, when the clip ended and the 1 s retry ran. |
  | `remove {"id":"cam"}` 2.2 s after `start` | `mosquitto_pub` returned in **0.011 s**. Presents then stayed at **0 for 7.61 s** and returned to 50/s. During the gap the present thread was in `futex` (`join`) and the `cam` thread was in `poll` (the resolver). |
  | SIGTERM 1.2 s after `start` | Exit **8.888 s**, rc 0. The offline status was published after the lookup finished. The same SIGTERM with only the healthy stream exited in **0.033 s**. |

  2.2 + 7.61 and 1.2 + 8.89 both land on the 10.0 s lookup. REVIEW4's
  8.11 s is that remainder, measured from a SIGTERM about 2 s into the
  lookup. After a failed lookup the worker exits, `tick()` joins it
  immediately, and `kRetryDelay` (1 s, `stream_player.cpp:13`) passes
  with no worker. The next open enters `getaddrinfo` again, so a
  `stop` / `remove` / SIGTERM lands inside a lookup for about 10 s of
  every 11 s. `closeAll()` aborts every worker before joining any, so
  several lookups cost the longest one. That overlap is from the code;
  two names were not run through the binary.

  The launch script polls 10 s after SIGTERM, then SIGKILL
  (`scripts/launch-coreelec.sh:151-161`). A lookup caught at the start
  is 10.0 s and sits on that poll. Two non-answering servers are 20.0 s
  in this glibc and are past it. Shutdown publishes the offline status
  only after the join, so a SIGKILL in that window skips the publish.
  The 20 s figure is the libc timing. The player was run with one server.
- **Why major:** `README.md:65` says one stalled read does not freeze the
  wall or the signal wait. A hostname whose resolver does not answer
  brings that back on every control action and on SIGTERM. It is bounded
  by `resolv.conf`, so it is not a blocker. An NXDOMAIN and a numeric
  host do not hit it. Whether CoreELEC's resolver waits out these
  timeouts or fails fast needs a GS-King X.
- **Fix:** On `remove`, `stop`, `restart`, and shutdown, `requestStop()`
  and move the session onto a reaper list. `tick()` joins only a worker
  whose loop has returned. Publish the offline status before the
  shutdown join, and if a worker is still inside libc after a short
  deadline, flush and `_exit(0)`. A test with a `FakeDecoder` whose
  `open()` ignores `abort()` and sleeps is the stand-in for
  `getaddrinfo`.

### 2. major — The Kodi add-on has no MQTT username or password, and Home Assistant's Mosquitto broker refuses anonymous clients (REVIEW4 K1)

- **Where:** `build_command()` passes only `MQTT_HOST`, `MQTT_PORT`, and
  `MQTT_TOPIC_PREFIX`
  (`kodi-addon/plugin.program.amlogic-multistream/resources/lib/amlogic_launcher.py:32-52`).
  `default.py:45-50` feeds it those three settings.
  `resources/settings.xml` has host, port, and prefix, and no username or
  password. The player reads `MQTT_USERNAME` / `MQTT_PASSWORD`
  (`src/mqtt_controller.cpp:267-272`, `docs/mqtt.md:18-19`).
  `docs/mqtt.md:22` says broker authentication is expected outside a lab
  LAN. The transient unit is started with an explicit `--setenv` list, so
  a username that exists only in Kodi's environment is not passed.
- **Evidence:** mosquitto 2.0.18 with `allow_anonymous false` and a
  password file. The player started with host, port, and prefix only
  (what the add-on builds) printed `MQTT connect rejected: Connection
  Refused: not authorised.` and `MQTT connection lost: The connection was
  refused. (will retry)` three times in 6 s and was still running.
  SIGTERM then shut it down. The same broker with `--mqtt-user` and
  `--mqtt-pass` connected, subscribed, and published status.

  Home Assistant's Mosquitto app docs say the app does not support
  anonymous logins and that every connection needs a username and
  password (`home-assistant/addons` `mosquitto/DOCS.md`). The 6.0.0
  changelog entry is "Support for anonymous logins has been removed".

  Launch stops Kodi before the player runs (`scripts/launch-coreelec.sh:253`).
  Returning to Kodi is `systemctl stop amlogic-multistream-launch` over
  SSH or a Home Assistant shell command (`docs/kodi-addon.md:42-47`).
  That stop is the documented return path for every launch on this
  build, including one that connects, because the player has no quit
  command and does not scan out. Against this broker the player also
  never connects, so no command and no status ever land, and a refused
  login does not start working later the way a down broker can.

  "Launch script override" is an expert setting that replaces the bundled
  script (`docs/kodi-addon.md:61`). The docs do not say to use it for
  credentials. A wrapper that exports `MQTT_USERNAME` and
  `MQTT_PASSWORD` and execs the bundled script would, because those
  variables are not in the add-on's `--setenv` list. That wrapper was
  not run under systemd here.
- **Why major:** The add-on is the launch path, and Home Assistant is the
  stated controller. With Home Assistant's own broker, the settings the
  add-on ships cannot log in. The player itself already can.
- **Fix:** Add `mqtt_username` and `mqtt_password` settings and pass them
  the same way as the other three. A password in `--setenv` shows up in
  `systemctl show`; an `EnvironmentFile=` mode `0600` under the add-on
  profile keeps it out of that listing. A CONNACK of "not authorised" or
  "bad username or password" should exit non-zero after a few attempts
  so the launch script starts Kodi again. Extend `tests/kodi_addon_test.py`
  so the credentials are on the `systemd-run` command.

### 3. minor — An HLS master with a separate ADTS audio rendition fails to open (REVIEW4 H1)

- **Where:** `kFormatWhitelist` is `hls,mpegts,mov,mp4,mpeg`
  (`src/ffmpeg_decoder.cpp:27`), set on `avformat_open_input` (`:128`).
  FFmpeg 6.1.1 `hls.c` copies that whitelist onto each rendition with
  `ff_copy_whiteblacklists` and opens the first segment of the rendition
  inside the master's `avformat_open_input`, before this player's
  `AVDISCARD_ALL` (`ffmpeg_decoder.cpp:185-191`) can drop the audio.
- **Evidence:** A master with
  `#EXT-X-MEDIA:TYPE=AUDIO,…,URI="audio.m3u8"` whose segments are ADTS
  (`ffprobe` format `aac`) and one H.264 MPEG-TS video playlist.
  `ffprobe` with no whitelist opens it (aac + h264). The player:

  ```text
  [aac @ …] Format not on whitelist 'hls,mpegts,mov,mp4,mpeg'
  FfmpegDecoder::open failed … avformat_open_input: Invalid argument
  ```

  Exit status 1, 0 frames. The video playlist alone presents.
  A file named `.aac` whose payload is actually MPEG-TS is format
  `mpegts`, stays on the whitelist, and presents. The rejection is the
  demuxer name. `ac3`, `eac3`, and `mp3` segment demuxers are absent
  from the same list; those three were not given their own fixtures.
- **Why minor:** The whole stream is rejected and, on the MQTT path,
  retried every 1 s with a status entry that still looks like any other
  stream (`id`, `url`, `muted` only). Packed-audio renditions are valid
  HLS. Camera and NVR playlists usually mux audio into TS or fMP4, which
  this whitelist already allows. This is a side effect of the demuxer
  allowlist added in #19.
- **Fix:** Add `aac,ac3,eac3,mp3` to `kFormatWhitelist`. Their decoders
  are already on `kCodecWhitelist`. One ADTS master in
  `ffmpeg_decoder_tests` is enough.

---

## Deferred until something scans out

### P1, downgraded from major — The present loop tops out near 50 fps, and a 60 fps live HLS falls behind

Kept out of the action list. Software mode does not drive HDMI
(`src/mqtt_controller.cpp:177-182`), and the 20 ms pause is the loop this
tree documents (`src/mqtt_controller.cpp:64`, `docs/mqtt.md` present
paragraph: one frame per stream, then about 20 ms).

The mechanism is real:

- `tick()` takes at most one frame per stream (`src/stream_player.cpp:376-381`).
  The decode thread waits until that slot is empty (`:160`) before
  `readFrame()`, so the demuxer does not run ahead of the presenter.
- A 30 fps MPEG-TS clip on a `64x36` viewport presented at about
  **49.5 frames/s** for five minutes. That is the tick, not the file.
- One 60 fps live HLS (`ffmpeg -re`, libx264, 1 s segments,
  `hls_list_size 4`, `delete_segments`), `--viewport 1280x720`, 18 s.
  The log reached `presented live frame 720` (at least 721 frames,
  under about 43/s; 60 × 18 would be 1,080). Segment fetches were about
  **1.38 s** apart. Segments **10 and 13** were never requested; the
  client jumped `9 → 11` and `12 → 14`. `stderr` had no FFmpeg warning
  (`av_log_set_level(AV_LOG_ERROR)`).

Nine 30 fps streams were not re-run. On this host the cap shows up at
50 fps and above, and at 1280x720 the full-viewport copy
(`src/software_renderer.cpp:215-231`) pulls the rate into the low 40s.
That is invisible until a DRM/KMS backend presents. When that loop is
written, a fixed 20 ms sleep and a slot that blocks the demuxer will
drop 50/60 fps live HLS the same way. Wake the presenter on a new frame,
and for live let a newer frame replace one that was not shown. Tick time
for 4 and 9 tiles on the S922X needs the box.

---

## Already fixed on `6641ca8` (not re-raised)

REVIEW4 section 1 still holds. Checked by reading the tree and, for the
stall, by running it.

| Prior item | What this pass saw |
| --- | --- |
| SURVIVING3 #1 / B1, one stalled or live-edge stream freezes every tile and SIGTERM | Still fixed. Own thread per stream, `abort()` before any join. Stall test: blackhole+trickle shutdown 88 ms, live+vod 152 presents and shutdown 17 ms. A DNS neighbor that is not being joined left the healthy tile at ~49.5/s for 311 s. Healthy SIGTERM 0.033 s. The remaining hole is survivor 1. |
| SURVIVING3 #2, no device build | `.github/workflows/coreelec-appimage.yml` still builds `coreelec21-ng-armv7hf` and `coreelec22-no-aarch64`. Not rebuilt here. |
| SURVIVING3 #3, unclean exit loses the last status | Stdout is line-buffered and unit-buffered (`src/main.cpp:9-10`). The will is refreshed on each status publish. |
| SURVIVING3 #4, a second signal during restore skips Kodi | `restore_kodi` ignores HUP/TERM/INT first (`scripts/launch-coreelec.sh:168-172`). |
| SURVIVING3 #5, `PLAYER_EXTRACT_DIR` is `rm -rf`'d | `dangerous_extract_root` refuses the unsafe roots (`:72`). Only `squashfs-root` and `.source-stamp` are removed (`:229-231`). |
| SURVIVING3 #6, `add` reaches every demuxer | Format and codec whitelists are applied. Survivor 3 is the side effect. |
| SURVIVING3 #7, Launch does not warn that HDMI stays blank | The startup line is still in `mqtt_controller.cpp:177-182`, README, and `docs/kodi-addon.md`. |
| SURVIVING3 #8, exit 1 when the broker is down at startup | The first connect retries with 1–30 s backoff and still takes signals (`mqtt_controller.cpp:184-214`). Survivor 2 is the refused login, which retrying does not fix. |
| SURVIVING3 #9, `signal_tree` overwrites `root` | `local root sig dir pid pp` (`launch-coreelec.sh:131`). |

Intentional gates, still not findings: issue #14 (the add-on does not
bundle the AppImage), issue #18 (optional self-hosted device smoke), and
hardware decode plus real DRM/KMS scanout. This build refuses
`isHardware()` (`stream_player.cpp:139-145`). `DrmRenderer` does not
modeset or commit a plane (`src/drm_renderer.h:10-13`).

---

## Needs a GS-King X

Not findings. Not checked here.

- **Survivor 1:** `cat /etc/resolv.conf`, then how long `getent hosts`
  waits when the upstream resolver does not answer. This pass only timed
  glibc 2.39 on Ubuntu.
- **Survivor 2:** Whether any remote or CEC path can stop
  `amlogic-multistream-launch` without SSH. The code and the docs say
  SSH or a Home Assistant shell command.
- **Deferred P1:** Present-tick time at 4 and 9 tiles in software mode
  on the S922X, once a picture is on screen.
- **DRM notes from REVIEW4 I1–I3:** `Renderer::present` draws one cell
  and has no begin/commit. `StreamPlayer` holds a `SoftwareRenderer&`.
  `DecodedFrame` has no DMA-BUF fd or release callback. `DrmRenderer`
  opens `card0` and asks for atomic and universal planes only when
  `-DAMLOGIC_DRM_KMS=ON`, and still does not commit. How many video
  planes CE21 and CE22 actually expose, and whether CE21's 4.9 kernel
  has atomic KMS or only fbdev plus `/dev/amvideo`, needs the box
  (`modetest -p`, `ls -l /dev/dri /dev/amvideo /dev/video*`).

---

## Dropped

- **M1 per-stream health, and a trickle that never fails** — Status is
  `id`, `url`, and `muted` (`src/mqtt_json.cpp:411-439`), which is the
  documented schema. Open failures go to stderr and the stream stays in
  the table; that is visible in the DNS run. A watchdog is a new feature.
  The missing read deadline is deliberate: interrupting the live-edge
  wait ends the HLS stream (`ffmpeg_decoder.cpp:65-69`).
- **O1 interleaved decode-thread log lines** — `std::cout << std::unitbuf`
  plus several insertions per line can tear a line, and the journal is
  the log on the box. The DNS and stall logs from this pass did not
  contain a torn line. Cosmetic.
- **CI1 device workflow runs no ARM unit tests and does not decode through MQTT** — The workflow builds the player and the verify script checks `--help`, a local `--url`, and an MQTT connect with no streams (`scripts/build-coreelec-appimage.sh:255`, `scripts/verify-coreelec-appimage.sh:190-253`). README describes that smoke. The stall test and the host CI cover the threading. Extra ARM tests are coverage, and REVIEW4's own qemu run already presented.
- **CI2 missing tests for an abort-ignoring backend, throughput, and a real-binary stall** — The abort-ignoring case belongs to the test on survivor 1. Throughput belongs to the deferred present loop. `stream_player_stall_test` already bounds in-process shutdown. A second test target is not a separate defect.
- **N1 `close()` comment says the decode thread** — `decoder_backend.h:45-46` is wrong: `finishWorker()` calls `close()` on the presenting thread after the join (`stream_player.cpp:127-132`). Nothing shipped follows the wrong sentence. Hardware decode is not in this build.
- **N2 two statements where HUP/TERM can skip `systemctl start kodi`** — After a normal player exit the script clears HUP/TERM/INT and then calls `restore_kodi` (`launch-coreelec.sh:268-269`), which ignores them again at `:172`. REVIEW4 did not reproduce it. This pass did not either.
- **N3 `stream_player_stall` and `mqtt_reconnect` can be skipped** — They are absent from `DECODER_TESTS` (`scripts/ctest-require.sh:5-9`), and the live section of the stall test returns 0 when libx264 is missing after the blackhole case has already run (`tests/stream_player_stall_test.cpp:556-561`). Both ran here. The require list is the three tests CI names on purpose.
- **I1–I3 plane count, DMA-BUF handles, CE21 fbdev** — Interface limits are in the code and are noted under "Needs a GS-King X". Plane counts and whether `/dev/dri/card0` exists on CE21 were not checked and are not work for this tree.
