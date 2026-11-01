# Amlogic hardware decode through FFmpeg

Research note, 2026-11-01. This is a reading of upstream FFmpeg, the CoreELEC trees this player targets, and the mainline Meson decoder. It does not change the player. Nothing here was run on a GS-King X.

The player links CoreELEC's FFmpeg and decodes on the CPU. `FfmpegDecoder::prepareAmlogicHwaccel()` in `src/ffmpeg_decoder.cpp` leaves `hw_device_ctx` null, and `isHardware()` stays false. A hardware backend, when one exists, is a separate `DecoderBackend`. `DecoderBackend::readFrame()` already defaults to failure so a backend that only yields DRM-PRIME frames can stay linkable.

Two device images matter:

| Target | Tree | Kernel | Userspace FFmpeg in the image |
| --- | --- | --- | --- |
| `coreelec21-ng-armv7hf` | `CoreELEC/CoreELEC` `coreelec-21`, project `Amlogic-ce`, device `Amlogic-ng` | `CoreELEC/linux-amlogic` `amlogic-4.9-20` (`50c8e815`), plus `media_modules-aml` `60561618` | 6.0.1 (`libavcodec.so.60`), matching `scripts/coreelec-targets.sh` |
| `coreelec22-no-aarch64` | `coreelec-22`, project `Amlogic-ce`, device `Amlogic-no` | `linux-amlogic` `5.15.196_20260306` (`44cbccde`) plus `common_drivers` `8ca9b6c3` and `media_modules-aml` `c15aebb1` | 9.0.2 (`libavcodec.so.63`) |

Kodi on both lines is `CoreELEC/xbmc` (`acc0fc66` on 21, `d49aa4ac` on 22) with `KODIPLAYER_DRIVER=libamcodec`. Playback on the box does not go through an FFmpeg hwaccel.

`PROJECT=Amlogic` inside the same `package.mk` is LibreELEC's mainline Meson project. CoreELEC images are `PROJECT=Amlogic-ce`. The two builds do not share a decoder.

## Upstream FFmpeg has v4l2m2m wrappers, and no v4l2request hwaccel

Checked on tags `n6.0.1` and `n9.0.2` (the two versions the device images ship). `libavcodec/hwaccels.h` on 9.0.2 lists VAAPI, VDPAU, NVDEC, Vulkan, D3D, VideoToolbox, and QSV. There is no `v4l2request` entry. `configure` has `v4l2_m2m` and has no `v4l2_request` flag.

`v4l2m2m` is a set of full decoders, selected by name, with `AV_CODEC_CAP_AVOID_PROBING`. `avcodec_find_decoder(AV_CODEC_ID_H264)` still returns the software decoder. The wrappers in `libavcodec/v4l2_m2m_dec.c` are:

| Name | Codec id | Bitstream filter applied by the wrapper |
| --- | --- | --- |
| `h264_v4l2m2m` | H.264 | `h264_mp4toannexb` |
| `hevc_v4l2m2m` | HEVC | `hevc_mp4toannexb` |
| `mpeg1_v4l2m2m` | MPEG-1 | |
| `mpeg2_v4l2m2m` | MPEG-2 | |
| `mpeg4_v4l2m2m` | MPEG-4 | |
| `h263_v4l2m2m` | H.263 | |
| `vc1_v4l2m2m` | VC-1 | |
| `vp8_v4l2m2m` | VP8 | |
| `vp9_v4l2m2m` | VP9 | |

`wrapper_name` is `v4l2m2m`. Private options are `num_output_buffers` (default 16) and `num_capture_buffers` (default 20). Buffers are `V4L2_MEMORY_MMAP` (`libavcodec/v4l2_buffers.c`). The same files have no `AV_PIX_FMT_DRM_PRIME`, no `hw_device_ctx`, and no `V4L2_MEMORY_DMABUF`. Upstream 6.0.1 and 9.0.2 copy the capture buffer into a software frame (NV12 or another format the driver advertised). The 2018–2019 DRM-PRIME series (Lukas Rusak, ffmpeg-devel, September 2019) is still outside FFmpeg master. A December 2024 ffmpeg-devel thread was still carrying that series as an out-of-tree patch.

