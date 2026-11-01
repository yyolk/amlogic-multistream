# Amlogic hardware decode to DRM/KMS on CoreELEC

Status: research and proposed design, not implemented or validated on a
GS-King X. Claims that still need an on-box check are marked `*verify*`.

Target: Beelink GS-King X (Amlogic S922X/G12B). CoreELEC 22 `Amlogic-no`
(64-bit, vendor 5.15) is the hardware-display target. CoreELEC 21
`Amlogic-ng` (32-bit, vendor 4.9) has no stock Amlogic DRM and is a
different display architecture.

This note deepens the first DRM/KMS research pass: plane inventory,
`DRM_FORMAT_MOD_AMLOGIC_FBC` (AM21C/AM21B), 1–9 tile composition, dma-buf
import vs the current `DecodedFrame` token, the CoreELEC 21 amvideo/vout
path, and DRM master after the launch script stops Kodi.

## Conclusion: concrete display architecture

| Line | Display owner after Kodi stop | 1–9 zero-copy grid? | Recommended architecture |
| --- | --- | --- | --- |
| CoreELEC 22 `Amlogic-no` | This process becomes DRM master on `/dev/dri/card0` and uses atomic KMS | Yes, as GPU/GE2D composition onto **one** OSD framebuffer. Not as nine native video planes. | Decode into application-allocated UVM dma-bufs; prefer **linear NV12/NV21**; import into GBM/EGL; blit all tiles into one RGB scanout; atomic-commit that FB on one OSD/primary plane. |
| CoreELEC 21 `Amlogic-ng` | No stock Amlogic DRM. The vendor `amvideo`/`vout` path self-presents `vframe`s | No. Hardware can place at most **two** video layers (VD1 + videopip/VD2). | Keep the current software wall. A 1-stream, or at most 1+PIP, hardware experiment can use `/dev/video10` plus `/sys/class/video/axis` and `axis_pip` after Kodi exits. Do not build a nine-tile KMS compositor on this kernel. |

Zero-copy here means decoded pixels are never mapped, swscaled, memcpy'd, or
read back by the CPU. Composition still writes one final scanout buffer.
That is not **direct scanout**.

Do not implement the hardware path by growing `FfmpegDecoder` or stuffing
dma-buf fds into `DecodedFrame`. `StreamPlayer` currently fails any
`isHardware() == true` backend and copies CPU planes into `OwnedFrame`.
The player, frame token, and presenter have to change together.

## Scope and terminology

There are two different meanings of “plane”:

- a **KMS plane** is a display layer that a CRTC blends into an output; and
- an **image plane** is one component of a pixel buffer, such as the Y and
  interleaved UV parts of NV12.

This document uses “plane” alone for a KMS plane and says “image plane” for
pixel-buffer components.

Protected or secure video is out of scope. Such buffers may reject export,
mapping, or import.

## Do not treat the three Amlogic stacks as one API

| Stack | Relevant behavior | Implication for this project |
| --- | --- | --- |
| CoreELEC 21 `Amlogic-ng`, vendor 4.9 | 32-bit userspace; `CONFIG_AMLOGIC_DRM` is not set; `DISPLAYSERVER="no"`; `KODIPLAYER_DRIVER="libamcodec"`; presentation is `vframe`/`amvideo`/`vout` | The DRM/KMS design below is unavailable on the stock image. Keep the software AppImage. Hardware display is an amvideo experiment, not a nine-tile compositor. |
| CoreELEC 22 `Amlogic-no`, vendor 5.15 | 64-bit userspace; `CONFIG_DRM_MESON` is unset; `CONFIG_AMLOGIC_DRM=y`; Kodi `libamcodec`; external `media_modules-aml` V4L2 mem2mem whose capture queue imports dma-bufs | First hardware target. Source contains a no-CPU-copy path, subject to runtime module and node verification (`*verify*`). |
| Upstream Linux `drm/meson` | One RGB primary (OSD1) and one YUV overlay (VD1); FBC modifiers on the overlay | Useful for modifier and atomic concepts. Not the CoreELEC 22 plane inventory. |

CoreELEC 22's in-tree `CONFIG_AMLOGIC_V4L2` option is disabled. The decoder
examined here comes from `media_modules-aml`. Kernel config, loaded
modules, and actual device nodes all need to be checked on the box
(`*verify*`).

## 1. Plane inventory on GS-King X

