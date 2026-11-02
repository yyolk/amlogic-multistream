#include "ffmpeg_decoder.h"

#include "software_renderer.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <vector>

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
#include <libavutil/opt.h>
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

bool urlLooksLikeMediaSegment(const std::string& url) {
    const std::string path = stripUrlQuery(url);
    static const char* const kExt[] = {".ts",  ".mp4", ".m4s",  ".m4v", ".mov",
                                       ".aac", ".ac3", ".eac3", ".mp3", ".gif"};
    for (const char* ext : kExt) {
        if (endsWithIgnoreCase(path, ext)) {
            return true;
        }
    }
    return false;
}

bool urlIsRemoteHttp(const std::string& url) {
    return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0 ||
           url.compare(0, 11, "hls+http://") == 0 || url.compare(0, 12, "hls+https://") == 0;
}

// avio_open2 speaks http(s); hls+ is only a demuxer scheme.
std::string avioPeekUrl(const std::string& url) {
    if (url.compare(0, 4, "hls+") == 0) {
        return url.substr(4);
    }
    return url;
}

// Local .ts / .mp4 skip the extra read (D2 still caps after open). http(s)
// always peeks: a .ts path may be mpegurl or a redirect to a master.
bool shouldPeekForHls(const std::string& url) {
    return urlIsRemoteHttp(url) || !urlLooksLikeMediaSegment(url);
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

// Same walk as FFmpeg 6.1.1 ff_parse_key_value: skip commas and
// whitespace, then strchr('=') from that point so a bare token
// swallows the next attribute (`NOTAKEY URI=` is one key). Quoted
// values may contain escapes, an unclosed quote takes the rest,
// duplicate exact keys last-win.
struct HlsMediaAttrs {
    std::string type;
    std::string uri;
    std::string method;
    bool unclosed_quote = false;
};

HlsMediaAttrs parseHlsMediaAttrs(const std::string& attrs) {
    HlsMediaAttrs out;
    size_t i = 0;
    if (i < attrs.size() && attrs[i] == ':') {
        ++i;
    }
    while (i < attrs.size()) {
        while (i < attrs.size() &&
               (attrs[i] == ',' || std::isspace(static_cast<unsigned char>(attrs[i])))) {
            ++i;
        }
        if (i >= attrs.size()) {
            break;
        }
        const size_t eq = attrs.find('=', i);
        if (eq == std::string::npos) {
            break;
        }
        const std::string key = attrs.substr(i, eq - i);
        i = eq + 1;
        std::string value;
        if (i < attrs.size() && attrs[i] == '"') {
            ++i;
            while (i < attrs.size() && attrs[i] != '"') {
                if (attrs[i] == '\\') {
                    if (i + 1 >= attrs.size()) {
                        break;
                    }
                    ++i;
                }
                value.push_back(attrs[i]);
                ++i;
            }
            if (i < attrs.size() && attrs[i] == '"') {
                ++i;
            } else {
                out.unclosed_quote = true;
            }
        } else {
            while (i < attrs.size() && attrs[i] != ',' &&
                   !std::isspace(static_cast<unsigned char>(attrs[i]))) {
                value.push_back(attrs[i]);
                ++i;
            }
        }
        if (key == "TYPE") {
            out.type = value;
        } else if (key == "URI") {
            out.uri = value;
        } else if (key == "METHOD") {
            out.method = value;
        }
    }
    return out;
}

// Same terminators as FFmpeg 6.1.1 ff_get_line: \n, \r, and \0. A \r\n
// pair is one break. The terminator is not part of the line. Stored
// length is capped at 4095 (ff_get_line(buf, MAX_URL_SIZE) with
// MAX_URL_SIZE == 4096); the rest of the line is still consumed.
constexpr size_t kFfGetLineMax = 4095;

bool hlsNextLine(const std::string& text, size_t* pos, std::string* line) {
    if (pos == nullptr || line == nullptr || *pos >= text.size()) {
        return false;
    }
    const size_t start = *pos;
    size_t i = start;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\n' || c == '\r' || c == '\0') {
            break;
        }
        ++i;
    }
    const size_t raw = i - start;
    *line = text.substr(start, raw > kFfGetLineMax ? kFfGetLineMax : raw);
    // ff_get_chomp_line strips trailing whitespace after the 4095 cap.
    while (!line->empty() && std::isspace(static_cast<unsigned char>(line->back()))) {
        line->pop_back();
    }
    if (i < text.size()) {
        const char c = text[i];
        ++i;
        if (c == '\r' && i < text.size() && text[i] == '\n') {
            ++i;
        }
    }
    *pos = i;
    return true;
}

