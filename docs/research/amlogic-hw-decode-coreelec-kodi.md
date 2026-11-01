# Research: CoreELEC and Kodi Hardware Decoding on Amlogic

This document summarizes how hardware video decoding is implemented and managed on Amlogic SoCs (especially the S922X / GS-King X) across CoreELEC and mainline Kodi.

Deep dive on the CoreELEC `libamcodec` / `CAMLCodec` path (STREAM_TYPE_FRAME call sequence, concurrency, abort/hang, licensing, DecoderBackend mapping, device probe): [`amlogic-libamcodec-amlcodec.md`](amlogic-libamcodec-amlcodec.md).

## 1. CoreELEC: Amlogic-ng vs Amlogic-no

CoreELEC maintains different branches based on the underlying Amlogic vendor kernel. The two primary ones for recent devices like the S922X (G12B family) are **Amlogic-ng** and **Amlogic-no**.

*   **Amlogic-ng (Next Generation)**: Based on the older Amlogic vendor kernel 4.9.269. This is the mature, stable branch. It supports Dolby Vision FEL (Full Enhancement Layer) playback, which is highly sought after by home theater enthusiasts.
*   **Amlogic-no (New Order)**: Based on the newer Amlogic vendor kernel 5.15.x. This is the active development branch (CoreELEC 22+). It drops support for older GXL chips (like S905X) and introduces support for newer SoCs. However, it currently lacks Dolby Vision FEL support because the `dovi.ko` kernel module provided by manufacturers has not been updated for the 5.15 kernel.

**Hardware Decoding Differences:**
While both branches support hardware decoding for standard formats (H.264, HEVC, VP9), there are nuances with newer codecs like AV1 on newer chips (e.g., S905X4). Amlogic-no (kernel 5.15) requires newer video microcode (ucode >= 3.49) for AV1 hardware decoding, whereas Amlogic-ng can decode AV1 with older ucode versions (e.g., 3.33).

## 2. amcodec / amvdec History vs Modern V4L2

There are two primary paradigms for Amlogic hardware decoding in the Linux ecosystem: the legacy vendor-specific `amcodec` and the modern mainline `V4L2` (Video4Linux2) API.

### Legacy: amcodec / libamcodec
Historically, Amlogic provided a proprietary, out-of-tree API for video decoding known as `amcodec`.
*   **CoreELEC/LibreELEC (Older)**: Kodi relied heavily on `AMLCodec.cpp` to interface directly with the Amlogic kernel drivers (`/dev/amvideo`, `/dev/amstream`).
*   **Mainline Kodi**: Kodi dropped Amlogic-specific `amcodec` support in Kodi 19 (Matrix). Users running standard Android on Amlogic shifted to using the standard Android MediaCodec API.
*   **Add-ons**: Because `amcodec` sometimes mishandles specific files (e.g., rotated phone clips or old MJPEG AVIs), community add-ons like [Limp](https://discourse.coreelec.org/t/limp-automatically-trigger-software-decoding-when-its-needed/59513) were created for CoreELEC to dynamically force software decoding (FFmpeg) for problematic files, falling back to `amcodec` for 4K/HDR content.

### Modern: V4L2 (meson-vdec)
Mainline Linux (and modern LibreELEC/Armbian) uses the `meson-vdec` driver, which exposes the Amlogic hardware decoder through the standard V4L2 Memory-to-Memory (M2M) stateful API.
*   **Implementation**: Exposes the decoder at `/dev/video0`.
*   **Kodi Integration**: Kodi uses `CDVDVideoCodecDRMPRIME` to interface with FFmpeg's `h264_v4l2m2m` (and similar V4L2 wrappers). This allows zero-copy rendering directly to the GPU via DRM PRIME.

## 3. FFmpeg Patches in CoreELEC / LibreELEC

To make the mainline V4L2 hardware decoding work seamlessly with Kodi, FFmpeg requires specific patches.

*   **The Problem**: Stock FFmpeg's V4L2 M2M decoder (`h264_v4l2m2m`) outputs standard pixel formats (like `NV12`). However, Kodi's hardware rendering path (`CDVDVideoCodecDRMPRIME`) requires the decoder to output `AV_PIX_FMT_DRM_PRIME` to achieve zero-copy rendering. If FFmpeg outputs `NV12`, Kodi silently falls back to software decoding.
*   **The Solution**: LibreELEC and CoreELEC maintain a `v4l2-drmprime` patchset for FFmpeg. This patch modifies the V4L2 M2M wrappers in FFmpeg to output DRM PRIME frames. 
*   **Reference**: See the [LibreELEC FFmpeg package patches](https://github.com/LibreELEC/LibreELEC.tv/tree/master/packages/multimedia/ffmpeg/patches/v4l2-drmprime) and community projects like [kodi-hwdec-amlogic-armbian](https://github.com/dogukangecko/kodi-hwdec-amlogic-armbian) which backport these patches to stock Ubuntu FFmpeg.

## 4. Kodi Launch/Stop Interaction and Decoder State

The interaction between Kodi and the Amlogic kernel decoder is critical. Improper state management often leads to the infamous "black screen" bug.

### Launch and Stop (amcodec)
In the CoreELEC `AMLCodec.cpp` implementation:
*   **Stop/Close**: When playback stops, Kodi calls `CAMLCodec::CloseDecoder()`. This function is responsible for calling `codec_close()`, releasing packets, disabling the Dolby Vision driver via sysfs, and closing the Amlogic video device.
*   **Code Pointer**: [`xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp`](https://github.com/CoreELEC/xbmc/blob/3f9d273a374386d917b875e0f368305d8db145f6/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp)

### The "Black Screen" Bug
Amlogic's kernel driver (`vdec.c`) manages decoders in an active list. A decoder transitions between `VDEC_STATUS_CONNECTED` (ready to run) and `VDEC_STATUS_ACTIVE` (owning hardware resources).
*   **The Issue**: If Kodi encounters an error or a stream is switched too rapidly, Kodi may fail to properly close the decoder. The kernel driver leaves the decoder in the `ACTIVE` state, failing to release the hardware frame buffers.
*   **Result**: Subsequent video launches result in a black screen while the audio timeline continues, because the kernel is confused about stream ownership (e.g., Kodi feeds stream 1, but the kernel displays stream 2).
*   **Workaround**: Historically, this required a full system reboot, or toggling the hardware decoder off and on in Kodi settings to force a reset of the software stack. CoreELEC intentionally disables multi-decode support (even though the hardware supports it) to minimize these kernel state desynchronization issues.
*   **Code Pointer**: [`drivers/amlogic/amports/vdec.c`](https://github.com/osmc/vero3-linux/blob/master/drivers/amlogic/amports/vdec.c) (OSMC/Amlogic kernel tree).