Physical G12B/S922X display hardware has two video layers and three graphics
layers. That is an upper bound, not a promise that five arbitrary buffers
can be scaled and blended at once.

### CoreELEC 22 vendor Meson DRM (the box we care about)

The GS-King X device tree includes the G12B DRM topology
(`g12b_s922x_beelink_gs_king_x.dts` → GT King → `g12b_a311d_w400_linux.dts`
→ `mesong12b_drm_dual_disp.dtsi` → `mesong12b_drm.dtsi`).

`mesong12b_drm.dtsi` for CRTC0 / HDMI describes:

- OSD1, OSD2, OSD3 (RGB/AFBC graphics);
- a three-input OSD blend path;
- video1 and video2 (YUV); and
- `vfm_mode = <1>` (“composer mode”).

`crtcmask_of_osd = <0 0 0>` and `crtcmask_of_video = <0 0>` attach those
five layers to CRTC 0. Dual-display can add an OSD on CRTC 1; it does not
create nine HDMI video planes.

The plane driver creates:

- one KMS plane per instantiated OSD block (`osd0`…), first free OSD on
  each CRTC marked `DRM_PLANE_TYPE_PRIMARY`, the rest `OVERLAY`;
- one KMS plane per instantiated video block (`video0`…), always
  `DRM_PLANE_TYPE_OVERLAY`.

Driver maxima are `MESON_MAX_OSD = 4` and `MESON_MAX_VIDEO = 3`. The G12B
DTS instantiates three OSD blocks and two video blocks on HDMI. Expected
userspace view after `DRM_CLIENT_CAP_UNIVERSAL_PLANES` (`*verify*` on box):

| Plane | Type | Native pixel formats | Role for this wall |
| --- | --- | --- | --- |
| `osd0` (typical primary) | RGB primary | XRGB/ARGB/RGB888 family, optional ARM AFBC | Final composed scanout |
| `osd1`, `osd2` | RGB overlay | same OSD list | Optional UI/chrome, not extra YUV tiles |
| `video0`, `video1` | YUV overlay | `NV12`, `NV21`, `UYVY`, `VUY888` plus Amlogic FBC modifiers | 1-stream, maybe 2-stream, direct-scanout proof only |

OSD planes do not advertise NV12. A decoded YUV buffer cannot be placed on
an OSD plane without a color conversion. Video planes do not give nine
independent tiles.

Runtime enumeration is authoritative: `possible_crtcs`, `IN_FORMATS` and
modifiers, `zpos`, alpha, color encoding/range, fences, and source/dest
size limits. The DTS is topology, not a bandwidth guarantee (`*verify*`).

### Upstream `drm/meson` (do not use as the CE22 ABI)

Current upstream source exposes:

- one RGB primary (`meson_plane.c`), immutable `zpos = 1`, must cover the
  CRTC, up to 5× upscale, no downscale;
- one YUV overlay (`meson_overlay.c`), immutable `zpos = 0`, linear
  `YUYV`/`NV12`/`NV21`/planar YUV plus FBC-only `YUV420_8BIT`/`YUV420_10BIT`,
  scale 1/5× through 5×.

The older kernel-doc “2 OSD + 2 video” diagram is stale relative to both
G12 hardware and current upstream code.

### CoreELEC 21 stock

`CONFIG_AMLOGIC_DRM` is not set. There is no meson KMS plane inventory to
query. Display is the vendor `amvideo` sink described in section 5.

## 2. `DRM_FORMAT_MOD_AMLOGIC_FBC` (AM21C / AM21B)

AM21C (and the older AM21B name) is Amlogic's internal label for lossless
video-framebuffer compression. Upstream and vendor DRM call the same family
`DRM_FORMAT_MOD_AMLOGIC_FBC`. Chewitt's meson-vdec work renamed in-driver
`am21c_*` helpers to `amfbc_*` to match that UAPI. Treat AM21C/AM21B as
vendor names, not extra fourccs.

### What the modifier is

The UAPI in `include/uapi/drm/drm_fourcc.h` (Linux 5.9+, commit
`d6528ec88309`, later parenthesis fix `da3a9e9a6aa9`) encodes:

```text
DRM_FORMAT_MOD_AMLOGIC_FBC(layout, options)
  layout  BASIC   (1)  64x32 body + 32-byte/128x64 header; transferrable
          SCATTER (2)  header holds IOMMU refs; not dumpable, not mmap'able
  option  MEM_SAVING (bit 0) packed 8-bit superblocks
```