bool hlsMasterHasExternalAudio(const std::string& text) {
    size_t pos = 0;
    std::string line;
    while (hlsNextLine(text, &pos, &line)) {
        if (line.compare(0, 13, "#EXT-X-MEDIA:") != 0) {
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const HlsMediaAttrs attrs = parseHlsMediaAttrs(line.substr(colon));
        // In-band TYPE=AUDIO (empty / missing URI) still makes hls.c run
        // find_stream_info(NULL) on the variant. Treat any AUDIO tag as
        // the same risk class as an external URI=.
        if (attrs.type == "AUDIO") {
            return true;
        }
    }
    return false;
}

bool hlsHasExtInf(const std::string& text) {
    size_t pos = 0;
    std::string line;
    while (hlsNextLine(text, &pos, &line)) {
        if (line.compare(0, 8, "#EXTINF:") == 0) {
            return true;
        }
    }
    return false;
}

bool hlsPlaylistHasEndlist(const std::string& text) {
    size_t pos = 0;
    std::string line;
    while (hlsNextLine(text, &pos, &line)) {
        if (trimCopy(line).compare(0, 14, "#EXT-X-ENDLIST") == 0) {
            return true;
        }
    }
    return false;
}

// FFmpeg 6.1.1 hls.c select_cur_seq_no: live_start_index defaults to -3,
// so a live playlist starts at max(n_segments - 3, 0).
size_t hlsLiveStartSegIndex(size_t n_segments) {
    if (n_segments <= 3) {
        return 0;
    }
    return n_segments - 3;
}

std::string firstHlsUriAfterTag(const std::string& text, const char* tag, size_t tag_len) {
    size_t pos = 0;
    std::string line;
    bool want_uri = false;
    while (hlsNextLine(text, &pos, &line)) {
        if (line.empty()) {
            continue;
        }
        if (line[0] == '#') {
            if (line.compare(0, tag_len, tag) == 0) {
                want_uri = true;
            }
            continue;
        }
        if (want_uri) {
            // ff_get_chomp_line only strips trailing whitespace. A
            // leading space is part of the URI FFmpeg fetches.
            return line;
        }
    }
    return {};
}

std::string firstHlsStreamInfUri(const std::string& text) {
    return firstHlsUriAfterTag(text, "#EXT-X-STREAM-INF:", 18);
}

std::vector<std::string> allHlsStreamInfUris(const std::string& text) {
    std::vector<std::string> out;
    size_t pos = 0;
    std::string line;
    bool want_uri = false;
    while (hlsNextLine(text, &pos, &line)) {
        if (line.empty()) {
            continue;
        }
        if (line[0] == '#') {
            if (line.compare(0, 18, "#EXT-X-STREAM-INF:") == 0) {
                want_uri = true;
            }
            continue;
        }
        if (want_uri) {
            out.push_back(line);
            want_uri = false;
        }
    }
    return out;
}

std::vector<std::string> allHlsOpenedMediaUris(const std::string& text) {
    std::vector<std::string> out;
    size_t pos = 0;
    std::string line;
    while (hlsNextLine(text, &pos, &line)) {
        if (line.compare(0, 13, "#EXT-X-MEDIA:") != 0) {
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const HlsMediaAttrs attrs = parseHlsMediaAttrs(line.substr(colon));
        if ((attrs.type == "VIDEO" || attrs.type == "AUDIO") && !attrs.uri.empty()) {
            out.push_back(attrs.uri);
        }
    }
    return out;
}

struct HlsSegmentRef {
    std::string uri;
    int64_t byte_offset = 0;
    int64_t byte_length = -1;
    bool encrypted = false;
};

bool parseHlsByteRange(const std::string& spec, int64_t* length, bool* has_offset, int64_t* offset) {
    if (length == nullptr || has_offset == nullptr || offset == nullptr) {
        return false;
    }
    const std::string s = trimCopy(spec);
    char* end = nullptr;
    const int64_t n = std::strtoll(s.c_str(), &end, 10);
    if (end == s.c_str() || n < 0) {
        return false;
    }
    *length = n;
    *has_offset = false;
    *offset = 0;
    if (end != nullptr && *end == '@') {
        char* off_end = nullptr;
        const int64_t o = std::strtoll(end + 1, &off_end, 10);
        if (off_end == end + 1 || o < 0) {
            return false;
        }
        *has_offset = true;
        *offset = o;
    }
    return true;
}

std::vector<HlsSegmentRef> hlsMediaSegments(const std::string& text) {
    std::vector<HlsSegmentRef> out;
    size_t pos = 0;
    std::string line;
    bool encrypted = false;
    bool want_uri = false;
    bool have_range = false;
    bool range_has_off = false;
    int64_t range_len = -1;
    int64_t range_off = 0;
    int64_t implicit_off = 0;
    while (hlsNextLine(text, &pos, &line)) {
        if (line.empty()) {
            continue;
        }
        if (line.compare(0, 11, "#EXT-X-KEY:") == 0) {
            const HlsMediaAttrs attrs = parseHlsMediaAttrs(line.substr(10));
            encrypted = !attrs.method.empty() && attrs.method != "NONE";
            continue;
        }
        if (line.compare(0, 17, "#EXT-X-BYTERANGE:") == 0) {
            have_range = parseHlsByteRange(line.substr(17), &range_len, &range_has_off, &range_off);
            continue;
        }
        if (line[0] == '#') {
            if (line.compare(0, 8, "#EXTINF:") == 0) {
                want_uri = true;
            }
            continue;
        }
        if (!want_uri) {
            continue;
        }
        HlsSegmentRef ref;
        ref.uri = line;
        ref.encrypted = encrypted;
        if (have_range) {
            ref.byte_length = range_len;
            ref.byte_offset = range_has_off ? range_off : implicit_off;
            implicit_off = ref.byte_offset + range_len;
            have_range = false;
        }
        out.push_back(ref);
        want_uri = false;
    }
    return out;
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

// Collapse . / .. like FFmpeg 6.1.1 ff_make_absolute_url2 append_path.
// Only http(s) URLs with an authority are simplified (file paths are not:
// the directory may be a symlink).
std::string normalizeUrlPath(const std::string& path) {
    std::vector<std::string> parts;
    const bool abs = !path.empty() && path[0] == '/';
    size_t i = abs ? 1 : 0;
    while (i <= path.size()) {
        const size_t j = path.find('/', i);
        const size_t end = j == std::string::npos ? path.size() : j;
        const std::string part = path.substr(i, end - i);
        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        if (j == std::string::npos) {
            break;
        }
        i = j + 1;
    }
    std::string out = abs ? "/" : "";
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k > 0) {
            out += '/';
        }
        out += parts[k];
    }
    if (!path.empty() && path.back() == '/' && (out.empty() || out.back() != '/')) {
        out += '/';
    }
    if (out.empty() && abs) {
        return "/";
    }
    return out;
}

std::string normalizeHttpUrl(const std::string& url) {
    std::string prefix;
    std::string u = url;
    if (u.compare(0, 4, "hls+") == 0) {
        prefix = "hls+";
        u = u.substr(4);
    }
    if (u.compare(0, 7, "http://") != 0 && u.compare(0, 8, "https://") != 0) {
        return url;
    }
    const auto scheme = u.find("://");
    if (scheme == std::string::npos) {
        return url;
    }
    const size_t path_start = u.find_first_of("/?#", scheme + 3);
    if (path_start == std::string::npos || u[path_start] != '/') {
        return url;
    }
    const size_t path_end = u.find_first_of("?#", path_start);
    const size_t end = path_end == std::string::npos ? u.size() : path_end;
    return prefix + u.substr(0, path_start) + normalizeUrlPath(u.substr(path_start, end - path_start)) +
           u.substr(end);
}

// Same join FFmpeg 6.1.1 ff_make_absolute_url uses for playlist refs:
// scheme-bearing and // refs win, a leading / replaces the path, else
// the ref is appended after the last slash of the base (query stripped).
// http(s) results then drop . / .. like ff_make_absolute_url2.
std::string resolvePlaylistUrl(const std::string& base, const std::string& ref) {
    // Keep leading whitespace: FFmpeg fetches " video.ts" as %20video.ts.
    std::string rel = ref;
    while (!rel.empty() && std::isspace(static_cast<unsigned char>(rel.back()))) {
        rel.pop_back();
    }
    if (rel.empty()) {
        return {};
    }
    std::string out;
    if (urlHasScheme(rel)) {
        out = rel;
    } else if (rel.compare(0, 2, "//") == 0) {
        const auto colon = base.find(':');
        if (colon != std::string::npos) {
            out = base.substr(0, colon + 1) + rel;
        } else {
            out = rel;
        }
    } else {
        std::string root = stripUrlQuery(base);
        if (!rel.empty() && rel[0] == '/') {
            const auto scheme = root.find("://");
            if (scheme == std::string::npos) {
                out = rel;
            } else {
                const auto path = root.find('/', scheme + 3);
                out = path == std::string::npos ? root + rel : root.substr(0, path) + rel;
            }
        } else {
            const auto slash = root.rfind('/');
            out = slash == std::string::npos ? rel : root.substr(0, slash + 1) + rel;
        }
    }
    return normalizeHttpUrl(out);
}

void stripUtf8Bom(std::string* text) {
    if (text != nullptr && text->size() >= 3 && static_cast<unsigned char>((*text)[0]) == 0xEF &&
        static_cast<unsigned char>((*text)[1]) == 0xBB &&
        static_cast<unsigned char>((*text)[2]) == 0xBF) {
        text->erase(0, 3);
    }
}

bool firstLineIsExtm3u(const std::string& text) {
    size_t pos = 0;
    std::string line;
    if (!hlsNextLine(text, &pos, &line)) {
        return false;
    }
    return trimCopy(line).compare(0, 7, "#EXTM3U") == 0;
}

std::string stripUrlFragment(const std::string& url) {
    const auto cut = url.find('#');
    return cut == std::string::npos ? url : url.substr(0, cut);
}

// FFmpeg fetches the exact playlist URL, including the query. A different
// query is a different playlist (master.m3u8?x=1 can be ID3 media).
bool samePlaylistUrl(const std::string& a, const std::string& b) {
    return stripUrlFragment(normalizeHttpUrl(a)) == stripUrlFragment(normalizeHttpUrl(b));
}

bool urlAlreadyVisited(const std::vector<std::string>& seen, const std::string& url) {
    for (const std::string& prev : seen) {
        if (samePlaylistUrl(prev, url)) {
            return true;
        }
    }
    return false;
}

std::string avioChildString(AVIOContext* pb, const char* name) {
    if (pb == nullptr || name == nullptr) {
        return {};
    }
    uint8_t* val = nullptr;
    if (av_opt_get(pb, name, AV_OPT_SEARCH_CHILDREN, &val) < 0 || val == nullptr) {
        return {};
    }
    std::string out(reinterpret_cast<char*>(val));
    av_free(val);
    return out;
}

bool mimeLooksMpegurl(const std::string& mime) {
    return av_stristr(mime.c_str(), "mpegurl") != nullptr;
}

bool bodyLooksLikeMpegTs(const std::string& body) {
    return body.size() >= 376 && static_cast<unsigned char>(body[0]) == 0x47 &&
           static_cast<unsigned char>(body[188]) == 0x47 &&
           static_cast<unsigned char>(body[376]) == 0x47;
}

bool bodyLooksLikeFtyp(const std::string& body) {
    return body.size() >= 8 && body.compare(4, 4, "ftyp") == 0;
}

bool bodyLooksLikeMediaSegment(const std::string& body) {
    return bodyLooksLikeMpegTs(body) || bodyLooksLikeFtyp(body);
}

bool bodyStartsWithId3At(const std::string& body, size_t off) {
    return off + 3 <= body.size() && body[off] == 'I' && body[off + 1] == 'D' && body[off + 2] == '3';
}

// FFmpeg 6.1.1 intercept_id3 / parse_id3 sets is_id3_timestamped when an
// ID3v2 PRIV owner is com.apple.streaming.transportStreamTimestamp (case
// insensitive) and the 8-byte payload is a 33-bit MPEG timestamp. Multiple
// leading ID3 tags are gathered; a compressed frame is inflated. If we
// cannot fully prove a segment is not timestamped, treat it as hostile.
constexpr const char kHlsId3TimestampOwner[] = "com.apple.streaming.transportStreamTimestamp";
constexpr size_t kHlsId3TimestampOwnerLen = 44;

uint32_t id3Synchsafe32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0] & 0x7f) << 21) | (static_cast<uint32_t>(p[1] & 0x7f) << 14) |
           (static_cast<uint32_t>(p[2] & 0x7f) << 7) | static_cast<uint32_t>(p[3] & 0x7f);
}

