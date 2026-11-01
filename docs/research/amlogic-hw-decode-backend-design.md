# Amlogic hardware `DecoderBackend` design

Status: design notes and pseudocode only. Nothing in this document is
implemented. The player still decodes on the CPU and `DrmRenderer::present()`
still refuses to scan out. Every hardware statement below **requires GS-King X
/ CoreELEC** to confirm; the ones marked *verify* have not been checked on a
box and the design keeps a fallback for each.

> **Correction (Path 5, 2026-11-01).** §§2–3 and §7 below were drafted as if
> CoreELEC 22 ran mainline `meson-vdec` + `meson_drm`. Cross-check of
> research drafts [#36](https://github.com/yyolk/amlogic-multistream/pull/36)–[#41](https://github.com/yyolk/amlogic-multistream/pull/41)
> and [#43](https://github.com/yyolk/amlogic-multistream/pull/43) against the
> current `main` interfaces shows both device images are the **vendor**
> stack: CE21 = Amlogic-ng kernel **4.9** + `libamcodec`; CE22 = Amlogic-no
> kernel **5.15** + `libamcodec`. CoreELEC's FFmpeg is built
> `--disable-v4l2_m2m`. The vendor V4L2 M2M node (`aml-vcodec-dec`,
> `/dev/video26`) exists in `media_modules-aml` source and may be loaded by
> a boot script, but `CONFIG_AMLOGIC_MEDIA_V4L_DEC` is unset in the CE22
> kernel config — presence on a GS-King X is Q1, not a given. Display on
> the vendor kernel is `amvideo` + VFM (Kodi's path); vendor DRM
> (`CONFIG_AMLOGIC_DRM`) may also exist on CE22 (Q3).
>
> The implementation-ready `DecoderBackend` contract, threading / reaper
> proof, shape A vs B, stage-1 spike, host tests, and the ten blocking
> questions are in [Appendix A](#appendix-a-decoderbackend-contract-path-5).
> A coder should spike from the appendix, not from §§2–3.

This first half proposes a V4L2-shaped backend beside `FfmpegDecoder`. It is
written against `src/decoder_backend.h`, `src/renderer.h`,
`src/stream_player.h`, and the AUDIT4 / AUDIT5 / SURVIVING notes. Treat it
as background; Appendix A is the contract.

## 1. Constraints taken from the current design

These are the rules the existing code already pays for. The hardware backend
has to keep every one of them.

| Rule | Where it is stated | Consequence for a V4L2 backend |
| --- | --- | --- |
| `open()`, `close()`, `readFrame()`, `isHardware()` run on one thread; only `abort()` is cross-thread. `abort()` latches for the life of the object. | `decoder_backend.h` | All V4L2 ioctls stay on the worker thread. `abort()` may only touch an atomic and an eventfd. |
| `close()` runs on the thread that owns the session **after** the in-flight call returned. In `StreamPlayer` that is the tick thread (`releaseDecoder()` after `reapFinished()` joined). | `decoder_backend.h`, `stream_player.cpp` | `close()` must not issue an ioctl that a wedged driver can hold. `VIDIOC_STREAMOFF` and `close(fd)` on a stuck Amlogic decoder would freeze the wall and the signal loop. See §5. |
| A call that ignores `abort()` (getaddrinfo) keeps its worker on the reaper. `tick()` never joins it. Per-id restarts wait for it (AUDIT4 A3); at most `kMaxBusyRetired` blocked workers for any id (AUDIT5 F3); process exit publishes offline, waits `kExitJoinDeadline` (2s), then `_exit` (AUDIT4 A1/A2). | `stream_player.h`, `mqtt_controller.cpp` | A V4L2 ioctl that never returns is the same shape as getaddrinfo. The backend must make sure such a call can only happen on the worker thread, where the reaper already tolerates it. |
| Thread bound: decode threads never exceed `kMaxStreams + kMaxBusyRetired - 1` (17). | `stream_player.h` | The backend creates **no** internal threads. One instance is one worker thread. |
| `renderer.h` and `decoder_backend.h` contain no FFmpeg or libdrm types. | both headers | The DMA-BUF description uses plain ints (`fourcc`, `modifier`, fds). |
| Plane pointers expire on the next `readFrame()` or `close()`. `StreamPlayer` copies bytes (`OwnedFrame`) so the decode thread can run ahead. | `decoder_backend.h`, `stream_player.h` | A DMA-BUF cannot be copied. The handoff needs an ownership token (§6) so the decoder knows when it may re-queue the buffer. |
| `StreamPlayer` refuses `isHardware()` (`WorkerFail::Hardware`), `decodeUrl` refuses it too. | `stream_player.cpp`, `app.cpp` | The gate flips only when the selected renderer can accept DMA-BUF frames (§7.3). `SoftwareRenderer` keeps refusing. |
| FFmpeg interrupt callback is a stop latch only. A read deadline breaks HLS on FFmpeg 6.1. | `ffmpeg_decoder.cpp` | Demux keeps the same `AVIOInterruptCB`, the same `rw_timeout`, the same whitelists. No periodic interrupt to pump the decoder. |
| `prepareAmlogicHwaccel()` is a no-op and hardware must not grow inside `FfmpegDecoder`. | `ffmpeg_decoder.cpp`, README | New class, new file. `FfmpegDecoder` is untouched. |

## 2. Why direct V4L2 and not FFmpeg's `*_v4l2m2m` wrappers

Two routes exist on CoreELEC:

- **A. Direct V4L2 M2M** from this process: libavformat for demux, the
  `h264_mp4toannexb` / `hevc_mp4toannexb` bitstream filters from libavcodec,
  raw `ioctl()` on `/dev/videoN`, `VIDIOC_EXPBUF` for DMA-BUF export.
- **B. `avcodec_find_decoder_by_name("hevc_v4l2m2m")`** with
  `AV_PIX_FMT_DRM_PRIME` output.

Route B is attractive on paper but fails the constraints above:

1. Upstream FFmpeg's v4l2m2m decoder yields CPU (mmap) frames. DRM_PRIME output
   needs the LibreELEC/CoreELEC patch series. Whether CoreELEC's own
   `libavcodec.so.60` (CE21) and `.63` (CE22) carry it is *verify*; the device
   AppImages link exactly those libraries (`scripts/coreelec-targets.sh`).
2. libavcodec has no abort hook. The v4l2m2m decoder polls the device inside
   `avcodec_receive_frame()` with no caller-visible latch. A wedged decoder
   holds the worker with nothing for `abort()` to wake. The current code's
   whole getaddrinfo story would repeat for every frame, not just for DNS.
3. Buffer re-queue happens when the `AVFrame` is unreferenced, on whatever
   thread drops it. `StreamPlayer` drops frames on the tick thread. That is a
   `VIDIOC_QBUF` on the presenting thread, which a stuck driver can hold.
4. Teardown (`avcodec_free_context`) does `STREAMOFF` and `close(fd)` wherever
   it is called, which is the tick thread.

Route A costs more code (a V4L2 state machine) but each blocking point is
visible and can be placed on the worker thread deliberately. Route B stays
useful as a bring-up probe on the box
(`ffmpeg -c:v hevc_v4l2m2m -i URL -f null -`), not as the backend.

## 3. Target platforms

| | CoreELEC 21 Amlogic-ng (`coreelec21-ng-armv7hf`) | CoreELEC 22 Amlogic-no (`coreelec22-no-aarch64`) |
| --- | --- | --- |
| Kernel | Vendor `amlogic-4.9` (`KODI_VENDOR=amlogic-4.9`) | Vendor `amlogic-5.15` (`KODI_VENDOR=amlogic-5.15`). **Not** mainline `meson-vdec`. |
| Kodi player | `KODIPLAYER_DRIVER=libamcodec` | `KODIPLAYER_DRIVER=libamcodec` |
| Decode userspace | `libamcodec.so` (armhf tarball) → `/dev/amstream*` | `libamcodec.so` (aarch64 tarball) → `/dev/amstream*` |
| Instance cap in tree | Patch 003: `MAX_INSTANCE_MUN` 9 → **2** | No `patches/`; header stays **9**. Running value is Q2. |
| Vendor V4L2 M2M | `amvdec_ports` listed in `modules-load.d`; node `/dev/video26` if it registered | Same source; `CONFIG_AMLOGIC_MEDIA_V4L_DEC` unset in `linux.aarch64.conf`. Node presence is Q1. |
| Display Kodi uses | fbdev OSD + `/dev/amvideo` + `/sys/class/vfm/map` | Same vendor path. `CONFIG_AMLOGIC_DRM` may also be on; plane inventory is Q3. |
| FFmpeg in the image | 6.0.1, `--disable-v4l2_m2m` (v4l2 patches applied then compiled out) | 9.0.2, `--disable-v4l2_m2m` (v4l2 patches not applied) |
| 32/64-bit | 32-bit ARM HF; `-D_FILE_OFFSET_BITS=64` if any `mmap` of V4L2 buffers | 64-bit aarch64 |

The DRM handoff in §6 and §7 is designed against CE22. On CE21 the decoder
half may work while the display half has no DRM device at all. That is a
real risk to the "multi_vdec" goal in the README: CE21 may need an
`AmvideoRenderer` (vendor video layer) instead of `DrmRenderer`. This
document does not design that renderer; it only keeps the token contract
renderer-agnostic so one could be added.

### 3.1 Probe before freezing the design

Run on the box before writing the backend. Each answer picks a branch below.

```sh
# Decoder nodes, driver names, codecs, capture formats
for n in /dev/video*; do v4l2-ctl -d "$n" --info; done
v4l2-ctl -d /dev/videoN --list-formats-out        # H264 / HEVC / MPEG2 present?
v4l2-ctl -d /dev/videoN --list-formats            # NV12M? NV21M? anything AFBC?
# vb2 capabilities: ORPHANED_BUFS, DMABUF import
v4l2-ctl -d /dev/videoN --reqbufs-out=1 --verbose  # look at "capabilities"
# Display: planes, formats, modifiers, who holds master
ls /dev/dri; drm_info 2>/dev/null || modetest -M meson -p
# Instance cap: open N decoders at once until EBUSY / ENOMEM
# CMA budget
cat /proc/meminfo | grep -i cma; dmesg | grep -i cma
# Does CoreELEC's FFmpeg have the patched wrapper (route B probe only)
ffmpeg -decoders | grep v4l2m2m; ffmpeg -h decoder=hevc_v4l2m2m | grep -i drm
```

## 4. API mapping

### 4.1 Class shape

```cpp
// src/amlogic_v4l2_decoder.h  (proposed; not implemented)
//
// Hardware demux/decode for one stream. requires GS-King X / CoreELEC.
// Compiled only with -DAMLOGIC_V4L2_DECODE=ON, mirroring AMLOGIC_DRM_KMS.
// The default build has no open() of /dev/video*.
class AmlogicV4l2Decoder : public DecoderBackend {
public:
    bool open(const std::string& url) override;   // demux + V4L2 init sequence
    void close() override;                        // CPU-side release only (§5.3)
    bool isHardware() const override { return true; }
    void abort() override;                        // latch + eventfd write
    bool readFrame(DecodedFrame& frame) override; // one DRM-PRIME frame
    void quiesce() override;                      // NEW on DecoderBackend (§5.2)
private:
    // demux (same options as FfmpegDecoder)
    AVFormatContext* fmt_; AVPacket* pkt_; AVBSFContext* bsf_; int video_index_;
    AVRational time_base_;
    // device
    int fd_ = -1;            // /dev/videoN, O_NONBLOCK | O_CLOEXEC
    int wake_fd_ = -1;       // eventfd, written by abort()
    std::atomic<bool> abort_{false};
    OutputQueue  out_;       // mmap'd OUTPUT_MPLANE buffers, compressed input
    CaptureQueue cap_;       // CAPTURE_MPLANE buffers, exported as DMA-BUF
    std::shared_ptr<ReleaseMailbox> mailbox_;  // §6.3; outlives this object
    PtsRing pts_;            // seq -> (pts, time_base) (§4.4)
    State state_;            // Init, Streaming, Draining, Eof, Failed, Quiesced
    StallWatchdog watchdog_; // §5.4
};
```

### 4.2 `open(url)`: demux, then the V4L2 initialization sequence

The demux half is `FfmpegDecoder::open()` with the decoder half removed. Same
`protocol_whitelist`, `format_whitelist`, `codec_whitelist` (the last still
matters for the audio probes inside `avformat_find_stream_info`), same
`rw_timeout`, same top-level demuxer check, same `AVDISCARD_ALL` on the
non-selected HLS variants, same `interrupt_callback` reading `abort_`.

The codec half maps `codecpar->codec_id` to a V4L2 OUTPUT fourcc and runs the
stateful-decoder initialization sequence from the V4L2 spec:

```
open(url):
  demux_open(url)                                  # as FfmpegDecoder, no avcodec_open2
  fourcc = { h264: V4L2_PIX_FMT_H264, hevc: HEVC, mpeg2video: MPEG2,
             mpeg1video: MPEG1, mpeg4: MPEG4 }[codec_id]
  if fourcc missing: return failOpen("codec has no hardware decoder")   # mjpeg: software only
  if extradata is avcC/hvcC: bsf_ = h264_mp4toannexb / hevc_mp4toannexb  # fMP4 HLS, mp4, mov
  node = probeDecoderNode(fourcc)                  # §4.3, cached process-wide
  fd_ = open(node, O_RDWR|O_NONBLOCK|O_CLOEXEC); wake_fd_ = eventfd(0, EFD_CLOEXEC|EFD_NONBLOCK)
  if abort_: return false

  VIDIOC_SUBSCRIBE_EVENT(V4L2_EVENT_SOURCE_CHANGE); VIDIOC_SUBSCRIBE_EVENT(V4L2_EVENT_EOS)
  VIDIOC_S_FMT(OUTPUT_MPLANE, fourcc, sizeimage = max(2 MiB, 1 MiB * ceil(coded_w*coded_h / (1920*1080))))
  VIDIOC_REQBUFS(OUTPUT_MPLANE, kOutputBuffers = 4, MMAP); QUERYBUF + mmap each
  VIDIOC_STREAMON(OUTPUT_MPLANE)

  # Header parsing: feed access units until the driver announces the stream
  deadline = now + kHeaderTimeout (5 s, aborted by abort_)
  loop:
    if abort_: quiesce(); return false
    if G_FMT(CAPTURE_MPLANE) reports width*height > 0 and no SOURCE_CHANGE is required by driver: break
    feedOnePacket()                               # §4.4; blocks in av_read_frame under the interrupt callback
    poll({fd_: POLLPRI|POLLOUT, wake_fd_: POLLIN}, 100 ms)
    if POLLPRI: DQEVENT; if SOURCE_CHANGE: break
    if now > deadline: return failOpen("decoder did not report a stream format")
  setupCapture()                                   # §4.5
  state_ = Streaming
  log "AmlogicV4l2Decoder::open <url> node=<node> codec=<name> <w>x<h> fmt=<capture fourcc> isHardware=1"
  return true
```

Fallbacks:

- A driver that reports the CAPTURE format straight after `S_FMT(OUTPUT)`
  (allowed by the spec, common on vendor kernels) skips the header loop.
- `open()` failing on `EBUSY` / `ENOMEM` from `REQBUFS` or `open()` is the
  instance cap. It is reported as a plain open failure so `StreamPlayer`
  applies its existing 1s backoff. A clearer message
  (`"hardware decoder instances exhausted"`) is logged once per session.

### 4.3 Node discovery

`/dev/video*` numbering is not stable. Scan once per process:

```
probeDecoderNode(fourcc):
  for node in sorted(/dev/video*):
    fd = open(node, O_RDWR|O_NONBLOCK); QUERYCAP
    if !(device_caps & V4L2_CAP_VIDEO_M2M_MPLANE): close; continue
    if fourcc not in ENUM_FMT(OUTPUT_MPLANE): close; continue
    if none of {NV12M, NV12, NV21M, NV21} in ENUM_FMT(CAPTURE_MPLANE): close; continue
    close(fd); return node                         # vendor aml-vcodec-dec if present (Q1); not mainline meson-vdec
  return none
```

Cache per fourcc under a mutex. The scan opens and closes nodes; it must not
run inside the per-stream `open()` after the first time.

### 4.4 Feeding compressed input

Stateful decoders expect one access unit per OUTPUT buffer, Annex-B framed for
H.264/HEVC. MPEG-TS from HLS is Annex-B already; fMP4/mp4/mov go through the
BSF, which also prepends SPS/PPS on key frames.

The V4L2 `timestamp` field is a `timeval`. Stream pts can be negative, wrap, or
be `AV_NOPTS_VALUE`, so it is not used as a timestamp directly. A sequence
number travels through the decoder and is mapped back:

```
feedOnePacket():
  if input_eof_: return
  slot = out_.freeSlot() or (DQBUF(OUTPUT) if POLLOUT) ; if none: return
  rc = av_read_frame(fmt_, pkt_)                   # may block; interrupt callback is abort_
  if abort_: return
  if rc == AVERROR_EOF:
      input_eof_ = true; VIDIOC_DECODER_CMD(V4L2_DEC_CMD_STOP); state_ = Draining; return
  if rc < 0: fail("av_read_frame")
  if pkt_->stream_index != video_index_: unref; count non-video like FfmpegDecoder (8192 cap); return
  if bsf_: av_bsf_send_packet / av_bsf_receive_packet
  if pkt_->size > sizeimage: fail("access unit larger than the output buffer")   # not split; sizeimage is generous
  memcpy(slot.mmap, pkt_->data, pkt_->size)
  seq = pts_.push(pkt_->pts, time_base_)           # ring of 64; returns a small int
  buf.timestamp = { tv_sec = seq, tv_usec = 0 }    # drivers copy OUTPUT timestamp to the matching CAPTURE buffer
  buf.m.planes[0].bytesused = pkt_->size
  VIDIOC_QBUF(OUTPUT, slot); watchdog_.fed()
```

Input pacing is pull-based exactly like `FfmpegDecoder`: `readFrame()` feeds
only when it needs a frame and the OUTPUT queue has a free slot. Nothing runs
ahead of the tick thread except what the decoder has queued internally.

### 4.5 Capture setup, resolution change, and teardown order

```
setupCapture():
  fmt = G_FMT(CAPTURE_MPLANE)                       # driver's choice; NV12M expected
  if fmt.pixelformat not in {NV12M, NV12, NV21M, NV21}: fail("capture format is not scanout-able")
  min = G_CTRL(V4L2_CID_MIN_BUFFERS_FOR_CAPTURE) or 4
  count = min + kHeldByPlayer(3) + 1               # §6.4 derives the 3
  caps = REQBUFS(CAPTURE_MPLANE, count, MMAP).capabilities
  orphan_ok_ = caps & V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS
  for each buffer i, each plane p:
      cap_.fd[i][p] = EXPBUF(CAPTURE, i, p, O_RDONLY|O_CLOEXEC)   # kept for the life of the queue
      cap_.pitch[i][p] = fmt.plane_fmt[p].bytesperline; cap_.offset[i][p] = 0 (NV12M) or p*pitch*height (NV12)
  QBUF every capture buffer; STREAMON(CAPTURE_MPLANE)

onSourceChange():                                   # mid-stream SPS change (ad insertion, HLS discontinuity)
  drain: keep DQBUF(CAPTURE) until V4L2_BUF_FLAG_LAST (bounded by abort_ and kDrainTimeout 2 s)
  STREAMOFF(CAPTURE_MPLANE)
  if orphan_ok_:      REQBUFS(CAPTURE, 0)           # buffers still held by the renderer stay alive via their DMA-BUF refs
  else:               reopenDevice()                # CE21: close(fd_) + the whole §4.2 device half again; held DMA-BUFs stay valid
  setupCapture()
```

The renderer may hold the on-screen frame indefinitely (§6.4). A design that
waits for all buffers to come back before `REQBUFS(0)` deadlocks: the renderer
will not release the on-screen frame until a new one arrives, and no new one
arrives until `REQBUFS` succeeds. Orphaned buffers (CE22) or a device reopen
(CE21) break that cycle without waiting. In both cases the DMA-BUF the
renderer holds is backed by kernel memory that outlives the V4L2 queue.

### 4.6 `readFrame(frame)`

```
readFrame(out):
  out = {}
  if abort_ or state_ in {Failed, Eof, Quiesced}: return false
  loop:
    if abort_: quiesce(); return false              # §5.2: device teardown on this thread, now
    requeueReleased()                               # drain mailbox_, QBUF(CAPTURE) each index (§6.3)
    if state_ == Streaming and out_.hasFreeSlot() and capture_not_ready(): feedOnePacket()
    ev = poll({fd_: POLLIN|POLLOUT|POLLPRI, wake_fd_: POLLIN}, 100 ms)
    if wake_fd_ readable: continue                  # abort_ is checked at loop top
    if POLLPRI:
        e = DQEVENT
        if e == SOURCE_CHANGE: onSourceChange(); continue
        if e == EOS: state_ = Eof; return false
    if POLLERR: fail("decoder entered an error state")
    if POLLIN:
        b = DQBUF(CAPTURE)
        if b.flags & V4L2_BUF_FLAG_LAST and b.bytesused == 0: state_ = Eof; return false
        if b.bytesused == 0: QBUF(CAPTURE, b.index); continue
        watchdog_.produced()
        fill out as DrmPrime (§6.1) from cap_[b.index]; attach token (§6.2)
        out.pts, out.time_base = pts_.take(b.timestamp.tv_sec)
        return true
    if watchdog_.stalled(): fail("no frame from the hardware decoder in 3 s with input queued")
```

`fail()` logs with the stream URL, sets `state_ = Failed`, and returns false.
`StreamPlayer` then does what it does for `FfmpegDecoder`: logs
`"no frame from <id> (will retry)"`, retires the worker, backs off 1s, reopens.
A wedged decoder instance is thereby recycled per stream without touching
the others.

### 4.7 `close()`

CPU-side only, so that it is safe on the tick thread:

```
close():
  if fd_ >= 0 and state_ != Quiesced:
      quiesce()                                     # expected on the single-threaded --url path (decodeUrl);
                                                    # under StreamPlayer the sessionLoop guard has already run (§5.3)
  munmap OUTPUT planes; close every cap_.fd not handed to a token (tokens own dup'ed fds)
  av_bsf_free; av_packet_free; avformat_close_input
  mailbox_->detach()                                # future releases just close their fds (§6.3)
  close(wake_fd_); reset state; abort_ stays latched (same rule as FfmpegDecoder)
```

## 5. Thread model and abort semantics with a stuck V4L2 device

### 5.1 Threads

```
main / tick thread          mosquitto thread        worker thread (one per visible stream)
---------------------       ----------------        ----------------------------------------
StreamPlayer::tick()        broker I/O only         sessionLoop:
  reapFinished()                                      decoder->open(url)      <- all ioctls
  take frame token                                    loop: readFrame()       <- poll + ioctls
  renderer.present(tok)                               on stop: quiesce()      <- STREAMOFF/close(fd)
  renderer.commit()  (§7.2)                           done = true
  DRM event read -> release old tokens -> mailbox
  requestStop(): worker.stop = true; decoder->abort()   <- atomic + eventfd only
```

No thread is added. `abort()` is the only cross-thread entry into the backend
and it does an atomic store plus `write(wake_fd_, 1)`. Token release (§6.3)
is cross-thread too but it only pushes an index into a mutex-protected
vector; the `VIDIOC_QBUF` happens later on the worker.

### 5.2 Where a V4L2 call can block, and what bounds it

| Call | Can it hang? | Bound |
| --- | --- | --- |
| `open(/dev/videoN)` | Vendor driver may load firmware on first open (hundreds of ms). Hangs are rare. | None needed; it is on the worker thread. |
| `S_FMT`, `REQBUFS`, `EXPBUF`, `STREAMON`, `SUBSCRIBE_EVENT` | Synchronous; vendor `STREAMON` may load firmware. Uninterruptible. | Worker thread only. A hang lands on the reaper like getaddrinfo. |
| `DQBUF` | Yes, if blocking. | Never called blocking. `O_NONBLOCK` + `poll()` with a 100ms slice and `wake_fd_` in the set. `abort()` returns from `poll()` immediately. |
| `av_read_frame` | Yes (network, HLS live edge). | Same as today: `AVIOInterruptCB` on `abort_`, `rw_timeout` 15s, getaddrinfo still ignores both. |
| `DECODER_CMD STOP` + drain | Driver may never emit `FLAG_LAST`. | `kDrainTimeout` 2s and `abort_`. On timeout, treat as EOF. |
| `STREAMOFF` | **Yes.** Waits for the hardware to return buffers; a wedged firmware (vendor) or a reset in progress can hold it. May sit in D state. | Only ever issued inside `quiesce()` on the worker thread (§5.3). |
| `close(fd)` | **Yes**, same path as `STREAMOFF` via vb2 release. | Same. |
| `QBUF` | Normally instant; wedged driver can take its lock. | Worker thread only (release goes through the mailbox). |

### 5.3 `quiesce()`: moving the dangerous calls onto the worker

Today `releaseDecoder()` calls `decoder->close()` on the tick thread. For
`FfmpegDecoder` that is fine. For V4L2 it would put `STREAMOFF` and
`close(fd)` on the thread that presents every other tile and services
`sigtimedwait`. A wedged decoder would then hang the wall, defeat
`exitIfBusy()`, and turn the launch script's 10s SIGTERM poll into SIGKILL,
which is the exact failure AUDIT4 A1 fixed for DNS.

Proposal: add one virtual to `DecoderBackend`, default no-op, and one call in
`sessionLoop`:

```cpp
// decoder_backend.h (proposed addition)
// Release the device on the worker thread before close() runs elsewhere.
// Called by the session thread after abort(), on every exit path of its
// loop, and never concurrently with open() or readFrame(). May block in
// the driver; that is the point of calling it here. close() after a
// completed quiesce() must not block.
virtual void quiesce() {}
```

```
sessionLoop(worker, id, url):                        # stream_player.cpp (proposed change, shown as pseudocode)
  struct QuiesceOnExit { ~QuiesceOnExit() { decoder->quiesce(); } } guard;   # runs on every return, including exceptions
  ... unchanged body ...
```

Effects:

- `requestStop()` → `abort()` → the worker leaves `poll()` within one slice →
  `readFrame()` returns false → `sessionLoop` returns → guard calls
  `quiesce()` on the worker. If `STREAMOFF` hangs, `done` stays false, the
  worker stays on the reaper, `reaperBusy(id)` blocks a same-id restart,
  `busyRetiredCount()` caps churn, `exitIfBusy()` `_exit`s at shutdown.
  Nothing new has to be added to `StreamPlayer`'s bookkeeping; the existing
  tests `SURVIVING4 T1`, `AUDIT4 A3`, `AUDIT5 F3` describe this behaviour
  already and a `StallingDecoder` whose `quiesce()` sleeps is the unit test.
- `close()` on the tick thread finds `state_ == Quiesced` and only frees
  memory and FFmpeg contexts.
- Under `StreamPlayer`, the one path where `close()` runs without a prior
  `quiesce()` is `startWorker()`'s `std::thread` failure branch, which
  happens before `open()` ran, so the device is not open and `close()` is
  trivially fast. On the `--url` path `decodeUrl` owns the only thread, so
  `close()` doing the device teardown itself is correct there.

Alternative considered: have `abort()` itself issue `STREAMOFF` from the tick
thread to speed up the worker's exit. Rejected: it puts the hang on the wrong
thread, and it races the worker's own ioctls on the same fd.

### 5.4 Stall watchdog

A decoder that accepts input but never produces output (firmware stuck,
unsupported profile the driver did not reject) must not look like a healthy
live-edge wait. The watchdog counts only while input has been accepted and
not yet returned:

```
StallWatchdog:
  fed():       queued_++ ; if queued_ == 1: since_ = now
  produced():  queued_ = max(0, queued_ - 1); since_ = now
  stalled():   return queued_ >= kOutputBuffers && now - since_ > 3 s
```

A healthy HLS stream waiting for the next segment has `queued_ == 0` (every
OUTPUT buffer was consumed) and does not trip it. Tripping it fails the read,
and `StreamPlayer` recycles the instance.

### 5.5 Exit path

Unchanged: `finishProcess()` in `mqtt_controller.cpp` calls `closeAll()` →
`abort()` on every worker, publishes offline, `drain(2s)`, then
`exitIfBusy()`. A worker blocked in `STREAMOFF` is what makes `drain()` return
false; `_exit` follows. The kernel then closes the V4L2 fd on process exit,
which may itself be slow in the driver, but at that point the launch script's
poll is watching a process in exit, which is the same as today for a process
with a stuck resolver thread.

## 6. DMA-BUF frame handoff

### 6.1 `DecodedFrame` extension (plain ints, no libdrm header)

```cpp
// decoder_backend.h (proposed)
enum class FramePixelFormat { Unknown = 0, Yuv420p, DrmPrime };

// Description of one DMA-BUF image. fourcc and modifier use the DRM values
// (DRM_FORMAT_NV12 == 0x3231564E, DRM_FORMAT_MOD_LINEAR == 0) written as
// integers so this header stays free of libdrm. fd[i] are dup()'ed file
// descriptors owned by DecodedFrame::hold; the receiver must not close them.
struct DrmPrimeDesc {
    static constexpr int kMaxPlanes = 4;
    int num_planes = 0;
    int fd[kMaxPlanes] = {-1, -1, -1, -1};     // may repeat the same fd (single-buffer NV12)
    uint32_t offset[kMaxPlanes] = {};
    uint32_t pitch[kMaxPlanes] = {};
    uint32_t fourcc = 0;
    uint64_t modifier = 0;
    enum class Matrix { Bt601, Bt709, Bt2020 } matrix = Matrix::Bt709;
    enum class Range { Limited, Full } range = Range::Limited;
    // Stable identity of the underlying buffer for GEM handle caching (§7.1).
    // (decoder instance id << 32) | (capture generation << 16) | index.
    uint64_t buffer_id = 0;
};

struct DecodedFrame {
    ... existing fields ...
    DrmPrimeDesc drm;                          // valid when format == DrmPrime; data[] are null then
    // Ownership token. While any copy is alive the decoder keeps this buffer
    // out of its capture queue. Dropping the last copy returns it. Copyable,
    // thread-safe, may outlive the decoder. Empty for Yuv420p frames.
    std::shared_ptr<const void> hold;
};
```

The "valid until the next `readFrame()`" rule for `data[]` is unchanged for
`Yuv420p`. For `DrmPrime` the rule is replaced by the token: the fds and the
image are valid for as long as `hold` is alive, on any thread.

### 6.2 Token construction in the decoder

```
makeToken(index):
  tok = std::shared_ptr<ReleaseToken>(new ReleaseToken{ mailbox_, index, dup'd fds },
            [](ReleaseToken* t) { t->mailbox->release(t->index); close each fd; delete t; })
  return tok   # stored as shared_ptr<const void> in DecodedFrame::hold (aliasing constructor)
```

Each token dups the exported fds so the renderer's view does not depend on
the decoder's own `cap_.fd` table surviving a device reopen (§4.5).

### 6.3 Release mailbox

```
struct ReleaseMailbox {
  std::mutex mu; std::vector<uint32_t> released; bool detached = false; uint32_t generation;
  void release(index):  lock; if (!detached) released.push_back(index)
  void detach():        lock; detached = true; released.clear()
  std::vector<uint32_t> drain(): lock; swap out
};

requeueReleased():                                  # worker thread, top of every readFrame loop
  for index in mailbox_->drain():
     if index belongs to the current capture generation: VIDIOC_QBUF(CAPTURE, index)
     # else: buffer of a torn-down generation, nothing to do; its memory went with the last fd
```

Releases happen on the tick thread (renderer drops the previous FB after a
flip, `OwnedFrame` replaced, `clearViewport()`), or in `~Worker` on the
reaper. None of those threads touch the fd.

### 6.4 How many capture buffers the player holds

Outside the decoder at any instant, per stream:

1. `Worker::frame` — the one frame `sessionLoop` produced and `tick()` has
   not taken yet (`frameReady`). `sessionLoop` waits for `!frameReady`
   before reading again, so at most 1.
2. The renderer's **pending** FB — staged by `present()`, committed, flip not
   yet signalled. At most 1.
3. The renderer's **on-screen** FB — released only when a later flip
   completes. At most 1, and held indefinitely when the stream stalls.

So `kHeldByPlayer = 3`, and `setupCapture()` asks for
`MIN_BUFFERS_FOR_CAPTURE + 3 + 1`. With H.264 min buffers often at 4–6 and
HEVC higher, a 1080p NV12 stream is roughly 3 MiB × (6..10) ≈ 18–30 MiB of
CMA; nine 1080p streams approach 300 MiB **plus** the vendor/mainline
decoders' own reference buffers. Whether CoreELEC's CMA pool on the GS-King X
covers that is *verify* (§3.1). The tile-size variant picker that
`ffmpeg_decoder.h` already defers is the lever if it does not: a 3x3 tile
needs a 426x240 variant, not 1080p.

### 6.5 `StreamPlayer` changes for the token

- `OwnedFrame` gains `DrmPrimeDesc drm; std::shared_ptr<const void> hold;`.
  `copyOwned()` becomes `takeOwned()`: for `Yuv420p` it copies bytes as now;
  for `DrmPrime` it copies the descriptor and the token and copies no planes.
- `OwnedFrame::view()` returns the descriptor and token for `DrmPrime`.
- Replacing `Worker::frame` drops the previous token (frame skipped, buffer
  returns to the decoder). This keeps the existing "present at most one
  frame per tick" behaviour and lets a live stream catch up by dropping.
- `renderer_.clearViewport()` on a topology change must also drop every
  token the renderer holds (§7.2).
- `tick()` ends with `renderer_.commit()` (§7.2), after the tile loop.

## 7. `DrmRenderer`: from stub to scanout

### 7.1 Import and FB cache

```
present(frame, dest):                               # tick thread
  if frame.format == Yuv420p: return false with the existing "not implemented" message (CPU upload is out of scope)
  fb = fbCache_.find(frame.drm.buffer_id)
  if !fb:
      for each distinct fd: drmPrimeFDToHandle(fd_, fd, &handle)    # same dmabuf imported twice yields the same GEM handle;
      handles are refcounted in a map keyed by buffer_id, closed once with DRM_IOCTL_GEM_CLOSE when the last FB using them is gone
      drmModeAddFB2WithModifiers(fd_, w, h, fourcc, handles, pitches, offsets, modifiers, &fb_id, DRM_MODE_FB_MODIFIERS if modifier != 0)
      fb = fbCache_.insert(buffer_id, {fb_id, handles, frame.hold})
  fb.hold = frame.hold                               # the renderer now co-owns the buffer
  staged_[tileIndex(dest)] = { fb, src = full frame, dst = letterbox(frame.w, frame.h, dest) }
  return true
```

The FB cache is keyed by `buffer_id`, so the same capture buffer coming back
every N frames reuses its `fb_id` instead of re-importing. A capture
generation change makes new ids; stale entries are evicted when their token
is the last reference.

### 7.2 Commit, flip, and release

```
commit():                                           # tick thread, once per tick, after the tile loop
  if staged_ is empty and nothing to clear: return
  req = drmModeAtomicAlloc()
  for each tile with a staged FB: set plane props FB_ID, CRTC_ID, SRC_X/Y/W/H (16.16), CRTC_X/Y/W/H, COLOR_ENCODING, COLOR_RANGE
  for each tile cleared: FB_ID = 0 on its plane
  drmModeAtomicCommit(fd_, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, this)
  on EBUSY (previous flip pending): keep staged_, retry next tick (the token keeps the buffer)
  on success: pending_ = staged_; staged_.clear()

pumpEvents():                                       # tick thread, start of tick, non-blocking poll on fd_
  drmHandleEvent -> page_flip_handler:
     for each tile: release onScreen_[tile].hold (drop shared_ptr); onScreen_[tile] = pending_[tile]; pending_[tile] = {}

clearViewport():
  staged_, pending_ (after its flip), onScreen_ -> drop all holds; commit FB_ID = 0 on every used plane
```

`DRM_MODE_ATOMIC_NONBLOCK` keeps the tick thread off vblank. One commit per
tick for all tiles, not one per `present()`. The `Renderer` interface needs
`virtual void commit() {}` (SoftwareRenderer ignores it) and
`clearViewport()` promoted from `SoftwareRenderer` to `Renderer`, since
`StreamPlayer` calls both.

DRM master: `launch-coreelec.sh` stops Kodi first, so card0 has no master.
If something still holds it, `drmModeAtomicCommit` fails with `EACCES` and
`present()`/`commit()` report it with the existing
"requires GS-King X / CoreELEC" prefix.

### 7.3 Flipping the hardware gate

`StreamPlayer` and `decodeUrl` currently treat `isHardware()` as a refusal.
Proposed rule: the refusal stays unless the renderer declares it accepts
`DrmPrime`.

```cpp
// renderer.h (proposed)
virtual bool accepts(FramePixelFormat format) const { return format == FramePixelFormat::Yuv420p; }
```

- `SoftwareRenderer`: default (Yuv420p only). A hardware decoder paired with
  it still fails with `WorkerFail::Hardware` and the existing message. The
  `stream_player_test` case that stores `hardware = true` keeps passing.
- `DrmRenderer`: `accepts(DrmPrime)` true, `accepts(Yuv420p)` false. A
  software decoder paired with it fails `present()` as it does today.
- Selection: `--decoder software|amlogic` on the `--url` path and an MQTT-mode
  flag of the same name, both compiled only with `AMLOGIC_V4L2_DECODE`. The
  factory handed to `StreamPlayer` is `AmlogicV4l2Decoder` when selected.
  Mixed walls (hardware for H.264, CPU fallback for MJPEG) need the renderer
  to accept both formats; that is a CPU upload path in `DrmRenderer` and is
  deferred.

### 7.4 One video plane, nine tiles

Mainline `meson_drm` exposes one video overlay plane (VD1). The `Renderer`
contract is "one `present()` draws one stream into its cell", which maps
onto planes only for the 1x1 layout. The handoff in §6 does not depend on
how tiles are composed, so the renderer can grow in stages:

1. **Fullscreen first.** 1x1 layout → the DMA-BUF goes straight to VD1 with
   `SRC_*` = frame, `CRTC_*` = letterboxed cell. The plane's scaler does the
   fit. Multi-tile layouts fail `present()` for tiles past the first with
   a clear message. This stage proves decode, export, import, flip, release,
   and the stuck-device handling on real hardware.
2. **Composition.** Candidates, to be chosen by the §3.1 probe:
   - GPU: GBM + EGL + GLES2, `EGL_EXT_image_dma_buf_import` of each NV12
     DMA-BUF, draw all tiles into a GBM surface, scan out on the primary
     plane. This is Kodi-GBM's path and the most capable. It adds mesa to
     the device AppImage's link set (CE22 ships it; CE21 does not).
   - GE2D: mainline `meson-ge2d` (V4L2 M2M, 5.12+) blits with scaling, but
     its upstream format list is RGB-only, so NV12 input needs a patched
     driver. *verify* on the CE22 kernel.
   - Second video plane: G12B has VD2; mainline exposes only VD1. A 1x2
     layout could use VD1 + VD2 on a patched kernel. Not planned.

The token contract (§6.2) is the same in every stage: a composer that reads
a DMA-BUF holds the token until its read completes, then drops it.

## 8. Failure matrix

| Event | Who notices | Behaviour |
| --- | --- | --- |
| No `/dev/video*` with the codec | `open()` | `failOpen`, `StreamPlayer` backs off 1s and retries. Logged once. |
| Instance cap (`EBUSY`/`ENOMEM`) | `open()` | Same; the tile shows nothing until another closes. The 1s retry picks the slot up. |
| Access unit larger than `sizeimage` | `feedOnePacket` | `fail`; recycle via `StreamPlayer`. `sizeimage` is sized to make this a bug report, not a path. |
| Decoder error (`POLLERR`) | `readFrame` | `fail`; recycle. |
| No output for 3s with input queued | watchdog | `fail`; recycle. The other tiles are unaffected. |
| `SOURCE_CHANGE` mid-stream | `readFrame` | Drain, `REQBUFS(0)` or reopen, new generation; held frames stay valid. |
| `abort()` during `poll` | worker | Returns within one 100ms slice; `quiesce()` on the worker. |
| `abort()` during `av_read_frame` | worker | FFmpeg interrupt callback, as today. getaddrinfo still ignores it. |
| `STREAMOFF`/`close(fd)` hangs | worker in `quiesce()` | Worker stays on the reaper; per-id restart waits; `kMaxBusyRetired` cap; `exitIfBusy` at shutdown. Identical to a stuck resolver. |
| Renderer holds a frame forever (stalled stream) | decoder | Capture queue sized for it; `REQBUFS(0)` never waits on the renderer. |
| DRM master held by Kodi | `commit()` | `EACCES`; logged; no scanout; decoding continues and frames are dropped by token replacement. |
| Process `_exit` with a stuck worker | kernel | V4L2 fd and DMA-BUFs released by the kernel at exit. |

## 9. Testing without a GS-King X

The backend's logic (state machine, mailbox, watchdog, abort, quiesce,
orphan-vs-reopen) is testable on CI if every kernel touch goes through a
small seam:

```cpp
struct V4l2Device {                                  // one virtual per syscall; the real one is ioctl()/poll()/mmap()
    virtual int ioctl(unsigned long req, void* arg) = 0;
    virtual int poll(pollfd*, nfds_t, int timeout_ms) = 0;
    virtual void* mmap(size_t len, off_t off) = 0;
    virtual int close() = 0;
};
```

- `amlogic_v4l2_decoder_test` (no FFmpeg, no device): a scripted
  `FakeV4l2Device` returns canned `QUERYCAP`/`ENUM_FMT`/`G_FMT`, hands out
  capture indices, and can be told to **wedge** on `STREAMOFF`. Checks: the
  init sequence order; `timestamp` → pts mapping; mailbox requeue happens on
  the worker and never from the releasing thread; token outliving the decoder
  only closes fds; `SOURCE_CHANGE` with and without `ORPHANED_BUFS`; the
  watchdog ignoring an idle queue; `abort()` leaving `poll` within one slice.
- `stream_player_test` additions, same shape as `StallingDecoder`: a fake
  whose `quiesce()` blocks shows the worker on the reaper, the other tile
  presenting, `drain(2s)` false, and `exitIfBusy` taken. A fake renderer with
  `accepts(DrmPrime)` true shows the hardware gate opening, and a token
  counter shows every token released on `clearViewport()`, `remove`, and
  shutdown.
- `mqtt_exit_test`: a decoder stuck in `quiesce()` instead of `open()`
  exercises AUDIT4 A1/A2 with the new blocking point.
- Host kernel fidelity (optional, local only): `modprobe vicodec` gives a
  real V4L2 stateful decoder with `SOURCE_CHANGE`, `FLAG_LAST`, and
  `EXPBUF`. Its FWHT bitstream comes from vicodec's own encoder instance
  via `v4l2-ctl`. CI runners cannot load modules, so this is not in ctest.
- Device (manual, **requires GS-King X / CoreELEC**): the §3.1 probe; then
  `--url URL --decoder amlogic --renderer drm --frames 300` on a 1x1
  layout; then nine `add`s over MQTT to find the CMA and instance limits;
  then `stop` / `remove` / `restart` / SIGTERM while a stream is at a live
  edge and while one is mid-`STREAMOFF` (pull the network).

## 10. Changes outside the new class

Listed so the scope is visible. All are small; none is implemented here.

| File | Change |
| --- | --- |
| `src/decoder_backend.h` | `FramePixelFormat::DrmPrime`, `DrmPrimeDesc`, `DecodedFrame::drm` and `::hold`, `virtual void quiesce() {}`. |
| `src/renderer.h` | `virtual bool accepts(FramePixelFormat) const`, `virtual void commit() {}`, `virtual void clearViewport()` moved up from `SoftwareRenderer`. |
| `src/stream_player.cpp` | `quiesce()` guard in `sessionLoop`; `takeOwned()` for `DrmPrime`; `renderer_.commit()` at the end of `tick()`; the hardware gate reads `renderer_.accepts()`. |
| `src/stream_player.h` | `OwnedFrame` carries `DrmPrimeDesc` and the token; `StreamPlayer` takes `Renderer&` instead of `SoftwareRenderer&`. |
| `src/drm_renderer.*` | Import, FB cache, atomic commit, flip events, release (§7). Still behind `AMLOGIC_DRM_KMS`. |
| `src/app.cpp`, `src/mqtt_controller.cpp` | `--decoder` flag; factory selection; `decodeUrl` accepts hardware when the renderer does. |
| `CMakeLists.txt` | `option(AMLOGIC_V4L2_DECODE ... OFF)`; `src/amlogic_v4l2_decoder.cpp` compiled only when on; `-D_FILE_OFFSET_BITS=64` for the armv7hf target; the new unit test target. |
| `scripts/build-coreelec-appimage.sh` | Stage 2 only: mesa/EGL in the link set if the GPU composer is chosen. |

## 11. Open questions (superseded)

The six questions that were here assumed a mainline CE22 decoder. They are
replaced by the ten blocking probes in [Appendix A §A.6](#a6-open-questions-that-block-freezing-the-design).

---

# Appendix A. `DecoderBackend` contract (Path 5)

Implementation-ready. Cross-checked against `origin/main` at `6d9d146`
(`decoder_backend.h`, `stream_player.{h,cpp}`, `renderer.h`,
`drm_renderer.{h,cpp}`, `mqtt_controller.cpp` after AUDIT5 #33/#34) and
research drafts [#36](https://github.com/yyolk/amlogic-multistream/pull/36)
(Kodi/`amcodec` history),
[#37](https://github.com/yyolk/amlogic-multistream/pull/37) (CE21 vs CE22 ABI),
[#38](https://github.com/yyolk/amlogic-multistream/pull/38) (9-stream limits),
[#39](https://github.com/yyolk/amlogic-multistream/pull/39) (vendor SDK / `libamcodec`),
[#40](https://github.com/yyolk/amlogic-multistream/pull/40) (vendor DRM + UVM),
[#41](https://github.com/yyolk/amlogic-multistream/pull/41) (FFmpeg: no `v4l2m2m` in the image),
[#43](https://github.com/yyolk/amlogic-multistream/pull/43) (code examples + three backend shapes).
Nothing in this appendix is compiled. Do not edit `src/` from it until a
spike lands.

## A.0 What a coder is allowed to assume

| Fact | Source on `main` | Do not break |
| --- | --- | --- |
| `open` / `close` / `readFrame` / `isHardware` are single-threaded; only `abort()` is cross-thread and it latches | `decoder_backend.h:40–47` | Hardware teardown that can sit in D state must not run on the tick thread. |
| `close()` is invoked from `StreamPlayer::releaseDecoder` **after** `sessionLoop` returned and the worker was `join`ed (`reapFinished`) | `stream_player.cpp:116–125, 136–156` | `close()` on a hardware backend must be CPU-only once `quiesce()` has run. |
| `tick()` never joins an in-flight `open`/`read`. A call that ignores `abort()` (getaddrinfo) stays on the reaper | `stream_player.h:26–34` | A wedged `codec_close` / `VIDIOC_STREAMOFF` is the same shape. |
| Per-id restart waits for `reaperBusy(id)` (AUDIT4 A3). New opens wait while `busyRetiredCount() >= kMaxBusyRetired` (AUDIT5 F3). Bound is `kMaxStreams + kMaxBusyRetired - 1` = 17 | `stream_player.h:37–50`, `stream_player.cpp:444–459` | Hardware must add **no** threads and **no** new caps. |
| MQTT exit after #34: `finishProcess` is `[[noreturn]]`. It `closeAll()`, publishes offline under `closeMu_`, `drain(2s)`, optional 500 ms DISCONNECT, then `_exit`. `exitIfBusy` has no production caller | `mqtt_controller.cpp:305–329`; AUDIT6 N4 | A wedged worker is abandoned by `_exit`. Do not put `codec_close` / `STREAMOFF` on this thread either — it would miss the 2 s bound the way the pre-#34 MQTT join did (AUDIT5 X1). |
| `isHardware()` is a hard refuse (`WorkerFail::Hardware`) in `sessionLoop` and in `decodeUrl` | `stream_player.cpp:189–196`; `app.cpp:187–191` | Flip only through `Renderer::accepts`. |
| Headers stay free of FFmpeg and libdrm types | `decoder_backend.h:14`, `renderer.h:10` | DMA-BUF description is plain ints. |
| `FfmpegDecoder` stays software. `prepareAmlogicHwaccel` stays a no-op | `ffmpeg_decoder.cpp:55–67` | New class, new file. |
| CE22 is vendor 5.15 + `libamcodec`, not mainline `meson-vdec` | #37, #39, #41, #43 | Stage-1 spike is shape A. Shape B is conditional on Q1. |

## A.1 Precise API delta (proposed headers, doc only)

Current `FramePixelFormat` is `{Unknown, Yuv420p}`. Current `DecoderBackend`
has `open` / `close` / `isHardware` / `abort` / default-false `readFrame`.
Current `Renderer` has `init` / `present` / `shutdown`. Current
`StreamPlayer` is constructed with `SoftwareRenderer&` and copies YUV bytes
into `OwnedFrame`.

The delta is three virtuals (`quiesce`, `accepts`, `commit`), one promoted
method (`clearViewport`), two formats, and a token. Defaults keep every
existing test green.

### A.1.1 `src/decoder_backend.h` (proposed whole file)

```cpp
#pragma once

#include <cstdint>
#include <memory>
#include <string>

constexpr int64_t kDecodedFrameNoPts = INT64_MIN;

// CPU image (Yuv420p) or a DMA-BUF image (DrmPrime) or a heartbeat from a
// backend that presents through the kernel video layer (SelfPresented).
//
// Yuv420p: plane pointers owned by the DecoderBackend. Valid until the next
// readFrame() or close() on that instance. A renderer that needs the bytes
// longer must copy them.
//
// DrmPrime: data[] are null. drm and hold describe the image. hold keeps
// the capture slot out of the decoder queue until the last copy is dropped,
// on any thread. The decoder must not VIDIOC_QBUF (or equivalent) that slot
// until then.
//
// SelfPresented: data[] are null, hold is empty, width/height/pts may be
// filled from codec_get_vdec_info so tick() can count a live tile. The
// kernel video layer is already showing the picture.
enum class FramePixelFormat {
    Unknown = 0,
    Yuv420p,
    DrmPrime,
    SelfPresented,
};

// Plain-int DMA-BUF description. fourcc/modifier use the DRM numeric
// values (DRM_FORMAT_NV12 == 0x3231564E, DRM_FORMAT_MOD_LINEAR == 0) so
// this header does not include libdrm. fd[i] are dup()'ed descriptors
// owned by DecodedFrame::hold; the receiver must not close them.
struct DrmPrimeDesc {
    static constexpr int kMaxPlanes = 4;
    int num_planes = 0;
    int fd[kMaxPlanes] = {-1, -1, -1, -1};
    uint32_t offset[kMaxPlanes] = {};
    uint32_t pitch[kMaxPlanes] = {};
    uint32_t fourcc = 0;
    uint64_t modifier = 0;
    enum class Matrix { Bt601, Bt709, Bt2020 };
    enum class Range { Limited, Full };
    Matrix matrix = Matrix::Bt709;
    Range range = Range::Limited;
    // Stable identity for GEM-handle caching:
    // (decoder_instance_id << 32) | (capture_generation << 16) | index.
    uint64_t buffer_id = 0;
};

struct DecodedFrame {
    const uint8_t* data[4] = {};
    int linesize[4] = {};
    int width = 0;
    int height = 0;
    FramePixelFormat format = FramePixelFormat::Unknown;
    int64_t pts = kDecodedFrameNoPts;
    int time_base_num = 0;
    int time_base_den = 0;
    DrmPrimeDesc drm;
    // Lifetime token. Empty for Yuv420p and SelfPresented.
    std::shared_ptr<const void> hold;
};

// Demux/decode backend. close() must be safe to call more than once,
// including after a failed open().
//
// open(), close(), readFrame(), isHardware(), and quiesce() run on one
// thread and must not be called concurrently with each other. abort() is
// the exception: a different thread may call it while open() or
// readFrame() is blocked. abort() latches for the life of the object.
// The in-flight open() or readFrame() then returns, and a later open()
// or readFrame() fails without blocking.
//
// close() runs after that call has returned, on the thread that owns the
// session. Under StreamPlayer that is the tick thread, after reapFinished()
// joined. A libc call that ignores abort() (getaddrinfo) and a driver call
// that ignores abort() (codec_close, VIDIOC_STREAMOFF) do not return until
// the call itself finishes — those calls belong in quiesce(), on the
// worker, never in close() on the tick thread.
class DecoderBackend {
public:
    virtual ~DecoderBackend() = default;
    virtual bool open(const std::string& url) = 0;
    virtual void close() = 0;
    virtual bool isHardware() const = 0;

    // Unblock an in-flight open() or readFrame(). Safe from another thread
    // and safe more than once. Default is a no-op.
    virtual void abort() {}

    // Release the device on the session thread before close() runs
    // elsewhere. StreamPlayer calls this from a destructor guard in
    // sessionLoop, after abort(), on every exit path, never concurrently
    // with open() or readFrame(). May block in the driver; that is why it
    // is here. After a completed quiesce(), close() must not block.
    // Default is a no-op so FfmpegDecoder stays unchanged.
    virtual void quiesce() {}

    // Format this instance will produce after a successful open().
    // Default Yuv420p matches FfmpegDecoder. A hardware backend returns
    // DrmPrime or SelfPresented so the gate in sessionLoop can ask the
    // renderer before the first readFrame().
    virtual FramePixelFormat outputFormat() const {
        return FramePixelFormat::Yuv420p;
    }

    // Pull one decoded video frame.
    // true  -> frame is filled. For Yuv420p, plane pointers stay valid
    //          until the next readFrame() or close(). For DrmPrime, the
    //          image stays valid while hold is alive. For SelfPresented,
    //          width/height/pts are informational.
    // false -> end of stream or a hard error. frame is left zeroed.
    virtual bool readFrame(DecodedFrame& frame) {
        frame = DecodedFrame{};
        return false;
    }
};
```

### A.1.2 `src/renderer.h` (proposed whole file)

```cpp
#pragma once

#include "decoder_backend.h"
#include "layout_engine.h"

// Presents one frame into a layout cell, or (SelfPresented) records that
// the decoder already showed it.
//
// Implementations that consume Yuv420p copy plane bytes during present().
// Implementations that consume DrmPrime take a copy of DecodedFrame::hold
// and must drop it when the FB is no longer on screen or pending. This
// header does not include FFmpeg or libdrm types.
//
// The caller places the stream. LayoutEngine::compute() supplies dest.
// One present() draws one stream. Unused cells stay untouched.
//
// init() prepares a viewport. shutdown() releases it and is safe more
// than once. Not thread-safe: tick thread only.
class Renderer {
public:
    virtual ~Renderer() = default;

    virtual bool init(int viewportWidth, int viewportHeight) = 0;
    virtual bool present(const DecodedFrame& frame, const LayoutEngine::Rect& dest) = 0;
    virtual void shutdown() = 0;

    // Hardware gate. SoftwareRenderer keeps the default (Yuv420p only),
    // so StreamPlayer's existing "refusing hardware decoder" test stays
    // green. DrmRenderer returns true for DrmPrime. An AmvideoRenderer
    // returns true for SelfPresented.
    virtual bool accepts(FramePixelFormat format) const {
        return format == FramePixelFormat::Yuv420p;
    }

    // Wall-level transaction. StreamPlayer calls this once per tick after
    // the tile loop. SoftwareRenderer ignores it. DrmRenderer submits one
    // non-blocking atomic commit for every staged tile.
    virtual void commit() {}

    // Drop every retained token / RGB cell. Safe more than once. No-op
    // before init(). StreamPlayer already calls SoftwareRenderer's method
    // of this name on stop and topology change; promoting it is the only
    // way a hardware renderer can release on-screen FBs.
    virtual void clearViewport() {}
};
```

### A.1.3 `StreamPlayer` / `OwnedFrame` / `sessionLoop` (proposed delta, not applied)

`StreamPlayer` is constructed with `Renderer&`, not `SoftwareRenderer&`.
`OwnedFrame` gains `FramePixelFormat format`, `DrmPrimeDesc drm`, and
`std::shared_ptr<const void> hold`. `copyOwned` becomes `takeOwned`:

```
takeOwned(src, dst):
  if src.format == Yuv420p: copy the three planes as today; dst.hold = {}
  if src.format == DrmPrime:
      if src.hold is empty or src.drm.num_planes < 1: return false
      dst.drm = src.drm; dst.hold = src.hold; dst.planes stay empty
  if src.format == SelfPresented:
      dst.width/height/pts/time_base = src; dst.hold = {}; dst.planes empty
  dst.format = src.format
  return true
```

`sessionLoop` (only the changed bits):

```
sessionLoop(worker, id, url):
  decoder = worker->decoder.get()
  struct QuiesceOnExit {
      DecoderBackend* d;
      ~QuiesceOnExit() { if (d) d->quiesce(); }     // worker thread, every path
  } guard{decoder};

  opened = decoder && decoder->open(url)
  if !opened: set failed=Open unless stop; return
  if !renderer.accepts(decoder->outputFormat()):    // replaces isHardware() refuse
      set failed=Hardware unless stop; return       // SoftwareRenderer still refuses DrmPrime/SelfPresented
  announcedOpen = true
  loop:
      wait until stop || !frameReady
      if stop: return
      got = decoder->readFrame(frame)
      if stop: return                               // drop frame (token dtor → mailbox)
      if !takeOwned(frame, copy): failed=Read; return
      worker->frame = move(copy); frameReady = true
```

`tick()` after the tile loop calls `renderer_.commit()`.
`requestStop` is unchanged: `stop=true`, `cv.notify`, `decoder->abort()`
only — no ioctl, no `codec_close`, no `STREAMOFF`.

`releaseDecoder` is unchanged in *shape*: it still calls `close()` on the
tick thread after `join`. The contract is that `close()` after `quiesce()`
does not touch the device.

`WorkerFail::Hardware` message stays
`"refusing hardware decoder for <id> (requires GS-King X / CoreELEC)"`
when the renderer rejects the format. That keeps
`tests/stream_player_test.cpp:785–798` (`stats->hardware.store(true)` plus
a `SoftwareRenderer`) passing if the fake's `outputFormat()` is `DrmPrime`
or `SelfPresented`. A fake that reports `isHardware()` true and
`outputFormat() == Yuv420p` against a software renderer is accepted — that
is the CPU-copy fallback of shape A.

### A.1.4 What does **not** change

- `abort()` signature and latch rule.
- `FfmpegDecoder` (default `quiesce` / `outputFormat` / `accepts`).
- `kMaxBusyRetired`, `reaperBusy`, `busyRetiredCount`, `kExitJoinDeadline`.
- MQTT command JSON, stream-table cap of 9, layout names.
- `prepareAmlogicHwaccel`.
- Production `exitIfBusy` (still a test helper after #34).

## A.2 Threading map — worker vs tick

Three threads exist today. The backend adds none.

```
tick / main (mqtt_controller loop)     mosquitto thread          worker (one per visible stream)
----------------------------------     -----------------         --------------------------------
tick() / reapFinished()                broker I/O, commands      sessionLoop:
present() / commit() / clearViewport() closeMu_ / closing_         open(url)          ← all device calls
requestStop: stop=true; abort()        (no decoder calls)          readFrame()        ← poll + feed
drain() waits on worker.done                                       QuiesceOnExit      ← STREAMOFF / codec_close
releaseDecoder → close()  [CPU only]                               done = true
finishProcess: closeAll, publish, drain(2s), _exit
```

`abort()` body, both shapes: `abort_.store(true); write(wake_fd_, 1)`. No
ioctl. No `codec_close`. No sysfs. That is the only decoder entry from the
tick thread while the worker is alive.

### A.2.1 Shape A — `libamcodec` (recommended stage-1)

| Call | Thread | Can it ignore `abort()`? | Bound |
| --- | --- | --- | --- |
| `avformat_open_input` / `av_read_frame` | worker | getaddrinfo yes; socket no (`AVIOInterruptCB` + `rw_timeout`) | Same as `FfmpegDecoder`. Reaper already covers the lookup. |
| `dlopen("libamcodec.so")` | worker, once per process | No | Cached. |
| sysfs: `vcodec_profile`, `dec_control`, `nal_skip_policy`, `tsync/enable`, `freerun_mode`, `vfm/map` | worker, inside `open` | Unlikely | Worker. |
| `codec_init` / `codec_init_cntl` | worker | Firmware load, rare hang | Worker. Lands on the reaper if it never returns. |
| `codec_write` (blocking ES push) | worker | **Yes** — Kodi and moonlight retry on `EAGAIN`; a full VDEC can block | `poll({amstream_fd, wake_fd_})` or non-blocking `codec_write` in 100 ms slices. `abort()` writes `wake_fd_`. |
| `codec_checkin_pts_us64` | worker | No | — |
| `codec_get_vdec_info` / `codec_get_vdec_state` | worker | Should be ioctl-fast | Heartbeat for `SelfPresented` `readFrame`. |
| `/dev/video10` `DQBUF`/`QBUF` pump (amlvideo; moonlight-embedded needs this or the pipeline stalls) | **worker**, same instance | `DQBUF` if blocking | `O_NONBLOCK` + `poll` with `wake_fd_`. Do not spawn a second thread (#43 example 12 does; that would break the 17-thread bound). |
| `/sys/class/video/axis` (window) | tick, inside `present(SelfPresented)` | sysfs write | Not a decoder ioctl. One layer — see Q6. |
| `codec_reset` / `codec_close` / close(`/dev/amstream*`) / close(`/dev/video10`) | **worker, `quiesce()` only** | **Yes.** CoreELEC's own notes call Kodi's open/close "faulty"; `vfree()` on a non-multi close is a reported teardown bug (#36, #38 §8.2.3) | Reaper + 2 s `_exit`. |
| `close()` after `quiesce()` | tick | Must not | `dlclose` is not taken (process-lifetime handle). Free FFmpeg contexts only. |

### A.2.2 Shape B — direct V4L2 + PRIME (conditional on Q1)

| Call | Thread | Can it ignore `abort()`? | Bound |
| --- | --- | --- | --- |
| Demux / BSF / `av_read_frame` | worker | Same as A | Same. |
| `open(/dev/videoN)`, `S_FMT`, `REQBUFS`, `EXPBUF`, `STREAMON`, `SUBSCRIBE_EVENT` | worker | `STREAMON` may load firmware | Worker. |
| `DQBUF` | worker | If blocking | Never blocking: `O_NONBLOCK` + `poll({fd, wake_fd_}, 100ms)`. |
| `QBUF` (OUTPUT feed, CAPTURE requeue) | worker | Wedged driver can take its lock | Capture requeue **only** via the mailbox drained at the top of `readFrame` on the worker. Token dtor on the tick thread pushes an index, it does not ioctl. |
| `DECODER_CMD STOP` + drain | worker | Driver may never emit `FLAG_LAST` | 2 s + `abort_`. |
| `VIDIOC_STREAMOFF`, `REQBUFS(0)`, `close(fd)` | **worker, `quiesce()` only** | **Yes.** Vendor vb2 / firmware reset | Reaper + 2 s `_exit`. |
| `drmPrimeFDToHandle`, `AddFB2`, atomic commit, `drmHandleEvent` | tick | `EBUSY` if previous flip pending | Non-blocking commit; retry next tick. Token keeps the buffer. |
| `close()` after `quiesce()` | tick | Must not | `munmap` OUTPUT, drop unused fds, `mailbox->detach()`, free FFmpeg. |

### A.2.3 Proof: no `STREAMOFF` / `codec_close` / `close(device fd)` on tick

Walk of the production call graph on `main` plus the proposed guard:

1. `requestStop` (`stream_player.cpp:105–114`) sets `stop`, notifies, calls
   `abort()`. That is the entire decoder surface from tick while the worker
   lives. `abort()` is specified as atomic + `eventfd` write.
2. `sessionLoop` returns only after `open`/`readFrame` returned. The
   proposed `QuiesceOnExit` then runs **on that same worker**, before
   `done = true` (`stream_player.cpp:266–270` sets `done` in the thread
   lambda after `sessionLoop` returns — put the guard *inside*
   `sessionLoop` so `quiesce()` happens before `done`).
3. `reapFinished` (`:136–156`) `join`s only when `done` is true, then
   calls `releaseDecoder` → `close()` on the tick thread.
4. Therefore `close()` on tick is reached only after `quiesce()` returned.
   A backend that puts `codec_close` / `STREAMOFF` / `close(fd)` exclusively
   in `quiesce()` cannot issue them on tick.
5. If `quiesce()` hangs, `done` stays false, `reapFinished` leaves the
   worker on `reaper_`, `tick()` does not join it, `drain(2s)` returns
   false, `finishProcess` `_exit`s. That is AUDIT4 A1 applied to the
   device, and AUDIT5 X1 applied to the network thread: the presenting
   thread and the process-exit path never sit out an uninterruptible
   driver call.
6. The `--url` path (`app.cpp:decodeUrl`) is single-threaded: `close()`
   may call `quiesce()` itself. That is the owning thread, so it is
   allowed (and already noted in §4.7).
7. `startWorker`'s `std::thread` failure (`stream_player.cpp:272–278`)
   calls `close()` before `open()` ran. No device is open.

A forbidden alternative, recorded so it is not re-proposed: `abort()`
itself issuing `STREAMOFF` or `codec_close` from the tick thread "to
speed the worker out". That puts the hang on the wrong thread and races
the worker's own ioctls on the same fd.

## A.3 Reaper / F3 / busy-retired if a hardware ioctl wedges

No new fields. The existing two gates already compose (AUDIT6: "The two
gates compose. A pending restart waits on both (`stream_player.cpp:444-459`)
and still runs once.").

| Event | What the current code does | Hardware case |
| --- | --- | --- |
| `stop` / `remove` / `restart` / hide / `closeAll` | `requestStop` + move worker onto `reaper_` | Same. `abort()` wakes `poll`/`codec_write` if the call is interruptible. |
| Worker still inside `open`/`read`/`quiesce` | `done == false` → `reaperBusy(id)` true → no second open for that id (A3) | A wedged `codec_init` / `codec_write` / `codec_close` / `STREAMOFF` occupies that slot until the driver returns or `_exit`. |
| Nine such workers, any ids | `busyRetiredCount() >= kMaxBusyRetired` (9) → `tick` starts no new open for any id; logs once; live tiles keep presenting (F3) | Same cap. A wall of nine wedged `codec_close`s blocks a tenth `add` from opening — the stream table still accepts it, MQTT still answers `added`, there is just no picture. |
| Thread bound | start requires `busyRetired < 9` and live sessions ≤ 8 after a retire, so running decode threads ≤ 17 | Holds if and only if the backend creates **zero** extra threads. Shape A's amlvideo pump stays on the worker, not a helper thread. |
| Process exit | `finishProcess`: `closeAll` + offline + `drain(2s)` + `_exit` | A worker in `quiesce()` makes `drain` return false (or true only after it unsticks). Either way `_exit` within ~2.5 s (AUDIT6 worst case with a blackholed broker was 2.52 s). Kernel then closes the fds. |
| AUDIT6 N3 | A full F3 cap also delays *healthy* reopens until one lookup finishes | Same trade-off for a healthy stream while nine `codec_close`s are stuck. Do not add a per-id reservation in the spike. |

`StallingDecoder` in `tests/stream_player_test.cpp:167–208` already models
"open ignores abort". The hardware equivalent is that class with the sleep
moved into `quiesce()`: T1 / A3 / F3 / `mqtt_exit_test` stay the tests;
only the blocking virtual changes.

## A.4 Two backend shapes — pick stage-1

#43's three shapes collapse to the two the task asked for. Shape B's
"CPU-copy V4L2" is listed as a fallback *inside* B, not as a third
contract.

| | **A. `libamcodec` self-present** (CPU-copy optional) | **B. Direct V4L2 + PRIME** |
| --- | --- | --- |
| Kernel interface | `/dev/amstream*`, `AMSTREAM_IOC_*`, VFM, `/dev/video10` (amlvideo metadata), `/dev/amvideo` | `/dev/videoN` M2M (`aml-vcodec-dec`, node 26 in source). `VIDIOC_*`, `VB2_DMABUF` or `EXPBUF` |
| Userspace lib | `dlopen("libamcodec.so")` — proprietary, binary in the image (#43). Headers from CoreELEC's `libamcodec` tarball | None. Raw ioctl. FFmpeg for demux/BSF only. CoreELEC's `libavcodec` has **no** `*_v4l2m2m` (#41) |
| What `readFrame` returns | `SelfPresented` heartbeat (`codec_get_vdec_info`). Optional later: mmap a post-processed buffer into `Yuv420p` if a dump path exists (not the Kodi path) | `DrmPrime` + token. CPU-copy fallback: `Yuv420p` from `mmap` CAPTURE (shape B-copy) |
| Who draws | Kernel video layer. `present()` writes `/sys/class/video/axis` (and z-order) for that tile | `DrmRenderer` imports the token and commits, **or** a GPU/GE2D compositor writes one OSD FB (#40) |
| Multi-stream decode | Vendor `MAX_INSTANCE_MUN`: 2 on CE21 (patch 003), 9 on CE22 source. Time-sliced, one VPU (#38) | Same `vdec_create` pool **if** the node is the vendor port (#43). Mainline `meson-vdec` is 1 session and is **not** what CE boots |
| Multi-stream display | One `amvideo` layer is what Kodi drives. N VFM maps into N windows is **unproven** (Q6). Community 2-stream PiP needed a kernel rebuild (#38) | Vendor DRM: 3 OSD + 2 video planes in the G12B DT (#40) — still not 9. A 3×3 wall is composition either way |
| Abort | `wake_fd_` in the `codec_write` / `video10` poll set | `wake_fd_` in the V4L2 `poll` set |
| Quiesce | `codec_close` + close amstream/video10 + restore VFM map | `STREAMOFF` both queues + `close(fd)` |
| Proven on this class? | **Yes, 1 stream** — Kodi `AMLCodec.cpp` on both CE lines. 2+ streams: one VDR report with the cap patch removed | **No** on stock CoreELEC. `v4lplayer` in `media_modules-aml/tools` is the source-level sample (#40); the node may not be registered (Q1) |
| License / AppImage | `dlopen` avoids a link-time dep. Header redistribution is unresolved (#43 licensing). CE21 needs the **armhf** tarball (Q5) | No proprietary link. Fits the existing "link CoreELEC's FFmpeg + libdrm" rule |

### A.4.1 Recommended stage-1 for the device spike: **shape A, one stream, CE22**

CE22 is vendor 5.15 + `libamcodec`. The V4L2 M2M node is not a given.
CoreELEC's FFmpeg cannot open `h264_v4l2m2m`. Mainline `meson-vdec` is a
different distribution. The only path a non-Kodi process has actually
used on this OS is shape A (#43 examples 1, 12, 13).

Spike goal, in order, on one GS-King X running the CE22 image the AppImage
targets:

1. **Contract, not the wall.** One `http://` H.264 HLS rendition, Kodi
   stopped. `AmlogicAmcodecDecoder` (`isHardware()==true`,
   `outputFormat()==SelfPresented`) + a 20-line `AmvideoRenderer`
   (`accepts(SelfPresented)`, `present` writes `axis` for the 1x1 cell).
   Prove: picture on HDMI, `abort()` unblocks `codec_write` within one
   100 ms slice, `quiesce()`/`codec_close` returns or sits on the reaper,
   SIGTERM publishes offline and `_exit`s inside 2.5 s with the worker
   wedged (force a hang by commenting out `codec_close` and sleeping in
   `quiesce()` if the driver is well-behaved).
2. **Two instances, decode only.** Second `codec_init` while the first is
   running. Watch `dmesg | grep vdec_create`. This is Q2/Q10 and decides
   whether CE22's header value of 9 is real for a non-Kodi client.
3. **Two pictures.** If step 2 opened, try two VFM maps and two `axis`
   windows (Q6). Expected failure: one layer, second picture black or
   replacing the first. That failure selects composition (stage-2) rather
   than "N video layers".
4. **Only if Q1 is yes:** a 200-line V4L2 client against `/dev/video26`
   (OUTPUT H264, CAPTURE NV12, mmap copy to a PPM). This is shape B-copy,
   and it reuses `SoftwareRenderer` + `accepts(Yuv420p)` so the hardware
   gate does not even open. Do not start PRIME work until this decode
   loop is stable.

Do **not** start stage-1 as shape B-PRIME. It needs a registered M2M node
(Q1), a DRM master with a video plane that accepts the decoder's
fourcc/modifier (Q3), and a token/FB cache — three unverified surfaces —
and it is not what Kodi on this image does.

Shape B-PRIME remains the right *architecture* for a 3×3 wall if Q1 and
the compositor import (EGL or GE2D, #40) both pass. The contract in A.1
is written so that swap is a new class + a renderer that `accepts(DrmPrime)`,
not a `StreamPlayer` rewrite. `OwnedFrame` already carries the token;
`QuiesceOnExit` already covers `STREAMOFF`.

## A.5 Test plan

### A.5.1 Host / CI — no GS-King X, no `/dev/video*`, no `libamcodec`

All of these hook the new virtuals through fakes. None opens a device.
`AMLOGIC_V4L2_DECODE` / `AMLOGIC_AMCODEC` stay off in CI, same as
`AMLOGIC_DRM_KMS`.

| Test (proposed target) | What it pins | Existing analogue |
| --- | --- | --- |
| `stream_player_test` + `QuiescingStallDecoder` | `quiesce()` sleeps 1.2 s ignoring `abort()`. Other tile keeps presenting. `reaperBusy(id)` blocks restart (A3). Nine ids hit F3 (`maxInFlight` ≤ 9). `drain(2s)` false. `exitIfBusy(0)` taken in a child | `StallingDecoder` (`SURVIVING4 T1`, `AUDIT4 A3`, `AUDIT5 F3`) with the sleep moved to `quiesce()` |
| `stream_player_test` + `FakeHardwareDecoder` (`outputFormat()==DrmPrime`, `isHardware()==true`) against `SoftwareRenderer` | Gate still refuses; `WorkerFail::Hardware`; one open, one close, zero presents | `stats->hardware.store(true)` at `:785` |
| same fake against `AcceptingRenderer` (`accepts(DrmPrime)==true`) | Gate opens; `takeOwned` moves the token; `present` sees `hold.use_count()>=2`; `clearViewport` / `remove` / shutdown drop to 0; mailbox index is **not** ioctled on the tick thread (fake records the calling `thread::id`) | new |
| `stream_player_test` + `SelfPresentedDecoder` + `AmvideoFakeRenderer` | `accepts(SelfPresented)`; `present` called with empty `data[]`; `commit` is a no-op | new |
| `amlogic_v4l2_decoder_test` + `FakeV4l2Device` | Init order; pts ring; mailbox requeue on the worker; token outliving the decoder only `close`s fds; `SOURCE_CHANGE` with/without `ORPHANED_BUFS`; watchdog ignores an idle queue; `abort()` leaves `poll` in one slice; `STREAMOFF` wedge never runs on the test's "tick" thread | §9 seam, still valid for shape B |
| `amlogic_amcodec_decoder_test` + `FakeAmcodec` | `codec_init` / `write` / `checkin_pts` / `close` order; `write` blocked until `wake_fd_`; `close` wedge is `quiesce` not `close`; VFM map save/restore | new, no real `.so` |
| `mqtt_exit_test` | Decoder stuck in `quiesce()` (not `open()`). SIGTERM, broker up: offline retained, rc 0, ≤2.5 s (AUDIT4 A1, AUDIT5 F1/X1). Auth give-up: rc 1, same bound (A2) | current `mqtt_exit_test` with the interpose moved |
| `present_smoke_test` | `--renderer drm` still fails without `AMLOGIC_DRM_KMS`; `--decoder amlogic` is rejected or not compiled on the host build | existing |

TSan: run `stream_player_test` and `mqtt_exit_test` with
`-fsanitize=thread` as AUDIT6 did. The token mailbox mutex is the new
shared surface.

### A.5.2 Must-run-on-device checklist (GS-King X, CE22 first, then CE21)

Kodi stopped (`systemctl stop kodi` / `launch-coreelec.sh` path). Record
`/etc/os-release` and `uname -r` at the top of the log.

1. Run the ten probes in A.6. Stop if Q1/Q2/Q3/Q5 disagree with the
   assumed CE22 vendor stack; update this appendix before writing more
   code.
2. Shape A 1-stream spike (A.4.1 step 1) against a local H.264 HLS
   fixture on disk **and** one live HTTP URL. Confirm HDMI picture, then
   `stop` / `restart` / SIGTERM.
3. Force-wedge: `abort()` during `codec_write` (pull the Ethernet cable
   mid-segment) and during `quiesce()` (inject a 5 s sleep if the driver
   returns). Confirm the other tile — if any — still ticks, and that
   process exit is ≤2.5 s.
4. A.4.1 steps 2–3 (second instance, second picture).
5. Q1 follow-up: if `/dev/video26` exists, shape B-copy one clip to a PPM
   via `v4l2-ctl` or the 200-line client. Do not import PRIME yet.
6. Soak: 30 min one-stream shape A. Watch `dmesg` for `vfree`,
   `codec_mm`, `DECODER_EMERGENCY_NO_MEM`.
7. Repeat 1–3 on CE21. Expect instance cap 2 and no usable DRM video
   plane.

## A.6 Open questions that block freezing the design

Ten questions. Each one changes the stage-1 class or the renderer. Run
on the box; do not freeze A.1–A.4 until Q1, Q2, Q3, and Q5 are answered
for the image being shipped.

1. **Is the vendor V4L2 M2M node registered on stock CE22?**
   Decides whether shape B exists at all, or only after a custom kernel.
   ```sh
   ls -l /dev/video* /dev/media* 2>/dev/null
   lsmod | grep -E 'amvdec_ports|aml_vcodec|vdec'
   # v4l-utils may be missing; a static v4l2-ctl or a 20-line QUERYCAP loop is enough:
   for n in /dev/video*; do echo "== $n"; v4l2-ctl -d "$n" --info --list-formats-out --list-formats 2>/dev/null; done
   dmesg | grep -iE 'aml-vcodec|video26|amvdec_ports'
   ```

2. **What instance cap does the running kernel enforce?**
   Decides the hardware admission number under the UI cap of 9.
   ```sh
   grep -R "MAX_INSTANCE" /sys/module/*/parameters 2>/dev/null
   dmesg | grep -E 'vdec_create|vdec_init|max_decode_instance'
   # then open N copies of a tiny H.264 ES via the spike (or a 10-line codec_init loop)
   # and record the first failure + `dmesg | grep vdec_create`
   ```

3. **What display path is actually usable with Kodi stopped?**
   Decides `AmvideoRenderer` vs `DrmRenderer` for stage-1 `present()`.
   ```sh
   ls -l /dev/dri /dev/fb0 /dev/amvideo /dev/video10 2>/dev/null
   cat /sys/class/vfm/map
   readlink -f /sys/class/drm/card0/device/driver 2>/dev/null
   cat /sys/kernel/debug/dri/0/state 2>/dev/null
   # if drm_info/modetest exist:
   drm_info /dev/dri/card0 2>/dev/null | head
   fuser -v /dev/dri/card0 /dev/amvideo /dev/fb0 2>/dev/null
   ```

4. **Does `codec_close` / `VIDIOC_STREAMOFF` hang after `abort()`?**
   Decides whether `quiesce()` is insurance or the load-bearing A1 fix.
   ```sh
   # during the shape-A spike: SIGSTOP the worker (or break in gdb) mid-codec_write,
   # then from another thread call codec_close / STREAMOFF and time it.
   # dmesg | grep -iE 'vfree|timeout|vdec_release|stuck'
   ```

5. **Which `libamcodec.so` is on the box, and may we `dlopen` it from the AppImage?**
   CE22 tarball is aarch64; CE21 userspace is armhf. Wrong ELF = stage-1
   cannot even `codec_init`.
   ```sh
   ls -l /usr/lib/libamcodec* /usr/include/amcodec/codec.h 2>/dev/null
   file /usr/lib/libamcodec.so*
   readelf -h /usr/lib/libamcodec.so* | grep -E 'Class|Machine'
   # headers present? if not, they are only in the CoreELEC package tarball
   ```

6. **Can two `STREAM_TYPE_FRAME` instances show two independent pictures?**
   Decides whether shape A can ever be a wall or is a 1-stream HDMI proof.
   ```sh
   cat /sys/class/vfm/map
   ls /sys/class/video/
   # after two codec_init: write two axis rectangles
   # echo "x y x2 y2" > /sys/class/video/axis
   # look for video2 / pip / path0 vs path1 nodes; record whether the second
   # picture replaces the first
   dmesg | grep -iE 'vfm|amvideo|video_composer'
   ```

7. **How big is `codec_mm` / CMA on this GS-King X image?**
   Decides 1080p vs tile-sized renditions before any 9-up attempt (#38 §6).
   ```sh
   grep -iE 'Cma|cma|codec' /proc/meminfo
   dmesg | grep -iE 'codec_mm|ion-dev|reserved'
   ls /proc/device-tree/reserved-memory/ 2>/dev/null
   # if dtc is present: dtc -I fs -O dts /proc/device-tree 2>/dev/null | grep -A2 codec_mm
   ```

8. **Does the image FFmpeg actually lack `v4l2m2m`?**
   Confirms #41 on the box; if symbols exist, shape B-copy can use
   `h264_v4l2m2m` instead of raw ioctl (still not the backend — no abort
   hook — but a faster probe).
   ```sh
   ffmpeg -hide_banner -decoders 2>/dev/null | grep v4l2
   ffmpeg -hide_banner -hwaccels 2>/dev/null
   strings /usr/lib/libavcodec.so* | grep -c v4l2m2m   # expect 0
   ```

9. **If Q1 is yes: capture fourcc, modifier, `EXPBUF` vs `DMABUF` import, min buffers?**
   Decides token layout and whether PRIME can feed DRM or only CPU copy.
   ```sh
   v4l2-ctl -d /dev/video26 --all
   v4l2-ctl -d /dev/video26 --list-ctrls
   v4l2-ctl -d /dev/video26 --get-ctrl min_buffers_for_capture
   # REQBUFS + EXPBUF probe (or the 200-line client). Record ORPHANED_BUFS in capabilities.
   ```

10. **What VFM map does a non-Kodi `STREAM_TYPE_FRAME` instance need so the picture is not black?**
    The #36/#39 blank-output failure is the most-reported multi-decode
    "success". Stage-1 is worthless if HDMI stays black with a live decoder.
    ```sh
    cat /sys/class/vfm/map
    # Kodi's working single-stream map (forum): 
    # echo "rm default" > /sys/class/vfm/map
    # echo "add default vdec.h264.00 ppmgr amvideo" > /sys/class/vfm/map
    # record maps created automatically by codec_init, and the dmesg line
    # `vdec_init ... VDEC_TYPE_FRAME_BLOCK`
    dmesg | grep -iE 'vdec_init|VDEC_TYPE|vfm'
    ```

## A.7 Spike file list (still not this PR)

When Q1–Q3 and Q5 are answered for CE22, the first production PR is
expected to touch only:

| File | Why |
| --- | --- |
| `src/decoder_backend.h` | Apply A.1.1 (defaults keep CI green). |
| `src/renderer.h` | Apply A.1.2. |
| `src/software_renderer.h` | `clearViewport` becomes `override`; `accepts` stays default. |
| `src/stream_player.{h,cpp}` | `Renderer&`, `QuiesceOnExit`, `takeOwned`, `accepts` gate, `commit()`. |
| `src/amlogic_amcodec_decoder.{h,cpp}` | Shape A. Compiled only with `-DAMLOGIC_AMCODEC=ON`. |
| `src/amvideo_renderer.{h,cpp}` | `accepts(SelfPresented)`. Same flag. |
| `tests/stream_player_test.cpp` | `QuiescingStallDecoder` + accepting-renderer cases. |
| `CMakeLists.txt` | Option off by default. |

Shape B (`amlogic_v4l2_decoder.*`, `DrmRenderer` growth) waits on Q1 and
Q3. `FfmpegDecoder` is not in that list.