The compressed storage is a **single image plane** whose fourcc is
`DRM_FORMAT_YUV420_8BIT` or `DRM_FORMAT_YUV420_10BIT`. It is not linear
NV12. Scatter buffers are hardware-producer/consumer only; userspace should
expect `mmap` of the dma-buf to fail.

Neil Armstrong documented the G12 family mapping as:

| SoC generation | 8-bit | 10-bit |
| --- | --- | --- |
| GX | `YUV420_8BIT` + FBC(BASIC, 0) | `YUV420_10BIT` + FBC(BASIC, 0) |
| G12 / S922X | `YUV420_8BIT` + FBC(SCATTER, MEM_SAVING) | `YUV420_10BIT` + FBC(SCATTER, MEM_SAVING) |

That is the expected S922X compressed layout (`*verify*` against the actual
capture format the CE22 decoder produces).

### Which kernels expose it

| Tree | Exposed? |
| --- | --- |
| Upstream Linux 5.9+ `drm/meson` overlay | Yes: BASIC/SCATTER × optional MEM_SAVING, plus LINEAR |
| CoreELEC 22 vendor `meson_plane.c` video planes | Yes: the same four FBC modifiers plus LINEAR on `video_fbc_modifier[]` |
| CoreELEC 21 vendor 4.9 | No KMS fourcc path. Compression stays inside `vframe`/`amvideo` |

Vendor video fourccs are still listed as linear `NV12`/`NV21`/`UYVY`/`VUY888`.
FBC is a modifier on those planes, and on upstream it is paired with the
special `YUV420_{8,10}BIT` fourccs. Do not assume every advertised pair
works; `DRM_MODE_ATOMIC_TEST_ONLY` is the check (`*verify*`).

### Chewitt / LibreELEC FFmpeg mapping

LibreELEC's Amlogic FFmpeg is chewitt's `amlogic-*` branch. Two different
things get conflated:

1. **LibreELEC `v4l2-drmprime` patch** (generic). Makes `*_v4l2m2m` advertise
   `AV_PIX_FMT_DRM_PRIME` and fills `AVDRMFrameDescriptor`. The LE 10.0
   patch sets `format_modifier = DRM_FORMAT_MOD_LINEAR`. Stock FFmpeg
   `h264_v4l2m2m` without that patch outputs CPU `nv12`, so a DRM-PRIME
   renderer never sees the hardware frame.
2. **Amlogic FBC-aware mapping** (chewitt / meson-vdec discussions). Compressed
   capture is described as `YUV420_8BIT`/`YUV420_10BIT` plus
   `DRM_FORMAT_MOD_AMLOGIC_FBC(...)`, not as linear NV12. That mapping is
   **not** in stock FFmpeg 6.0.1 / 9.0.2, which is what the CoreELEC
   device AppImages link.

This project should not depend on a chewitt FFmpeg rebuild to set a
modifier. Prefer the vendor V4L2/UVM path and attach fourcc + modifier
from the negotiated capture format and SoC table above. If an FFmpeg
`DecoderBackend` is added later, it is a separate, forked-FFmpeg decision.

### Can a stock libdrm client set the modifier?

Yes. The modifier is a `uint64_t`. `drmModeAddFB2WithModifiers` (libdrm)
and `DRM_IOCTL_MODE_ADDFB2` with `DRM_MODE_FB_MODIFIERS` accept it. This
repo already pins libdrm **2.4.124**, whose `drm_fourcc.h` has
`DRM_FORMAT_MOD_AMLOGIC_FBC`. No LibreELEC-forked libdrm is required.

What a fork does **not** buy:

- a plane that did not advertise the modifier will still reject the FB;
- Mali/EGL will not suddenly sample SCATTER FBC;
- CoreELEC 21 still has no KMS.

What still needs a fork or custom code is **producing** an
`AV_PIX_FMT_DRM_PRIME` frame with the FBC modifier from stock FFmpeg
v4l2m2m. Avoid that path.

**Recommendation:** negotiate **linear NV12/NV21** capture for the wall so
EGL/GE2D can import it. Use FBC only for a single-plane direct-scanout
experiment on `video0`, and only after `TEST_ONLY` succeeds (`*verify*`).
Scatter FBC is the wrong default for a nine-tile compositor.

## 3. Composition for 1–9 tiles

