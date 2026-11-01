#include "ffmpeg_decoder.h"

#include "software_renderer.h"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <iostream>
#include <sstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace {

// Socket read timeout passed to protocols that understand rw_timeout
// (microseconds). File inputs ignore it. This bounds one silent socket
// operation on this decoder's thread. It does not bound a peer that keeps
// the connection just alive; DecoderBackend::abort() does.
constexpr const char* kRwTimeoutUs = "15000000";

// format_whitelist passed to avformat_open_input. hls.c copies it onto each
// rendition before AVDISCARD_ALL can drop a separate audio playlist, so the
// packed-audio demuxers (aac, ac3, eac3, mp3) have to be listed here
// (SURVIVING4 H1). gif, image2, and the other image demuxers are not listed.
constexpr const char* kFormatWhitelist =
    "hls,mpegts,mov,mp4,mpeg,aac,ac3,eac3,mp3";

// Post-open check of the caller's own demuxer. Packed-audio names stay off
// this list: a standalone song.mp3 with an ID3 APIC cover otherwise opens
// as mjpeg video and the cover is presented (AUDIT4 A4).
constexpr const char* kTopLevelFormatWhitelist = "hls,mpegts,mov,mp4,mpeg";

// Video codecs this player decodes, plus audio codecs
// avformat_find_stream_info may open while probing HLS or MPEG-TS. gif is
// not listed. On FFmpeg 6.1 the default MP3 decoder is named mp3float, so
// that name has to be listed or each probe logs that it is off the
// whitelist. mp3 stays for a build that registers that decoder name
// (AUDIT4 A5). This is not a TLS or privilege boundary.
constexpr const char* kCodecWhitelist =
    "h264,hevc,mpeg4,mpeg2video,mpeg1video,mjpeg,aac,mp3,mp3float,ac3,eac3";

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

// Same pixel-count bound as an 8192x8192 presentable frame. A 8224x32
// still is under this cap and is rejected later by extent, before convert.
int64_t presentableMaxPixels() {
    return static_cast<int64_t>(SoftwareRenderer::kMaxExtent) *
           static_cast<int64_t>(SoftwareRenderer::kMaxExtent);
}

bool decodedFramePresentable(const AVFrame* src) {
    if (src == nullptr || !SoftwareRenderer::sizeAcceptable(src->width, src->height)) {
        return false;
    }
    for (int i = 0; i < AV_NUM_DATA_POINTERS; ++i) {
        const int stride = src->linesize[i];
        if (stride > SoftwareRenderer::kMaxStride || stride < -SoftwareRenderer::kMaxStride) {
            return false;
        }
    }
    return true;
}

void setPresentableMaxPixels(AVDictionary** opts) {
    av_dict_set_int(opts, "max_pixels", presentableMaxPixels(), 0);
}

void setDecoderIoOptions(AVDictionary** opts, bool allow_local) {
    av_dict_set(opts, "rw_timeout", kRwTimeoutUs, 0);
    av_dict_set(opts, "user_agent", "amlogic-multistream/0.1", 0);
    av_dict_set(opts, "protocol_whitelist",
                allow_local ? "file,http,https,tls,tcp,crypto,hls"
                            : "http,https,tls,tcp,crypto,hls",
                0);
}

std::string stripUrlQuery(const std::string& url) {
    const auto cut = url.find_first_of("?#");
    return cut == std::string::npos ? url : url.substr(0, cut);
}

bool endsWithIgnoreCase(const std::string& s, const char* suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    if (s.size() < n) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i]))) {
            return false;
        }
    }
    return true;
}

// Peek only playlist-looking URLs. A .ts / .mp4 open must not pay an extra
// read; D4's master and the ADTS-audio master are .m3u8 (or hls+).
bool urlLooksLikeHlsPlaylist(const std::string& url) {
    if (url.compare(0, 4, "hls+") == 0 || url.compare(0, 4, "HLS+") == 0) {
        return true;
    }
    const std::string path = stripUrlQuery(url);
    return endsWithIgnoreCase(path, ".m3u8") || endsWithIgnoreCase(path, ".m3u");
}

