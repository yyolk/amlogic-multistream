#pragma once

#include "decoder_backend.h"

#include <atomic>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

// One software-decoded stream (HLS, HTTP, or a local file).
//
// isHardware() is always false. Amlogic hwaccel is not selected; open()
// calls a no-op hook marked "requires GS-King X / CoreELEC" so a future
// hardware backend has an obvious insertion point that is not this class.
//
// An HLS master lists every variant as its own program. open() keeps the
// first video stream in playlist order and sets AVDISCARD_ALL on the
// others, not the highest bitrate. Later segments of the discarded
// variants are not requested. Segments already read inside the probe
// pre-pass or avformat_find_stream_info() are not rewound. A tile-size
// bitrate picker is a later change.
//
// open() drops any previous stream first. On failure it returns false and
// leaves the object closed (safe to destroy or open() again).
// readFrame() yields FramePixelFormat::Yuv420p planes owned by this object.
class FfmpegDecoder : public DecoderBackend {
public:
    FfmpegDecoder() = default;
    ~FfmpegDecoder() override;

    FfmpegDecoder(const FfmpegDecoder&) = delete;
    FfmpegDecoder& operator=(const FfmpegDecoder&) = delete;
    FfmpegDecoder(FfmpegDecoder&&) = delete;
    FfmpegDecoder& operator=(FfmpegDecoder&&) = delete;

    // `--url` passes true only for a scheme-less path or file: / file:// so
    // fixture files open. Nested remote forms (http://, https://,
    // hls+http://, hls+https://) leave the default false (AUDIT7 B2):
    // avformat_open_input then allows only http, https, tls, tcp, crypto,
    // and hls. MQTT must not set this; `add` already rejects non-HTTP URLs.
    // open() sets format_whitelist to hls, mpegts, mov, mp4, mpeg, plus aac,
    // ac3, eac3, and mp3 so a packed-audio HLS rendition can open. The
    // demuxer accepted for this URL is only hls, mpegts, mov, mp4, or mpeg,
    // so a standalone mp3 or aac is rejected (AUDIT4 A4). The codec
    // whitelist is h264, hevc, mpeg4, mpeg2video, mpeg1video, mjpeg, aac,
    // mp3, mp3float, ac3, and eac3. On FFmpeg 6.1 the MP3 decoder is named
    // mp3float; mp3 stays for a build that registers that name (AUDIT4 A5).
    // gif and image demuxers are rejected on every path, including `--url`.
    // open() sets decoder max_pixels to SoftwareRenderer::kMaxExtent squared
    // so FFmpeg can refuse an oversize still before it materializes. Streams
    // created during avformat_find_stream_info (PSI-less MPEG-TS, extra
    // PIDs after a PMT, HLS of those files) are demuxed into existence
    // first so the same cap reaches those probe decoders (AUDIT10 D2).
    // The pre-pass uses the same probesize / max_analyze_duration window
    // as find_stream_info so a normal HLS master is not drained. If that
    // window ends on a timestamp gap, the queued packets are kept and
    // the probe is clamped so a later PID cannot be born without the
    // cap (AUDIT11 D3). Those clamps run after avformat_open_input.
    // FFmpeg 6.1.1 HLS still probes a TYPE=AUDIO rendition inside
    // read_header with find_stream_info(NULL), so that nested decoder
    // would keep max_pixels == INT_MAX. open() peeks http(s) even when
    // the path ends in .ts (mpegurl Content-Type or a redirect to a
    // master). Playlist lines end on \\n / \\r / \\0 like ff_get_line
    // and are capped at 4095 bytes, then chomped like ff_get_chomp_line.
    // Quoted values drop a final backslash. Any TYPE=AUDIO tag (URI=
    // or in-band) is rewritten or refused. A truncated #EXTM3U, a peek
    // that errors after any bytes, mpegurl that is not #EXTM3U, or a
    // non-playlist http(s) decoy is refused rather than opened
    // uncapped (AUDIT12 D4).
    // readFrame() also refuses to convert a frame that fails those
    // extent and stride caps (AUDIT9 D1).
    void setAllowLocalFile(bool allow);

    bool open(const std::string& url) override;
    void close() override;
    bool isHardware() const override { return false; }
    bool readFrame(DecodedFrame& frame) override;

    // Latches an interrupt for FFmpeg's AVIOInterruptCB. See abort_ below.
    void abort() override;

private:
    static int interruptCallback(void* opaque);
    bool failOpen(const char* what, int err);
    bool failRead(const char* what, int err);
    bool publishYuv420(const AVFrame* src, DecodedFrame& out);
    int readDemuxPacket();
    void freeProbePackets();

    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVFrame* decoded_ = nullptr;
    AVFrame* yuv_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVPacket** probe_pkts_ = nullptr;
    unsigned probe_pkt_count_ = 0;
    unsigned probe_pkt_pos_ = 0;
    SwsContext* sws_ = nullptr;
    std::string url_;
    int video_index_ = -1;
    int time_base_num_ = 0;
    int time_base_den_ = 0;
    bool opened_ = false;
    bool input_eof_ = false;
    bool decode_eof_ = false;
    bool failed_ = false;
    bool logged_format_ = false;
    bool allow_local_file_ = false;
    // Set by abort() from the player thread. The decode thread's
    // interruptCallback reads it. close() does not clear it: a decoder that
    // has been aborted is not reused.
    std::atomic<bool> abort_{false};
};