bool containsIgnoreCase(const char* hay, size_t hay_n, const char* needle, size_t needle_n) {
    if (hay == nullptr || needle == nullptr || hay_n < needle_n) {
        return false;
    }
    for (size_t i = 0; i + needle_n <= hay_n; ++i) {
        if (av_strncasecmp(hay + i, needle, needle_n) == 0) {
            return true;
        }
    }
    return false;
}

bool id3FrameCompressedOrEncrypted(int ver, unsigned char flags2) {
    // ID3v2.3 format flags are 0x80 compress / 0x40 encrypt. FFmpeg 6.1.1
    // still tests ID3v2_FLAG_COMPRESSION (0x0008) and ENCRYPTION (0x0004)
    // on the 16-bit v2.3 frame flags, so 0x08 + zlib is inflated. Treat
    // both encodings, and any other version, as uncertain.
    if (ver == 3 || ver == 4) {
        return (flags2 & 0x80) != 0 || (flags2 & 0x40) != 0 || (flags2 & 0x08) != 0 ||
               (flags2 & 0x04) != 0;
    }
    return true;
}

// True if FFmpeg might set is_id3_timestamped, or we cannot prove it will not.
bool bodyMayBeId3Timestamped(const std::string& body) {
    if (body.size() < 3) {
        return true;
    }
    if (!bodyStartsWithId3At(body, 0)) {
        return false;
    }
    const auto* p = reinterpret_cast<const unsigned char*>(body.data());
    size_t off = 0;
    while (bodyStartsWithId3At(body, off)) {
        if (off + 10 > body.size()) {
            return true;
        }
        const int ver = p[off + 3];
        const unsigned char tag_flags = p[off + 5];
        const uint32_t tag_size = id3Synchsafe32(p + off + 6);
        const size_t tag_end = off + 10 + static_cast<size_t>(tag_size);
        if (tag_end > body.size()) {
            return true;
        }
        // v2.3 0x40 is an extended header. The size field starts with
        // 0x00, which the frame walk treated as padding and skipped a
        // later zlib PRIV. Unsync / experimental flags are also
        // uncertain. Refuse any non-zero tag flag.
        if (ver >= 3 && tag_flags != 0) {
            return true;
        }
        if (containsIgnoreCase(body.data() + off, tag_end - off, kHlsId3TimestampOwner,
                               kHlsId3TimestampOwnerLen)) {
            return true;
        }
        if (ver < 2 || ver > 4) {
            return true;
        }
        const size_t frame_hdr = ver == 2 ? 6 : 10;
        size_t i = off + 10;
        while (i + frame_hdr <= tag_end) {
            if (body[i] == '\0') {
                break;
            }
            uint32_t frame_size = 0;
            unsigned char flags2 = 0;
            if (ver == 2) {
                frame_size = (static_cast<uint32_t>(p[i + 3]) << 16) |
                             (static_cast<uint32_t>(p[i + 4]) << 8) | static_cast<uint32_t>(p[i + 5]);
            } else if (ver == 4) {
                frame_size = id3Synchsafe32(p + i + 4);
                flags2 = p[i + 9];
            } else {
                frame_size = (static_cast<uint32_t>(p[i + 4]) << 24) | (static_cast<uint32_t>(p[i + 5]) << 16) |
                             (static_cast<uint32_t>(p[i + 6]) << 8) | static_cast<uint32_t>(p[i + 7]);
                flags2 = p[i + 9];
            }
            if (id3FrameCompressedOrEncrypted(ver, flags2)) {
                return true;
            }
            if (frame_size > tag_end - (i + frame_hdr)) {
                return true;
            }
            i += frame_hdr + frame_size;
        }
        off = tag_end;
    }
    return false;
}