`v4l2request` is a real hwaccel on the software decoder. It parses in userspace and submits a media-request per frame. Jonas Karlman's August 2024 series (`h264_v4l2request`, `hevc_v4l2request`, `mpeg2_v4l2request`, device type `v4l2request`) was still marked New on patchwork when checked. FFmpeg 9.0.1, which LibreELEC and a 2026 Kodi Flatpak pull both call out, still has none of it. The code that ships in appliance trees is LibreELEC's `packages/multimedia/ffmpeg/patches/v4l2-request`.

That split matches the kernel. A stateful decoder (`V4L2_CAP_VIDEO_M2M` or `V4L2_CAP_VIDEO_M2M_MPLANE`, driver keeps reference frames) is what `*_v4l2m2m` opens. A stateless request-API decoder (Hantro, Cedrus, rkvdec) is what `*_v4l2request` opens. Amlogic's vendor decoder and mainline `meson-vdec` are stateful.

## v4l2m2m: CLI and AVCodecContext

On a kernel that registers a stateful M2M node, and an FFmpeg built with `--enable-v4l2_m2m`:

```bash
ffmpeg -hide_banner -decoders | grep v4l2m2m
ffmpeg -c:v h264_v4l2m2m -i sample.ts -f null -
ffmpeg -c:v hevc_v4l2m2m -num_capture_buffers 8 -i sample.ts -f rawvideo -pix_fmt nv12 out.yuv
```

The wrapper probes `/dev/video*` and keeps the first node whose capabilities include `V4L2_CAP_VIDEO_M2M` or `V4L2_CAP_VIDEO_M2M_MPLANE` (`v4l2_m2m.c`). A capture-only node is skipped. Failure looks like `Could not find a valid device`.

```c
const AVCodec* codec = avcodec_find_decoder_by_name("h264_v4l2m2m");
AVCodecContext* ctx = avcodec_alloc_context3(codec);
avcodec_parameters_to_context(ctx, stream->codecpar);
ctx->pkt_timebase = stream->time_base;

AVDictionary* opts = nullptr;
av_dict_set_int(&opts, "num_capture_buffers", 8, 0);
int rc = avcodec_open2(ctx, codec, &opts);
av_dict_free(&opts);

AVPacket* pkt = av_packet_alloc();
AVFrame* frame = av_frame_alloc();
// avcodec_send_packet / avcodec_receive_frame as usual.
// frame->format is the software format the driver negotiated, often NV12.
```

`hw_device_ctx` is unused on this path in upstream 6.0.1 and 9.0.2. Setting it does not turn the frame into DRM-PRIME.

### DRM-PRIME export, only with the out-of-tree patch

`packages/multimedia/ffmpeg/patches/v4l2-drmprime/0001-v4l2-drmprime.patch` on `coreelec-22` is the Rusak series, rebased. It adds `AV_PIX_FMT_DRM_PRIME` to the v4l2m2m decoder's `pix_fmts`, calls `ff_get_format`, and on that choice `VIDIOC_EXPBUF`s the capture buffer into an `AVDRMFrameDescriptor` stored at `frame->data[0]`. It allocates an `AV_HWDEVICE_TYPE_DRM` device internally when the caller did not pass one. The patch commit message names the historical CLI:

```bash
ffmpeg -hwaccel drm -hwaccel_output_format drm_prime \
  -c:v h264_v4l2m2m -i sample.ts -f null -
```

Application side, once that patch is compiled in:

```c
static enum AVPixelFormat pick_drm(AVCodecContext* ctx, const enum AVPixelFormat* fmts) {
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_DRM_PRIME)
            return *p;
    }
    return fmts[0];
}

const AVCodec* codec = avcodec_find_decoder_by_name("hevc_v4l2m2m");
AVCodecContext* ctx = avcodec_alloc_context3(codec);
avcodec_parameters_to_context(ctx, stream->codecpar);
ctx->pkt_timebase = stream->time_base;
ctx->get_format = pick_drm;
av_hwdevice_ctx_create(&ctx->hw_device_ctx, AV_HWDEVICE_TYPE_DRM,
                        "/dev/dri/card0", nullptr, 0);
avcodec_open2(ctx, codec, nullptr);

// After avcodec_receive_frame:
// frame->format == AV_PIX_FMT_DRM_PRIME
// auto* desc = reinterpret_cast<const AVDRMFrameDescriptor*>(frame->data[0]);
// desc->objects[i].fd is a dma-buf. desc->objects[i].format_modifier is
// DRM_FORMAT_MOD_* . desc->layers[i].format is the DRM fourcc.
// The fd is owned by the AVFrame. dup() it before av_frame_unref if the
// renderer still needs it. Close the duplicate; do not close the original.
```

`AVDRMFrameDescriptor` (`libavutil/hwcontext_drm.h` on 9.0.2) holds up to four objects and four layers. Upstream documents that the DRM hwdevice does not allocate frames itself: the decoder exports them.

One `AVCodecContext` per stream. Each open is its own V4L2 fd and its own capture pool. Sharing one context across tiles is a data race, and `AV_CODEC_CAP_HARDWARE` wrappers are not a thread-safe decode pool. The dma-buf import (`drmPrimeFDToHandle`) has to use the same DRM card fd the modifier was meant for.

## v4l2request: CLI and AVCodecContext

The patch CoreELEC carries at `packages/multimedia/ffmpeg/patches/v4l2-request/` on `coreelec-22` (files `0001-v4l2-request.patch`, `0002-v4l2-request-vc1.patch`) adds hwaccels that sit on the software decoders and output `AV_PIX_FMT_DRM_PRIME`:

| Hwaccel name | Needs kernel control (approx.) |
| --- | --- |
| `mpeg2_v4l2request` | `V4L2_CID_STATELESS_MPEG2_SEQUENCE` |
| `h264_v4l2request` | `V4L2_CID_STATELESS_H264_DECODE_MODE` (kernel 5.11) |
| `hevc_v4l2request` | stateless HEVC controls |
| `vp8_v4l2request` | stateless VP8 controls |
| `vp9_v4l2request` | stateless VP9 controls |
| `av1_v4l2request` | `V4L2_CID_STATELESS_AV1_FRAME` (kernel 6.5) |
| `vc1_v4l2request` | second patch; also selected by WMV3 |

Device type string is `v4l2request` (`AV_HWDEVICE_TYPE_V4L2REQUEST`). Configure flags from the patch are `--enable-libudev --enable-v4l2-request`. The hwaccel probes a media-controller device and a video node that advertises the stateless controls, then mmaps OUTPUT buffers and queues a request fd per slice or frame.

```bash
ffmpeg -hwaccel v4l2request -hwaccel_output_format drm_prime -i sample.ts -f null -
```

There is no `-c:v h264_v4l2request`. The software `h264` decoder grows a hwaccel, the same way VAAPI does.

```c
const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
AVCodecContext* ctx = avcodec_alloc_context3(codec);
avcodec_parameters_to_context(ctx, stream->codecpar);
ctx->pkt_timebase = stream->time_base;
ctx->get_format = pick_drm;  // same helper as above, prefer DRM_PRIME
av_hwdevice_ctx_create(&ctx->hw_device_ctx, AV_HWDEVICE_TYPE_V4L2REQUEST,
                        nullptr, nullptr, 0);
// extra_hw_frames adds capture slots beyond the DPB. Stateless drivers
// need the references resident as dma-bufs.
ctx->extra_hw_frames = 8;
avcodec_open2(ctx, codec, nullptr);
// frame->data[0] is again an AVDRMFrameDescriptor.
```

`pick_drm` and `AV_HWDEVICE_TYPE_V4L2REQUEST` exist only after this patch. Stock 6.0.1 and 9.0.2 headers have neither.

