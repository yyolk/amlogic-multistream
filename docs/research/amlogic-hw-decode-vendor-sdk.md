# Amlogic S922X hardware decode — vendor SDK, Buildroot, Android TV, and chip-manual references

Research notes for a hardware `DecoderBackend` and a real display path (see
`src/ffmpeg_decoder.cpp` `prepareAmlogicHwaccel` and `src/drm_renderer.cpp`).
Target hardware: Beelink GS-King X (S922X, G12B) running CoreELEC 21
(Amlogic-ng) or CoreELEC 22 (Amlogic-no).

Every link below is publicly readable (no NDA, no login) and was fetched or
otherwise verified on 2026-11-01. Each entry says what the source *actually
contains*, not what its name implies. Where a fact comes from reading source
code rather than prose, the file path inside the repo is given.

## TL;DR — usable facts

- S922X (G12B) video decode hardware per the public datasheet: AVE-10 engine,
  two decoder domains (VDEC1 + VDEC2) plus a dual-pipeline HEVC block
  (front/back). "Multi-video decoder up to 4Kx2K@60fps+1x1080P@60fps",
  multiple concurrent "secured" sessions. Per-codec ceilings: H.265
  MP-10@L5.1 4K60, VP9 Profile-2 4K60, AVS2 4K60, H.264 HP@L5.1 4K30,
  MPEG-1/2/4, VC-1/WMV, AVS+, MVC, MJPEG, RealVideo ≤1080p60.
- The vendor decode stack is fully public in source form through CoreELEC's
  mirrors: `CoreELEC/linux-amlogic` (vendor kernels 4.9 / 5.4 / 5.15 / 6.12),
  `CoreELEC/media_modules-aml` (decoder + amstream + amvideo drivers), and a
  prebuilt `libamcodec.so` + headers on `sources.coreelec.org`. The full
  Amlogic Buildroot SDK itself is account-gated; only an old (2018) G12B
  tarball and u-boot/Mali fragments are mirrored publicly.
- Vendor userspace decode API = `libamcodec` (`codec_init`, `codec_write`,
  `codec_checkin_pts_us64`, …) on top of `/dev/amstream_vbuf` and
  `AMSTREAM_IOC_*` ioctls. Kodi's `AMLCodec.cpp` in `CoreELEC/xbmc` is the
  reference consumer and dlopens `libamcodec.so` — no link-time dependency.
- Multi-instance decode: vendor `vdec.h` sets `MAX_INSTANCE_MUN 9`. CoreELEC
  21 (kernel 4.9) ships a patch capping it to 2 because Kodi can't drive more
  than one decoder; CoreELEC 22 (kernel 5.15) carries no patch, so 9
  instances are available. Multi-instance requires the frame-block mode the
  `multi_vdec`/`*_multi` decoders use (`ammvdec_h265`, provider names like
  `vdec.h265.00`), plus a per-instance VFM pipeline map.
- Display on the vendor kernel is not DRM/KMS: GUI/compositing is fbdev OSD
  (`/dev/fb0`), video is a separate hardware layer driven through
  `/dev/amvideo` + `/sys/class/video/*` (`axis`, `zoom`, `screen_mode`,
  `disable_video`, `blackout_policy`, …) and pipeline wiring via
  `/sys/class/vfm/map`. Kodi's `AMLCodec.cpp`/`AMLUtils.cpp` show every path.
  This directly affects `DrmRenderer` plans: `/dev/dri/card0` exists on
  CoreELEC but there is no KMS video-plane pipeline for decoded frames.
- The vendor media_modules tree also contains an optional V4L2 M2M decoder
  (`CONFIG_AMLOGIC_MEDIA_V4L_DEC`, device `aml-vcodec-dec`,
  `drivers/amvdec_ports/`, `drivers/frame_provider/decoder_v4l/`), but
  CoreELEC 22's kernel config does not enable it — stock CoreELEC has no
  vendor V4L2 node.
- Mainline (LibreELEC `AMLGX`) covers the GS-King X with
  `meson-g12b-gsking-x.dts`, DRM/KMS (`CONFIG_DRM_MESON`), and the staging
  `meson-vdec` stateful V4L2 M2M driver — but only MPEG-1/2, H.264, and VP9
  on G12B (HEVC crashes on G12A+ and is enabled for GXBB/GXL/GXM only).
  FFmpeg's generic `*_v4l2m2m` decoders (h264/hevc/vp9/mpeg1/2/4/h263/vc1/
  vp8) are the userspace side of that path.

