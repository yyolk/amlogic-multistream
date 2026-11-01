# Deep dive: libamcodec / CoreELEC AMLCodec (multi-stream)

Companion to [`amlogic-hw-decode-coreelec-kodi.md`](amlogic-hw-decode-coreelec-kodi.md). This path is the vendor `libamcodec` + CoreELEC `CAMLCodec` stack used on GS-King X (S922X / G12B) under CoreELEC 21 Amlogic-ng and CoreELEC 22 Amlogic-no. It is **not** mainline `meson-vdec` / DRM PRIME.

No decoder implementation lives here. Claims that need a box are tagged **\*verify\***.

## Pinned sources

Cloned for this note (shallow, 2026-11-01):

| Tree | Ref | HEAD |
| --- | --- | --- |
| [CoreELEC/xbmc](https://github.com/CoreELEC/xbmc) CE21 / Amlogic-ng | `aml-4.9-21.3` | `acc0fc66bc6af69c31a93c5d17e36900343263b7` |
| [CoreELEC/xbmc](https://github.com/CoreELEC/xbmc) CE22 / Amlogic-no | `aml-5.15.196-22.0` | `d49aa4ac2362adf51da4eaa7c6ccea6e619fb860` |
| [CoreELEC/CoreELEC](https://github.com/CoreELEC/CoreELEC) CE21 | `coreelec-21` | `15970b8e469b8e299a8947b1751593bdaf53ed82` |
| [CoreELEC/CoreELEC](https://github.com/CoreELEC/CoreELEC) CE22 | `coreelec-22` | `98a729b80ece21a7ceb8dbab127a6d19a6cff60a` |
| libamcodec CE21-ng tarball | `8a779caba41b163fed9ebd0df82db36dd962a9a3` | [sources.coreelec.org](https://sources.coreelec.org/libamcodec-aarch64-8a779caba41b163fed9ebd0df82db36dd962a9a3.tar.xz) |
| libamcodec CE22 tarball | `d28bf07facc8727de52ba1f70c1362f9e19adc25` | [package.mk](https://github.com/CoreELEC/CoreELEC/blob/coreelec-22/projects/Amlogic-ce/packages/multimedia/libamcodec/package.mk) |

Line numbers below are from those HEADs. CE21 `AMLCodec.cpp` is the default citation; CE22 keeps the same `STREAM_TYPE_FRAME` + `/dev/video10` contract (OpenDecoder still sets `dec_mode = STREAM_TYPE_FRAME` at line 2335).

Both images set `KODIPLAYER_DRIVER="libamcodec"`:

- CE21 ng: `projects/Amlogic-ce/devices/Amlogic-ng/options:102`
- CE22 no: `projects/Amlogic-ce/devices/Amlogic-no/options:105`

---

## 1. STREAM_TYPE_FRAME userspace sequence

Kodi never talks to a standard V4L2 M2M decoder for this path. `libamcodec` owns format and ES write (`/dev/amstream_*` inside `codec_init` / `codec_write`). `/dev/video10` (`amlvideo`) is only a **picture-ready ring**: dequeue an index, later queue it back. There is **no** `VIDIOC_S_FMT` / `VIDIOC_REQBUFS` / `VIDIOC_STREAMON` in Kodi.

`dec_mode_t` in the shipped header:

```45:49:usr/include/amcodec/codec_type.h
typedef enum {
    STREAM_TYPE_SINGLE = 0,
    STREAM_TYPE_FRAME,
    STREAM_TYPE_STREAM,
} dec_mode_t;
```

Kodi default is `STREAM_TYPE_FRAME` + `FRAME_BASE_PATH_AMLVIDEO_AMVIDEO` ([AMLCodec.cpp:2013–2014](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L2013)). Dolby Vision FEL/profile 4/7 not-MEL flips to `STREAM_TYPE_STREAM` ([AMLCodec.cpp:2047–2054](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L2047)).

### 1.1 Open (lazy)

1. `CDVDVideoCodecAmlogic::Open` constructs `CAMLCodec` and **does not** call `OpenDecoder` ([DVDVideoCodecAmlogic.cpp:340](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.cpp#L340), decoder open is deferred to first `AddData` at [499–505](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.cpp#L499)).
2. `CAMLCodec` ctor `dlopen`s `libamcodec.so` via `DllLibAmCodec` ([AMLCodec.cpp:1828–1830](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L1828)). Handles start at `-1` ([1832–1835](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L1832)).
3. `OpenDecoder` → `OpenAmlVideo`:

```2221:2233:https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp
bool CAMLCodec::OpenAmlVideo(const CDVDStreamInfo &hints)
{
  PosixFilePtr amlVideoFile = std::make_shared<PosixFile>();
  if (!amlVideoFile->Open("/dev/video10", O_RDONLY | O_NONBLOCK))
    ...
  m_amlVideoFile = amlVideoFile;
  m_defaultVfmMap = GetVfmMap("default");
  return true;
}
```

4. Fill `aml_generic_param` / `codec_para_t` ([2001–2014](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L2001)):

   | Field | Value |
   | --- | --- |
   | `noblock` | `0` (blocking `codec_write`) |
   | `stream_type` | `STREAM_TYPE_ES_VIDEO` |
   | `dec_mode` | `STREAM_TYPE_FRAME` |
   | `video_path` | `FRAME_BASE_PATH_AMLVIDEO_AMVIDEO` |
   | `video_type` / `format` | `codecid_to_vformat` / `codec_tag_to_vdec_type` |
   | `width` / `height` / `rate` | from `CDVDStreamInfo` |
   | `SessionID` / `player_id` | **never set** — `codec_init_para` `memset`s the struct to 0 ([AMLCodec.cpp:197–223](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L197)) |

5. `codec_init_para` then `codec_init(&vcodec)` ([2158](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L2158), [2172](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L2172)). Format ioctls live **inside** the proprietary `.so` (`AMSTREAM_IOC_VFORMAT`, `AMSTREAM_IOC_SYSINFO`, `AMSTREAM_SET_FRAME_BASE_PATH` in `amstream.h`).
6. Post-init: `codec_pause`, `codec_set_cntl_mode(TRICKMODE_NONE)`, `codec_set_video_delay_limited_ms(1000)`, `codec_set_cntl_avthresh` / `syncthresh`, `/sys/class/tsync/enable = 0`, `pre_header_feeding` (H.264/HEVC extradata), `/sys/class/video/freerun_mode = 1`, `SetPollDevice(vcodec.cntl_handle)` ([2182–2216](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L2182)).

`STREAM_TYPE_SINGLE` is the only mode that rewrites VFM (`SetVfmMap("default", "decoder ppmgr amlvideo deinterlace amvideo")` at [2169–2170](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L2169)). Frame mode leaves the existing map.

**\*verify\***: exact amstream nodes `codec_init` opens on S922X 4.9 vs 5.15 (`/dev/amstream_vbuf`, `/dev/amstream_es`, `/dev/amvideo`, …). **\*verify\***: who pre-queues `/dev/video10` buffers if userspace never `REQBUFS`.

### 1.2 Feed ES

`CDVDVideoCodecAmlogic::AddData` converts avcC→Annex-B / HEVC, waits for a keyframe, then `CAMLCodec::AddData` ([DVDVideoCodecAmlogic.cpp:410–509](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.cpp#L410)).

`CAMLCodec::AddData` ([2415–2551](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L2415)):

1. `codec_get_vbuf_state` — skip if `free_len == 0` or level ≥ 100%.
2. Frame-mode ready threshold is 5% (`m_buffer_level_ready`), stream-mode 90%.
3. First packet may prepend `hdr_buf` extradata.
4. PTS/DTS → `am_pkt.avpts` / `avdts` (`UINT64_0` if invalid).
5. `set_header_info` (MPEG4/VC1 prefixes).
6. Loop `write_av_packet` until consumed or 100 iterations.

`write_av_packet` ([660–717](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L660)):

1. `check_in_pts` → `codec_checkin_pts_us64`.
2. `write_header` → `codec_write` of codec-specific header.
3. `codec_write(pkt->codec, buf, size)` for the ES. `noblock=0`. On `EAGAIN`, sleep `RW_WAIT_TIME` (5 ms, comment says 20 ms) and return `PLAYER_SUCCESS` with leftover so the 100-iteration loop retries.

Packets `> 50000` bytes sleep an extra 2 ms after the write loop ([2537–2538](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L2537)).

### 1.3 Dequeue pictures from `/dev/video10`

`GetPicture` ([2654–2711](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L2654)) only dequeues when not draining, buffer level is ready, and `DequeueBuffer()` returns 0.

```2627:2651:https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp
int CAMLCodec::DequeueBuffer()
{
  v4l2_buffer vbuf = v4l2_buffer();
  vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  int ret = (m_amlVideoFile->IOControl(VIDIOC_DQBUF, &vbuf) < 0) ? errno : 0;
  if (ret == 0) {
    m_cur_pts  = (uint64_t)(uint32_t)vbuf.timestamp.tv_sec << 32;
    m_cur_pts += (uint32_t)vbuf.timestamp.tv_usec;
    m_bufferIndex = vbuf.index;
  }
  return ret;
}
```

Open is `O_NONBLOCK`, so empty ring is `EAGAIN` → `VC_BUFFER`. Success fills `VideoPicture` pts/duration only. `CDVDVideoCodecAmlogic::GetPicture` then wraps a `CAMLVideoBuffer` that stores `{shared_ptr<CAMLCodec>, omxPts, duration, bufferIndex}` — **no plane pointers** ([DVDVideoCodecAmlogic.cpp:541–555](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.cpp#L541), [DVDVideoCodecAmlogic.h:34–46](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.h#L34)).

### 1.4 Release

```2581:2598:https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp
int CAMLCodec::ReleaseFrame(const uint32_t index, bool drop)
{
  v4l2_buffer vbuf = {};
  vbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  vbuf.index = index;
  if (drop)
    vbuf.flags |= V4L2_BUF_FLAG_DONE;
  return m_amlVideoFile->IOControl(VIDIOC_QBUF, &vbuf);
}
```

| Caller | drop | Meaning |
| --- | --- | --- |
| `CRendererAML::RenderUpdate` ([185–199](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/VideoRenderers/HwDecRender/RendererAML.cpp#L185)) | `false` | Show this index on the amvideo plane, then `QBUF` |
| `CRendererAML::ReleaseBuffer` / pool `Return` | `true` | Discard (`V4L2_BUF_FLAG_DONE`) |

`RenderUpdate` also writes `/sys/class/video/axis` via `SetVideoRect` and calls `CAMLCodec::PollFrame()` (`poll` 50 ms on `cntl_handle`).

### 1.5 Close

`CloseDecoder` ([2307–2350](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L2307)): `SetPollDevice(-1)` → `codec_set_cntl_mode(TRICKMODE_NONE)` if trick → `codec_close` → free packets/config → `tsync/enable=1` → wait `/sys/class/amdolby_vision/dv_video_on==0` up to `m_decoder_timeout` seconds → `dolby_vision_enable=N` → `ShowMainVideo(false)` (`/sys/class/video/disable_video=1`) → `CloseAmlVideo` (close `/dev/video10`).

---

## 2. Concurrent AMLCodec instances

**Kodi does not create N concurrent `CAMLCodec` objects for a grid.** VideoPlayer owns one video decoder. `CDVDVideoCodecAmlogic::Open` constructs one `CAMLCodec` ([DVDVideoCodecAmlogic.cpp:340](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.cpp#L340)). There is no session-id allocator.

`CDVDVideoCodecAmlogic.h:107` declares `static std::atomic<bool> m_InstanceGuard` on both CE21 and CE22. It is **never defined or used** in `DVDVideoCodecAmlogic.cpp`. The live guard is only on `CDVDVideoCodecAndroidMediaCodec` (single-instance). So Kodi will not refuse a second Amlogic codec if something constructed one — it also never does so for PiP.

Shared process-global state (one decoder at a time is assumed):

| Object | Scope | File |
| --- | --- | --- |
| `CAMLCodec::m_pollDevice` | static fd | [AMLCodec.h:135](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.h#L135), [AMLCodec.cpp:2554](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L2554) |
| `pollSyncMutex` | static | `PollFrame` / `SetPollDevice` |
| `saved_disable_video` | function-static | `ShowMainVideo` [2749–2758](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L2749) |
| `/dev/video10` | one node, opened `O_RDONLY` | `OpenAmlVideo` |
| `/sys/class/video/{axis,zoom,disable_video,screen_mode,blackout_policy,freerun_mode}` | global VPU plane | `SetVideoRect` / `ShowMainVideo` |
| `/sys/class/tsync/enable` | global A/V sync | open/close |
| `/sys/class/vfm/map` | global video path | `STREAM_TYPE_SINGLE` only |
| `/dev/amvecm` | PQ / vadj1 | `Enable_vadj1` |

`codec_para_t` fields Kodi leaves at zero that a multi-stream client would need ([codec_type.h:126–139](https://sources.coreelec.org/libamcodec-aarch64-8a779caba41b163fed9ebd0df82db36dd962a9a3.tar.xz) in the CE21 tarball):

- `SessionID`, `player_id`
- `display_mode` (`DISPLAY_MODE_AMVIDEO` vs `DISPLAY_MODE_PIPVIDEO`)
- `video_path` alternatives: `FRAME_BASE_PATH_IONVIDEO`, `FRAME_BASE_PATH_AMLVIDEO1_AMVIDEO2`, `FRAME_BASE_PATH_PIP_TUNNEL_MODE`

CoreELEC staff: hardware can schedule several vdec instances; they keep multi-decode off because Kodi open/close desyncs the kernel and shows the wrong stream ([Multidecode thread](https://discourse.coreelec.org/t/multidecode/18514)).

**\*verify\***: `open("/dev/video10")` from two processes while Kodi is stopped. **\*verify\***: whether 4.9 / 5.15 `vdec` honors `SessionID` / `player_id` when set. **\*verify\***: max concurrent frame-mode instances on S922X with current ucode (forum figure is “up to 9”).

---

## 3. Buffer ownership / zero-copy vs CPU copy

Playback is an **overlay**, not a CPU image:

- `CRendererAML::IsGuiLayer()` is `false` ([RendererAML.h:37](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/VideoRenderers/HwDecRender/RendererAML.h#L37)). GLES draws OSD only.
- `CAMLVideoBuffer` is an index ticket. No `mmap`, no `VIDIOC_EXPBUF`, no DRM PRIME fd.
- ES is **CPU-copied** into the amstream ring (`codec_write`). Decoded pictures stay in kernel CMA/ion and are composited by the VPU onto the amvideo plane (`/sys/class/video/axis`).
- That is zero-copy **display**, not zero-copy **to our `DecodedFrame`**.

DRM PRIME / ion handoff on this path: **none**. `FRAME_BASE_PATH_IONVIDEO` exists in `amstream.h:192` but Kodi never selects it. Kodi’s `DMAHeapBufferObject` (`/dev/dma_heap/linux,cma`) is a different renderer path, not `CRendererAML`.

CPU copy exists only for screenshots: `CScreenshotAML::CaptureVideoFrame` opens `/dev/amvideocap0`, GE2D-captures RGB, `pread`s into a heap buffer ([ScreenshotAML.cpp:26–48](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/utils/ScreenshotAML.cpp#L26)). Full-frame copy, BGR, 32-byte stride. Unsuitable as a 9-tile read path.

Implication for this repo: a libamcodec backend cannot fill `DecodedFrame` YUV420P without a new capture/export path. `DecoderBackend::readFrame`’s default (return false, leave frame zeroed) matches an overlay backend.

**\*verify\***: whether `FRAME_BASE_PATH_IONVIDEO` or `VIDIOC_EXPBUF` on `/dev/video10` yields a dma-buf on 4.9 ng. **\*verify\***: CE22 5.15 amlvideo vs ionvideo.

---

## 4. Error / flush / seek / abort (reaper / quiesce)

There is **no** `abort()` in `CAMLCodec`. Every teardown is synchronous on the decoder thread.

| Path | What Kodi does | Blocks? | Hang risk |
| --- | --- | --- | --- |
| Feed full | `codec_write` `noblock=0`; `EAGAIN` → 5 ms sleep; loop ≤ 100 | yes, bounded ~500 ms plus 2 ms on large NALs | **\*verify\*** if `codec_write` can sleep longer than `EAGAIN` |
| Feed stuck | loop==100 → `Reset()` ([2531–2534](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L2531)) | `Reset` itself | see Reset |
| `GetPicture` starve | `VC_FLUSHED` after `m_decoder_timeout` s (advancedsettings) | no (nonblock DQBUF) | player reopens codec |
| Drain | `SetDrain(true)` → `VC_EOF` without DQBUF | no | leftover indexes until `ReleaseFrame` / close |
| Seek / flush | `CDVDVideoCodecAmlogic::Reset` → `codec_pause` + `codec_reset`, re-feed headers, **does not** close `/dev/video10` or DQ leftover indexes ([2362–2413](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L2362)) | `codec_reset` | **\*verify\*** `codec_reset` while ACTIVE; stale `video10` indexes after seek |
| Trick / pause | `codec_set_cntl_mode` only (`codec_pause`/`resume` commented out) | **\*verify\*** | |
| Poll | `poll(cntl_handle, 50 ms)` | 50 ms | CE22 also polls a vsync kick fd ([AMLCodec.cpp:3044](https://github.com/CoreELEC/xbmc/blob/aml-5.15.196-22.0/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L3044)) |
| Close DV | spin on `dv_video_on` up to `m_decoder_timeout` s, 10 ms steps | up to timeout | bounded |
| Close | `codec_close` then close `/dev/video10` | **\*verify\*** | classic black-screen if vdec stays ACTIVE ([vdec.c](https://github.com/osmc/vero3-linux/blob/master/drivers/amlogic/amports/vdec.c) comment: no kill API if decoder cannot leave ACTIVE) |
| `codec_init` fail | `OpenDecoder` returns false after `OpenAmlVideo` succeeded ([2173–2176](https://github.com/CoreELEC/xbmc/blob/aml-4.9-21.3/xbmc/cores/VideoPlayer/DVDCodecs/Video/AMLCodec.cpp#L2173)) | — | `/dev/video10` leak until `Close()`; `codec_close` on a never-inited handle |

Critical for our reaper (`StreamPlayer::closeAll` + `drain` + `exitIfBusy`):

1. Do **not** call `codec_close` / `codec_reset` from the abort thread. Those are not documented as async-signal/thread-safe, and they share `/sys/class/video/*`.
2. `abort()` can only latch and, at most, poke a `noblock` write or close a **private** wait fd. Closing `/dev/video10` under an in-flight `VIDIOC_DQBUF` is **\*verify\*** (likely `EBADF` / kernel warn).
3. `codec_write` with `noblock=0` has no interrupt callback. A hung write holds the decode thread the way FFmpeg `getaddrinfo` does today. Bound it (`noblock=1` + timeout) or treat it as a reaper stall and `_exit` after `kExitJoinDeadline`.
4. After abort, the session thread must `ReleaseFrame` every dequeued index, then `CloseDecoder`. Skipping `QBUF` before `codec_close` is a suspected leak into the black-screen state. **\*verify\***.
5. Seek-style `codec_reset` without draining `/dev/video10` is the closest Kodi analog to a messy restart. Our `restart` should prefer close + open over `codec_reset` until probed.

---

## 5. Licensing: CE21 vs CE22

| Item | CE21 Amlogic-ng | CE22 Amlogic-no |
| --- | --- | --- |
| Package | `projects/Amlogic-ce/packages/multimedia/libamcodec/package.mk` | same path |
| `PKG_LICENSE` | `proprietary` | `proprietary` |
| Tarball | `libamcodec-aarch64-8a779cab….tar.xz` | `libamcodec-aarch64-d28bf07f….tar.xz` |
| Image payload | `usr/lib/libamcodec.so.0.0` + symlink `libamcodec.so`; `usr/include/amcodec/{codec.h,codec_type.h,amports/amstream.h,…}` | same layout in package.mk (`cp -PR usr/lib $INSTALL/usr`; headers only to sysroot) |
| Header copyright | “Copyright (c) 2014 Amlogic, Inc. All rights reserved” + pointer to a `LICENSE` **not shipped in the tarball** | **\*verify\*** CE22 tarball text (not unpacked here) |
| `.so` strings | `codec_init amstream version : %d.%d` only; no GPL banner | **\*verify\*** |
| Kodi wrapper | GPL-2.0-or-later (`AMLCodec.cpp`, `DVDVideoCodecAmlogic.cpp`, `RendererAML.cpp`) | same |

What we can do without taking a license from Amlogic or relicensing this repo:

| Action | Allowed? |
| --- | --- |
| Cite ioctl names, device paths, call order (this document) | yes, reference |
| `dlopen` the **on-image** `/usr/lib/libamcodec.so` at runtime (Kodi `DllDynamic` pattern) | yes, if we do not ship the `.so` |
| Vendor / AppImage-bundle `libamcodec.so` or the header tree | no — proprietary, no LICENSE file in the source tarball |
| Copy `AMLCodec.cpp` / `RendererAML.cpp` into this tree | no, unless the project becomes GPL-2.0-or-later |
| Talk to `/dev/amstream_*` and `/dev/video10` with locally written ioctl wrappers | legally grey: numbers come from Amlogic headers; kernel UAPI may be GPL. **\*verify\*** with counsel before shipping |
| Link `-lamcodec` at build time against the CE sysroot | same as shipping the `.so` if the binary redistributes symbols / requires the lib on host |

CE22 still selects `libamcodec` for Amlogic-no. The first-draft note that “Kodi 19 dropped amcodec” is true for **upstream** xbmc, not for CoreELEC’s fork.

---

## 6. DecoderBackend sketch (no code)

Our surface is `DecoderBackend` in [`src/decoder_backend.h`](../../src/decoder_backend.h): `open` / `close` / `isHardware` / `abort` / `readFrame`. Quiesce is `StreamPlayer::closeAll` + `drain` + `exitIfBusy` ([`src/stream_player.h`](../../src/stream_player.h)), not a virtual.

| Our method | Kodi analog | 1:1? | Notes |
| --- | --- | --- | --- |
| `open(url)` | FFmpeg demux **plus** `Open` + lazy `OpenDecoder` | **no** | AMLCodec takes `CDVDStreamInfo` (codec, size, fps, extradata), not a URL. Keep demux in `FfmpegDecoder` (or a thin demuxer) and hand Annex-B NALs to a new hw backend. |
| `readFrame(DecodedFrame&)` | `GetPicture` + `DequeueBuffer` | **no** for YUV | Overlay path should leave the default `false` / zeroed frame and signal “presented in hardware” another way. A CPU `DecodedFrame` needs `/dev/amvideocap0` or ion export (neither is multi-stream ready). |
| `close()` | `CDVDVideoCodecAmlogic::Close` → `CloseDecoder` → `CloseAmlVideo` | **yes**, on the session thread | Must `ReleaseFrame` outstanding indexes first. Safe to call after failed `open` only if `OpenAmlVideo` / `codec_init` success is tracked separately. |
| `isHardware()` | `SetVideoDecoderName(..., true)` | **yes** | Always `true`. Today `StreamPlayer` **closes** a backend that reports hardware ([stream_player.h:37](../../src/stream_player.h)). That gate must change before this backend is wired. |
| `abort()` | *(none)* | **no** | Latch only. Do not `codec_close` here. Optional: set a flag that makes `AddData` skip `codec_write` and makes `GetPicture` return immediately. Hung `codec_write`/`codec_close` stay on the reaper until `kExitJoinDeadline` then `_exit`. |
| Quiesce (`closeAll`/`drain`) | player stop + `CloseDecoder` | **partial** | Same shape as software: abort → reaper → join → `close()` on the worker. Extra rule: one process-global hw lock until SessionID / `/dev/video10` sharing is **\*verify\***’d. Restart of the same id waits for `codec_close` to finish, not just `abort()`. |

Suggested object split (sketch only):

```
AmlogicAmcodecDecoder : DecoderBackend
  open(url):
    demux first video stream (reuse FFmpeg open, hwaccel off)
    OpenAmlVideo + codec_init(STREAM_TYPE_FRAME, FRAME_BASE_PATH_AMLVIDEO_AMVIDEO)
  readFrame:
    write pending NALs (noblock=1, timeout)
    DQBUF; store index in a side struct; return false (overlay) OR copy via amvideocap (debug)
  close:
    QBUF all held indexes; codec_close; close video10; restore tsync/disable_video
  abort:
    atomic latch; wake poll; do not touch libamcodec from this thread
```

Do not grow this inside `FfmpegDecoder::prepareAmlogicHwaccel`.

---

## 7. Device probe checklist (GS-King X)

Run on the box with Kodi **stopped** (`systemctl stop kodi` / launch script). Tag each line pass/fail. Everything here is **\*verify\*** until that log exists.

### Identity

- [ ] `cat /etc/os-release` — CoreELEC 21 ng vs 22 no
- [ ] `uname -r` — `4.9.269` vs `5.15.x`
- [ ] `cat /proc/device-tree/compatible` — `amlogic,s922x` / beelink gs-king-x
- [ ] `ls /usr/lib/libamcodec.so*` and `readlink -f` — `.so.0.0` present
- [ ] `md5sum /usr/lib/libamcodec.so.0.0` vs CE21 `8a779cab…` / CE22 `d28bf07f…` tarball
- [ ] `ls /usr/include/amcodec` — headers may be absent on the image (sysroot-only); note it

### Nodes

- [ ] `ls -l /dev/video10 /dev/amvideo /dev/amstream* /dev/amvideocap0 /dev/amvecm /dev/ion /dev/dma_heap/*`
- [ ] `v4l2-ctl -d /dev/video10 -D` — driver name `amlvideo` vs `meson-vdec`
- [ ] `v4l2-ctl -d /dev/video10 --all` — formats, buffer type, whether REQBUFS is advertised
- [ ] `cat /sys/class/vfm/map`
- [ ] `cat /sys/class/video/{disable_video,axis,screen_mode,blackout_policy,freerun_mode,device_resolution}`
- [ ] `ls /sys/class/tsync /sys/module/amvdec_h264 /sys/module/amvdec_h265 /sys/module/amdolby_vision`

### Ucode / multi-dec

- [ ] `dmesg | grep -iE 'vdec|ucode|firmware|amvdec'`
- [ ] any `video_ucode` / `firmware` version file under `/lib/firmware` or sysfs
- [ ] `grep -r . /sys/module/amvdec*/parameters/ 2>/dev/null | head`

### Single-instance smoke (do not leave decoder open)

- [ ] One process: `open("/dev/video10", O_RDONLY|O_NONBLOCK)` succeeds
- [ ] Second process: same open — success, `EBUSY`, or shared queue?
- [ ] `VIDIOC_DQBUF` without `codec_init` — `EAGAIN` vs `EINVAL`
- [ ] After Kodi plays one file and exits cleanly: `lsof /dev/video10 /dev/amstream*` empty

### Hang / close (watchdog the tester)

- [ ] Time `codec_init` + `codec_close` with no ES (need a tiny `dlopen` probe, not shipped here)
- [ ] Time `codec_reset` after one fed IDR
- [ ] Kill -9 mid-playback; reboot needed or next `codec_init` works?
- [ ] Rapid open/close 20× — black screen or not
- [ ] `CloseDecoder` DV wait duration with DV off (should be ~0)

### Capture / export

- [ ] `/dev/amvideocap0` open + one `pread` while Kodi is playing (optional, Kodi running)
- [ ] `VIDIOC_EXPBUF` on `/dev/video10` after a successful DQBUF
- [ ] ion / dma-heap alloc while vdec is running — OOM?

Record kernel lines for any `vdec` ACTIVE leftover. That log decides whether our reaper can retry `open` after a killed worker or must reboot.
