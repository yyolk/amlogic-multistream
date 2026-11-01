#include "ffmpeg_decoder.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Local software-decode smoke test. Generates a tiny fixture with the
// ffmpeg CLI and pulls one YUV420P frame through FfmpegDecoder.
// Exit 77 when the CLI is missing so ctest can mark the test skipped.
// FFmpeg dev libraries are required to link this binary; CMake omits the
// target when they are absent.
//
// The dual-variant case serves a master playlist on 127.0.0.1 from this
// process. It does not use an outside network. It fails if a later segment
// of the variant that was not selected is still requested.
//
// SURVIVING4 H1: an HLS master with a separate ADTS audio rendition. hls.c
// copies the format whitelist onto that rendition during open, before audio
// can be discarded. open() must succeed and a video frame must be presented.
//
// AUDIT4 A4: a standalone song.mp3 with an ID3 APIC cover must not open.
// Packed-audio names stay on the format_whitelist option only.

#ifndef FFMPEG_CLI
#define FFMPEG_CLI ""
#endif

namespace {

int g_failures = 0;
const char* g_case = "";

void check(bool cond, const char* expr, int line) {
    if (cond) {
        return;
    }
    std::cerr << "FAIL " << g_case << " " << expr << " (" << __FILE__ << ":" << line << ")\n";
    ++g_failures;
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

constexpr int kSkip = 77;

bool decoderTestsRequired() {
    const char* value = std::getenv("AMS_REQUIRE_DECODER_TESTS");
    return value != nullptr && std::string(value) == "1";
}
constexpr int kWidth = 160;
constexpr int kHeight = 120;

class TempDir {
public:
    TempDir()
        : path_(std::filesystem::temp_directory_path() /
                ("amlogic-ffmpeg-decoder-" + std::to_string(static_cast<long long>(::getpid())))) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Returns the child exit status, 127 if the executable cannot be run, or -1
// on a fork/wait failure. stderr is captured in err_file.
int run(const std::vector<std::string>& args, const std::filesystem::path& err_file) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        const int fd = ::open(err_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDERR_FILENO);
            if (fd != STDERR_FILENO) {
                ::close(fd);
            }
        }
        execvp(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (!WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

std::string ffmpegBinary() {
    const char* configured = FFMPEG_CLI;
    if (configured != nullptr && configured[0] != '\0') {
        return configured;
    }
    return "ffmpeg";
}

bool encoderMissing(const std::string& log) {
    const bool unknown = log.find("Unknown encoder") != std::string::npos;
    const bool not_found = log.find("not found") != std::string::npos;
    return unknown || (not_found && log.find("libx264") != std::string::npos);
}

void expectYuv420(const DecodedFrame& frame) {
    CHECK(frame.format == FramePixelFormat::Yuv420p);
    CHECK(frame.width == kWidth);
    CHECK(frame.height == kHeight);
    CHECK(frame.data[0] != nullptr);
    CHECK(frame.data[1] != nullptr);
    CHECK(frame.data[2] != nullptr);
    CHECK(frame.linesize[0] >= frame.width);
    const int chroma_w = (frame.width + 1) / 2;
    CHECK(frame.linesize[1] >= chroma_w);
    CHECK(frame.linesize[2] >= chroma_w);
    if (frame.data[0] == nullptr || frame.data[1] == nullptr || frame.data[2] == nullptr) {
        return;
    }
    // Touch the planes so a bad pointer fails here instead of looking valid.
    const volatile uint8_t y = frame.data[0][0];
    const volatile uint8_t u = frame.data[1][0];
    const volatile uint8_t v = frame.data[2][0];
    (void)y;
    (void)u;
    (void)v;
}

void testDecode(const std::string& url) {
    FfmpegDecoder decoder;
    CHECK(decoder.isHardware() == false);
    // Without the CLI allowance a local path must not reach the demuxer.
    CHECK(!decoder.open(url));
    decoder.setAllowLocalFile(true);

    const bool opened = decoder.open(url);
    CHECK(opened);
    CHECK(decoder.isHardware() == false);
    if (!opened) {
        decoder.close();
        return;
    }

    int got = 0;
    for (; got < 32; ++got) {
        DecodedFrame frame;
        if (!decoder.readFrame(frame)) {
            break;
        }
        expectYuv420(frame);
    }
    CHECK(got >= 1);
    CHECK(got < 32);

    decoder.close();
    decoder.close();

    CHECK(decoder.open(url));
    DecodedFrame again;
    CHECK(decoder.readFrame(again));
    expectYuv420(again);
    decoder.close();
}

// One-connection-at-a-time HTTP/1.1 server. Enough for libavformat's HLS
// client against files in root. Every request path is appended to log_path.
struct LocalHttp {
    pid_t pid = -1;
    int port = 0;

    LocalHttp() = default;
    LocalHttp(const LocalHttp&) = delete;
    LocalHttp& operator=(const LocalHttp&) = delete;
    LocalHttp(LocalHttp&& other) noexcept : pid(other.pid), port(other.port) {
        other.pid = -1;
        other.port = 0;
    }
    LocalHttp& operator=(LocalHttp&& other) noexcept {
        if (this != &other) {
            stop();
            pid = other.pid;
            port = other.port;
            other.pid = -1;
            other.port = 0;
        }
        return *this;
    }

    ~LocalHttp() { stop(); }

    void stop() {
        if (pid <= 0) {
            return;
        }
        kill(pid, SIGTERM);
        int status = 0;
        waitpid(pid, &status, 0);
        pid = -1;
    }
};

bool startsWith(const std::string& value, const char* prefix) {
    const std::string p(prefix);
    return value.size() >= p.size() && value.compare(0, p.size(), p) == 0;
}

void appendLog(const std::filesystem::path& log_path, const std::string& path) {
    const int fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return;
    }
    const std::string line = path + "\n";
    const ssize_t n = ::write(fd, line.data(), line.size());
    ::close(fd);
    (void)n;
}

std::string readRequest(int fd) {
    std::string data;
    char buf[2048];
    while (data.find("\r\n\r\n") == std::string::npos && data.size() < 65536) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, 2000);
        if (pr <= 0) {
            break;
        }
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        data.append(buf, static_cast<size_t>(n));
    }
    return data;
}

std::string requestPath(const std::string& headers) {
    const auto line_end = headers.find("\r\n");
    const std::string line = headers.substr(0, line_end == std::string::npos ? headers.size() : line_end);
    if (!startsWith(line, "GET ")) {
        return {};
    }
    const auto sp = line.find(' ', 4);
    if (sp == std::string::npos) {
        return {};
    }
    std::string url = line.substr(4, sp - 4);
    const auto query = url.find('?');
    if (query != std::string::npos) {
        url = url.substr(0, query);
    }
    const auto scheme = url.find("://");
    if (scheme != std::string::npos) {
        const auto slash = url.find('/', scheme + 3);
        url = slash == std::string::npos ? "/" : url.substr(slash);
    }
    return url;
}

bool safeRelative(const std::string& url, std::filesystem::path& rel) {
    if (url.empty() || url[0] != '/') {
        return false;
    }
    rel = std::filesystem::path(url.substr(1));
    for (const std::filesystem::path& part : rel) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

void sendAll(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        const ssize_t n = ::write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (n == 0) {
            return;
        }
        off += static_cast<size_t>(n);
    }
}

void sendResponse(int fd, int status, const std::string& body, const char* content_type) {
    const char* text = status == 200 ? "OK" : "Not Found";
    std::ostringstream hdr;
    hdr << "HTTP/1.1 " << status << " " << text
        << "\r\nContent-Length: " << body.size()
        << "\r\nContent-Type: " << content_type
        << "\r\nConnection: close\r\n\r\n";
    const std::string header = hdr.str();
    sendAll(fd, header.data(), header.size());
    if (!body.empty()) {
        sendAll(fd, body.data(), body.size());
    }
}

[[noreturn]] void serveLoop(int listen_fd,
                            const std::filesystem::path& root,
                            const std::filesystem::path& log_path) {
    ::signal(SIGPIPE, SIG_IGN);
    while (true) {
        const int client = ::accept(listen_fd, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            _exit(0);
        }
        const std::string headers = readRequest(client);
        const std::string url = requestPath(headers);
        if (!url.empty()) {
            appendLog(log_path, url);
        }
        std::filesystem::path rel;
        std::string body;
        bool ok = !url.empty() && safeRelative(url, rel);
        if (ok) {
            const std::filesystem::path full = root / rel;
            std::ifstream in(full, std::ios::binary);
            if (!in) {
                ok = false;
            } else {
                std::ostringstream ss;
                ss << in.rdbuf();
                body = ss.str();
            }
        }
        if (!ok) {
            sendResponse(client, 404, "missing\n", "text/plain");
        } else {
            const char* type = "application/octet-stream";
            if (rel.extension() == ".m3u8") {
                type = "application/vnd.apple.mpegurl";
            }
            sendResponse(client, 200, body, type);
        }
        ::close(client);
    }
}

LocalHttp startLocalHttp(const std::filesystem::path& root, const std::filesystem::path& log_path) {
    LocalHttp server;
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return server;
    }
    const int yes = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listen_fd, 16) != 0) {
        ::close(listen_fd);
        return server;
    }
    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0) {
        ::close(listen_fd);
        return server;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        ::close(listen_fd);
        return server;
    }
    if (pid == 0) {
        serveLoop(listen_fd, root, log_path);
    }
    ::close(listen_fd);
    server.pid = pid;
    server.port = ntohs(bound.sin_port);
    return server;
}

