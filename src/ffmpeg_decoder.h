#pragma once

#include "decoder_backend.h"

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

    // `--url` passes true so local fixture files open. The default is false:
    // avformat_open_input then allows only http, https, tls, tcp, crypto, and
    // hls. MQTT must not set this; `add` already rejects non-HTTP URLs.
    void setAllowLocalFile(bool allow);

    bool open(const std::string& url) override;
    void close() override;
    bool isHardware() const override { return false; }
    bool readFrame(DecodedFrame& frame) override;

private:
    bool failOpen(const char* what, int err);
    bool failRead(const char* what, int err);
    bool publishYuv420(const AVFrame* src, DecodedFrame& out);

    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVFrame* decoded_ = nullptr;
    AVFrame* yuv_ = nullptr;
    AVPacket* pkt_ = nullptr;
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
};
