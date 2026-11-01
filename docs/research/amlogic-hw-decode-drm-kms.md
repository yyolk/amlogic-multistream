# Amlogic hardware decode to DRM/KMS on CoreELEC

Status: research and proposed design, not implemented or validated on a GS-King X.

Target: Beelink GS-King X (Amlogic S922X/G12B), primarily CoreELEC 22
`Amlogic-no` (64-bit userspace, vendor 5.15 kernel). CoreELEC 21
`Amlogic-ng` uses a different 32-bit userspace and vendor 4.9 media stack and
must be treated as a separate port.

## Conclusion

The proposed CoreELEC 22 design is:

1. stop Kodi and take DRM master;
2. decode each stream with the Amlogic V4L2 mem2mem decoder into
   application-allocated UVM dma-bufs;
3. import those dma-bufs into a GPU or GE2D compositor without mapping them
   on the CPU;
4. render all active tiles into one double- or triple-buffered RGB scanout
   surface; and
5. present that surface on one DRM/KMS OSD plane with atomic commits.

This is normally called a zero-copy media path: decoded pixels are never
copied through or read back by the CPU. It is not *direct scanout*, because
composition writes each frame once into a final display buffer.

One decoded stream per KMS plane is not a general 1–9-stream solution. The
CoreELEC 22 G12B display topology describes three RGB OSD planes and two YUV
video planes on the HDMI CRTC. Only the two video planes are natural direct
scanout candidates for decoder output, and real availability is further
limited by format, scaling, routing, z-order, and bandwidth constraints.
Runtime DRM enumeration is authoritative.

The current Kodi Amlogic decoder path is also not a reusable dma-buf export
API. It exchanges Amlogic `vframe` objects with `/dev/video10` and presents
through the vendor `amvideo` path. The separate CoreELEC 22 V4L2 decoder is
the better integration point: its capture queue accepts dma-bufs allocated
by the application, as demonstrated by CoreELEC's `v4lplayer` sample.
That source-level path is conditional on the matching external media module
and mem2mem node being present in the installed image; this has not yet been
verified on the GS-King X.

## Scope and terminology

There are two different meanings of “plane” in this area:

- a **KMS plane** is a display layer that a CRTC blends into an output; and
- an **image plane** is one component of a pixel buffer, such as the Y and
  interleaved UV parts of NV12.

This document uses “plane” alone for a KMS plane and says “image plane” for
pixel-buffer components.

“Zero-copy” here means:

- compressed packets may be copied into decoder input buffers;
- decoded image data remains in device-accessible dma-bufs;
- the CPU does not map, swscale, memcpy, or read back decoded pixels; and
- the compositor samples the decoder buffers and writes the final scanout
  buffer once.

The stronger case, where a decoder buffer is scanned out directly and no
composition write occurs, is called **direct scanout** below.

Protected or secure video is out of scope. Such buffers may deliberately
reject export, mapping, or import by an untrusted display client.

## Do not treat the three Amlogic stacks as one API

| Stack | Relevant behavior | Implication for this project |
| --- | --- | --- |
| CoreELEC 21 `Amlogic-ng`, vendor 4.9 | 32-bit userspace, legacy Amlogic media/display integration; the stock kernel config disables Amlogic DRM | The DRM/KMS design in this document is not available on the stock image. Keep the existing software build target, but treat hardware display as a separate kernel-stack port. |
| CoreELEC 22 `Amlogic-no`, vendor 5.15 | 64-bit userspace; vendor Meson DRM; Kodi `libamcodec`; an external-media-tree V4L2 mem2mem implementation whose capture queue imports dma-bufs | Recommended first hardware target. The source contains the pieces needed for a no-CPU-copy path, subject to runtime module/node verification. |
| Upstream Linux `drm/meson` | Different driver and exposed plane model from the CoreELEC vendor tree | Useful for DRM concepts, not as evidence of the CoreELEC ABI or plane inventory. |

CoreELEC 22 enables its vendor `CONFIG_AMLOGIC_DRM` implementation rather
than upstream `CONFIG_DRM_MESON`. Its in-tree `CONFIG_AMLOGIC_V4L2` option is
disabled; the decoder implementation examined here comes from
`media_modules-aml`. Therefore the kernel config, loaded modules, and actual
device nodes all need to be checked on the target image.