bool parseSegmentIndex(const std::string& name, int& index) {
    const std::string prefix = "index";
    const std::string suffix = ".ts";
    if (name.size() <= prefix.size() + suffix.size()) {
        return false;
    }
    if (name.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return false;
    }
    const std::string num = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    if (num.empty()) {
        return false;
    }
    for (char c : num) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    index = std::stoi(num);
    return true;
}

int maxSegmentIndex(const std::filesystem::path& dir) {
    int max_index = -1;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        int index = -1;
        if (!parseSegmentIndex(entry.path().filename().string(), index)) {
            continue;
        }
        if (index > max_index) {
            max_index = index;
        }
    }
    return max_index;
}

void writeMaster(const std::filesystem::path& path, bool low_first) {
    std::ofstream out(path);
    out << "#EXTM3U\n";
    const char* low = "#EXT-X-STREAM-INF:BANDWIDTH=400000,RESOLUTION=320x240\nlo/index.m3u8\n";
    const char* high = "#EXT-X-STREAM-INF:BANDWIDTH=1500000,RESOLUTION=640x480\nhi/index.m3u8\n";
    if (low_first) {
        out << low << high;
    } else {
        out << high << low;
    }
}

// First listed variant is the one open() keeps. The other variant's last
// segment must not be requested. Probe traffic during avformat_find_stream_info
// still fetches the early segments of both; the fixture is long enough that
// the last segment is past that probe.
void testMasterOrder(LocalHttp& server,
                     const std::filesystem::path& root,
                     const std::filesystem::path& log_path,
                     bool low_first,
                     int last_index) {
    const char* master_name = low_first ? "master-low-first.m3u8" : "master-high-first.m3u8";
    writeMaster(root / master_name, low_first);
    std::ofstream trunc(log_path, std::ios::trunc);
    trunc.close();

    const int expect_w = low_first ? 320 : 640;
    const int expect_h = low_first ? 240 : 480;
    const std::string chosen = low_first ? "lo" : "hi";
    const std::string other = low_first ? "hi" : "lo";
    const std::string chosen_last = "/" + chosen + "/index" + std::to_string(last_index) + ".ts";
    const std::string other_last = "/" + other + "/index" + std::to_string(last_index) + ".ts";
    const std::string url =
        "http://127.0.0.1:" + std::to_string(server.port) + "/" + master_name;

    g_case = low_first ? "hls master low first" : "hls master high first";
    FfmpegDecoder decoder;
    const bool opened = decoder.open(url);
    CHECK(opened);
    if (!opened) {
        std::cerr << readFile(log_path);
        return;
    }

    int got = 0;
    int width = 0;
    int height = 0;
    for (; got < 400; ++got) {
        DecodedFrame frame;
        if (!decoder.readFrame(frame)) {
            break;
        }
        if (got == 0) {
            width = frame.width;
            height = frame.height;
        }
        CHECK(frame.format == FramePixelFormat::Yuv420p);
    }
    decoder.close();

    const std::string log = readFile(log_path);
    if (got < 1 || width != expect_w || height != expect_h || log.find(chosen_last) == std::string::npos ||
        log.find(other_last) != std::string::npos || log.find("/lo/index0.ts") == std::string::npos ||
        log.find("/hi/index0.ts") == std::string::npos) {
        std::cerr << "GET log for " << url << ":\n" << log;
    }
    CHECK(got >= 1);
    CHECK(width == expect_w);
    CHECK(height == expect_h);
    CHECK(log.find("/lo/index0.ts") != std::string::npos);
    CHECK(log.find("/hi/index0.ts") != std::string::npos);
    CHECK(log.find(chosen_last) != std::string::npos);
    CHECK(log.find(other_last) == std::string::npos);
}