struct PlaylistPreview {
    std::string body;
    std::string final_url;
    bool mpegurl = false;
    bool open_error = false;
    bool read_error = false;
    bool hit_max = false;
};

PlaylistPreview readHlsPlaylistPreview(const std::string& url, bool allow_local, AVIOInterruptCB* cb) {
    PlaylistPreview out;
    AVDictionary* opts = nullptr;
    setDecoderIoOptions(&opts, allow_local);
    AVIOContext* pb = nullptr;
    const std::string peek_url = avioPeekUrl(url);
    const int rc = avio_open2(&pb, peek_url.c_str(), AVIO_FLAG_READ, cb, &opts);
    av_dict_free(&opts);
    if (rc < 0 || pb == nullptr) {
        out.open_error = true;
        return out;
    }
    out.mpegurl = mimeLooksMpegurl(avioChildString(pb, "mime_type"));
    out.final_url = avioChildString(pb, "location");
    const bool remote = urlIsRemoteHttp(url);
    unsigned char buf[4096];
    constexpr size_t kSniff = 256;
    constexpr size_t kMax = 1024 * 1024;
    while (out.body.size() < kMax) {
        const int n = avio_read(pb, buf, sizeof(buf));
        if (n == 0 || n == AVERROR_EOF) {
            break;
        }
        if (n < 0) {
            out.read_error = true;
            break;
        }
        out.body.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
        if (out.body.size() >= kSniff) {
            std::string sniff = out.body;
            stripUtf8Bom(&sniff);
            if (firstLineIsExtm3u(sniff)) {
                continue;
            }
            // Local non-playlist can stop. A remote .ts/.mp4 can stop on a
            // real segment sniff (stall.ts drips). Extensionless remote
            // URLs keep reading: a finished stub then master must not
            // look like a successful peek.
            if (!remote || (urlLooksLikeMediaSegment(url) && bodyLooksLikeMediaSegment(sniff))) {
                break;
            }
        }
    }
    if (out.body.size() >= kMax) {
        out.hit_max = true;
    }
    avio_closep(&pb);
    stripUtf8Bom(&out.body);
    return out;
}

