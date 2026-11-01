# Amlogic hardware decode: open-source code examples

Research only. No implementation, no behaviour change. Written 2026-11-01.

Goal: find open-source code that drives Amlogic (S922X / G12B class) video decode hardware on Linux, and say how each piece applies to `DecoderBackend` in this repo. For every example: repo URL, key files, license, whether it handles several streams at once, and how it applies here.

## How to read this document

Evidence tags:

- **[V]** verified by reading the source at the pinned commit listed in the entry (cloned and grepped, not recalled).
- **[S]** secondary: a forum thread, issue, or wiki page. Treat as a lead, not a fact.
- **[I]** inference from verified facts. Needs a device check.

Applicability rating for this repo: **High** (use as a design or code template), **Medium** (useful part), **Low** (background only), **Reference only** (do not copy; license or scope blocks reuse).

Nothing here was run on a GS-King X. Every claim about runtime behaviour is **[V]** about source or **[S]**/**[I]** about the device. The checklist at the end lists what to try on the box.

Coverage limits: GitHub code search was rate limited during this research, so discovery leaned on repository search, the CoreELEC/LibreELEC trees, and web search. "We found no project that does X" means "not found in that search", not "does not exist".

## Findings in brief

1. **There are two incompatible Amlogic decode stacks on the boxes this repo targets, and they differ on multi-stream.**
   - The **vendor stack** (CoreELEC's `media_modules-aml`, Amlogic kernel 4.9 on CoreELEC 21 and 5.15 on CoreELEC 22) has a real multi-instance decoder core. `MAX_INSTANCE_MUN` is 9 in the CoreELEC 22 source. It is reached two ways: libamcodec `/dev/amstream*` (what Kodi uses) and a V4L2 mem2mem node `/dev/video26` (`aml-vcodec`).
   - The **mainline-style stack** (staging `meson-vdec`, and the new stateless `amvdec` that LibreELEC master carries as patches) allows **one streaming session at a time**. Both return `-EBUSY` on a second `STREAMON`. It cannot serve a multi-stream wall.
2. **CoreELEC throttles multi-instance on CoreELEC 21.** `003-multi-decoders-limit-maximum-number-of-decoder.patch` in the CoreELEC 20 and 21 packages changes `MAX_INSTANCE_MUN` from 9 to 2. Its commit message says "limit ... to 1". The `0..N-1` ID range in `vdec_create()` suggests 2. The patch is not in the CoreELEC 22 package at the pinned version. The GS-King X can run either line, so the hardware stream ceiling may differ by line.
3. **CoreELEC's own FFmpeg is built without V4L2 decoders.** `config/graphic` leaves `V4L2_SUPPORT="no"` and Amlogic-ce sets `GRAPHIC_DRIVERS=""`, so `ffmpeg/package.mk` takes the `--disable-v4l2_m2m` branch on CoreELEC 21 and 22. This repo's device AppImage links CoreELEC's `libavcodec`. So `h264_v4l2m2m` should not be available through the linked FFmpeg today, whatever the kernel offers (derived from build config; the checklist has the on-device check).
4. **No project found does multi-stream Amlogic hardware decode in userspace.** Kodi, libamcodec examples, Yasem, and moonlight-embedded are all single-stream. One 2021 forum report says Amlogic's own LibPlayer examples fail on the second stream. A CoreELEC developer says the hardware supports up to 9 parallel streams and that CoreELEC limited it to one because Kodi cannot handle more [S]. This repo would be breaking new ground, so plan a device spike before committing to a design.
5. **`DecoderBackend` as written fits a CPU-frame hardware path, not a zero-copy or self-presenting one.** `readFrame()` returns CPU `Yuv420p` planes, and `StreamPlayer::sessionLoop` currently rejects any backend whose `isHardware()` is true. Both need design work for any hardware backend. Details are in [Fit with `DecoderBackend`](#fit-with-decoderbackend).
6. **Licensing splits the field.** Reusable without friction: LGPL-2.1+ code (FFmpeg, mpv's `drm_prime.c` and `hwdec_drmprime*.c`) and Apache-2.0 (`BayLibre/ffmpeg-drm`). GPL-only (Kodi, moonlight-embedded, kernel drivers) is reference only unless this repo's license is chosen to match. The repo has **no LICENSE file** at the root today. libamcodec is proprietary and ships binary-only on CoreELEC.

## Summary table

| # | Example | Repo (pinned) | Key files | License | Multi-stream | Applicability |
| - | --- | --- | --- | --- | --- | --- |
| 1 | Kodi AMLCodec (CoreELEC fork) | [CoreELEC/xbmc @ d49aa4a](https://github.com/CoreELEC/xbmc/tree/d49aa4ac2362adf51da4eaa7c6ccea6e619fb860) | `AMLCodec.cpp`, `DVDVideoCodecAmlogic.cpp`, `utils/AMLUtils.cpp`, `RendererAML.cpp` | GPL-2.0-or-later (links proprietary libamcodec via `dlopen`) | Kernel yes, Kodi no (one codec, one video layer) | **High** for the vendor path; copy nothing |
| 2 | Vendor kernel decoder modules | [CoreELEC/media_modules-aml @ c15aebb](https://github.com/CoreELEC/media_modules-aml/tree/c15aebb1ac93028d0aceadf8f35dda2204d33424) (CE22), branch `amlogic-4.9.269` @ `6056161` (CE21 pin) | `utils/vdec.c`, `utils/vdec.h`, `h264_multi/vmh264.c`, `amvdec_ports/aml_vcodec_dec*.c`, `amports/amstream.c` | GPL v2 or later in headers, `MODULE_LICENSE("GPL v2")`; no top-level LICENSE | **Yes**, up to `MAX_INSTANCE_MUN` (9 on CE22 source, 2 on CE20/21 via patch) | **High** as the ABI and limits reference; kernel code, not linkable |
| 3 | CoreELEC packaging | [CoreELEC/CoreELEC @ 98a729b](https://github.com/CoreELEC/CoreELEC/tree/98a729b80ece21a7ceb8dbab127a6d19a6cff60a) (branch `coreelec-22`), `coreelec-21` | `projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml/*`, `.../multimedia/libamcodec/package.mk`, `packages/multimedia/ffmpeg/package.mk`, `config/graphic`, `devices/Amlogic-no/linux/linux.aarch64.conf` | GPL-2.0-or-later (build scripts); libamcodec `proprietary` | n/a (shows what is on the box) | **High** for what the sysroot actually contains |
| 4 | LibreELEC mainline Amlogic | [LibreELEC/LibreELEC.tv @ f59641e](https://github.com/LibreELEC/LibreELEC.tv/tree/f59641e) | `projects/Amlogic/patches/linux/amlogic-0117..0128,0209`, `packages/multimedia/ffmpeg/package.mk`, `projects/Amlogic/patches/kodi/kodi-0007..0010` | GPL-2.0-or-later (packaging); patches inherit kernel GPL-2.0 / FFmpeg LGPL | **No** (`-EBUSY` on a second session) | **Medium**: only if the project leaves CoreELEC |
| 5 | Mainline staging `meson-vdec` | [torvalds/linux `drivers/staging/media/meson/vdec`](https://github.com/torvalds/linux/tree/master/drivers/staging/media/meson/vdec) | `vdec.c`, `vdec_platform.c`, `codec_h264.c`, `codec_vp9.c`, `codec_mpeg12.c` | GPL-2.0+ | **No** (`cur_sess` claim) | **Low-Medium**: reference for stateful V4L2 behaviour; no HEVC |
| 6 | FFmpeg `v4l2_m2m` (upstream) | [FFmpeg/FFmpeg](https://github.com/FFmpeg/FFmpeg/tree/master/libavcodec) | `v4l2_m2m_dec.c`, `v4l2_context.c`, `v4l2_buffers.c`, `v4l2_fmt.c`, `v4l2_m2m.c` | LGPL-2.1+ | Per-instance contexts; limit comes from the driver | **Medium**: cheapest CPU-frame hardware path, but absent from CoreELEC's build |
| 7 | `v4l2-drmprime` / `v4l2-request` FFmpeg patches | [CoreELEC `v4l2-drmprime` patch](https://github.com/CoreELEC/CoreELEC/blob/98a729b80ece21a7ceb8dbab127a6d19a6cff60a/packages/multimedia/ffmpeg/patches/v4l2-drmprime/0001-v4l2-drmprime.patch), [chewitt/ffmpeg @ c04518b](https://github.com/chewitt/ffmpeg/commit/c04518bc5d82315e57b14a7c915581a0667d1b4c) | `v4l2_buffers.c`, `v4l2_context.c`, `v4l2_m2m_dec.c`, `v4l2_request*.c` | LGPL-2.1+ (FFmpeg) | Per instance; mainline drivers cap at 1 | **Medium**: zero-copy DRM PRIME from V4L2; not on CoreELEC's Amlogic-ce FFmpeg |
| 8 | `BayLibre/ffmpeg-drm` | [BayLibre/ffmpeg-drm @ 633b046](https://github.com/BayLibre/ffmpeg-drm/tree/633b046) | `main.c` (664 lines), `build.sh` | Apache-2.0 per source header; no LICENSE file | No | **Medium-High**: smallest decode-to-KMS-plane template |
| 9 | mpv DRM PRIME path | [mpv-player/mpv @ 914a11c](https://github.com/mpv-player/mpv/tree/914a11c) | `video/out/hwdec/hwdec_drmprime.c`, `hwdec_drmprime_overlay.c`, `video/out/drm_prime.c`, `drm_atomic.c`, `video/decode/vd_lavc.c` | GPL-2.0+ default; LGPL-2.1+ with `-Dgpl=false`; `drm_prime.c` and `hwdec_drmprime.c` carry LGPL headers | No (one player) | **High** for dmabuf-to-framebuffer import and overlay-plane commit |
| 10 | Kodi DRM PRIME path | [xbmc/xbmc](https://github.com/xbmc/xbmc) (read in the CoreELEC fork at d49aa4a) | `DVDVideoCodecDRMPRIME.cpp`, `VideoLayerBridgeDRMPRIME.cpp`, `RendererDRMPRIME*.cpp` | GPL-2.0-or-later | No | **High** as a design reference for decoder discovery and plane programming |
| 11 | jellyfin-ffmpeg | [jellyfin/jellyfin-ffmpeg @ c6869cd](https://github.com/jellyfin/jellyfin-ffmpeg/tree/c6869cd75) | `libavcodec/rkmppdec.c` (pattern), `libavcodec/v4l2_*.c`, `debian/rules` | LGPL-2.1+ (GPL when built with `--enable-gpl`) | n/a | **Low** for Amlogic (no Amlogic code); **Medium** as a pattern for a hardware decoder written as a libavcodec codec |
| 12 | moonlight-embedded Amlogic sink | [moonlight-stream/moonlight-embedded @ f32e415](https://github.com/moonlight-stream/moonlight-embedded/tree/f32e415) | `src/video/aml.c` (219 lines) | GPL-3.0 | No | **Medium**: shortest libamcodec ES-write example; GPL-3.0 means reference only |
| 13 | Yasem (CoreELEC fork) | [AytosnetTV/yasem @ 5782ee2](https://github.com/AytosnetTV/yasem/tree/5782ee2) | `src/player.cpp` (1280 lines) | Fork has no LICENSE; upstream [mvasilchuk/yasem](https://github.com/mvasilchuk/yasem) is GPL-2.0 | No (`STREAM_TYPE_SINGLE`) | **Medium**: FFmpeg demux plus libamcodec in a non-Kodi process on CoreELEC; unreviewed hobby code |
| 14 | libamcodec / LibPlayer sources | [osmc/libamcodec](https://github.com/osmc/libamcodec), [Stane1983/amlogic-libplayer](https://github.com/Stane1983/amlogic-libplayer), [hardkernel/buildroot_platform_packages_amlogic_LibPlayer](https://github.com/hardkernel/buildroot_platform_packages_amlogic_LibPlayer) | `amcodec/include/codec.h`, `examples/{esplayer,kplayer,tsplayer,psplayer,simple_player}.c` | osmc and Stane1983: no LICENSE, Amlogic copyright headers; Hardkernel: Amlogic confidential SLA text | Reported to fail on stream 2 [S] | **Reference only** |
| 15 | Small community repos | see [section 15](#15-small-community-repos) | | none declared (one has per-file GPL-2.0 SPDX) | No | **Low** |

## Fit with `DecoderBackend`

What the interface assumes today, from the source:

- `src/decoder_backend.h`: `readFrame()` yields `DecodedFrame` with CPU plane pointers; the only `FramePixelFormat` is `Yuv420p` (8-bit planar). Planes are valid until the next `readFrame()` or `close()`. The default `readFrame()` returns false so "a future DRM-prime hardware decoder stays linkable", which means the header already anticipates a backend that cannot produce CPU frames, but nothing consumes such frames.
- `abort()` must unblock an in-flight `open()` or `readFrame()` from another thread and then latch. A hardware backend that blocks in `codec_write()`, `poll()`, or `VIDIOC_DQBUF` has to honour that. Kodi's `AMLCodec.cpp` includes `sys/eventfd.h` and `sys/poll.h` and has a `SetPollDevice` helper [V], which suggests the same eventfd-plus-poll pattern would work here.
- `src/stream_player.cpp` (`sessionLoop`): `const bool hardware = opened && decoder->isHardware();` and a hardware backend is closed with `WorkerFail::Hardware`. `StreamPlayer` documents "a backend that reports isHardware() is closed". Any hardware backend therefore needs a deliberate change here, not just a new class.
- `src/renderer.h`: `Renderer::present(const DecodedFrame&, const Rect&)` takes CPU frames. `DrmRenderer` is a stub (`-DAMLOGIC_DRM_KMS=ON` opens `/dev/dri/card0` only).
- `README.md` states the intended direction: hardware decode "must not grow inside `FfmpegDecoder`; a hardware decoder should be its own `DecoderBackend`". `prepareAmlogicHwaccel()` in `src/ffmpeg_decoder.cpp` names `h264_v4l2m2m`, `hevc_v4l2m2m`, `AV_PIX_FMT_DRM_PRIME`, and `hw_device_ctx` as the intended hooks.
- `LayoutEngine::kMaxStreams` is 9 and `StreamPlayer` runs one decode thread per visible stream. That equals the CoreELEC 22 vendor `MAX_INSTANCE_MUN` of 9 [V]. On CoreELEC 21 the packaged limit is 2.

The examples fall into three backend shapes. Each is a different answer to "what does `readFrame()` return and who draws":

| Shape | Decode path | What `readFrame()` returns | Who draws | Examples | Multi-stream potential |
| --- | --- | --- | --- | --- | --- |
| **A. Self-presenting vendor decoder** | libavformat demux, packets written to libamcodec (`codec_write`) | Nothing (no CPU frames) | The kernel video layer through the VFM chain and `/sys/class/video/axis` | 1, 12, 13, 14 | Kernel supports several instances; display needs one video layer per stream. Number of usable layers unverified |
| **B. CPU-copy V4L2 decoder** | V4L2 mem2mem (`/dev/video26` vendor, `/dev/video0` mainline) through `*_v4l2m2m` | NV12/NV21 CPU frames after an mmap copy | Existing `Renderer` after adding an NV12 or NV21 `FramePixelFormat` | 2, 5, 6 | Vendor node: multiple contexts per open. Mainline: 1 |
| **C. Zero-copy DRM PRIME** | Patched FFmpeg exports dmabufs (`AV_PIX_FMT_DRM_PRIME`) | A dmabuf descriptor, not CPU planes | A KMS plane (`drmModeAddFB2` plus atomic commit) or an EGL import | 7, 8, 9, 10 | One KMS video plane on mainline `meson-drm`; mainline decoders cap at 1 session |

Shape A is the only one demonstrated on this hardware class by shipping software (Kodi on CoreELEC). Shape B has the smallest code delta but is blocked today by CoreELEC's FFmpeg build. Shape C is what mpv and Kodi use on mainline kernels, but it needs a new frame type, a dmabuf-aware renderer, a patched FFmpeg that CoreELEC's Amlogic-ce build does not carry, and (on mainline) it cannot do multiple streams.

## Per-example details

### 1. Kodi AMLCodec (CoreELEC fork)

- Repo: <https://github.com/CoreELEC/xbmc>, branch `aml-5.15.196-22.0`, read at `d49aa4ac2362adf51da4eaa7c6ccea6e619fb860`. The file is **not** in upstream `xbmc/xbmc` (404 on the contents API); it exists only in the CoreELEC fork.
- Key files (under `xbmc/cores/VideoPlayer/`): `DVDCodecs/Video/AMLCodec.cpp` (3688 lines), `AMLCodec.h`, `DVDCodecs/Video/DVDVideoCodecAmlogic.cpp` (837 lines), `VideoRenderers/HwDecRender/RendererAML.cpp`, and `xbmc/utils/AMLUtils.cpp`.
- License: GPL-2.0-or-later (SPDX header in `AMLCodec.cpp`). It talks to proprietary libamcodec: `DECLARE_DLL_WRAPPER(DllLibAmCodec, "libamcodec.so")` (lines 210-211) and `#include <amcodec/codec.h>` (line 119). The headers are needed at build time; the library is `dlopen`ed.
- What it shows [V]:
  - `codec_para_t` setup: `stream_type = STREAM_TYPE_ES_VIDEO`, `dec_mode = STREAM_TYPE_FRAME` (the multi-instance, frame-based mode), `video_path = FRAME_BASE_PATH_AMLVIDEO_AMVIDEO` (lines 2325-2336). Other codecs switch `dec_mode` to `STREAM_TYPE_STREAM` or `STREAM_TYPE_SINGLE` (lines 2400, 2436, 2479).
  - Module parameters set by sysfs before open, for example `/sys/module/amvdec_h264/parameters/dec_control` and `amvdec_h265/parameters/nal_skip_policy` (lines 2424, 2487).
  - `codec_init_para` then `codec_init` (lines 2517, 2542); `/sys/class/tsync/enable` and `/sys/class/video/freerun_mode` (lines 2560-2570).
  - VFM routing helpers `GetVfmMap` / `SetVfmMap` over `/sys/class/vfm/map` (lines 2638-2700).
  - Video placement through `/sys/class/video/axis` and `screen_mode` (lines 3608-3609); `/dev/video10` (amlvideo) opened at line 2584; `/dev/amvecm` for picture controls.
  - `DVDVideoCodecAmlogic.cpp` gates codecs on capability probes (`aml_support_hevc`, `aml_support_hevc_4k2k`, `aml_support_hevc_10bit`, lines 328-342) and converts `hvcC` to Annex-B (line ~356). Streams with length-prefixed NALs need the equivalent bitstream filter before `codec_write`.
- Multi-stream: the kernel decoder is multi-instance, and Kodi requests the frame-based multi-instance mode. Kodi still plays one video through one `CAMLCodec` and one video layer. A CoreELEC developer says CoreELEC capped the kernel decoder count because Kodi cannot handle more than one [S] (see example 2).
- Applicability: **High** as the only shipping reference for shape A on exactly this OS. Use it to learn the call order, sysfs knobs, and VFM handling. Do not copy it; it is GPL and ties to libamcodec headers with unclear terms (see licensing notes).

### 2. Vendor kernel decoder modules (`media_modules-aml`)

- Repo: <https://github.com/CoreELEC/media_modules-aml>. Branch `5.15.196_20260225`, read at `c15aebb` (the commit CoreELEC 22 pins). Branch `amlogic-4.9.269` is the CoreELEC 21 kernel line.
- Key files [V]:
  - `drivers/frame_provider/decoder/utils/vdec.h:70` defines `MAX_INSTANCE_MUN 9`. `vdec.c:1809` allocates instance IDs with `ida_simple_get(&vdec_core->ida, 0, MAX_INSTANCE_MUN, ...)` in `vdec_create()`, so the cap is hard.
  - `drivers/frame_provider/decoder/h264_multi/vmh264.c` is the multi-instance H.264 decoder (`ammvdec_h264`).
  - `drivers/stream_input/amports/amstream.c` is the `/dev/amstream*` interface that libamcodec drives.
  - `drivers/amvdec_ports/aml_vcodec_dec_drv.c` registers a V4L2 video device with node number 26 (`video_register_device(..., 26)`, lines 673-675); `aml_vcodec_dec.c` lists the formats: H.264, HEVC, VP9, MPEG-1/2/4, MJPEG, AV1, AVS/AVS2/AVS3, VC-1, H.266 as compressed formats, and NV12, NV21, NV12M, NV21M, YUV420, YUV420M as raw formats (format tables at lines 139-356). It handles `VB2_MEMORY_DMABUF` buffers (line 938). The 4.9 tree has the same driver with HEVC in its format table.
- License: file headers say GNU GPL v2 or later (`vdec.c`); `MODULE_LICENSE("GPL v2")` in the V4L2 driver. There is no top-level LICENSE in the repo. Firmware blobs ship separately and are not covered by this.
- Multi-stream: **Yes at the kernel level.** The V4L2 node gives each `open()` its own context (`ctx->id = dev->id_counter++`). The vendor V4L2 path calls the same `vdec_create` core, so the same ID pool should apply **[I]**.
- The CoreELEC limit patch [V]: `projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml/patches/003-multi-decoders-limit-maximum-number-of-decoder.patch` exists on the `coreelec-19`, `coreelec-20`, and `coreelec-21` branches of CoreELEC/CoreELEC. It changes `#define MAX_INSTANCE_MUN 9` to `2` and removes the duplicate define in `vdec_profile.c`. Its commit message (Portisch, 2019-12-27) says Kodi can only handle one decoder and that the limit "needs to be reverted" if Kodi gains multi-decoder support. The patch is **absent** from the `coreelec-22` package directory (only `package.mk`, `scripts/`, `system.d/`).
- Applicability: **High** as the ground truth for what a multi-stream backend can rely on. It is kernel code, so it is a behaviour reference, not something to link or copy. The practical consequences: the safe stream budget on CoreELEC 21 may be 2, and on CoreELEC 22 may be 9, **[I]** until measured on a box.
- Secondary evidence [S]: a Khadas forum log shows `h264_v4l2m2m` using `/dev/video26`, driver `aml-vcodec-dec`, card `platform:amlogic`, requesting `output=H264 capture=NM21` (VIM4, Fenix/Ubuntu image, so another userspace). A CoreELEC forum thread shows the multi-instance dmesg lines `vdec_create instance ..., total 1` and `vdec_init ... VDEC_TYPE_FRAME_BLOCK`, which are useful strings to grep when counting live instances.

### 3. CoreELEC packaging (what is actually on the box)

- Repo: <https://github.com/CoreELEC/CoreELEC>. Default branch `coreelec-22` read at `98a729b`; `coreelec-21` read through the contents API.
- Key files [V]:
  - `projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml/package.mk` and `scripts/media_modules-aml.sh`: builds the `amvdec_*` modules (`CONFIG_AMLOGIC_MEDIA_VDEC_H264_MULTI=m`, `..._H265`, `..._VP9`, `..._AV1`, and more) and a boot script that runs `modprobe amvdec_ports`, `amvdec_mh264`, `amvdec_h265`, `amvdec_vp9`, and the rest. So the V4L2 decoder module is loaded on both lines; `modules-load.d/media_modules.conf` on `coreelec-21` lists `amvdec_ports` as well.
  - `projects/Amlogic-ce/packages/multimedia/libamcodec/package.mk`: `PKG_LICENSE="proprietary"`, binary tarball `libamcodec-aarch64-<sha>.tar.xz` from `sources.coreelec.org`.
  - `devices/Amlogic-no/linux/linux.aarch64.conf`: `CONFIG_AMLOGIC_MEDIA_MULTI_DEC=y` (the literal "multi decoder" switch the README calls `multi_vdec`), `# CONFIG_AMLOGIC_V4L2 is not set`, `CONFIG_V4L2_MEM2MEM_DEV=y`.
  - `projects/Amlogic-ce/packages/linux/package.mk`: kernel `CoreELEC/linux-amlogic` branch `5.15.196_20260306`, plus `common_drivers`.
  - `config/graphic` and `projects/Amlogic-ce/devices/Amlogic-no/options` (`GRAPHIC_DRIVERS=""`), and the same on `coreelec-21` (`devices/Amlogic-ng/options`): `V4L2_SUPPORT` stays `"no"`. `packages/multimedia/ffmpeg/package.mk` then sets `PKG_FFMPEG_V4L2="--disable-v4l2_m2m --disable-libudev --disable-v4l2-request"`, and the `Generic|Amlogic-ce` case adds no hwaccel patch directory.
- License: GPL-2.0-or-later for the build scripts. Patches carry the upstream project's license.
- Applicability: **High**. It settles three design inputs: libamcodec exists on the box but is binary-only; the vendor V4L2 decoder module is loaded; and CoreELEC's FFmpeg has no V4L2 decoders and no DRM PRIME patches. That last point affects the existing device-AppImage build, which links CoreELEC's `libavcodec`.
- The Amlogic project (non-`-ce`) in the same tree builds `chewitt/ffmpeg` at `c04518bc5d82315e57b14a7c915581a0667d1b4c` (branch label `amlogic-9.0.1`) with `--enable-v4l2-request`; that is the mainline stack in example 4, not the one on CoreELEC.

### 4. LibreELEC mainline Amlogic (new stateless `amvdec`)

- Repo: <https://github.com/LibreELEC/LibreELEC.tv> at `f59641e` (2026-11-01).
- Key files [V]: `projects/Amlogic/patches/linux/amlogic-0117-staging-media-meson-vdec-remove-the-driver.patch` (removes the staging driver), `amlogic-0118` (AM21B/AM21C compressed formats), `amlogic-0119` (decoder core), `0120` (MPEG-2), `0121` (H.264), `0122` (HEVC), `0123` (VP9), `0127`, `0209` (S4), `0161/0162` (deinterlacer); `projects/Amlogic/linux/linux.aarch64.conf` (`CONFIG_VIDEO_MESON_AMVDEC=m`); `projects/Amlogic/patches/kodi/kodi-0007..0010` (DRM PRIME hardware deinterlace in Kodi); `packages/multimedia/ffmpeg/package.mk` (Amlogic: `chewitt/ffmpeg`, v4l2-request).
- License: packaging GPL-2.0-or-later; the driver files are `SPDX GPL-2.0+` / `GPL-2.0`; FFmpeg parts LGPL-2.1+.
- What it is: a stateless V4L2 request-API driver (`media: amlogic: add the video decoder core`) that replaces staging `meson/vdec` on GXBB, GXL, GXLX, GXM, G12A, G12B and SM1. G12B reuses the G12A compatible. HEVC Main/Main10 at 3840x2160 on every SoC, VP9 on G12A/G12B/SM1, H.264 3840x2160 on G12/SM1, MPEG-2 to 1920x1088. CAPTURE is NV12M with optional AM21C compressed output. Firmware `*_multi.bin` images are noted as "not yet in linux-firmware".
- Provenance caution: these are `LOCAL`-carried patches dated 2026-09-26 with `Assisted-by` AI trailers and no upstream review trail visible in the tree. Treat as unreviewed.
- Multi-stream: **No.** `vdec_start_streaming()` claims the shared decoder with `core->cur_sess` under `claim_lock` and returns `-EBUSY` for a second session [V]. The mem2mem scheduler is there, but the claim happens at `STREAMON`.
- Applicability: **Medium**. It is the most capable open decode path for Amlogic (HEVC included) and the model for shape C, but only for a mainline-kernel distribution, not the CoreELEC kernels the README targets, and it cannot serve several concurrent streams.

### 5. Mainline staging `meson-vdec`

- Repo: <https://github.com/torvalds/linux/tree/master/drivers/staging/media/meson/vdec> (master as fetched 2026-11-01; LibreELEC master removes it, see example 4).
- Key files [V]: `vdec.c`, `vdec_platform.c`, `codec_h264.c`, `codec_vp9.c`, `codec_mpeg12.c`, `esparser.c`, `vdec_1.c`, `vdec_hevc.c`. License `GPL-2.0+` (BayLibre, Maxime Jourdan).
- Behaviour [V]:
  - Stateful V4L2 mem2mem. The TODO file says it stays in staging until the stateful decoder spec and compliance tests settle.
  - S922X (G12B) uses the `amlogic,g12a-vdec` compatible (`meson-g12-common.dtsi` node `vdec: video-decoder@ff620000`). The G12A format table has H.264 and VP9 up to 3840x2160 and MPEG-1/2 up to 1920x1080. There is **no HEVC** entry in `vdec_platform.c`.
  - Capture format NV12M (and YUV420M for MPEG).
  - Single session: `vdec_start_streaming()` returns `-EBUSY` when `core->cur_sess && core->cur_sess != sess`.
- Multi-stream: **No.**
- Applicability: **Low-Medium**: a readable model of what a stateful V4L2 decoder needs from userspace (buffer counts, resolution-change events). A community report on a mainline-ish 6.12 kernel [S] shows H.264 decode moving from about 137 percent CPU to about 6 percent with the drmprime patch (example 15).

### 6. FFmpeg upstream `v4l2_m2m`

- Repo: <https://github.com/FFmpeg/FFmpeg/tree/master/libavcodec>.
- Key files [V]: `v4l2_m2m_dec.c` (decoders `h264_v4l2m2m`, `hevc_v4l2m2m`, `mpeg1`, `mpeg2`, `mpeg4`, `h263`, `vc1`, `vp8`, `vp9`; H.264 and HEVC insert the `*_mp4toannexb` bitstream filter), `v4l2_context.c`, `v4l2_buffers.c`, `v4l2_fmt.c`, `v4l2_m2m.c`.
- License: LGPL-2.1+ (file headers; `LICENSE.md`). `--enable-gpl` switches the whole build to GPL v2+.
- Behaviour [V]: buffers are `V4L2_MEMORY_MMAP` only, and `AV_PIX_FMT_DRM_PRIME` does not appear in any `libavcodec/v4l2_*` file at master. Output is therefore a CPU copy. This matches the mpv wiki: "upstream ffmpeg ... only supports v4l2m2m with decoded video frames being copied to system memory first" [S].
- Multi-stream: each decoder instance creates its own `V4L2m2mContext` and opens a device, so the concurrency limit is the driver's. Vendor `/dev/video26`: several contexts; mainline: one.
- mpv exposes these as `--hwdec=v4l2m2m-copy`: `vd_lavc.c` lines ~361-435 iterate libavcodec codecs, treat a wrapper decoder with an `INTERNAL` hw config as a hwdec method named after the wrapper, and append `-copy` when the output is not a hardware format [V]. An Armbian user reports it working on Ubuntu 24.04 and on mpv 0.37 [S].
- Applicability: **Medium**. For this repo it is the smallest hardware path: pick `h264_v4l2m2m` instead of `h264` in `FfmpegDecoder`-style code and read NV12/NV21 frames back. It needs: an NV12 or NV21 `FramePixelFormat` (or a `swscale` step), a `StreamPlayer` change for `isHardware()`, and an FFmpeg built with `--enable-v4l2_m2m`, which CoreELEC's is not (example 3). A real advantage: demux, HLS, and `abort()` handling in `FfmpegDecoder` carry over unchanged.

### 7. DRM PRIME / request-API FFmpeg patches (`v4l2-drmprime`, `v4l2-request`)

- Sources [V]:
  - `packages/multimedia/ffmpeg/patches/v4l2-drmprime/0001-v4l2-drmprime.patch` in [CoreELEC/CoreELEC](https://github.com/CoreELEC/CoreELEC) (also in LibreELEC.tv). It is Lukas Rusak's "libavcodec: v4l2m2m: output AVDRMFrameDescriptor" (2018). It exports each V4L2 buffer and wraps it in an `AVDRMFrameDescriptor`, "like it is done in rkmpp". The commit message lists Amlogic S905, Raspberry Pi, i.MX6, and Dragonboard 410c as users, and says it was developed with Kodi. Touches `libavcodec/v4l2_buffers.c`, `v4l2_context.c`, `v4l2_m2m.h`, `v4l2_m2m_dec.c`, `configure`.
  - `.../patches/v4l2-request/0001-v4l2-request.patch`: stateless V4L2 request hwaccel (`libavutil/hwcontext_v4l2request.c`, `libavcodec/v4l2_request*.c` for MPEG-2, H.264, HEVC, VP8, later VP9 and AV1).
  - [chewitt/ffmpeg](https://github.com/chewitt/ffmpeg) at `c04518b` (26 commits ahead of `n9.0.1` with these files), branches `kodi-amlogic` and `master-drmprime`. This is what LibreELEC's Amlogic project builds.
  - Others named by the mpv wiki [S]: [jc-kynesim/rpi-ffmpeg](https://github.com/jc-kynesim/rpi-ffmpeg) (v4l2m2m) and [jernejsk/FFmpeg](https://github.com/jernejsk/FFmpeg) (v4l2-request); [Kwiboo/FFmpeg](https://github.com/Kwiboo/FFmpeg) also carries v4l2-request.
- License: FFmpeg's LGPL-2.1+; the forks' GitHub license field reads NOASSERTION because FFmpeg ships several COPYING files.
- Multi-stream: per instance, so limited by the driver (mainline: 1).
- Applicability: **Medium**. This is how stateful and stateless V4L2 decoders reach zero-copy rendering. Useful if the project ever bundles its own FFmpeg or moves to a mainline kernel. Not usable with CoreELEC's stock Amlogic-ce FFmpeg.

### 8. `BayLibre/ffmpeg-drm`

- Repo: <https://github.com/BayLibre/ffmpeg-drm> at `633b046` (2018-06-18, 11 stars). Files: `main.c` (664 lines), `build.sh`, `run.sh`.
- License: the `main.c` header grants Apache-2.0. The repo has no LICENSE file.
- What it shows [V]: open `h264_v4l2m2m`, set `c->pix_fmt = AV_PIX_FMT_DRM_PRIME` before `avcodec_open2`, `avcodec_send_packet`/`avcodec_receive_frame`, read `AVDRMFrameDescriptor` from `frame->data[0]`, then `drmPrimeFDToHandle` (line 117), `drmModeAddFB2` (line 134), and `drmModeSetPlane` (line 108) to scan out. It uses the legacy `drmModeSetPlane`, not atomic commits. It was written against Linaro's DragonBoard 820c (Venus decoder) with the `lrusak/v4l2-drmprime` FFmpeg branch, not Amlogic hardware.
- Multi-stream: No, one decoder and one plane.
- Applicability: **Medium-High** for `DrmRenderer`. At 664 lines it is the shortest complete example of "decoded dmabuf to KMS plane". The atomic variant is in mpv (example 9) and Kodi (example 10).

### 9. mpv DRM PRIME path

- Repo: <https://github.com/mpv-player/mpv> read at `914a11c` (2026-11-01).
- Key files [V]:
  - `video/out/hwdec/hwdec_drmprime_overlay.c`: atomic commit of a DRM PRIME frame to a video plane. Sets `FB_ID`, `CRTC_ID`, `SRC_X/Y/W/H`, `CRTC_X/Y/W/H` on `ctx->drmprime_video_plane` (lines ~190-200) and handles disabling the plane.
  - `video/out/drm_prime.c`: `drm_prime_create_framebuffer` (`drmPrimeFDToHandle`, then `drmModeAddFB2WithModifiers`, fallback `drmModeAddFB2`) with handle reference counting for frames that share a buffer.
  - `video/out/hwdec/hwdec_drmprime.c`: GL/Vulkan import of DRM PRIME frames (the path for compositing several streams on the GPU).
  - `video/out/drm_atomic.c`, `drm_common.c`: DRM plane and property discovery.
  - `video/decode/vd_lavc.c`: how wrapper decoders become hwdec methods (example 6).
- Commit `25fa1b0` ("hwdec/drmprime: add drmprime hwdec-interop", PR [#10496](https://github.com/mpv-player/mpv/pull/10496)) says it was tested on RockPro64 and that Amlogic m2m coverage came from others. In that PR a contributor (sfan5) reports an earlier version working on an S905X3 board with the rpi-ffmpeg fork [S].
- Options relevant to Amlogic [S]: `--hwdec=drm`, `--hwdec=drm-copy`, `--drm-draw-plane=overlay --drm-drmprime-video-plane=primary`.
- License: `Copyright` file: GPL-2.0+ by default, LGPL-2.1+ with `-Dgpl=false`. `drm_prime.c`, `drm_atomic.c`, and `hwdec_drmprime.c` have LGPL-2.1+ headers [V]. LGPL is the friendliest license in this list for borrowing structure, though copying files still carries LGPL obligations.
- Multi-stream: No (one player). The overlay path assumes one video plane.
- Applicability: **High** as the template for a DRM PRIME `Renderer`, and the multi-plane problem it avoids: mainline `meson-drm` exposes **one** overlay plane (`meson_overlay_create` makes a single `meson_overlay_plane`, [V] `drivers/gpu/drm/meson/meson_overlay.c`). A 9-tile wall in shape C would need GPU composition (EGL import, as mpv's `hwdec_drmprime.c` and Kodi's `RendererDRMPRIMEGLES` do) or CPU copies. How many planes the CoreELEC vendor DRM driver exposes was not checked.

### 10. Kodi DRM PRIME path

- Repo: <https://github.com/xbmc/xbmc>. The files exist upstream (`DVDVideoCodecDRMPRIME.cpp` blob `809f3d59092c`, `VideoLayerBridgeDRMPRIME.cpp` blob `54509e2ee584`); the lines below were read in the CoreELEC fork at `d49aa4a`.
- Key files [V] (under `xbmc/cores/VideoPlayer/`): `DVDCodecs/Video/DVDVideoCodecDRMPRIME.cpp`, `VideoRenderers/HwDecRender/VideoLayerBridgeDRMPRIME.cpp`, `RendererDRMPRIME.cpp`, `RendererDRMPRIMEGLES.cpp`, `DRMPRIMEEGL.cpp`.
- What it shows [V]:
  - Decoder discovery: iterate `av_codec_iterate`, keep decoders with `avcodec_get_hw_config` of format `AV_PIX_FMT_DRM_PRIME` and method `HW_DEVICE_CTX` or `INTERNAL` (`FindHWConfig`, `FindDecoder`, lines ~153-200). This is how `h264_v4l2m2m` with the drmprime patch gets picked.
  - `GetFormat` accepts DRM PRIME or planar YUV (lines ~203-225).
  - A gotcha worth noting: "ffmpeg's v4l2m2m resizes without re-invoking our GetFormat callback; republish from the frames" (line ~658).
  - Plane programming through atomic properties `FB_ID`, `CRTC_ID`, `SRC_*`, `CRTC_*`, plus `COLOR_ENCODING` and `COLOR_RANGE` (`VideoLayerBridgeDRMPRIME.cpp` lines ~39-231). One video plane.
- License: GPL-2.0-or-later (SPDX in file headers).
- Multi-stream: No.
- Applicability: **High** as a design reference (not copyable under a non-GPL license). Its decoder-discovery loop is the thing a hardware `DecoderBackend` for shape C would replicate.

### 11. jellyfin-ffmpeg

- Repo: <https://github.com/jellyfin/jellyfin-ffmpeg> branch `jellyfin` at `c6869cd`.
- Findings [V]: the tree carries the upstream `libavcodec/v4l2_*.c` files and `libavcodec/rkmppdec.c` / `rkmppenc.c`; `debian/rules` enables `--enable-libdrm` and `--enable-rkmpp`; there is no Amlogic or meson code anywhere in the tree (searched file names across all 10,701 paths). Jellyfin's hardware acceleration docs list Intel, NVIDIA, AMD, VAAPI, VideoToolbox, RKMPP, and Raspberry Pi V4L2 only [S].
- A Jellyfin-on-A311D issue ([conversun/fnos-apps#167](https://github.com/conversun/fnos-apps/issues/167)) was closed as an upstream limitation because the official `jellyfin-ffmpeg` has no Amlogic decode support and the packaged service does not expose the decoder node [S].
- License: LGPL-2.1+ in `rkmppdec.c` (header read); the project license follows FFmpeg.
- `rkmppdec.c` is a useful pattern: a full hardware decoder written as a libavcodec codec that outputs `AV_PIX_FMT_DRM_PRIME` frames (line 162), allocates an `AV_HWDEVICE_TYPE_DRM` device (line 247), fills `AVDRMFrameDescriptor` (line 305) and a hwframes context (lines 358-457), and sets `AV_CODEC_CAP_HARDWARE`, `wrapper_name = "rkmpp"` (lines 572-575).
- Multi-stream: not applicable.
- Applicability: **Low** as Amlogic source, **Medium** if the project ever wraps libamcodec as an FFmpeg codec (a much larger change than a `DecoderBackend`).

### 12. moonlight-embedded Amlogic sink

- Repo: <https://github.com/moonlight-stream/moonlight-embedded> at `f32e415` (2026-06-05). File: `src/video/aml.c` (219 lines).
- License: GPL-3.0.
- What it shows [V]: the minimal libamcodec ES loop. `codec_para_t` setup for H.264, HEVC, and AV1 (`CODEC_TAG_AV1`), `SYNC_OUTSIDE`, `codec_init`, `codec_set_freerun_mode`, `codec_checkin_pts`, and a `codec_write` loop that retries on `EAGAIN` up to `MAX_WRITE_ATTEMPTS` then requests an IDR. `dec_mode = STREAM_TYPE_FRAME` is set under `#ifdef STREAM_TYPE_FRAME`. If the VFM map contains `amlvideo`, it spawns a thread that does `VIDIOC_DQBUF`/`VIDIOC_QBUF` on `/dev/video10` to keep the pipeline flowing, so a "decode without Kodi" program has to service that node. Display is the kernel video layer; the program draws nothing.
- Multi-stream: No (global `codecParam`).
- Applicability: **Medium**. Shortest readable shape-A example. GPL-3.0 means reference only.

### 13. Yasem (CoreELEC fork)

- Repo: <https://github.com/AytosnetTV/yasem> at `5782ee2` (2026-10-21), a fork of [mvasilchuk/yasem](https://github.com/mvasilchuk/yasem) (GPL-2.0). The fork has no LICENSE file; its README says to respect the original license. File: `src/player.cpp` (1280 lines).
- What it shows [V]: a standalone, non-Kodi process on CoreELEC that demuxes with FFmpeg and feeds libamcodec: format mapping (`VFORMAT_H264`, `HEVC`, `VP9`, `MPEG12`, ...), `dec_mode = STREAM_TYPE_SINGLE` (line 796), `/dev/video10`, VFM rewrite (`rm default` / `add default decoder ppmgr amlvideo [deinterlace] amvideo`), `disable_video`, `freerun_mode`, `blackout_policy`, `/sys/class/video/axis` plus `codec_utils_set_video_position`, and framebuffer layering through `fb0/blank`, `video_zorder`, `fb0/order`. README says the reference build is 32-bit ARM HF on CoreELEC 19.x.
- Multi-stream: No; single-instance mode and one video layer.
- Applicability: **Medium** as the closest analogue to this repo's "stop Kodi, run our binary" model: it documents what a non-Kodi process has to set up. It is recent, unreviewed hobby code with no declared license.

### 14. libamcodec and LibPlayer sources

- Repos: [osmc/libamcodec](https://github.com/osmc/libamcodec) (`4eebd67`, 2017), [Stane1983/amlogic-libplayer](https://github.com/Stane1983/amlogic-libplayer) (`ecad10e`, 2014), [hardkernel/buildroot_platform_packages_amlogic_LibPlayer](https://github.com/hardkernel/buildroot_platform_packages_amlogic_LibPlayer) (`a242536`, 2018), [wegel/amlogic-libs](https://github.com/wegel/amlogic-libs) (`e223fd0`, 2020).
- Key files [V]: `amcodec/include/codec.h`, `codec_type.h`, `amcodec/codec/`, `examples/esplayer.c`, `kplayer.c`, `tsplayer.c`, `psplayer.c`, `simple_player.c`, `TsPlayer/`.
- License [V]: osmc and Stane1983 have no LICENSE file; `codec.h` says "Copyright (C) 2007-2011, Amlogic Inc. All right reserved". The Hardkernel repo's `LICENSE` is Amlogic's confidential Software License Agreement text, which prohibits redistribution and modification without written permission. These are old snapshots: `dec_mode` and `STREAM_TYPE_FRAME`, which Kodi uses to select the multi-instance mode, do not appear in the osmc headers.
- Multi-stream: a 2021 Khadas forum report (VIM3L, S905D3) says the LibPlayer examples (`kplayer`, `ionplayer`, `esplayer`) fail on the second stream even when pointed at `/dev/amstream_vbuf_sched`, and that Kodi does not play two streams either [S]. One report; the datasheet claims 4x1080p60.
- Applicability: **Reference only.** Needed only to understand `codec.h` semantics. Building against libamcodec needs `amcodec/codec.h`, whose only public copies have unclear or restrictive terms. Resolve that before any code depends on it (see licensing notes).

### 15. Small community repos

| Repo | What it is | License | Applicability |
| --- | --- | --- | --- |
| [dogukangecko/kodi-hwdec-amlogic-armbian](https://github.com/dogukangecko/kodi-hwdec-amlogic-armbian) (`b5a7966`) | Dockerfiles that rebuild FFmpeg 4.4.2 with LibreELEC's `v4l2-drmprime` patch and a GLES Kodi 19.4 for S905X (G12A per its README) on Armbian. Reports `meson-vdec` H.264 at about 6 percent CPU versus about 137 percent software; HEVC falls back to software [S]. | none | Low: confirms the shape-C recipe on mainline, which is not CoreELEC |
| [kotmartovskiy/x96max-media-node](https://github.com/kotmartovskiy/x96max-media-node) (`ef5bd16`) | Personal notes and mpv config for an S905X2 box on a custom 6.18 kernel. Records HEVC 1080p and VP9 hardware decode working, H.264 hardware decode livelocking, 4K HEVC exhausting 256 MB CMA, and an FFmpeg fix for an mpv crash (commit `4fa1d3e`, "suppress POLLERR and fix crash when buffers are uninitialized") [S]. Docs are in Russian. | none | Low: a list of failure modes to expect from V4L2 decoders |
| [jefflessard/meson-vdec](https://github.com/jefflessard/meson-vdec) and [meson-vdec-extra](https://github.com/jefflessard/meson-vdec-extra) | Work-in-progress out-of-tree kernel driver rewrite (stateless, per its README, with most phase checkboxes unchecked) and a collection of Amlogic vendor firmware blobs. | no top-level LICENSE; per-file SPDX `GPL-2.0` / `GPL-2.0+`; firmware blobs undeclared | Low: unfinished, and the firmware's redistribution terms are unclear |
| [HankShao/amlogic_ffmpeg_egl](https://github.com/HankShao/amlogic_ffmpeg_egl) | FFmpeg software decode plus EGL/GLES output sample bundled with Amlogic IPC SDK headers (`aml_ipc_*`), which look aimed at Amlogic camera SoCs. Its README says software decode. | none | Not applicable |

## Multi-stream constraints, in one place

| Layer | Limit | Evidence |
| --- | --- | --- |
| Vendor kernel decoder core, CoreELEC 22 source | 9 instances (`MAX_INSTANCE_MUN`, `ida_simple_get(0, 9)`) | [V] `vdec.h:70`, `vdec.c:1809` at `c15aebb` |
| Vendor kernel decoder core, CoreELEC 20/21 package | Patched to 2 (message says 1) | [V] `003-multi-decoders-limit-maximum-number-of-decoder.patch` on `coreelec-20`, `coreelec-21` |
| Mainline staging `meson-vdec` | 1 streaming session | [V] `vdec_start_streaming()` returns `-EBUSY` |
| New LibreELEC `amvdec` (stateless) | 1 streaming session | [V] same claim logic in `amlogic-0119` |
| `meson-drm` video planes | 1 overlay plane on mainline | [V] `meson_overlay_create` creates one plane. Vendor CoreELEC DRM not checked |
| Kodi | 1 video, 1 codec, 1 video layer | [S] CoreELEC developer; [V] one `CAMLCodec` per player |
| libamcodec examples | Second stream fails (one report) | [S] Khadas forum |
| CoreELEC's FFmpeg | No `v4l2m2m` decoders | [V] `config/graphic` and `ffmpeg/package.mk` on `coreelec-21` and `coreelec-22` |

What the table says about each backend shape:

- **A (libamcodec, self-presenting):** decode capacity exists in the kernel (2 or 9 instances), but turning several decoders into a tiled picture needs more than one video layer or a path that returns frames. Nobody in this research demonstrated either on Amlogic. Highest risk, highest payoff.
- **B (vendor V4L2, CPU copy):** the only shape where both decode capacity and tiling are plausible with the existing `SoftwareRenderer`. CPU copies at S922X memory bandwidth are an unmeasured cost. The blocker is FFmpeg: CoreELEC's has no `v4l2m2m`, so this needs the project to ship its own FFmpeg or talk to `/dev/video26` directly. The vendor V4L2 node's behaviour with stock FFmpeg on CoreELEC is unverified.
- **C (DRM PRIME, mainline-style):** works on mainline for one stream. A wall needs GPU composition and a driver without the single-session claim. Not a fit for the stated multi-stream goal today.

## Suggested next steps (spikes, not implementation)

Ordered by information per hour on a GS-King X:

1. **Measure the instance ceiling** on both CoreELEC lines. Without any app code: start N copies of a known H.264 clip through whatever decoder path is reachable (Kodi cannot; see the checklist for `ffmpeg` and `v4l2-ctl`), and watch `dmesg` for `vdec_create instance ..., total N` and failures. This decides whether the CoreELEC 21 cap of 2 is real for the V4L2 node and whether CoreELEC 22 reaches 9.
2. **Prove the vendor V4L2 node is usable** from a small standalone program or a custom FFmpeg: open `/dev/video26`, set OUTPUT H264, request CAPTURE NV21/NV12, decode a clip, export or copy a frame. Record which formats and buffer counts it accepts, and whether CAPTURE supports `VIDIOC_EXPBUF`.
3. **Settle the FFmpeg question.** CoreELEC's linked `libavcodec` lacks `v4l2_m2m`. Options to evaluate: bundle a project-built FFmpeg in the device AppImage (changes `scripts/coreelec-targets.sh`'s "link CoreELEC's own FFmpeg" rule), or implement the small V4L2 mem2mem client directly and keep FFmpeg for demux only.
4. **Prototype shape A single-stream** on the box with libamcodec using moonlight-embedded's `aml.c` as the call-order guide, to see what a non-Kodi process has to set up (VFM map, `video_zorder`, framebuffer blanking) and whether two instances can show in two positions.
5. **Decide the interface changes** before writing a backend: an NV12/NV21 `FramePixelFormat` for shape B, a way for a backend to say "I present myself" for shape A, and the `StreamPlayer` hardware gate. The license decision (below) also gates whether Kodi, mpv, or moonlight-embedded code may be adapted or only read.

## Device verification checklist (not run)

Run on the box over SSH. None of this has been executed; outputs would turn the **[I]** claims into facts.

```bash
cat /etc/os-release | grep -E 'ARCH|VERSION'
uname -r
lsmod | grep -E 'amvdec|vdec'
ls -l /dev/video* /dev/amstream* 2>/dev/null
v4l2-ctl -d /dev/video26 --info --list-formats-out --list-formats   # if v4l2-ctl exists
cat /sys/class/vfm/map
ls /usr/lib/libamcodec* /usr/lib/libav*  # libamcodec present? which libavcodec soname?
strings /usr/lib/libavcodec.so* | grep -c v4l2m2m   # expect 0 if built with --disable-v4l2_m2m
dmesg | grep -E 'vdec_create|vdec_release|VDEC_TYPE'   # instance counts while testing
```

## Licensing notes

- This repo has no `LICENSE` file at the root (checked). Choose one before adapting any third-party code.
- **Safe to borrow structure from, with LGPL obligations if files are copied:** FFmpeg (LGPL-2.1+), mpv `drm_prime.c`, `drm_atomic.c`, `hwdec_drmprime.c` (LGPL-2.1+ headers; the mpv binary is GPL-2.0+ unless built with `-Dgpl=false`), `BayLibre/ffmpeg-drm` (Apache-2.0 header).
- **GPL, reference only unless the project is GPL-compatible:** Kodi and the CoreELEC fork (GPL-2.0-or-later), moonlight-embedded (GPL-3.0, which is incompatible with GPL-2.0-only), the kernel drivers (GPL-2.0).
- **libamcodec** is `proprietary`, binary-only in CoreELEC (`PKG_LICENSE="proprietary"`, tarball from `sources.coreelec.org`). Kodi sidesteps linking by `dlopen`, but still compiles against `amcodec/codec.h`. The public header copies are either unlicensed (osmc, Stane1983) or under Amlogic's confidential SLA (Hardkernel). A dependency on libamcodec needs a clear answer to "where do the headers come from and may we redistribute them".
- **Firmware** (`meson/vdec/*.bin`, `video_ucode.bin`) is separate from the driver licenses. `*_multi.bin` images are not in linux-firmware per the LibreELEC patch message [V].

## Method and sources

- Cloned (shallow, sparse) and read: CoreELEC/CoreELEC (`coreelec-22`), CoreELEC/xbmc (`aml-5.15.196-22.0`), CoreELEC/media_modules-aml (`5.15.196_20260225`), CoreELEC/common_drivers, LibreELEC/LibreELEC.tv (`master`), mpv-player/mpv (`master`), moonlight-stream/moonlight-embedded, BayLibre/ffmpeg-drm, osmc/libamcodec, Stane1983/amlogic-libplayer, hardkernel LibPlayer, wegel/amlogic-libs, AytosnetTV/yasem, jefflessard/meson-vdec and -extra, dogukangecko, kotmartovskiy, HankShao.
- Read through the GitHub API or raw files: torvalds/linux (`drivers/staging/media/meson/vdec`, `drivers/gpu/drm/meson/meson_overlay.c`, `arch/arm64/boot/dts/amlogic/meson-g12-common.dtsi`), FFmpeg/FFmpeg `libavcodec/v4l2_*`, jellyfin/jellyfin-ffmpeg (tree listing, `rkmppdec.c`, `debian/rules`), CoreELEC branches `coreelec-19/20/21` (limit patch, ffmpeg package, graphics config), `CoreELEC/media_modules-aml` branch `amlogic-4.9.269`.
- Web search for forum threads and wiki pages ([S]): CoreELEC forum "Experiments with hw decoding", Khadas "VIM3L: multi-stream hardware video decode... how?", Khadas VIM4 FFmpeg thread, mpv wiki "V4L2 drmprime support", mpv PR #10496, Jellyfin hardware acceleration docs, Armbian S905X thread.
- Dates in this environment are 2026; commit hashes above are what was fetched on 2026-11-01. The LibreELEC `amvdec` patch series and several small repos are recent and unreviewed, so re-check them before relying on details.
