# CoreELEC 21 Amlogic-ng (armv7) vs CoreELEC 22 Amlogic-no (aarch64) hardware decode stacks

## Scope

This note compares the two CoreELEC device targets currently packaged in this repo for S922X-class hardware (for example GS-King X / Odroid N2 family):

- `coreelec21-ng-armv7hf` (CE21, Amlogic-ng, 32-bit userspace)
- `coreelec22-no-aarch64` (CE22, Amlogic-no, 64-bit userspace)

Focus areas:

1. Kernel/driver stack lineage
2. Userspace libraries and FFmpeg packaging
3. ABI constraints that matter to the existing CE21/CE22 AppImages in this repo

---

## TL;DR

- The two CoreELEC lines are not ABI-compatible for one binary: CE21 target is `armv7hf` + `/lib/ld-linux-armhf.so.3`, CE22 target is `aarch64` + `/lib/ld-linux-aarch64.so.1` (`scripts/coreelec-targets.sh`).
- FFmpeg ABI diverges across lines: CE21 pins `libavcodec.so.60` (`FFmpeg 6.0.1`), CE22 pins `libavcodec.so.63` (`FFmpeg 9.0.2`) plus matching `libavformat/libavutil/libswscale` soname bumps (`scripts/coreelec-targets.sh`).
- Upstream CoreELEC lineage differs in kernel branch and Amlogic-ce device family:
  - CE21 Amlogic-ng: `LINUX="amlogic-4.9"`
  - CE22 Amlogic-no: `LINUX="amlogic-5.15"`
  (`projects/Amlogic-ce/devices/.../options` at tags `21.3-Omega` and `22.0-Piers_rc1`).
- In this repo today, "hardware decode stack" differences are mostly packaging/runtime concerns, not active decode behavior: current decoder path is software (`isHardware=0`), and DRM/KMS hooks are off unless explicitly enabled (`AMLOGIC_DRM_KMS=ON`).
- Section 6 deep-dives instance caps, module load order, `/dev/video10` vs `/dev/video26`, FFmpeg flags that disable v4l2m2m, CE21’s missing DRM video plane, and AppImage `libamcodec`/`libdrm` symbols. Device-only claims are marked **\*verify\***.

---

## 1) Kernel and device-stack lineage (upstream CoreELEC)

### CE21 (`21.3-Omega`) -> Amlogic-ce / Amlogic-ng

From upstream CoreELEC tag `21.3-Omega`:

- `projects/Amlogic-ce/devices/Amlogic-ng/options`
  - `LINUX="amlogic-4.9"`
  - `KODI_VENDOR="amlogic-4.9"`
  - `KODIPLAYER_DRIVER="libamcodec"`

### CE22 (`22.0-Piers_rc1`) -> Amlogic-ce / Amlogic-no

From upstream CoreELEC tag `22.0-Piers_rc1`:

- `projects/Amlogic-ce/devices/Amlogic-no/options`
  - `LINUX="amlogic-5.15"`
  - `KODI_VENDOR="amlogic-5.15"`
  - `KODIPLAYER_DRIVER="libamcodec"`

### Practical interpretation for S922X-class devices

- Both lines keep Amlogic-centric Kodi player integration (`libamcodec`), but they live on different Amlogic-ce kernel generations (`4.9` vs `5.15`).
- For this repo, that means CE21 and CE22 should continue to be treated as separate device targets for any future DRM/KMS or hardware decode enablement, even if higher-level CLI behavior looks the same.

---

## 2) Userspace library stack and FFmpeg packaging in this repo

The device AppImage flow is codified in:

- `scripts/coreelec-targets.sh`
- `scripts/fetch-coreelec-sysroot.sh`
- `scripts/build-coreelec-appimage.sh`
- `scripts/verify-coreelec-appimage.sh`

### Sysroot model

`fetch-coreelec-sysroot.sh` pulls the official CoreELEC update archive and extracts only:

- `/usr/lib/*.so*`
- `/etc/os-release`

It explicitly does **not** package kernel modules, Mali blobs, Kodi, or Python into the build sysroot. So the app build is anchored to CoreELEC userspace ABI, not full firmware image behavior.

### FFmpeg model

Per target, `build-coreelec-appimage.sh`:

- Installs FFmpeg headers from a pinned upstream FFmpeg release tarball (no FFmpeg library build in this repo).
- Symlinks FFmpeg runtime libs from the CoreELEC sysroot.
- Verifies header major versions against required sonames (`check_ffmpeg_versions`).

Pinned values in this repo:

| Target | FFmpeg version (headers) | Required sonames |
| --- | --- | --- |
| CE21 `coreelec21-ng-armv7hf` | `6.0.1` | `libavcodec.so.60`, `libavformat.so.60`, `libavutil.so.58`, `libswscale.so.7` |
| CE22 `coreelec22-no-aarch64` | `9.0.2` | `libavcodec.so.63`, `libavformat.so.63`, `libavutil.so.61`, `libswscale.so.10` |

Upstream CoreELEC package manifests at those tags also advertise FFmpeg `6.0.1` (CE21) vs `9.0.2` (CE22), with Amlogic project overrides to an Amlogic-tuned rpi-ffmpeg fork commit. This repo still keys compatibility on soname/header-major matching.

### What is bundled vs borrowed

- Bundled in AppImage: `libmosquitto.so.1` (cross-built, no TLS).
- Borrowed from CoreELEC target line at runtime: dynamic loader, glibc, libstdc++, FFmpeg libs, libdrm.

---

## 3) ABI differences that matter to existing CE21/CE22 AppImages

### ELF class, architecture, and loader path

Per target metadata (`scripts/coreelec-targets.sh`):

- CE21:
  - triple: `arm-linux-gnueabihf`
  - ELF class: 32-bit ARM hard-float
  - interpreter: `/lib/ld-linux-armhf.so.3`
- CE22:
  - triple: `aarch64-linux-gnu`
  - ELF class: 64-bit ARM
  - interpreter: `/lib/ld-linux-aarch64.so.1`

This is a hard ABI split: each line requires its own artifact.

### FFmpeg ABI split

`libav*` soname majors are different between CE21 and CE22. Even if CPU architecture matched, binaries linked against one soname set would fail on the other unless that exact soname family exists.

### Installer-side enforcement already present

`scripts/install-kodi-addon.sh` enforces both:

1. target architecture (`32-bit ARM` vs `64-bit ARM (aarch64)`)
2. embedded CoreELEC target marker in the AppImage payload (`usr/share/amlogic-multistream/coreelec-target`)

So even an aarch64 host-smoke AppImage is rejected for CE22 device install.

### Symbol-version compatibility gate

`scripts/verify-coreelec-appimage.sh` validates that versioned symbol references (`GLIBC_*`, `GLIBCXX_*`, etc.) resolve against the selected CoreELEC sysroot libraries under qemu-user, preventing accidental linkage drift from the host toolchain.

`README.md` currently documents the expected glibc floor difference between the two pinned lines (CE21 around glibc 2.38 vs CE22 around glibc 2.44). The verifier enforces this indirectly by checking that every required versioned symbol exists in the target line's sysroot libraries.

---

## 4) Current "hardware decode" status in this repo

For both CE21 and CE22 device builds in this repo:

- `FfmpegDecoder` is software decode only; `isHardware()` is false.
- `prepareAmlogicHwaccel()` is a no-op placeholder.
- DRM/KMS `/dev/dri` hooks are compiled only when `-DAMLOGIC_DRM_KMS=ON`; default builds leave it off.

So, today, CE21-vs-CE22 differences are mainly about staying ABI-correct and line-correct for packaging/install/run, not about two distinct active hardware decode backends in this codebase.

---

## 5) Implications for future CE21/CE22 hardware decode work

