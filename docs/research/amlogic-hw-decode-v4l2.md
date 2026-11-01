# Amlogic S922X (G12B) hardware video decode from Linux userspace on CoreELEC

Research notes for a future hardware `DecoderBackend` on the Beelink GS-King X (S922X, `g12b_s922x_beelink_gs_king_x`). Nothing here has been run on a box. Every claim is tied to a source file, a shipped CoreELEC image, or a datasheet. Items that still need a GS-King X are listed in [What still needs a GS-King X](#what-still-needs-a-gs-king-x).

This document does not implement a decoder.

## Summary

- **CoreELEC 21 (Amlogic-ng, kernel 4.9.269)** ships Amlogic's vendor V4L2 memory-to-memory stateful decoder (`amvdec_ports.ko`, driver name `aml-vcodec-dec`). It registers at **`/dev/video26`**. CoreELEC patches the vendor decoder core so that **at most 2 decoder instances** exist at once, system-wide. That limit is shared with Kodi's own decode path.
- **CoreELEC 22 (Amlogic-no, kernel 5.15.196)** does **not** ship that V4L2 decoder. The `22.0-Piers_rc1` image has no `amvdec_ports.ko` and no `*_v4l.ko` decoders, because CoreELEC's build never sets `CONFIG_AMLOGIC_MEDIA_V4L_DEC`. The vendor instance limit is unpatched there (9).
- Both lines ship **FFmpeg built with `--disable-v4l2_m2m`**. `h264_v4l2m2m` and `hevc_v4l2m2m` do not exist in the libavcodec this repo links on the box. v4l2-request is also disabled.
- **v4l2-request (stateless) does not apply to Amlogic.** There is no stateless Amlogic decoder in mainline or in the vendor trees. Mainline has only the stateful staging `meson-vdec` driver. CoreELEC does not use it, and it allows one streaming session per SoC.
- Kodi on CoreELEC decodes through the legacy **amstream + VFM** path via the proprietary `libamcodec.so`. Decoded frames go straight to the hardware video layer. Kodi opens `/dev/video10` (`amlvideo`) only to dequeue frame timing, not pixels.
- **Capture formats are NV21, NV12, NV21M, and NV12M.** The vendor driver offers I420 only for MJPEG. `DecodedFrame` currently describes only `Yuv420p`, so a hardware backend needs an NV12 variant or a CPU deinterleave.
- **Multi-instance budget for a 9-tile wall:**
  - The vendor `vdec_core` instance cap is 9 stock, 2 on CE21.
  - The datasheet throughput budget is "4Kx2K@60fps + 1x1080P@60fps" in total, and H.264 is capped at 4K30.
  - The codec_mm CMA pool is 432 MiB on CE21's GS-King X device tree and 308 MiB on CE22's.
  - On the display side, the CE22 vendor KMS driver exposes only 2 NV12-capable video planes. CE21 has no KMS driver at all.

## Source snapshot

All paths below are relative to these trees. The pins match the CoreELEC build files at the time of writing (2026-11-01).

| Tree | Commit | Used by |
| --- | --- | --- |
| [CoreELEC/CoreELEC `coreelec-21`](https://github.com/CoreELEC/CoreELEC/tree/15970b8e469b8e299a8947b1751593bdaf53ed82) | `15970b8e` | CE21 build system |
| [CoreELEC/CoreELEC `coreelec-22`](https://github.com/CoreELEC/CoreELEC/tree/98a729b80ece21a7ceb8dbab127a6d19a6cff60a) | `98a729b8` | CE22 build system |
| [CoreELEC/linux-amlogic `amlogic-4.9-20`](https://github.com/CoreELEC/linux-amlogic/tree/50c8e8154c5aba3c7cd8438d04dd5fdd51733e41) | `50c8e815` | CE21 kernel (`packages/linux/package.mk`, `LINUX="amlogic-4.9"`) |
| [CoreELEC/linux-amlogic `5.15.196_20260306`](https://github.com/CoreELEC/linux-amlogic/tree/44cbccdee100b6a388814b9121b3b7629ab94382) | `44cbccde` | CE22 kernel (`projects/Amlogic-ce/packages/linux/package.mk`) |
| [CoreELEC/common_drivers](https://github.com/CoreELEC/common_drivers/tree/8ca9b6c3b7152fa2959147a403fc5e7e697d78d2) | `8ca9b6c3` | CE22 Amlogic drivers and device trees |
| [CoreELEC/media_modules-aml `amlogic-4.9.269`](https://github.com/CoreELEC/media_modules-aml/tree/60561618f2bf154c1438d3c790b625ff01f215c3) | `60561618` | CE21 decoder modules |
| [CoreELEC/media_modules-aml `5.15.196_20260225`](https://github.com/CoreELEC/media_modules-aml/tree/c15aebb1ac93028d0aceadf8f35dda2204d33424) | `c15aebb1` | CE22 decoder modules |
| [CoreELEC/xbmc `aml-5.15.196-22.0`](https://github.com/CoreELEC/xbmc/tree/d49aa4ac2362adf51da4eaa7c6ccea6e619fb860) | `d49aa4ac` | CE22 Kodi |
| [torvalds/linux](https://github.com/torvalds/linux/tree/cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a) | `cbbddb6b` | mainline `meson-vdec` |
| [FFmpeg `release/6.0`](https://github.com/FFmpeg/FFmpeg/tree/573f2b81ebd97da81def2ba762e81b0328547004) | `573f2b81` | CE21's FFmpeg 6.0.1 |

Shipped images, checked by unpacking `target/SYSTEM`. Their sha256 digests match `scripts/coreelec-targets.sh`:

- `CoreELEC-Amlogic-ng.arm-21.3-Omega.tar`
- `CoreELEC-Amlogic-no.aarch64-22.0-Piers_rc1.tar`

Base URL shorthands used below:

- `MM49` = `https://github.com/CoreELEC/media_modules-aml/blob/60561618f2bf154c1438d3c790b625ff01f215c3`
- `MM515` = `https://github.com/CoreELEC/media_modules-aml/blob/c15aebb1ac93028d0aceadf8f35dda2204d33424`
- `CE21` = `https://github.com/CoreELEC/CoreELEC/blob/15970b8e469b8e299a8947b1751593bdaf53ed82`
- `CE22` = `https://github.com/CoreELEC/CoreELEC/blob/98a729b80ece21a7ceb8dbab127a6d19a6cff60a`
- `CD515` = `https://github.com/CoreELEC/common_drivers/blob/8ca9b6c3b7152fa2959147a403fc5e7e697d78d2`
- `K49` = `https://github.com/CoreELEC/linux-amlogic/blob/50c8e8154c5aba3c7cd8438d04dd5fdd51733e41`

## What the repo's `DecoderBackend` expects

From `src/decoder_backend.h`, `src/stream_player.{h,cpp}`, and `src/ffmpeg_decoder.{h,cpp}`:

- `open(url)` covers demux and decoder setup. `close()` must be idempotent. `isHardware()` is `false` today.
- `readFrame(DecodedFrame&)` returns one CPU image. Plane pointers stay valid until the next `readFrame()` or `close()` on the same instance. `FramePixelFormat` has only `Yuv420p` (three 8-bit planes).
- `abort()` may be called from another thread. It must unblock an in-flight `open()` or `readFrame()`, and it latches.
- `StreamPlayer::sessionLoop` treats `isHardware() == true` as a failure (`WorkerFail::Hardware`) and closes the decoder (`src/stream_player.cpp` around line 189). A hardware backend has to lift that guard.
- `StreamPlayer::copyOwned` copies three planes of every frame into `OwnedFrame` before presenting. `OwnedFrame::view()` hard-codes `Yuv420p`.
- A retired worker's decoder is `close()`d only in `reapFinished()`, after its thread has returned (`src/stream_player.cpp` lines 116–157). The cap on decode threads is `LayoutEngine::kMaxStreams + kMaxBusyRetired - 1`. That is 17 in total, more than the vendor instance cap.
- `prepareAmlogicHwaccel()` in `src/ffmpeg_decoder.cpp` is a deliberate no-op. The README says a hardware path must be its own `DecoderBackend`, not grow inside `FfmpegDecoder`.

## Platform matrix

| | CE21 Amlogic-ng (`coreelec21-ng-armv7hf`) | CE22 Amlogic-no (`coreelec22-no-aarch64`) | Mainline Linux (not CoreELEC) |
| --- | --- | --- | --- |
| Kernel | vendor 4.9.269 | vendor 5.15.196 (GKI layout plus `common_drivers`) | upstream |
| Userspace | 32-bit armhf | aarch64 | — |
| V4L2 M2M decoder | **yes**: `amvdec_ports.ko`, `aml-vcodec-dec`, `/dev/video26` | **no**: not built (see below) | `meson-vdec` (staging), node number dynamic |
| Stateless / v4l2-request | no | no | no |
| amstream (`/dev/amstream_*`) | yes | yes | no |
| Vendor decoder instance cap | **2** (CoreELEC patch) | 9 (stock) | 1 streaming session |
| FFmpeg `v4l2_m2m` in the shipped libavcodec | disabled | disabled | depends on distro |
| `libamcodec.so` (proprietary) | shipped | shipped | — |
| KMS driver | **none** (`CONFIG_AMLOGIC_DRM` unset, no `DRM_MESON`) | vendor `meson` DRM (`CONFIG_AMLOGIC_DRM=y`) | `meson-drm` |
| `v4lvideo` / `video_composer` | not built | built in | — |
| codec_mm CMA (GS-King X device tree chain) | 432 MiB (`0x1b000000`) | 308 MiB (`0x13400000`) | — |

### Evidence for the CE21 row

- **Driver build.** `amvdec_ports/` is built when `CONFIG_V4L2_MEM2MEM_DEV` is set ([`MM49/drivers/Makefile` L5-L7](https://github.com/CoreELEC/media_modules-aml/blob/60561618f2bf154c1438d3c790b625ff01f215c3/drivers/Makefile#L5-L7)). The CE21 kernel config sets `CONFIG_V4L2_MEM2MEM_DEV=m` (`CE21/projects/Amlogic-ce/devices/Amlogic-ng/linux/linux.aarch64.conf` line 3579). The module is autoloaded from `CE21/projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml/modules-load.d/media_modules.conf` line 14.
- **Shipped image.** The 21.3-Omega image contains `/usr/lib/kernel-overlays/base/lib/modules/4.9.269/media_modules-aml/amvdec_ports.ko`.
- **Instance cap.** CoreELEC applies [`003-multi-decoders-limit-maximum-number-of-decoder.patch`](https://github.com/CoreELEC/CoreELEC/blob/15970b8e469b8e299a8947b1751593bdaf53ed82/projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml/patches/003-multi-decoders-limit-maximum-number-of-decoder.patch), dated 2019-12-27 by Portisch. It changes `#define MAX_INSTANCE_MUN 9` to `2` in `drivers/frame_provider/decoder/utils/vdec.h`. The commit message says this is because "Kodi can only handle one decoder", and that the change should be reverted if Kodi gains parallel decoders. The patch sits at project level, so it applies to Amlogic-ng. The same patch is also carried for Amlogic-ne.
- **No KMS.** The CE21 kernel config has `# CONFIG_AMLOGIC_DRM is not set` (line 1698) and no `CONFIG_DRM_MESON` entry. The display is fbdev (`CONFIG_AMLOGIC_MEDIA_FB=y`, line 1445) plus the amvideo video layer. `/dev/dri/card0`, which `DrmRenderer` opens, should therefore be absent on CE21. Verify on a box.

### Evidence for the CE22 row

- **No V4L2 decoder.** In the 5.15 tree, `amvdec_ports/` and `frame_provider/decoder_v4l/` (the `*_v4l.ko` decoders) are gated on `CONFIG_AMLOGIC_MEDIA_V4L_DEC`:
  - [`MM515/drivers/Makefile` L7](https://github.com/CoreELEC/media_modules-aml/blob/c15aebb1ac93028d0aceadf8f35dda2204d33424/drivers/Makefile#L7)
  - `MM515/drivers/frame_provider/Makefile` line 2

  Amlogic's own top-level `Makefile` sets that symbol ([L43](https://github.com/CoreELEC/media_modules-aml/blob/c15aebb1ac93028d0aceadf8f35dda2204d33424/Makefile#L43)). CoreELEC instead invokes `kernel_make M=.../drivers` with an explicit `CONFIG_*` list that omits it ([`CE22/projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml/package.mk`](https://github.com/CoreELEC/CoreELEC/blob/98a729b80ece21a7ceb8dbab127a6d19a6cff60a/projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml/package.mk)). The kernel tree has no Kconfig symbol by that name either.
- **Shipped image.** The 22.0-Piers_rc1 image's `media_modules-aml/` directory has `amvdec_mh264.ko`, `amvdec_h265.ko`, `amvdec_vp9.ko`, and others, but no `amvdec_ports.ko` and no `*_v4l.ko`. The startup script still runs `modprobe -q amvdec_ports` ([`scripts/media_modules-aml.sh` L8](https://github.com/CoreELEC/CoreELEC/blob/98a729b80ece21a7ceb8dbab127a6d19a6cff60a/projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml/scripts/media_modules-aml.sh#L8)). `-q` hides the failure.
- **Instance cap.** CE22 has no `patches/` directory for media_modules, so `MAX_INSTANCE_MUN` stays 9 ([`MM515/.../vdec.h` L70](https://github.com/CoreELEC/media_modules-aml/blob/c15aebb1ac93028d0aceadf8f35dda2204d33424/drivers/frame_provider/decoder/utils/vdec.h#L70)).
- **KMS.** The kernel config has `CONFIG_AMLOGIC_DRM=y` (line 6452) and `# CONFIG_DRM_MESON is not set` (line 4384).

### Evidence that FFmpeg's V4L2 decoders are off

- CoreELEC's `config/graphic` sets `V4L2_SUPPORT="no"` unless `GRAPHIC_DRIVERS` names a Mesa or V4L2 driver (`CE21/config/graphic` line 34, `CE22/config/graphic` line 33).
- Both Amlogic-ce device `options` files set `GRAPHIC_DRIVERS=""`:
  - `CE21/projects/Amlogic-ce/devices/Amlogic-ng/options` line 96
  - `CE22/projects/Amlogic-ce/devices/Amlogic-no/options` line 99
- With `V4L2_SUPPORT=no`, `packages/multimedia/ffmpeg/package.mk` passes `--disable-v4l2_m2m` (CE21 line 68, CE22 line 82).
- The configure string embedded in the shipped libraries confirms it:
  - CE21 `libavcodec.so.60.3.100` contains `--disable-v4l2_m2m --disable-libudev --disable-v4l2-request --enable-libdrm`.
  - CE22 `libavcodec.so.63.1.102` contains `--disable-v4l2_m2m ... --enable-libdrm`.

The repo's device AppImages link CoreELEC's own libavcodec (README "Device AppImages"). So `avcodec_find_decoder_by_name("h264_v4l2m2m")` returns `NULL` on both targets today.

## API 1: vendor V4L2 M2M stateful decoder (`amvdec_ports`)

This is the only V4L2 decode API that exists on a stock CoreELEC install, and only on CE21.

### Device node and capabilities

- The driver registers `aml-vcodec-dec` with `VFL_DIR_M2M` and fixed minor **26**:
  - [`MM49/drivers/amvdec_ports/aml_vcodec_dec_drv.c` L455-L498](https://github.com/CoreELEC/media_modules-aml/blob/60561618f2bf154c1438d3c790b625ff01f215c3/drivers/amvdec_ports/aml_vcodec_dec_drv.c#L455-L498), with `video_register_device(vfd_dec, VFL_TYPE_GRABBER, 26)`.
  - The name comes from `AML_VCODEC_DEC_NAME` in [`aml_vcodec_drv.h` L33](https://github.com/CoreELEC/media_modules-aml/blob/60561618f2bf154c1438d3c790b625ff01f215c3/drivers/amvdec_ports/aml_vcodec_drv.h#L33).
  - 5.15 is the same at [`MM515/.../aml_vcodec_dec_drv.c` L673-L675](https://github.com/CoreELEC/media_modules-aml/blob/c15aebb1ac93028d0aceadf8f35dda2204d33424/drivers/amvdec_ports/aml_vcodec_dec_drv.c#L673-L675).
- `device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING`.
- **Gotcha: the queue types do not match the advertised caps.** The OUTPUT (bitstream) queue is always `V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE`. The CAPTURE queue is `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE` only when the module parameter `multiplanar` is true. It defaults to false:
  - queue setup: [`MM49/.../aml_vcodec_dec.c` L2682-L2703](https://github.com/CoreELEC/media_modules-aml/blob/60561618f2bf154c1438d3c790b625ff01f215c3/drivers/amvdec_ports/aml_vcodec_dec.c#L2682-L2703)
  - parameter: [`aml_vcodec_dec_drv.c` L615-L617](https://github.com/CoreELEC/media_modules-aml/blob/60561618f2bf154c1438d3c790b625ff01f215c3/drivers/amvdec_ports/aml_vcodec_dec_drv.c#L615-L617)

  Generic clients that pick buffer types from `device_caps`, such as FFmpeg's `v4l2_m2m.c`, will use `CAPTURE_MPLANE` against a single-planar vb2 queue. Either write `1` to `/sys/module/amvdec_ports/parameters/multiplanar` (mode `0644`, root) before opening the node, or use single-planar CAPTURE ioctls. This is inferred from source; verify on a box.
- vb2 memory is `vb2_dma_contig_memops` on both queues. `io_modes` are `DMABUF | MMAP` for OUTPUT and `DMABUF | MMAP | USERPTR` for CAPTURE. `VIDIOC_EXPBUF` is wired up (`aml_vcodec_dec.c` line 1281), so CAPTURE buffers can be exported as dma-buf fds. They come from CMA, so they are physically contiguous.

### Coded formats (OUTPUT queue)

On 4.9, [`aml_video_formats[]` L63-L127](https://github.com/CoreELEC/media_modules-aml/blob/60561618f2bf154c1438d3c790b625ff01f215c3/drivers/amvdec_ports/aml_vcodec_dec.c#L63-L127) lists:

- `H264`, `HEVC`, `VP9`, `MPEG1`, `MPEG2`, `MPEG4`, `MJPEG`, `AV1`

5.15 adds `AVS`, `AVS2`, `AVS3`, `VC1_ANNEX_G`, `VC1_ANNEX_L`, and `H266`.

Each OUTPUT buffer carries one access unit in Annex-B form, with start codes. HLS MPEG-TS packets from libavformat are already Annex-B. fMP4/CMAF variants need `h264_mp4toannexb` / `hevc_mp4toannexb` first. Those bitstream filters are present in CoreELEC's libavcodec (`--enable-bsfs`).

### Raw formats (CAPTURE queue)

`NV21`, `NV21M`, `NV12`, `NV12M`, `YUV420`, and on 5.15 also `YUV420M`.

- `VIDIOC_ENUM_FMT` lists them in that order, so **NV21 is the default**. Request `NV12` or `NV12M` explicitly with `VIDIOC_S_FMT`.
- I420 is offered only for MJPEG, and only with `support_format_I420` set. See the "I420 only used for mjpeg" comment in [`MM515/.../aml_vcodec_dec.c` L3674-L3693](https://github.com/CoreELEC/media_modules-aml/blob/c15aebb1ac93028d0aceadf8f35dda2204d33424/drivers/amvdec_ports/aml_vcodec_dec.c#L3674-L3693). Do not plan on getting `Yuv420p` straight from the hardware for H.264 or HEVC.
- `VIDIOC_TRY_FMT` on CAPTURE clamps width and height to `AML_VDEC_MAX_W`/`AML_VDEC_MAX_H` = 2048×1088 ([`aml_vcodec_dec.h` L32-L33](https://github.com/CoreELEC/media_modules-aml/blob/60561618f2bf154c1438d3c790b625ff01f215c3/drivers/amvdec_ports/aml_vcodec_dec.h#L32-L33)). `VIDIOC_ENUM_FRAMESIZES` reports the same stepwise range. The real coded size arrives through `VIDIOC_G_FMT` after `V4L2_EVENT_SOURCE_CHANGE` (`vidioc_vdec_g_fmt` fills it from `ctx->picinfo` once `ctx->state >= AML_STATE_PROBE`). Always read the size back with `G_FMT`. Never trust the size you passed to `S_FMT`.
- Bytes per line equal the aligned width, and planes are canvas-aligned. 5.15 exposes the alignment through private controls `AML_V4L2_GET_WIDTH_ALIGN` and `AML_V4L2_GET_HEIGHT_ALIGN`.

### Vendor configuration

Amlogic-private controls start at `V4L2_CID_USER_BASE + 0x1100` ([`MM515/.../aml_vcodec_dec.c` L91-L109](https://github.com/CoreELEC/media_modules-aml/blob/c15aebb1ac93028d0aceadf8f35dda2204d33424/drivers/amvdec_ports/aml_vcodec_dec.c#L91-L109)). They include:

- `AML_V4L2_SET_DRMMODE` (secure path)
- `AML_V4L2_GET_INPUT_BUFFER_NUM`
- `AML_V4L2_DEC_PARMS_CONFIG`
- `AML_V4L2_GET_INST_ID`
- `AML_V4L2_SET_STREAM_MODE`

Decoder parameters such as `double_write_mode`, `init_width`, `init_height`, `ref_buf_margin`, `canvas_mem_mode`, and `low_latency_mode` go in through `VIDIOC_S_PARM` on the OUTPUT type. The driver reinterprets `parm.raw_data` as `struct aml_dec_params`:

- handler: [`vidioc_vdec_s_parm`, MM515 L5607-L5647](https://github.com/CoreELEC/media_modules-aml/blob/c15aebb1ac93028d0aceadf8f35dda2204d33424/drivers/amvdec_ports/aml_vcodec_dec.c#L5607-L5647)
- structs: `struct aml_vdec_cfg_infos` and `struct aml_dec_params` in [`aml_vcodec_drv.h` L561-L638](https://github.com/CoreELEC/media_modules-aml/blob/c15aebb1ac93028d0aceadf8f35dda2204d33424/drivers/amvdec_ports/aml_vcodec_drv.h#L561-L638)

The 4.9 tree has the same mechanism with a smaller struct. The layout differs between the trees, so a client must use the header from the tree its kernel was built from.

**Hardware downscale.** `double_write_mode` controls the linear YUV copy that HEVC, VP9, and AV1 write next to their compressed reference frames ([`MM49/.../decoder/vdec_hevc_if.c` L317-L368](https://github.com/CoreELEC/media_modules-aml/blob/60561618f2bf154c1438d3c790b625ff01f215c3/drivers/amvdec_ports/decoder/vdec_hevc_if.c#L317-L368)):

| `double_write_mode` | Linear YUV output |
| --- | --- |
| `0x1` | full size |
| `0x4` | 1/2 scale |
| `0x2` | 1/4 scale |
| `0x100`, `0x200`, `0x300` | 1/2 or 1/4, chosen automatically above 1080p or 720p |

The default the V4L2 path sets for HEVC is `0x10` (full size, 8-bit, no AFBC). This could produce tile-sized frames for a 3×3 grid without a CPU or GE2D scale. It does not apply to H.264.

### Concrete call sequence

This follows the kernel's stateful decoder spec ([docs.kernel.org: Memory-to-Memory Stateful Video Decoder Interface](https://docs.kernel.org/userspace-api/media/v4l/dev-decoder.html), source `Documentation/userspace-api/media/v4l/dev-decoder.rst`), with the vendor-specific steps marked. Open-source implementations of the same sequence:

- FFmpeg [`libavcodec/v4l2_m2m_dec.c`](https://github.com/FFmpeg/FFmpeg/blob/573f2b81ebd97da81def2ba762e81b0328547004/libavcodec/v4l2_m2m_dec.c) and [`v4l2_context.c`](https://github.com/FFmpeg/FFmpeg/blob/573f2b81ebd97da81def2ba762e81b0328547004/libavcodec/v4l2_context.c)
- GStreamer `v4l2videodec` (`subprojects/gst-plugins-good/sys/v4l2/gstv4l2videodec.c`)
- Amlogic's own test client, [`MM49/drivers/amvdec_ports/test/`](https://github.com/CoreELEC/media_modules-aml/tree/60561618f2bf154c1438d3c790b625ff01f215c3/drivers/amvdec_ports/test)

```text
# vendor: before open, if the client uses *_MPLANE capture types
echo 1 > /sys/module/amvdec_ports/parameters/multiplanar

fd = open("/dev/video26", O_RDWR | O_NONBLOCK)
VIDIOC_QUERYCAP                       -> driver "aml-vcodec-dec", M2M_MPLANE
VIDIOC_ENUM_FMT(OUTPUT_MPLANE)        -> confirm H264 / HEVC
VIDIOC_S_FMT(OUTPUT_MPLANE, H264, sizeimage = e.g. 1 MiB)
VIDIOC_S_PARM(OUTPUT_MPLANE, raw_data = struct aml_dec_params)   # vendor, optional
VIDIOC_SUBSCRIBE_EVENT(V4L2_EVENT_SOURCE_CHANGE)
VIDIOC_SUBSCRIBE_EVENT(V4L2_EVENT_EOS)
VIDIOC_REQBUFS(OUTPUT_MPLANE, MMAP, n)  + VIDIOC_QUERYBUF + mmap each
VIDIOC_STREAMON(OUTPUT_MPLANE)        # vdec_create() runs on the first queued buffer

loop:   # feed Annex-B access units; pts goes in buf.timestamp (TIMESTAMP_COPY)
  memcpy(au) ; VIDIOC_QBUF(OUTPUT)
  poll(fd, POLLIN | POLLOUT | POLLPRI, plus an abort eventfd)
  POLLPRI -> VIDIOC_DQEVENT
     SOURCE_CHANGE (first time or a resolution change):
       VIDIOC_G_FMT(CAPTURE)          -> real width/height/bytesperline
       VIDIOC_G_CTRL(V4L2_CID_MIN_BUFFERS_FOR_CAPTURE)
       VIDIOC_S_FMT(CAPTURE, NV12 or NV12M)
       VIDIOC_REQBUFS(CAPTURE, MMAP or DMABUF, min + extra)
       mmap and/or VIDIOC_EXPBUF each buffer ; VIDIOC_QBUF all
       VIDIOC_STREAMON(CAPTURE)
  POLLOUT -> VIDIOC_DQBUF(OUTPUT)     # reclaim bitstream buffers
  POLLIN  -> VIDIOC_DQBUF(CAPTURE)    # decoded picture: index, bytesused, timestamp
             ... hand out the planes ... ; VIDIOC_QBUF(CAPTURE) when released

drain:  VIDIOC_DECODER_CMD(V4L2_DEC_CMD_STOP) ; DQBUF until V4L2_BUF_FLAG_LAST / EOS
close:  STREAMOFF both ; REQBUFS(0) both ; close(fd)   # releases the vdec id
```

### Diagnostics on the box

These sysfs files are created by `vdec.c` and `codec_mm`:

- `/sys/class/vdec/vdec_status`, `/sys/class/vdec/core`, `/sys/class/vdec/dump_vdec_chunks` (attributes at [`MM49/.../vdec.c` L5633-L5665](https://github.com/CoreELEC/media_modules-aml/blob/60561618f2bf154c1438d3c790b625ff01f215c3/drivers/frame_provider/decoder/utils/vdec.c#L5633-L5665))
- `/sys/class/codec_mm/codec_mm_dump`
- `/sys/class/vfm/map`
- `/sys/module/amvdec_ports/parameters/*`

## API 2: amstream + VFM (+ `libamcodec`), as used by Kodi

This is the path CoreELEC's Kodi uses on both lines. It is the only hardware path on CE22 as shipped.

### Device nodes

`media_modules/drivers/stream_input/amports/amstream.c` registers ([`MM515/.../amstream.c` L300-L405](https://github.com/CoreELEC/media_modules-aml/blob/c15aebb1ac93028d0aceadf8f35dda2204d33424/drivers/stream_input/amports/amstream.c#L300-L405)):

- `/dev/amstream_vbuf`, `amstream_vbuf_sched`, `amstream_vframe`
- `amstream_hevc`, `amstream_hevc_frame`, `amstream_hevc_sched`
- `amstream_mpts`, `amstream_mpts_sched`
- `amstream_dves_*`
- audio, subtitle, and userdata ports

### Exclusivity

- A video port without `PORT_TYPE_DECODER_SCHED` is exclusive. `amstream_open` returns `-EBUSY` if any other non-sched video port is in use ([`MM49/.../amstream.c` L1733-L1747](https://github.com/CoreELEC/media_modules-aml/blob/60561618f2bf154c1438d3c790b625ff01f215c3/drivers/stream_input/amports/amstream.c#L1733-L1747)). `is_mult_inc()` is true only for `*_sched` and `*_frame` ports ([`MM49/.../vdec.c` L332-L341](https://github.com/CoreELEC/media_modules-aml/blob/60561618f2bf154c1438d3c790b625ff01f215c3/drivers/frame_provider/decoder/utils/vdec.c#L332-L341)).
- **Multi-instance use therefore means the frame ports** (`amstream_vframe`, `amstream_hevc_frame`) with the `amvdec_m*` multi-instance decoders (`amvdec_mh264`, `amvdec_mmpeg12`, and others). The kernel side needs `CONFIG_AMLOGIC_MEDIA_MULTI_DEC=y`, which both CE kernels set: CE21 config line 1332, CE22 config line 6464.

### Control ioctls

The ioctls are in `include/linux/amlogic/media/utils/amstream.h`. On 4.9 that is [`K49/include/linux/amlogic/media/utils/amstream.h`](https://github.com/CoreELEC/linux-amlogic/blob/50c8e8154c5aba3c7cd8438d04dd5fdd51733e41/include/linux/amlogic/media/utils/amstream.h); 5.15 has the same path in `CD515`. They include:

- `AMSTREAM_IOC_SET` / `AMSTREAM_IOC_GET` with `AMSTREAM_SET_VFORMAT`, `AMSTREAM_SET_VID`, `AMSTREAM_SET_FRAME_BASE_PATH`, and `AMSTREAM_SET_EOS`
- `AMSTREAM_IOC_SYSINFO`
- `AMSTREAM_IOC_PORT_INIT`
- `AMSTREAM_IOC_TSTAMP`
- `AMSTREAM_IOC_VB_STATUS`

Elementary-stream bytes are fed with `write()`.

### Where the frames go

Decoded frames are not returned to the caller. They are pushed into a VFM chain configured in `/sys/class/vfm/map`. The default chain is `decoder → amvideo`, which is the hardware video layer.

Kodi's sequence in [`CoreELEC/xbmc .../AMLCodec.cpp`](https://github.com/CoreELEC/xbmc/blob/d49aa4ac2362adf51da4eaa7c6ccea6e619fb860/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp) is:

1. `dlopen("libamcodec.so")` (L211).
2. For single-mode decode, set the VFM map `default` to `decoder amlvideo deinterlace amvideo` (L2536-L2540).
3. Call `codec_init()`.
4. Open `/dev/video10` (`amlvideo`) `O_RDONLY | O_NONBLOCK` (L2584). It then calls `VIDIOC_DQBUF` and `VIDIOC_QBUF` (L3204, L3244) only to learn which frame is on screen and its pts.

`libamcodec` is proprietary and binary-only: `PKG_LICENSE="proprietary"` in [`CE22/projects/Amlogic-ce/packages/multimedia/libamcodec/package.mk`](https://github.com/CoreELEC/CoreELEC/blob/98a729b80ece21a7ceb8dbab127a6d19a6cff60a/projects/Amlogic-ce/packages/multimedia/libamcodec/package.mk). It is shipped as `/usr/lib/libamcodec.so` on both images.

### Getting frames back into userspace on CE22

- The vendor route is `v4lvideo` (`CONFIG_AMLOGIC_V4L_VIDEO3=y` on CE22 only). It has up to 9 instances (`N_DEVS 9`) at `/dev/video30` + instance number, and VFM receivers named `v4lvideo.<n>` ([`CD515/drivers/media/video_processor/v4lvideo/v4lvideo.c` L54-L69, L2736-L2749](https://github.com/CoreELEC/common_drivers/blob/8ca9b6c3b7152fa2959147a403fc5e7e697d78d2/drivers/media/video_processor/v4lvideo/v4lvideo.c#L54-L69)).
- It works on UVM-backed dma-bufs (`meson_uvm_core.h`) and is designed around Android's gralloc and `video_composer`. No open-source Linux client of it is known to us. This route has the most unknowns.
- On CE21, `v4lvideo` and `video_composer` are not built (config lines 1510 and 1515). Only `amlvideo` (`amlvideodri.ko`, `/dev/video10`) exists.
- An older open-source amstream client that talks to the ioctls directly, without libamcodec, is [OtherCrashOverride/c2play](https://github.com/OtherCrashOverride/c2play). It targets the 3.14 kernel ABI and needs re-checking against the 4.9 and 5.15 `amstream.h`.

### Fit for a multi-tile wall

The amstream path is built for one full-screen video layer.

- The CE22 vendor KMS driver exposes only two video planes, `video1_block` and `video2_block` ([`CD515/arch/arm64/boot/dts/amlogic/mesong12b_drm.dtsi` L194-L206](https://github.com/CoreELEC/common_drivers/blob/8ca9b6c3b7152fa2959147a403fc5e7e697d78d2/arch/arm64/boot/dts/amlogic/mesong12b_drm.dtsi#L194-L206)).
- Their formats are NV12, NV21, UYVY, and VUY888 ([`CD515/drivers/drm/meson_plane.c` L193-L198](https://github.com/CoreELEC/common_drivers/blob/8ca9b6c3b7152fa2959147a403fc5e7e697d78d2/drivers/drm/meson_plane.c#L193-L198)).

A 3×3 grid therefore cannot be one video plane per stream. Frames have to be composited, by GE2D, by the Mali-G52 through EGL dma-buf import, or on the CPU.

## API 3: mainline `meson-vdec` (staging)

This is reference material. CoreELEC does not use it. LibreELEC's mainline "Amlogic" project does, together with the chewitt/ffmpeg commit [`c04518bc`](https://github.com/chewitt/ffmpeg/commit/c04518bc5d82315e57b14a7c915581a0667d1b4c) (labelled `amlogic-9.0.1`) that CE22's `ffmpeg/package.mk` pins for `PROJECT=Amlogic`.

- **Location.** [`drivers/staging/media/meson/vdec/`](https://github.com/torvalds/linux/tree/cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a/drivers/staging/media/meson/vdec). Device tree compatible `amlogic,g12a-vdec`, in `arch/arm64/boot/dts/amlogic/meson-g12-common.dtsi` line 2148; G12B inherits it. Video device name `meson-video-decoder`, dynamic node number (`vdec.c` L1078, L1089).
- **Formats on G12A/G12B** ([`vdec_platform.c` L192-L240](https://github.com/torvalds/linux/blob/cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a/drivers/staging/media/meson/vdec/vdec_platform.c#L192-L240)):

  | Codec | Max size | CAPTURE formats |
  | --- | --- | --- |
  | VP9 | 3840×2160 | `NV12M` |
  | H.264 | 3840×2160 | `NV12M` |
  | MPEG-1/2 | 1920×1080 | `NV12M`, `YUV420M` |

  There is no HEVC entry. HEVC needs out-of-tree LibreELEC patches.
- **One session.** `vdec_start_streaming` returns `-EBUSY` when `core->cur_sess` is another session ([`vdec.c` L289-L291](https://github.com/torvalds/linux/blob/cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a/drivers/staging/media/meson/vdec/vdec.c#L289-L291)). Only one decoder can stream at a time.
- **Status.** Staging. The [`TODO`](https://github.com/torvalds/linux/blob/cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a/drivers/staging/media/meson/vdec/TODO) says the driver is not guaranteed to follow the latest stateful decoder specification.

## API 4: v4l2-request (stateless)

Not applicable to Amlogic.

- The stateless API ([`dev-stateless-decoder.rst`](https://docs.kernel.org/userspace-api/media/v4l/dev-stateless-decoder.html)) is implemented by Cedrus, Hantro, rkvdec, and similar drivers. Mainline `drivers/media/platform/amlogic/` contains only `meson-ge2d` and the C3 ISP and MIPI drivers. The only Amlogic decoder is the stateful staging driver above. The vendor trees have none either.
- CoreELEC carries Jonas Karlman's v4l2-request FFmpeg patch set (`CE22/packages/multimedia/ffmpeg/patches/v4l2-request/0001-v4l2-request.patch`, 13 patches: hwcontext plus MPEG-2, H.264, HEVC, VP8, VP9, and AV1 hwaccels). It is configured off for Amlogic-ce (`PKG_FFMPEG_REQUEST_*` are emptied for `Generic|Amlogic-ce`, and `V4L2_SUPPORT=no` adds `--disable-v4l2-request`).

## API 5: FFmpeg `h264_v4l2m2m` / `hevc_v4l2m2m`, if self-built

Only relevant if the device AppImage bundles its own FFmpeg. It does not today; it links CoreELEC's libraries.

- **Decoders.** `M2MDEC(h264, ..., "h264_mp4toannexb")`, `M2MDEC(hevc, ..., "hevc_mp4toannexb")`, plus MPEG-1/2/4, H.263, VC-1, VP8, and VP9 ([`v4l2_m2m_dec.c` L260-L268](https://github.com/FFmpeg/FFmpeg/blob/573f2b81ebd97da81def2ba762e81b0328547004/libavcodec/v4l2_m2m_dec.c#L260-L268)).
- **Device choice.** There is no `device` option. `ff_v4l2_m2m_codec_init` scans `/dev/video*` in `readdir` order and keeps the first node whose caps and formats fit ([`v4l2_m2m.c` L291-L328](https://github.com/FFmpeg/FFmpeg/blob/573f2b81ebd97da81def2ba762e81b0328547004/libavcodec/v4l2_m2m.c#L291-L328)). On CE21 the only M2M node should be `video26`. `amlvideo` at `video10` is capture-only.
- **Frames.** Frames wrap the mmapped CAPTURE buffers with `av_buffer_create`, so there is no copy ([`v4l2_buffers.c` L256-L270](https://github.com/FFmpeg/FFmpeg/blob/573f2b81ebd97da81def2ba762e81b0328547004/libavcodec/v4l2_buffers.c#L256-L270)). The pixel format maps `NV12`/`NV12M` to `AV_PIX_FMT_NV12` and `NV21M` to `AV_PIX_FMT_NV21` ([`v4l2_fmt.c` L46-L72](https://github.com/FFmpeg/FFmpeg/blob/573f2b81ebd97da81def2ba762e81b0328547004/libavcodec/v4l2_fmt.c#L46-L72)).
- **No DRM PRIME upstream.** Upstream v4l2m2m does not emit `AV_PIX_FMT_DRM_PRIME`. It is absent in both 6.0 and current master. That needs the LibreELEC "v4l2-drmprime" patch by Lukas Rusak ("libavcodec: v4l2m2m: output AVDRMFrameDescriptor", `CE22/packages/multimedia/ffmpeg/patches/v4l2-drmprime/0001-v4l2-drmprime.patch`). CoreELEC applies it only to non-Amlogic projects.
- **Expected issues against `aml-vcodec-dec`:**
  - the `multiplanar` mismatch described above
  - NV21 being the first enumerated CAPTURE format
  - the 2048×1088 `TRY_FMT` clamp; FFmpeg sets the CAPTURE format from `G_FMT` after the source-change event, so this is probably harmless
  - no way to pass `struct aml_dec_params`

  A hand-written V4L2 client avoids all four. It can keep using CoreELEC's libavformat for HLS demux, and libavcodec for bitstream filters only.

## `/dev/video*` map

| Node | Driver | Kind | CE21 | CE22 |
| --- | --- | --- | --- | --- |
| `/dev/video10` | `amlvideo` (`video_processor/video_dev/amlvideo.c`, `video_nr_base = 10`) | capture of VFM frames; Kodi uses it for timing | `amlvideodri.ko` | built in |
| `/dev/video11` | `amlvideo2` (`video_nr = 11`) | capture | not built | built in (`CONFIG_AMLOGIC_V4L_VIDEO2=y`) |
| `/dev/video13`+ | `ionvideo` (`video_nr_base = 13`) | capture to ION | not built | built in (`CONFIG_AMLOGIC_IONVIDEO=y`) |
| `/dev/video26` | `aml-vcodec-dec` (`amvdec_ports`) | **V4L2 M2M stateful decoder** | yes | **no** |
| `/dev/video30`+ | `v4lvideo` (`video_nr_base = 30`, `N_DEVS 9`) | capture of decoder vframes into UVM dma-bufs | not built | built in (`CONFIG_AMLOGIC_V4L_VIDEO3=y`) |

Node numbers come from the drivers' `video_register_device` calls in `K49/drivers/amlogic/media/video_processor/` and `CD515/drivers/media/video_processor/`. Check on a box with `for d in /sys/class/video4linux/*; do echo "$d $(cat $d/name)"; done`.

## Buffer formats: NV12 and DRM PRIME

- **Decoder output.** Semi-planar 4:2:0, NV21 by default, NV12 on request. The CAPTURE buffers are CMA-backed and physically contiguous. They can be exported as dma-buf with `VIDIOC_EXPBUF`.
- **CE21.** There is no KMS driver, so DRM PRIME scanout is not available. Output would have to go through fbdev/OSD on the CPU or GE2D, or through the amvideo layer via VFM, which is not dma-buf driven.
- **CE22 scanout.** The vendor KMS video planes accept NV12 and NV21 framebuffers. `meson_video_plane_fb_check` needs a GEM object it can resolve to a physical address, or a UVM dma-buf ([`CD515/drivers/drm/meson_plane.c` L620-L688](https://github.com/CoreELEC/common_drivers/blob/8ca9b6c3b7152fa2959147a403fc5e7e697d78d2/drivers/drm/meson_plane.c#L620-L688)). Whether a dma-buf exported from a V4L2 CAPTURE buffer imports cleanly through `drmPrimeFDToHandle` → `drmModeAddFB2(DRM_FORMAT_NV12)` is unverified. It is also moot on stock CE22, which has no V4L2 decoder.
- **Mali-G52.** `EGL_EXT_image_dma_buf_import` with NV12 is the usual way to composite many decoded tiles on the GPU. CoreELEC ships Amlogic's `opengl-meson` (libMali) blobs. Whether they support that extension on fbdev and GBM targets has not been checked.

## Multi-instance limits

| Limit | Value | Source |
| --- | --- | --- |
| `vdec_core` instance ids | `ida_simple_get(..., 0, MAX_INSTANCE_MUN)`: 9 stock, **2 on CE21** | [`MM49/.../vdec.c` L1141-L1142](https://github.com/CoreELEC/media_modules-aml/blob/60561618f2bf154c1438d3c790b625ff01f215c3/drivers/frame_provider/decoder/utils/vdec.c#L1141-L1142), [`MM515/.../vdec.c` L1808-L1809](https://github.com/CoreELEC/media_modules-aml/blob/c15aebb1ac93028d0aceadf8f35dda2204d33424/drivers/frame_provider/decoder/utils/vdec.c#L1808-L1809), CE21 patch 003 |
| Shared between V4L2 and amstream | yes: the V4L2 adapter calls the same `vdec_create()` | [`MM49/.../aml_vcodec_adapt.c` L444](https://github.com/CoreELEC/media_modules-aml/blob/60561618f2bf154c1438d3c790b625ff01f215c3/drivers/amvdec_ports/aml_vcodec_adapt.c#L444) |
| Throughput (datasheet) | "multi-video decoder up to 4Kx2K@60fps+1x1080P@60fps"; H.264 HP@L5.1 up to 4Kx2K@30fps; HEVC and VP9 up to 4Kx2K@60fps | [S922X Public Datasheet V0.2 (Hardkernel)](https://dn.odroid.com/S922X/ODROID-N2/Datasheet/S922X_Public_Datasheet_V0.2.pdf), "Video/Picture CODEC" |
| CMA for decoder buffers (`codec_mm_cma`) | CE21: `0x1b000000` (432 MiB) from `g12b_a311d_w400.dts`, the base of the GS-King X device tree. CE22: `0x13400000` (308 MiB) from `g12b_a311d_w400_linux.dts` | [`K49/.../g12b_a311d_w400.dts` L135](https://github.com/CoreELEC/linux-amlogic/blob/50c8e8154c5aba3c7cd8438d04dd5fdd51733e41/arch/arm64/boot/dts/amlogic/g12b_a311d_w400.dts#L135), [`CD515/.../g12b_a311d_w400_linux.dts` L144-L152](https://github.com/CoreELEC/common_drivers/blob/8ca9b6c3b7152fa2959147a403fc5e7e697d78d2/arch/arm64/boot/dts/amlogic/g12b_a311d_w400_linux.dts#L144-L152) |
| `v4lvideo` instances (CE22) | 9 | `CD515/.../v4lvideo.c` L69 |
| KMS video planes (CE22) | 2 | `CD515/.../mesong12b_drm.dtsi` L194-L206 |
| Mainline `meson-vdec` | 1 streaming session | `vdec.c` L289-L291 |

### Rough capacity for this repo's 3×3 HLS wall

These are estimates, not measurements.

- **H.264 throughput.** One 1080p30 H.264 stream is 8,160 macroblocks per frame × 30 = 244,800 MB/s. The H.264 4K30 ceiling is 32,400 × 30 = 972,000 MB/s, which is about **4 streams of 1080p30 H.264**. Nine tiles need lower-resolution variants: nine 720p30 streams are 9 × 108,000 = 972,000 MB/s, right at the ceiling. Nine 540p streams fit with margin. This matches the "tile-size bitrate picker" the README lists as later work.
- **HEVC throughput.** HEVC has its own core with the 4K60 budget (about 8 × 1080p30 by pixel rate). It can also use `double_write_mode` to emit tile-sized frames.
- **Memory.** A 1920×1088 NV12 frame is about 3.1 MB. With a DPB of about 16 frames plus 4 to 6 output buffers, one 1080p H.264 instance needs roughly 60–70 MB of CMA. Nine of them would not fit in 308–432 MiB. Lower variants or a smaller `ref_buf_margin` would be required.
- **On CE21 none of this matters past 2 instances.** Kodi is stopped while the player runs, so both instances are available to the player. The other seven tiles would stay on `FfmpegDecoder`, or CoreELEC's patch would have to be reverted in a custom image.

## How this maps onto `DecoderBackend`

A future `V4l2M2mDecoder : DecoderBackend` for CE21 (and for CE22 only if `amvdec_ports` gets built) would line up with the existing contract like this:

| `DecoderBackend` | V4L2 M2M implementation |
| --- | --- |
| `open(url)` | Keep libavformat demux. The format and protocol whitelists, `rw_timeout`, `AVIOInterruptCB`, and the HLS variant discard in `FfmpegDecoder::open` are not decoder-specific and could move into a shared demux helper. Then open `/dev/video26` and run the OUTPUT setup up to `STREAMON(OUTPUT)`. Fail fast if the node is missing, the codec is not in `ENUM_FMT`, or `vdec_create` fails at the first `QBUF`/`STREAMON` because of the instance cap (look for `vdec_create request id failed` in `dmesg`). The factory can then fall back to `FfmpegDecoder` for that tile. |
| `readFrame(frame)` | Re-queue the CAPTURE buffer handed out last time. Then read packets → `QBUF(OUTPUT)` → `poll()` → `DQBUF(CAPTURE)`. Fill `DecodedFrame` from the mmapped planes. This lifetime matches "valid until the next `readFrame()` or `close()`" exactly. Copy `pts` from `v4l2_buffer.timestamp`, which the driver propagates with `V4L2_BUF_FLAG_TIMESTAMP_COPY`. |
| `abort()` | Set the latch and signal an `eventfd` that is in the `poll()` set next to the V4L2 fd. Demux-side blocking is already covered by the same `AVIOInterruptCB` as `FfmpegDecoder`. |
| `close()` | `STREAMOFF` → `REQBUFS(0)` → `close(fd)`. Closing the fd is what returns the vdec id and the CMA memory. |
| `isHardware()` | `true`. `StreamPlayer::sessionLoop` currently rejects this and must be changed. |

Interface changes this research implies:

1. **Pixel format.** The hardware emits NV12 or NV21, not three-plane `Yuv420p`. Either add `FramePixelFormat::Nv12` (`data[0]` = Y, `data[1]` = interleaved CbCr) and teach `SoftwareRenderer`, `StreamPlayer::OwnedFrame`/`copyOwned`, and `app.cpp`'s `frameLooksComplete` about it, or deinterleave to I420 inside the backend, which costs one CPU pass per frame. The first is cheaper and keeps `Renderer` free of V4L2 types.
2. **Zero-copy, later.** The comment on `DecoderBackend::readFrame` already anticipates "a future DRM-prime hardware decoder". The natural shape is an optional descriptor next to `DecodedFrame`: dma-buf fd per plane, offsets, pitches, `DRM_FORMAT_NV12`, and modifier `DRM_FORMAT_MOD_LINEAR`, filled from `VIDIOC_EXPBUF`. Only a CE22 `DrmRenderer` could consume it. With only two KMS video planes, a 3×3 wall still needs a compositor (GE2D or GLES) in between.
3. **Instance accounting in `StreamPlayer`.** Up to `kMaxBusyRetired` retired workers can still own their decoder while a stuck libavformat read finishes (`reapFinished` calls `close()` only after the thread returns). A hardware decoder in that state still holds a vdec id and its CMA. A hardware backend should drop its V4L2 fd as soon as `abort()` is seen on the decode thread, and not wait for `close()`. Alternatively, the factory should count live hardware sessions against the platform cap (2 on CE21) and hand out `FfmpegDecoder` beyond it.
4. **Factory selection.** `MqttController::DecoderFactory` (`src/mqtt_controller.cpp` line 157) is the seam. A mixed policy fits the measured limits: hardware for the first N tiles, where N comes from probing `/dev/video26` and the instance cap, and software for the rest.
5. **Build.** No new link dependency is needed for the V4L2 path: just `linux/videodev2.h` and ioctls. Amlogic-private controls and `struct aml_dec_params` would need a vendored header that matches the target's media_modules tree. `libamcodec` should be avoided because it is proprietary. amstream ioctls would need the GPL `amstream.h` from the matching kernel tree.

### Recommendation

- **CE21 Amlogic-ng.** A small hand-rolled V4L2 M2M client on `/dev/video26` is the realistic first hardware backend. Expect at most two hardware tiles unless CoreELEC's instance-limit patch is reverted. Keep libavformat for demux.
- **CE22 Amlogic-no.** There is no V4L2 decoder as shipped. The options are:
  - Ask CoreELEC to add `CONFIG_AMLOGIC_MEDIA_V4L_DEC=y` to the media_modules `make_target`, which builds `amvdec_ports.ko` and the `*_v4l.ko` decoders. `CONFIG_MODVERSIONS=y` with no module signing means a matching out-of-tree build would also load.
  - Go through amstream frame ports plus `v4lvideo`, which is vendor-specific, Android-shaped, and has no open-source Linux client.
  - Stay on software decode.
- **Not worth pursuing on CoreELEC:**
  - v4l2-request, because no driver exists.
  - Mainline `meson-vdec`, because it is not in CoreELEC's kernels and is single-session.
  - FFmpeg `*_v4l2m2m` from CoreELEC's libavcodec, because it is compiled out.

## What still needs a GS-King X

Run each of these on both CoreELEC lines. Each check is followed by what to record.

- `ls -l /dev/video* /dev/amstream_* /dev/dri 2>&1` and the `/sys/class/video4linux/*/name` loop above: confirm `video26` is present on CE21 and absent on CE22, and whether `/dev/dri/card0` exists on CE21.
- `lsmod | grep -E 'amvdec|v4l2'` and `dmesg | grep -iE 'v4ldec|vcodec|vdec_create'`: which modules are loaded, and whether the decoder registered.
- `cat /sys/module/amvdec_ports/parameters/multiplanar` (CE21): the default value. Then check whether FFmpeg-style `CAPTURE_MPLANE` `REQBUFS` fails with `EINVAL` while it is `N`.
- Open three `aml-vcodec-dec` sessions at once on CE21: does the third fail at `vdec_create`? Record the errno seen by userspace.
- `cat /sys/class/codec_mm/codec_mm_dump` while N 1080p and 720p H.264 sessions run: CMA use per session.
- `cat /sys/class/vdec/vdec_status` and `dump_fps` with 4 to 9 sessions: actual throughput against the datasheet estimate.
- CE22: whether a `media_modules-aml` build with `CONFIG_AMLOGIC_MEDIA_V4L_DEC=y`, done in the CoreELEC build tree, loads and registers `/dev/video26`.
- CE22: whether a V4L2 or `v4lvideo` dma-buf imports into the vendor KMS video plane as `DRM_FORMAT_NV12`.