The upstream Meson documentation describes the hardware blocks, while
current upstream source exposes a primary RGB plane and a YUV overlay in
`meson_plane.c` and `meson_overlay.c`. Neither description should replace
enumeration of the vendor driver running on the box.

## CoreELEC 22 display resources

The CoreELEC 22 G12B DRM device-tree topology contains:

- OSD1, OSD2, and OSD3 blocks;
- a three-input OSD blend path;
- video1 and video2 blocks; and
- `vfm_mode = <1>`, documented there as “composer mode.”

The plane driver lists RGB formats for OSD planes. Its video-plane list is:

- `DRM_FORMAT_NV12`;
- `DRM_FORMAT_NV21`;
- `DRM_FORMAT_UYVY`; and
- `DRM_FORMAT_VUY888`.

The video planes list linear and Amlogic FBC modifiers. When an imported
framebuffer is backed by UVM, the driver also retrieves the associated
`vframe` metadata from the dma-buf. That is promising for buffers allocated
through `/dev/uvm`, but it does not prove that every advertised
format/modifier/scaling combination works on this board.

The GS-King X device tree includes the G12B display topology through its
board include chain. A dual-display include can add an OSD resource to the
second CRTC; it does not turn the HDMI CRTC into a nine-layer compositor.

At runtime, inspect:

- each plane's `possible_crtcs`;
- the `type`, `IN_FORMATS`, `zpos`, `alpha`, color encoding/range, and fence
  properties;
- source and destination size limits;
- which connector and CRTC drive HDMI; and
- which planes accept the exact fourcc and modifier produced by the
  decoder.

The device tree is an upper-bound topology, not a promise that five
arbitrary buffers can be scaled and blended simultaneously.

## Decoder output paths

### Kodi's current `libamcodec` path

CoreELEC 22 selects Kodi's Amlogic `libamcodec` integration. Kodi's
`AMLCodec` opens `/dev/video10`; dequeue returns a buffer index and timing
information associated with an Amlogic `vframe`, not a userspace image
dma-buf. `RendererAML` sets the video rectangle and requeues the frame for
the vendor video pipeline.

Consequences:

- `VIDIOC_DQBUF` on this path must not be assumed to return decoded image
  file descriptors;
- `VIDIOC_EXPBUF` is not established as the handoff API;
- requeueing a frame participates in the vendor display lifecycle; and
- copying Kodi's rectangle/sysfs behavior would couple this application to
  the legacy `amvideo` presentation path rather than to DRM composition.

This path may be useful as a reference for codec setup, timestamps, and
Amlogic quirks, but it is not the proposed frame-sharing contract.

### CoreELEC 22 V4L2 mem2mem path

The separate Amlogic V4L2 decoder initializes both queues with
`VB2_DMABUF | VB2_MMAP | VB2_USERPTR`; its capture queue uses
`vb2_dma_contig_memops`. CoreELEC's `tools/v4lplayer` demonstrates the
important direction of ownership:

1. userspace allocates buffers from `/dev/uvm`;
2. userspace requests and queues V4L2 capture buffers with
   `V4L2_MEMORY_DMABUF`;
3. the decoder writes decoded frames into those imported buffers; and
4. userspace receives the completed buffer index from `VIDIOC_DQBUF`.

This is not “ask the decoder to export an internal buffer.” The application
allocates each dma-buf once and gives the same fd to the decoder and the
consumer. That is still a zero-copy decoder-to-compositor handoff.

`/dev/video26` is used by the sample/source snapshot examined for this
research, but device minor numbers are not an ABI. Production code must
enumerate media/V4L2 devices, query capabilities and formats, and select the
matching decoder node.

The sample proves the intended buffer contract in the external media source;
it does not prove that a given CoreELEC release installs and registers that
decoder on the GS-King X. If no matching mem2mem node is present, `/dev/video10`
is not a substitute: enabling/building the external decoder module or changing
the image is a prerequisite.

Each completed frame needs a descriptor containing at least:

```text
dma-buf fd(s)
width and height
DRM fourcc
modifier
image-plane count
stride and byte offset for every image plane
timestamp
color primaries, transfer function, matrix, and range when known
```

A dma-buf fd by itself does not describe an image. Guessing a linear
modifier, tightly packed stride, or NV12 layout can produce corruption or
read beyond the allocation.