If/when hardware decode is enabled:

1. Keep separate CE21/CE22 artifacts (do not collapse targets).
2. Expect kernel-interface divergence (`amlogic-4.9` vs `amlogic-5.15`) to surface in DRM/KMS and decoder plumbing.
3. Keep FFmpeg ABI pinning explicit per line; soname jumps are major upgrade boundaries.
4. Preserve/install metadata guardrails (`coreelec-target` marker + target checks) to prevent wrong-line deployment.

---

## Source pointers used

- Repo:
  - `scripts/coreelec-targets.sh`
  - `scripts/fetch-coreelec-sysroot.sh`
  - `scripts/build-coreelec-appimage.sh`
  - `scripts/verify-coreelec-appimage.sh`
  - `scripts/install-kodi-addon.sh`
  - `README.md` (Device AppImage and verification notes)
- Upstream CoreELEC tag snapshots (for stack lineage):
  - `21.3-Omega`:
    - `projects/Amlogic-ce/devices/Amlogic-ng/options`
    - `packages/multimedia/ffmpeg/package.mk`
  - `22.0-Piers_rc1`:
    - `projects/Amlogic-ce/devices/Amlogic-no/options`
    - `packages/multimedia/ffmpeg/package.mk`

---

## 6) Deep comparison: CE21 Amlogic-ng vs CE22 Amlogic-no (GS-King X)

Pinned images used below (same as `scripts/coreelec-targets.sh`):

| Line | Repo target | Official archive | Kernel package | media_modules-aml |
| --- | --- | --- | --- | --- |
| CE21 Amlogic-ng | `coreelec21-ng-armv7hf` | `CoreELEC-Amlogic-ng.arm-21.3-Omega.tar` | `projects/Amlogic-ce/packages/linux/package.mk`: `LINUX=amlogic-4.9`, `PKG_VERSION=50c8e815…`, `PKG_GIT_BRANCH=amlogic-4.9-20` | `projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml/package.mk`: `PKG_VERSION=60561618…` |
| CE22 Amlogic-no | `coreelec22-no-aarch64` | `CoreELEC-Amlogic-no.aarch64-22.0-Piers_rc1.tar` | same linux package: `PKG_VERSION=968a93c2…`, `PKG_GIT_BRANCH=5.15.196_20260306`, plus `common_drivers` `4dbe1c27…` | same package path: `PKG_VERSION=c15aebb1…` |

`meson-vdec` (mainline `projects/Amlogic` / AMLGX) is **not** the GS-King X stack on either CE21 ng or CE22 no. Both lines are Amlogic-ce + vendor `amvideo` / `amvdec_ports` / `libamcodec`.

Facts that can only be confirmed on a GS-King X are marked **\*verify\***.

### 6.1 `MAX_INSTANCE_MUN` patches (message says 1, code sets 2)

The live macro is `MAX_INSTANCE_MUN` (typo for NUM). It lives in `drivers/frame_provider/decoder/utils/vdec.h`. `vdec_profile.c` also has a local copy.

| | CE21 Amlogic-ng (`21.3-Omega`) | CE22 Amlogic-no (`22.0-Piers_rc1`) |
| --- | --- | --- |
| Package | `projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml` | same package path; **no patches/ directory** |
| Source commit | `60561618f2bf154c1438d3c790b625ff01f215c3` | `c15aebb1ac93028d0aceadf8f35dda2204d33424` |
| Upstream before | `vdec.h`: `#define MAX_INSTANCE_MUN  9`; `vdec_profile.c`: local `#define MAX_INSTANCE_MUN  9` | same `9` in both files (unchanged) |
| Patch | `patches/003-multi-decoders-limit-maximum-number-of-decoder.patch` (also copied under `devices/Amlogic-ne/…`; Amlogic-ng uses the shared copy) | none |
| After | `vdec.h`: `#define MAX_INSTANCE_MUN  2`; `vdec_profile.c` local `#define` **deleted** so it uses the header | still `9` / `9` |
| Subject vs code | Subject: “limit … to **1**. Kodi can only handle one decoder (12/2019).” Hunk: `9` → **`2`**. That is the CE21 discrepancy. | n/a |