void testRejectGif(const std::string& ffmpeg, const std::filesystem::path& dir) {
    g_case = "generate gif";
    const std::filesystem::path gif = dir / "clip.gif";
    const std::filesystem::path log = dir / "gif-ffmpeg.log";
    const int gif_rc = run(
        {ffmpeg, "-y", "-loglevel", "error", "-f", "lavfi",
         "-i", "testsrc=size=16x16:rate=1:duration=0.2", gif.string()},
        log);
    if (gif_rc != 0) {
        std::cerr << "failed to generate GIF fixture (exit " << gif_rc << ")\n" << readFile(log);
        ++g_failures;
        return;
    }

    g_case = "gif local";
    FfmpegDecoder local;
    local.setAllowLocalFile(true);
    CHECK(!local.open(gif.string()));
    local.close();

    g_case = "gif http";
    const std::filesystem::path get_log = dir / "gif-gets.log";
    LocalHttp server = startLocalHttp(dir, get_log);
    CHECK(server.pid > 0);
    CHECK(server.port > 0);
    if (server.pid <= 0 || server.port <= 0) {
        return;
    }
    const std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/clip.gif";
    FfmpegDecoder http;
    const bool opened = http.open(url);
    CHECK(!opened);
    http.close();
    server.stop();
}

// ID3v2.3 MP3 with a JPEG APIC cover. The mp3 demuxer is on format_whitelist
// so an HLS packed-audio rendition can open, and it is not accepted as the
// top-level format. Before that split, open() selected the cover as mjpeg.
void testRejectCoverMp3(const std::string& ffmpeg, const std::filesystem::path& dir) {
    g_case = "generate song.mp3";
    const std::filesystem::path mp3 = dir / "song.mp3";
    const std::filesystem::path log = dir / "song-mp3-ffmpeg.log";
    const int mp3_rc = run(
        {ffmpeg, "-y", "-loglevel", "error", "-f", "lavfi",
         "-i", "sine=frequency=440:sample_rate=44100:duration=0.4",
         "-f", "lavfi", "-i", "color=c=red:s=16x16:d=0.04,format=rgb24",
         "-map", "0:a", "-map", "1:v",
         "-c:a", "libmp3lame", "-b:a", "64k", "-c:v", "mjpeg",
         "-id3v2_version", "3",
         "-metadata:s:v", "title=Album cover",
         "-metadata:s:v", "comment=Cover (front)",
         "-disposition:v:0", "attached_pic",
         mp3.string()},
        log);
    if (mp3_rc != 0) {
        std::cerr << "failed to generate song.mp3 fixture (exit " << mp3_rc << ")\n"
                  << readFile(log);
        ++g_failures;
        return;
    }

    {
        std::ifstream in(mp3, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const bool id3 = bytes.size() >= 3 && bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3';
        const bool apic = bytes.find("APIC") != std::string::npos;
        const bool jpeg = bytes.find(std::string("\xFF\xD8", 2)) != std::string::npos;
        if (!in || !id3 || !apic || !jpeg) {
            std::cerr << "song.mp3 fixture is missing an ID3 APIC JPEG cover\n";
            ++g_failures;
            return;
        }
    }

    g_case = "song.mp3 local";
    FfmpegDecoder local;
    local.setAllowLocalFile(true);
    CHECK(!local.open(mp3.string()));
    local.close();

    g_case = "song.mp3 http";
    const std::filesystem::path get_log = dir / "song-mp3-gets.log";
    LocalHttp server = startLocalHttp(dir, get_log);
    CHECK(server.pid > 0);
    CHECK(server.port > 0);
    if (server.pid <= 0 || server.port <= 0) {
        return;
    }
    const std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/song.mp3";
    FfmpegDecoder http;
    CHECK(!http.open(url));
    http.close();
    server.stop();
}

// Master playlist plus a raw-ADTS audio rendition. The video playlist is
// MPEG-TS. The audio playlist's segment probes as format "aac", which hls.c
// opens under the copied format whitelist.
void testAdtsAudioMaster(const std::string& ffmpeg, const std::filesystem::path& dir) {
    g_case = "generate adts hls master";
    const std::filesystem::path root = dir / "adts-master";
    const std::filesystem::path video_dir = root / "video";
    std::filesystem::create_directories(video_dir);
    const std::filesystem::path log = root / "ffmpeg.log";

    const int video_rc = run(
        {ffmpeg, "-y", "-loglevel", "error", "-f", "lavfi",
         "-i", "testsrc=size=160x120:rate=10:duration=0.4",
         "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-an",
         "-hls_time", "1", "-hls_list_size", "0", "-hls_playlist_type", "vod",
         "-hls_segment_filename", (video_dir / "index%d.ts").string(),
         (video_dir / "index.m3u8").string()},
        log);
    if (video_rc != 0) {
        std::cerr << "failed to generate ADTS-master video (exit " << video_rc << ")\n"
                  << readFile(log);
        ++g_failures;
        return;
    }

    const int audio_rc = run(
        {ffmpeg, "-y", "-loglevel", "error", "-f", "lavfi",
         "-i", "sine=frequency=440:sample_rate=44100:duration=0.4",
         "-c:a", "aac", "-f", "adts", (root / "audio0.aac").string()},
        log);
    if (audio_rc != 0) {
        std::cerr << "failed to generate ADTS audio (exit " << audio_rc << ")\n"
                  << readFile(log);
        ++g_failures;
        return;
    }
    // A .aac name whose payload is MPEG-TS stays on the old whitelist. The
    // rendition has to probe as ADTS (sync word 0xFFF) or this case is not H1.
    {
        std::ifstream adts(root / "audio0.aac", std::ios::binary);
        unsigned char hdr[2] = {};
        adts.read(reinterpret_cast<char*>(hdr), 2);
        if (!adts || hdr[0] != 0xFF || (hdr[1] & 0xF0) != 0xF0) {
            std::cerr << "ADTS fixture does not start with the ADTS sync word\n";
            ++g_failures;
            return;
        }
    }

    {
        std::ofstream audio(root / "audio.m3u8");
        audio << "#EXTM3U\n"
              << "#EXT-X-VERSION:3\n"
              << "#EXT-X-TARGETDURATION:1\n"
              << "#EXT-X-MEDIA-SEQUENCE:0\n"
              << "#EXTINF:0.400,\n"
              << "audio0.aac\n"
              << "#EXT-X-ENDLIST\n";
        std::ofstream master(root / "master.m3u8");
        master << "#EXTM3U\n"
               << "#EXT-X-VERSION:4\n"
               << "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",DEFAULT=YES,"
                  "AUTOSELECT=YES,URI=\"audio.m3u8\"\n"
               << "#EXT-X-STREAM-INF:BANDWIDTH=400000,CODECS=\"avc1.42e01e,mp4a.40.2\","
                  "AUDIO=\"aud\"\n"
               << "video/index.m3u8\n";
    }

    g_case = "hls adts audio master";
    FfmpegDecoder decoder;
    decoder.setAllowLocalFile(true);
    const std::string url = (root / "master.m3u8").string();
    const bool opened = decoder.open(url);
    CHECK(opened);
    if (!opened) {
        decoder.close();
        return;
    }

    int got = 0;
    for (; got < 32; ++got) {
        DecodedFrame frame;
        if (!decoder.readFrame(frame)) {
            break;
        }
        expectYuv420(frame);
    }
    CHECK(got >= 1);
    decoder.close();
}

void testExtentCap(const std::string& ffmpeg, const std::filesystem::path& dir) {
    const std::filesystem::path log = dir / "extent-ffmpeg.log";

    g_case = "generate 8224x32";
    const std::filesystem::path wide = dir / "wide-8224.mp4";
    const int wide_rc = run(
        {ffmpeg, "-y", "-loglevel", "error", "-f", "lavfi",
         "-i", "testsrc=size=8224x32:rate=10:duration=0.4",
         "-c:v", "libx264", "-pix_fmt", "yuv420p", wide.string()},
        log);
    if (wide_rc != 0) {
        if (encoderMissing(readFile(log))) {
            std::cout << "extent cap fixture skipped: libx264 encoder not available\n";
            return;
        }
        std::cerr << "failed to generate 8224x32 fixture (exit " << wide_rc << ")\n"
                  << readFile(log);
        ++g_failures;
        return;
    }

    g_case = "8224x32 reject before convert";
    {
        FfmpegDecoder decoder;
        decoder.setAllowLocalFile(true);
        const bool opened = decoder.open(wide.string());
        CHECK(opened);
        if (opened) {
            DecodedFrame frame;
            CHECK(!decoder.readFrame(frame));
            CHECK(frame.width == 0);
            CHECK(frame.height == 0);
        }
        decoder.close();
    }

    g_case = "generate 8192x32";
    const std::filesystem::path edge = dir / "edge-8192.mp4";
    const int edge_rc = run(
        {ffmpeg, "-y", "-loglevel", "error", "-f", "lavfi",
         "-i", "testsrc=size=8192x32:rate=10:duration=0.4",
         "-c:v", "libx264", "-pix_fmt", "yuv420p", edge.string()},
        log);
    if (edge_rc != 0) {
        std::cerr << "failed to generate 8192x32 fixture (exit " << edge_rc << ")\n"
                  << readFile(log);
        ++g_failures;
        return;
    }

    g_case = "8192x32 presentable";
    {
        FfmpegDecoder decoder;
        decoder.setAllowLocalFile(true);
        const bool opened = decoder.open(edge.string());
        CHECK(opened);
        if (opened) {
            DecodedFrame frame;
            CHECK(decoder.readFrame(frame));
            CHECK(frame.format == FramePixelFormat::Yuv420p);
            CHECK(frame.width == 8192);
            CHECK(frame.height == 32);
        }
        decoder.close();
    }

    g_case = "generate 12000x12000 mjpeg";
    const std::filesystem::path huge = dir / "huge-12000.mp4";
    const int huge_rc = run(
        {ffmpeg, "-y", "-loglevel", "error", "-f", "lavfi",
         "-i", "color=c=red:s=12000x12000:d=0.04", "-frames:v", "1",
         "-c:v", "mjpeg", "-q:v", "31", huge.string()},
        log);
    if (huge_rc != 0) {
        std::cerr << "failed to generate 12000x12000 fixture (exit " << huge_rc << ")\n"
                  << readFile(log);
        ++g_failures;
        return;
    }

    g_case = "12000x12000 reject";
    {
        FfmpegDecoder decoder;
        decoder.setAllowLocalFile(true);
        DecodedFrame frame;
        const bool opened = decoder.open(huge.string());
        const bool got = opened && decoder.readFrame(frame);
        CHECK(!got);
        CHECK(frame.width == 0);
        CHECK(frame.height == 0);
        decoder.close();
    }
}

// Drop PAT/PMT/SI so MPEG-TS streams appear only during demux (AUDIT10 D2).
bool writePsiLessTs(const std::filesystem::path& src, const std::filesystem::path& dst) {
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dst, std::ios::binary);
    char pkt[188];
    bool kept = false;
    while (in.read(pkt, 188) && in.gcount() == 188) {
        if (static_cast<unsigned char>(pkt[0]) != 0x47) {
            return false;
        }
        const int pid = ((static_cast<unsigned char>(pkt[1]) & 0x1f) << 8) |
                        static_cast<unsigned char>(pkt[2]);
        if (pid >= 0x20 && pid != 0x1000) {
            out.write(pkt, 188);
            kept = true;
        }
    }
    return kept && out.good();
}