## 1. Chip manuals / datasheets

### S922X Public Datasheet V0.2 (2019-03-10)

- https://dn.odroid.com/S922X/ODROID-N2/Datasheet/S922X_Public_Datasheet_V0.2.pdf
  (canonical, linked from the ODROID-N2 wiki hardware page)
- https://www.scs.stanford.edu/~zyedidia/docs/amlogic/s922x.pdf
  (byte-identical mirror)
- https://wiki.odroid.com/odroid-n2/hardware (board page that links it)

What it contains: ~100 pages. Sections: overview/features, pin-out and pin
multiplexing, electrical/operating conditions, and partial system registers
(clocks, power domains, boot). Header says "Confidential for Hardkernel!" —
this is the redacted "public" revision Hardkernel distributes; it is *not*
the full register manual. VDEC register-level documentation is absent except
clock/power control (`HHI_VDEC_CLK_CNTL`, `HHI_VDEC2_CLK_CNTL`,
`DOS_MEM_PD_*`, reset lines).

Usable facts:

- §2.1: "Amlogic Video Engine (AVE-10) offloads the Cortex-A53 CPUs from all
  video CODEC processing … capable of decoding 4Kx2K resolution video at
  75fps with complete Trusted Video Path (TVP)… supports … MVC, MPEG-1/2/4,
  VC-1/WMV, AVS, AVS+, AVS2 RealVideo, MJPEG streams, H.264, H265-10, VP9 and
  also JPEG pictures with no size limitation." Encoder: JPEG/H.265/H.264 up
  to 1080p60.
- §2.2 feature list: "Support multi-video decoder up to
  4Kx2K@60fps+1x1080P@60fps" and "Supports multiple 'secured' video decoding
  sessions and simultaneous decoding and encoding" — the marketing form of
  the hardware concurrency the vendor driver exposes as `MAX_INSTANCE_MUN 9`.
- Per-codec ceilings: VP9 Profile-2 4Kx2K@60; H.265 HEVC MP-10@L5.1 4Kx2K@60;
  AVS2-P2 4Kx2K@60; H.264 AVC HP@L5.1 4Kx2K@30; H.264 MVC 1080p60; MPEG-4 ASP
  1080p60; WMV/VC-1 1080p60; AVS+ 1080p60; MPEG-2 MP/HL 1080p60; MPEG-1
  1080p60.
- Display I/O: "HDMI2.1 transmitter with 3D, Dynamic HDR, CEC and HDCP 2.2",
  stereo DAC, CVBS, 4-lane MIPI DSI.
- Clock/power tables confirm two independent decoder domains: `VDEC1`,
  `VDEC2`, dual-lane `HEVC` (hevcf/hevcb), `HCODEC`, `HEVC Encoder` with
  separate power-switch and isolation bits — hardware-level evidence that
  concurrent decodes are a designed-for case.

What it does *not* contain: VDEC programming registers, firmware/u-code
interface, display-pipeline (VPP/OSD) registers. Those remain NDA-only; the
public substitute is reading `media_modules-aml` source (§3).

No separate public A311D datasheet was found; A311D is the same G12B die with
an NPU (linux-meson.com: "A311D: similar to S922X … with a Neural Processor
Unit"), so the S922X document plus G12B device trees cover both.

## 2. Amlogic vendor Linux SDK (Buildroot)

### OpenLinux portal

- http://openlinux.amlogic.com/mw/index.php?title=Main_Page
- http://openlinux.amlogic.com/mw/index.php?title=Linux_SDK_release

What it contains: a MediaWiki catalog of Amlogic SDK lines (RDK, Yocto-SDK,
Yocto-BSP, Zapper, SmartDisplay, Camera, Audio) with release versions per
chip. Main page states specs/SDKs require an Amlogic sales contact. The
release table lists current SoCs (S905X4/Y4, T962D4, A311D/A311D2, …); the
S922X-era Buildroot SDK (2018, kernel 4.9) is the predecessor documented by
the two items below rather than by this table.

### Amlogic Linux BSP Application Notes (public mirror)

- https://device.report/m/54a71dd31043e9b84ae8d8b08388c0e89e2e3ab4af84407177eaa19f57be8d5e.pdf

What it contains: Amlogic's "Openlinux notes" for the Linux BSP reference
release: kernel 4.9.76, `repo init -u
ssh://git@openlinux.amlogic.com/buildroot/platform/manifest.git -m
buildroot-openlinux-20180418.xml` (account-gated git), and a free tarball
download path of the form
`http://openlinux.amlogic.com:8000/download/ARM/filesystem/Linux_BSP/buildroot_openlinux_kernel_4.9_fbdev_20180418.tar.gz`.
Build flow: `. buildroot/build/setenv.sh` then pick a
`meson<family>_<board>_<bits>_release` config (G12A examples shown:
`mesong12a_u200_32_release`; G12B equivalents follow the same pattern), then
serial top-level `make` (Buildroot: no top-level `-jN`). Toolchain: Linaro
GCC 6.3.1 aarch64. The tarball has no git metadata.

### G12B Buildroot tarball (referenced by upstream u-boot docs)

- https://docs.u-boot.org/en/latest/board/amlogic/w400.html (W400 = Amlogic's
  S922X reference board; also `u200.html` for G12A)