`MAX_INSTANCE_NUM` in `media_sync_*.c` is a different counter (10 / 40) and is not this decoder-instance cap.

**\*verify\*** on each box: whether the running `amvdec_*` modules actually refuse a 3rd (CE21) or 10th (CE22) concurrent instance, and whether Kodi / `amvdec_ports` hit the cap independently of this macro.

### 6.2 Kernel modules: built-in vs modular, load order

| Component | CE21 Amlogic-ng | CE22 Amlogic-no |
| --- | --- | --- |
| `media_modules-aml` | Out-of-tree `.ko` set, `CONFIG_AMLOGIC_MEDIA_VDEC_*=m` in `package.mk` | Same package, newer codec list (`h266`, `avs3`, `*_fb`, `CONFIG_AMLOGIC_HW_DEMUX=m`; several old single-instance codecs dropped) |
| Load mechanism | `modules-load.d/media_modules.conf` (systemd-modules-load, file order) | `system.d/media_modules-aml.service` (`Type=oneshot`, `Before=kodi.service`, `After=opentee_linuxdriver.service`) runs `/usr/lib/coreelec/media_modules-aml` |
| `amvdec_ports` | Modular (`.ko`). Listed in `media_modules.conf` **after** several codecs (`avs2`…`mmpeg4`) and **before** `amvdec_real` | Modular. Script **`modprobe -q amvdec_ports` first**, then codecs |
| `amvideo` / video sink | Built-in: `CONFIG_AMLOGIC_MEDIA_VIDEO=y` in `devices/Amlogic-ng/linux/linux.aarch64.conf`. Source: `linux-amlogic` `drivers/amlogic/media/video_sink/video.c` | Built-in: `CONFIG_AMLOGIC_MEDIA_VIDEO=y`. Source moved to `common_drivers` `drivers/media/video_sink/video.c` |
| `amlvideo` (V4L2 capture wrapper) | Built-in via `CONFIG_AMLOGIC_MEDIA_VIDEO_PROCESSOR=y` (`drivers/amlogic/media/video_processor/video_dev/amlvideo.c`) | Built-in via same kconfig (`common_drivers/.../video_dev/amlvideo.c`) |
| `meson-vdec` | **Not this line.** Mainline-only (`projects/Amlogic` AMLGX patches). `# CONFIG_DRM_MESON` is not the vdec. | **Not this line.** `# CONFIG_DRM_MESON is not set`. Vendor stack is `amvdec_ports` + `amvideo` |
| DRM driver | `# CONFIG_AMLOGIC_DRM is not set`. Generic `CONFIG_DRM=y` + `CONFIG_DRM_FBDEV_EMULATION=y` + `CONFIG_AMLOGIC_MEDIA_FB=y` (OSD fb) | `CONFIG_AMLOGIC_DRM=y`, `CONFIG_AMLOGIC_DRM_VPU=y`, `CONFIG_AMLOGIC_DRM_USE_ION=y`, `CONFIG_AMLOGIC_DRM_EMULATE_FBDEV=y` in `common_drivers/drivers/drm/` |

CE21 `media_modules.conf` order (as shipped):

```
amvdec_avs2
amvdec_avs
amvdec_mavs
amvdec_h264
amvdec_h264mvc
amvdec_h265
amvdec_mh264
amvdec_mjpeg
amvdec_mmjpeg
amvdec_mmpeg12
amvdec_mmpeg4
amvdec_mpeg12
amvdec_mpeg4
amvdec_ports
amvdec_real
amvdec_vc1
amvdec_av1
amvdec_vp9
```