// Keep the PMT program from `pmt`, then append `extra`'s video PID remapped
// to 0x101 so it is born during demux (AUDIT10 D2).
bool writeExtraPidTs(const std::filesystem::path& pmt,
                     const std::filesystem::path& extra,
                     const std::filesystem::path& dst) {
    std::ifstream base(pmt, std::ios::binary);
    std::ifstream more(extra, std::ios::binary);
    std::ofstream out(dst, std::ios::binary);
    out << base.rdbuf();
    char pkt[188];
    bool kept = false;
    while (more.read(pkt, 188) && more.gcount() == 188) {
        if (static_cast<unsigned char>(pkt[0]) != 0x47) {
            return false;
        }
        const int pid = ((static_cast<unsigned char>(pkt[1]) & 0x1f) << 8) |
                        static_cast<unsigned char>(pkt[2]);
        if (pid == 0x100) {
            pkt[1] = static_cast<char>((pkt[1] & 0xE0) | 0x01);
            pkt[2] = 0x01;
            out.write(pkt, 188);
            kept = true;
        }
    }
    return kept && out.good();
}

void writeMediaPlaylist(const std::filesystem::path& playlist, const std::string& segment) {
    std::ofstream out(playlist);
    out << "#EXTM3U\n"
        << "#EXT-X-VERSION:3\n"
        << "#EXT-X-TARGETDURATION:1\n"
        << "#EXT-X-MEDIA-SEQUENCE:0\n"
        << "#EXTINF:0.400,\n"
        << segment << "\n"
        << "#EXT-X-ENDLIST\n";
}