What it contains: the canonical public pointer to the G12B SDK tarball
`http://openlinux2.amlogic.com:8000/ARM/filesystem/Linux_BSP/buildroot_openlinux_kernel_4.9_fbdev_20180706.tar.gz`,
used for the bootloader/FIP binaries (`bootloader/uboot-repo/fip`,
bl2/bl30/bl31, DDR firmware). Note "fbdev" in the tarball name: the vendor
4.9 userspace display stack is fbdev-based, matching §4's display findings.
The host did not answer from this network at research time (connection
failure, HTTP 000) — availability of openlinux{,2}.amlogic.com:8000 is
intermittent, so treat the tarball as obtainable-but-unreliable and prefer
the mirrors below.

### Vendor u-boot mirror (Buildroot SDK component)

- https://github.com/BayLibre/u-boot (`buildroot-openlinux-20180418` branch)

What it contains: Amlogic's SDK u-boot as a public git branch, with
`g12b_w400_v1_defconfig` and `board/amlogic/g12b_w400_v1/` — the same tree
the Buildroot SDK builds its bootloader from. Used by the u-boot docs above
for G12A/G12B manual signing. The corresponding mainline-side FIP helper is
`LibreELEC/amlogic-boot-fip` (referenced from the same docs).

### Other public fragments of the Buildroot SDK

- https://github.com/not-aml/openlinux-amlogic-uboot — unofficial mirror of
  u-boot tarballs from `openlinux.amlogic.com:8000/download/ARM` as orphan
  branches; README states the obvious: public sources are old, updated
  sources are behind Amlogic's SLA/NDA git server.
- https://github.com/hardkernel/buildroot_linux_amlogic_meson_mali —
  Hardkernel's import of the SDK's `meson_mali` Buildroot package; commit
  history includes "add g12b fbdev and drm mali so" with prebuilt
  `lib/arm64/gondul/r12p0/{fbdev,drm}/libMali.so` (gondul = G12B Mali-G52
  userspace, r12p0). Confirms two GPU userspace flavors ship in the SDK:
  fbdev and GBM/DRM.
- No complete public mirror of the G12B Buildroot rootfs was found. The
  practical public substitute for the SDK's media stack is CoreELEC's git
  mirrors (§3), which are the same sources Amlogic ships in the SDK.

## 3. Vendor decode stack, public via CoreELEC

CoreELEC redistributes the SDK's kernel, media drivers, and codec library as
public artifacts. These are the most usable references for a Linux userspace
decoder on the GS-King X, because CoreELEC 21 (Amlogic-ng, kernel 4.9.269)
and CoreELEC 22 (Amlogic-no, kernel 5.15) are exactly the deployment targets
of this repo (see README "Device AppImages").

### CoreELEC/linux-amlogic — vendor kernel

- https://github.com/CoreELEC/linux-amlogic

Branches (verified 2026-11-01): `amlogic-3.14-nougat`, `amlogic-4.9-20`
(G12A/G12B/SM1, used by CoreELEC ≤21), `amlogic-5.4.210` (Amlogic-ne),
`5.15.196_20260306` (default; Amlogic-no), `6.12.69_202606`.

What it contains: the full vendor kernel, including the GS-King X device
trees this repo's README references:

- 4.9: `arch/arm64/boot/dts/amlogic/g12b_s922x_beelink_gs_king_x.dts` (plus
  `_rev_a`, GT-King, Odroid N2/N2+, Ugoos AM6, Minix U22XJ, VIM3, …)
- 5.15: `arch/arm64/boot/dts/amlogic/meson-g12b-gsking-x.dts` and
  `meson-g12b-s922x.dtsi`

Decoder nodes are not per-board: the media modules self-register platform
devices and match SoC family via compatibles like
`amlogic,cpu-major-id-g12b-b` (see `drivers/…/utils/vdec.c`,
`vdec_module_init`, in media_modules-aml), so the dts mostly wires clocks,
power domains, and reserved memory.

### CoreELEC/media_modules-aml — vendor media drivers

- https://github.com/CoreELEC/media_modules-aml

Branches: `amlogic-4.9.269`, `amlogic-5.4.210`, `5.15.196_20260225`
(default), `6.12.69_202606`. Built as out-of-tree modules against the kernel
above (CoreELEC 9.2.2 release notes: "building the media modules as external
modules, as Amlogic intended").

What it contains (5.15 branch, directory layout):

- `drivers/stream_input/amports/` — the amstream char devices:
  `amstream_vbuf` (compressed video ES write), `amstream_vframe`,
  `amstream_abuf`, `amstream_mpts`, `amstream_hevc`, `amstream_sub`,
  `amstream_userdata`, dves variants. `register_chrdev(AMSTREAM_MAJOR,
  "amstream", …)`. This is the kernel end of libamcodec's `codec_write`.
- `drivers/frame_provider/decoder/` — classic stream/frame decoders:
  `h264_multi`, `h264_mvc`, `h265`, `h265_fb`, `vp9`, `vp9_fb`, `avs2`,
  `avs2_fb`, `avs3`, `avs_multi`, `h266`, `mjpeg`, `mpeg12`, `mpeg4`,
  `vav1`, `vav1_fb`, `vc1` — i.e. the per-format front ends the
  `CONFIG_AMLOGIC_MEDIA_VDEC_*` switches select (list below).
- `drivers/frame_provider/decoder_v4l/` + `drivers/amvdec_ports/` —
  Amlogic's V4L2 M2M decoder port: `aml_vcodec_dec_drv.c` registers a
  `VFL_DIR_M2M` video device named `aml-vcodec-dec`
  (`V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING`, DT compatible
  `amlogic, vcodec-dec`). Gated on `CONFIG_AMLOGIC_MEDIA_V4L_DEC`, which also
  selects the `decoder_v4l/` front ends.
- `drivers/frame_provider/decoder/utils/vdec.h` —
  `#define MAX_INSTANCE_MUN 9` on the 5.15 branch (and
  `VDEC_DATA_MAX_INSTANCE_NUM (MAX_INSTANCE_MUN * 2)`): the driver's own
  concurrency ceiling for parallel decoder instances.
- `drivers/frame_sink/`, `drivers/media_sync/`, `drivers/common/` — amvideo
  video sink, tsync A/V sync, media clock.

CoreELEC's build of it (build-system side):

- https://github.com/CoreELEC/CoreELEC/blob/coreelec-22/projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml/package.mk
  builds, as modules: MPEG2_MULTI, MPEG4_MULTI, VC1, H264_MULTI, H264_MVC,
  H265, H266, VP9, MJPEG_MULTI, AVS_MULTI, AVS2, AVS3, AV1, plus the *_FB
  (frame-buffer) variants of VP9/H265/AV1/AVS2, and HW_DEMUX. This is the
  authoritative list of codec modules present on a CoreELEC 22 box.
- https://github.com/CoreELEC/CoreELEC/blob/coreelec-21/projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml/patches/003-multi-decoders-limit-maximum-number-of-decoder.patch
  (also on coreelec-19): changes `MAX_INSTANCE_MUN` 9 → 2 with commit
  message "Kodi can only handle one decoder … Because of threading issues in
  Kodi it is needed to limit the multi decoder instances". So: CoreELEC 21
  kernel 4.9 allows 2 parallel instances; CoreELEC 22 kernel 5.15 keeps the
  upstream 9. A non-Kodi app (this repo) is exactly the "Kodi does implement
  multiple decoder support" case the patch author anticipated; on CE21 the
  cap is baked into the shipped driver.
- CoreELEC 22 kernel config
  (`projects/Amlogic-ce/devices/Amlogic-no/linux/linux.aarch64.conf`) has
  `CONFIG_AMLOGIC_MEDIA_MODULE=y`, `CONFIG_V4L2_MEM2MEM_DEV=y`, but no
  `CONFIG_AMLOGIC_MEDIA_V4L_DEC` — the vendor V4L2 decoder node is *not*
  present on stock CoreELEC 22. Decode userspace on CoreELEC is the amcodec
  path only, unless the kernel is rebuilt.

### libamcodec — vendor userspace codec library (prebuilt + headers)

- Package: https://github.com/CoreELEC/CoreELEC/blob/coreelec-22/projects/Amlogic-ce/packages/multimedia/libamcodec/package.mk
- Payload: `https://sources.coreelec.org/libamcodec-aarch64-<commit>.tar.xz`
  (fetched and inspected; ~24 KB)

What it contains: `usr/lib/libamcodec.so(.0.0)` (proprietary, site listed as
openlinux.amlogic.com — this is the SDK's amcodec, repackaged) and the public
headers under `usr/include/amcodec/`: `codec.h`, `codec_type.h`,
`codec_error.h`, `codec_info.h`, `codec_msg.h`, `codec_videoinfo.h`,
`audio_priv.h`, `amports/amstream.h`, `amports/vformat.h`,
`amports/aformat.h`, `amports/videosync.h`, `ppmgr/ppmgr.h`.

Usable facts:

- API model (from `codec.h` + Kodi's usage): fill `codec_para_t`
  (`vformat_t` from `vformat.h`, stream type), `codec_init()`, then push
  elementary stream with blocking `codec_write()` into `/dev/amstream_vbuf`;
  timestamps via `codec_checkin_pts_us64()`; status via
  `codec_get_vbuf_state()` / `codec_get_vdec_state()` /
  `codec_get_vdec_info()` (`AMSTREAM_IOC_VDECINFO`, returns
  `struct vdec_info`: decoder name, width/height, frame rate, bitrate,
  frame/error/drop counters). Control channel `codec_init_cntl()` +
  `codec_set_cntl_*` (av/sync thresholds).
- No source is distributed by CoreELEC; the C source (`amcodec/codec/
  codec_ctrl.c` etc.) lives in the NDA SDK. The ABI is fully described by the
  headers above, and Kodi (below) is a complete consumer to copy behavior
  from. Historical source context: xbmc PR #13081 notes libamcodec "is
  available in Amlogic buildroot".
  (https://github.com/xbmc/xbmc/pull/13081)

### Kodi (CoreELEC fork) — reference userspace for decode *and* display

- https://github.com/CoreELEC/xbmc
  - `xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp`
  - `xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.cpp`
  - `xbmc/utils/AMLUtils.cpp`

What it contains: the only maintained open-source consumer of the whole
vendor path. `AMLCodec.cpp` declares `DllLibAmCodec` with
`DECLARE_DLL_WRAPPER(DllLibAmCodec, "libamcodec.so")` — the library is
dlopened at runtime, so a separate player binary can do the same without
link-time coupling (relevant to AppImage distribution).

Decode-side facts visible in the source: per-format setup in
`DVDVideoCodecAmlogic.cpp` (`am-h264`, `am-h265`, `am-vp9`, …), bitstream
conversion to Annex-B before feeding (`h264_mp4toannexb`-equivalent),
`/sys/class/amstream/vcodec_profile` for live decoder info.

Display-side facts (the sysfs surface a non-Kodi player must drive):

- `/dev/amvideo` — opened directly by `AMLUtils.cpp`; the video layer device.
- `/dev/video10` — the "amlvideo2" V4L-ish metadata device
  (`CAMLCodec::OpenAmlVideo`).
- `/dev/amvecm` — video enhancement/color manager (brightness/contrast via
  `/sys/class/amvecm/*1`).
- `/sys/class/vfm/map` — pipeline wiring: multi-instance decoders get a
  kernel-created map; the CoreELEC forum thread below shows a broken default
  map fixed by
  `echo "add default vdec.h265.00 ppmgr amvideo" > /sys/class/vfm/map`.
- `/sys/class/video/axis`, `screen_mode`, `zoom`, `disable_video`,
  `blackout_policy`, `saturation`, `freerun_mode` — video layer placement
  and behavior (this is how the video window is positioned under/over the
  fbdev OSD GUI).
- `/sys/class/tsync/enable`, `/sys/class/tsync/videostarted` — A/V sync
  engine; `/sys/class/ppmgr/ppscaler` — post-processing scaler;
  `/sys/module/amvdec_h264/parameters/dec_control` — per-decoder knobs;
  `/sys/class/amdolby_vision/*` and `/sys/module/amdolby_vision/*` — DV.
- OSD side is fbdev (`/dev/fb0`); the vendor 4.9 stack has no KMS video
  planes. (Hardkernel's SDK Mali package ships both fbdev and drm Mali
  userspaces; CoreELEC runs fbdev.)

### CoreELEC platform docs (kernel line ↔ SoC ↔ status)

- https://wiki.coreelec.org/coreelec:ce_dev_cycle and
  https://wiki.coreelec.org/coreelec:devgeneric

What they contain: the authoritative flavor matrix. Amlogic-ng = vendor
kernel 4.9.269 (CoreELEC ≤ 21.3, covers G12B = A311D/S922X; now EOL);
Amlogic-ne = 5.4.210 (SC2/T7/S4/S5 — not S922X); Amlogic-no = 5.15
(5.15.170 → 5.15.196, CoreELEC 22 "Piers", active; G12A through S7 including
S922X; drops GXL). Confirms the GS-King X runs either CE21 (4.9) or CE22
(5.15), matching this repo's two device AppImage targets.

### CoreELEC forum — multi_vdec / frame-block mode (practical detail)

- https://discourse.coreelec.org/t/experiments-with-hw-decoding/16349

What it contains: a user comparing Kodi vs moonlight-embedded dmesg logs on
CoreELEC. Established: the `multi_vdec` parameter switches a decoder from
stream-parser mode (`VDEC_TYPE_STREAM_PARSER`) to frame-block mode
(`VDEC_TYPE_FRAME_BLOCK`, device names like `ammvdec_h265`); Amlogic's multi
decoder supports up to 9 parallel streams; CoreELEC limited it for Kodi;
frame-block mode needs a correct per-instance VFM map or the picture is
blank; Kodi "sends frames to the hardware decoder, not a stream" — i.e. the
app must parse container → frames itself (libavformat does this for us) and
feed complete frames.

### CoreELEC 9.2.2 release notes (vendor stack provenance)

- https://github.com/CoreELEC/CoreELEC/releases/tag/9.2.2

What it contains: states the -ng kernel is "based on the latest Amlogic
Android BSP", that media modules moved to external builds, and that Kodi was
fixed to use "the Amlogic multi decoder u-code" (single-decoder u-code
abandoned by Amlogic). Evidence that the Linux vendor kernel + media stack
is derivative of the Android TV BSP (see §4) and that multi-decoder firmware
is the only maintained path.

## 4. Android TV / Android references

Amlogic's Android TV SDK proper is NDA-only, but the same G12B stack is
publicly visible through Khadas' VIM3 (A311D) release and Google's AOSP
yukawa device.

### Khadas VIM3 (A311D) Android source — public Android 9 SDK tree

- https://docs.khadas.com/products/sbc/vim3/development/android/download-android-source-code
- https://github.com/khadas/android_manifest

What it contains: repo-manifest instructions for Khadas' Amlogic-SDK-derived
Android trees: `repo init -u https://github.com/khadas/android_manifest.git
-b khadas-vim3-p-64bit` (Android 9 64-bit, "main maintenance version";
`khadas-vim3-r-32bit` for Android 11, `khadas-vims-pie` for Android 9
32-bit), with git-lfs payload. This is the closest public artifact to the
"Android TV SDK for S922X-class silicon": same G12B family, Amlogic 4.9
kernel, vendor media modules, and the Android media stack on top
(OMX/`media_codecs` wiring, SurfaceFlinger → vendor hwcomposer → OSD/video
layers, the Android-side equivalent of the amvideo pipeline in §3).

### AOSP upstream: device/amlogic/yukawa (VIM3 = Android TV by default)

- https://aospandaaos.github.io/device-vim3.html (build walkthrough)
- `device/amlogic/yukawa` in AOSP; background wiki at
  https://gitlab.baylibre.com/baylibre/amlogic/atv/aosp/device/amlogic/yukawa/-/wikis/home

What it contains: Google's reference Amlogic device, supporting `sei610`,
VIM3 and VIM3L; `m TARGET_VIM3=true` builds VIM3 **Android TV** (tablet
launcher is the opt-in alternative). Prebuilt kernels selectable 4.19–6.1
(`TARGET_KERNEL_USE`), i.e. mainline-family kernels rather than the vendor
4.9. Demonstrates the other Android decode/display arrangement:
mainline-ish kernel + meson drivers underneath Android's media framework.
Useful mainly as proof that G12B Android TV works without the vendor 4.9
stack — not as a source of vendor codec documentation.

### What to take from the Android side

- The vendor Linux kernel and media modules CoreELEC ships are derived from
  the Android BSP (CoreELEC 9.2.2 notes), so Android-side behavior (decoder
  u-code, VFM pipelines, OSD/video layering) is the design baseline for the
  Linux vendor stack.
- No public S922X Android *TV* product tree was found (Beelink/Ugoos ship
  binaries only). Khadas' manifest + AOSP yukawa bracket the problem from the
  vendor and mainline ends.

## 5. Mainline Linux path (reference, not the CoreELEC target)

### meson-vdec (staging) — stateful V4L2 M2M decoder

- Binding: https://www.kernel.org/doc/Documentation/devicetree/bindings/media/amlogic%2Cgx-vdec.yaml
- Driver: `drivers/staging/media/meson/vdec/` (e.g.
  https://gbmc.googlesource.com/linux/+/34e1a5d43c5deec563b94f3330b690dde9d1de53/drivers/staging/media/meson/vdec/vdec_platform.c)
- HEVC series: https://lkml.iu.edu/hypermail/linux/kernel/2303.1/08881.html

What they contain: the upstream driver for the same hardware (ESPARSER →
VIFIFO → VDEC_1 / VDEC_HEVC). Binding lists compatibles `amlogic,gxbb-vdec`
… `amlogic,gxm-vdec`, `amlogic,g12a-vdec`, `amlogic,sm1-vdec` — G12B boards
reuse `amlogic,g12a-vdec` (linux-meson.com: "G12B: Identical to G12A").
Notably, VDEC_1 and VDEC_HEVC share one IRQ and "cannot run concurrently" in
this driver — a driver-level serialization that does not exist in the vendor
multi-instance design. `vdec_platform.c` enumerates per-SoC formats:
VP9 and H.264 up to 3840x2160, MPEG-1/2 up to 1920x1080, capture formats
`NV12M` (and `YUV420M` for MPEG-1/2), firmware blobs `meson/vdec/*.bin`
(e.g. `g12a_h264.bin`, `g12a_vp9.bin`). HEVC was added (2023 staging series)
but enabled for GXBB/GXL/GXM only: "decoding currently crashes on G12A and
later SoCs when trying to decode a 10-bit HEVC stream" — so on S922X
mainline decodes H.264/VP9/MPEG-1/2 only, no HEVC.

### FFmpeg userspace for V4L2 M2M

- https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/v4l2_m2m_dec.c

What it contains: the generic stateful V4L2 M2M hwaccel-free decoders
registered as `h264_v4l2m2m`, `hevc_v4l2m2m`, `mpeg1/2/4_v4l2m2m`,
`h263_v4l2m2m`, `vc1_v4l2m2m`, `vp8/vp9_v4l2m2m` (with
`*_mp4toannexb` bitstream filters auto-attached for h264/hevc). This is the
practical FFmpeg-side API for meson-vdec (and, if ever enabled, for the
vendor `aml-vcodec-dec` node — same V4L2 M2M shape). The HEVC staging series
notes conformance work paired against the Raspberry Pi Foundation's FFmpeg
fork (`jc-kynesim/rpi-ffmpeg`), whose v4l2_m2m/v4l2_request improvements are
being upstreamed.

### linux-meson.com hardware page

- https://linux-meson.com/hardware.html

What it contains: community status matrix: "HDMI Output up to 4k is working
on … S922X, A311D …"; video decoding "in Staging … supports MPEG-2, H264 and
VP9"; SoC family relationships (G12B ≡ G12A + A73/A53 big.LITTLE; S922X
mostly pin-compatible with S905X2; A311D = S922X + NPU).

### LibreELEC AMLGX (mainline distro covering GS-King X)

- https://wiki.libreelec.tv/hardware/amlogic

What it contains: AMLGX supports GXBB/GXL/GXM/G12A/**G12B (S922X/A311D)**/SM1
with modern kernels; "box" images boot via vendor u-boot recovery with a
user-selected dtb (e.g. `meson-g12b-gtking.dtb`), "board" images use mainline
u-boot. Upstream kernel has `arch/arm64/boot/dts/amlogic/meson-g12b-gsking-x.dts`
(verified present in torvalds/linux). This is the production proof of the
mainline path on our exact box: DRM/KMS display (`CONFIG_DRM_MESON`) +
meson-vdec + Kodi DRMPrime/GBM.

## 6. Synthesis for this repo

Decode API options for a hardware `DecoderBackend`, in order of fit:

1. **libamcodec / amstream (vendor, CoreELEC 21 + 22).** Public headers via
   the CoreELEC sources tarball; dlopen `libamcodec.so` like Kodi does (no
   link-time coupling, works from an AppImage). We parse/demux with
   libavformat (already in tree) and push complete frames with
   `codec_write()` + PTS check-in; the kernel decodes and can present
   directly on the amvideo layer. Concurrency: 2 instances on CE21 (patched
   `MAX_INSTANCE_MUN`), 9 on CE22; needs frame-block (`multi_vdec`) mode per
   stream and a per-instance VFM map (`vdec.<fmt>.NN → ppmgr → amvideo`).
   H.264/H.265/VP9 all supported — matches HLS content best.
2. **FFmpeg `*_v4l2m2m` against the vendor `aml-vcodec-dec` node.** Cleanest
   userspace shape (same as mainline), but `CONFIG_AMLOGIC_MEDIA_V4L_DEC` is
   off in CoreELEC 22's kernel config — requires a custom kernel, so not a
   first target.
3. **Mainline meson-vdec + FFmpeg `*_v4l2m2m` (LibreELEC-style).** Real
   DRM/KMS display (fits `DrmRenderer`'s `/dev/dri/card0` plan) and fully
   open stack, but: HEVC unavailable on G12B (10-bit crash), staging driver
   serializes VDEC_1/VDEC_HEVC, multi-instance scaling unproven, and it
   implies shipping/supporting a mainline image instead of CoreELEC.

Display options:

- **Vendor kernel (CoreELEC):** no DRM/KMS video path. GUI = fbdev `/dev/fb0`
  (or Mali fbdev GLES), video = amvideo layer positioned via
  `/sys/class/video/axis` + `zoom` + `screen_mode`, layered against the OSD,
  with `/sys/class/vfm/map` selecting the pipeline. Kodi's `AMLCodec.cpp` is
  the working example. A grid of N streams = N VFM pipelines into one amvideo
  layer region each (or per-layer use is single-video — needs on-device
  verification; the datasheet's "4K+1080p" and the driver's 9-instance limit
  suggest multiple video planes is the intended multi-display mode on this
  silicon, but the amvideo layer count on G12B must be confirmed on the box).
- **Mainline kernel:** DRM/KMS meson with video planes; decoded `NV12M`
  buffers go V4L2 → dmabuf → atomic commit. This is the only path where the
  current `DrmRenderer` sketch applies verbatim.

Open questions to verify on a GS-King X (device checklist):

- Concurrent instance count actually achievable for 1080p H.264 on CE21
  (driver cap 2) vs CE22 (cap 9), and per-instance VFM map behavior when
  Kodi is not running (the forum's blank-output map fix was with Kodi
  stopped).
- amvideo layer count/plane semantics for a multi-cell grid, and whether
  `/sys/class/video/axis` per-layer or a single layer with per-window
  positioning applies to N pipelines.
- Whether `libamcodec.so` from the CE22 tarball runs on CE21's 32-bit
  (armv7) userspace — the packaged tarball is `aarch64`; CE21 Amlogic-ng is
  32-bit ARM, so an armhf variant must be sourced (CoreELEC 21 sysroot or
  built from the CE21 package).
- Latency/back-pressure behavior of blocking `codec_write()` per stream and
  abort/reset semantics (`codec_reset`, `codec_close`) needed by this repo's
  "stop/restart unblocks within 2s" guarantees.

*Method note: facts marked with file paths were read directly in the linked
repos on 2026-11-01 (branch names recorded above). The datasheet is the
public V0.2 revision; anything marked "Confidential for Hardkernel" beyond it
remains NDA material and was not used.*