CE22 script order: `amvdec_ports`, then `avs`, `avs2`, `avs3`, `mavs`, `h264`, `h264mvc`, `mh264`, `h265`, `h266`, `mjpeg`, `mmjpeg`, `mpeg12`, `mmpeg12`, `mpeg4`, `mmpeg4`, `real`, `vc1`, `av1`, `vp9`, then `*_fb` variants.

**\*verify\*** on GS-King X: `lsmod` vs `/sys/module` after boot (built-in names will not appear as `.ko`); whether `amvdec_ports` is actually loaded before first decode.

### 6.3 `/dev/video*` and DRM nodes

| Node / role | CE21 (source) | CE22 (source) |
| --- | --- | --- |
| `/dev/video10` | `amlvideo.c`: `video_nr_base = 10`; inst 0 registers `VFL_TYPE_GRABBER` at 10. `vidioc_querycap` sets `device_caps = V4L2_CAP_VIDEO_CAPTURE` only (plus STREAMING/READWRITE in capabilities). **Capture-only VFM wrapper**, not M2M. | Same `video_nr_base = 10`; inst 0 registers `VFL_TYPE_VIDEO` at 10. Caps: `V4L2_CAP_VIDEO_CAPTURE \| V4L2_CAP_STREAMING`. Still capture-only. |
| `/dev/video23` | If `CONFIG_AMLOGIC_MEDIA_MULTI_DEC=y` (`n_devs = 2`), inst 1 uses `video_nr_base_second = 23`. Both CE configs set `CONFIG_AMLOGIC_MEDIA_MULTI_DEC=y`. | Same formula. |
| `/dev/video26` | `amvdec_ports/aml_vcodec_dec_drv.c`: `video_register_device(..., VFL_TYPE_GRABBER, 26)`; `vfl_dir = VFL_DIR_M2M`; `device_caps = V4L2_CAP_VIDEO_M2M_MPLANE \| V4L2_CAP_STREAMING`. Logs `decoder registered as /dev/video%d`. Name `aml-vcodec-dec`. | Same node 26; `#if LINUX_VERSION_CODE < 5.15` uses `VFL_TYPE_GRABBER`, else `VFL_TYPE_VIDEO`. Same M2M caps. Logs `v4ldec registered as /dev/video%d`. |
| `/dev/video30+` | `v4lvideo.c`: `video_nr_base = 30` (`V4L2_CAP_VIDEO_CAPTURE`). Extra capture nodes, not the M2M decoder. | Same family lives under `common_drivers` (path present). **\*verify\*** exact numbers. |
| `/dev/dri/card0` | Generic DRM + OSD fb helper may create a card, but **`CONFIG_AMLOGIC_DRM` is off** and there is no meson video-plane driver. Not a usable Amlogic FBC video plane. | `CONFIG_AMLOGIC_DRM=y`. `am_meson_plane_create()` inits OSD overlays then `pipeline->num_video` **VIDEO_PLANE** overlays named `video%d`, each advertised with `video_fbc_modifier[]` (`DRM_FORMAT_MOD_AMLOGIC_FBC` BASIC/SCATTER ± MEM_SAVING, plus LINEAR). Caps: `MESON_MAX_OSD=4`, `MESON_MAX_VIDEO=3` (`meson_drv.h`). How many G12B/S922X actually enables is **\*verify\***. |

Implication for this player:

- **CE21:** hardware pictures are presented by the **amvideo video sink** (VFM / `libamcodec` `codec_amvideo_dev_*`), i.e. AmvideoRenderer-style self-present. A DRM-prime + KMS plane path is not available on this line.
- **CE22:** source has a real meson DRM video-plane + Amlogic FBC modifier path. Whether GS-King X can scan out `amvdec_ports` FBC frames on `card0` without going through amvideo is **\*verify\***. Until that is proven, treat amvideo self-present as the fallback on both lines.

### 6.4 FFmpeg configure flags that kill v4l2m2m / v4l2-request