This hwaccel has nowhere to bind on a CoreELEC box. The vendor decoder does not implement the stateless request API, and mainline `meson-vdec` is a stateful M2M driver. `-hwaccel v4l2request` on the GS-King X kernel fails the media-device probe.

## What the CoreELEC FFmpeg build actually enables

`config/graphic` sets `V4L2_SUPPORT="no"` and turns it on for Gallium drivers (`lima`, `panfrost`, `vc4`, `etnaviv`, `freedreno`, `imagination`). `Amlogic-ce` sets `GRAPHIC_DRIVERS=""` and `OPENGLES="opengl-meson"`, so `V4L2_SUPPORT` stays no.

### CoreELEC 22 (`coreelec-22` `packages/multimedia/ffmpeg/package.mk`)

`Amlogic-ce` matches `Generic|Amlogic-ce`. That branch clears the v4l2-request configure words, because stock 9.0.2 has no `--disable-v4l2-request` option and would fail configure if it were passed. With `V4L2_SUPPORT=no` the image is configured `--disable-v4l2_m2m`.

`PKG_PATCH_DIRS` for `Amlogic-ce` is `postproc` and `libreelec` only. The v4l2 patch directories are not applied. Patches that do apply from `packages/multimedia/ffmpeg/patches/` are Kodi bitstream and subtitle fixes (MVC, MKV block-addition mapping, DTS core bits, PGS). They are not a hardware decoder.

The v4l2 patch directories are still in the git tree, unused by this project:

| Directory | What it is |
| --- | --- |
| `patches/v4l2-request/` | Stateless request hwaccels above, plus VC-1 |
| `patches/v4l2-drmprime/` | DRM-PRIME export for stateful `*_v4l2m2m` |
| `patches/vf-deinterlace-v4l2m2m/` | V4L2 M2M deinterlace filter |
| `patches/detlev/` | Rockchip HEVC `sps_st_rps`, not applied to `Amlogic-ce` |
| `patches/rpi/` | Raspberry Pi, not applied |

`package.mk` also contains a `PROJECT=Amlogic` arm that downloads `chewitt/FFmpeg` commit `c04518bc` and labels it `amlogic-9.0.1`. CoreELEC device builds never take that arm. It is the LibreELEC mainline fork, described below.

### CoreELEC 21 (`coreelec-21` `package.mk`)

FFmpeg is 6.0.1. The `*` arm, which includes `Amlogic-ce`, appends `v4l2-request` and `v4l2-drmprime` to `PKG_PATCH_DIRS`, so those patches are applied to the 6.0.1 tree during the image build. `V4L2_SUPPORT=no` then configures `--disable-v4l2_m2m --disable-libudev --disable-v4l2-request`. The symbols are compiled out of `libavcodec.so.60`. `ffmpeg -decoders` and `ffmpeg -hwaccels` on a 21 image should not list them. That has not been executed on a box; it is what the recipe says.

### Device check, when a box is available

```bash
ffmpeg -hide_banner -decoders | grep v4l2
ffmpeg -hide_banner -hwaccels
ls -l /dev/video* /dev/dri/*
```

`v4l2-ctl --all -d /dev/videoN` distinguishes a capture node (`V4L2_CAP_VIDEO_CAPTURE`) from an M2M decoder (`V4L2_CAP_VIDEO_M2M_MPLANE`). The image recipes do not install `v4l-utils`.

## The decoder CoreELEC actually runs

Both kernels build the vendor modules from `CoreELEC/media_modules-aml` and set `CONFIG_AMLOGIC_MEDIA_MULTI_DEC=y`. Kodi's `CAMLCodec::OpenDecoder` (`xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp` at both pins) calls `libamcodec` `codec_init` / `codec_write`. The default mode is `STREAM_TYPE_FRAME` with `video_path = FRAME_BASE_PATH_AMLVIDEO_AMVIDEO`. In `vdec.h` that is `VDEC_TYPE_FRAME_BLOCK`: frame-based multi-instance decode, input as a block list. `STREAM_TYPE_SINGLE` is reserved for MVC and VC-1 and rewrites the vfm map to `decoder amlvideo deinterlace amvideo`. Dolby Vision FEL can switch the same open to `STREAM_TYPE_STREAM`.