bool hlsPreviewIncomplete(const PlaylistPreview& prev) {
    if (!firstLineIsExtm3u(prev.body)) {
        return prev.mpegurl;
    }
    if (prev.open_error || prev.read_error || prev.hit_max) {
        return true;
    }
    return firstHlsStreamInfUri(prev.body).empty() && !hlsHasExtInf(prev.body);
}

PlaylistPreview readUrlRange(const std::string& url, bool allow_local, AVIOInterruptCB* cb,
                             int64_t offset, int64_t length) {
    PlaylistPreview out;
    AVDictionary* opts = nullptr;
    setDecoderIoOptions(&opts, allow_local);
    AVIOContext* pb = nullptr;
    const std::string peek_url = avioPeekUrl(url);
    const int rc = avio_open2(&pb, peek_url.c_str(), AVIO_FLAG_READ, cb, &opts);
    av_dict_free(&opts);
    if (rc < 0 || pb == nullptr) {
        out.open_error = true;
        return out;
    }
    if (offset > 0 && avio_seek(pb, offset, SEEK_SET) < 0) {
        out.open_error = true;
        avio_closep(&pb);
        return out;
    }
    unsigned char buf[4096];
    constexpr size_t kMax = 1024 * 1024;
    const size_t want = length > 0 ? static_cast<size_t>(std::min(length, static_cast<int64_t>(kMax))) : kMax;
    while (out.body.size() < want) {
        const int n = avio_read(pb, buf, sizeof(buf));
        if (n == 0 || n == AVERROR_EOF) {
            break;
        }
        if (n < 0) {
            out.read_error = true;
            break;
        }
        const size_t take = std::min(static_cast<size_t>(n), want - out.body.size());
        out.body.append(reinterpret_cast<const char*>(buf), take);
    }
    if (out.body.size() >= kMax) {
        out.hit_max = true;
    }
    avio_closep(&pb);
    return out;
}