std::string trimCopy(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

// HLS attribute list after the ':'. Commas inside quoted values stay inside
// the value. TYPE=AUDIO and URI= can appear in any order.
std::string hlsAttrValue(const std::string& attrs, const char* key) {
    const size_t key_len = std::char_traits<char>::length(key);
    bool in_quote = false;
    for (size_t i = 0; i < attrs.size(); ++i) {
        const char c = attrs[i];
        if (c == '"') {
            in_quote = !in_quote;
            continue;
        }
        if (in_quote) {
            continue;
        }
        const bool at_token = (i == 0) || attrs[i - 1] == ',' || attrs[i - 1] == ':' ||
                              std::isspace(static_cast<unsigned char>(attrs[i - 1]));
        if (!at_token || attrs.compare(i, key_len, key) != 0 || i + key_len >= attrs.size() ||
            attrs[i + key_len] != '=') {
            continue;
        }
        size_t v = i + key_len + 1;
        while (v < attrs.size() && std::isspace(static_cast<unsigned char>(attrs[v]))) {
            ++v;
        }
        if (v < attrs.size() && attrs[v] == '"') {
            const size_t end = attrs.find('"', v + 1);
            if (end == std::string::npos) {
                return {};
            }
            return attrs.substr(v + 1, end - (v + 1));
        }
        size_t end = v;
        while (end < attrs.size() && attrs[end] != ',') {
            ++end;
        }
        return trimCopy(attrs.substr(v, end - v));
    }
    return {};
}

bool hlsMasterHasExternalAudio(const std::string& text) {
    std::string line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.compare(0, 13, "#EXT-X-MEDIA:") != 0) {
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string attrs = line.substr(colon);
        if (hlsAttrValue(attrs, "TYPE") == "AUDIO" && !hlsAttrValue(attrs, "URI").empty()) {
            return true;
        }
    }
    return false;
}

std::string firstHlsStreamInfUri(const std::string& text) {
    std::string line;
    std::istringstream in(text);
    bool want_uri = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string trimmed = trimCopy(line);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed[0] == '#') {
            want_uri = trimmed.compare(0, 18, "#EXT-X-STREAM-INF:") == 0;
            continue;
        }
        if (want_uri) {
            return trimmed;
        }
    }
    return {};
}

bool urlHasScheme(const std::string& url) {
    const auto colon = url.find(':');
    if (colon == std::string::npos || colon == 0) {
        return false;
    }
    for (size_t i = 0; i < colon; ++i) {
        const unsigned char c = static_cast<unsigned char>(url[i]);
        if (!(std::isalnum(c) || c == '+' || c == '.' || c == '-')) {
            return false;
        }
    }
    return true;
}

// Same join FFmpeg 6.1.1 ff_make_absolute_url uses for playlist refs:
// scheme-bearing and // refs win, a leading / replaces the path, else
// the ref is appended after the last slash of the base (query stripped).
std::string resolvePlaylistUrl(const std::string& base, const std::string& ref) {
    const std::string rel = trimCopy(ref);
    if (rel.empty()) {
        return {};
    }
    if (urlHasScheme(rel)) {
        return rel;
    }
    if (rel.compare(0, 2, "//") == 0) {
        const auto colon = base.find(':');
        if (colon != std::string::npos) {
            return base.substr(0, colon + 1) + rel;
        }
        return rel;
    }
    std::string root = stripUrlQuery(base);
    if (!rel.empty() && rel[0] == '/') {
        const auto scheme = root.find("://");
        if (scheme == std::string::npos) {
            return rel;
        }
        const auto path = root.find('/', scheme + 3);
        if (path == std::string::npos) {
            return root + rel;
        }
        return root.substr(0, path) + rel;
    }
    const auto slash = root.rfind('/');
    if (slash == std::string::npos) {
        return rel;
    }
    return root.substr(0, slash + 1) + rel;
}