Hardware cannot assign one YUV KMS plane per stream. Even the generous
CE22 DTS has two video planes. Upstream meson has one. CE21 amvideo has
VD1 + PIP.

| Strategy | Streams | CPU copy? | Assessment |
| --- | --- | --- | --- |
| Direct video-plane scanout | 1, maybe 2 on CE22 | No | Proof that UVM, modifier, scaling, and fences agree. Not the wall. |
| VD1 + OSD hybrid (YUV under RGB with holes) | 2 | No for the VD1 tile | Extra path, alpha/`zpos`/`*verify*`. Still not nine tiles. |
| GBM/EGL GPU blit to one OSD FB | 1–9 | No decoded-pixel CPU copy | **CE22 recommended.** One KMS plane, existing 1x1–3x3 layout stays compositor math. |
| GE2D blit to one RGB/OSD FB | 1–9 if imports work | No if dma-buf I/O works | Useful CE22 alternative; only realistic CE21 compose engine. |
| CPU `swscale`/memcpy | 1–9 | Yes | Fallback only. Cost below. |

### GBM / EGL GPU blit (CE22 default)

1. Import each retained decoder dma-buf with
   `EGL_EXT_image_dma_buf_import` and, when the modifier is not linear,
   `EGL_EXT_image_dma_buf_import_modifiers`.
2. Draw nine (or fewer) textured quads with aspect-fit into a GBM
   `GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING` buffer.
3. Create or reuse a KMS FB for that BO and atomic-commit it on the
   primary OSD plane.

Mali-G52 / CoreELEC GLES support for the exact fourcc+modifier is
unproven (`*verify*`). Linear NV12 is the conservative import. Vulkan is
the second choice: PanVK on Bifrost is weaker at
`VK_EXT_image_drm_format_modifier` and YCbCr conversion.

One wall-level transaction per tick: collect the latest retained frame
per visible stream, draw the whole grid, submit one scanout. Per-decoder
KMS commits would tear and race lifetimes.

### GE2D — it does take YUV

The vendor GE2D driver (`/dev/ge2d`) accepts `AML_GE2D_MEM_DMABUF` and
defines YUV source formats including `GE2D_FORMAT_M24_NV12`,
`GE2D_FORMAT_M24_NV21`, `GE2D_FMT_M24_YUV420`, and `GE2D_FORMAT_S16_YUV422`.
It stretch-blits and color-converts into RGB/YUV destinations.

That is enough, on paper, to compose linear NV12 tiles into one RGB
buffer without the GPU. It does **not** advertise Amlogic FBC/AM21C as a
GE2D source format. GE2D is therefore a linear-YUV compositor, not an FBC
decoder.

Use GE2D if EGL import of UVM dma-bufs fails, or as the CE21 compose
engine toward fbdev. Prove dma-buf fd import, stretch to each
`LayoutEngine` rect, and completion signaling on the box (`*verify*`).
The ioctl surface is vendor-specific.

### CPU fallback cost

A 1920×1080 NV12 frame is 3,110,400 bytes before alignment. Nine such
streams at 30 fps is about **840 MiB/s** of source traffic. BT.601
conversion to RGB888 adds about **710 MiB/s** of writes, plus cache
churn. `SoftwareRenderer` already does this for one stream in host tests.

S922X's little cores will not sustain nine 1080p30 conversions plus HLS
demux. A CPU fallback is a bring-up aid (one 720p tile, or PPM dumps),
not a product wall. If EGL and GE2D both fail to import, stop and fix
import; do not silently copy nine decoded frames.

## 4. DMA-BUF import path and the `DecodedFrame` token

### PRIME fd → GEM → FB → plane

For a buffer that KMS will scan out (direct video plane, or the final GBM
RGB buffer):

1. `drmPrimeFDToHandle` each dma-buf fd (one handle per distinct fd);
2. `drmModeAddFB2WithModifiers` with the real fourcc, modifier, handles,
   pitches, and offsets;
3. atomic `FB_ID`, `CRTC_ID`, `SRC_*` (16.16), `CRTC_*`;
4. cache GEM handle + FB id for that buffer identity.

`TEST_ONLY | ALLOW_MODESET` before the first real layout commit.

Teardown order: disable the plane, wait for the commit, `RmFB`,
`GEM_CLOSE`, then close or recycle the dma-buf. Do not import/export per
frame.

### Multi-fd / multi-image-plane