bool segmentMayBeId3Timestamped(const HlsSegmentRef& seg, const std::string& base, bool allow_local,
                                AVIOInterruptCB* cb) {
    if (seg.encrypted || seg.uri.empty()) {
        return true;
    }
    const std::string resolved = resolvePlaylistUrl(base, seg.uri);
    if (resolved.empty()) {
        return true;
    }
    const int64_t length = seg.byte_length > 0 ? seg.byte_length : static_cast<int64_t>(1024 * 1024);
    const PlaylistPreview peek = readUrlRange(resolved, allow_local, cb, seg.byte_offset, length);
    if (peek.open_error) {
        return true;
    }
    return bodyMayBeId3Timestamped(peek.body);
}

bool playlistWouldNestedId3Probe(const PlaylistPreview& prev, const std::string& url, bool allow_local,
                                 AVIOInterruptCB* cb, int depth, std::vector<std::string>* seen) {
    if (depth > 8) {
        return true;
    }
    if (seen != nullptr && urlAlreadyVisited(*seen, url)) {
        return false;
    }
    if (seen != nullptr) {
        seen->push_back(url);
    }
    const std::string base = !prev.final_url.empty() ? prev.final_url : url;
    // Opening this playlist as a variant runs the nested probe if it
    // names TYPE=AUDIO. Depth 0 already rewrote those masters.
    if (hlsMasterHasExternalAudio(prev.body)) {
        return true;
    }
    const std::vector<HlsSegmentRef> segs = hlsMediaSegments(prev.body);
    // Depth 0 is the URL we would open: check every EXTINF (live ID3 can
    // sit on a later segment). VOD STREAM-INF variants only need the
    // first segment — fetching a later segment of the unused variant
    // breaks dual-variant selection. Live variants start at
    // live_start_index (-3), not segment 0. Two or more live siblings
    // are aligned with highest_cur_seq_no; check every live segment
    // rather than miss the bumped index.
    if (!segs.empty()) {
        if (depth == 0) {
            for (size_t i = 0; i < segs.size(); ++i) {
                if (segmentMayBeId3Timestamped(segs[i], base, allow_local, cb)) {
                    return true;
                }
            }
        } else if (hlsPlaylistHasEndlist(prev.body)) {
            if (segmentMayBeId3Timestamped(segs[0], base, allow_local, cb)) {
                return true;
            }
        } else if (segmentMayBeId3Timestamped(segs[hlsLiveStartSegIndex(segs.size())], base,
                                             allow_local, cb)) {
            return true;
        }
    }
    std::vector<std::string> variants = allHlsStreamInfUris(prev.body);
    const std::vector<std::string> media_uris = allHlsOpenedMediaUris(prev.body);
    variants.insert(variants.end(), media_uris.begin(), media_uris.end());

    struct InnerVar {
        std::string resolved;
        PlaylistPreview prev;
        bool live = false;
    };
    std::vector<InnerVar> inners;
    inners.reserve(variants.size());
    int live_siblings = 0;
    for (const std::string& rel : variants) {
        const std::string resolved = resolvePlaylistUrl(base, rel);
        if (resolved.empty() || samePlaylistUrl(resolved, url) ||
            (seen != nullptr && urlAlreadyVisited(*seen, resolved))) {
            continue;
        }
        const PlaylistPreview inner = readHlsPlaylistPreview(resolved, allow_local, cb);
        if (inner.open_error || inner.body.empty()) {
            return true;
        }
        if (bodyMayBeId3Timestamped(inner.body)) {
            return true;
        }
        InnerVar v;
        v.resolved = resolved;
        v.prev = inner;
        const std::vector<HlsSegmentRef> inner_segs = hlsMediaSegments(inner.body);
        v.live = !inner_segs.empty() && !hlsPlaylistHasEndlist(inner.body);
        if (v.live) {
            ++live_siblings;
        }
        inners.push_back(std::move(v));
    }
    for (InnerVar& v : inners) {
        if (v.live && live_siblings >= 2) {
            const std::vector<HlsSegmentRef> inner_segs = hlsMediaSegments(v.prev.body);
            // highest_cur_seq_no can bump a live sibling onto a later
            // segment (ID3 on the shorter playlist's index 2). Check
            // every live segment when more than one live variant exists.
            for (size_t i = 0; i < inner_segs.size(); ++i) {
                if (segmentMayBeId3Timestamped(inner_segs[i], !v.prev.final_url.empty() ? v.prev.final_url
                                                                                       : v.resolved,
                                             allow_local, cb)) {
                    return true;
                }
            }
        }
        if (playlistWouldNestedId3Probe(v.prev, v.resolved, allow_local, cb, depth + 1, seen)) {
            return true;
        }
    }
    return false;
}

