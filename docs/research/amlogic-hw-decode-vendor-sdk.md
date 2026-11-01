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
- Vendor V4L2 M2M (`amvdec_ports`, `/dev/video26`, name `aml-vcodec-dec`) is
  **not a stock CoreELEC decode path we can rely on**. CE21 ships and
  autoloads the `.ko` (DT node present on G12B / GS-King X) but the 32-bit
  userspace compat ioctl wrapper skips `v4l2_compat_ioctl32`. CE22's load
  script still `modprobe -q amvdec_ports`, but the module is **not built**
  (`CONFIG_AMLOGIC_MEDIA_V4L_DEC` unset). Verdict in [§7](#7-path-3-vendor-v4l2--amvdec_ports--stock-coreelec-gono-go):
  **no-go for direct vendor V4L2; must use libamcodec on stock CoreELEC.**
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
2. **FFmpeg `*_v4l2m2m` against vendor `aml-vcodec-dec` (`/dev/video26`).**
   **No-go on stock CoreELEC** — see [§7](#7-path-3-vendor-v4l2--amvdec_ports--stock-coreelec-gono-go).
   CE22 does not package the module. CE21 packages it but 32-bit compat is
   broken-by-construction for standard V4L2 structs. Only becomes interesting
   with a rebuilt `media_modules-aml` (`CONFIG_AMLOGIC_MEDIA_V4L_DEC=m`) and
   64-bit userspace; even then frames come back as NV12/NV21, not amvideo
   HDMI scanout.
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

## 7. Path 3 — vendor V4L2 / `amvdec_ports` (stock CoreELEC go/no-go)

Pinned trees this section was read from (2026-11-01):

| Line | Tree | Pin |
| --- | --- | --- |
| CE21 media_modules-aml | `CoreELEC/media_modules-aml` | `60561618f2bf154c1438d3c790b625ff01f215c3` (`package.mk` on `coreelec-21`) |
| CE21 kernel | `CoreELEC/linux-amlogic` | `50c8e8154c5aba3c7cd8438d04dd5fdd51733e41` (`amlogic-4.9-20`) |
| CE21 instance-cap patch | `CoreELEC/CoreELEC` `coreelec-21` | `projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml/patches/003-multi-decoders-limit-maximum-number-of-decoder.patch` |
| CE22 media_modules-aml | `CoreELEC/media_modules-aml` | `c15aebb1ac93028d0aceadf8f35dda2204d33424` (`package.mk` on `coreelec-22`) |
| CE22 kernel | `CoreELEC/linux-amlogic` | `44cbccdee100b6a388814b9121b3b7629ab94382` (`5.15.196_20260306`) |
| CE22 in-kernel media (amlvideo) | `CoreELEC/common_drivers` | `8ca9b6c3b7152fa2959147a403fc5e7e697d78d2` |

File:line citations below are against those pins, not `HEAD`. Facts that still need a GS-King X are tagged ***verify***.

### 7.1 Verdict

**No-go: do not implement a hardware `DecoderBackend` that talks vendor V4L2 M2M (`/dev/video26`, `aml-vcodec-dec`) on stock CoreELEC 21 or 22.**

**Go: libamcodec / `/dev/amstream_*` remains the only stock-CoreELEC userspace decode API** that (a) is packaged on both CE21 armv7 and CE22 aarch64, (b) is what Kodi actually drives, and (c) can feed the amvideo HDMI layer without a custom kernel.

`amvdec_ports` is a real Amlogic Android/Buildroot M2M port sitting on the same `vdec_create` / `MAX_INSTANCE_MUN` core as libamcodec. It is **not** equivalently productized on CoreELEC.

### 7.2 Registration — which `/dev/videoN`, formats, memory

One M2M video node, hard-coded minor **26**:

- CE21 probe: `video_register_device(vfd_dec, VFL_TYPE_GRABBER, 26)` → log
  `"decoder registered as /dev/video%d"`
  (`aml_vcodec_dec_drv.c:490`, `:503–504`). Name `AML_VCODEC_DEC_NAME`
  `"aml-vcodec-dec"` (`aml_vcodec_drv.h:33`). Caps
  `V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING`, `vfl_dir = VFL_DIR_M2M`
  (`aml_vcodec_dec_drv.c:460–462`). DT compatible `"amlogic, vcodec-dec"`
  (`aml_vcodec_dec_drv.c:515–516`).
- CE22 (source only; see §7.4): same minor 26
  (`aml_vcodec_dec_drv.c:672–675`, `VFL_TYPE_GRABBER` if `< 5.15` else
  `VFL_TYPE_VIDEO`). Same compatible (`:751–752`).

QUERYCAP driver string is `aml-vcodec-dec` (`aml_vcodec_dec.c:1446` CE21).

**OUTPUT (bitstream) fourccs** (CE21 `aml_video_formats[]`,
`aml_vcodec_dec.c:63–129`): `H264`, `HEVC`, `VP9`, `MPEG1`, `MPEG2`,
`MPEG4`, `MJPEG`, `AV1`. CE22 adds `AVS`, `AVS2`, `AVS3`, `VC1_ANNEX_{L,G}`,
`H266` (`aml_vcodec_dec.c:139–217`).

**CAPTURE (decoded) fourccs** (CE21): `NV21`, `NV21M`, `NV12`, `NV12M`,
`YUV420`. CE22 also `YUV420M`. Default capture is single-plane unless module
param `multiplanar=1` (`aml_vcodec_dec_drv.c:615–617` CE21; default `false`
→ `dst_vq->type` is `VIDEO_CAPTURE` not `_MPLANE`,
`aml_vcodec_dec.c:2697–2698`).

Frame-size enum default is 64×64 .. 2048×1088 (`AML_VDEC_MAX_W/H` in
`aml_vcodec_dec.h:32–33`); `enum_framesizes` lifts that to 4096×2304 when
`VCODEC_CAPABILITY_4K_DISABLED` is clear (`aml_vcodec_dec.c:1810–1816`).
`dec_capability` is initialized to `0` (`:1318`), so 4K is advertised.
***verify*** whether a 4K stream actually configures the G12B HEVC block
through this port.

**Memory / PRIME:**

| Queue | CE21 (`aml_vcodec_dec.c:2682–2703`) | CE22 (`:5816–5837`) |
| --- | --- | --- |
| OUTPUT | `VB2_DMABUF \| VB2_MMAP`; `vb2_dma_contig_memops` | `VB2_DMABUF \| VB2_MMAP \| VB2_USERPTR`; `vb2_dma_sg_memops` |
| CAPTURE | `VB2_DMABUF \| VB2_MMAP \| VB2_USERPTR`; contig | same io_modes; contig |

`VIDIOC_EXPBUF` is wired (`vidioc_vdec_expbuf` → `v4l2_m2m_ioctl_expbuf`,
CE21 `:1281–1289`, ioctl table `:2634`). That is the PRIME export path:
`REQBUFS(MMAP)` + `EXPBUF` → dmabuf fd. OUTPUT can also be queued as
`MEMORY_DMABUF` (prepare short-circuits for DMABUF output,
`:2038–2041`). Vendor "drm mode" is a custom write-only ctrl
`AML_V4L2_SET_DRMMODE = V4L2_CID_USER_BASE+0x1100`
(`aml_vcodec_dec.c:50–51`, `:2474–2479`) — Android TVP, not GBM/DRM KMS.

### 7.3 Instance accounting — 9 vs 2, global, `-EBUSY`

The V4L2 `open()` path does **not** take a hardware instance. It allocates a
`aml_vcodec_ctx`, bumps `dev->id_counter`, and creates an m2m context
(`aml_vcodec_dec_drv.c:54–129` CE21). No `MAX_INSTANCE_MUN` check, no
`-EBUSY` on the Nth `open()`.

The hardware slot is taken at **`VIDIOC_S_FMT` on OUTPUT** while
`ctx->state == AML_STATE_IDLE`:

1. `vidioc_vdec_s_fmt` → `vdec_if_init(ctx, fourcc)` (`aml_vcodec_dec.c:1766–1775`).
   Failure is returned to userspace as **`-EINVAL`**, not the inner errno.
2. `vdec_if_init` picks the per-codec `dec_if` and calls `dec_if->init`
   (`vdec_drv_if.c:36–70`).
3. e.g. `vdec_h264_init` → `video_decoder_init` (`decoder/vdec_h264_if.c:296–328`)
   → `vdec_ports_init` (`aml_vcodec_adapt.c:482–497`).
4. `vdec_ports_init` calls `vdec_create(&ada_ctx->port, NULL)`
   (`aml_vcodec_adapt.c:444–446`). `NULL` → return `-1`.
5. `vdec_create` allocates a **global** ida id in `[0, MAX_INSTANCE_MUN)`
   (`vdec.c:1141–1146` CE21; `:1808–1809` CE22). Exhaustion is
   `ida_simple_get` → **`-ENOSPC`** (kernel ida), logged
   `"vdec_create request id failed!"`, function returns `NULL`. **Not
   per-codec.** HEVC and H.264 share the same 2 / 9 slots.
6. After create, `video_decoder_init` / `vdec_init` can still return
   **`-EBUSY`**:
   - `is_res_locked(...)` at `vdec_init` entry (`vdec.c:2401–2404` CE21).
   - `"vdec_init vfm path busy."` when the single stream-parser VFM slot is
     taken (`vdec.c:2427–2432` CE21; `:3478–3479` CE22).
   - 2s wait timeout in `vdec_resource_checking` (`vdec.c:2376–2377`).

Those `-EBUSY` values propagate out of `video_decoder_init` but are then
collapsed to **`-EINVAL` at `S_FMT`**. Userspace should treat `S_FMT`
failure as "no slot / init failed", not look for `EBUSY`.

`MAX_INSTANCE_MUN` itself:

- Upstream trees (both pins): `#define MAX_INSTANCE_MUN  9`
  (CE21 `vdec.h:51`, CE22 `vdec.h:70`).
- **CE21 stock image applies the CoreELEC patch**
  `003-multi-decoders-limit-maximum-number-of-decoder.patch`:
  `#define MAX_INSTANCE_MUN  2` with message "Kodi can only handle one
  decoder … limit the multi decoder instances". **Shipped CE21 cap = 2.**
- **CE22 `package.mk` has no such patch.** Shipped CE22 cap = 9.

`amvdec_ports` forces frame-block mode (`vdec->type = VDEC_TYPE_FRAME_BLOCK`,
`PORT_TYPE_FRAME`, `aml_vcodec_adapt.c:408–409`) — the same multi-instance
mode `multi_vdec` selects on the libamcodec path. Default VFM path is
`FRAME_BASE_PATH_V4L_OSD` (`:410`): the created map is
`provider → vf_receiver_name` only (`vdec.c:2623–2628`), **not**
`… ppmgr deinterlace amvideo`. Decoded frames stay in the V4L2 CAPTURE
queue for userspace. HDMI scanout is not automatic.

Module params `aml_set_vfm_enable` / `aml_set_vfm_path`
(`aml_vcodec_dec_drv.c:591–597`) can override the path to the amvideo
chain. Default is off. ***verify*** a non-default path actually lights HDMI
from `/dev/video26` without Kodi.

### 7.4 Is `amvdec_ports` packaged into CE21 / CE22 GS-King X kernels?

**CE21 Amlogic-ng (GS-King X): packaged and autoloaded.**

- Kbuild: `ifdef CONFIG_V4L2_MEM2MEM_DEV` / `obj-y += amvdec_ports/`
  (`drivers/Makefile:5–7`). CE21 `linux.aarch64.conf` has
  `CONFIG_V4L2_MEM2MEM_DEV=m`.
- `amvdec_ports/Makefile`: `obj-m += amvdec_ports.o` — always a `.ko` when
  the directory is built. `package.mk` `makeinstall_target` copies every
  `*.ko`.
- Autoload: `modules-load.d/media_modules.conf` lists `amvdec_ports`
  (between `amvdec_mpeg4` and `amvdec_real`).
- DT on this box: `g12b_s922x_beelink_gs_king_x.dts` →
  `g12b_s922x_beelink_gt_king.dts` → `coreelec_g12b.dtsi` →
  `g12b_a311d_w400.dts` `#include "mesong12b.dtsi"`. That dtsi has

  ```
  vcodec_dec {
      compatible = "amlogic, vcodec-dec";
      dev_name = "aml-vcodec-dec";
      status = "okay";
  };
  ```

  (`mesong12b.dtsi:1459–1463`). Probe should bind. ***verify***
  `ls /dev/video26` and `dmesg | grep aml-vcodec` on a CE21 GS-King X
  (node number is requested 26; if taken, V4L2 assigns the next free).

**CE22 Amlogic-no (GS-King X): source present, module not built.**

- Kbuild: `obj-$(CONFIG_AMLOGIC_MEDIA_V4L_DEC) += amvdec_ports/`
  (`drivers/Makefile:7`) and `decoder_v4l/` the same flag.
- `package.mk` `make_target` sets `CONFIG_AMLOGIC_MEDIA_VDEC_*=m` for the
  classic decoders only. **It does not pass `CONFIG_AMLOGIC_MEDIA_V4L_DEC=m`.**
- `devices/Amlogic-no/linux/linux.aarch64.conf` has
  `CONFIG_AMLOGIC_MEDIA_MODULE=y` and `CONFIG_V4L2_MEM2MEM_DEV=y` but **no
  `CONFIG_AMLOGIC_MEDIA_V4L_DEC`**.
- The boot script still runs `modprobe -q amvdec_ports` first
  (`scripts/media_modules-aml.sh`). `-q` swallows "module not found".
  ***verify*** `modprobe amvdec_ports; echo $?` and `ls /dev/video26` on
  CE22 — expected: module absent, no node.

So: the port exists in the Android TV / Buildroot / vendor 5.15 *source*
tree CoreELEC redistributes, and CE21 happens to ship the 4.9 `.ko`. CE22
stock images do **not** ship it. This is not "Android-only", but it is
"not a CE22 product surface."

### 7.5 Ioctl timeline — one session and N parallel

One session (stateful M2M, Android-shaped):

1. `open("/dev/video26")` → `fops_vcodec_open`. New ctx, no HW yet.
2. `VIDIOC_QUERYCAP` / `ENUM_FMT` / `ENUM_FRAMESIZES` / `SUBSCRIBE_EVENT`
   (`V4L2_EVENT_SOURCE_CHANGE`, `V4L2_EVENT_EOS`).
3. Optional `VIDIOC_S_CTRL` `AML_V4L2_SET_DRMMODE` (skip unless TVP).
4. `VIDIOC_S_FMT` OUTPUT (`H264`/`HEVC`/`VP9`/…) → **`vdec_if_init` /
   `vdec_create` / `vdec_init`**. This is the slot-taking ioctl. Failure:
   `-EINVAL` (cap / init), possibly after an inner `-EBUSY` / `-ENOSPC`.
5. `VIDIOC_REQBUFS` OUTPUT (`MMAP` or `DMABUF`), `QUERYBUF`+`mmap` or
   import fds, `QBUF` a few Annex-B access units (64-byte DMA align
   required, `vdec_drv_if.c:88–93`).
6. `VIDIOC_STREAMON` OUTPUT then CAPTURE — `vidioc_decoder_streamon`
   (`aml_vcodec_dec.c:1179–1213`) only clears stream-off flags / maybe
   `aml_vdec_reset` on capture re-on, then `v4l2_m2m_ioctl_streamon`.
   **No second instance check at STREAMON.**
7. `DQBUF` OUTPUT (recycle), wait `SOURCE_CHANGE`, `G_CTRL`
   `MIN_BUFFERS_FOR_CAPTURE`, `S_FMT` CAPTURE, `REQBUFS` CAPTURE,
   `QBUF` CAPTURE, more `QBUF` OUTPUT, `DQBUF` CAPTURE → NV12/NV21.
8. Drain: `VIDIOC_DECODER_CMD` `V4L2_DEC_CMD_STOP` (`:1097–1131`).
9. `STREAMOFF` / `close` → `fops_vcodec_release` → `vdec_release` frees
   the ida slot.

N parallel sessions:

- N `open()`s of the **same** `/dev/video26` (standard V4L2 m2m). Each fh
  is one ctx. `open` always succeeds (until `kzalloc` fails).
- The Nth **`S_FMT` OUTPUT** is what hits the global cap (2 on CE21, 9 on
  CE22). Earlier sessions already `STREAMON`'d are unaffected.
- Second-session `STREAMON` does not serialize against the first; if both
  `S_FMT`s succeeded, both stream. ***verify*** real 1080p×N on G12B —
  datasheet "4K60+1080p60" is a throughput claim, not a V4L2 slot claim.
- Capture default is single-planar `NV21`/`NV12` unless `multiplanar=1`.
  FFmpeg `*_v4l2m2m` prefers `_MPLANE`; ***verify*** it enumerates the
  non-mplane CAPTURE queue.

### 7.6 Relation to `amlvideo` `/dev/video10` (Kodi + libamcodec)

`/dev/video10` is **not** `amvdec_ports`. It is the capture-only VFM tap
`amlvideo`:

- CE21 kernel `amlvideo.c:90–96,961–970`: `video_nr_base = 10`;
  `n_devs = 2` when `CONFIG_AMLOGIC_MEDIA_MULTI_DEC` (set on CE21).
  Instance 0 registers at **10**, instance 1 at `video_nr_base_second = 23`.
  Caps: `V4L2_CAP_VIDEO_CAPTURE` (+ `STREAMING`/`READWRITE` in
  `capabilities`, `:466–470`). No M2M, no OUTPUT queue, no bitstream.
- CE22 `common_drivers` `amlvideo.c:78–86,968–977`: same numbering.
  `device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING` (`:454`,
  `:966`).
- `amlvideo2` is a *different* grabber at `video_nr = 11`
  (CE21 `amlvideo2.c:124,6543`).

Kodi `CAMLCodec::OpenAmlVideo` opens `/dev/video10` `O_RDONLY|O_NONBLOCK`
after libamcodec is already feeding `/dev/amstream_*`
(`CoreELEC/xbmc` `AMLCodec.cpp` ~2205–2212). Purpose: pull already-decoded
vframes out of the VFM chain (`amlvideo` is a VFM receiver/provider named
`"amlvideo"`) for stills / GUI, while HDMI continues through `amvideo`.
Opening `/dev/video10` does **not** start a decoder. It is useless as a
`DecoderBackend`.

`/dev/video26` (M2M in) and `/dev/video10` (VFM capture out) can coexist;
they are opposite ends of the pipeline. libamcodec never opens 26.

### 7.7 What a userspace `DecoderBackend` would open — and what breaks if absent

If we skipped libamcodec and spoke V4L2:

```
open("/dev/video26")                         # aml-vcodec-dec M2M
# not /dev/video10 (amlvideo capture)
# not /dev/amstream_vbuf (libamcodec)
VIDIOC_S_FMT(OUTPUT, H264/HEVC/VP9, mplane)
VIDIOC_REQBUFS + QBUF bitstream (MMAP or DMABUF)
VIDIOC_STREAMON both directions
DQBUF CAPTURE → NV12/NV21 (EXPBUF/PRIME optional)
# then we still have to blit/scanout ourselves
```

FFmpeg would look for `h264_v4l2m2m` / `hevc_v4l2m2m` / `vp9_v4l2m2m` on
that node. The vendor test stub `amvdec_ports/test/vcodec_m2m_test.c` is
libavcodec-based and does **not** show the ioctl sequence (not a usable
cookbook).

**If the module is absent (stock CE22, or CE21 if probe failed):**

| Call | Result |
| --- | --- |
| `open("/dev/video26")` | `-ENOENT` |
| FFmpeg `*_v4l2m2m` | no device / "Unable to open a context" |
| `modprobe amvdec_ports` | CE22: module not found (script already `-q`s this). CE21: module exists |
| libamcodec `codec_init` / `/dev/amstream_vbuf` | **unaffected** — different driver (`stream_input/amports`) |
| `/dev/video10` amlvideo | **unaffected** — lives in the kernel `video_processor` tree |
| HDMI via `/sys/class/video/*` | **unaffected** |

### 7.8 32-bit compat (CE21 armv7 userspace on aarch64 kernel)

This repo's CE21 AppImage is `coreelec21-ng-armv7hf`. The kernel is aarch64
with `CONFIG_COMPAT=y` (CE21 `linux.aarch64.conf`).

`amvdec_ports` installs its own `.compat_ioctl32`:

```
v4l2_compat_ioctl(...)
    → v4l2_vcodec_ioctl(file, cmd, (ulong)compat_ptr(arg));
```

(`aml_vcodec_dec_drv.c:395–414` CE21; same pattern CE22 `:488–505`).

That is **not** `v4l2_compat_ioctl32()`. `compat_ptr` only widens the
userspace pointer. `video_ioctl2` then interprets the pointed-to struct as
the **native 64-bit** `v4l2_format` / `v4l2_buffer` / `v4l2_exportbuffer`
layout. 32-bit those structs differ (pointer-sized fields, `timeval`,
`planes` pointers). Android 64-bit userspace never hits this. CE21 32-bit
FFmpeg / `v4l2-ctl` will.

Vendor private ioctls on the same `unlocked_ioctl`
(`V4LVIDEO_IOCTL_{ALLOC,CHECK}_FD`, `{SET,GET}_CONFIG_PARAMS`, `'I'` magic,
CE22 `aml_vcodec_dec_drv.c:54–58`) also go through `compat_ptr` +
`copy_from_user` of a native `v4l2_config_parm` — another 32/64 trap, but
FFmpeg `*_v4l2m2m` does not need those.

CE22 userspace is aarch64, so compat is irrelevant *if* the module existed.
It does not, on stock.

***verify*** on CE21: `v4l2-ctl -d /dev/video26 --all` from the 32-bit
rootfs. If QUERYCAP works but `S_FMT`/`REQBUFS` fault or return garbage,
the compat wrapper is the reason — another independent no-go.

### 7.9 Go / no-go summary

| Question | Stock CE21 (armv7 / kernel 4.9) | Stock CE22 (aarch64 / kernel 5.15) |
| --- | --- | --- |
| Is `amvdec_ports.ko` in the image? | Yes (autoload + DT okay) | **No** (not built; `modprobe -q` is a no-op) |
| `/dev/video26` present? | ***verify*** (should be) | **No** (ENOENT) |
| Direct V4L2 `DecoderBackend`? | **No-go** (32-bit compat + NV12-not-HDMI + cap 2) | **No-go** (no device) |
| libamcodec `DecoderBackend`? | **Go** | **Go** |
| Rebuild `media_modules-aml` with `CONFIG_AMLOGIC_MEDIA_V4L_DEC=m`? | Possible but still 32-bit compat + not stock | Possible, 64-bit, still not stock; frames are V4L2 CAPTURE not amvideo |

**Success criterion:** on stock CoreELEC, skip vendor V4L2. Implement hardware
decode via libamcodec (Kodi's path). Treat `/dev/video26` as an Android /
custom-kernel experiment only.

*Method note: facts marked with file paths were read directly in the linked
repos on 2026-11-01 (branch names / pins recorded above). The datasheet is the
public V0.2 revision; anything marked "Confidential for Hardkernel" beyond it
remains NDA material and was not used. `*verify*` marks on-device checks
this environment cannot run.*