std::string readHlsPlaylistPreview(const std::string& url, bool allow_local, AVIOInterruptCB* cb) {
    AVDictionary* opts = nullptr;
    setDecoderIoOptions(&opts, allow_local);
    AVIOContext* pb = nullptr;
    const int rc = avio_open2(&pb, url.c_str(), AVIO_FLAG_READ, cb, &opts);
    av_dict_free(&opts);
    if (rc < 0 || pb == nullptr) {
        return {};
    }
    std::string out;
    unsigned char buf[4096];
    constexpr size_t kMax = 1024 * 1024;
    while (out.size() < kMax) {
        const int n = avio_read(pb, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        out.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
    }
    avio_closep(&pb);
    return out;
}

// FFmpeg 6.1.1 hls_read_header calls avformat_find_stream_info(pls->ctx, NULL)
// for an ID3-timestamped or TYPE=AUDIO rendition (AUDIT12 D4). There is no
// format-wide codec option, so that nested probe keeps max_pixels == INT_MAX.
// This player only presents video: open the first STREAM-INF playlist instead
// of a master that names an external audio group. Packed-audio renditions
// (the ADTS H1 case) stay unused, same as AVDISCARD_ALL after a full open.
void preferFirstHlsVariantIfExternalAudio(std::string* url, bool allow_local, AVIOInterruptCB* cb) {
    if (url == nullptr || url->empty() || !urlLooksLikeHlsPlaylist(*url)) {
        return;
    }
    std::string body = readHlsPlaylistPreview(*url, allow_local, cb);
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF &&
        static_cast<unsigned char>(body[1]) == 0xBB &&
        static_cast<unsigned char>(body[2]) == 0xBF) {
        body.erase(0, 3);
    }
    const auto nl = body.find('\n');
    const std::string start = trimCopy(body.substr(0, nl == std::string::npos ? body.size() : nl));
    if (start.compare(0, 7, "#EXTM3U") != 0 || !hlsMasterHasExternalAudio(body)) {
        return;
    }
    const std::string rel = firstHlsStreamInfUri(body);
    const std::string resolved = resolvePlaylistUrl(*url, rel);
    if (!resolved.empty() && resolved != *url) {
        *url = resolved;
    }
}

void freeStreamOptions(AVDictionary** stream_opts, unsigned count) {
    if (stream_opts == nullptr) {
        return;
    }
    for (unsigned i = 0; i < count; ++i) {
        av_dict_free(&stream_opts[i]);
    }
    av_freep(&stream_opts);
}

// FFmpeg 6.1.1's avformat_find_stream_info snapshots orig_nb_streams and
// applies options[i] only while i < that count (libavformat/demux.c). There
// is no format-wide codec-option default for streams created later.
bool formatNameIs(const AVFormatContext* fmt, const char* name) {
    const char* got = fmt != nullptr && fmt->iformat != nullptr ? fmt->iformat->name : nullptr;
    return got != nullptr && name != nullptr && av_match_list(got, name, ',') > 0;
}

bool formatMayAddStreams(const AVFormatContext* fmt) {
    if (fmt == nullptr) {
        return false;
    }
    // mpegts auto_guess and HLS of a PMT segment can add a PID after some
    // streams already exist. A headerless input starts at nb_streams == 0.
    return formatNameIs(fmt, "mpegts") || formatNameIs(fmt, "hls") || fmt->nb_streams == 0;
}

int64_t probeAnalyzeDuration(const AVFormatContext* fmt) {
    if (fmt != nullptr && fmt->max_analyze_duration > 0) {
        return fmt->max_analyze_duration;
    }
    // Same defaults as FFmpeg 6.1.1 avformat_find_stream_info.
    if (formatNameIs(fmt, "mpegts") || formatNameIs(fmt, "mpeg")) {
        return 7 * static_cast<int64_t>(AV_TIME_BASE);
    }
    return 5 * static_cast<int64_t>(AV_TIME_BASE);
}

int64_t packetTimeUs(const AVFormatContext* fmt, const AVPacket* pkt) {
    if (fmt == nullptr || pkt == nullptr || pkt->stream_index < 0 ||
        static_cast<unsigned>(pkt->stream_index) >= fmt->nb_streams) {
        return AV_NOPTS_VALUE;
    }
    const AVStream* st = fmt->streams[pkt->stream_index];
    if (st == nullptr || st->time_base.num <= 0 || st->time_base.den <= 0) {
        return AV_NOPTS_VALUE;
    }
    const int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
    if (ts == AV_NOPTS_VALUE) {
        return AV_NOPTS_VALUE;
    }
    return av_rescale_q(ts, st->time_base, AV_TIME_BASE_Q);
}

int firstVideoStreamIndex(const AVFormatContext* fmt) {
    if (fmt == nullptr) {
        return AVERROR_STREAM_NOT_FOUND;
    }
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        if (st != nullptr && st->codecpar != nullptr &&
            st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            return static_cast<int>(i);
        }
    }
    return AVERROR_STREAM_NOT_FOUND;
}

void freePacketQueue(AVPacket*** pkts, unsigned* count, unsigned* pos) {
    if (pkts == nullptr) {
        return;
    }
    if (*pkts != nullptr) {
        const unsigned n = count != nullptr ? *count : 0;
        for (unsigned i = 0; i < n; ++i) {
            av_packet_free(&(*pkts)[i]);
        }
        av_freep(pkts);
    }
    if (count != nullptr) {
        *count = 0;
    }
    if (pos != nullptr) {
        *pos = 0;
    }
}