bool playlistWouldNestedId3Probe(const PlaylistPreview& prev, const std::string& url, bool allow_local,
                                 AVIOInterruptCB* cb) {
    std::vector<std::string> seen;
    seen.reserve(9);
    return playlistWouldNestedId3Probe(prev, url, allow_local, cb, 0, &seen);
}

// FFmpeg 6.1.1 hls_read_header calls avformat_find_stream_info(pls->ctx, NULL)
// for an ID3-timestamped or TYPE=AUDIO rendition (AUDIT12 D4). There is no
// format-wide codec option, so that nested probe keeps max_pixels == INT_MAX.
// Follow STREAM-INF while the playlist names an external audio group. If the
// preview looks like a playlist but is truncated or has no safe variant,
// refuse rather than falling through to an uncapped open.
//
// Returns false if avformat_open_input must not run on the current URL.
// When a remote peek accepted a TS/`ftyp` body, forced_fmt pins the
// demuxer so a later GET of that URL cannot ride as HLS.
bool preferFirstHlsVariantIfExternalAudio(std::string* url, bool allow_local, AVIOInterruptCB* cb,
                                          const AVInputFormat** forced_fmt) {
    if (url == nullptr || url->empty()) {
        return true;
    }
    *url = normalizeHttpUrl(*url);
    constexpr int kMaxRewrites = 8;
    std::vector<std::string> seen;
    seen.reserve(static_cast<size_t>(kMaxRewrites) + 1);
    seen.push_back(*url);
    for (int depth = 0; depth < kMaxRewrites; ++depth) {
        if (!shouldPeekForHls(*url)) {
            return true;
        }
        const PlaylistPreview prev = readHlsPlaylistPreview(*url, allow_local, cb);
        if (hlsMasterHasExternalAudio(prev.body)) {
            const std::string rel = firstHlsStreamInfUri(prev.body);
            const std::string base = !prev.final_url.empty() ? prev.final_url : *url;
            const std::string resolved = resolvePlaylistUrl(base, rel);
            if (rel.empty() || resolved.empty() || urlAlreadyVisited(seen, resolved)) {
                return false;
            }
            *url = resolved;
            seen.push_back(resolved);
            continue;
        }
        // Nested master without TYPE=AUDIO on this playlist: if the
        // first STREAM-INF is itself an audio-group master, recurse
        // the same way as opening that master.
        if (firstLineIsExtm3u(prev.body)) {
            const std::string rel = firstHlsStreamInfUri(prev.body);
            if (!rel.empty()) {
                const std::string base = !prev.final_url.empty() ? prev.final_url : *url;
                const std::string resolved = resolvePlaylistUrl(base, rel);
                if (!resolved.empty() && !urlAlreadyVisited(seen, resolved)) {
                    const PlaylistPreview inner = readHlsPlaylistPreview(resolved, allow_local, cb);
                    if (!inner.open_error && hlsMasterHasExternalAudio(inner.body)) {
                        *url = resolved;
                        seen.push_back(resolved);
                        continue;
                    }
                }
            }
        }
        // Bytes then error on a non-segment URL (short read + RST, TLS
        // close, empty after error): the next open can still get a
        // master. A remote .ts/.mp4 stall or drip is not in that class.
        const bool remote_media = urlIsRemoteHttp(*url) && urlLooksLikeMediaSegment(*url) &&
                                  !firstLineIsExtm3u(prev.body) && !prev.mpegurl;
        if ((prev.read_error ||
             (urlIsRemoteHttp(*url) && (prev.open_error || prev.body.empty()))) &&
            !remote_media) {
            return false;
        }
        if (firstLineIsExtm3u(prev.body) || prev.mpegurl) {
            if (hlsPreviewIncomplete(prev)) {
                return false;
            }
            return !playlistWouldNestedId3Probe(prev, *url, allow_local, cb);
        }
        // Remote non-#EXTM3U: a finished TS/`ftyp` body on an
        // extensionless / playlist URL must not clear a later GET of
        // the same URL (that GET can be a master). A path that already
        // looks like a media segment can proceed; pin the demuxer so a
        // later playlist cannot ride the URL (hole 6).
        if (urlIsRemoteHttp(*url)) {
            if (!urlLooksLikeMediaSegment(*url)) {
                return false;
            }
            if (forced_fmt != nullptr) {
                if (bodyLooksLikeFtyp(prev.body) || endsWithIgnoreCase(stripUrlQuery(*url), ".mp4") ||
                    endsWithIgnoreCase(stripUrlQuery(*url), ".m4s") ||
                    endsWithIgnoreCase(stripUrlQuery(*url), ".m4v") ||
                    endsWithIgnoreCase(stripUrlQuery(*url), ".mov")) {
                    *forced_fmt = av_find_input_format("mp4");
                } else {
                    *forced_fmt = av_find_input_format("mpegts");
                }
            }
            return true;
        }
        return true;
    }
    return false;
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

    // AUDIT12 D4: a master with TYPE=AUDIO, or a media playlist whose
    // first segment is ID3-timestamped, is probed inside
    // avformat_open_input with find_stream_info(NULL). Follow STREAM-INF
    // to a playlist that does not name that group. Refuse when there is
    // no safe video playlist, or when the first segment would set
    // is_id3_timestamped, so the nested probe never runs uncapped.
    std::string open_url = url_;
    const AVInputFormat* forced_fmt = nullptr;
    if (!preferFirstHlsVariantIfExternalAudio(&open_url, allow_local_file_,
                                             &fmt_->interrupt_callback, &forced_fmt)) {
        av_dict_free(&opts);
        return failOpen("HLS playlist would run an uncapped nested probe", AVERROR(EINVAL));
    }
    const int open_rc = avformat_open_input(&fmt_, open_url.c_str(), forced_fmt, &opts);
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
