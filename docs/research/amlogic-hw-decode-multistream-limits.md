# Amlogic S922X / GS-King X hardware-decode multi-stream limits under CoreELEC

- **Status:** draft research note — **not** an implementation, and **not** device-verified
- **Date:** 2026-11-01
- **Scope:** 1–9 concurrent hardware-decoded streams on a Beelink GS-King X
  (Amlogic S922X-H, device tree `g12b_s922x_beelink_gs_king_x`, 4 GB RAM)
  running CoreELEC 21 Amlogic-ng or CoreELEC 22 Amlogic-no, as in the two
  device AppImages described in the [README](../../README.md)
- **Deliverable relation:** research only. No source, build, or packaging
  change in this project follows from this document until the on-device
  validation plan in §9 has been run on a GS-King X.

> Nothing in this note has been run on a GS-King X. Every statement is
> labelled by evidence type: **[datasheet]**, **[kernel source]**,
> **[CoreELEC source]**, **[forum/issue report]**, **[calculation]**, or
> **[unverified]**. Where public reports conflict, both readings are given.

---

## 1. TL;DR for this project's 9-stream goal

This project caps the wall at 9 streams for a UI reason:
`LayoutEngine::kMaxStreams == 9` is the largest equal grid (`3x3`), and
`StreamManager` / MQTT reject a 10th stream on that basis (see
`src/layout_engine.h`, `src/stream_manager.cpp`). That number happens to
coincide with Amlogic's vendor-driver instance cap of 9 — it is **not**
evidence that 9 streams decode, are displayed, or are sustainable.

| Question | Short answer |
| --- | --- |
| Does the S922X VPU accept up to 9 decoder instances? | **Yes, in the vendor driver.** `MAX_INSTANCE_MUN 9` in `vdec.h`; H.264 multi driver `H264_DEV_NUM 9` [kernel source]. Instances time-share one VPU — they are not 9 parallel engines. |
| Does stock CoreELEC let other programs use all 9? | **CoreELEC 21: no** (a CoreELEC patch rewrites the cap; see §4). **CoreELEC 22: apparently the patch is gone** — re-check on the box before relying on it (§4.2). |
| Is 9× anything-at-any-resolution therefore feasible? | **No.** The datasheet rates the VPU at `4Kx2K@60 + 1×1080p@60` aggregate (§3). 9 streams fit that budget only at low per-stream pixel rates (e.g. 9×720p30 ≈ one 4K30 stream; 9×1080p60 is ~1.8× the rated aggregate). |
| Is DRAM bandwidth the first wall? | **Probably not at tile-sized resolutions** (§5). Decoded-output traffic for 9×1080p30 is ≈0.84 GB/s against a ≈10.7–12.8 GB/s theoretical DRAM ceiling. Buffer-pool capacity (§6) and VPU throughput (§3) bind earlier; display composition may bind before either (§7). |
| Has anyone publicly demonstrated 9 concurrent streams on S922X / GS-King X? | **No report found.** The strongest public multi-stream evidence is 2 streams (VDR picture-in-picture with the CoreELEC cap removed) [forum report]. One Khadas poster could not get even a 2nd vendor-driver stream to open on a sibling SoC [forum report]. See §8. |
| What is practical today, unmodified? | **1 stream** is proven everywhere. **2** is plausible/proven in the VDR case with a kernel change. **3–9 are unproven at any resolution** and must be treated as an experiment with an admission budget (§10), not as a spec. |
| What limits this project *right now*? | Not the VPU. The only backend is CPU software decode (`FfmpegDecoder::isHardware() == false`), and `StreamPlayer` actively refuses a hardware backend. Today's 9-stream ceiling is the S922X CPU, which is also unmeasured on-device (README, "Still requires a GS-King X"). |

**Bottom line:** design for *9 tiles*, budget for *an aggregate pixel rate*,
and expect the first hardware wall to appear somewhere in the 2–6 stream
range at 1080p-class resolutions — earlier if instances, buffers, or the
display path reject streams before throughput does. Tile-resolution
(≤720p, ideally ≤640×360) HLS renditions are what make 9 arithmetically
plausible at all.

---

## 2. Project context: where "9" comes from here

- `LayoutEngine::suggestLayout()` maps 7–9 streams to `3x3`, clamps anything
  above 9 to `3x3`, and `compute()` places at most `kMaxStreams` (9) tiles
  (`src/layout_engine.h`). `add` over MQTT fails with `stream limit reached`
  past 9 (`docs/mqtt.md`).