Both Amlogic-ce devices set `GRAPHIC_DRIVERS=""` (`projects/Amlogic-ce/devices/Amlogic-ng/options` and `…/Amlogic-no/options`). `config/graphic` `get_graphicdrivers()` therefore leaves **`V4L2_SUPPORT="no"`** (it is only flipped to yes for etnaviv/lima/panfrost/vc4/…).

`packages/multimedia/ffmpeg/package.mk` then:

| | CE21 `21.3-Omega` (`PKG_VERSION=6.0.1`) | CE22 `22.0-Piers_rc1` (`PKG_VERSION=9.0.2`) |
| --- | --- | --- |
| Project match | `Amlogic-ce` falls in `*)` | `Amlogic-ce` matches `Generic\|Amlogic-ce` and **clears** `PKG_FFMPEG_REQUEST_{ENABLE,DISABLE}` |
| When `V4L2_SUPPORT=no` | `PKG_FFMPEG_V4L2="--disable-v4l2_m2m --disable-libudev --disable-v4l2-request"` | `PKG_FFMPEG_V4L2="--disable-v4l2_m2m"` + empty request flags |
| Extra | `DISPLAYSERVER != x11` → `--enable-libdrm` (overwrites the VAAPI slot). `--disable-vdpau`. `--enable-hwaccels`. | same `--enable-libdrm`; `--disable-vdpau` is hardcoded |
| v4l2 patches applied? | `*)` adds `PKG_PATCH_DIRS+=" v4l2-request v4l2-drmprime"` even though configure then disables those features | v4l2-request patch dir is added **only if** `V4L2_SUPPORT=yes`, so **not** applied on Amlogic-ce |

So on both GS-King X images, **stock FFmpeg cannot open `h264_v4l2m2m` / `hevc_v4l2m2m` / v4l2-request**. CE21 also hard-disables libudev + request. CE22 only hard-disables `v4l2_m2m`; request is not enabled either (no `--enable-v4l2-request`, no request patches, `GRAPHIC_DRIVERS` empty).

This repo’s device AppImages link that image FFmpeg and do not rebuild it. Enabling v4l2m2m in the player therefore needs either a rebuilt CoreELEC FFmpeg or a non-FFmpeg backend (`libamcodec` / raw `amvdec_ports`).

### 6.5 DRM: CE21 has no usable video-plane / FBC path

| | CE21 Amlogic-ng | CE22 Amlogic-no |
| --- | --- | --- |
| `CONFIG_AMLOGIC_DRM` | **is not set** | **=y** (+ VPU, ION, fbdev emulate) |
| Video plane | none in meson DRM (driver absent) | `VIDEO_PLANE` overlays; FBC modifiers `DRM_FORMAT_MOD_AMLOGIC_FBC(...)` in `common_drivers/drivers/drm/meson_plane.c` |
| OSD / UI | `CONFIG_AMLOGIC_MEDIA_FB=y` (classic OSD fb) | OSD planes + AFBC/AFRC modifiers on OSD; primary plane is OSD0 |
| Kodi player driver | `KODIPLAYER_DRIVER="libamcodec"` | same |
| What Kodi uses | Amcodec + **amvideo self-present** (`codec_amvideo_dev_close` et al. in `libamcodec`) | still `libamcodec`; DRM video planes exist in kernel source. **\*verify\*** whether Piers Kodi on GS-King X actually commits FBC to those planes or still self-presents via amvideo |

For this repo: `-DAMLOGIC_DRM_KMS=ON` opening `/dev/dri/card0` is only a resource probe today. On CE21 it cannot become a video scanout path. On CE22 it is the first line that *might* (`\*verify\*`).

### 6.6 AppImage / sysroot implications (`libamcodec` vs `libdrm`)

Current device AppImage (`scripts/build-coreelec-appimage.sh` + `fetch-coreelec-sysroot.sh` + `verify-coreelec-appimage.sh`):

