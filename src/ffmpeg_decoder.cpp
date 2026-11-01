#include "ffmpeg_decoder.h"

#include <cerrno>
#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace {

// Socket read timeout passed to protocols that understand rw_timeout
// (microseconds). File inputs ignore it.
constexpr const char* kRwTimeoutUs = "15000000";

void logAvError(const char* prefix, const std::string& url, const char* what, int err) {
    std::cerr << prefix << " for '" << url << "': " << what;
    if (err < 0) {
        char buf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(err, buf, sizeof(buf));
        std::cerr << ": " << buf;
    }
    std::cerr << "\n";
}

// TODO(hwaccel): requires GS-King X / CoreELEC.
//
// Insertion point for Amlogic multi_vdec / V4L2 M2M
// (h264_v4l2m2m, hevc_v4l2m2m, AV_PIX_FMT_DRM_PRIME, hw_device_ctx).
// This software backend must leave the codec on the CPU. A real hwaccel
// path belongs on its own DecoderBackend so isHardware() can be true there
// without changing this class.
void prepareAmlogicHwaccel(AVCodecContext* codec) {
    if (codec == nullptr) {
        return;
    }
    codec->hw_device_ctx = nullptr;
}

}  // namespace

FfmpegDecoder::~FfmpegDecoder() {
    close();
}

bool FfmpegDecoder::failOpen(const char* what, int err) {
    logAvError("FfmpegDecoder::open failed", url_, what, err);
    close();
    return false;
}

bool FfmpegDecoder::failRead(const char* what, int err) {
    logAvError("FfmpegDecoder::readFrame failed", url_, what, err);
    failed_ = true;
    return false;
}

void FfmpegDecoder::setAllowLocalFile(bool allow) {
    allow_local_file_ = allow;
}

bool FfmpegDecoder::open(const std::string& url) {
    close();
    if (url.empty()) {
        std::cerr << "FfmpegDecoder::open failed: empty url\n";
        return false;
    }
    // avformat_open_input takes a C string. A NUL would hide the tail from
    // the whitelist check below and from libavformat.
    if (url.find('\0') != std::string::npos) {
        std::cerr << "FfmpegDecoder::open failed: url contains NUL\n";
        return false;
    }
    url_ = url;

    // av_log_set_level is process-wide. Decoder failures are still reported
    // with av_strerror, so libav's own info and warning lines stay quiet.
    av_log_set_level(AV_LOG_ERROR);

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rw_timeout", kRwTimeoutUs, 0);
    av_dict_set(&opts, "user_agent", "amlogic-multistream/0.1", 0);
    // file is only on the explicit --url local-file allowance. hls is the
    // protocol name libavformat registers; crypto covers AES-128 media.
    av_dict_set(&opts, "protocol_whitelist",
                allow_local_file_ ? "file,http,https,tls,tcp,crypto,hls"
                                  : "http,https,tls,tcp,crypto,hls",
                0);

    // libavformat initializes network libraries itself. Do not call
    // avformat_network_init(); it is optional and not recommended.
    const int open_rc = avformat_open_input(&fmt_, url_.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (open_rc < 0) {
        // Failed open frees a context it allocated and nulls *ps.
        fmt_ = nullptr;
        return failOpen("avformat_open_input", open_rc);
    }

    fmt_->flags |= AVFMT_FLAG_GENPTS;

    const int info_rc = avformat_find_stream_info(fmt_, nullptr);
    if (info_rc < 0) {
        return failOpen("avformat_find_stream_info", info_rc);
    }

    video_index_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_index_ < 0) {
        return failOpen("no video stream", video_index_);
    }

    // Keep the first video stream av_find_best_stream selected (HLS master:
    // the first variant in playlist order). AVDISCARD_ALL on every other
    // stream makes hls.c playlist_needed() drop the rest, so later segments
    // of those variants are not fetched. Probe reads that already happened
    // inside avformat_find_stream_info stay fetched.
    for (unsigned i = 0; i < fmt_->nb_streams; ++i) {
        AVStream* candidate = fmt_->streams[i];
        if (candidate == nullptr) {
            continue;
        }
        candidate->discard =
            static_cast<int>(i) == video_index_ ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }

    AVStream* stream = fmt_->streams[video_index_];
    if (stream == nullptr || stream->codecpar == nullptr) {
        return failOpen("video stream has no codec parameters", AVERROR(EINVAL));
    }
    time_base_num_ = stream->time_base.num;
    time_base_den_ = stream->time_base.den;

    // Software decoder by codec id. Never avcodec_find_decoder_by_name() of
    // an Amlogic / v4l2m2m wrapper — see prepareAmlogicHwaccel().
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == nullptr) {
        return failOpen("no software decoder for video codec", AVERROR_DECODER_NOT_FOUND);
    }

    codec_ = avcodec_alloc_context3(decoder);
    if (codec_ == nullptr) {
        return failOpen("avcodec_alloc_context3", AVERROR(ENOMEM));
    }
    const int par_rc = avcodec_parameters_to_context(codec_, stream->codecpar);
    if (par_rc < 0) {
        return failOpen("avcodec_parameters_to_context", par_rc);
    }
    codec_->pkt_timebase = stream->time_base;

    prepareAmlogicHwaccel(codec_);
    if (codec_->hw_device_ctx != nullptr) {
        return failOpen(
            "refusing hw_device_ctx on the software backend "
            "(requires GS-King X / CoreELEC)",
            AVERROR(ENOSYS));
    }

    const int codec_rc = avcodec_open2(codec_, decoder, nullptr);
    if (codec_rc < 0) {
        return failOpen("avcodec_open2", codec_rc);
    }

    decoded_ = av_frame_alloc();
    yuv_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    if (decoded_ == nullptr || yuv_ == nullptr || pkt_ == nullptr) {
        return failOpen("frame/packet alloc", AVERROR(ENOMEM));
    }

    opened_ = true;
    std::cout << "FfmpegDecoder::open " << url_
              << " stream=" << video_index_
              << " codec=" << avcodec_get_name(codec_->codec_id)
              << " software isHardware=0\n";
    return true;
}