## dma-buf import and framebuffer creation

There are two consumers to prove independently.

### Compositor import

Preferred options, in order of implementation practicality on the target:

1. EGL/GLES import with `EGL_EXT_image_dma_buf_import` and, when needed,
   `EGL_EXT_image_dma_buf_import_modifiers`;
2. Vulkan external-memory import plus YCbCr conversion; or
3. the vendor GE2D dma-buf interface.

CoreELEC's graphics stack and Mali driver determine which EGL/Vulkan
extensions and modifiers actually work. No implementation should assume
that an advertised DRM modifier is sampleable by EGL, or that a UVM
dma-buf is accepted by every consumer. A device probe must import and render
known frames before the architecture is committed.

The compositor should keep import objects cached by buffer identity. For
each wall update it samples the newest frame for every stream, applies
aspect-fit/crop and color conversion, and draws all tiles into one GBM
scanout buffer. A stream with no new frame can keep its previous retained
buffer; an empty cell is cleared.

### KMS import

For direct scanout, or for a final GBM dma-buf not already represented by a
framebuffer:

1. import each dma-buf fd with `drmPrimeFDToHandle`;
2. create an FB with `drmModeAddFB2WithModifiers` using the real fourcc,
   modifier, handles, pitches, and offsets;
3. set `FB_ID`, `CRTC_ID`, `SRC_*`, and `CRTC_*` in one atomic request; and
4. cache the GEM handles and FB ID for the lifetime of that buffer.

Use 16.16 fixed-point values for `SRC_X`, `SRC_Y`, `SRC_W`, and `SRC_H`.
Before the first real layout commit, submit the whole state with
`DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET`.

On teardown, first remove the FB from every active plane and wait for the
commit to complete. Then remove the FB, close imported GEM handles, and
finally close or return the dma-buf. Do not create and destroy imports on
every frame.

## Grid strategy for 1–9 streams

| Strategy | Useful range | Copies/composition | Assessment |
| --- | --- | --- | --- |
| One decoded buffer per video plane | 1 stream; possibly 2 | Direct scanout | Valuable proof that UVM, format/modifier, scaling, and KMS synchronization agree. Not the wall architecture. |
| Mix video and OSD planes | Small fixed layouts | YUV streams can use video planes; OSD inputs require RGB buffers | Resource-specific and creates multiple rendering paths. It still cannot provide nine native YUV planes. |
| GPU/Vulkan composition to one OSD FB | 1–9 | One device-side composition write | Recommended general design. Stable KMS resource use and arbitrary grid/layout handling. |
| GE2D composition to one OSD FB | 1–9 if all imports and operations work | One device-side composition write | Possible lower-complexity vendor path, but format, scaling, fence, and dma-buf support need a device proof. |
| CPU conversion/copy | 1–9 | CPU map and copy | Functional fallback only; not the requested zero-copy path and unlikely to scale well. |

For a direct-plane experiment, update all active planes in one atomic commit
so a layout change cannot show a mixture of old and new geometry. Disable
unused planes in that same commit. A successful `TEST_ONLY` result is
necessary but does not establish sustained bandwidth or decoder throughput.

For the general compositor, use one primary/OSD plane for the completed wall.
That keeps KMS use constant as stream count changes and lets the existing
row-major 1x1 through 3x3 layout remain a compositor concern.

### Why a “maximum of nine decoder instances” is not a performance promise

Several codec drivers in the CoreELEC media tree define
`MAX_DECODE_INSTANCE_NUM` as 9, and CoreELEC discussions describe
multi-decoder experiments. This shows that the software was designed to
schedule multiple instances; it does not guarantee nine simultaneous
streams of an arbitrary codec, resolution, frame rate, profile, or bit
depth.

The proof matrix must measure:

- the intended H.264/HEVC mix, profiles, levels, and bit depths;
- aggregate coded pixel rate and output pixel rate;
- CMA/UVM allocation failures and total dma-buf memory;
- decoder drops, latency, and timestamp behavior;
- compositor and display bandwidth;
- CPU load from demuxing and packet submission; and
- clocks, temperature, and throttling over a sustained run.

