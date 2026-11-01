# Amlogic hardware decode — fleet synthesis

Status: **recommendation**. Docs only. Not implemented. Nothing here was run
on a GS-King X. Device-only claims are tagged *verify*.

Cross-checked 2026-11-01 against `origin/main` @ `6d9d146` and draft/open
research PRs [#36](https://github.com/yyolk/amlogic-multistream/pull/36)–[#43](https://github.com/yyolk/amlogic-multistream/pull/43),
[#45](https://github.com/yyolk/amlogic-multistream/pull/45),
[#46](https://github.com/yyolk/amlogic-multistream/pull/46). Those PRs are
**not** merged by this note. A coder should spike from this file plus
[#42 Appendix A](https://github.com/yyolk/amlogic-multistream/pull/42), not
from the first-half V4L2 sketch in #42 or the overview in #36.

> **Decision (do not re-litigate):** stock CoreELEC hardware decode is
> `libamcodec`. The 1–9 wall is **CE22 EGL compose** of those decoded
> tiles onto one OSD framebuffer. **CE21 is limited** (instance cap 2, no
> usable DRM video plane). FFmpeg `*_v4l2m2m` and mainline `meson-vdec`
> are dead on these images; they appear only in the scorecard.

---

## 1. Executive recommendation

Build `AmlogicAmcodecDecoder` (`dlopen` on-image `libamcodec.so`,
`STREAM_TYPE_FRAME`, FFmpeg demux/BSF only). Do not grow hardware inside
`FfmpegDecoder`. Do not ship a stock-image backend against `/dev/video26`,
`h264_v4l2m2m`, or `meson-vdec`.
**CE22** (vendor 5.15, aarch64) is the hardware wall: stage-1 is
`AmvideoRenderer` (`SelfPresented` + `/sys/class/video/axis`, one tile);
the 3×3 product is **EGL compose** (GE2D fallback) onto **one** OSD FB —
not nine KMS planes (G12B has two). Keep the UI cap of 9; admit by Mpx/s
and `codec_mm`. Feed **tile-sized HLS** (≤720p, prefer ~360–540p).
Overflow tiles stay on `FfmpegDecoder`.
**CE21** (vendor 4.9, armv7hf) stays a **software wall** (instance cap
**2**, no usable DRM video plane). Optional 1-stream (maybe PIP) amvideo
experiment only — no nine-tile hardware promise. Keep two AppImages;
`dlopen` the matching ELF. Freeze Appendix A after *verify* **V1, V2, V3,
V5** on a GS-King X.

---

## 2. Path scorecard

| Path | Verdict | Why |
| --- | --- | --- |
| **libamcodec** / `/dev/amstream*` + VFM + `/dev/video10` (amlvideo timing) | **GO** — the backend | Only stock userspace decode API on **both** CE lines. `KODIPLAYER_DRIVER=libamcodec` on both. Kodi-proven at 1 stream (`STREAM_TYPE_FRAME`, `FRAME_BASE_PATH_AMLVIDEO_AMVIDEO`). Hits the same `vdec_create` / `MAX_INSTANCE_MUN` pool the datasheet’s multi-decode line describes. `dlopen` avoids a link-time proprietary dep. Overlay is self-present (no CPU `Yuv420p`); that is why stage-1 is `SelfPresented`, not `SoftwareRenderer`. |
| **amvdec_ports** vendor V4L2 M2M (`aml-vcodec-dec`, `/dev/video26`) | **NO-GO** on stock images | **CE22:** `CONFIG_AMLOGIC_MEDIA_V4L_DEC` is unset; `22.0-Piers_rc1` has no `amvdec_ports.ko`; boot `modprobe -q` is a no-op; `open` is `ENOENT`. **CE21:** `.ko` ships and should register (*verify*), but the 32-bit `compat_ioctl32` is `compat_ptr` + native 64-bit structs (not `v4l2_compat_ioctl32`), default CAPTURE is single-planar NV21 while caps advertise `_MPLANE`, default VFM is `FRAME_BASE_PATH_V4L_OSD` (not HDMI), and the instance cap is 2. Slot is taken at `S_FMT` OUTPUT and remapped to `-EINVAL`. Custom-kernel only. |
| **meson-vdec** (mainline staging `drivers/staging/media/meson/vdec/`) | **NO-GO** | Not the CoreELEC path. CE21 4.9 has no source. CE22 5.15 has a staging copy but `# CONFIG_STAGING_MEDIA is not set`, and the vendor DT is `amlogic, vdec` / `amlogic, vcodec-dec`, not `amlogic,g12a-vdec`. One live session (`-EBUSY` on a second `STREAMON`). **No HEVC** on G12A/G12B. LibreELEC’s replacement `amvdec` (stateless, 2026-09-26 LOCAL patches) is also one session and is not on CoreELEC. |
| **FFmpeg `*_v4l2m2m`** (and v4l2-request) | **NO-GO** as a backend | Both Amlogic-ce images set `GRAPHIC_DRIVERS=""` → `V4L2_SUPPORT=no` → `--disable-v4l2_m2m`. CE21 also `--disable-v4l2-request`. Device AppImages link that `libavcodec`. Even if rebuilt: no `abort()` hook inside `avcodec_receive_frame`, `STREAMOFF`/`close(fd)` run wherever `avcodec_free_context` is called (the tick thread today), buffer requeue happens on `AVFrame` unref (tick), and upstream has no `AV_PIX_FMT_DRM_PRIME` without the unused `v4l2-drmprime` patch. Allowed as a **box probe only**, and only if V8 finds symbols (it should not). |

Dead-path leftovers worth not rediscovering:

- `/dev/video10` is **amlvideo**, a capture-only VFM tap. Kodi `DQBUF`s an
  index, not pixels. It is not a decoder.
- v4l2-request needs a stateless driver. Amlogic has none.
- `#42` §§2–3 sketched `AmlogicV4l2Decoder` against a mainline CE22. That
  assumption was withdrawn. Spike from Appendix A.

---

## 3. Stage-1 spike plan (device-gated)

Exact contract: **#42 Appendix A**. Exact shape: **A.4.1**.
CE target: **CE22 first**, then the same 1-stream proof on CE21.

### 3.1 Backend / renderer shape

| Piece | Class | Contract |
| --- | --- | --- |
| Decoder | `AmlogicAmcodecDecoder : DecoderBackend` | `isHardware()==true`, `outputFormat()==SelfPresented`. Compiled only with `-DAMLOGIC_AMCODEC=ON` (default off, like `AMLOGIC_DRM_KMS`). |
| Renderer | `AmvideoRenderer` (~20 lines) | `accepts(SelfPresented)`, `present()` writes `/sys/class/video/axis` for the 1×1 cell. `commit()` no-op. |
| Gate | `Renderer::accepts(decoder->outputFormat())` | Replaces today’s `sessionLoop` refuse of `isHardware()`. `SoftwareRenderer` keeps the default (`Yuv420p` only), so existing tests stay green. |
| Teardown | `DecoderBackend::quiesce()` | New virtual, default no-op. `QuiesceOnExit` guard **inside** `sessionLoop`, on the worker, **before** `done=true`. `codec_close` / close(`/dev/amstream*`) / close(`/dev/video10`) live **only** here. `close()` on tick after `join` is CPU-only (free FFmpeg; do not `dlclose`). |
| Abort | `abort()` = atomic + `eventfd` write | No ioctl, no `codec_close`, no sysfs. `codec_write` and amlvideo `DQBUF` are `O_NONBLOCK` + `poll({fd, wake_fd_}, 100ms)`. **No helper thread** for the `/dev/video10` pump (moonlight-embedded spawns one; that breaks the 17-thread bound). |
| Token | unused in stage-1 | `SelfPresented` heartbeat from `codec_get_vdec_info` (width/height/pts). `hold` empty. `DrmPrime` / mailbox wait for a later class. |

`FfmpegDecoder` is untouched. `prepareAmlogicHwaccel` stays a no-op.

### 3.2 API delta (apply from #42 A.1; defaults keep CI green)

- `FramePixelFormat` grows `DrmPrime` and `SelfPresented`.
- `DrmPrimeDesc` + `DecodedFrame::hold` (plain ints; no libdrm in the header).
- `DecoderBackend`: `quiesce()`, `outputFormat()`.
- `Renderer`: `accepts()`, `commit()`, `clearViewport()` promoted from
  `SoftwareRenderer`.
- `StreamPlayer` takes `Renderer&`. `copyOwned` → `takeOwned`.
  `tick()` calls `commit()` after the tile loop.
- `requestStop` still only `abort()`s. Reaper / `kMaxBusyRetired` / 2 s
  `_exit` (`finishProcess` after #34) are unchanged.

### 3.3 Ordered device steps (Kodi stopped)

1. **Contract, not the wall.** One local H.264 HLS fixture **and** one live
   HTTP URL. Picture on HDMI. `abort()` unblocks `codec_write` within one
   100 ms slice. `quiesce()`/`codec_close` returns or sits on the reaper.
   SIGTERM publishes offline and `_exit`s inside 2.5 s with a wedged
   worker (inject a 5 s sleep in `quiesce()` if the driver is well-behaved).
2. **Two instances, decode only.** Second `codec_init` while the first
   runs. Watch `dmesg | grep vdec_create`. This is V2 / V10 and decides
   whether CE22’s header 9 is real for a non-Kodi client.
3. **Two pictures.** If step 2 opened, two VFM maps and two `axis`
   windows (V6). Expected failure: one layer, second picture black or
   replacing the first. That failure **selects EGL compose (stage-2)**,
   not “N video layers”.
4. **Only if V1 is yes:** 200-line V4L2 client on `/dev/video26`
   (OUTPUT H264, CAPTURE NV12, mmap → PPM). Shape B-copy via
   `SoftwareRenderer` + `accepts(Yuv420p)`. **Do not start PRIME.**
5. Soak 30 min, one stream. Watch `dmesg` for `vfree`, `codec_mm`,
   `DECODER_EMERGENCY_NO_MEM`.
6. Repeat 1–3 on **CE21**. Expect cap 2 and no usable DRM video plane.

Do **not** start stage-1 as shape B-PRIME. It needs a registered M2M
node (V1), a DRM video plane that accepts the capture fourcc/modifier
(V3), and a token/FB cache — three unverified surfaces — and it is not
what Kodi on this image does.

### 3.4 Success criteria (stage-1 ships only if all pass)

| # | Criterion |
| --- | --- |
| S1 | HDMI picture for one H.264 HLS rendition on CE22 with Kodi stopped. |
| S2 | `stop` / `restart` / SIGTERM: offline retained, process gone in ≤2.5 s, Kodi restartable. |
| S3 | A forced `quiesce()` hang leaves the other tile (if any) ticking; `_exit` still fires. Thread count ≤ 17. |
| S4 | Host CI: `QuiescingStallDecoder`, `SelfPresented` + accepting renderer, hardware-gate refuse against `SoftwareRenderer`. `AMLOGIC_AMCODEC` off in CI. |
| S5 | V1, V2, V3, V5 logged for the shipped image **before** more `src/` than the A.7 file list. |

### 3.5 First production PR file list (#42 A.7)

`decoder_backend.h`, `renderer.h`, `software_renderer.h` (`clearViewport`
override), `stream_player.{h,cpp}`, `amlogic_amcodec_decoder.{h,cpp}`,
`amvideo_renderer.{h,cpp}`, `tests/stream_player_test.cpp`,
`CMakeLists.txt` (option off). Not `FfmpegDecoder`. Not `DrmRenderer`
growth. Not `amlogic_v4l2_decoder.*`.

### 3.6 Stage-2 (after stage-1, not this spike)

CE22 3×3: keep `AmlogicAmcodecDecoder`; add a compositor renderer that
`accepts(DrmPrime)` **or** a later export from `v4lvideo` / UVM / GE2D
into linear NV12 dma-bufs, then GBM/EGL blit into one OSD FB, one
non-blocking atomic commit per tick (#40). Shape B-PRIME is a **new
class**, not a `StreamPlayer` rewrite, and only if V1 is unexpectedly
yes. Per-tile HLS variant pick belongs in the shared open path (today’s
“first playlist variant” can pick 1080p for a 360p tile).

CE21 stage-2 is software + at most two amvideo layers. A nine-tile
hardware wall on CE21 needs a custom kernel (revert patch 003) **and**
a compose engine onto fbdev. Out of scope.

---

## 4. Hard limits (CE21 vs CE22)

Sources: S922X datasheet v0.2; `media_modules-aml` pins `60561618` (CE21)
and `c15aebb1` (CE22); GS-King X DT chain; CoreELEC tags `21.3-Omega` /
`22.0-Piers_rc1`. Running values are *verify*.

| Limit | CE21 Amlogic-ng (4.9, armv7hf) | CE22 Amlogic-no (5.15, aarch64) |
| --- | --- | --- |
| **Vdec instances** (`MAX_INSTANCE_MUN`, shared amstream + V4L2) | **2** (patch 003; subject says “1”, hunk is 9→2) | **9** in source; *verify* running |
| **Scheduling** | Time-sliced; one `VDEC_STATUS_ACTIVE`. Not 9 engines. | Same |
| **VPU throughput** (datasheet, both) | HEVC/VP9 ≈ **622 Mpx/s** (`4K60 + 1080p60`). H.264 HP@L5.1 **4K30 ≈ 249 Mpx/s**. 9×720p30 ≈ 249 (at the H.264 line). 9×1080p30 ≈ 560 (HEVC only, no headroom). 9×1080p60 is 1.8× over. | Same silicon |
| **Practical H.264 1080p30** | ≤ **2** instances, then software | ~**4** by macroblock math; 9 needs ≤720p (prefer 360–540p) |
| **Public multi-stream evidence** | 1 proven (Kodi). 2 only with the cap patch removed (VDR PiP, forum). **≥3: none found.** | Same vacuum at 3–9 |
| **`codec_mm` CMA** (GS-King X DT) | **432 MiB** (`0x1b000000`, `g12b_a311d_w400.dts`) *verify* | **308 MiB** (`0x13400000`, `g12b_a311d_w400_linux.dts`) *verify* |
| **1080p buffer order-of-mag** | ~25–70 MiB/instance → 9×1080p ≈ whole pool. Tile variants required. | Tighter (308 MiB). Same conclusion. |
| **KMS video planes** | **None.** `CONFIG_AMLOGIC_DRM` unset. `/dev/dri/card0` absent or a stub (*verify*). | Vendor meson DRM: **2** YUV overlays (`video0`/`video1`) + 3 OSD. `MESON_MAX_VIDEO=3` is a driver max, not 9 tiles. |
| **amvideo layers** | VD1 + `videopip`/VD2 (2). `/sys/class/video/axis` + `axis_pip`. | Same vendor sink still used by Kodi. DRM planes exist *in addition*. |
| **9-tile display** | **No native path.** GE2D/CPU onto fbdev only. | **EGL compose → one OSD FB** (GE2D fallback). Direct scanout is 1, maybe 2. |
| **V4L2 M2M node** | `amvdec_ports.ko` shipped; `/dev/video26` should exist (*verify*). 32-bit compat is a trap. | **Not built.** `ENOENT`. |
| **amlvideo** | `/dev/video10` (inst 1 → `/dev/video23` if multi-dec) | `/dev/video10`; plus `v4lvideo` `/dev/video30+` (UVM, 9 nodes) |
| **FFmpeg hw decoders** | `--disable-v4l2_m2m --disable-v4l2-request` (`libavcodec.so.60`) | `--disable-v4l2_m2m` (`libavcodec.so.63`) |
| **`libamcodec.so`** | 32-bit ARM, SONAME unversioned, `proprietary`. `dlopen` the image copy. | 64-bit AArch64, same SONAME. Wrong ELF = cannot `codec_init`. |
| **Codecs to target** | H.264 + HEVC (whitelist). MJPEG: software (forum: S922X HW broken). AV1/H.266: later SoCs. | Same. HEVC `double_write_mode` can emit 1/2 or 1/4 linear YUV (V4L2 path only). |
| **AppImage** | `arm-linux-gnueabihf`, glibc ~2.38, FFmpeg 6.0.1 | `aarch64-linux-gnu`, glibc ~2.44, FFmpeg 9.0.2 |

Admission policy (both lines, once a hardware backend exists):

1. UI cap stays 9 (`LayoutEngine::kMaxStreams`).
2. Hardware slots ≤ running `MAX_INSTANCE_MUN` (2 / *verify* 9).
3. Σ width×height×fps ≤ budget. Start at the **H.264 249 Mpx/s** line.
4. Soft fail in order: lower rendition → software overflow tile → refuse
   `add`. Never let the (N+1)th `codec_init` be admission control
   (kernel failures are sticky: VFM black, `vfree`, CMA fragment).

---

## 5. Open *verify* probes (GS-King X, before freezing design)

Deduped from #36 §7, #37 §6.7, #38 §9, #39 §6–7, #40, #41, #42 A.6,
#43 checklist, #45, #46 §6. Run on **CE22 first**, then CE21. Stop Kodi
(`launch-coreelec.sh` / `systemctl stop kodi`). Record `/etc/os-release`
and `uname -r` at the top of the log.

**Blocking — do not freeze Appendix A until V1, V2, V3, V5 are answered
for the image being shipped.**

| ID | Question | Why it gates the design | Command sketch |
| --- | --- | --- | --- |
| **V1** | Is vendor V4L2 M2M registered on **stock** CE22? On CE21? | Decides whether shape B exists without a custom kernel. Source says CE22 **no**, CE21 **yes**. | `ls -l /dev/video* /dev/media*`; `for n in /sys/class/video4linux/*; do echo $n $(cat $n/name); done`; `lsmod \| grep -E 'amvdec_ports\|aml_vcodec'`; `dmesg \| grep -iE 'aml-vcodec\|video26'`; `modprobe amvdec_ports; echo $?` |
| **V2** | What instance cap does the **running** kernel enforce for a non-Kodi `codec_init`? | Hardware admission under the UI cap of 9. Source: 2 / 9. | `dmesg \| grep -E 'vdec_create\|max_decode_instance'`; open N tiny H.264 ES via the spike (or a `codec_init` loop); first failure + `dmesg`. On CE21 expect fail at 3. |
| **V3** | What display path is usable with Kodi stopped? | Stage-1 `AmvideoRenderer` vs later `DrmRenderer`. | `ls -l /dev/dri /dev/fb0 /dev/amvideo /dev/video10`; `cat /sys/class/vfm/map`; `readlink -f /sys/class/drm/card0/device/driver`; `fuser -v /dev/dri/card0 /dev/amvideo /dev/fb0`; `drm_info` / `modetest` if present (CE22: OSD + `video0`/`video1`?) |
| **V5** | Which `libamcodec.so` is on the box, and can the AppImage `dlopen` it? | Wrong ELF = stage-1 cannot `codec_init`. | `ls -l /usr/lib/libamcodec*`; `file` / `readelf -h` (Class/Machine); headers present? (`/usr/include/amcodec` is often sysroot-only) |

**Picture / teardown — stage-1 is worthless if these fail.**

| ID | Question | Why | Command sketch |
| --- | --- | --- | --- |
| **V10** | What VFM map does a non-Kodi `STREAM_TYPE_FRAME` instance need so HDMI is not black? | Most-reported “decode works, picture black” failure. | `cat /sys/class/vfm/map` before/after `codec_init`; `dmesg \| grep -iE 'vdec_init\|VDEC_TYPE\|vfm'`; if black: `echo rm default > /sys/class/vfm/map` then `add default vdec.h264.00 ppmgr amvideo` (forum recipe — record what `codec_init` created on its own) |
| **V6** | Can two `STREAM_TYPE_FRAME` instances show **two independent pictures**? | Decides whether shape A can ever be a wall. Expected: no. | After two `codec_init`: two `axis` rects; look for `video2` / `pip` / `axis_pip`; record whether the second replaces the first |
| **V4** | Does `codec_close` / `VIDIOC_STREAMOFF` hang after `abort()`? | Whether `quiesce()` is insurance or the load-bearing A1 fix. | During the spike: SIGSTOP the worker mid-`codec_write`, `codec_close` from another thread, time it; `dmesg \| grep -iE 'vfree\|timeout\|vdec_release\|stuck'` |
| **V4b** | Kill -9 mid-playback; does the next `codec_init` work or is a reboot required? | Reaper retry vs “must reboot” (ACTIVE leftover). | One kill; one reopen; record `dmesg` ACTIVE leftovers |

**Budget — required before any 9-up attempt, not before the 1-stream spike.**

| ID | Question | Why | Command sketch |
| --- | --- | --- | --- |
| **V7** | How big is `codec_mm` / CMA on **this** GS-King X image? | 1080p vs tile-sized renditions. DT says 432 / 308 MiB. | `grep -iE 'Cma\|cma\|codec' /proc/meminfo`; `cat /sys/class/codec_mm/codec_mm_dump` if present; `dmesg \| grep -iE 'codec_mm\|ion-dev\|reserved'`; `/proc/device-tree/reserved-memory/` |
| **V8** | Does image FFmpeg actually lack `v4l2m2m`? | Confirms the scorecard on the box. | `ffmpeg -hide_banner -decoders \| grep v4l2`; `ffmpeg -hwaccels`; `strings /usr/lib/libavcodec.so* \| grep -c v4l2m2m` (expect 0) |
| **V9** | If V1 is yes: capture fourcc, modifier, `EXPBUF` vs DMABUF, `MIN_BUFFERS_FOR_CAPTURE`, `ORPHANED_BUFS`, `multiplanar`? | Token layout; PRIME vs CPU copy. CE21: also whether 32-bit `v4l2-ctl --all` is garbage (compat). | `v4l2-ctl -d /dev/video26 --all --list-ctrls`; `cat /sys/module/amvdec_ports/parameters/multiplanar`; REQBUFS+EXPBUF via the 200-line client |

**Identity (log once per image, not design-blocking).**

- `cat /proc/device-tree/compatible` — `amlogic,s922x` / gs-king-x.
- `ls -l /dev/amstream* /dev/amvideo /dev/amvideocap0 /dev/ge2d /dev/ion /dev/dma_heap/*`.
- `zcat /proc/config.gz | grep -E 'AMLOGIC_DRM|AMLOGIC_MEDIA_VIDEO|STAGING_MEDIA|VIDEO_MESON'` if present.
- `md5sum /usr/lib/libamcodec.so.0.0` vs the CE21 `8a779cab…` / CE22 `d28bf07f…` tarball.
- Rapid open/close 20× — black screen or not.

Do not treat a `dmesg` line containing `amvdec` as proof that mainline
`meson-vdec` is loaded.

---

## 6. Conflicts the fleet already settled

Prefer the **deeper / later** note. Listed so a reader of a sibling PR
does not revive a withdrawn claim.

| Topic | Earlier / shallower | Preferred |
| --- | --- | --- |
| CE22 decoder | #42 §§2–3 (and #36 overview) treated CE22 as mainline `meson-vdec` + `meson_drm` | #42 Appendix A, #37, #39, #41, #46: **vendor 5.15 + libamcodec** |
| CE21 first HW backend | #45 recommends a hand-rolled `/dev/video26` client first | #39 §7 + #42 A.4: **no-go on stock CE21** (compat + not HDMI + cap 2). V1/V9 are probes, not the backend. |
| CE22 `/dev/video26` | #43: “V4L2 module is loaded on both lines” | #39, #45, #46: **CE22 does not ship `amvdec_ports.ko`**. *verify* V1. |
| Vendor display | #39 first-pass: “vendor kernel is not DRM/KMS” | #40, #37 §6.5: **CE22 `CONFIG_AMLOGIC_DRM=y`** (2 video + 3 OSD). Kodi still self-presents via amvideo. CE21 has no usable DRM video plane. |
| CE22 product compositor | #40 proposed `AmlogicV4L2DecoderBackend` + UVM as the CE22 class | Decode stays **libamcodec**. EGL compose (#40 §3) is the **display** architecture for 3–9, fed by a later export (`v4lvideo`/UVM/GE2D — *verify*), not stock `video26`. |
| CE21 `libamcodec` ELF | #36 pinned the CE21 tarball as `libamcodec-aarch64-8a779cab` | #37: CE21 package is **`libamcodec-arm-…` (32-bit)**; CE22 is aarch64 `d28bf07f`. Match the AppImage ELF. |
| `codec_mm` size | #38 used ≈432 MiB from a UGOOS AM6 boot log | #45: GS-King X DT **432 MiB CE21 / 308 MiB CE22**. Still *verify* V7 on the box. |
| Patch 003 “limit to 1” | Forum / patch subject | Every later note: hunk is **9→2**. Plan for 2 on CE21. |
| “Kodi 19 dropped amcodec” | #36 overview (upstream xbmc) | #36 deep-dive / #37: **CoreELEC’s fork still uses it** on both lines. |

Unresolved on purpose (need the box, not another paper): V1–V10 above;
whether CE22 `v4lvideo` actually exports UVM dma-bufs a Mali EGL can
import; mixed H.264+HEVC concurrency; HEVC `work_buf_size` per instance.

---

## 7. Index of fleet docs (provenance)

Fetch each file from the PR **head ref**. Do not merge those PRs for this
synthesis.

| PR | Head branch | Path | Owns |
| --- | --- | --- | --- |
| [#36](https://github.com/yyolk/amlogic-multistream/pull/36) | `cursor/amlogic-hw-decode-research-af09` | `docs/research/amlogic-hw-decode-coreelec-kodi.md` | Overview: ng vs no, amcodec vs V4L2 history, Kodi black-screen. Superseded on “modern = meson-vdec”. |
| [#36](https://github.com/yyolk/amlogic-multistream/pull/36) | same | `docs/research/amlogic-libamcodec-amlcodec.md` | **Deep** Kodi `CAMLCodec` sequence, `/dev/video10` ticket (no planes), hang/close, licensing, overlay ⇒ `readFrame` false. |
| [#37](https://github.com/yyolk/amlogic-multistream/pull/37) | `cursor/amlogic-hw-decode-docs-ba5b` | `docs/research/amlogic-hw-decode-ce21-vs-ce22.md` | ABI split, FFmpeg sonames, patch 003, module load, `/dev/video10` vs `26`, DRM on/off, AppImage `libamcodec` ELF. |
| [#38](https://github.com/yyolk/amlogic-multistream/pull/38) | `cursor/amlogic-hw-decode-multistream-limits-6f29` | `docs/research/amlogic-hw-decode-multistream-limits.md` | Instance ≠ throughput; Mpx/s table; DRAM; community 1 / 2 / none-at-3–9; failure modes; admission policy. |
| [#39](https://github.com/yyolk/amlogic-multistream/pull/39) | `cursor/amlogic-hw-decode-research-621a` | `docs/research/amlogic-hw-decode-vendor-sdk.md` | Datasheet, SDK mirrors, **§7 V4L2 no-go** (compat, CE22 unbuilt, `S_FMT` → `-EINVAL`). |
| [#40](https://github.com/yyolk/amlogic-multistream/pull/40) | `cursor/amlogic-drm-kms-research-3b05` | `docs/research/amlogic-hw-decode-drm-kms.md` | CE22 plane inventory, FBC vs linear NV12, **EGL compose**, CE21 amvideo-only, DRM master after Kodi stop. |
| [#41](https://github.com/yyolk/amlogic-multistream/pull/41) | `cursor/amlogic-hw-decode-ffmpeg-c764` | `docs/research/amlogic-hw-decode-ffmpeg.md` | Upstream `*_v4l2m2m` / request / drmprime; CoreELEC `--disable-v4l2_m2m`; why not a backend. |
| [#42](https://github.com/yyolk/amlogic-multistream/pull/42) | `cursor/amlogic-hw-decode-backend-design-444e` | `docs/research/amlogic-hw-decode-backend-design.md` | **Appendix A = the contract.** Shape A vs B, threading/reaper proof, stage-1 spike, A.6 questions, A.7 file list. Ignore §§2–3 as withdrawn. |
| [#43](https://github.com/yyolk/amlogic-multistream/pull/43) | `cursor/amlogic-hw-decode-research-9550` | `docs/research/amlogic-hw-decode-code-examples.md` | Kodi / moonlight / Yasem / mpv / meson-vdec examples; no multi-stream userspace found; licensing. |
| [#45](https://github.com/yyolk/amlogic-multistream/pull/45) | `cursor/amlogic-hw-decode-research-7809` | `docs/research/amlogic-hw-decode-v4l2.md` | Vendor V4L2 ioctl sequence, NV21 default, `multiplanar`, CMA DT sizes, `/dev/video*` map. Backend recommendation superseded by #39/#42 (see §6). |
| [#46](https://github.com/yyolk/amlogic-multistream/pull/46) | `cursor/amlogic-hw-decode-meson-vdec-41b9` | `docs/research/amlogic-hw-decode-meson-vdec.md` | Mainline `meson-vdec` vs vendor `amvdec_*` vs LibreELEC `amvdec`; G12B no HEVC; **no-go** for the wall. |

This file: `docs/research/amlogic-hw-decode-SYNTHESIS.md`.