void FfmpegDecoder::close() {
    sws_freeContext(sws_);
    sws_ = nullptr;
    av_frame_free(&decoded_);
    av_frame_free(&yuv_);
    av_packet_free(&pkt_);
    avcodec_free_context(&codec_);
    avformat_close_input(&fmt_);
    url_.clear();
    video_index_ = -1;
    time_base_num_ = 0;
    time_base_den_ = 0;
    opened_ = false;
    input_eof_ = false;
    decode_eof_ = false;
    failed_ = false;
    logged_format_ = false;
}

bool FfmpegDecoder::publishYuv420(const AVFrame* src, DecodedFrame& out) {
    if (src == nullptr || src->width <= 0 || src->height <= 0) {
        std::cerr << "FfmpegDecoder::readFrame failed: decoded frame has no image\n";
        return false;
    }
    const auto src_fmt = static_cast<AVPixelFormat>(src->format);
    if (src_fmt == AV_PIX_FMT_NONE) {
        std::cerr << "FfmpegDecoder::readFrame failed: decoded frame has no pixel format\n";
        return false;
    }

    if (!logged_format_) {
        const char* name = av_get_pix_fmt_name(src_fmt);
        std::cout << "FfmpegDecoder: video " << src->width << "x" << src->height
                  << " " << (name != nullptr ? name : "unknown")
                  << " -> yuv420p\n";
        logged_format_ = true;
    }

    if (yuv_->format != AV_PIX_FMT_YUV420P || yuv_->width != src->width || yuv_->height != src->height) {
        av_frame_unref(yuv_);
        yuv_->format = AV_PIX_FMT_YUV420P;
        yuv_->width = src->width;
        yuv_->height = src->height;
        const int buf_rc = av_frame_get_buffer(yuv_, 32);
        if (buf_rc < 0) {
            logAvError("FfmpegDecoder::readFrame failed", url_, "av_frame_get_buffer", buf_rc);
            return false;
        }
    }
    const int writable = av_frame_make_writable(yuv_);
    if (writable < 0) {
        logAvError("FfmpegDecoder::readFrame failed", url_, "av_frame_make_writable", writable);
        return false;
    }

    sws_ = sws_getCachedContext(
        sws_,
        src->width,
        src->height,
        src_fmt,
        src->width,
        src->height,
        AV_PIX_FMT_YUV420P,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr);
    if (sws_ == nullptr) {
        const char* name = av_get_pix_fmt_name(src_fmt);
        std::cerr << "FfmpegDecoder::readFrame failed: cannot convert "
                  << (name != nullptr ? name : "unknown") << " to yuv420p\n";
        return false;
    }

    const int scaled = sws_scale(
        sws_,
        src->data,
        src->linesize,
        0,
        src->height,
        yuv_->data,
        yuv_->linesize);
    if (scaled != src->height) {
        std::cerr << "FfmpegDecoder::readFrame failed: sws_scale returned " << scaled << "\n";
        return false;
    }

    out.format = FramePixelFormat::Yuv420p;
    out.width = src->width;
    out.height = src->height;
    out.pts = src->best_effort_timestamp != AV_NOPTS_VALUE ? src->best_effort_timestamp : src->pts;
    if (out.pts == AV_NOPTS_VALUE) {
        out.pts = kDecodedFrameNoPts;
    }
    out.time_base_num = time_base_num_;
    out.time_base_den = time_base_den_;
    for (int i = 0; i < 3; ++i) {
        out.data[i] = yuv_->data[i];
        out.linesize[i] = yuv_->linesize[i];
    }
    return true;
}

bool FfmpegDecoder::readFrame(DecodedFrame& out) {
    out = DecodedFrame{};
    if (!opened_) {
        std::cerr << "FfmpegDecoder::readFrame: decoder is not open\n";
        return false;
    }
    if (failed_ || decode_eof_) {
        return false;
    }

    int skipped_non_video = 0;
    while (true) {
        const int recv = avcodec_receive_frame(codec_, decoded_);
        if (recv == 0) {
            const bool ok = publishYuv420(decoded_, out);
            av_frame_unref(decoded_);
            if (!ok) {
                out = DecodedFrame{};
                failed_ = true;
                return false;
            }
            return true;
        }
        if (recv != AVERROR(EAGAIN)) {
            if (recv == AVERROR_EOF) {
                decode_eof_ = true;
                return false;
            }
            return failRead("avcodec_receive_frame", recv);
        }
        if (input_eof_) {
            decode_eof_ = true;
            return false;
        }

        const int read_rc = av_read_frame(fmt_, pkt_);
        if (read_rc == AVERROR_EOF) {
            input_eof_ = true;
            const int flush_rc = avcodec_send_packet(codec_, nullptr);
            if (flush_rc < 0 && flush_rc != AVERROR_EOF) {
                return failRead("avcodec_send_packet flush", flush_rc);
            }
            continue;
        }
        if (read_rc < 0) {
            return failRead("av_read_frame", read_rc);
        }
        if (pkt_->stream_index != video_index_) {
            av_packet_unref(pkt_);
            if (++skipped_non_video > 8192) {
                return failRead("too many non-video packets before a video frame", 0);
            }
            continue;
        }

        const int sent = avcodec_send_packet(codec_, pkt_);
        av_packet_unref(pkt_);
        if (sent < 0) {
            return failRead("avcodec_send_packet", sent);
        }
    }
}