A linear 1920x1080 NV12 image alone is about 3 MiB before alignment. Decoder
reference frames, capture queues, nine streams, and two or three final
scanout buffers can therefore consume hundreds of MiB. Compressed modifiers
may reduce traffic or storage but add compatibility constraints.

## Buffer ownership and synchronization

The capture queue cannot reuse a buffer while KMS or the compositor still
reads it. Treat every buffer as moving through this state machine:

```text
FREE
  -> QUEUED_TO_DECODER
  -> DEQUEUED_READY
  -> IMPORTED_OR_RENDERING
  -> RETAINED_BY_CURRENT_WALL
  -> RELEASE_PENDING
  -> QUEUED_TO_DECODER
```

Rules:

- Keep the dma-buf fd and its allocation alive while any decoder, EGL,
  Vulkan, GE2D, GEM, FB, or KMS reference exists.
- Do not `VIDIOC_QBUF` a capture slot merely because its frame was submitted
  to the compositor.
- Requeue it only after GPU/GE2D completion and after it is no longer the
  retained frame for that tile.
- Do not reuse a final scanout buffer until its previous KMS presentation is
  complete.
- Allocate enough capture buffers for the codec's minimum/DPB requirement
  plus frames retained by the compositor. The exact count comes from the
  V4L2 negotiation, not a fixed constant.

Prefer explicit synchronization when the runtime properties support it:

- pass the producer fence to a plane's `IN_FENCE_FD`;
- request the CRTC `OUT_FENCE_PTR`; and
- release the prior scanout only after the out-fence or page-flip event.

For a composed path, wait on decoder/dma-buf readiness before sampling and
export the compositor completion fence to KMS. If the vendor stack does not
expose explicit fences end-to-end, preserve dma-buf implicit-fence semantics
and use page-flip completion before reuse. Never substitute a fixed sleep.

Frame scheduling is separate from memory ownership. Streams will not share
timestamps, so the wall needs a policy such as “newest completed frame at
each display tick,” with a bounded queue and drop-old behavior.

## Kodi and DRM ownership

Opening `/dev/dri/card0`, enabling atomic client capabilities, or opening a
render node does not grant display control. Modesets and plane commits
require DRM master (or a lease containing all required objects). CoreELEC
Kodi calls `drmSetMaster` and atomically presents its GBM GUI buffer, so a
second process should expect KMS commits or `drmSetMaster` to fail with Kodi
running.

The supported operational model for this project is exclusive handoff:

1. stop the Kodi service;
2. wait for Kodi to exit and close its DRM fd;
3. open the primary DRM node, enable universal-plane and atomic caps, and
   acquire DRM master;
4. enumerate resources, select the connected HDMI connector/CRTC/mode, and
   perform the application's atomic modeset;
5. run the wall;
6. atomically disable application planes, wait for completion, and release
   FB/GEM/dma-buf resources;
7. drop master and close the DRM fd; and
8. restart Kodi, which creates fresh FB IDs and restores its own display
   state.

Record the active mode and relevant properties before takeover so the
application can preserve the output mode. Do not attempt to restore Kodi's
old FB IDs after Kodi has closed them; they are not persistent resources.
On a crash, the launch wrapper's `EXIT`/signal handling remains responsible
for restarting Kodi.

The existing `scripts/launch-coreelec.sh` already implements the outer
stop/run/restart lifecycle. The player should still report a clear error if
master acquisition fails rather than attempting commits in a loop.

DRM leases are not a practical shortcut here. They require a cooperating
DRM master to create a lease, CoreELEC Kodi does not currently provide that
integration, and both applications would still contend for a shared HDMI
CRTC and blend pipeline. A render node can support GPU rendering but cannot
perform KMS commits.

## Proposed architecture in this repository

The current `DecodedFrame` is deliberately a short-lived CPU YUV view, and
`FfmpegDecoder` is deliberately software-only. Do not add dma-buf ownership
to that struct or hide the hardware path inside `FfmpegDecoder`.

Add separate interfaces along these lines:

```text
AmlogicV4L2DecoderBackend
  owns compressed-input and capture queues
  yields a reference-counted HardwareFrame

HardwareFrame
  owns/borrows dma-buf allocation(s)
  carries fourcc, modifier, image-plane layout, color data, and PTS
  returns its capture slot only after the last consumer releases it

WallCompositor
  imports HardwareFrame objects
  renders the current 1–9 tile set into a ScanoutBuffer
  returns a completion fence

KmsOutput
  owns DRM master, connector/CRTC/plane property IDs, GBM buffers, and FBs
  submits atomic commits and reports presentation completion
```