| Capture style | fds | KMS / EGL notes |
| --- | --- | --- |
| UVM single-fd NV12 (CE22 `v4lplayer` style) | 1 | UV at `offsets[1]`; same handle twice for `AddFB2` if the driver wants two image planes |
| Upstream meson-vdec `NV12M` | 2 (`EXPBUF` per image plane) | Two PRIME imports; `handles[0]`, `handles[1]`, pitches/offsets per image plane |
| Amlogic FBC | 1 | Single-plane `YUV420_8BIT`/`_10BIT` + FBC modifier; not a 2-image-plane NV12 FB |

A fd is not an image. The descriptor must carry fourcc, modifier, image-plane
count, stride, and offset. Guessing linear tightly packed NV12 will corrupt
or overread (`*verify*` the real capture layout).

### Lifetime vs `DecodedFrame`

`DecodedFrame` is a borrowed CPU view: pointers die on the next
`readFrame()` or `close()`. `StreamPlayer::OwnedFrame` copies those bytes
so the decode thread can continue. That token cannot represent a dma-buf.

Hardware needs a **refcounted token** (call it `HardwareFrame`) that:

- owns or borrows the dma-buf allocation(s);
- carries fourcc, modifier, image-plane layout, color, and PTS;
- stays dequeued from V4L2 while any EGL/GE2D/GEM/FB/KMS user exists;
- returns the capture slot to `VIDIOC_QBUF` only after the last consumer
  drops it **and** it is no longer the retained tile.

State machine:

```text
FREE → QUEUED_TO_DECODER → DEQUEUED_READY
    → IMPORTED_OR_RENDERING → RETAINED_BY_CURRENT_WALL
    → RELEASE_PENDING → QUEUED_TO_DECODER
```

Do not `QBUF` because the compositor *submitted* the frame. Do not reuse a
scanout BO until its out-fence or page-flip completes. Prefer
`IN_FENCE_FD` / `OUT_FENCE_PTR` when the plane/CRTC expose them (Kodi's
`AMLDisplay` already uses both on CE22). Otherwise honor implicit dma-buf
fences and flip events. Never sleep.

Capture-pool size is V4L2 min/DPB plus compositor retain (at least two
displayed tiles plus one in-flight). Nine streams of 1080p with a deep DPB
can consume hundreds of MiB (`*verify*` CMA/UVM headroom).

## 5. CoreELEC 21 gap: amvideo / vout, not KMS

Stock CE21 disables Amlogic DRM. After `systemctl stop kodi` there is
still no meson master to take. The working hardware path is the same one
Kodi already uses: decoder `vframe`s flow through VFM into `amvideo`,
which programs VD1/VD2 and `vout`.

Kodi's `AMLCodec` opens `/dev/video10`. `VIDIOC_DQBUF` returns an index
and timing, not an image dma-buf. `RendererAML` calls `SetVideoRect` and
requeues; `/sys/class/video/axis` is the window. `VIDIOC_EXPBUF` is not
the contract. Requeueing participates in vendor display lifetime.

The 4.9 `amvideo` class exposes, among others:

- `/sys/class/video/axis` — VD1 destination rectangle;
- `/sys/class/video/axis_pip` — PIP/VD2 rectangle;
- `/sys/class/video/disable_video`;
- crop, zoom, screen_mode, blackout_policy.

The sink has two receivers (`amvideo` and `videopip`). That is **two**
hardware video layers, matching VD1+VD2. There is no nine-layer amvideo
grid API.

### Can amvideo do a grid?

| Wanted | CE21 amvideo? |
| --- | --- |
| One full-screen hardware stream | Yes: one decoder, set `axis`, let amvideo present |
| Two hardware tiles | Maybe: second decoder into `videopip` + `axis_pip` (`*verify*` VFM map and whether CE21 Kodi/userspace enables PIP) |
| 3–9 hardware tiles | No native path |
| 3–9 by composing into one buffer | Only if GE2D or CPU writes a framebuffer that `vout`/fbdev scans out. That is not the Kodi `vframe` contract, and FBC `vframe`s are not CPU images |

A userspace client that only pokes `axis` still needs *some* provider to
push `vframe`s into the VFM graph. Copying Kodi's `/dev/video10` loop can
feed **one** layer. A second layer needs a second provider and a VFM
route into `videopip`. There is no supported way to attach nine decoder
instances to nine rectangles.