int pushPacketQueue(AVPacket*** pkts, unsigned* count, const AVPacket* src) {
    AVPacket* copy = av_packet_alloc();
    if (copy == nullptr || av_packet_ref(copy, src) < 0) {
        av_packet_free(&copy);
        return AVERROR(ENOMEM);
    }
    AVPacket** grown = static_cast<AVPacket**>(av_realloc_array(*pkts, *count + 1, sizeof(*grown)));
    if (grown == nullptr) {
        av_packet_free(&copy);
        return AVERROR(ENOMEM);
    }
    *pkts = grown;
    (*pkts)[(*count)++] = copy;
    return 0;
}

int rewindAfterDiscovery(AVFormatContext* fmt, int64_t start) {
    int seek_rc = AVERROR(ENOSYS);
    if (fmt->nb_streams > 0) {
        seek_rc = av_seek_frame(fmt, 0, 0, AVSEEK_FLAG_BACKWARD);
        if (seek_rc < 0) {
            seek_rc = avformat_seek_file(fmt, 0, INT64_MIN, 0, INT64_MAX, 0);
        }
    }
    if (seek_rc < 0) {
        seek_rc = avformat_seek_file(fmt, -1, INT64_MIN, 0, INT64_MAX, 0);
    }
    if (seek_rc < 0) {
        seek_rc = av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
    }
    if (seek_rc < 0 && start >= 0) {
        seek_rc = av_seek_frame(fmt, -1, start, AVSEEK_FLAG_BYTE);
    }
    // Do not fall back to avio_seek on fmt->pb: HLS's pb is the playlist
    // file, so that seek can succeed while the segment stays at EOF.
    return seek_rc;
}