Keep the current software decoder and renderer intact for host tests and
fallback. Device-only classes should be compiled only for the appropriate
CoreELEC target and should fail closed when capabilities differ from the
validated set.

`StreamPlayer` currently treats `isHardware() == true` as a worker failure,
and the renderer contract accepts only `DecodedFrame` CPU pointers. Adding a
hardware backend alone will therefore not activate hardware playback. The
player, frame queue, and presentation contract must change together.

The other main integration issue is that the current player presents one
stream at a time. Hardware composition needs a wall-level transaction:
collect the latest retained frame for every visible stream, render the
complete grid, then submit one scanout. Calling a KMS commit independently
from each decoder thread would tear layouts, race buffer lifetimes, and make
one atomic display state impossible.

## Device discovery

Run these while Kodi is running and again after stopping it where relevant:

```sh
cat /etc/os-release
uname -a
readlink -f /sys/class/drm/card0/device/driver
ls -l /dev/dri /dev/video* /dev/media*

modetest -M meson -c -p
drm_info /dev/dri/card0
cat /sys/kernel/debug/dri/0/state
cat /sys/kernel/debug/dma_buf/bufinfo
fuser -v /dev/dri/card0

v4l2-ctl --list-devices
v4l2-ctl -d /dev/video26 --all
v4l2-ctl -d /dev/video26 --list-formats-out
v4l2-ctl -d /dev/video26 --list-formats
```

CoreELEC images may not ship `modetest`, `drm_info`, or `v4l2-ctl`. Use
temporary target-architecture static tools or a development image; do not
parse these command outputs as a production discovery API. Replace
`/dev/video26` with the node found by enumeration.

Also capture:

- plane `IN_FORMATS` blobs and all modifiers;
- EGL/Vulkan extension lists;
- V4L2 `TRY_FMT`/`S_FMT`, control, minimum-buffer, and crop/compose results;
- media-controller topology if present;
- the result of importing one sample UVM buffer into the compositor; and
- atomic `TEST_ONLY` results for direct and composed paths.

## Gated proof plan

Each stage should pass on the GS-King X before building the next:

1. **Ownership:** with Kodi stopped, acquire DRM master, modeset, show two
   alternating RGB GBM buffers, cleanly release master, and restart Kodi.
2. **One decoder:** use the V4L2 node directly to decode one known H.264
   stream into UVM dma-bufs. Validate dimensions, strides, offsets, fourcc,
   modifier, timestamps, and queue lifetime.
3. **One import:** import a decoded buffer into EGL/Vulkan or GE2D and render
   it into an RGB scanout buffer without CPU mapping.
4. **Direct-plane experiment:** import one decoded UVM buffer into a DRM FB
   and `TEST_ONLY`/commit it on a video plane. This tests the vendor
   UVM-to-DRM contract even though it is not the final wall design.
5. **Composed wall:** render 1, 2, 4, and 9 synthetic dma-buf sources into
   one scanout, then repeat with real decoders.
6. **Stress matrix:** test the intended codec/resolution/fps combinations,
   disconnect/reconnect, stream restart, layout changes, EOS, corrupt input,
   memory pressure, thermal steady state, and repeated Kodi handoff.

Failure at stage 3 should trigger a focused comparison of EGL/Vulkan and
GE2D import support. Failure at stage 4 does not invalidate GPU composition;
the final scanout is an RGB GBM buffer rather than a decoder UVM buffer.
Failure to sustain nine decoders requires reducing the supported stream
matrix, not allocating more KMS planes.

## Open questions

- Which capture fourcc/modifier combinations does the S922X decoder produce
  for each target codec and bit depth?
- Can CoreELEC's Mali userspace sample those UVM dma-bufs through EGL or
  Vulkan without a conversion allocation?
- Does GE2D expose a maintained dma-buf and fence API in this CoreELEC
  image, and does it support all required crop/scale/color operations?
- Which explicit fence properties are exposed by the vendor DRM driver on
  the GS-King X?
- How many capture buffers does each codec require, and how much CMA/UVM
  memory remains with nine active instances?