- The README goal is "Concurrent HLS streams with Amlogic hardware decoding
  (multi_vdec)", but the shipped device AppImages decode on the CPU:
  `prepareAmlogicHwaccel` in `src/ffmpeg_decoder.cpp` is a no-op marked
  "requires GS-King X / CoreELEC", `isHardware()` is always `false`, and
  `StreamPlayer::sessionLoop` fails any backend whose `isHardware()` is
  `true` ("refusing hardware decoder … (requires GS-King X / CoreELEC)").
  A hardware backend must be a new `DecoderBackend`, not a change inside
  `FfmpegDecoder` (per the comment at that hook).
- One consequence for §10: this project's HLS handling currently keeps the
  variant `av_find_best_stream` returns — for a master playlist, the *first*
  variant, not the highest **or the tile-appropriate** one
  (`src/ffmpeg_decoder.h`). For a 9-up wall, per-tile rendition choice is
  the single largest decode-budget lever available (§3.3).

So this note answers: *if* a vendor-driver hardware backend existed, what
would actually stop it between stream 1 and stream 9?

---

## 3. Limit 1 — VPU throughput (the hard physical budget)

### 3.1 Datasheet rating [datasheet]

The public S922X datasheet (v0.2, Hardkernel mirror [1]) specifies the
Amlogic Video Engine (AVE-10) as:

- "Support multi-video decoder up to **4Kx2K@60fps + 1x1080P@60fps**"
- VP9 Profile-2, H.265 HEVC MP-10@L5.1, AVS2: up to 4Kx2K@60fps
- **H.264 AVC HP@L5.1: up to 4Kx2K@30fps only** (half the HEVC pixel rate)
- MPEG-4 / VC-1 / AVS+ / MPEG-2 / MPEG-1 / RealVideo: up to 1080p60 each
- "Supports multiple 'secured' video decoding sessions and simultaneous
  decoding and encoding" — no per-session count is given in the datasheet
- Independent encoder up to 1080p60 (not used by this project)

GS-King X retail listings repeat the `4K@60 + 1080p@60` line verbatim [10].

In pixel-rate terms [calculation]:

| Unit | Pixels/frame | @30 fps | @60 fps |
| --- | ---: | ---: | ---: |
| 3840×2160 (4K UHD) | 8,294,400 | 248.8 Mpx/s | 497.7 Mpx/s |
| 1920×1080 | 2,073,600 | 62.2 Mpx/s | 124.4 Mpx/s |
| 1280×720 | 921,600 | 27.6 Mpx/s | 55.3 Mpx/s |
| 640×360 (one 3×3 tile of a 1920×1080 wall) | 230,400 | 6.9 Mpx/s | 13.8 Mpx/s |

Rated aggregate for HEVC/VP9 ≈ 497.7 + 124.4 ≈ **622 Mpx/s**. For H.264 the
headline rating is **249 Mpx/s** (4K30) — whether the "+1×1080p" companion
stream applies to H.264 at the same rate is not stated [unverified].

### 3.2 What N streams cost [calculation]

| Scenario (all streams equal) | Aggregate | vs 622 Mpx/s HEVC rating | vs 249 Mpx/s H.264 rating |
| --- | ---: | ---: | ---: |
| 9 × 640×360@30 | 62 Mpx/s | 10% | 25% |
| 9 × 720p30 | 249 Mpx/s | 40% | **100% (at the line)** |
| 9 × 720p60 | 498 Mpx/s | 80% | 200% |
| 9 × 1080p30 | 560 Mpx/s | 90% | 225% |
| 9 × 1080p60 | 1,120 Mpx/s | **180%** | 450% |
| 4 × 1080p30 | 249 Mpx/s | 40% | 100% |
| 2 × 1080p60 | 249 Mpx/s | 40% | 100% |

Reading: **9×1080p60 hardware decode is outside the datasheet envelope for
every codec; 9×1080p30 is inside it only for HEVC/VP9-class throughput and
with no headroom** for reference-frame re-reads, a 4K stream mixed in, the
encoder, or inefficiency. Macroblock-rate, bitrate, profile/level, and
reference-frame count move the real limit below the pixel-rate estimate —
pixel rate is a necessary, not sufficient, condition [calculation caveat].

### 3.3 Implication already visible in this codebase

Decode cost follows the **source** resolution, not the tile size. Scaling a
1080p decode down to a 640×360 tile spends full 1080p VPU throughput (the
scaling itself is cheap; the decode is not). A 9-up wall fed with
1080p/4K HLS renditions therefore spends 9× the budget of the same wall fed
with ≈360p–720p renditions. Any future backend needs a per-tile rendition
picker; the current "first variant in playlist order" behaviour (§2) can
accidentally pick the most expensive rendition for every tile.