void testLateStreamMaxPixels(const std::string& ffmpeg, const std::filesystem::path& dir) {
    const std::filesystem::path log = dir / "late-stream-ffmpeg.log";

    g_case = "generate small psi-less ts";
    const std::filesystem::path smallPmt = dir / "small-pmt.ts";
    const std::filesystem::path smallPsi = dir / "small-psiless.ts";
    const int small_rc = run(
        {ffmpeg, "-y", "-loglevel", "error", "-f", "lavfi",
         "-i", "testsrc=size=160x120:rate=10:duration=0.4",
         "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
         "-f", "mpegts", smallPmt.string()},
        log);
    if (small_rc != 0) {
        if (encoderMissing(readFile(log))) {
            std::cout << "late-stream fixture skipped: libx264 encoder not available\n";
            return;
        }
        std::cerr << "failed to generate small mpegts fixture (exit " << small_rc << ")\n"
                  << readFile(log);
        ++g_failures;
        return;
    }
    if (!writePsiLessTs(smallPmt, smallPsi)) {
        std::cerr << "failed to strip PSI from small mpegts fixture\n";
        ++g_failures;
        return;
    }

    g_case = "small psi-less ts presentable";
    testDecode(smallPsi.string());

    g_case = "small psi-less hls presentable";
    const std::filesystem::path smallHls = dir / "small-psiless.m3u8";
    writeMediaPlaylist(smallHls, smallPsi.filename().string());
    testDecode(smallHls.string());

    g_case = "generate 12000x12000 psi-less ts";
    const std::filesystem::path hugePmt = dir / "huge-pmt.ts";
    const std::filesystem::path hugePsi = dir / "huge-psiless.ts";
    const int huge_rc = run(
        {ffmpeg, "-y", "-loglevel", "error", "-f", "lavfi",
         "-i", "color=c=red:s=12000x12000:d=0.04", "-frames:v", "1",
         "-c:v", "libx264", "-preset", "ultrafast", "-tune", "stillimage",
         "-crf", "51", "-pix_fmt", "yuv420p", "-f", "mpegts", hugePmt.string()},
        log);
    if (huge_rc != 0) {
        std::cerr << "failed to generate 12000x12000 mpegts fixture (exit " << huge_rc << ")\n"
                  << readFile(log);
        ++g_failures;
        return;
    }
    if (!writePsiLessTs(hugePmt, hugePsi)) {
        std::cerr << "failed to strip PSI from 12000x12000 mpegts fixture\n";
        ++g_failures;
        return;
    }

    g_case = "12000x12000 psi-less reject";
    {
        FfmpegDecoder decoder;
        decoder.setAllowLocalFile(true);
        DecodedFrame frame;
        const bool opened = decoder.open(hugePsi.string());
        const bool got = opened && decoder.readFrame(frame);
        CHECK(!got);
        CHECK(frame.width == 0);
        CHECK(frame.height == 0);
        decoder.close();
    }

    g_case = "generate extra-pid ts";
    const std::filesystem::path extraPid = dir / "extra-pid.ts";
    if (!writeExtraPidTs(smallPmt, hugePmt, extraPid)) {
        std::cerr << "failed to build extra-PID mpegts fixture\n";
        ++g_failures;
        return;
    }

    g_case = "extra-pid ts presents small stream";
    testDecode(extraPid.string());

    g_case = "extra-pid hls presents small stream";
    const std::filesystem::path extraHls = dir / "extra-pid.m3u8";
    writeMediaPlaylist(extraHls, extraPid.filename().string());
    testDecode(extraHls.string());
}