- Does the two-video-plane direct test accept both linear and Amlogic FBC
  buffers with scaling?
- What codec/resolution/fps matrix can the S922X sustain without thermal or
  memory-bandwidth failure?
- What equivalent path, if any, should be implemented for CoreELEC 21's
  vendor 4.9 stack?

## Sources

Kernel interfaces:

- [Buffer allocation and exchange (dma-buf)](https://docs.kernel.org/userspace-api/dma-buf-alloc-exchange.html)
- [dma-buf framework](https://docs.kernel.org/driver-api/dma-buf.html)
- [DRM userspace API: primary nodes, master, render nodes, and leases](https://docs.kernel.org/gpu/drm-uapi.html)
- [DRM/KMS and atomic state](https://docs.kernel.org/gpu/drm-kms.html)
- [Meson DRM hardware overview](https://docs.kernel.org/gpu/meson.html)
- [Upstream Meson RGB plane](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/meson/meson_plane.c)
- [Upstream Meson YUV overlay](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/meson/meson_overlay.c)

CoreELEC kernel configurations and CoreELEC 22 vendor sources examined at
the pinned `5.15.196_20260225` vendor-source snapshot:

- [CoreELEC 21 Amlogic-ng kernel config](https://github.com/CoreELEC/CoreELEC/blob/coreelec-21/projects/Amlogic-ce/devices/Amlogic-ng/linux/linux.aarch64.conf)
- [CoreELEC 22 Amlogic-no kernel config](https://github.com/CoreELEC/CoreELEC/blob/coreelec-22/projects/Amlogic-ce/devices/Amlogic-no/linux/linux.aarch64.conf)
- [Meson DRM plane formats, modifiers, and UVM import](https://github.com/CoreELEC/common_drivers/blob/5.15.196_20260225/drivers/drm/meson_plane.c)
- [G12B DRM topology](https://github.com/CoreELEC/common_drivers/blob/5.15.196_20260225/arch/arm64/boot/dts/amlogic/mesong12b_drm.dtsi)
- [G12B dual-display additions](https://github.com/CoreELEC/common_drivers/blob/5.15.196_20260225/arch/arm64/boot/dts/amlogic/mesong12b_drm_dual_disp.dtsi)
- [GS-King X device tree](https://github.com/CoreELEC/common_drivers/blob/5.15.196_20260225/arch/arm64/boot/dts/amlogic/g12b_s922x_beelink_gs_king_x.dts)
- [Amlogic V4L2 decoder queue setup](https://github.com/CoreELEC/media_modules-aml/blob/5.15.196_20260225/drivers/amvdec_ports/aml_vcodec_dec.c)
- [`v4lplayer` V4L2 dma-buf sample](https://github.com/CoreELEC/media_modules-aml/blob/5.15.196_20260225/tools/v4lplayer/v4l2_dec.c)
- [`v4lplayer` UVM allocator](https://github.com/CoreELEC/media_modules-aml/blob/5.15.196_20260225/tools/v4lplayer/aml_uvm.c)

CoreELEC Kodi sources:

- [CoreELEC 22 build options selecting `libamcodec`](https://github.com/CoreELEC/CoreELEC/blob/coreelec-22/projects/Amlogic-ce/devices/Amlogic-no/options)
- [Kodi `AMLCodec`](https://github.com/CoreELEC/xbmc/blob/aml-5.15.196-22.0/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp)
- [Kodi `RendererAML`](https://github.com/CoreELEC/xbmc/blob/aml-5.15.196-22.0/xbmc/cores/VideoPlayer/VideoRenderers/HwDecRender/RendererAML.cpp)
- [Kodi `AMLDisplay` DRM ownership and atomic presentation](https://github.com/CoreELEC/xbmc/blob/aml-5.15.196-22.0/xbmc/windowing/amlogic/AMLDisplay.cpp)

Historical/community context (useful evidence, not an ABI or throughput
guarantee):

- [CoreELEC hardware-decoding experiments](https://discourse.coreelec.org/t/experiments-with-hw-decoding/16349)
- [CoreELEC multidecode discussion](https://discourse.coreelec.org/t/multidecode/18514)
- [CoreELEC 9.2.2 release note mentioning multi-decoder support](https://github.com/CoreELEC/CoreELEC/releases/tag/9.2.2)