Pictures are dequeued from `/dev/video10`, which `common_drivers` `amlvideo.c` registers as `V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING | V4L2_CAP_READWRITE`, base index 10. Kodi's `DequeueBuffer` reads the buffer index and the timestamp. It does not treat the node as a bitstream decoder and it does not export a DRM-PRIME fd. `h264_v4l2m2m` will not bind to `/dev/video10`.

The source tree also contains `drivers/amvdec_ports/`, a stateful multiplanar M2M decoder (`V4L2_CAP_VIDEO_M2M_MPLANE`, `video_register_device(..., 26)`, so `/dev/video26` if it loads). Its fourcc table lists H.264, HEVC, VP9, MPEG-1/2/4, MJPEG, AVS, AV1, AVS2, AVS3, VC-1, H.266, and NV12/NV21 capture layouts. The module is behind `CONFIG_AMLOGIC_MEDIA_V4L_DEC` in `drivers/Makefile`. The CE22 `media_modules-aml` `make_target` does not pass that symbol, and `linux.aarch64.conf` on `Amlogic-no` does not set it. A separate bool, `CONFIG_AMLOGIC_V4L2` ("Amlogic V4L2 VENDOR HOOKS"), is also unset. The M2M decoder is source that the image does not build.

Modules the CE22 recipe does build: MPEG-2 multi, MPEG-4 multi, VC-1, H.264 multi, H.264 MVC, HEVC, H.266, VP9, MJPEG multi, AVS multi, AVS2, AVS3, AV1, and the front-back VP9/HEVC/AV1/AVS2 variants. `Amlogic-no` is one image for many SoCs (`SUBDEVICES` includes Odroid N2 / S922X and later parts). A `.ko` in the image is not a statement that the S922X die has that codec. AV1 and H.266 hardware post-date G12B. CE forum reports treat S922X MJPEG hardware decode as broken and leave it on the software decoder.

### How many instances

`vdec.h` in both module pins defines `MAX_INSTANCE_MUN` as 9 (the identifier is spelled that way). The Kconfig help for `AMLOGIC_MEDIA_MULTI_DEC` says the option is required for concurrent playback.

CoreELEC 21 still applies `projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml/patches/003-multi-decoders-limit-maximum-number-of-decoder.patch`. The subject says the limit is 1. The diff sets `MAX_INSTANCE_MUN` from 9 to 2 and drops a second define in `vdec_profile.c`. The header context matches `media_modules-aml` `60561618`, so the 21 module build is capped at two instances. The patch is a Kodi workaround from December 2019 (Portisch): Kodi opens one decoder, and a second instance confused the vfm map (display on stream 2, bitstream fed to stream 1, black screen).

CoreELEC 22's `media_modules-aml` package has no `patches/` directory. The 22 driver keeps the header value of 9. That is a driver array bound, not a measured S922X budget. Memory, the firmware, and the single video plane still cap a grid. This project has not opened two decoders on either image.

`amlvideo` numbers further capture nodes as `inst * 10 + 10`, then `video_nr_base_second` 23. Those nodes are the capture side of a vfm path. They show up only when a decoder instance is feeding them.

## Mainline meson-vdec, which CoreELEC does not boot

`drivers/staging/media/meson/vdec/` on `torvalds/linux` master (fetched 2026-11-01) is a stateful V4L2 M2M driver. GS-King X is upstream as `arch/arm64/boot/dts/amlogic/meson-g12b-gsking-x.dts` (`compatible = "azw,gsking-x", "amlogic,s922x", "amlogic,g12b"`). The vdec node comes from `meson-g12-common.dtsi` through `meson-g12.dtsi` and `meson-g12b.dtsi`:

```
vdec: video-decoder@ff620000 {
    compatible = "amlogic,g12a-vdec";
};
```

`vdec_platform_g12a` formats:

| Bitstream | Max | Capture |
| --- | --- | --- |
| H.264 | 3840x2160 | `V4L2_PIX_FMT_NV12M` |
| VP9 | 3840x2160 | `NV12M` |
| MPEG-1, MPEG-2 | 1920x1080 | `NV12M`, `YUV420M` |

HEVC is absent from the G12A and SM1 tables. VP9 uses the HEVC hardware block (`vdec_hevc_ops`) and is still VP9. Both queues allow `VB2_MMAP | VB2_DMABUF`, and the ioctl table includes `VIDIOC_EXPBUF`. That is the kernel half of a dma-buf export. Upstream FFmpeg still copies those buffers out. The staging `TODO` says the driver waits on stateful-decoder compliance, and MPEG-1/2 lack dynamic-resolution change.

LibreELEC's `PROJECT=Amlogic` is the consumer of this driver. Its FFmpeg is `chewitt/FFmpeg` `c04518bc` (parent `a5c6ec5`, the pin on LibreELEC master, dated 2026-10-07 for the child). That fork carries the v4l2request hwcontext plus Amlogic capture layouts that are not in upstream `videodev2.h`:

- `V4L2_PIX_FMT_NV12M`, described in the commit as the downstream Amlogic decoder's multi-planar NV12. Each plane is its own dma-buf.
- `V4L2_PIX_FMT_MESON_AM21C` (`AM21`), mapped to `DRM_FORMAT_MOD_AMLOGIC_FBC(SCATTER)` so the Meson overlay can scan the native framebuffer compression out as `DRM_FORMAT_YUV420_8BIT` / `10BIT` without a downsample to NV12.
- `V4L2_PIX_FMT_MESON_AM21B` (`AM2B`), the basic FBC layout, with the memory-saving option for 8-bit.

`DRM_FORMAT_MOD_AMLOGIC_FBC`, `AMLOGIC_FBC_LAYOUT_BASIC`, `AMLOGIC_FBC_LAYOUT_SCATTER`, and `AMLOGIC_FBC_OPTION_MEM_SAVING` are in upstream `drm_fourcc.h`. The `AM21` fourccs are local to that FFmpeg tree. Those modifiers are how a mainline Meson DRM plane imports a decoder buffer. They are not a format the CoreELEC vendor pipeline produces for FFmpeg.

## Multi-stream DRM-PRIME on the GS-King X

A grid of hardware frames wants one dma-buf set per tile and one place to composite them. The two kernels offer different compositors.

On the CoreELEC vendor kernel the scanout path is vfm: decoder, then `amlvideo`, deinterlace, `amvideo`. `CONFIG_AMLOGIC_VIDEO_COMPOSER=y` and `CONFIG_AMLOGIC_MEDIA_GE2D=y` are the pieces that can place more than one video into the single video plane. `video_composer.c` defaults `max_width` / `max_height` to 2560x1440 (picture mode 3840x2160). Kodi never drives that for a grid; it opens one `libamcodec` instance and one `/dev/video10`. Direct `drmModeAddFB2` of a linear NV12 PRIME buffer is a mainline-DRM shape. The vendor frames live in canvas, ION (`CONFIG_AMLOGIC_ION`), and the codec_mm heap (`CONFIG_AMLOGIC_HEAP_CODEC_MM`), and the plane Kodi uses is the vendor video plane plus DRM properties for Dolby Vision (`dv_mode`, `dv_policy` on the CRTC). `DrmRenderer` in this tree does not commit a plane.

On mainline, one Meson video overlay imports one FBC or NV12M buffer per atomic commit. A grid is either several overlay planes, which this hardware does not have in useful quantity, or a compositor (GE2D is not the mainline interface; GLES or a writeback does the scale). `meson-vdec` itself is one staging device. Concurrent sessions are not something this note can claim.

Practical shape for a later backend, still not this class:

1. One decoder object per tile, its own `AVCodecContext` or its own `codec_para_t`.
2. CE21's patched module allows two instances. CE22's header allows nine. Either number is the array size, not a promise that two 1080p streams fit in codec_mm on an S922X.
3. If the frames are DRM-PRIME, `dup()` each object fd out of the `AVDRMFrameDescriptor` before the next `avcodec_receive_frame` or `av_frame_unref`. Import with `drmPrimeFDToHandle` on the card that matches the modifier. Unref the frame only after the duplicate exists.
4. Upstream `*_v4l2m2m` without the drmprime patch cannot feed step 3. The shipped CE FFmpeg cannot either: 22 never builds the wrapper, 21 builds it and then `--disable`s it.
5. `v4l2request` frames are DRM-PRIME, and the GS-King X kernel has no stateless decoder for them to come from.
6. The path that already decodes on the box is `libamcodec` in `STREAM_TYPE_FRAME`, with pictures on the amlvideo capture node. Turning those into a grid means a second instance plus `video_composer` or GE2D, or an explicit dma-buf export the current Kodi code does not do. That is a new `DecoderBackend`. `prepareAmlogicHwaccel()` stays empty so the software decoder cannot grow an `hw_device_ctx`.

Codecs worth targeting first, because both the vendor multi decoders and (for H.264) mainline `meson-vdec` agree, and this player's whitelist already accepts them: H.264 and HEVC. HEVC is vendor-only until mainline grows a G12A format entry. VP9 is on both, and is outside the current whitelist. AV1 is a later SoC.

## Source pins

- FFmpeg `n6.0.1`, `n9.0.2`: `libavcodec/v4l2_m2m_dec.c`, `v4l2_m2m.c`, `v4l2_buffers.c`, `hwaccels.h`, `configure`.
- Rusak DRM-PRIME series: ffmpeg-devel, 2019-09, "V4L2 support for DRM_PRIME". Patch carried by MythTV and by LibreELEC `v4l2-drmprime`.
- Kwiboo / Jernej v4l2request: FFmpeg patchwork, series "Add V4L2 Request API hwaccels", 2024-08-06, state New when checked. LibreELEC `patches/v4l2-request` updated through 2026-03.
- CoreELEC `coreelec-21` and `coreelec-22`: `packages/multimedia/ffmpeg/package.mk`, `config/graphic`, `projects/Amlogic-ce/devices/Amlogic-ng/options`, `Amlogic-no/options`, `projects/Amlogic-ce/packages/linux/package.mk`, `linux-drivers/amlogic/media_modules-aml/package.mk`, CE21 patch `003-multi-decoders-limit-maximum-number-of-decoder.patch`.
- `CoreELEC/media_modules-aml` `c15aebb1` and `60561618`: `drivers/frame_provider/decoder/utils/vdec.h` (`MAX_INSTANCE_MUN`, `VDEC_TYPE_*`), `drivers/amvdec_ports/aml_vcodec_dec.c`, `aml_vcodec_dec_drv.c`.
- `CoreELEC/common_drivers` `8ca9b6c3`: `drivers/media/Kconfig`, `drivers/media/video_processor/video_dev/amlvideo.c`, `video_composer/video_composer.c`. Kernel config `projects/Amlogic-ce/devices/Amlogic-no/linux/linux.aarch64.conf` and the CE21 `Amlogic-ng` config.
- `CoreELEC/xbmc` `d49aa4ac` and `acc0fc66`: `AMLCodec.cpp` (`STREAM_TYPE_FRAME`, `/dev/video10`).
- `chewitt/FFmpeg` `c04518bc`: `hwcontext_v4l2request.c` Amlogic NV12M / AM21C / AM21B mappings.
- `torvalds/linux` master, 2026-11-01: `drivers/staging/media/meson/vdec/vdec_platform.c`, `vdec.c`, `arch/arm64/boot/dts/amlogic/meson-g12b-gsking-x.dts`, `meson-g12-common.dtsi`, `include/uapi/drm/drm_fourcc.h`.