void testMissingFile(const std::filesystem::path& dir) {
    g_case = "missing file";
    FfmpegDecoder decoder;
    decoder.setAllowLocalFile(true);
    const std::string url = (dir / "no-such-file.mp4").string();
    CHECK(!decoder.open(url));
    CHECK(decoder.isHardware() == false);
    decoder.close();
    decoder.close();
}

// AUDIT7 B2: an HTTP playlist whose only segment is file:///… must not
// decode unless the caller set the local-file allowance. MQTT add and an
// http(s) --url leave that flag off. The allowance-on case proves the
// fixture is a real file segment, not a broken playlist.
void testHttpPlaylistFileSegment(const std::filesystem::path& mp4, const std::filesystem::path& dir) {
    g_case = "http playlist file:// segment";
    const std::filesystem::path playlist = dir / "file_seg.m3u8";
    {
        std::ofstream out(playlist);
        out << "#EXTM3U\n"
            << "#EXT-X-VERSION:3\n"
            << "#EXT-X-TARGETDURATION:1\n"
            << "#EXT-X-MEDIA-SEQUENCE:0\n"
            << "#EXTINF:0.400,\n"
            << "file://" << mp4.string() << "\n"
            << "#EXT-X-ENDLIST\n";
    }

    const std::filesystem::path get_log = dir / "file-seg-gets.log";
    LocalHttp server = startLocalHttp(dir, get_log);
    CHECK(server.pid > 0);
    CHECK(server.port > 0);
    if (server.pid <= 0 || server.port <= 0) {
        return;
    }
    const std::string hostPath = "127.0.0.1:" + std::to_string(server.port) + "/file_seg.m3u8";
    const std::string url = "http://" + hostPath;
    const std::string hlsUrl = "hls+http://" + hostPath;

    FfmpegDecoder remote;
    CHECK(!remote.open(url));
    remote.close();

    g_case = "hls+http playlist file:// segment";
    FfmpegDecoder nested;
    CHECK(!nested.open(hlsUrl));
    nested.close();
    g_case = "http playlist file:// segment";

    FfmpegDecoder local;
    local.setAllowLocalFile(true);
    const bool opened = local.open(url);
    CHECK(opened);
    if (opened) {
        DecodedFrame frame;
        CHECK(local.readFrame(frame));
        expectYuv420(frame);
    }
    local.close();
    server.stop();
}

}  // namespace

