# Mainline `meson-vdec` and vendor `amvdec` on S922X (GS-King X)

Research only. No player, build, or packaging changes. Written 2026-11-01 against
this repo at `6d9d146` (merge of #34). Nothing here was run on a GS-King X.
Claims that still need a box are tagged *verify*.

This note is the meson-vdec / amvdec file for the 1–9 tile HLS wall
(`LayoutEngine::kMaxStreams == 9`). Sibling drafts already cover adjacent
ground; this document does not repeat their call sequences, FFmpeg wrappers,
DRM plane topology, or datasheet Mpx/s arithmetic.

| Sibling | File | What it owns; what this note adds |
| --- | --- | --- |
| #36 | `amlogic-hw-decode-coreelec-kodi.md` | Kodi / amcodec history |
| #37 | `amlogic-hw-decode-ce21-vs-ce22.md` | Userspace ABI / AppImage split |
| #38 | `amlogic-hw-decode-multistream-limits.md` | Instance cap vs throughput / CMA |
| #39 | `amlogic-hw-decode-vendor-sdk.md` | SDK / datasheet / libamcodec catalogue |
| #40 | `amlogic-hw-decode-drm-kms.md` | Vendor KMS planes / composition |
| #41 | `amlogic-hw-decode-ffmpeg.md` | `*_v4l2m2m`, drmprime, request API |
| #42 | `amlogic-hw-decode-backend-design.md` | Proposed `AmlogicV4l2Decoder` |
| #43 | `amlogic-hw-decode-code-examples.md` | Userspace examples + LibreELEC `amvdec` |
| #45 | `amlogic-hw-decode-v4l2.md` | Vendor V4L2 M2M / amstream APIs |

Focus here: the staging mainline driver, the three things named “amvdec”,
which CoreELEC 21 vs 22 image actually ships, the GS-King X device trees,
and whether `meson-vdec` can be the multi-stream `DecoderBackend`.

## Verdict in one paragraph

`meson-vdec` is a **no-go** as the 1–9 tile path. It is a stateful V4L2 M2M
driver with one live session (`-EBUSY` on a second `STREAMON`), no HEVC
format entry on G12A/G12B, and it is **not bound or built** on either
CoreELEC line this repo packages. The multi-instance hardware path on the
box is the vendor `media_modules-aml` core (`MAX_INSTANCE_MUN`), reached
from Kodi via `libamcodec` / `/dev/amstream*`, and on CoreELEC 21 only also
via `amvdec_ports` at `/dev/video26` (capped at 2). CoreELEC 22 still has
the `amvdec_ports` source and the `vcodec_dec` DT node, but the image
recipe does not build the module. *verify* both nodes on a box.

## Three names that are not one driver

| Name | What it is | Multi-session | On GS-King X CoreELEC |
| --- | --- | --- | --- |
| **mainline `meson-vdec`** | Staging stateful V4L2 M2M in `drivers/staging/media/meson/vdec/`. Internal structs are named `amvdec_*`. Video node `meson-video-decoder`. | No. `core->cur_sess` is exclusive. | Not used. CE21 tree lacks the driver. CE22 5.15 tree has a copy; `STAGING_MEDIA` is off and the vendor DT does not use `amlogic,g12a-vdec`. |
| **LibreELEC `amvdec`** | Out-of-tree *stateless* replacement (`VIDEO_MESON_AMVDEC`) that *deletes* staging `meson-vdec`. Same `g12a-vdec` compatible, G12B reuses it. | No. Same `cur_sess` / `-EBUSY` claim. | Not on CoreELEC. Unreviewed LOCAL patches dated 2026-09-26; see #43. |
| **vendor `amvdec_*.ko` + `amvdec_ports.ko`** | Per-codec firmware loaders (`amvdec_mh264`, `amvdec_h265`, …) plus optional V4L2 wrapper `aml-vcodec-dec` at `/dev/video26`. Both call `vdec_create()`. | Yes, up to `MAX_INSTANCE_MUN` (9 stock; **2** on CE21 after a CoreELEC patch). | Codec modules: both lines. `amvdec_ports`: CE21 yes, CE22 source-only. |

Do not treat a `dmesg` line containing `amvdec` as proof that mainline
`meson-vdec` is loaded.

## Source pins

Read on 2026-11-01. CoreELEC pins are the **release tags** this repo’s
device AppImages already use (`scripts/coreelec-targets.sh`: `21.3-Omega`,
`22.0-Piers_rc1`), not later branch HEADs.

| Tree | Commit / tag | Role |
| --- | --- | --- |
| [torvalds/linux](https://github.com/torvalds/linux/tree/cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a) | `cbbddb6b` (master) | Mainline `meson-vdec`, bindings, GS-King X dts |
| [CoreELEC/CoreELEC](https://github.com/CoreELEC/CoreELEC/tree/fc61125e8900ab0c2593a29b615980ed0cd5b939) | `fc61125e` = tag `21.3-Omega` | CE21 recipes, kernel config, instance-cap patch |
| [CoreELEC/CoreELEC](https://github.com/CoreELEC/CoreELEC/tree/75626f5a6e955cdee0aa1a4c97b0d8660e1ea1af) | `75626f5a` = tag `22.0-Piers_rc1` | CE22 recipes, kernel config, `media_modules-aml.sh` |
| [CoreELEC/linux-amlogic](https://github.com/CoreELEC/linux-amlogic/tree/50c8e8154c5aba3c7cd8438d04dd5fdd51733e41) | `50c8e815` (`amlogic-4.9-20`) | CE21 kernel + vendor GS-King X dts |
| [CoreELEC/linux-amlogic](https://github.com/CoreELEC/linux-amlogic/tree/968a93c28223f72ff94e706eabb0277292bac878) | `968a93c2` (`5.15.196_20260306`) | CE22 kernel (includes unused staging `meson-vdec`) |
| [CoreELEC/common_drivers](https://github.com/CoreELEC/common_drivers/tree/4dbe1c27ae176fde4c1cbef7506618fd7f62b976) | `4dbe1c27` | CE22 vendor dts / `mesong12b.dtsi` |
| [CoreELEC/media_modules-aml](https://github.com/CoreELEC/media_modules-aml/tree/60561618f2bf154c1438d3c790b625ff01f215c3) | `60561618` | CE21 decoder modules |
| [CoreELEC/media_modules-aml](https://github.com/CoreELEC/media_modules-aml/tree/c15aebb1ac93028d0aceadf8f35dda2204d33424) | `c15aebb1` | CE22 decoder modules |
| [LibreELEC/LibreELEC.tv](https://github.com/LibreELEC/LibreELEC.tv/tree/f59641eb70c727cc81e33508177ac700abac81ab) | `f59641eb` (master) | Replacement `amvdec` patch series |

Shorthands below: `ML` = torvalds/linux `@cbbddb6b`, `CE21` / `CE22` = the
CoreELEC tag commits, `MM49` / `MM515` = the two `media_modules-aml` commits,
`K49` / `K515` = the two `linux-amlogic` commits, `CD515` = `common_drivers`
`4dbe1c27`.

---

## 1. Mainline `meson-vdec`

### Repo, path, registration

- Path: `drivers/staging/media/meson/vdec/`
  ([tree](https://github.com/torvalds/linux/tree/cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a/drivers/staging/media/meson/vdec)).
- Kconfig: `VIDEO_MESON_VDEC` (`ML/.../Kconfig` L3–L11). Help text still
  says “gxbb/gxl/gxm chips”; the OF table also matches G12A and SM1.
- Objects (`Makefile` L6): `vdec.o vdec_platform.o vdec_helpers.o
  vdec_1.o vdec_hevc.o esparser.o codec_mpeg12.o codec_h264.o
  codec_hevc_common.o codec_vp9.o`. There is **no** `codec_hevc.o`.
- Video device name `meson-video-decoder`,
  `V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING`, dynamic minor
  (`video_register_device(..., VFL_TYPE_VIDEO, -1)`)
  (`ML/.../vdec.c` L1078–L1089).
- OF match `amlogic,g12a-vdec` → `vdec_platform_g12a`
  (`ML/.../vdec.c` L978–L979). There is no `amlogic,g12b-vdec`. G12B
  (S922X) reuses the G12A compatible.

Binding (`Documentation/devicetree/bindings/media/amlogic,gx-vdec.yaml`
L19–L26):

- ESPARSER writes the bitstream into a VIFIFO; VDEC blocks read it.
- **VDEC_1**: MPEG-1/2/4, MJPEG, H.263, H.264, VC-1.
- **VDEC_HEVC**: HEVC and VP9.
- Both blocks **share the `vdec` IRQ and cannot run concurrently**.

That last sentence is a hardware constraint, not just a software cap: one
H.264 session and one VP9 session cannot overlap even if the driver later
grew a session list.

### Codecs on G12A / G12B (S922X)

`vdec_formats_g12a[]` (`ML/.../vdec_platform.c` L192–L240):

| OUTPUT fourcc | Max | CAPTURE | Firmware | Block |
| --- | --- | --- | --- | --- |
| `V4L2_PIX_FMT_VP9` | 3840×2160 | `NV12M` only | `meson/vdec/g12a_vp9.bin` | `vdec_hevc_ops` |
| `V4L2_PIX_FMT_H264` | 3840×2160 | `NV12M` only | `meson/vdec/g12a_h264.bin` | `vdec_1_ops` |
| `V4L2_PIX_FMT_MPEG1` | 1920×1080 | `NV12M`, `YUV420M` | `meson/vdec/gxl_mpeg12.bin` | `vdec_1_ops` |
| `V4L2_PIX_FMT_MPEG2` | 1920×1080 | `NV12M`, `YUV420M` | `meson/vdec/gxl_mpeg12.bin` | `vdec_1_ops` |

`V4L2_PIX_FMT_HEVC` does not appear in **any** platform table (GXBB, GXL,
GXLX, GXM, G12A, SM1). `codec_hevc_common.c` is shared FBC/MMU helper
code used by VP9 (`codec_vp9.c` includes it). `vdec_hevc.c` is the
VDEC_HEVC *power/clock/IMEM* block, not an HEVC elementary-stream codec.

linux-firmware still ships `meson/vdec/g12a_hevc_mmu.bin` (listed next to
`g12a_h264.bin` / `g12a_vp9.bin` in the firmware tree). The driver does
not declare it in `MODULE_FIRMWARE` (`vdec_platform.c` L328–L335). So
HEVC firmware exists; the staging format table never selects it.

H.264 and VP9 advertise `V4L2_FMT_FLAG_DYN_RESOLUTION`. MPEG-1/2 do not.
The staging `TODO` (`ML/.../TODO` L1–L8) says the driver is not
guaranteed to follow the latest stateful-decoder spec, “especially with
compression standards like MPEG1/2 where the driver does not support
dynamic resolution switching, including the first one used to determine
coded resolution.”

This player’s codec whitelist (`src/ffmpeg_decoder.h` L47–L49) is
`h264, hevc, mpeg4, mpeg2video, mpeg1video, mjpeg` plus audio names.
Against mainline `meson-vdec` on S922X that leaves:

- H.264: yes.
- HEVC: **no** (the wall’s other common HLS codec).
- MPEG-2: yes, 1080p, no first-resolution event.
- MPEG-4 / MJPEG: not in the G12A table.
- VP9: hardware yes; not in the current whitelist.

### Multi-instance behaviour

`vdec_start_streaming()` (`ML/.../vdec.c` L281–L364):

```c
if (core->cur_sess && core->cur_sess != sess) {
    ret = -EBUSY;
    goto bufs_done;
}
```

`core` is a singleton (`struct amvdec_core`, one `cur_sess` at
`ML/.../vdec.h` L67, L92). A second file descriptor can `open()`
(`vdec_open` allocates a new `amvdec_session`). The **second**
`VIDIOC_STREAMON` that would power the hardware returns `-EBUSY` and
returns both queues’ buffers as `QUEUED`.

The claim is taken on OUTPUT `STREAMON` once `streamon_out` is set
(L294–L300, L344–L345): CAPTURE-only `STREAMON` before OUTPUT is a no-op
for power-on. `vdec_stop_streaming` clears `cur_sess` when it powers off
(L418).

The check is **not** under `core->lock`. Concurrent `STREAMON` from two
fds can both observe `cur_sess == NULL` (TOCTOU). Anand Moon’s 2026
series (“Protect session exclusivity check with lock” / STREAMON–STREAMOFF
races, v5–v7 on linux-amlogic / dri-devel) proposes wrapping that
compare-and-set. It is **not** in `cbbddb6b`. *verify* that a naive
two-process `STREAMON` still returns `EBUSY` rather than double-init.

VIFIFO is `SIZE_VIFIFO` = `SZ_16M` per session (`vdec.c` L33, L315–L318).
Only one session can own it at a time, so this is not a 9×16 MiB problem
on mainline; it is a 16 MiB CMA hit for the single live decoder.

### DMA-BUF, `VIDIOC_EXPBUF`, DRM PRIME

Both vb2 queues (`m2m_queue_init`, `vdec.c` L812–L836):

- types: `VIDEO_OUTPUT_MPLANE` / `VIDEO_CAPTURE_MPLANE`
- `io_modes = VB2_MMAP | VB2_DMABUF`
- `mem_ops = vb2_dma_contig_memops` (Kconfig `select VIDEOBUF2_DMA_CONTIG`)
- `timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY`

Ioctl table (`vdec.c` L793): `.vidioc_expbuf = v4l2_m2m_ioctl_expbuf`.

So a capture buffer is CMA-contiguous and can be:

1. `mmap`’d and copied into `DecodedFrame` (CPU path), or
2. exported with `VIDIOC_EXPBUF` per plane and imported with
   `drmPrimeFDToHandle` (DRM PRIME).

CAPTURE for H.264/VP9 is **`NV12M`** (two planes, typically two dma-buf
fds). That is not `FramePixelFormat::Yuv420p` and not single-plane
`NV12`. A PRIME importer must use `drmModeAddFB2` / `AddFB2WithModifiers`
with two handles, or an EGL `EGL_EXT_image_dma_buf_import` image with
two fds.

Stock meson-vdec emits **linear** NV12M. It does **not** emit
`V4L2_PIX_FMT_MESON_AM21C` / `AM21B` or
`DRM_FORMAT_MOD_AMLOGIC_FBC(...)`. Those fourccs live in the chewitt
FFmpeg fork / LibreELEC `PROJECT=Amlogic` path (#41). Do not plan a
modifier-aware `DrmRenderer` around this driver.

Upstream FFmpeg `*_v4l2m2m` still `mmap`s and copies; it has no
`AV_PIX_FMT_DRM_PRIME` (#41). Zero-copy from `meson-vdec` needs either a
hand-rolled `EXPBUF` client or the out-of-tree `v4l2-drmprime` patch,
neither of which CoreELEC’s Amlogic-ce FFmpeg builds.

Mainline `meson-drm` exposes one overlay plane (`meson_overlay_create`).
A 3×3 wall cannot be one KMS plane per tile even if nine decoders
existed. Composition (GE2D / GLES / CPU) is a display problem, covered
by #40; it does not lift the single-session decode cap.

---

## 2. Vendor `amvdec` / `media_modules` vs mainline — CE21 vs CE22

### What each image builds

| | CE21 Amlogic-ng (`21.3-Omega`) | CE22 Amlogic-no (`22.0-Piers_rc1`) | Mainline |
| --- | --- | --- | --- |
| Kernel | vendor 4.9.269 (`K49` `50c8e815`) | vendor 5.15.196 (`K515` `968a93c2`) + `common_drivers` `4dbe1c27` | upstream |
| Userspace | armv7hf (this repo’s `coreelec21-ng-armv7hf`) | aarch64 (`coreelec22-no-aarch64`) | — |
| `meson-vdec` **source** | **absent** (no `drivers/staging/media/meson/vdec/`) | **present** (5.15 staging copy; same `cur_sess` / no HEVC) | present |
| `VIDEO_MESON_VDEC` | symbol does not exist | not built: `# CONFIG_STAGING_MEDIA is not set` (`linux.aarch64.conf` L5576) | distro-dependent |
| Vendor decode core | `media_modules-aml` `60561618` | `media_modules-aml` `c15aebb1` | none |
| `amvdec_ports` (`aml-vcodec-dec`, `/dev/video26`) | **built**: `drivers/Makefile` L5–L7 `ifdef CONFIG_V4L2_MEM2MEM_DEV`; config L3579 `CONFIG_V4L2_MEM2MEM_DEV=m`; autoload `amvdec_ports` | **not built**: `obj-$(CONFIG_AMLOGIC_MEDIA_V4L_DEC)` and `make_target` omits that symbol. Script still `modprobe -q amvdec_ports` (failure hidden). | n/a |
| `amvdec_*.ko` codecs | yes (`amvdec_mh264`, `amvdec_h265`, `amvdec_vp9`, …) | yes (plus `amvdec_h266`, `*_fb`, AVS3, …) | n/a |
| `MAX_INSTANCE_MUN` | **2** (patch 003; subject says “1”, diff sets 2) | **9** (no `patches/` dir) | 1 streaming session |
| `CONFIG_AMLOGIC_MEDIA_MULTI_DEC` | `=y` (config L1332) | `=y` (config L6466) | n/a |
| DT decoder nodes | `compatible = "amlogic, vdec"` + `amlogic, vcodec-dec` | same | `amlogic,g12a-vdec` |
| Kodi path | `libamcodec` / amstream | `libamcodec` / amstream | DRM PRIME / v4l2m2m on a mainline image |

CE21 `media_modules-aml/package.mk` `make_target` (tag `21.3-Omega` L19–L40)
lists per-codec `CONFIG_AMLOGIC_MEDIA_VDEC_*` and does **not** mention
`amvdec_ports`. The 4.9 `drivers/Makefile` pulls `amvdec_ports/` in
whenever the *kernel* has `CONFIG_V4L2_MEM2MEM_DEV`, which Amlogic-ng
sets. Autoload:

```
# CE21 .../modules-load.d/media_modules.conf L14
amvdec_ports
```

CE22 `package.mk` `make_target` (tag `22.0-Piers_rc1` L19–L38) is an
explicit `CONFIG_*` list with no `CONFIG_AMLOGIC_MEDIA_V4L_DEC`. The
module tree’s own top-level `Makefile` default `CONFIGS` includes
`CONFIG_AMLOGIC_MEDIA_V4L_DEC=y` (`MM515/Makefile` L43); CoreELEC does
not use that default. The boot script still tries:

```
# CE22 .../scripts/media_modules-aml.sh L8
modprobe -q amvdec_ports
```

`-q` hides the missing-module error. *verify* `lsmod` / `/dev/video26` on
both images; #45 reports the 21.3-Omega squashfs contains
`amvdec_ports.ko` and the 22.0-Piers_rc1 squashfs does not.

### Instance id pool (shared by V4L2 and amstream)

Both trees:

```c
#define MAX_INSTANCE_MUN  9
id = ida_simple_get(&vdec_core->ida, 0, MAX_INSTANCE_MUN, GFP_KERNEL);
```

- CE21 header: `MM49/.../vdec.h` L51; allocator `vdec.c` L1141–L1142
  (`vdec_create` at L1129). After patch 003 this is **2**.
- CE22 header: `MM515/.../vdec.h` L70; allocator `vdec.c` L1808–L1809
  (`vdec_create` at L1797). Unpatched **9**.

`amvdec_ports` calls the same `vdec_create()` (see #45). A live Kodi
`libamcodec` session and a `/dev/video26` session share the ida. This
player stops Kodi first (`scripts/launch-coreelec.sh`), so on CE21 both
ids are available to the player. *verify*.

CE21 patch
`projects/Amlogic-ce/packages/linux-drivers/amlogic/media_modules-aml/patches/003-multi-decoders-limit-maximum-number-of-decoder.patch`
(Portisch, 2019-12-27): subject “limit … to 1”; hunk changes
`MAX_INSTANCE_MUN` 9 → 2. The `0 .. N-1` ida range is N instances.

### Vendor V4L2 node (CE21 only, as shipped)

`video_register_device(..., 26)` → `/dev/video26`, name `aml-vcodec-dec`
(`MM49/.../aml_vcodec_dec_drv.c` L460–L490; CE22 source L644–L675 if
built). Caps `V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING`. Module
param `multiplanar` defaults false (L615–L617): OUTPUT is always
`*_MPLANE`, CAPTURE is single-planar unless that sysfs is set. Format
tables include HEVC (unlike mainline). Full ioctl sequence and the
NV21-default / 2048×1088 `TRY_FMT` clamp are in #45 — not repeated here.

### LibreELEC replacement `amvdec` (not CoreELEC)

`projects/Amlogic/patches/linux/amlogic-0117` *removes* staging
`meson-vdec`. `0119` adds `VIDEO_MESON_AMVDEC` (stateless request API)
for GXBB through SM1; G12B reuses `amlogic,g12a-vdec`. HEVC is in *that*
format table. `vdec_start_streaming` still does:

```c
if (core->cur_sess && core->cur_sess != sess) {
    ret = -EBUSY;
```

under `claim_lock` (`amlogic-0119` around the new `vdec.c` L568–L574).
So even on a future mainline-ish image this replacement is still
single-session. Patches are LOCAL, 2026-09-26, no visible upstream
review (#43). Out of scope for this repo’s CoreELEC targets.

---

## 3. Device tree / GS-King X

### Mainline

`arch/arm64/boot/dts/amlogic/meson-g12b-gsking-x.dts` (`ML`, 158 lines):

```dts
compatible = "azw,gsking-x", "amlogic,s922x", "amlogic,g12b";
model = "Beelink GS-King X";
```

It includes `meson-g12b-w400.dtsi` → `meson-g12b.dtsi` →
`meson-g12-common.dtsi`. The GS-King X file does **not** mention `vdec`;
the node comes from the common include (`meson-g12-common.dtsi`
L2147–L2168):

```dts
vdec: video-decoder@ff620000 {
    compatible = "amlogic,g12a-vdec";
    reg = <0x0 0xff620000 0x0 0x10000>,
          <0x0 0xffd0e180 0x0 0xe4>;
    reg-names = "dos", "esparser";
    interrupts = <GIC_SPI 44 IRQ_TYPE_EDGE_RISING>,
                 <GIC_SPI 32 IRQ_TYPE_EDGE_RISING>;
    interrupt-names = "vdec", "esparser";
    amlogic,ao-sysctrl = <&rti>;
    amlogic,canvas = <&canvas>;
    clocks = <&clkc CLKID_PARSER>, <&clkc CLKID_DOS>,
             <&clkc CLKID_VDEC_1>, <&clkc CLKID_VDEC_HEVC>,
             <&clkc CLKID_VDEC_HEVCF>;
    clock-names = "dos_parser", "dos", "vdec_1",
                  "vdec_hevc", "vdec_hevcf";
    resets = <&reset RESET_PARSER>;
    reset-names = "esparser";
};
```

G12A/SM1 need the fifth clock `vdec_hevcf` (binding L99–L110;
`vdec_probe` L1025–L1029). No `status = "disabled"`. On a mainline
kernel with `VIDEO_MESON_VDEC` and the firmware blobs, this node is what
binds `meson-vdec`. *verify* `/sys/class/video4linux/*/name` =
`meson-video-decoder` on any mainline image; it will not be the CoreELEC
node.

### Vendor CoreELEC (both lines)

CE21 board file `K49/.../g12b_s922x_beelink_gs_king_x.dts` and CE22
`CD515/.../g12b_s922x_beelink_gs_king_x.dts` are thin overlays on
`g12b_s922x_beelink_gt_king.dts` (fan, `coreelec-dt-id =
"g12b_s922x_beelink_gs_king_x"`). They do not describe the decoder.

The SoC include `mesong12b.dtsi` registers the **vendor** devices
(CE21 L1441–L1463, CE22 `CD515` L1463–L1485):

```dts
vdec {
    compatible = "amlogic, vdec";
    dev_name = "vdec.0";
    status = "okay";
    interrupt-names = "vsync", "demux", "parser",
                      "mailbox_0", "mailbox_1", "mailbox_2";
};

vcodec_dec {
    compatible = "amlogic, vcodec-dec";
    dev_name = "aml-vcodec-dec";
    status = "okay";
};
```

`amlogic, vdec` is the amstream / `media_modules` core. `amlogic,
vcodec-dec` is the platform device `amvdec_ports` binds to. That second
node is **present on CE22** even though the module is not built: enabling
`CONFIG_AMLOGIC_MEDIA_V4L_DEC` in a custom `media_modules` build is the
plausible way to get `/dev/video26` on CE22, not a DTS edit. *verify*
`/sys/devices` / `dmesg | grep vcodec`.

`amlogic,g12a-vdec` does **not** appear in the vendor GS-King X chain.
Turning on `VIDEO_MESON_VDEC` in the CE22 5.15 tree would compile the
staging driver and then fail to bind: wrong compatible, wrong reg layout,
wrong interrupt names. Putting a mainline vdec node into the vendor dts
while `media_modules` already owns DOS/ESPARSER/IRQs is a conflict, not a
config flip.

README already names the vendor tree
`g12b_s922x_beelink_gs_king_x` (device AppImages section). That is this
overlay, not `meson-g12b-gsking-x.dts`.

---

## 4. Mapping onto `DecoderBackend`

Today (`6d9d146`):

- `DecoderBackend::isHardware()` is required (`src/decoder_backend.h` L53).
  `FfmpegDecoder` always returns false (`src/ffmpeg_decoder.h` L15–L17, L55).
- `readFrame()` default fails so “a future DRM-prime hardware decoder stays
  linkable” (`decoder_backend.h` L64–L69). The only filled format is
  `Yuv420p` (three 8-bit planes).
- `StreamPlayer::sessionLoop` (`src/stream_player.cpp` L186–L196):

  ```cpp
  const bool hardware = opened && decoder->isHardware();
  if (!opened || hardware) {
      worker->failed = hardware ? WorkerFail::Hardware : WorkerFail::Open;
      ...
      return;
  }
  ```

  The log is `StreamPlayer: refusing hardware decoder for <id> (requires
  GS-King X / CoreELEC)` (L519–L521). Header comment:
  “a backend that reports `isHardware()` is closed”
  (`src/stream_player.h` L37).
- `copyOwned` rejects anything that is not `Yuv420p`
  (`src/stream_player.cpp` L22–L24).
- `MqttController` default factory is `FfmpegDecoder`
  (`src/mqtt_controller.cpp` L165).
- `prepareAmlogicHwaccel()` is a deliberate no-op
  (`src/ffmpeg_decoder.cpp` L55–L67). README: a hardware path must be its
  own `DecoderBackend`.
- Wall size is `LayoutEngine::kMaxStreams == 9` (`src/layout_engine.h` L57).
  One decode thread per visible tile; retired workers can hold a decoder
  until `reapFinished()` → `releaseDecoder()` → `close()` (L136–L154).

### If the backend were mainline `meson-vdec`

| Hook | What it would do | Why it fails the wall |
| --- | --- | --- |
| `open(url)` | libavformat demux (keep the existing whitelist / `abort` / HLS discard), then `open("/dev/videoN")` where `name == meson-video-decoder`, `S_FMT` H.264, `STREAMON(OUTPUT)`. | Node is missing on CoreELEC. HEVC HLS cannot bind. Second tile’s `STREAMON` is `-EBUSY`. |
| `isHardware()` | `true` after a successful `STREAMON`. | `sessionLoop` closes the backend until that guard is lifted. Lifting it for a 1-session driver still leaves tiles 2–9 on `FfmpegDecoder`. |
| `readFrame()` | `poll` + `DQBUF(CAPTURE)` + either mmap-copy NV12M→I420 or a new `Nv12` / `DrmPrime` format. | `OwnedFrame` / `SoftwareRenderer` only know `Yuv420p`. PRIME needs `DrmRenderer` work (#40, #42). |
| `abort()` | eventfd in the `poll` set; same `AVIOInterruptCB` as `FfmpegDecoder` for demux. | V4L2 `DQBUF` without `O_NONBLOCK` can ignore the latch; use `O_NONBLOCK` + poll. |
| `close()` | `STREAMOFF` both, `REQBUFS(0)`, `close(fd)` so `cur_sess` clears. | A retired worker that has not reached `close()` still owns the **only** session. `abort()` must drop the fd on the worker thread (see #42 `quiesce()`), or tile 1’s restart blocks the whole wall. |

A mixed factory (one hardware tile + eight `FfmpegDecoder`) is the only
shape `meson-vdec` can ever feed, and only on a **mainline** kernel this
repo does not ship.

### If the backend were vendor `amvdec_ports` / `libamcodec`

That is the #45 / #42 design, not this driver’s. Short mapping so the
go/no-go is comparable:

- CE21 `/dev/video26`: up to **2** hardware tiles after the patch; HEVC
  is in the vendor format table; CAPTURE is NV12/NV21; `EXPBUF` is wired.
  Still needs the `isHardware()` guard lifted, an NV12 (or copy) format,
  and instance accounting against retired workers.
- CE22: no node unless `CONFIG_AMLOGIC_MEDIA_V4L_DEC` is added. The
  shipped path is `libamcodec` + VFM, which does not return CPU frames
  (`DecoderBackend::readFrame` default). Self-presenting into `amvideo`
  cannot tile 9 streams on one video layer (#38, #40).

---

## 5. Go / no-go

| Candidate | Multi-stream (1–9) | HEVC | On CE21 / CE22 as shipped | `DecoderBackend` fit | Decision |
| --- | --- | --- | --- | --- | --- |
| **Mainline `meson-vdec`** | **No** (`-EBUSY`) | **No** on G12A/G12B | **No** (CE21 absent; CE22 source, not built, wrong DT) | Stateful V4L2 is implementable; wall is not | **No-go** for the multi-stream path. Do not grow `prepareAmlogicHwaccel()` toward `h264_v4l2m2m` on this driver. |
| **LibreELEC stateless `amvdec`** | **No** (`-EBUSY`) | Yes in that tree | **No** (wrong distro; unreviewed) | Request API ≠ current `readFrame` | **No-go** for CoreELEC targets. |
| **Vendor `amvdec_ports` `/dev/video26`** | CE21: **2**; CE22: not built (header 9 if rebuilt) | Yes in vendor tables | CE21 yes; CE22 no | Best V4L2-shaped hardware backend; needs interface work | **Go for a CE21 spike only** (max 2 tiles + software rest). CE22 needs a custom module build. Details: #45, #42. |
| **`libamcodec` / amstream / `amvdec_m*`** | Kernel yes (2 / 9); display is one video layer | Yes | **Yes on both** (what Kodi uses) | Frames do not come back as `DecodedFrame`; proprietary `.so` | **Go as the only CE22 hardware decode**, **no-go** as a drop-in `readFrame()` backend without VFM/`v4lvideo` work. Licensing: #43. |

Recommended next implementation (docs only here): keep `FfmpegDecoder` as
the nine-tile path; treat hardware as a later `DecoderBackend` on
**vendor** `amvdec_ports` (CE21) or a rebuilt CE22 module / amstream
frame ports. Do not target `meson-vdec` unless the project leaves
CoreELEC for a mainline kernel, and even then only for a single tile.

---

## 6. On-device probe checklist

Run on **both** CoreELEC lines after `scripts/launch-coreelec.sh` has
stopped Kodi. None of this has been executed. Record stdout / `dmesg`.

```sh
# identity
cat /etc/os-release | grep -E 'NAME|VERSION|ARCH|COREELEC|DISTRO'
uname -r
cat /proc/device-tree/coreelec-dt-id 2>/dev/null; echo
cat /proc/device-tree/model; echo
# *verify* CE21: COREELEC_ARCH=Amlogic-ng.arm, dt-id g12b_s922x_beelink_gs_king_x
# *verify* CE22: DISTRO_ARCH=Amlogic-no.aarch64

# which decoder stack is actually present
ls -l /dev/video* /dev/amstream* /dev/dri/* 2>&1
for d in /sys/class/video4linux/video*; do
  echo "$d name=$(cat $d/name 2>/dev/null) dev=$(cat $d/dev 2>/dev/null)"
done
lsmod | grep -E 'amvdec|meson_vdec|vdec'
# *verify* CE21: name aml-vcodec-dec on video26; no meson-video-decoder
# *verify* CE22: no video26 / no amvdec_ports.ko; amvdec_mh264 / amvdec_h265 loaded

# DT compatibles the running kernel sees
find /sys/firmware/devicetree/base -name compatible -print0 \
  | xargs -0 grep -H -E 'vdec|vcodec' 2>/dev/null
# *verify* vendor: "amlogic, vdec" and "amlogic, vcodec-dec"
# *verify* absence of "amlogic,g12a-vdec"

# firmware the staging driver would want (won't be used on CE)
ls -l /lib/firmware/meson/vdec 2>&1
ls -l /lib/firmware/video /lib/firmware/video/NO_TEE 2>&1

# CE21 amvdec_ports only
cat /sys/module/amvdec_ports/parameters/multiplanar 2>&1
# v4l-utils is not in the image recipe; skip if missing
v4l2-ctl -d /dev/video26 --info --list-formats --list-formats-out 2>&1

# instance cap: open N OUTPUT STREAMON sessions (small H.264 Annex-B)
# expect CE21 fail at session 3 (ida, MAX_INSTANCE_MUN=2)
# expect meson-vdec (if ever bound) fail at session 2 with EBUSY on STREAMON
# expect CE22 /dev/video26 missing
dmesg | grep -E 'vdec_create|request id failed|meson-video|aml-vcodec|EBUSY'

# shared IRQ / one-block-at-a-time (only if meson-vdec is ever loaded)
# start H.264 STREAMON, then VP9 STREAMON on a second fd → EBUSY
```

Two-process `STREAMON` sketch (host tool, not this repo). *verify*:

```sh
# after v4l2-ctl confirms a meson-video-decoder node (mainline image only)
v4l2-ctl -d /dev/video0 --set-fmt-video-out=pixelformat=H264 --stream-mmap --stream-count=1 &
pid=$!
sleep 1
v4l2-ctl -d /dev/video0 --set-fmt-video-out=pixelformat=H264 --stream-mmap --stream-count=1
# expect: EBUSY (or a silent hang / KASAN if the unlocked claim races)
kill $pid 2>/dev/null
```

On stock CoreELEC that second command should fail earlier: no
`meson-video-decoder` node.

---

## What this does **not** decide

- Whether CE22 should grow `CONFIG_AMLOGIC_MEDIA_V4L_DEC` (product
  request to CoreELEC, or a matching out-of-tree `amvdec_ports.ko`).
- Whether `StreamPlayer` should accept `isHardware()` (needed for any
  hardware backend; not specific to `meson-vdec`).
- Display composition for nine NV12 tiles (#40).
- Throughput / CMA for 9×1080p (#38).

Those stay with the sibling notes. The only decision this file is meant
to lock: **do not implement the multi-stream hardware path on
`meson-vdec`.**