**CE21 recommendation:** do not promise a zero-copy 1–9 grid on stock
21.3. Ship software decode as today. If a device demo is required on CE21,
gate a 1-stream (optional PIP) amvideo experiment behind the existing
Kodi stop/start trap, and document it as a dead-end for the nine-tile
goal. The nine-tile wall is a CE22 project.

## 6. Kodi stopped: who owns DRM, atomic vs legacy

`scripts/launch-coreelec.sh` already does exclusive handoff: `systemctl
stop kodi`, run the player, start Kodi on EXIT/signal. That is the
correct outer lifecycle on both lines. The script does not program planes
or amvideo today.

### CoreELEC 22

Kodi's `AMLDisplay` opens the primary node, enables universal planes +
atomic, picks the HDMI connector/CRTC and a `DRM_PLANE_TYPE_PRIMARY`,
calls `drmSetMaster()`, modesets with `DRM_MODE_ATOMIC_ALLOW_MODESET`,
and presents the GBM GUI BO with non-blocking atomic commits using
`IN_FENCE_FD` / `OUT_FENCE_PTR`. Video pixels are *not* on that plane;
they go through libamcodec/amvideo.

While Kodi holds master:

- this process can open `card0` and query (as `DrmRenderer` already does);
- `drmSetMaster` returns `EBUSY`;
- atomic/legacy modeset and plane commits return `EACCES`;
- `renderD*` can render but cannot KMS;
- root does not steal master.

After Kodi exits it drops master (orderly path calls `drmDropMaster`).
The player must:

1. wait until `card0` has no master (`*verify*` with `fuser` / a failed
   then successful `drmSetMaster`);
2. open the primary node, set `UNIVERSAL_PLANES` + `ATOMIC`, `drmSetMaster`;
3. enumerate, pick the live HDMI connector/CRTC/mode, atomic modeset;
4. run the wall;
5. disable planes, wait, drop FBs/GEM/dma-bufs, `drmDropMaster`, close;
6. let the launch script start Kodi, which allocates **new** FB ids.

Do not restore Kodi's old FB ids. Do not loop on failed commits. Do not
use legacy `drmModeSetCrtc` / `drmModeSetPlane` on CE22: Kodi and the
vendor driver are atomic; mixed legacy/atomic clients fight global state.

DRM leases need a cooperating master and a spare connector/CRTC/plane.
CE22 Kodi has no lease broker, and HDMI is one CRTC. Render nodes are not
a display path. Logind does not own Kodi's fd on CoreELEC.

### CoreELEC 21

Stopping Kodi frees `amvideo`/`vout` and `/dev/video10`, not a DRM
master. Opening `/dev/dri/card0` is expected to fail or to be a
non-Amlogic stub (`*verify*`). Atomic vs legacy is moot. The player
should fail closed if it asked for KMS on CE21, or take the amvideo
branch explicitly.

## Proposed architecture in this repository

Keep `DecodedFrame`, `FfmpegDecoder`, and `SoftwareRenderer` for host
tests and CE21 software. Add device-only types, compiled for the CE22
target, failing closed when capabilities differ:

```text
AmlogicV4L2DecoderBackend    CE22; UVM + V4L2_MEMORY_DMABUF capture
HardwareFrame                refcounted dma-buf token (not DecodedFrame)
WallCompositor               EGL first, GE2D fallback; 1–9 tiles → ScanoutBuffer
KmsOutput                    DRM master, atomic commits, one OSD FB
```

CE21 optional later, not on the critical path:

```text
AmvideoPresenter             one (maybe two) vframe layer(s) + sysfs axis
```

`StreamPlayer` must accept `isHardware()`, retain `HardwareFrame`s instead
of copying YUV, and present a wall transaction instead of
`present(frame, rect)` per stream.

## Device discovery

Run with Kodi up and again after `systemctl stop kodi`:

```sh
cat /etc/os-release
uname -a
readlink -f /sys/class/drm/card0/device/driver
ls -l /dev/dri /dev/video* /dev/media* /dev/uvm /dev/ge2d
fuser -v /dev/dri/card0 /dev/video10

# CE22 KMS
modetest -M meson -c -p
drm_info /dev/dri/card0
cat /sys/kernel/debug/dri/0/state
cat /sys/kernel/debug/dma_buf/bufinfo

# Decoder
v4l2-ctl --list-devices
# replace video26 after enumeration
v4l2-ctl -d /dev/video26 --all
v4l2-ctl -d /dev/video26 --list-formats --list-formats-out

# CE21 amvideo
ls -l /sys/class/video
cat /sys/class/video/axis /sys/class/video/axis_pip
cat /sys/class/vfm/map
```