int main() {
    try {
        const std::string ffmpeg = ffmpegBinary();
        TempDir tmp;
        const std::filesystem::path log = tmp.path() / "ffmpeg.log";
        const std::filesystem::path mp4 = tmp.path() / "clip.mp4";

        g_case = "generate mp4";
        const int mp4_rc = run(
            {ffmpeg, "-y", "-loglevel", "error", "-f", "lavfi",
             "-i", "testsrc=size=160x120:rate=10:duration=0.4",
             "-c:v", "mpeg4", "-pix_fmt", "yuv420p", mp4.string()},
            log);
        if (mp4_rc == 127) {
            std::cout << "SKIP ffmpeg CLI not found\n";
            return kSkip;
        }
        if (mp4_rc != 0) {
            std::cerr << "failed to generate MP4 fixture (exit " << mp4_rc << ")\n"
                      << readFile(log);
            return 1;
        }

        g_case = "mp4";
        testDecode(mp4.string());
        testMissingFile(tmp.path());
        testRejectGif(ffmpeg, tmp.path());
        testRejectCoverMp3(ffmpeg, tmp.path());
        testHttpPlaylistFileSegment(mp4, tmp.path());
        testExtentCap(ffmpeg, tmp.path());
        testLateStreamMaxPixels(ffmpeg, tmp.path());

        g_case = "generate hls";
        const std::filesystem::path hls_dir = tmp.path() / "hls";
        std::filesystem::create_directories(hls_dir);
        const std::filesystem::path playlist = hls_dir / "index.m3u8";
        const int hls_rc = run(
            {ffmpeg, "-y", "-loglevel", "error", "-f", "lavfi",
             "-i", "testsrc=size=160x120:rate=10:duration=0.4",
             "-c:v", "libx264", "-pix_fmt", "yuv420p",
             "-hls_time", "1", "-hls_list_size", "0", playlist.string()},
            log);
        bool hls_decoded = false;
        if (hls_rc == 0) {
            g_case = "hls";
            testDecode(playlist.string());
            hls_decoded = true;
            testAdtsAudioMaster(ffmpeg, tmp.path());

            g_case = "generate hls master";
            const std::filesystem::path master_root = tmp.path() / "master";
            const std::filesystem::path lo_dir = master_root / "lo";
            const std::filesystem::path hi_dir = master_root / "hi";
            std::filesystem::create_directories(lo_dir);
            std::filesystem::create_directories(hi_dir);
            // Eight seconds, one-second segments. find_stream_info probes the
            // start of both variants; the last segment is past that probe, so
            // a discard miss still fetches it and this test fails.
            const int lo_enc = run(
                {ffmpeg, "-y", "-loglevel", "error", "-f", "lavfi",
                 "-i", "testsrc=size=320x240:rate=10:duration=8",
                 "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                 "-g", "10", "-hls_time", "1", "-hls_list_size", "0",
                 "-hls_playlist_type", "vod",
                 "-hls_segment_filename", (lo_dir / "index%d.ts").string(),
                 (lo_dir / "index.m3u8").string()},
                log);
            const int hi_enc = run(
                {ffmpeg, "-y", "-loglevel", "error", "-f", "lavfi",
                 "-i", "testsrc=size=640x480:rate=10:duration=8",
                 "-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                 "-g", "10", "-hls_time", "1", "-hls_list_size", "0",
                 "-hls_playlist_type", "vod",
                 "-hls_segment_filename", (hi_dir / "index%d.ts").string(),
                 (hi_dir / "index.m3u8").string()},
                log);
            if (lo_enc != 0 || hi_enc != 0) {
                std::cerr << "failed to generate dual-variant HLS fixture (lo " << lo_enc
                          << " hi " << hi_enc << ")\n"
                          << readFile(log);
                return 1;
            }
            const int last_index = maxSegmentIndex(lo_dir);
            const int hi_last = maxSegmentIndex(hi_dir);
            if (last_index < 4 || hi_last != last_index) {
                std::cerr << "dual-variant fixture segments lo=" << last_index
                          << " hi=" << hi_last << "\n";
                return 1;
            }
            const std::filesystem::path get_log = master_root / "gets.log";
            LocalHttp server = startLocalHttp(master_root, get_log);
            if (server.pid <= 0 || server.port <= 0) {
                std::cerr << "failed to bind local HLS server\n";
                return 1;
            }
            testMasterOrder(server, master_root, get_log, true, last_index);
            testMasterOrder(server, master_root, get_log, false, last_index);
            server.stop();
        } else if (hls_rc == 127) {
            std::cout << "SKIP ffmpeg CLI not found\n";
            return kSkip;
        } else {
            const std::string hls_log = readFile(log);
            if (!encoderMissing(hls_log)) {
                std::cerr << "failed to generate HLS fixture (exit " << hls_rc << ")\n" << hls_log;
                return 1;
            }
            std::cout << "HLS fixture skipped: libx264 encoder not available\n";
        }

        if (decoderTestsRequired() && !hls_decoded) {
            std::cerr << "HLS coverage required (AMS_REQUIRE_DECODER_TESTS=1) but it did not run\n";
            return 1;
        }

        if (g_failures != 0) {
            std::cerr << g_failures << " check(s) failed\n";
            return 1;
        }
        std::cout << "ffmpeg_decoder_tests ok (" << (hls_decoded ? "mp4, hls" : "mp4") << ")\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL exception " << ex.what() << "\n";
        return 1;
    }
}
