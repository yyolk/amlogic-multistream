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