---

## 4. Limit 2 — VPU instance caps (the administrative budget)

### 4.1 Vendor driver (`media_modules-aml`, what CoreELEC ships) [kernel source]

CoreELEC does **not** use mainline `meson-vdec`; it builds Amlogic's vendor
`media_modules-aml` (`CONFIG_AMLOGIC_MEDIA_VDEC_H264_MULTI=m`,
`..._H265=m`, `..._VP9=m`, … in each branch's `package.mk`).

- Global cap: `#define MAX_INSTANCE_MUN 9` in
  `drivers/frame_provider/decoder/utils/vdec.h` (sic — `MUN`) [2].
- H.264 multi-instance driver (`ammvdec_h264`, `h264_multi/vmh264.c`):
  `#define H264_DEV_NUM 9`, `max_decode_instance_num = H264_DEV_NUM` [3].
- HEVC multi-instance driver (`ammvdec_h265`, legacy 4.9-era source):
  `MAX_DECODE_INSTANCE_NUM 12` [4] — a per-codec array size; the global
  cap of 9 is expected to bind first where both apply [unverified which
  check fires first on the CoreELEC builds].
- Scheduling is **time-sliced, not parallel**: `vdec.c` keeps a
  connected/active list, exactly one `VDEC_STATUS_ACTIVE` instance "owns
  HW resources" at a time, round-robin from the instance after the
  previous active one; "Only support time sliced decoding for frame based
  input" [5]. Nine instances therefore means nine contexts taking turns —
  aggregate throughput is still §3's budget, plus context-switch overhead
  and per-instance scheduling latency that grows with stream count.
- Mixed codecs are not one pool: mainline's binding notes VDEC_1 and
  VDEC_HEVC "share the 'vdec' IRQ and as such cannot run concurrently"
  [6] (mainline driver statement; the vendor stack schedules around the
  same physical blocks). A wall mixing H.264 tiles with HEVC tiles is a
  different — and much less publicly exercised — path than 9× one codec.
  Dual-layer Dolby Vision additionally consumes a master/slave decoder
  pair in this driver family [5], i.e. two slots for one picture.

### 4.2 The CoreELEC patch — read the code, not the subject line [CoreELEC source]

CoreELEC carries `003-multi-decoders-limit-maximum-number-of-decoder.patch`.
Its subject says "limit maximum number of decoder in parallel **to 1** …
Kodi can only handle one decoder (12/2019). Because of threading issues in
Kodi it is needed to limit the multi decoder instances … If Kodi do
implement multiple decoder support in parallel this decoder limit needs
to be reverted." The diff actually changes `MAX_INSTANCE_MUN 9` → **`2`**
and deletes a duplicate define in `vdec_profile.c` [7]. Forum summaries
(including by the patch author) consistently describe it as a limit "to
one stream" because Kodi, the only consumer they target, opens one [8][9].
Both readings matter here:

- **CoreELEC 21 (Amlogic-ng, kernel 4.9):** the patch is present at
  `projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml/patches/003-…`
  (verified by fetching `coreelec-21`, 2026-11-01). A non-Kodi consumer on
  a stock CE21 GS-King X image should expect the patched value — whatever
  the running kernel reports — not 9. Since this project stops Kodi
  before running (`scripts/launch-coreelec.sh`), Kodi's own limitation is
  not the binding one; the patched cap is.
- **CoreELEC 22 (Amlogic-no, kernel 5.15):** the same tree fetch found
  **no `patches/` directory at all** under the CE22
  `media_modules-aml` package, and both branches' pinned
  `media_modules-aml` commits (CE21 `6056161…`, CE22 `c15aebb…`, checked
  2026-11-01) still carry the unpatched `MAX_INSTANCE_MUN 9` in their
  vendored `vdec.h`, as does the current default branch
  (`5.15.196_20260225`) [2][3]. That
  suggests the limit was dropped (not moved) for CE22 — but "patch file
  absent" is not proof of the running value on a given image
  **[unverified until checked on the box, §9]**.
- Per-device patch locations have moved between branches before, which
  has already confused at least one VDR user comparing `-ng` and `-ne`
  builds [forum report, 11]. Do not infer the cap from the CoreELEC
  version number alone; read it off the target (§9, step 0).

### 4.3 Mainline stack (for contrast — not the CoreELEC path) [kernel source]

The upstream `meson-vdec` V4L2 M2M driver is explicitly **single-instance**
("The decoder is single instance", driver submission [12]; restated by
mainline developers: "AML drivers do support multi-instance but …
for now the mainline drivers are limited to single instance" [13]).
FFmpeg's `h264_v4l2m2m` / `hevc_v4l2m2m` on a mainline kernel is therefore
a 1-stream path no matter what the hardware can do, and stock
`h264_v4l2m2m` outputs NV12 rather than `AV_PIX_FMT_DRM_PRIME` without
out-of-tree FFmpeg patches [14]. This matters because the `TODO(hwaccel)`
comment in `src/ffmpeg_decoder.cpp` names "V4L2 M2M (h264_v4l2m2m,
hevc_v4l2m2m, AV_PIX_FMT_DRM_PRIME)" as the insertion point: **on CoreELEC's
vendor kernel the multi-instance path is the Amlogic amcodec/`multi_vdec`
interface Kodi uses (`/dev/amstream*`, `vdec.*` providers), not mainline
V4L2 M2M.** Choosing between them is an open design question for the
future backend (§10), and public "ffmpeg v4l2m2m works/doesn't work on
Amlogic" reports mostly describe the mainline stack, not CoreELEC's.

---

## 5. Limit 3 — Memory bandwidth [datasheet + calculation]

- Interface: 32-bit DRAM, dual-rank, ≤4 GB total [1]. Controller supports
  DDR4-2666 and LPDDR4-3200, giving a **theoretical peak of
  2666 MT/s × 4 B = 10.7 GB/s (DDR4) to 3200 MT/s × 4 B = 12.8 GB/s
  (LPDDR4)** [calculation]. Retail GS-King X listings disagree on whether
  the fitted part is DDR4 or LPDDR4 and do not state its clock [10], so
  plan against the 10.7 GB/s end. Sustained bandwidth shared with CPU,
  Mali-G52, display scanout, and GbE/USB DMA is materially below peak.
- Decoded-output write traffic alone (NV12/YUV420P 8-bit = 1.5 B/px)
  [calculation]:

  | Load | Per stream | ×9 |
  | --- | ---: | ---: |
  | 720p30 | 41 MB/s | 0.37 GB/s |
  | 1080p30 | 93 MB/s | 0.84 GB/s |
  | 1080p60 | 187 MB/s | 1.68 GB/s |
  | 4K60 | 746 MB/s | — |

  Motion-compensation reference reads/writes multiply VPU-side traffic
  (codec-, content-, and reference-count-dependent; a 2–4× multiplier on
  the output figure is a planning heuristic, not a measurement
  [unverified]). Even at 4×, 9×1080p30 ≈ 3.4 GB/s of VPU traffic plus
  composition (§7) stays under the theoretical ceiling — **bandwidth is
  unlikely to be the first limit at ≤1080p30, and is a plausible
  co-limit at 1080p60/4K mixes or with uncompressed (non-AFBC) 10-bit
  buffers.** AFBC/MMU-compressed HEVC buffers in the vendor stack reduce
  this further when enabled [unverified on GS-King X builds].
- Network is not a decode limit here: 9 HLS renditions at 1–8 Mbit/s each
  are 9–72 Mbit/s against Gigabit Ethernet. Per-stream bitrate still
  matters to the VPU (entropy decode) and to live-edge behaviour (§8.3).

---

## 6. Limit 4 — Decoder buffer pools (`codec_mm` / CMA / ION)

Throughput arithmetic assumes buffers exist. Each instance needs its own
bitstream buffer, decoded-picture buffer (reference frames + reorder +
output queue), and firmware work area, allocated from the reserved
`codec_mm` pool (vendor stack) rather than the general page allocator.

- Order-of-magnitude sizing [calculation, assumption stated]: at
  8–12 frame-sized buffers per instance, 1080p (3.11 MB/frame NV12)
  costs ≈25–37 MB per stream → **≈225–335 MB for 9 streams before
  work/bitstream buffers**; 720p (1.38 MB/frame) costs ≈11–17 MB →
  ≈100–150 MB for 9. Legacy HEVC vendor source also carries a
  `work_buf_size = 48 MiB` constant [4] — whether that is per-instance
  on current builds is **[unverified]**, but if it were, 9 instances
  (432 MiB) would alone meet or exceed the pool size below.
- Reference point [forum log]: a CoreELEC Amlogic-no boot log (UGOOS AM6,
  not a GS-King X) reserves `linux,codec_mm_cma` 442,368 KB (≈432 MiB),
  plus `linux,ion-dev` 128 MiB and smaller `ion-fb`/`ppmgr` pools, with
  generic CMA at only 8 MiB [15]. **The GS-King X DT values must be read
  off the box (§9)**; treat ≈432 MiB as the planning scale. On that
  scale, 9×1080p buffer demand is the same order as the whole pool —
  feasible only if per-instance allocations land at the low end, and
  fragile under fragmentation (§8.2).
- The vendor error taxonomy names this failure directly:
  `DECODER_ERROR_ALLOC_BUFFER_FAIL` and `DECODER_EMERGENCY_NO_MEM` in
  `vdec.h` [2]. The mainline analogue is
  `VIDIOC_REQBUFS failed: Cannot allocate memory` from `h264_v4l2m2m`
  [16] and stalled capture when too many DRM_PRIME buffers are retained
  [17] — same physics, different stack.

---

## 7. Limit 5 — Display composition (decoding 9 ≠ showing 9)

Even with 9 decoded frame streams, the G12 display hardware cannot scan
out 9 video planes:

- G12A-class VPU hardware provides 2 video inputs and up to 4 OSD planes
  [18]; the mainline DRM driver exposes OSD1 as the primary plane plus
  **one** video overlay plane (VD1), with pre-blend video pre-scaling
  listed as missing [19]. The vendor `amvideo` path likewise presents
  one main video picture (with `ppmgr` scaling) — which is exactly why
  the CoreELEC multi-decode refusal cites "the Kernel display stream
  two and Kodi feeds stream one" confusion [9].
- Therefore a 3×3 wall must **composite**: scale + colour-convert 9
  decoded frames into one (or few) scanout buffers, via Mali-G52 (GPU),
  GE2D/ppmgr, or CPU. At 1080p60 output that is ≈0.84 GB/s of tile reads
  (9×1080p30 sources, §5) plus 0.19 GB/s scanout writes/reads
  [calculation], competing with the VPU for the same DRAM (§5) and
  adding a full frame of latency. This project's current design already
  composites on the CPU into one RGB viewport (`SoftwareRenderer`), at
  software-decode cost; a hardware design needs the zero-copy equivalent
  (DRM_PRIME dmabuf → GPU/GE2D → KMS primary plane), which does not yet
  exist here (`DrmRenderer::present()` does not scan out; README).
- Practical consequence: the stream count at which tiles start tearing,
  dropping to vsync, or missing the 20 ms present tick may be set by
  composition, not by the VPU — and it will not show up in decoder-only
  benchmarks (`ffmpeg -benchmark` style tests overstate the wall).

---

## 8. Reported community experience

### 8.1 What has actually been demonstrated in public

| # streams | Evidence | Setup | Outcome |
| ---: | --- | --- | --- |
| 1 | Extensive [forum/issue] | Kodi/CoreELEC `multi_vdec` (frame-based) on many S922X boxes; Moonlight-embedded with `multi_vdec` added [8] | Works; required correct VFM map (`add default vdec.h265.00 ppmgr amvideo`), otherwise black picture. Non-multi path produced `Trying to vfree() bad address` on decoder release [8]. |
| 1 (mainline) | Extensive | `meson-vdec` + FFmpeg/mpv v4l2m2m [13][14] | Works single-instance; HEVC on early mainline froze boards [13]; buffer-request failures at 4K [16]. |
| 2 (PiP) | **[forum report]** VDR + `softhdodroid` on CoreELEC, Odroid N2+ (S922X) [11] | Kernel rebuilt **with the CoreELEC limit patch removed** | PiP reported working, Kodi still working; handover between Kodi and VDR occasionally leaves a black picture — the only fix considered safe by participants was reinstating the limit. German-language thread; no frame-rate/resolution matrix published. |
| 2 (attempt) | **[forum report]** Khadas VIM3L (S905D3, sibling SoC) [20] | Amlogic libplayer examples modified to open `/dev/amstream_vbuf_sched` | **Second stream open failed** despite the datasheet's "4×1080p60" multi-video line and the multi-instance firmware loading. No root cause found in-thread. Different SoC/driver build — not direct S922X evidence, but the clearest public "datasheet ≠ works" data point. |
| 3–9 | **None found** | — | No forum post, GitHub issue, or benchmark located (searched 2026-11-01) demonstrating ≥3 concurrent hardware decodes on S922X, let alone 9 on a GS-King X. CoreELEC's position remains Portisch (2022): Kodi "can only handle one video stream … Amlogic hardware would be able to handle up to 9" [9] — a statement about the driver cap, repeated since 2019, not a measurement. |

### 8.2 Failure modes when oversubscribed (observed or directly implied)

1. **Instance refusal at open.** The (N+1)th decoder open fails or never
   becomes ready when the cap in §4 binds — N is 9 unpatched, the
   patched value on CE21, 1 on mainline. On the Khadas attempt this is
   what a 2nd stream hit [20]. *Signature:* open-time error, no frames,
   other streams unaffected.
2. **Black or wrong picture, decode "working".** VFM-map / receiver
   misrouting: frames decode but are bound to `amvideo`/`ppmgr`
   incorrectly, or the kernel displays stream 2 while the app feeds
   stream 1 [8][9]. *Signature:* `dmesg` clean, counters advance, HDMI
   black or frozen. This is the most-reported multi-stream failure and
   it is a **display-path** failure, not a decode failure (§7).
3. **Teardown corruption.** Releasing a non-multi (`VDEC_TYPE_STREAM_PARSER`)
   decoder produced `Trying to vfree() bad address` [8]; Kodi's
   open/close handling is called "faulty" by CoreELEC in the multi
   context [9]. *Signature:* kernel warnings on stop/restart, worsening
   with churn — directly relevant to this project's `restart` and
   reaper design, which retires decoders without joining blocked workers.
4. **Buffer-pool exhaustion / fragmentation.** `DECODER_ERROR_ALLOC_BUFFER_FAIL`
   / `DECODER_EMERGENCY_NO_MEM` (vendor) [2], `VIDIOC_REQBUFS: Cannot
   allocate memory` (mainline) [16]. Reserved pools fragment under
   repeated open/close of different resolutions; "although enough memory
   may be available, it is not contiguous and the allocation fails"
   (Amlogic-no discussion, about CMA display buffers [15]).
   *Signature:* failure count grows with stream count **and** with
   uptime/churn; a reboot temporarily "fixes" it.
5. **Scheduler starvation and timeouts.** One active instance at a time,
   round-robin [5]: as aggregate demand approaches §3's budget, every
   stream's frame latency rises together; vendor timeout handling
   (`decode_timeout_count`, timeout work in `vh265` [4]) resets laggards.
   *Signature:* all tiles drop frames roughly uniformly, live HLS tiles
   drift behind the edge and eventually skip segments; one 4K/high-fps
   stream degrades all others rather than only itself.
6. **Throughput cliff.** Past the pixel/macroblock budget the failure is
   not graceful per-stream degradation in any public report — expect
   dropped frames, A/V desync (mpv dropped-frame reports in the
   mainline buffer-failure case [16]), and decoder resets.
   **[unverified for the vendor stack at 3–9 streams — §9 exists to
   measure this.]**
7. **Retained-buffer stall (zero-copy specific).** Holding DRM_PRIME /
   V4L2 buffers too long starves the capture queue
   ("V4L2 poll capture empty" → I/O error [17]). A compositor that keeps
   one frame per tile (§7) must bound retained buffers per instance or
   it manufactures failure mode 4/5 at low stream counts.

### 8.3 Software-decode baseline (today's actual path)

For calibration: S922X software 1080p H.264 (incl. Hi10P, which has no
hardware path) is reported comfortable on the 4×A73 cluster (~35–40% CPU
for one 1080p Hi10P stream, forum reports [21]). Nine software decoders on
six cores — before the CPU colour-conversion/composition this project
also does — is a different regime entirely and, per the README, has never
been measured on the GS-King X. Do not use single-stream software headroom
to argue for 9 software tiles either.

---

## 9. On-device validation plan (GS-King X, required before any claim)

Run under the exact image to be supported (CE21-ng **and** CE22-no
separately). Stop Kodi first, as `scripts/launch-coreelec.sh` does.

0. **Read the actual caps off the box.** Record `/etc/os-release`,
   `uname -a`, `ls /dev/amstream* /dev/vdec* 2>/dev/null`,
   `cat /sys/class/vfm/map`, `grep -E 'Cma|codec' /proc/meminfo`,
   reserved-memory sizes from the DT
   (`/proc/device-tree/reserved-memory/*/reg` or `dtc -I fs /proc/device-tree`),
   and `dmesg | grep -iE 'vdec|codec_mm|ion'`. This settles §4.2 and §6
   for that image instead of trusting branch archaeology.
1. **Ramp:** open 1, 2, 3, 4, 6, 9 concurrent instances of the *same*
   stream (looped local HLS/TS, no network variance), 5 min per step.
   Matrix at minimum: {H.264, HEVC} × {720p30, 1080p30, 1080p60} ×
   {360p-class rendition for the 9-step}. Log per-stream decoded fps,
   dropped frames, `dmesg`, and pool free space at each step.
2. **Find which limit fires first** by watching for the §8.2 signatures
   in order: open failure (instance cap) → alloc failure (buffers) →
   uniform frame drops (throughput/scheduling) → black tiles
   (display/VFM). Change one variable at a time (resolution, then
   count, then codec mix).
3. **Churn:** 50 open/close cycles at the highest passing count, then
   repeat step 1's last passing step. Fragmentation failures (§8.2.4)
   only appear here.
4. **Soak:** 2 h at the target wall (proposed: 9× tile-size renditions).
   Watch thermals/clocks (`/sys/class/thermal/…`), timeouts, and
   live-edge drift per tile.
5. **Composition check:** repeat the passing decode ramp *with* the
   §7 compositor attached at 1080p60 output. A decode-only pass does
   not predict the wall (§7).

Publish the resulting table (streams × codec × res × fps → sustained?)
as an update to this note; until then §1's "unproven" stands.

---

## 10. Implications for this project (recommendations, not commitments)

1. **Keep the 9-tile UI cap; add an aggregate decode budget beneath it.**
   Admission should be in Mpx/s (and per-codec instance counts), e.g.
   "Σ width×height×fps ≤ budget, ≤9 instances", with the budget set from
   §9 measurements — starting conservatively near the H.264 line
   (249 Mpx/s) if H.264 must be supported, since it is this project's
   most common HLS codec and the datasheet's weaker rating.
2. **Pick renditions per tile size.** A 640×360 tile should open a
   ≤720p (preferably ≈360p–540p) HLS variant. This is the difference
   between 62 and 560 Mpx/s for the same 9-up wall (§3.2), and it
   requires replacing the current first-variant behaviour (§2) in the
   future hardware backend — ideally in the shared open path so the
   software fallback benefits too.
3. **Decide the kernel interface explicitly.** Vendor amcodec/`multi_vdec`
   (multi-instance, CoreELEC-native, Kodi-proven at 1 stream, VFM-map
   and `/dev/amstream*` quirks) vs mainline-style V4L2 M2M DRM_PRIME
   (cleaner FFmpeg story, single-instance on mainline, not what
   CoreELEC's kernel is for) (§4.3). The existing `TODO(hwaccel)` comment
   conflates them; the future `DecoderBackend` should name one.
4. **Ship a patched-kernel story or target CE22 — knowingly.** On stock
   CE21 the instance cap is patched down (§4.2); a 3+-stream hardware
   backend there implies either a custom CoreELEC build (removing
   patch 003, accepting the Kodi-interaction risk CoreELEC cites — this
   project stops Kodi, which mitigates but does not erase it) or a
   documented limitation to the patched cap. CE22 may not need this;
   verify per §9 step 0 before promising either.
5. **Design the compositor as a first-class limit.** One scanout plane
   (§7) means GPU/GE2D composition with bounded DRM_PRIME buffer
   retention (§8.2.7); budget its DRAM share (§5/§7) alongside decode.
6. **Make oversubscription fail soft.** When the budget in (1) is
   exceeded, prefer (in order): lower rendition, lower tile frame rate
   (present every 2nd/3rd frame — decode cost unchanged, so this only
   helps composition), software-decode overflow tiles at low resolution,
   refuse the new stream (`add` already has a refusal path). Never let
   the (N+1)th hardware open be the admission control — §8.2 shows its
   failures are kernel-level and sticky.
7. **Instrument from day one.** Per-stream decoded/presented fps, drop
   counts, open-failure reasons, and pool headroom belong in MQTT status;
   without them §8.2's failure modes are indistinguishable from the
   outside.

---

## 11. Open questions

- What is `MAX_INSTANCE_MUN` (and the per-codec `max_decode_instance_num`)
  in the *running* kernels of the pinned CE21/CE22 images on GS-King X?
  (§9 step 0)
- Is the HEVC `work_buf_size` per-instance on current `media_modules-aml`,
  and what does one idle-but-open instance actually reserve from
  `codec_mm` at 720p/1080p/4K?
- Mixed-codec concurrency: can H.264-multi and HEVC-multi instances be
  open simultaneously on the vendor stack, and at what aggregate?
- Does the GS-King X DT/`codec_mm` pool match the ≈432 MiB reference
  in §6, and how fragmented is it after Kodi has played one stream?
- What does `ppmgr`/GE2D composition of 9×720p→1080p60 cost on the
  Mali-G52/DRAM, vs GPU composition? (No public numbers found.)
- GS-King X fitted DRAM type/clock (hence which end of §5's range
  applies)? Vendor listings conflict [10].

## 12. Sources

1. S922X Public Datasheet v0.2 (Hardkernel mirror) — AVE-10 ratings, DRAM interface: <https://dn.odroid.com/S922X/ODROID-N2/Datasheet/S922X_Public_Datasheet_V0.2.pdf>
2. CoreELEC `media_modules-aml`, `drivers/frame_provider/decoder/utils/vdec.h` (`MAX_INSTANCE_MUN 9`, error taxonomy): <https://github.com/CoreELEC/media_modules-aml/blob/5.15.196_20260225/drivers/frame_provider/decoder/utils/vdec.h>
3. Amlogic 4.9 `h264_multi/vmh264.c` (`H264_DEV_NUM 9`): <https://github.com/gcsuri/aml_dvb-4.9/blob/master/drivers/frame_provider/decoder/h264_multi/vmh264.c>
4. Legacy `vh265.c` (`MAX_DECODE_INSTANCE_NUM 12`, `work_buf_size 48 MiB`): <https://github.com/osmc/vero3-linux/blob/master/drivers/amlogic/amports/vh265.c>
5. Legacy `vdec.c` (time-sliced scheduling, one active instance, master/slave): <https://github.com/osmc/vero3-linux/blob/master/drivers/amlogic/amports/vdec.c>
6. Mainline DT binding, `amlogic,gx-vdec` (VDEC_1 / VDEC_HEVC share IRQ): <https://www.kernel.org/doc/Documentation/devicetree/bindings/media/amlogic%2Cgx-vdec.yaml>
7. CoreELEC patch 003 as fetched from `coreelec-21` (subject "to 1", diff `9`→`2`): <https://raw.githubusercontent.com/CoreELEC/CoreELEC/coreelec-21/projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml/patches/003-multi-decoders-limit-maximum-number-of-decoder.patch>
8. CoreELEC forum, "Experiments with hw decoding" (Portisch et al.: 9-instance statement, patch, VFM-map fix, `vfree` error): <https://discourse.coreelec.org/t/experiments-with-hw-decoding/16349>
9. CoreELEC forum, "Multidecode" feature request (Portisch, 2022: Kodi one stream, black-screen rationale): <https://discourse.coreelec.org/t/multidecode/18514>
10. GS-King X listings (4 GB, `4K@60 + 1080p@60`; RAM type inconsistently stated): <https://www.geekbuying.com/item/Beelink-GS-King-X-S922X-H-4GB-64GB-NAS-Dolby-DTS-4K-TV-Box-425755.html>
11. VDR-Portal, `softhdodroid` thread (PiP with patch removed, black-screen handover, patch-location confusion): <https://www.vdr-portal.de/forum/thread/135593-softhdodroid-nach-wechsel-von-kodi-zu-vdr-bleibt-bild-manchmal-schwarz/>
12. Mainline `meson-vdec` submission ("The decoder is single instance"): <https://lwn.net/Articles/767126/>
13. ODROID forum, "V4L2 M2M decoders on mainline: need testers!" (mainline single-instance statement, HEVC freeze warning): <https://forum.odroid.com/viewtopic.php?t=30724>
14. FFmpeg `v4l2-drmprime` patch requirement / NV12-vs-DRM_PRIME analysis: <https://github.com/dogukangecko/kodi-hwdec-amlogic-armbian>
15. CoreELEC forum, Amlogic-no discussion (boot-log reserved pools ≈432 MiB `codec_mm_cma`; CMA fragmentation explanation): <https://discourse.coreelec.org/t/amlogic-no-discussion/46663?page=95>
16. mpv issue #8825, `h264_v4l2m2m: VIDIOC_REQBUFS failed: Cannot allocate memory` (4K, meson-vdec): <https://github.com/mpv-player/mpv/issues/8825>
17. jc-kynesim/rpi-ffmpeg issue #43 (retained DRM_PRIME buffers stall V4L2 decode): <https://github.com/jc-kynesim/rpi-ffmpeg/issues/43>
18. Linux DRM Meson VPU docs / G12A match data (2 video, 4 OSD on G12A): <https://dri.freedesktop.org/docs/drm/gpu/meson.html>
19. drm/meson overlay-plane series (one VD1 overlay, preblend missing): <https://lists.freedesktop.org/archives/dri-devel/2018-November/195605.html>
20. Khadas forum, "VIM3L: multi-stream hardware video decode… how?" (2nd vendor stream open fails, S905D3): <https://forum.khadas.com/t/vim3l-multi-stream-hardware-video-decode-how/13599>
21. CoreELEC/Kodi forums on S922X software Hi10P 1080p headroom (single-stream calibration only): <https://discourse.coreelec.org/t/is-hi10p-playback-ok-with-ce-on-ng-boxes/10210>

*Branch/path checks in §4.2 were made against the public CoreELEC trees
on 2026-11-01; both branches move, so re-verify before acting on them.*