CoreELEC may not ship `modetest`, `drm_info`, or `v4l2-ctl`. Use temporary
static tools. Do not hardcode `/dev/video26`.

Also capture plane `IN_FORMATS`/modifiers, EGL extension lists, V4L2
`TRY_FMT`/`S_FMT`/min buffers, one UVM→EGL and one UVM→GE2D import, and
atomic `TEST_ONLY` for linear NV12 and FBC on `video0` (`*verify*`).

## Gated proof plan

CE22, each stage on the GS-King X before the next:

1. **Master:** Kodi stopped, `drmSetMaster`, RGB GBM flip-flop, drop
   master, Kodi returns.
2. **One decoder:** V4L2 node → UVM dma-bufs; record fourcc/modifier/
   strides (`*verify*`).
3. **One import:** EGL (then GE2D) samples a linear NV12 dma-buf into an
   RGB scanout with no CPU map.
4. **Direct plane (optional):** one UVM FB on `video0`, linear then FBC
   `TEST_ONLY`. Failure here does not kill stage 3.
5. **Composed wall:** 1, 2, 4, 9 synthetic dma-bufs, then real decoders.
6. **Stress:** codec/resolution/fps matrix, layout changes, EOS, memory,
   thermal, repeated Kodi handoff.

CE21, only if someone insists on hardware on 21.3:

1. Kodi stopped, one `/dev/video10` stream, `axis` full screen.
2. Optional second stream on `videopip` (`*verify*`).
3. Stop. Nine tiles wait for CE22.

## Open questions (`*verify*`)

- Does the installed CE22 image register the `media_modules-aml` mem2mem
  node, and which `/dev/videoN` is it?
- Capture fourcc/modifier per codec and bit depth on S922X.
- Does CoreELEC Mali EGL import UVM NV12 without a staging allocation?
- Does `/dev/ge2d` accept those fds and stretch-blit all nine rects?
- Which fence properties exist on GS-King X vendor DRM?
- CMA/UVM remaining with nine DPBs plus three scanout BOs.
- Does `video1` accept a second scaled NV12 at the same time as `video0`?
- After `systemctl stop kodi` on CE21, is `/dev/dri/card0` absent, and is
  `videopip` usable from a non-Kodi process?

## Sources

Kernel interfaces:

- [Buffer allocation and exchange (dma-buf)](https://docs.kernel.org/userspace-api/dma-buf-alloc-exchange.html)
- [dma-buf framework](https://docs.kernel.org/driver-api/dma-buf.html)
- [DRM userspace API: primary nodes, master, render nodes, and leases](https://docs.kernel.org/gpu/drm-uapi.html)
- [DRM/KMS and atomic state](https://docs.kernel.org/gpu/drm-kms.html)
- [Meson DRM hardware overview](https://docs.kernel.org/gpu/meson.html)
- [Upstream `drm_fourcc.h` Amlogic FBC modifiers](https://github.com/torvalds/linux/blob/master/include/uapi/drm/drm_fourcc.h)
- [Upstream Meson RGB plane](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/meson/meson_plane.c)
- [Upstream Meson YUV overlay](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/meson/meson_overlay.c)
- [FBC modifier added: `d6528ec88309`](https://github.com/torvalds/linux/commit/d6528ec883096e7ccdb08257bcc45670bc878519)
- [FBC macro parenthesis fix](https://lists.openwall.net/linux-kernel/2020/07/27/215)
- [G12 FBC fourcc/modifier mapping (Armstrong, Jan 2023)](https://lists.infradead.org/pipermail/linux-amlogic/2023-January/014991.html)
- [drm/meson FBC series](https://lists.freedesktop.org/archives/dri-devel/2020-May/264754.html)

CoreELEC kernel configurations and vendor sources (pinned
`5.15.196_20260225` where a snapshot is named):

- [CoreELEC 21 Amlogic-ng kernel config](https://github.com/CoreELEC/CoreELEC/blob/coreelec-21/projects/Amlogic-ce/devices/Amlogic-ng/linux/linux.aarch64.conf)
- [CoreELEC 22 Amlogic-no kernel config](https://github.com/CoreELEC/CoreELEC/blob/coreelec-22/projects/Amlogic-ce/devices/Amlogic-no/linux/linux.aarch64.conf)
- [CoreELEC 21 device options (`libamcodec`, `DISPLAYSERVER=no`)](https://github.com/CoreELEC/CoreELEC/blob/coreelec-21/projects/Amlogic-ce/devices/Amlogic-ng/options)
- [CoreELEC 22 device options](https://github.com/CoreELEC/CoreELEC/blob/coreelec-22/projects/Amlogic-ce/devices/Amlogic-no/options)
- [Meson DRM plane formats, FBC modifiers, UVM import](https://github.com/CoreELEC/common_drivers/blob/5.15.196_20260225/drivers/drm/meson_plane.c)
- [G12B DRM topology](https://github.com/CoreELEC/common_drivers/blob/5.15.196_20260225/arch/arm64/boot/dts/amlogic/mesong12b_drm.dtsi)
- [G12B dual-display additions](https://github.com/CoreELEC/common_drivers/blob/5.15.196_20260225/arch/arm64/boot/dts/amlogic/mesong12b_drm_dual_disp.dtsi)
- [GS-King X device tree](https://github.com/CoreELEC/common_drivers/blob/5.15.196_20260225/arch/arm64/boot/dts/amlogic/g12b_s922x_beelink_gs_king_x.dts)
- [GE2D YUV/dma-buf formats](https://github.com/CoreELEC/common_drivers/blob/5.15.196_20260225/include/linux/amlogic/media/ge2d/ge2d.h)
- [GE2D `/dev/ge2d` node](https://github.com/CoreELEC/common_drivers/blob/5.15.196_20260225/drivers/media/common/ge2d/ge2d_main.c)
- [Amlogic V4L2 decoder queue setup](https://github.com/CoreELEC/media_modules-aml/blob/5.15.196_20260225/drivers/amvdec_ports/aml_vcodec_dec.c)
- [`v4lplayer` V4L2 dma-buf sample](https://github.com/CoreELEC/media_modules-aml/blob/5.15.196_20260225/tools/v4lplayer/v4l2_dec.c)
- [`v4lplayer` UVM allocator](https://github.com/CoreELEC/media_modules-aml/blob/5.15.196_20260225/tools/v4lplayer/aml_uvm.c)
- [CE 4.9 `amvideo` class (`axis`, `axis_pip`)](https://github.com/CoreELEC/linux-amlogic/blob/amlogic-4.9-20/drivers/amlogic/media/video_sink/video.c)
- [CE22 vendor `amvideo` class (still used by Kodi)](https://github.com/CoreELEC/common_drivers/blob/5.15.196_20260225/drivers/media/video_sink/video.c)

CoreELEC Kodi:

- [Kodi `AMLCodec` (`/dev/video10`)](https://github.com/CoreELEC/xbmc/blob/aml-5.15.196-22.0/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp)
- [Kodi `RendererAML`](https://github.com/CoreELEC/xbmc/blob/aml-5.15.196-22.0/xbmc/cores/VideoPlayer/VideoRenderers/HwDecRender/RendererAML.cpp)
- [Kodi `AMLDisplay` DRM master and atomic presentation](https://github.com/CoreELEC/xbmc/blob/aml-5.15.196-22.0/xbmc/windowing/amlogic/AMLDisplay.cpp)

FFmpeg / LibreELEC context (mapping evidence, not this repo's decoder):

- [LibreELEC FFmpeg package (chewitt Amlogic branch)](https://github.com/LibreELEC/LibreELEC.tv/blob/master/packages/multimedia/ffmpeg/package.mk)
- [LibreELEC 10 `v4l2-drmprime` patch (sets `DRM_FORMAT_MOD_LINEAR`)](https://raw.githubusercontent.com/LibreELEC/LibreELEC.tv/libreelec-10.0/packages/multimedia/ffmpeg/patches/v4l2-drmprime/ffmpeg-001-v4l2-drmprime.patch)

Historical/community context (not an ABI or throughput guarantee):

- [CoreELEC hardware-decoding experiments](https://discourse.coreelec.org/t/experiments-with-hw-decoding/16349)
- [CoreELEC multidecode discussion](https://discourse.coreelec.org/t/multidecode/18514)
- [CoreELEC 9.2.2 multi-decoder note](https://github.com/CoreELEC/CoreELEC/releases/tag/9.2.2)