// Demux-only: av_read_frame creates AVStreams for PIDs that appear after
// avformat_open_input (PSI-less MPEG-TS, extra PIDs, HLS of those files)
// without opening a decoder. The window matches find_stream_info
// (probesize and max_analyze_duration) so a normal HLS master is not
// drained past the usual probe. If the container can rewind and this
// pass did not stop early, seek back so find_stream_info can apply
// max_pixels to every index that now exists (AUDIT10 D2). Local HLS
// cannot rewind, and a timestamp-gap stop is not a real end of the
// probe (AUDIT11 D3): those packets are queued for readFrame() and
// find_stream_info is not allowed to read further.
void discoverStreamsForProbeOptions(AVFormatContext* fmt,
                                    AVPacket*** queued,
                                    unsigned* queued_count,
                                    unsigned* queued_pos) {
    freePacketQueue(queued, queued_count, queued_pos);
    if (!formatMayAddStreams(fmt) || queued == nullptr || queued_count == nullptr) {
        return;
    }
    AVPacket* pkt = av_packet_alloc();
    if (pkt == nullptr) {
        return;
    }
    const int64_t start = fmt->pb != nullptr ? avio_tell(fmt->pb) : 0;
    const int64_t probesize = fmt->probesize > 0 ? fmt->probesize : 5000000;
    const int64_t max_analyze = probeAnalyzeDuration(fmt);
    int64_t read_size = 0;
    int64_t first_ts = AV_NOPTS_VALUE;
    bool stash_ok = true;
    // A 7 s PTS jump ends this window (mpegts default max_analyze_duration)
    // but FFmpeg 6.1.1's find_stream_info does not stop on that gap
    // (AUDIT11 D3). Keep the queue and clamp the probe in that case.
    bool stopped_early = false;
    while (read_size < probesize) {
        if (fmt->interrupt_callback.callback != nullptr &&
            fmt->interrupt_callback.callback(fmt->interrupt_callback.opaque) != 0) {
            stopped_early = true;
            break;
        }
        const int rc = av_read_frame(fmt, pkt);
        if (rc < 0) {
            break;
        }
        read_size += pkt->size > 0 ? pkt->size : 1;
        const int64_t ts = packetTimeUs(fmt, pkt);
        if (ts != AV_NOPTS_VALUE) {
            if (first_ts == AV_NOPTS_VALUE) {
                first_ts = ts;
            }
            if (ts - first_ts >= max_analyze) {
                if (stash_ok && pushPacketQueue(queued, queued_count, pkt) < 0) {
                    stash_ok = false;
                    freePacketQueue(queued, queued_count, queued_pos);
                }
                av_packet_unref(pkt);
                stopped_early = true;
                break;
            }
        }
        if (stash_ok && pushPacketQueue(queued, queued_count, pkt) < 0) {
            stash_ok = false;
            freePacketQueue(queued, queued_count, queued_pos);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    // HLS seek is not trustworthy here: av_seek_frame is often EPERM, and
    // a successful avio_seek only rewinds the playlist file. Always keep
    // the queue for hls. A timestamp-gap stop is the same hole: MPEG-TS
    // seek reports success, dropping the queue would let find_stream_info
    // create a late PID without max_pixels (AUDIT11 D3). Only rewind and
    // drop copies when this pass consumed its window (or hit EOF).
    if (!stopped_early && !formatNameIs(fmt, "hls") && rewindAfterDiscovery(fmt, start) >= 0) {
        freePacketQueue(queued, queued_count, queued_pos);
    } else {
        // Already consumed the probe window, or stopped early and must
        // not let find_stream_info read past it.
        fmt->probesize = 1;
        fmt->max_analyze_duration = 1;
    }
}

}  // namespace

FfmpegDecoder::~FfmpegDecoder() {
    close();
}

// Polled by libavformat on the decode thread during socket waits and the HLS
// live-edge sleep (100ms slices). A read deadline is not installed: on
// FFmpeg 6.1, interrupting that wait surfaces as EOF / AVERROR_EXIT and the
// HLS demuxer does not deliver the next segment. Each stream therefore has
// its own thread, and this callback is only the stop/shutdown latch.
// tcp_open calls getaddrinfo before any of those waits. glibc does not
// consult this callback there, and this player does not replace that lookup.
int FfmpegDecoder::interruptCallback(void* opaque) {
    if (opaque == nullptr) {
        return 0;
    }
    const auto* self = static_cast<const FfmpegDecoder*>(opaque);
    return self->abort_.load() ? 1 : 0;
}

void FfmpegDecoder::abort() {
    abort_.store(true);
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
    if (abort_.load()) {
        return false;
    }

    // av_log_set_level is process-wide. Decoder failures are still reported
    // with av_strerror, so libav's own info and warning lines stay quiet.
    av_log_set_level(AV_LOG_ERROR);

    AVDictionary* opts = nullptr;
    // file is only on the explicit --url local-file allowance. hls is the
    // protocol name libavformat registers; crypto covers AES-128 media.
    setDecoderIoOptions(&opts, allow_local_file_);
    av_dict_set(&opts, "format_whitelist", kFormatWhitelist, 0);
    av_dict_set(&opts, "codec_whitelist", kCodecWhitelist, 0);

    // libavformat initializes network libraries itself. Do not call
    // avformat_network_init(); it is optional and not recommended.
    // A context allocated here keeps interrupt_callback across open.
    // On failure FFmpeg frees that context and nulls the pointer.
    fmt_ = avformat_alloc_context();
    if (fmt_ == nullptr) {
        av_dict_free(&opts);
        return failOpen("avformat_alloc_context", AVERROR(ENOMEM));
    }
    fmt_->interrupt_callback.callback = &FfmpegDecoder::interruptCallback;
    fmt_->interrupt_callback.opaque = this;

    // AUDIT12 D4: a master with TYPE=AUDIO URI= is probed inside
    // avformat_open_input with find_stream_info(NULL). Open the first
    // video playlist instead so that nested decode never runs uncapped.
    std::string open_url = url_;
    preferFirstHlsVariantIfExternalAudio(&open_url, allow_local_file_, &fmt_->interrupt_callback);
    const int open_rc = avformat_open_input(&fmt_, open_url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (open_rc < 0) {
        fmt_ = nullptr;
        if (abort_.load()) {
            close();
            return false;
        }
        return failOpen("avformat_open_input", open_rc);
    }
    fmt_->interrupt_callback.callback = &FfmpegDecoder::interruptCallback;
    fmt_->interrupt_callback.opaque = this;

    fmt_->flags |= AVFMT_FLAG_GENPTS;
    if (fmt_->iformat == nullptr || fmt_->iformat->name == nullptr ||
        av_match_list(fmt_->iformat->name, kTopLevelFormatWhitelist, ',') <= 0) {
        const char* name = fmt_->iformat != nullptr ? fmt_->iformat->name : nullptr;
        std::cerr << "FfmpegDecoder::open failed for '" << url_
                  << "': demuxer is not allowed: " << (name != nullptr ? name : "unknown") << "\n";
        close();
        return false;
    }

    // Probe decoders use their own AVCodecContext. Discover streams that
    // MPEG-TS / HLS will add during the probe, then pass the same pixel cap
    // so find_stream_info does not materialize an oversize still
    // (AUDIT9 D1, AUDIT10 D2). FFmpeg 6.1.1 will not apply these options to
    // a stream that does not exist yet.
    discoverStreamsForProbeOptions(fmt_, &probe_pkts_, &probe_pkt_count_, &probe_pkt_pos_);
    AVDictionary** stream_opts = nullptr;
    const unsigned stream_count = fmt_->nb_streams;
    if (stream_count > 0) {
        stream_opts = static_cast<AVDictionary**>(av_calloc(stream_count, sizeof(*stream_opts)));
        if (stream_opts != nullptr) {
            for (unsigned i = 0; i < stream_count; ++i) {
                setPresentableMaxPixels(&stream_opts[i]);
            }
        }
    }
    const int info_rc = avformat_find_stream_info(fmt_, stream_opts);
    freeStreamOptions(stream_opts, stream_count);
    if (info_rc < 0) {
        if (abort_.load()) {
            close();
            return false;
        }
        return failOpen("avformat_find_stream_info", info_rc);
    }

    // First video in playlist / PMT order, not av_find_best_stream. The D2
    // pre-pass gives every HLS variant codec parameters, and
    // av_find_best_stream then prefers the higher-resolution one. Extra
    // PIDs born after the program stream also lose if "best" wins.
    video_index_ = firstVideoStreamIndex(fmt_);
    if (video_index_ < 0) {
        return failOpen("no video stream", video_index_);
    }

    // AVDISCARD_ALL on every other stream makes hls.c playlist_needed()
    // drop the rest, so later segments of those variants are not fetched.
    // Probe reads that already happened stay fetched.
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
    if (decoder->name == nullptr || av_match_list(decoder->name, kCodecWhitelist, ',') <= 0) {
        std::cerr << "FfmpegDecoder::open failed for '" << url_
                  << "': video codec is not allowed: "
                  << (decoder->name != nullptr ? decoder->name : "unknown") << "\n";
        close();
        return false;
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

    AVDictionary* codec_opts = nullptr;
    av_dict_set(&codec_opts, "codec_whitelist", kCodecWhitelist, 0);
    setPresentableMaxPixels(&codec_opts);
    codec_->max_pixels = presentableMaxPixels();
    const int codec_rc = avcodec_open2(codec_, decoder, &codec_opts);
    av_dict_free(&codec_opts);
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

void FfmpegDecoder::freeProbePackets() {
    freePacketQueue(&probe_pkts_, &probe_pkt_count_, &probe_pkt_pos_);
}

int FfmpegDecoder::readDemuxPacket() {
    if (probe_pkt_pos_ < probe_pkt_count_) {
        av_packet_unref(pkt_);
        const int ref_rc = av_packet_ref(pkt_, probe_pkts_[probe_pkt_pos_]);
        av_packet_free(&probe_pkts_[probe_pkt_pos_]);
        ++probe_pkt_pos_;
        if (probe_pkt_pos_ >= probe_pkt_count_) {
            freePacketQueue(&probe_pkts_, &probe_pkt_count_, &probe_pkt_pos_);
        }
        return ref_rc;
    }
    return av_read_frame(fmt_, pkt_);
}

void FfmpegDecoder::close() {
    // abort_ stays latched so a stopped decoder cannot open again.
    freeProbePackets();
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
    // Same caps as SoftwareRenderer::frameAcceptable, before the YUV420
    // buffer or sws_scale. A 12000x12000 still used to convert first.
    if (!decodedFramePresentable(src)) {
        std::cerr << "FfmpegDecoder::readFrame failed: decoded frame " << src->width << "x"
                  << src->height << " cannot be presented\n";
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
    if (abort_.load()) {
        return false;
    }
    if (!opened_) {
        std::cerr << "FfmpegDecoder::readFrame: decoder is not open\n";
        return false;
    }
    if (failed_ || decode_eof_) {
        return false;
    }

    int skipped_non_video = 0;
    while (true) {
        if (abort_.load()) {
            return false;
        }
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

        const int read_rc = readDemuxPacket();
        if (abort_.load()) {
            av_packet_unref(pkt_);
            return false;
        }
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