- Extracts `usr/lib/*.so*` from the official SYSTEM squashfs, then **deletes `libMali*`**.
- **Requires** (else sysroot verify fails): dynamic loader, `libc.so.6`, `libstdc++.so.6`, `libdrm.so.2`, and the per-line `CE_FFMPEG_SONAMES`.
- **Does not require** `libamcodec`. The player `NEEDED` list is FFmpeg + libdrm + bundled `libmosquitto.so.1` only.
- Headers for FFmpeg/libdrm come from pinned upstream tarballs, not from CoreELEC.

`libamcodec` as shipped by CoreELEC’s package (sources.coreelec.org tarballs referenced by `projects/Amlogic-ce/packages/multimedia/libamcodec/package.mk`):

| | CE21 Amlogic-ng (`PKG_VERSION=8a779cab…`, `libamcodec-arm-….tar.xz`) | CE22 Amlogic-no (`PKG_VERSION=d28bf07f…`, `libamcodec-aarch64-….tar.xz`) |
| --- | --- | --- |
| ELF | 32-bit ARM EABI5 | 64-bit AArch64 |
| File | `usr/lib/libamcodec.so` → `libamcodec.so.0.0` (67 456 bytes) | same names (133 776 bytes) |
| SONAME | `libamcodec.so` (unversioned) | `libamcodec.so` (unversioned) |
| NEEDED | `libm.so.6`, `libc.so.6` | same |
| Notable exports | `codec_init`, `codec_close`, `codec_amvideo_dev_close`, `codec_set_drmmode`, `codec_h_write`, `codec_get_vdec_info`, … | same names; **different ABI** (ELF class) |

If the official SYSTEM image installs that package, `fetch-coreelec-sysroot.sh` will keep `libamcodec.so*` (it is not in the Mali delete list). **\*verify\*** that the squashfs actually contains it (the package.mk installs it; the AppImage scripts never check).

`libdrm.so.2` is already the link/run ABI this repo uses on both lines (headers pinned to libdrm 2.4.124). CE22 meson DRM ioctls / Amlogic FBC modifiers are **kernel UAPI**, not extra userspace SONAMEs; `include/uapi/amlogic/drm/meson_drm.h` is not in the AppImage sysroot.

Practical AppImage rules:

1. Do **not** ship one binary for both lines (`armv7hf` vs `aarch64`, FFmpeg soname 60 vs 63, glibc 2.38 vs 2.44).
2. Linking `libamcodec.so` later still needs two artifacts (unversioned SONAME, opposite ELF class).
3. Stock image FFmpeg has v4l2m2m compiled out; an AppImage cannot “turn on” `/dev/video26` through `libavcodec` without a different FFmpeg.
4. CE21 cannot grow a DRM-prime presenter against this kernel; CE22 can try, after **\*verify\*** of `card0` video planes on GS-King X.

### 6.7 Probe commands (run on the box for that CE line)

Identify the line first:

```sh
. /etc/os-release
echo "arch=${COREELEC_ARCH:-$DISTRO_ARCH} version=$VERSION"
# expect Amlogic-ng.arm + 21.3-Omega   or   Amlogic-no.aarch64 + 22.0-Piers_rc1
```

Modules / built-in:

```sh
ls /lib/modules/$(uname -r)/ -R | grep -E 'amvdec|amvideo|amlvideo|media_modules'
lsmod | grep -E 'amvdec|amvideo|amlvideo|meson'
grep -E 'amvdec|amvideo|amlvideo|media_modules' /lib/systemd/system/media_modules-aml.service \
  /usr/lib/coreelec/media_modules-aml \
  /usr/lib/modules-load.d/media_modules.conf 2>/dev/null
cat /proc/modules | grep -E 'amvdec|amvideo'
# built-in: no .ko; check kconfig leftovers if /proc/config.gz exists
zcat /proc/config.gz 2>/dev/null | grep -E 'AMLOGIC_DRM|AMLOGIC_MEDIA_VIDEO|MESON_VDEC|VIDEO_MESON'
```

