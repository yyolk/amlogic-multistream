# Amlogic hardware `DecoderBackend` design

Status: design notes and pseudocode only. Nothing in this document is
implemented. The player still decodes on the CPU and `DrmRenderer::present()`
still refuses to scan out. Every hardware statement below **requires GS-King X
/ CoreELEC** to confirm; the ones marked *verify* have not been checked on a
box and the design keeps a fallback for each.

This proposes `AmlogicV4l2Decoder`, a second `DecoderBackend` beside
`FfmpegDecoder`, that demuxes with libavformat, decodes through the kernel's
V4L2 memory-to-memory stateful decoder, and hands DMA-BUF frames to
`DrmRenderer` without a CPU copy. It is written against the interfaces in
`src/decoder_backend.h`, `src/renderer.h`, `src/stream_player.h`, and the
AUDIT4 / AUDIT5 / SURVIVING notes in those files.

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
| Kernel | Amlogic vendor 4.9 | Mainline-derived 6.x |
| V4L2 decoder | Vendor `aml_vcodec_dec` ("multi_vdec" ports, several instances) — *verify* node name and instance cap | Staging `meson-vdec` (`drivers/staging/media/meson/vdec`) |
| Codecs via V4L2 | H.264, HEVC, VP9, MPEG-1/2/4, MJPEG (vendor) — *verify* | MPEG-1/2, H.264, VP9 upstream; HEVC only with CoreELEC/LibreELEC patches — *verify* with `VIDIOC_ENUM_FMT` |
| Capture pixel format | `NV21M` / `NV12M` with vendor "double write" controls — *verify* | `NV12M` (two DMA-BUFs) |
| `V4L2_BUF_CAP_SUPPORTS_ORPHANED_BUFS` | No (pre-5.0 vb2) | Yes (vb2 sets it unconditionally since 5.0) |
| DRM/KMS | Vendor kernel; CoreELEC-ng Kodi draws through fbdev + `/dev/amvideo`. Whether `/dev/dri/card0` exists with an atomic video plane is *verify*; likely **no** | Mainline `meson_drm`: `/dev/dri/card0`, atomic, one primary (OSD1) plane plus **one** video overlay plane (VD1) with NV12/NV21/YUV420 |
| 32/64-bit | 32-bit userspace: build with `-D_FILE_OFFSET_BITS=64` so `mmap()` offsets from `VIDIOC_QUERYBUF` fit | 64-bit |

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
    close(fd); return node                         # "meson-vdec" on CE22; vendor name on CE21 (verify)
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

## 11. Open questions

1. CE21 (Amlogic-ng): does `/dev/dri/card0` exist with a usable video plane,
   or is the display path `/dev/amvideo` + fbdev? If the latter, the
   "multi_vdec" goal needs an `AmvideoRenderer`, and the token contract above
   is what it would consume.
2. HEVC on CE22: present in CoreELEC's `meson-vdec` build, or only H.264 /
   MPEG-2 / VP9? Decides whether HEVC HLS falls back to the CPU.
3. Capture format on each kernel: `NV12M` linear, or an AFBC/compressed
   format that the video plane can scan out but a GPU composer cannot import
   without the matching modifier.
4. Instance cap and CMA budget for nine simultaneous decoders at the
   variants the test streams actually serve.
5. Whether `STREAMOFF` on the vendor driver can hang in practice, which
   decides how much the `quiesce()` machinery matters versus being insurance.
6. Presentation pacing: `tick()` presents whatever is ready every ~20ms with
   no pts clock. Hardware decode makes the burstiness of HLS segments
   visible on screen. A pts-paced present is a separate change; the token
   design allows holding a frame until its time without blocking the decoder.