`MAX_INSTANCE_MUN` (running modules; **\*verify\***):

```sh
# dmesg / sysfs names differ by kernel; try:
dmesg | grep -iE 'max_instance|multi.?decod|vdec.*instance'
find /sys /proc -iname '*instance*' 2>/dev/null | head
# strings on the loaded amvdec .ko (if modular):
modinfo -n amvdec_ports
strings "$(modinfo -n amvdec_h265 2>/dev/null)" | grep -n MAX_INSTANCE || true
```

`/dev/video*` roles:

```sh
ls -l /dev/video* /dev/dri/* 2>/dev/null
for n in /dev/video*; do
  echo "==== $n ===="
  v4l2-ctl -d "$n" --all 2>/dev/null | sed -n '1,40p'
  v4l2-ctl -d "$n" --list-formats-ext 2>/dev/null
done
# expect: video10 CAPTURE-only (amlvideo); video26 M2M_MPLANE (aml-vcodec-dec)
# *verify* extra nodes (video23, video30+)
```

DRM planes / modifiers (CE22; CE21 should lack meson video planes):

```sh
modetest -M meson -c -p 2>/dev/null || modetest -c -p
# look for overlay planes named video0/video1 and modifiers containing AMLOGIC_FBC
cat /sys/class/drm/card0-*/status 2>/dev/null
```

FFmpeg (image binary, not this repo’s AppImage):

```sh
ffmpeg -hide_banner -hwaccels
ffmpeg -hide_banner -decoders | grep -E 'v4l2|amf|amcodec'
# expect no v4l2m2m / v4l2request on stock CE21/CE22 FFmpeg
```

AppImage sysroot / on-box libraries:

```sh
# on the build host after fetch-coreelec-sysroot.sh TARGET:
SYS=.cache/coreelec/TARGET/sysroot/usr/lib
ls -l "$SYS"/libamcodec.so* "$SYS"/libdrm.so* "$SYS"/libavcodec.so* 2>&1
readelf -d "$SYS"/libamcodec.so 2>/dev/null | grep SONAME
readelf -d "$SYS"/libdrm.so.2 | grep SONAME
file "$SYS"/libamcodec.so.0.0 "$SYS"/libavcodec.so.*

# on the box:
ls -l /usr/lib/libamcodec.so* /usr/lib/libdrm.so* /usr/lib/libavcodec.so*
file /usr/lib/libamcodec.so /storage/amlogic-multistream
# AppImage marker:
unsquashfs -l /storage/amlogic-multistream 2>/dev/null | grep coreelec-target
```

### 6.8 Extra source pins

- CE21 `MAX_INSTANCE_MUN` patch: `https://raw.githubusercontent.com/CoreELEC/CoreELEC/21.3-Omega/projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml/patches/003-multi-decoders-limit-maximum-number-of-decoder.patch`
- CE21 amlvideo capture node: `linux-amlogic@50c8e815` `drivers/amlogic/media/video_processor/video_dev/amlvideo.c` (`video_nr_base=10`)
- Both lines M2M node: `media_modules-aml` `drivers/amvdec_ports/aml_vcodec_dec_drv.c` (`video_register_device(..., 26)`)
- CE22 DRM video + FBC: `common_drivers@4dbe1c27` `drivers/drm/meson_plane.c` (`video_fbc_modifier[]`, `am_video_plane_create`)
- FFmpeg kill flags: `packages/multimedia/ffmpeg/package.mk` at each tag + `config/graphic` `V4L2_SUPPORT` default
- `libamcodec` blobs: `https://sources.coreelec.org/libamcodec-arm-8a779caba41b163fed9ebd0df82db36dd962a9a3.tar.xz` and `libamcodec-aarch64-d28bf07facc8727de52ba1f70c1362f9e19adc25.tar.xz`
