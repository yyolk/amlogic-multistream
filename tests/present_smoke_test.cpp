#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

// Black-box check of the --url presenter. Generates a tiny local file with
// the ffmpeg CLI, presents two frames through the software renderer, and
// checks that --renderer drm fails without opening a successful scanout.
// AUDIT7 B2: an HTTP playlist whose only segment is file:///… must be
// rejected on --url, including hls+http:// (same as MQTT add). A path or
// file:// of the fixture must still present. Exit 77 when the ffmpeg CLI
// is missing so ctest can skip.

#ifndef PLAYER_BIN
#define PLAYER_BIN ""
#endif

#ifndef FFMPEG_CLI
#define FFMPEG_CLI ""
#endif

namespace {

constexpr int kSkip = 77;

class TempDir {
public:
    TempDir()
        : path_(std::filesystem::temp_directory_path() /
                ("amlogic-present-smoke-" + std::to_string(static_cast<long long>(::getpid())))) {
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

int run(const std::vector<std::string>& args,
        const std::filesystem::path& outFile,
        const std::filesystem::path& errFile,
        long* maxRssKb = nullptr) {
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
        const int outFd = ::open(outFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        const int errFd = ::open(errFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (outFd >= 0) {
            dup2(outFd, STDOUT_FILENO);
            if (outFd != STDOUT_FILENO) {
                ::close(outFd);
            }
        }
        if (errFd >= 0) {
            dup2(errFd, STDERR_FILENO);
            if (errFd != STDERR_FILENO) {
                ::close(errFd);
            }
        }
        execvp(argv[0], argv.data());
        _exit(127);
    }

    int status = 0;
    rusage usage{};
    if (wait4(pid, &status, 0, &usage) < 0) {
        return -1;
    }
    if (maxRssKb != nullptr) {
        *maxRssKb = usage.ru_maxrss;
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

bool ppmLooksValid(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string header(64, '\0');
    in.read(header.data(), static_cast<std::streamsize>(header.size()));
    const auto n = in.gcount();
    if (n <= 0) {
        return false;
    }
    header.resize(static_cast<std::size_t>(n));
    return header.compare(0, 3, "P6\n") == 0 && header.find("\n255\n") != std::string::npos &&
           std::filesystem::file_size(path) > 32;
}

void rstClose(int fd) {
    linger lin{};
    lin.l_onoff = 1;
    lin.l_linger = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
    ::close(fd);
}

std::string mpegTsSyncPrefix() {
    std::string body(377, 'X');
    body[0] = body[188] = body[376] = static_cast<char>(0x47);
    return body;
}

std::string ftypPrefix() {
    std::string body(32, '\0');
    body[4] = 'f';
    body[5] = 't';
    body[6] = 'y';
    body[7] = 'p';
    return body;
}

std::string id3Synchsafe(uint32_t n) {
    std::string s(4, '\0');
    s[0] = static_cast<char>((n >> 21) & 0x7f);
    s[1] = static_cast<char>((n >> 14) & 0x7f);
    s[2] = static_cast<char>((n >> 7) & 0x7f);
    s[3] = static_cast<char>(n & 0x7f);
    return s;
}

std::string id3v23Frame(const char id[4], const std::string& payload, unsigned char flags2 = 0) {
    std::string f(10, '\0');
    std::memcpy(&f[0], id, 4);
    const uint32_t n = static_cast<uint32_t>(payload.size());
    f[4] = static_cast<char>((n >> 24) & 0xff);
    f[5] = static_cast<char>((n >> 16) & 0xff);
    f[6] = static_cast<char>((n >> 8) & 0xff);
    f[7] = static_cast<char>(n & 0xff);
    f[9] = static_cast<char>(flags2);
    return f + payload;
}

std::string id3v23Tag(const std::string& frames) {
    std::string tag = "ID3";
    tag.push_back(3);
    tag.push_back(0);
    tag.push_back(0);
    tag += id3Synchsafe(static_cast<uint32_t>(frames.size()));
    return tag + frames;
}

std::string id3PrivPayload(const char* owner) {
    std::string p(owner);
    p.push_back('\0');
    p.append(5, '\0');
    p.push_back(0x01);
    p.push_back(static_cast<char>(0x5F));
    p.push_back(static_cast<char>(0x90));
    return p;
}

std::string id3v23HlsTimestampTag(const char* owner = "com.apple.streaming.transportStreamTimestamp",
                                  size_t pad_before_priv = 0, unsigned char priv_flags2 = 0) {
    std::string frames(pad_before_priv, '\0');
    frames += id3v23Frame("PRIV", id3PrivPayload(owner), priv_flags2);
    return id3v23Tag(frames);
}

std::string id3v23Tit2Tag() {
    return id3v23Tag(id3v23Frame("TIT2", std::string("\x00en", 3) + "decoy"));
}

std::string id3v23ZlibPrivFlag08() {
    static const unsigned char z[] = {
        0x78, 0x9c, 0x4b, 0xce, 0xcf, 0xd5, 0x4b, 0x2c, 0x28, 0xc8, 0x49, 0xd5, 0x2b, 0x2e,
        0x29, 0x4a, 0x4d, 0xcc, 0xcd, 0xcc, 0x4b, 0xd7, 0x2b, 0x29, 0x4a, 0xcc, 0x2b, 0x2e,
        0xc8, 0x2f, 0x2a, 0x09, 0x06, 0x0b, 0x85, 0x64, 0xe6, 0xa6, 0x16, 0x97, 0x24, 0xe6,
        0x16, 0x30, 0x80, 0x01, 0x63, 0xfc, 0x04, 0x00, 0x27, 0xd6, 0x12, 0xa3};
    return id3v23Tag(id3v23Frame("PRIV",
                                 std::string(reinterpret_cast<const char*>(z), sizeof(z)), 0x08));
}

std::string id3v23ExtHdrZlibPriv() {
    static const unsigned char z[] = {
        0x78, 0x9c, 0x4b, 0xce, 0xcf, 0xd5, 0x4b, 0x2c, 0x28, 0xc8, 0x49, 0xd5, 0x2b, 0x2e,
        0x29, 0x4a, 0x4d, 0xcc, 0xcd, 0xcc, 0x4b, 0xd7, 0x2b, 0x29, 0x4a, 0xcc, 0x2b, 0x2e,
        0xc8, 0x2f, 0x2a, 0x09, 0x06, 0x0b, 0x85, 0x64, 0xe6, 0xa6, 0x16, 0x97, 0x24, 0xe6,
        0x16, 0x30, 0x80, 0x01, 0x63, 0xfc, 0x04, 0x00, 0x27, 0xd6, 0x12, 0xa3};
    const std::string frames = id3v23Frame(
        "PRIV", std::string(reinterpret_cast<const char*>(z), sizeof(z)), 0x08);
    std::string ext(10, '\0');
    ext[3] = 6;
    std::string tag = "ID3";
    tag.push_back(3);
    tag.push_back(0);
    tag.push_back(0x40);
    tag += id3Synchsafe(static_cast<uint32_t>(ext.size() + frames.size()));
    return tag + ext + frames;
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

std::string readRequest(int fd) {
    std::string data;
    char buf[2048];
    while (data.find("\r\n\r\n") == std::string::npos && data.size() < 65536) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, 2000) <= 0) {
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

std::string requestRawUrl(const std::string& headers) {
    const auto lineEnd = headers.find("\r\n");
    const std::string line = headers.substr(0, lineEnd == std::string::npos ? headers.size() : lineEnd);
    if (line.compare(0, 4, "GET ") != 0) {
        return {};
    }
    const auto sp = line.find(' ', 4);
    if (sp == std::string::npos) {
        return {};
    }
    return line.substr(4, sp - 4);
}

std::string requestPath(const std::string& headers) {
    std::string url = requestRawUrl(headers);
    const auto query = url.find('?');
    if (query != std::string::npos) {
        url.resize(query);
    }
    return url;
}

[[noreturn]] void serveLoop(int listenFd, std::string body) {
    ::signal(SIGPIPE, SIG_IGN);
    while (true) {
        const int client = ::accept(listenFd, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            _exit(0);
        }
        const std::string path = requestPath(readRequest(client));
        if (path != "/file_seg.m3u8") {
            const char* missing =
                "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(client, missing, std::char_traits<char>::length(missing));
            ::close(client);
            continue;
        }
        std::ostringstream hdr;
        hdr << "HTTP/1.1 200 OK\r\nContent-Length: " << body.size()
            << "\r\nContent-Type: application/vnd.apple.mpegurl\r\nConnection: close\r\n\r\n";
        const std::string header = hdr.str();
        sendAll(client, header.data(), header.size());
        sendAll(client, body.data(), body.size());
        ::close(client);
    }
}

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

[[noreturn]] void serveDirLoop(int listenFd, std::filesystem::path root) {
    ::signal(SIGPIPE, SIG_IGN);
    while (true) {
        const int client = ::accept(listenFd, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            _exit(0);
        }
        const std::string headers = readRequest(client);
        const std::string rawUrl = requestRawUrl(headers);
        const std::string path = requestPath(headers);
        auto sendBody = [&](const std::string& payload, const char* type) {
            std::ostringstream hdr;
            hdr << "HTTP/1.1 200 OK\r\nContent-Length: " << payload.size()
                << "\r\nContent-Type: " << type << "\r\nConnection: close\r\n\r\n";
            const std::string header = hdr.str();
            sendAll(client, header.data(), header.size());
            sendAll(client, payload.data(), payload.size());
            ::close(client);
        };
        if (rawUrl == "/d4-query.m3u8?x=1") {
            std::ifstream in(root / "d4-id3-media.m3u8");
            std::ostringstream ss;
            ss << in.rdbuf();
            sendBody(ss.str(), "application/vnd.apple.mpegurl");
            continue;
        }
        if (rawUrl == "/dotdot/../id3/media.m3u8") {
            std::ifstream in(root / "d4-video.m3u8");
            std::ostringstream ss;
            ss << in.rdbuf();
            sendBody(ss.str(), "application/vnd.apple.mpegurl");
            continue;
        }
        std::filesystem::path rel;
        bool ok = !path.empty() && path[0] == '/';
        if (ok) {
            rel = std::filesystem::path(path.substr(1));
            for (const std::filesystem::path& part : rel) {
                if (part == "..") {
                    ok = false;
                    break;
                }
            }
        }
        std::string body;
        if (ok && rel == "seg.ts") {
            const char* redir =
                "HTTP/1.1 302 Found\r\nLocation: /d4-space.m3u8\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(client, redir, std::char_traits<char>::length(redir));
            ::close(client);
            continue;
        }
        if (ok && rel == "failpeek.m3u8") {
            static int failpeek_hits = 0;
            if (failpeek_hits++ == 0) {
                ::close(client);
                continue;
            }
        }
        if (ok && rel == "garbage.m3u8") {
            static int garbage_hits = 0;
            if (garbage_hits++ == 0) {
                const char* stub =
                    "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n"
                    "Content-Type: application/vnd.apple.mpegurl\r\n"
                    "Connection: close\r\n\r\nX";
                sendAll(client, stub, std::char_traits<char>::length(stub));
                ::close(client);
                continue;
            }
        }
        if (ok && rel == "partial.m3u8") {
            static int partial_hits = 0;
            if (partial_hits++ == 0) {
                const char* stub =
                    "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n"
                    "Content-Type: video/mp2t\r\nConnection: close\r\n\r\nX";
                sendAll(client, stub, std::char_traits<char>::length(stub));
                rstClose(client);
                continue;
            }
        }
        if (ok && rel == "decoy256") {
            std::string stub =
                "HTTP/1.1 200 OK\r\nContent-Length: 400\r\n"
                "Content-Type: video/mp2t\r\nConnection: close\r\n\r\n";
            stub.append(256, 'X');
            sendAll(client, stub.data(), stub.size());
            rstClose(client);
            continue;
        }
        if (ok && rel == "hop.m3u8") {
            static int hop_hits = 0;
            const char* loc = hop_hits++ == 0 ? "/decoy256" : "/playlist";
            std::string redir = std::string("HTTP/1.1 302 Found\r\nLocation: ") + loc +
                                "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(client, redir.data(), redir.size());
            ::close(client);
            continue;
        }
        auto sendDecoyRst = [&](const std::string& body, const char* type, size_t extra) {
            std::ostringstream hdr;
            hdr << "HTTP/1.1 200 OK\r\nContent-Length: " << (body.size() + extra)
                << "\r\nContent-Type: " << type << "\r\nConnection: close\r\n\r\n";
            const std::string header = hdr.str();
            sendAll(client, header.data(), header.size());
            sendAll(client, body.data(), body.size());
            rstClose(client);
        };
        if (ok && (rel == "decoyts" || rel == "syncrst")) {
            static int decoyts_hits = 0;
            if (rel == "syncrst" || decoyts_hits++ == 0) {
                sendDecoyRst(mpegTsSyncPrefix(), "video/mp2t", 200);
                continue;
            }
        }
        if (ok && (rel == "decoyftyp" || rel == "ftyprst")) {
            static int decoyftyp_hits = 0;
            if (rel == "ftyprst" || decoyftyp_hits++ == 0) {
                sendDecoyRst(ftypPrefix(), "video/mp4", 200);
                continue;
            }
        }
        if (ok && (rel == "decoyfin" || rel == "finrst")) {
            static int decoyfin_hits = 0;
            if (rel == "finrst" || decoyfin_hits++ == 0) {
                std::ifstream in(root / "small.ts", std::ios::binary);
                std::ostringstream ss;
                ss << in.rdbuf();
                sendDecoyRst(ss.str(), "video/mp2t", 128);
                continue;
            }
        }
        auto sendFlipMaster = [&]() {
            std::ifstream in(root / "d4-master-audio.m3u8");
            std::ostringstream ss;
            ss << in.rdbuf();
            sendBody(ss.str(), "application/vnd.apple.mpegurl");
        };
        if (ok && (rel == "flip.ts" || rel == "flip.mp4")) {
            static int flip_ts_hits = 0;
            static int flip_mp4_hits = 0;
            int* hits = rel == "flip.ts" ? &flip_ts_hits : &flip_mp4_hits;
            if ((*hits)++ == 0) {
                if (rel == "flip.ts") {
                    std::ifstream in(root / "small.ts", std::ios::binary);
                    std::ostringstream ss;
                    ss << in.rdbuf();
                    sendBody(ss.str(), "video/mp2t");
                } else {
                    sendBody(ftypPrefix(), "video/mp4");
                }
                continue;
            }
            sendFlipMaster();
            continue;
        }
        if (ok && rel == "flip-hop.ts") {
            static int flip_hop_hits = 0;
            if (flip_hop_hits++ == 0) {
                const char* redir =
                    "HTTP/1.1 302 Found\r\nLocation: /small.ts\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n";
                sendAll(client, redir, std::char_traits<char>::length(redir));
                ::close(client);
                continue;
            }
            sendFlipMaster();
            continue;
        }
        if (ok && (rel == "fin-sync" || rel == "fin-ftyp" || rel == "fin-small")) {
            static int fin_sync_hits = 0;
            static int fin_ftyp_hits = 0;
            static int fin_small_hits = 0;
            int* hits = rel == "fin-sync" ? &fin_sync_hits : rel == "fin-ftyp" ? &fin_ftyp_hits
                                                                              : &fin_small_hits;
            if ((*hits)++ == 0) {
                std::string body;
                const char* type = "video/mp2t";
                if (rel == "fin-sync") {
                    body = mpegTsSyncPrefix();
                } else if (rel == "fin-ftyp") {
                    body = ftypPrefix();
                    type = "video/mp4";
                } else {
                    std::ifstream in(root / "small.ts", std::ios::binary);
                    std::ostringstream ss;
                    ss << in.rdbuf();
                    body = ss.str();
                }
                std::ostringstream hdr;
                hdr << "HTTP/1.1 200 OK\r\nContent-Length: " << body.size()
                    << "\r\nContent-Type: " << type << "\r\nConnection: close\r\n\r\n";
                const std::string header = hdr.str();
                sendAll(client, header.data(), header.size());
                sendAll(client, body.data(), body.size());
                ::close(client);
                continue;
            }
        }
        if (ok && (rel == "hop-ts.m3u8" || rel == "hop-ftyp.m3u8" || rel == "hop-fin.m3u8")) {
            static int hop_ts_hits = 0;
            static int hop_ftyp_hits = 0;
            static int hop_fin_hits = 0;
            const char* loc = "/playlist";
            if (rel == "hop-ts.m3u8") {
                loc = hop_ts_hits++ == 0 ? "/syncrst" : "/playlist";
            } else if (rel == "hop-ftyp.m3u8") {
                loc = hop_ftyp_hits++ == 0 ? "/ftyprst" : "/playlist";
            } else {
                loc = hop_fin_hits++ == 0 ? "/finrst" : "/playlist";
            }
            std::string redir = std::string("HTTP/1.1 302 Found\r\nLocation: ") + loc +
                                "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(client, redir.data(), redir.size());
            ::close(client);
            continue;
        }
        if (ok && rel == "trunc.m3u8") {
            static int trunc_hits = 0;
            if (trunc_hits++ == 0) {
                const char* stub =
                    "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n"
                    "Content-Type: application/vnd.apple.mpegurl\r\n"
                    "Connection: close\r\n\r\n#EXTM3U\n";
                sendAll(client, stub, std::char_traits<char>::length(stub));
                ::close(client);
                continue;
            }
        }
        if (ok) {
            std::ifstream in(root / rel, std::ios::binary);
            if (!in) {
                ok = false;
            } else {
                std::ostringstream ss;
                ss << in.rdbuf();
                body = ss.str();
            }
        }
        if (!ok) {
            const char* missing =
                "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(client, missing, std::char_traits<char>::length(missing));
            ::close(client);
            continue;
        }
        const char* type = "application/octet-stream";
        const std::string ext = rel.extension().string();
        if (ext == ".m3u8" || ext == ".m3u" || body.compare(0, 7, "#EXTM3U") == 0) {
            type = "application/vnd.apple.mpegurl";
        }
        std::ostringstream hdr;
        hdr << "HTTP/1.1 200 OK\r\nContent-Length: " << body.size()
            << "\r\nContent-Type: " << type << "\r\nConnection: close\r\n\r\n";
        const std::string header = hdr.str();
        sendAll(client, header.data(), header.size());
        sendAll(client, body.data(), body.size());
        ::close(client);
    }
}

LocalHttp startDirServer(const std::filesystem::path& root) {
    LocalHttp server;
    const int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
        return server;
    }
    const int yes = 1;
    ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listenFd, 16) != 0) {
        ::close(listenFd);
        return server;
    }
    sockaddr_in bound{};
    socklen_t boundLen = sizeof(bound);
    if (::getsockname(listenFd, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) {
        ::close(listenFd);
        return server;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        ::close(listenFd);
        return server;
    }
    if (pid == 0) {
        serveDirLoop(listenFd, root);
    }
    ::close(listenFd);
    server.pid = pid;
    server.port = ntohs(bound.sin_port);
    return server;
}

LocalHttp startPlaylistServer(const std::string& body) {
    LocalHttp server;
    const int listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd < 0) {
        return server;
    }
    const int yes = 1;
    ::setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listenFd, 16) != 0) {
        ::close(listenFd);
        return server;
    }
    sockaddr_in bound{};
    socklen_t boundLen = sizeof(bound);
    if (::getsockname(listenFd, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) {
        ::close(listenFd);
        return server;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        ::close(listenFd);
        return server;
    }
    if (pid == 0) {
        serveLoop(listenFd, body);
    }
    ::close(listenFd);
    server.pid = pid;
    server.port = ntohs(bound.sin_port);
    return server;
}

int64_t decodePesPts(const unsigned char* b) {
    return (static_cast<int64_t>((b[0] >> 1) & 0x7) << 30) | (static_cast<int64_t>(b[1]) << 22) |
           (static_cast<int64_t>((b[2] >> 1) & 0x7F) << 15) | (static_cast<int64_t>(b[3]) << 7) |
           (static_cast<int64_t>((b[4] >> 1) & 0x7F));
}

void encodePesPts(unsigned char* b, int prefix, int64_t pts) {
    b[0] = static_cast<unsigned char>((prefix << 4) | (((pts >> 30) & 7) << 1) | 1);
    b[1] = static_cast<unsigned char>((pts >> 22) & 0xFF);
    b[2] = static_cast<unsigned char>((((pts >> 15) & 0x7F) << 1) | 1);
    b[3] = static_cast<unsigned char>((pts >> 7) & 0xFF);
    b[4] = static_cast<unsigned char>(((pts & 0x7F) << 1) | 1);
}

int mpegTsPid(const unsigned char* pkt) {
    return ((pkt[1] & 0x1f) << 8) | pkt[2];
}

int mpegTsPayloadOffset(const unsigned char* pkt) {
    const int adapt = (pkt[3] >> 4) & 0x3;
    if (adapt == 1) {
        return 4;
    }
    if (adapt == 2 || adapt == 3) {
        return 5 + static_cast<int>(pkt[4]);
    }
    return 188;
}

// AUDIT11 D3: bump the second video PES timestamp by 10 s (90 kHz) so the
// D2 pre-pass stops on FFmpeg's 7 s mpegts analyze gap, then append an
// undeclared 0x101 PID. find_stream_info does not stop on that gap.
bool writeJumpThenExtraTs(const std::filesystem::path& pmt,
                          const std::filesystem::path& extra,
                          const std::filesystem::path& dst) {
    std::ifstream in(pmt, std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 188 || data.size() % 188 != 0) {
        return false;
    }
    int pes = 0;
    bool rewritten = false;
    for (size_t i = 0; i + 188 <= data.size(); i += 188) {
        auto* pkt = reinterpret_cast<unsigned char*>(&data[i]);
        if (pkt[0] != 0x47) {
            return false;
        }
        if ((pkt[1] & 0x40) == 0 || mpegTsPid(pkt) != 0x100) {
            continue;
        }
        const int off = mpegTsPayloadOffset(pkt);
        if (off + 14 > 188 || pkt[off] != 0 || pkt[off + 1] != 0 || pkt[off + 2] != 1 ||
            pkt[off + 3] != 0xE0) {
            continue;
        }
        ++pes;
        const int pts_dts = (pkt[off + 7] >> 6) & 0x3;
        if (pes != 2 || (pts_dts != 2 && pts_dts != 3)) {
            continue;
        }
        unsigned char* ptsb = pkt + off + 9;
        const int prefix = pts_dts == 2 ? 2 : 3;
        encodePesPts(ptsb, prefix, decodePesPts(ptsb) + 900000);
        if (pts_dts == 3 && off + 19 <= 188) {
            encodePesPts(pkt + off + 14, 1, decodePesPts(pkt + off + 14) + 900000);
        }
        rewritten = true;
        break;
    }
    if (!rewritten) {
        return false;
    }

    std::ofstream out(dst, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    std::ifstream more(extra, std::ios::binary);
    char pkt[188];
    bool kept = false;
    while (more.read(pkt, 188) && more.gcount() == 188) {
        if (static_cast<unsigned char>(pkt[0]) != 0x47) {
            return false;
        }
        const int pid = mpegTsPid(reinterpret_cast<unsigned char*>(pkt));
        if (pid == 0x100) {
            pkt[1] = static_cast<char>((pkt[1] & 0xE0) | 0x01);
            pkt[2] = 0x01;
            out.write(pkt, 188);
            kept = true;
        }
    }
    return kept && out.good();
}

}  // namespace

int main() {
    try {
        const char* player = PLAYER_BIN;
        if (player == nullptr || player[0] == '\0') {
            std::cerr << "PLAYER_BIN is not set\n";
            return 1;
        }

        TempDir tmp;
        const std::filesystem::path log = tmp.path() / "ffmpeg.log";
        const std::filesystem::path empty = tmp.path() / "empty.log";
        const std::filesystem::path mp4 = tmp.path() / "clip.mp4";
        const int gen = run({ffmpegBinary(), "-y", "-loglevel", "error", "-f", "lavfi", "-i",
                             "testsrc=size=160x120:rate=10:duration=0.4", "-c:v", "mpeg4", "-pix_fmt",
                             "yuv420p", mp4.string()},
                            empty, log);
        if (gen == 127) {
            std::cout << "SKIP ffmpeg CLI not found\n";
            return kSkip;
        }
        if (gen != 0) {
            std::cerr << "failed to generate fixture (exit " << gen << ")\n" << readFile(log);
            return 1;
        }

        const std::filesystem::path frames = tmp.path() / "frames";
        const std::filesystem::path out = tmp.path() / "present.out";
        const std::filesystem::path err = tmp.path() / "present.err";
        const int presented = run({player, "--url", mp4.string(), "--frames", "2", "--render-dir", frames.string()},
                                  out, err);
        const std::string presentOut = readFile(out);
        const std::string presentErr = readFile(err);
        if (presented != 0) {
            std::cerr << "software present failed (exit " << presented << ")\n" << presentOut << presentErr;
            return 1;
        }
        if (presentOut.find("Presenter software (no /dev/dri)") == std::string::npos ||
            presentOut.find("fmt=yuv420p") == std::string::npos ||
            presentOut.find("layout 1x1") == std::string::npos ||
            presentOut.find("rgbsum=") == std::string::npos ||
            presentOut.find("Decoded 2 frame(s)") == std::string::npos) {
            std::cerr << "software present output missing expected lines\n" << presentOut;
            return 1;
        }
        if (!ppmLooksValid(frames / "frame-000000.ppm") || !ppmLooksValid(frames / "frame-000001.ppm")) {
            std::cerr << "expected two PPM frames in " << frames << "\n";
            return 1;
        }

        // AUDIT8 P1: 8224x32 is inside the old copy caps (stride/height) but
        // present() rejects width > 8192. --url must fail that frame once,
        // not print it and then die at viewport init.
        {
            const std::filesystem::path wideMp4 = tmp.path() / "wide.mp4";
            const int genWide = run({ffmpegBinary(), "-y", "-loglevel", "error", "-f", "lavfi", "-i",
                                     "testsrc=size=8224x32:rate=10:duration=0.4", "-c:v", "libx264",
                                     "-pix_fmt", "yuv420p", wideMp4.string()},
                                    empty, log);
            if (genWide != 0) {
                std::cerr << "failed to generate wide fixture (exit " << genWide << ")\n"
                          << readFile(log);
                return 1;
            }
            const int wideRc = run({player, "--url", wideMp4.string(), "--frames", "2"}, out, err);
            const std::string wideOut = readFile(out);
            const std::string wideErr = readFile(err);
            // AUDIT9 D1: reject before sws_scale, so the convert line is absent.
            if (wideRc == 0 || wideOut.find("presented ") != std::string::npos ||
                wideOut.find("Decoded ") != std::string::npos ||
                wideOut.find("layout ") != std::string::npos ||
                wideOut.find("-> yuv420p") != std::string::npos ||
                wideErr.find("cannot be presented") == std::string::npos ||
                wideErr.find("viewport must be") != std::string::npos) {
                std::cerr << "wide --url must reject the frame before present (exit " << wideRc
                          << ")\n"
                          << wideOut << wideErr;
                return 1;
            }
        }

        // Inclusive cap: 8192x32 must still present.
        {
            const std::filesystem::path edgeMp4 = tmp.path() / "edge.mp4";
            const int genEdge = run({ffmpegBinary(), "-y", "-loglevel", "error", "-f", "lavfi", "-i",
                                     "testsrc=size=8192x32:rate=10:duration=0.4", "-c:v", "libx264",
                                     "-pix_fmt", "yuv420p", edgeMp4.string()},
                                    empty, log);
            if (genEdge != 0) {
                std::cerr << "failed to generate edge fixture (exit " << genEdge << ")\n"
                          << readFile(log);
                return 1;
            }
            const int edgeRc = run({player, "--url", edgeMp4.string(), "--frames", "1"}, out, err);
            const std::string edgeOut = readFile(out);
            const std::string edgeErr = readFile(err);
            if (edgeRc != 0 || edgeOut.find("presented ") == std::string::npos ||
                edgeOut.find("8192x32") == std::string::npos) {
                std::cerr << "8192x32 --url must present (exit " << edgeRc << ")\n"
                          << edgeOut << edgeErr;
                return 1;
            }
        }

        // AUDIT10 D2: a PSI-less 12000x12000 H.264 TS creates its stream
        // during find_stream_info. Probe max_pixels must still apply.
        {
            const std::filesystem::path hugeTs = tmp.path() / "huge.ts";
            const std::filesystem::path hugePsi = tmp.path() / "huge-psiless.ts";
            const int genHugeTs = run({ffmpegBinary(), "-y", "-loglevel", "error", "-f", "lavfi", "-i",
                                       "color=c=red:s=12000x12000:d=0.04", "-frames:v", "1", "-c:v",
                                       "libx264", "-preset", "ultrafast", "-tune", "stillimage",
                                       "-crf", "51", "-pix_fmt", "yuv420p", "-f", "mpegts",
                                       hugeTs.string()},
                                      empty, log);
            if (genHugeTs != 0) {
                std::cerr << "failed to generate 12000x12000 mpegts fixture (exit " << genHugeTs
                          << ")\n"
                          << readFile(log);
                return 1;
            }
            {
                std::ifstream in(hugeTs, std::ios::binary);
                std::ofstream out(hugePsi, std::ios::binary);
                char pkt[188];
                bool kept = false;
                while (in.read(pkt, 188) && in.gcount() == 188) {
                    if (static_cast<unsigned char>(pkt[0]) != 0x47) {
                        std::cerr << "huge mpegts fixture is not 188-byte packets\n";
                        return 1;
                    }
                    const int pid = ((static_cast<unsigned char>(pkt[1]) & 0x1f) << 8) |
                                    static_cast<unsigned char>(pkt[2]);
                    if (pid >= 0x20 && pid != 0x1000) {
                        out.write(pkt, 188);
                        kept = true;
                    }
                }
                if (!kept) {
                    std::cerr << "failed to strip PSI from 12000x12000 mpegts fixture\n";
                    return 1;
                }
            }
            long maxRssKb = 0;
            const int hugeTsRc =
                run({player, "--url", hugePsi.string(), "--frames", "1"}, out, err, &maxRssKb);
            const std::string hugeTsOut = readFile(out);
            const std::string hugeTsErr = readFile(err);
            constexpr long kConvertRssKb = 300000;
            if (hugeTsRc == 0 || hugeTsOut.find("presented ") != std::string::npos ||
                hugeTsOut.find("Decoded ") != std::string::npos ||
                hugeTsOut.find("layout ") != std::string::npos ||
                hugeTsOut.find("-> yuv420p") != std::string::npos || maxRssKb >= kConvertRssKb) {
                std::cerr << "psi-less oversize --url must fail before convert (exit " << hugeTsRc
                          << " rss_kb=" << maxRssKb << ")\n"
                          << hugeTsOut << hugeTsErr;
                return 1;
            }

            // HLS of a PMT 160x120 plus an undeclared 12000x12000 PID. The
            // extra stream is born after open_input; probe max_pixels must
            // still apply (AUDIT10 D2).
            const std::filesystem::path smallTs = tmp.path() / "small.ts";
            const int genSmallTs = run({ffmpegBinary(), "-y", "-loglevel", "error", "-f", "lavfi",
                                        "-i", "testsrc=size=160x120:rate=10:duration=0.4", "-c:v",
                                        "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p",
                                        "-f", "mpegts", smallTs.string()},
                                       empty, log);
            if (genSmallTs != 0) {
                std::cerr << "failed to generate small mpegts fixture (exit " << genSmallTs << ")\n"
                          << readFile(log);
                return 1;
            }
            const std::filesystem::path extraPid = tmp.path() / "extra-pid.ts";
            {
                std::ifstream base(smallTs, std::ios::binary);
                std::ifstream more(hugeTs, std::ios::binary);
                std::ofstream extra(extraPid, std::ios::binary);
                extra << base.rdbuf();
                char pkt[188];
                bool kept = false;
                while (more.read(pkt, 188) && more.gcount() == 188) {
                    if (static_cast<unsigned char>(pkt[0]) != 0x47) {
                        std::cerr << "huge mpegts fixture is not 188-byte packets\n";
                        return 1;
                    }
                    const int pid = ((static_cast<unsigned char>(pkt[1]) & 0x1f) << 8) |
                                    static_cast<unsigned char>(pkt[2]);
                    if (pid == 0x100) {
                        pkt[1] = static_cast<char>((pkt[1] & 0xE0) | 0x01);
                        pkt[2] = 0x01;
                        extra.write(pkt, 188);
                        kept = true;
                    }
                }
                if (!kept) {
                    std::cerr << "failed to build extra-PID mpegts fixture\n";
                    return 1;
                }
            }
            const std::filesystem::path extraHls = tmp.path() / "extra-pid.m3u8";
            {
                std::ofstream playlist(extraHls);
                playlist << "#EXTM3U\n"
                         << "#EXT-X-VERSION:3\n"
                         << "#EXT-X-TARGETDURATION:1\n"
                         << "#EXT-X-MEDIA-SEQUENCE:0\n"
                         << "#EXTINF:0.400,\n"
                         << extraPid.filename().string() << "\n"
                         << "#EXT-X-ENDLIST\n";
            }
            long extraRssKb = 0;
            const int extraRc =
                run({player, "--url", extraHls.string(), "--frames", "1"}, out, err, &extraRssKb);
            const std::string extraOut = readFile(out);
            const std::string extraErr = readFile(err);
            if (extraRc != 0 || extraOut.find("presented ") == std::string::npos ||
                extraOut.find("160x120") == std::string::npos || extraRssKb >= kConvertRssKb) {
                std::cerr << "extra-PID HLS --url must present the small stream (exit " << extraRc
                          << " rss_kb=" << extraRssKb << ")\n"
                          << extraOut << extraErr;
                return 1;
            }

            // AUDIT11 D3: 10 s PTS jump on the PMT program, then the same
            // undeclared 12000x12000 PID. The pre-pass stops on the gap;
            // find_stream_info must not decode the late picture uncapped.
            const std::filesystem::path jumpExtra = tmp.path() / "jump-then-extra.ts";
            if (!writeJumpThenExtraTs(smallTs, hugeTs, jumpExtra)) {
                std::cerr << "failed to build jump-then-extra mpegts fixture\n";
                return 1;
            }
            long jumpRssKb = 0;
            const int jumpRc =
                run({player, "--url", jumpExtra.string(), "--frames", "1"}, out, err, &jumpRssKb);
            const std::string jumpOut = readFile(out);
            const std::string jumpErr = readFile(err);
            // Uncapped D3 was ~381 MB. D2 in-window controls stay under ~90 MB.
            constexpr long kJumpRssKb = 100000;
            if (jumpRc != 0 || jumpOut.find("presented ") == std::string::npos ||
                jumpOut.find("160x120") == std::string::npos || jumpRssKb >= kJumpRssKb) {
                std::cerr << "jump-then-extra --url must present the small stream (exit " << jumpRc
                          << " rss_kb=" << jumpRssKb << ")\n"
                          << jumpOut << jumpErr;
                return 1;
            }

            // AUDIT12 D4: master whose AUDIO group is the 12000x12000 TS
            // and whose video playlist is small.ts. FFmpeg 6.1.1 probes
            // that audio rendition inside avformat_open_input with
            // find_stream_info(NULL) / max_pixels == INT_MAX (~380 MB).
            const std::filesystem::path audioHls = tmp.path() / "d4-audio.m3u8";
            const std::filesystem::path videoHls = tmp.path() / "d4-video.m3u8";
            const std::filesystem::path masterAudio = tmp.path() / "d4-master-audio.m3u8";
            {
                std::ofstream audio(audioHls);
                audio << "#EXTM3U\n"
                      << "#EXT-X-VERSION:3\n"
                      << "#EXT-X-TARGETDURATION:1\n"
                      << "#EXT-X-MEDIA-SEQUENCE:0\n"
                      << "#EXTINF:0.040,\n"
                      << hugeTs.filename().string() << "\n"
                      << "#EXT-X-ENDLIST\n";
                std::ofstream video(videoHls);
                video << "#EXTM3U\n"
                      << "#EXT-X-VERSION:3\n"
                      << "#EXT-X-TARGETDURATION:1\n"
                      << "#EXT-X-MEDIA-SEQUENCE:0\n"
                      << "#EXTINF:0.400,\n"
                      << smallTs.filename().string() << "\n"
                      << "#EXT-X-ENDLIST\n";
                std::ofstream master(masterAudio);
                master << "#EXTM3U\n"
                       << "#EXT-X-VERSION:4\n"
                       << "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\","
                          "DEFAULT=YES,AUTOSELECT=YES,URI=\""
                       << audioHls.filename().string() << "\"\n"
                       << "#EXT-X-STREAM-INF:BANDWIDTH=400000,"
                          "CODECS=\"avc1.42e01e,mp4a.40.2\",AUDIO=\"aud\"\n"
                       << videoHls.filename().string() << "\n";
            }
            auto d4MustStayCapped = [&](const char* label, int rc, long rss, const std::string& gotOut,
                                        const std::string& gotErr, bool mustPresent) {
                constexpr long kD4RssKb = 100000;
                if (rss >= kD4RssKb || gotOut.find("12000x12000") != std::string::npos ||
                    (gotOut.find("presented ") != std::string::npos &&
                     gotOut.find("160x120") == std::string::npos)) {
                    std::cerr << "AUDIT12 D4 " << label << " must stay under " << kD4RssKb
                              << " KB (exit " << rc << " rss_kb=" << rss << ")\n"
                              << gotOut << gotErr;
                    return false;
                }
                if (mustPresent &&
                    (rc != 0 || gotOut.find("presented ") == std::string::npos ||
                     gotOut.find("160x120") == std::string::npos)) {
                    std::cerr << "AUDIT12 D4 " << label << " must present 160x120 (exit " << rc
                              << " rss_kb=" << rss << ")\n"
                              << gotOut << gotErr;
                    return false;
                }
                if (!mustPresent &&
                    (rc == 0 || gotOut.find("presented ") != std::string::npos ||
                     gotOut.find("Decoded ") != std::string::npos)) {
                    std::cerr << "AUDIT12 D4 " << label << " must refuse open (exit " << rc
                              << " rss_kb=" << rss << ")\n"
                              << gotOut << gotErr;
                    return false;
                }
                return true;
            };

            long d4RssKb = 0;
            const int d4Rc =
                run({player, "--url", masterAudio.string(), "--frames", "1"}, out, err, &d4RssKb);
            if (!d4MustStayCapped("master-audio", d4Rc, d4RssKb, readFile(out), readFile(err),
                                  true)) {
                return 1;
            }

            // No STREAM-INF: rewrite used to keep the master and the nested
            // probe still ran at INT_MAX (~381 MB). Refuse instead.
            const std::filesystem::path mediaOnly = tmp.path() / "d4-media-only.m3u8";
            {
                std::ofstream playlist(mediaOnly);
                playlist << "#EXTM3U\n"
                         << "#EXT-X-VERSION:4\n"
                         << "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\","
                            "DEFAULT=YES,URI=\""
                         << audioHls.filename().string() << "\"\n"
                         << "#EXT-X-TARGETDURATION:1\n"
                         << "#EXT-X-MEDIA-SEQUENCE:0\n"
                         << "#EXTINF:0.400,\n"
                         << smallTs.filename().string() << "\n"
                         << "#EXT-X-ENDLIST\n";
            }
            long mediaRss = 0;
            const int mediaRc =
                run({player, "--url", mediaOnly.string(), "--frames", "1"}, out, err, &mediaRss);
            if (!d4MustStayCapped("media-only AUDIO", mediaRc, mediaRss, readFile(out),
                                  readFile(err), false)) {
                return 1;
            }

            // STREAM-INF points at this playlist. The first rewrite used to
            // skip (resolved == url) and FFmpeg then recursed (~1 GB).
            const std::filesystem::path selfMaster = tmp.path() / "d4-self.m3u8";
            {
                std::ofstream playlist(selfMaster);
                playlist << "#EXTM3U\n"
                         << "#EXT-X-VERSION:4\n"
                         << "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\","
                            "DEFAULT=YES,URI=\""
                         << audioHls.filename().string() << "\"\n"
                         << "#EXT-X-STREAM-INF:BANDWIDTH=400000,AUDIO=\"aud\"\n"
                         << selfMaster.filename().string() << "\n";
            }
            long selfRss = 0;
            const int selfRc =
                run({player, "--url", selfMaster.string(), "--frames", "1"}, out, err, &selfRss);
            if (!d4MustStayCapped("self-loop STREAM-INF", selfRc, selfRss, readFile(out),
                                  readFile(err), false)) {
                return 1;
            }

            // One-level rewrite used to stop on the inner master, which still
            // names the same audio group.
            const std::filesystem::path nestedInner = tmp.path() / "d4-nested-inner.m3u8";
            const std::filesystem::path nestedOuter = tmp.path() / "d4-nested-outer.m3u8";
            {
                std::ofstream inner(nestedInner);
                inner << "#EXTM3U\n"
                      << "#EXT-X-VERSION:4\n"
                      << "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\","
                         "DEFAULT=YES,URI=\""
                      << audioHls.filename().string() << "\"\n"
                      << "#EXT-X-STREAM-INF:BANDWIDTH=400000,AUDIO=\"aud\"\n"
                      << videoHls.filename().string() << "\n";
                std::ofstream outer(nestedOuter);
                outer << "#EXTM3U\n"
                      << "#EXT-X-VERSION:4\n"
                      << "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\","
                         "DEFAULT=YES,URI=\""
                      << audioHls.filename().string() << "\"\n"
                      << "#EXT-X-STREAM-INF:BANDWIDTH=400000,AUDIO=\"aud\"\n"
                      << nestedInner.filename().string() << "\n";
            }
            long nestedRss = 0;
            const int nestedRc =
                run({player, "--url", nestedOuter.string(), "--frames", "1"}, out, err, &nestedRss);
            if (!d4MustStayCapped("nested master", nestedRc, nestedRss, readFile(out),
                                  readFile(err), true)) {
                return 1;
            }

            const std::filesystem::path nestedNoAudio = tmp.path() / "d4-nested-noaudio-outer.m3u8";
            {
                std::ofstream outer(nestedNoAudio);
                outer << "#EXTM3U\n#EXT-X-VERSION:4\n"
                      << "#EXT-X-STREAM-INF:BANDWIDTH=400000\n"
                      << nestedInner.filename().string() << "\n";
            }
            long noAudioRss = 0;
            const int noAudioRc =
                run({player, "--url", nestedNoAudio.string(), "--frames", "1"}, out, err, &noAudioRss);
            if (!d4MustStayCapped("nested master without outer AUDIO", noAudioRc, noAudioRss,
                                  readFile(out), readFile(err), true)) {
                return 1;
            }

            const std::filesystem::path spaceMaster = tmp.path() / "d4-space.m3u8";
            const std::filesystem::path lastWins = tmp.path() / "d4-lastwins.m3u8";
            const std::filesystem::path unclosed = tmp.path() / "d4-unclosed.m3u8";
            const std::filesystem::path playlistTs = tmp.path() / "playlist.ts";
            const std::filesystem::path truncMaster = tmp.path() / "trunc.m3u8";
            {
                std::ostringstream space;
                space << "#EXTM3U\n"
                      << "#EXT-X-VERSION:4\n"
                      << "#EXT-X-MEDIA:TYPE=AUDIO GROUP-ID=\"aud\" NAME=\"en\" "
                         "DEFAULT=YES URI=\""
                      << audioHls.filename().string() << "\"\n"
                      << "#EXT-X-STREAM-INF:BANDWIDTH=400000 AUDIO=\"aud\"\n"
                      << videoHls.filename().string() << "\n";
                std::ofstream(spaceMaster) << space.str();
                std::ofstream(playlistTs) << space.str();
                std::ofstream(truncMaster) << space.str();
                std::ofstream(lastWins) << "#EXTM3U\n"
                                        << "#EXT-X-VERSION:4\n"
                                        << "#EXT-X-MEDIA:TYPE=VIDEO,TYPE=AUDIO,URI=\"\","
                                           "URI=\""
                                        << audioHls.filename().string() << "\"\n"
                                        << "#EXT-X-STREAM-INF:BANDWIDTH=400000\n"
                                        << videoHls.filename().string() << "\n";
                std::ofstream(unclosed) << "#EXTM3U\n"
                                        << "#EXT-X-VERSION:4\n"
                                        << "#EXT-X-MEDIA:TYPE=AUDIO,URI=\""
                                        << audioHls.filename().string() << "\n"
                                        << "#EXT-X-STREAM-INF:BANDWIDTH=400000\n"
                                        << videoHls.filename().string() << "\n";
            }
            long spaceRss = 0;
            const int spaceRc =
                run({player, "--url", spaceMaster.string(), "--frames", "1"}, out, err, &spaceRss);
            if (!d4MustStayCapped("space-separated MEDIA", spaceRc, spaceRss, readFile(out),
                                  readFile(err), true)) {
                return 1;
            }
            long lastRss = 0;
            const int lastRc =
                run({player, "--url", lastWins.string(), "--frames", "1"}, out, err, &lastRss);
            if (!d4MustStayCapped("last-wins TYPE/URI", lastRc, lastRss, readFile(out),
                                  readFile(err), true)) {
                return 1;
            }
            long unclosedRss = 0;
            const int unclosedRc =
                run({player, "--url", unclosed.string(), "--frames", "1"}, out, err, &unclosedRss);
            if (!d4MustStayCapped("unclosed URI quote", unclosedRc, unclosedRss, readFile(out),
                                  readFile(err), true)) {
                return 1;
            }

            auto writeD4Master = [&](const std::filesystem::path& path, const std::string& mediaPrefix,
                                     bool mediaAfterStreaminf) {
                std::string body = "#EXTM3U\n#EXT-X-VERSION:4\n";
                const std::string media = std::string("#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",") +
                                          "NAME=\"en\",DEFAULT=YES,URI=\"" +
                                          audioHls.filename().string() + "\"";
                const std::string stream = "#EXT-X-STREAM-INF:BANDWIDTH=400000,AUDIO=\"aud\"";
                if (mediaAfterStreaminf) {
                    body += stream;
                    body += mediaPrefix;
                    body += media;
                    body += '\n';
                } else {
                    body += mediaPrefix;
                    body += media;
                    body += '\n';
                    body += stream;
                    body += '\n';
                }
                body += videoHls.filename().string();
                body += '\n';
                std::ofstream(path, std::ios::binary) << body;
            };
            const std::filesystem::path crMedia = tmp.path() / "d4-cr-media.m3u8";
            const std::filesystem::path nulMedia = tmp.path() / "d4-nul-media.m3u8";
            const std::filesystem::path cNulMedia = tmp.path() / "d4-cnul-media.m3u8";
            const std::filesystem::path streamCr = tmp.path() / "d4-streaminf-cr-media.m3u8";
            writeD4Master(crMedia, std::string(1, '\r'), false);
            writeD4Master(nulMedia, std::string(1, '\0'), false);
            writeD4Master(cNulMedia, std::string("#c") + '\0', false);
            writeD4Master(streamCr, std::string(1, '\r'), true);
            {
                std::ifstream src(masterAudio);
                std::ofstream(tmp.path() / "failpeek.m3u8") << src.rdbuf();
            }
            {
                std::ifstream src(masterAudio);
                std::ofstream(tmp.path() / "garbage.m3u8") << src.rdbuf();
            }
            long crRss = 0;
            const int crRc =
                run({player, "--url", crMedia.string(), "--frames", "1"}, out, err, &crRss);
            if (!d4MustStayCapped("CR-prefixed MEDIA", crRc, crRss, readFile(out), readFile(err),
                                  true)) {
                return 1;
            }
            long nulRss = 0;
            const int nulRc =
                run({player, "--url", nulMedia.string(), "--frames", "1"}, out, err, &nulRss);
            if (!d4MustStayCapped("NUL-prefixed MEDIA", nulRc, nulRss, readFile(out), readFile(err),
                                  true)) {
                return 1;
            }
            long cNulRss = 0;
            const int cNulRc =
                run({player, "--url", cNulMedia.string(), "--frames", "1"}, out, err, &cNulRss);
            if (!d4MustStayCapped("comment-NUL MEDIA", cNulRc, cNulRss, readFile(out),
                                  readFile(err), true)) {
                return 1;
            }
            long streamCrRss = 0;
            const int streamCrRc =
                run({player, "--url", streamCr.string(), "--frames", "1"}, out, err, &streamCrRss);
            if (!d4MustStayCapped("STREAM-INF CR MEDIA", streamCrRc, streamCrRss, readFile(out),
                                  readFile(err), true)) {
                return 1;
            }

            const std::filesystem::path notakeyUri = tmp.path() / "d4-notakey-uri.m3u8";
            const std::filesystem::path notakeyType = tmp.path() / "d4-notakey-type.m3u8";
            const std::filesystem::path notakeyExtinf = tmp.path() / "d4-notakey-extinf.m3u8";
            const std::filesystem::path overlong = tmp.path() / "d4-overlong.m3u8";
            {
                std::ofstream(notakeyUri)
                    << "#EXTM3U\n"
                    << "#EXT-X-VERSION:4\n"
                    << "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",DEFAULT=YES,URI=\""
                    << audioHls.filename().string() << "\",NOTAKEY URI=\"\"\n"
                    << "#EXT-X-STREAM-INF:BANDWIDTH=400000,AUDIO=\"aud\"\n"
                    << videoHls.filename().string() << "\n";
                std::ofstream(notakeyType)
                    << "#EXTM3U\n"
                    << "#EXT-X-VERSION:4\n"
                    << "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",DEFAULT=YES,URI=\""
                    << audioHls.filename().string() << "\",NOTAKEY TYPE=VIDEO\n"
                    << "#EXT-X-STREAM-INF:BANDWIDTH=400000,AUDIO=\"aud\"\n"
                    << videoHls.filename().string() << "\n";
                std::ofstream(notakeyExtinf)
                    << "#EXTM3U\n"
                    << "#EXT-X-VERSION:4\n"
                    << "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",DEFAULT=YES,URI=\""
                    << audioHls.filename().string() << "\",NOTAKEY URI=\"\"\n"
                    << "#EXT-X-TARGETDURATION:1\n"
                    << "#EXT-X-MEDIA-SEQUENCE:0\n"
                    << "#EXTINF:0.400,\n"
                    << smallTs.filename().string() << "\n"
                    << "#EXT-X-ENDLIST\n";
                std::string media =
                    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",DEFAULT=YES,URI=\"";
                media += audioHls.filename().string();
                media += "\",PAD=";
                if (media.size() < 4095) {
                    media.append(4095 - media.size(), 'x');
                }
                media += ",TYPE=VIDEO,URI=\"\"";
                std::string longBody = "#EXTM3U\n#EXT-X-VERSION:4\n";
                longBody += media;
                longBody += "\n#EXT-X-STREAM-INF:BANDWIDTH=400000,AUDIO=\"aud\"\n";
                longBody += videoHls.filename().string();
                longBody += '\n';
                std::ofstream(overlong, std::ios::binary) << longBody;
                std::ifstream src(notakeyUri);
                std::ofstream(tmp.path() / "notakey-http.m3u8") << src.rdbuf();
                std::ifstream masterSrc(masterAudio);
                std::ofstream(tmp.path() / "partial.m3u8") << masterSrc.rdbuf();
            }
            long nkUriRss = 0;
            const int nkUriRc =
                run({player, "--url", notakeyUri.string(), "--frames", "1"}, out, err, &nkUriRss);
            if (!d4MustStayCapped("NOTAKEY URI=", nkUriRc, nkUriRss, readFile(out), readFile(err),
                                  true)) {
                return 1;
            }
            long nkTypeRss = 0;
            const int nkTypeRc =
                run({player, "--url", notakeyType.string(), "--frames", "1"}, out, err, &nkTypeRss);
            if (!d4MustStayCapped("NOTAKEY TYPE=VIDEO", nkTypeRc, nkTypeRss, readFile(out),
                                  readFile(err), true)) {
                return 1;
            }
            long nkExtRss = 0;
            const int nkExtRc =
                run({player, "--url", notakeyExtinf.string(), "--frames", "1"}, out, err, &nkExtRss);
            if (!d4MustStayCapped("NOTAKEY URI= EXTINF media", nkExtRc, nkExtRss, readFile(out),
                                  readFile(err), false)) {
                return 1;
            }
            long overRss = 0;
            const int overRc =
                run({player, "--url", overlong.string(), "--frames", "1"}, out, err, &overRss);
            if (!d4MustStayCapped("overlong MEDIA line", overRc, overRss, readFile(out),
                                  readFile(err), true)) {
                return 1;
            }

            const std::filesystem::path chompSpace = tmp.path() / "d4-chomp-space.m3u8";
            const std::filesystem::path chompBs = tmp.path() / "d4-chomp-bs.m3u8";
            const std::filesystem::path chompLong = tmp.path() / "d4-chomp-overlong.m3u8";
            const std::filesystem::path inbandSmall = tmp.path() / "d4-inband-small.m3u8";
            const std::filesystem::path inbandHuge = tmp.path() / "d4-inband-huge.m3u8";
            {
                std::ofstream(chompSpace, std::ios::binary)
                    << "#EXTM3U\n#EXT-X-VERSION:4\n#EXT-X-MEDIA:TYPE=\"AUDIO \n"
                    << "#EXT-X-STREAM-INF:BANDWIDTH=400000\n"
                    << videoHls.filename().string() << "\n";
                std::ofstream(chompBs, std::ios::binary)
                    << "#EXTM3U\n#EXT-X-VERSION:4\n#EXT-X-MEDIA:TYPE=\"AUDIO\\\n"
                    << "#EXT-X-STREAM-INF:BANDWIDTH=400000\n"
                    << videoHls.filename().string() << "\n";
                std::string media =
                    "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",DEFAULT=YES,URI=\"";
                media += audioHls.filename().string();
                media += '"';
                if (media.size() < 4095) {
                    media.append(4095 - media.size(), ' ');
                }
                media += ",TYPE=VIDEO,URI=\"\"";
                std::string longBody = "#EXTM3U\n#EXT-X-VERSION:4\n";
                longBody += media;
                longBody += "\n#EXT-X-STREAM-INF:BANDWIDTH=400000,AUDIO=\"aud\"\n";
                longBody += videoHls.filename().string();
                longBody += '\n';
                std::ofstream(chompLong, std::ios::binary) << longBody;
                std::ofstream(inbandSmall)
                    << "#EXTM3U\n#EXT-X-VERSION:4\n"
                    << "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",DEFAULT=YES\n"
                    << "#EXT-X-STREAM-INF:BANDWIDTH=400000,AUDIO=\"aud\"\n"
                    << videoHls.filename().string() << "\n";
                std::ofstream(inbandHuge)
                    << "#EXTM3U\n#EXT-X-VERSION:4\n"
                    << "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",DEFAULT=YES,URI=\"\"\n"
                    << "#EXT-X-STREAM-INF:BANDWIDTH=400000,AUDIO=\"aud\"\n"
                    << audioHls.filename().string() << "\n";
                std::ifstream src(chompSpace);
                std::ofstream(tmp.path() / "chomp-http.m3u8") << src.rdbuf();
            }

            const std::filesystem::path id3HugeTs = tmp.path() / "d4-id3-huge.ts";
            const std::filesystem::path id3Media = tmp.path() / "d4-id3-media.m3u8";
            const std::filesystem::path id3Rewrite = tmp.path() / "d4-id3-rewrite.m3u8";
            const std::filesystem::path id3Variant = tmp.path() / "d4-id3-variant.m3u8";
            {
                std::ifstream in(hugeTs, std::ios::binary);
                std::ofstream out(id3HugeTs, std::ios::binary);
                out << id3v23HlsTimestampTag();
                out << in.rdbuf();
                std::ofstream(id3Media)
                    << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
                    << "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:0.040,\n"
                    << id3HugeTs.filename().string() << "\n#EXT-X-ENDLIST\n";
                std::ofstream(id3Rewrite)
                    << "#EXTM3U\n#EXT-X-VERSION:4\n"
                    << "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\",DEFAULT=YES,URI=\""
                    << audioHls.filename().string() << "\"\n"
                    << "#EXT-X-STREAM-INF:BANDWIDTH=400000,AUDIO=\"aud\"\n"
                    << id3Media.filename().string() << "\n";
                std::ofstream(id3Variant)
                    << "#EXTM3U\n#EXT-X-VERSION:4\n"
                    << "#EXT-X-STREAM-INF:BANDWIDTH=400000\n"
                    << id3Media.filename().string() << "\n";
                {
                    std::ifstream in(hugeTs, std::ios::binary);
                    std::ofstream out(tmp.path() / "d4-id3-upper.ts", std::ios::binary);
                    out << id3v23HlsTimestampTag("COM.APPLE.STREAMING.TRANSPORTSTREAMTIMESTAMP");
                    out << in.rdbuf();
                }
                std::ofstream(tmp.path() / "d4-id3-upper.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
                    << "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:0.040,\n"
                    << "d4-id3-upper.ts\n#EXT-X-ENDLIST\n";
                {
                    std::ifstream in(hugeTs, std::ios::binary);
                    std::ofstream out(tmp.path() / "d4-id3-second.ts", std::ios::binary);
                    out << id3v23Tit2Tag() << id3v23HlsTimestampTag();
                    out << in.rdbuf();
                }
                std::ofstream(tmp.path() / "d4-id3-second.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
                    << "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:0.040,\n"
                    << "d4-id3-second.ts\n#EXT-X-ENDLIST\n";
                {
                    std::ifstream in(hugeTs, std::ios::binary);
                    std::ofstream out(tmp.path() / "d4-id3-zlib.ts", std::ios::binary);
                    out << id3v23HlsTimestampTag("com.apple.streaming.transportStreamTimestamp", 0,
                                                0x80);
                    out << in.rdbuf();
                }
                std::ofstream(tmp.path() / "d4-id3-zlib.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
                    << "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:0.040,\n"
                    << "d4-id3-zlib.ts\n#EXT-X-ENDLIST\n";
                {
                    std::ifstream in(hugeTs, std::ios::binary);
                    std::ofstream out(tmp.path() / "d4-id3-late.ts", std::ios::binary);
                    out << id3v23HlsTimestampTag("com.apple.streaming.transportStreamTimestamp",
                                                5000);
                    out << in.rdbuf();
                }
                std::ofstream(tmp.path() / "d4-id3-late.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
                    << "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:0.040,\n"
                    << "d4-id3-late.ts\n#EXT-X-ENDLIST\n";
                {
                    std::ofstream out(tmp.path() / "d4-id3-range.ts", std::ios::binary);
                    out << std::string(1000, 'X');
                    out << id3v23HlsTimestampTag();
                    std::ifstream in(hugeTs, std::ios::binary);
                    out << in.rdbuf();
                }
                {
                    const uint64_t slice = id3v23HlsTimestampTag().size() +
                                           std::filesystem::file_size(hugeTs);
                    std::ofstream(tmp.path() / "d4-id3-range.m3u8")
                        << "#EXTM3U\n#EXT-X-VERSION:4\n#EXT-X-TARGETDURATION:1\n"
                        << "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:0.040,\n"
                        << "#EXT-X-BYTERANGE:" << slice << "@1000\n"
                        << "d4-id3-range.ts\n#EXT-X-ENDLIST\n";
                }
                std::ofstream(tmp.path() / "d4-id3-key.bin", std::ios::binary) << std::string(16, 'K');
                std::ofstream(tmp.path() / "d4-id3-aes.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
                    << "#EXT-X-KEY:METHOD=AES-128,URI=\"d4-id3-key.bin\"\n"
                    << "#EXTINF:0.040,\n"
                    << hugeTs.filename().string() << "\n#EXT-X-ENDLIST\n";
                std::ofstream(tmp.path() / "d4-id3-live.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
                    << "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:0.400,\n"
                    << smallTs.filename().string() << "\n#EXTINF:0.040,\n"
                    << id3HugeTs.filename().string() << "\n";
                std::ofstream(tmp.path() / "d4-id3-second-var.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:4\n"
                    << "#EXT-X-STREAM-INF:BANDWIDTH=400000\n"
                    << videoHls.filename().string() << "\n"
                    << "#EXT-X-STREAM-INF:BANDWIDTH=500000\n"
                    << id3Media.filename().string() << "\n";
                std::ofstream(tmp.path() / "d4-id3-nested-inner.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:4\n#EXT-X-STREAM-INF:BANDWIDTH=400000\n"
                    << id3Media.filename().string() << "\n";
                std::ofstream(tmp.path() / "d4-id3-nested-outer.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:4\n#EXT-X-STREAM-INF:BANDWIDTH=400000\n"
                    << "d4-id3-nested-inner.m3u8\n";
                {
                    std::ifstream in(hugeTs, std::ios::binary);
                    std::ofstream outz(tmp.path() / "d4-id3-zlib08.ts", std::ios::binary);
                    outz << id3v23ZlibPrivFlag08();
                    outz << in.rdbuf();
                }
                std::ofstream(tmp.path() / "d4-id3-zlib08.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
                    << "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:0.040,\n"
                    << "d4-id3-zlib08.ts\n#EXT-X-ENDLIST\n";
                std::ofstream(tmp.path() / "d4-id3-live4.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
                    << "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:0.400,\n"
                    << smallTs.filename().string() << "\n#EXTINF:0.040,\n"
                    << id3HugeTs.filename().string() << "\n#EXTINF:0.400,\n"
                    << smallTs.filename().string() << "\n#EXTINF:0.400,\n"
                    << smallTs.filename().string() << "\n";
                std::ofstream(tmp.path() / "d4-id3-live-var.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:4\n#EXT-X-STREAM-INF:BANDWIDTH=400000\n"
                    << "d4-id3-live4.m3u8\n";
                std::ofstream(tmp.path() / "d4-query.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:4\n#EXT-X-STREAM-INF:BANDWIDTH=400000\n"
                    << "d4-query.m3u8?x=1\n";
                std::filesystem::create_directories(tmp.path() / "dotdot");
                std::filesystem::create_directories(tmp.path() / "id3");
                std::ofstream(tmp.path() / "dotdot" / "master.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:4\n#EXT-X-STREAM-INF:BANDWIDTH=400000\n"
                    << "../id3/media.m3u8\n";
                std::ifstream id3src(id3Media);
                std::ofstream(tmp.path() / "id3" / "media.m3u8") << id3src.rdbuf();
                {
                    std::ifstream in(hugeTs, std::ios::binary);
                    std::ofstream outx(tmp.path() / "d4-id3-exthdr.ts", std::ios::binary);
                    outx << id3v23ExtHdrZlibPriv();
                    outx << in.rdbuf();
                }
                std::ofstream(tmp.path() / "d4-id3-exthdr.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
                    << "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:0.040,\n"
                    << "d4-id3-exthdr.ts\n#EXT-X-ENDLIST\n";
                {
                    std::ifstream in(hugeTs, std::ios::binary);
                    std::ofstream outx(tmp.path() / " video.ts", std::ios::binary);
                    outx << id3v23HlsTimestampTag();
                    outx << in.rdbuf();
                }
                {
                    std::ifstream in(smallTs, std::ios::binary);
                    std::ofstream(tmp.path() / "video.ts", std::ios::binary) << in.rdbuf();
                }
                std::ofstream(tmp.path() / "d4-id3-leadws.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
                    << "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:0.040,\n"
                    << " video.ts\n#EXT-X-ENDLIST\n";
                std::ofstream(tmp.path() / "d4-id3-media-video.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:4\n"
                    << "#EXT-X-MEDIA:TYPE=VIDEO,GROUP-ID=\"vid\",NAME=\"main\",DEFAULT=YES,URI=\""
                    << id3Media.filename().string() << "\"\n"
                    << "#EXT-X-STREAM-INF:BANDWIDTH=400000,VIDEO=\"vid\"\n"
                    << videoHls.filename().string() << "\n";
                std::ofstream(tmp.path() / "d4-id3-live5.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
                    << "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:0.400,\n"
                    << smallTs.filename().string() << "\n#EXTINF:0.400,\n"
                    << smallTs.filename().string() << "\n#EXTINF:0.400,\n"
                    << smallTs.filename().string() << "\n#EXTINF:0.400,\n"
                    << smallTs.filename().string() << "\n#EXTINF:0.400,\n"
                    << smallTs.filename().string() << "\n";
                std::ofstream(tmp.path() / "d4-id3-live4b.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
                    << "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:0.400,\n"
                    << smallTs.filename().string() << "\n#EXTINF:0.400,\n"
                    << smallTs.filename().string() << "\n#EXTINF:0.040,\n"
                    << id3HugeTs.filename().string() << "\n#EXTINF:0.400,\n"
                    << smallTs.filename().string() << "\n";
                std::ofstream(tmp.path() / "d4-id3-live-align.m3u8")
                    << "#EXTM3U\n#EXT-X-VERSION:4\n"
                    << "#EXT-X-STREAM-INF:BANDWIDTH=500000\n"
                    << "d4-id3-live5.m3u8\n"
                    << "#EXT-X-STREAM-INF:BANDWIDTH=400000\n"
                    << "d4-id3-live4b.m3u8\n";
            }
            long chompRss = 0;
            const int chompRc =
                run({player, "--url", chompSpace.string(), "--frames", "1"}, out, err, &chompRss);
            if (!d4MustStayCapped("chomp TYPE=AUDIO space", chompRc, chompRss, readFile(out),
                                  readFile(err), true)) {
                return 1;
            }
            long chompBsRss = 0;
            const int chompBsRc =
                run({player, "--url", chompBs.string(), "--frames", "1"}, out, err, &chompBsRss);
            if (!d4MustStayCapped("chomp TYPE=AUDIO backslash", chompBsRc, chompBsRss, readFile(out),
                                  readFile(err), true)) {
                return 1;
            }
            long chompLongRss = 0;
            const int chompLongRc =
                run({player, "--url", chompLong.string(), "--frames", "1"}, out, err, &chompLongRss);
            if (!d4MustStayCapped("chomp spaces to 4095", chompLongRc, chompLongRss, readFile(out),
                                  readFile(err), true)) {
                return 1;
            }
            long inSmallRss = 0;
            const int inSmallRc =
                run({player, "--url", inbandSmall.string(), "--frames", "1"}, out, err, &inSmallRss);
            if (!d4MustStayCapped("in-band AUDIO small variant", inSmallRc, inSmallRss,
                                  readFile(out), readFile(err), true)) {
                return 1;
            }
            long inHugeRss = 0;
            const int inHugeRc =
                run({player, "--url", inbandHuge.string(), "--frames", "1"}, out, err, &inHugeRss);
            if (!d4MustStayCapped("in-band AUDIO huge variant", inHugeRc, inHugeRss, readFile(out),
                                  readFile(err), false)) {
                return 1;
            }

            long id3Rss = 0;
            const int id3Rc =
                run({player, "--url", id3Media.string(), "--frames", "1"}, out, err, &id3Rss);
            if (!d4MustStayCapped("ID3-timestamped huge media", id3Rc, id3Rss, readFile(out),
                                  readFile(err), false)) {
                return 1;
            }
            long id3RwRss = 0;
            const int id3RwRc =
                run({player, "--url", id3Rewrite.string(), "--frames", "1"}, out, err, &id3RwRss);
            if (!d4MustStayCapped("rewrite to ID3-timestamped media", id3RwRc, id3RwRss,
                                  readFile(out), readFile(err), false)) {
                return 1;
            }
            long id3VarRss = 0;
            const int id3VarRc =
                run({player, "--url", id3Variant.string(), "--frames", "1"}, out, err, &id3VarRss);
            if (!d4MustStayCapped("STREAM-INF ID3-timestamped variant", id3VarRc, id3VarRss,
                                  readFile(out), readFile(err), false)) {
                return 1;
            }
            const char* id3Extra[][2] = {
                {"d4-id3-upper.m3u8", "ID3 uppercase owner"},
                {"d4-id3-second.m3u8", "second ID3 tag PRIV"},
                {"d4-id3-zlib.m3u8", "zlib-compressed PRIV"},
                {"d4-id3-late.m3u8", "ID3 PRIV past 4096"},
                {"d4-id3-range.m3u8", "BYTERANGE ID3 segment"},
                {"d4-id3-aes.m3u8", "AES-128 first segment"},
                {"d4-id3-live.m3u8", "live playlist ID3 segment 1"},
                {"d4-id3-second-var.m3u8", "second STREAM-INF ID3"},
                {"d4-id3-nested-outer.m3u8", "nested master ID3"},
                {"d4-id3-zlib08.m3u8", "ID3v2.3 compression flag 0x08"},
                {"d4-id3-live-var.m3u8", "live STREAM-INF ID3 live_start_index"},
                {"d4-id3-exthdr.m3u8", "ID3v2.3 extended header 0x40"},
                {"d4-id3-leadws.m3u8", "leading-space segment URI"},
                {"d4-id3-media-video.m3u8", "EXT-X-MEDIA TYPE=VIDEO ID3"},
                {"d4-id3-live-align.m3u8", "two live variants ID3 shorter index 2"},
            };
            for (const auto& extra : id3Extra) {
                long extraRss = 0;
                const int extraRc = run({player, "--url", (tmp.path() / extra[0]).string(),
                                         "--frames", "1"},
                                        out, err, &extraRss);
                if (!d4MustStayCapped(extra[1], extraRc, extraRss, readFile(out), readFile(err),
                                      false)) {
                    return 1;
                }
            }

            // Extensionless HTTP URL, Content-Type mpegurl. The first peek
            // only ran for .m3u8 / hls+.
            const std::filesystem::path extless = tmp.path() / "playlist";
            {
                std::ifstream src(masterAudio);
                std::ofstream dst(extless);
                dst << src.rdbuf();
            }
            LocalHttp d4http = startDirServer(tmp.path());
            if (d4http.pid <= 0 || d4http.port <= 0) {
                std::cerr << "D4 extensionless playlist server failed to start\n";
                return 1;
            }
            const std::string httpBase = "http://127.0.0.1:" + std::to_string(d4http.port);
            const std::string extlessUrl = httpBase + "/playlist";
            long extlessRss = 0;
            const int extlessRc =
                run({player, "--url", extlessUrl, "--frames", "1"}, out, err, &extlessRss);
            if (!d4MustStayCapped("extensionless HTTP", extlessRc, extlessRss, readFile(out),
                                  readFile(err), true)) {
                d4http.stop();
                return 1;
            }

            long tsRss = 0;
            const int tsRc =
                run({player, "--url", httpBase + "/playlist.ts", "--frames", "1"}, out, err, &tsRss);
            if (!d4MustStayCapped("playlist.ts mpegurl", tsRc, tsRss, readFile(out), readFile(err),
                                  true)) {
                d4http.stop();
                return 1;
            }

            long redirRss = 0;
            const int redirRc =
                run({player, "--url", httpBase + "/seg.ts", "--frames", "1"}, out, err, &redirRss);
            if (!d4MustStayCapped("seg.ts redirect", redirRc, redirRss, readFile(out),
                                  readFile(err), true)) {
                d4http.stop();
                return 1;
            }

            long truncRss = 0;
            const int truncRc =
                run({player, "--url", httpBase + "/trunc.m3u8", "--frames", "1"}, out, err,
                    &truncRss);
            if (!d4MustStayCapped("truncated EXTM3U preview", truncRc, truncRss, readFile(out),
                                  readFile(err), false)) {
                d4http.stop();
                return 1;
            }

            long failRss = 0;
            const int failRc =
                run({player, "--url", httpBase + "/failpeek.m3u8", "--frames", "1"}, out, err,
                    &failRss);
            if (!d4MustStayCapped("http peek failure", failRc, failRss, readFile(out),
                                  readFile(err), false)) {
                d4http.stop();
                return 1;
            }

            long garbageRss = 0;
            const int garbageRc =
                run({player, "--url", httpBase + "/garbage.m3u8", "--frames", "1"}, out, err,
                    &garbageRss);
            if (!d4MustStayCapped("mpegurl body X", garbageRc, garbageRss, readFile(out),
                                  readFile(err), false)) {
                d4http.stop();
                return 1;
            }

            long nkHttpRss = 0;
            const int nkHttpRc =
                run({player, "--url", httpBase + "/notakey-http.m3u8", "--frames", "1"}, out, err,
                    &nkHttpRss);
            if (!d4MustStayCapped("NOTAKEY URI= HTTP", nkHttpRc, nkHttpRss, readFile(out),
                                  readFile(err), true)) {
                d4http.stop();
                return 1;
            }

            long partialRss = 0;
            const int partialRc =
                run({player, "--url", httpBase + "/partial.m3u8", "--frames", "1"}, out, err,
                    &partialRss);
            if (!d4MustStayCapped("partial peek reset", partialRc, partialRss, readFile(out),
                                  readFile(err), false)) {
                d4http.stop();
                return 1;
            }

            long chompHttpRss = 0;
            const int chompHttpRc =
                run({player, "--url", httpBase + "/chomp-http.m3u8", "--frames", "1"}, out, err,
                    &chompHttpRss);
            if (!d4MustStayCapped("chomp TYPE=AUDIO space HTTP", chompHttpRc, chompHttpRss,
                                  readFile(out), readFile(err), true)) {
                d4http.stop();
                return 1;
            }

            long decoyRss = 0;
            const int decoyRc =
                run({player, "--url", httpBase + "/decoy256", "--frames", "1"}, out, err, &decoyRss);
            if (!d4MustStayCapped("256-byte video/mp2t reset", decoyRc, decoyRss, readFile(out),
                                  readFile(err), false)) {
                d4http.stop();
                return 1;
            }

            long hopRss = 0;
            const int hopRc =
                run({player, "--url", httpBase + "/hop.m3u8", "--frames", "1"}, out, err, &hopRss);
            if (!d4MustStayCapped("302 decoy then master", hopRc, hopRss, readFile(out),
                                  readFile(err), false)) {
                d4http.stop();
                return 1;
            }

            {
                std::ifstream src(masterAudio);
                std::ofstream(tmp.path() / "decoyts") << src.rdbuf();
                std::ifstream src2(masterAudio);
                std::ofstream(tmp.path() / "decoyftyp") << src2.rdbuf();
                std::ifstream src3(masterAudio);
                std::ofstream(tmp.path() / "decoyfin") << src3.rdbuf();
            }

            long decoyTsRss = 0;
            const int decoyTsRc =
                run({player, "--url", httpBase + "/decoyts", "--frames", "1"}, out, err,
                    &decoyTsRss);
            if (!d4MustStayCapped("TS sync prefix reset", decoyTsRc, decoyTsRss, readFile(out),
                                  readFile(err), false)) {
                d4http.stop();
                return 1;
            }
            long decoyFtypRss = 0;
            const int decoyFtypRc =
                run({player, "--url", httpBase + "/decoyftyp", "--frames", "1"}, out, err,
                    &decoyFtypRss);
            if (!d4MustStayCapped("ftyp prefix reset", decoyFtypRc, decoyFtypRss, readFile(out),
                                  readFile(err), false)) {
                d4http.stop();
                return 1;
            }
            long decoyFinRss = 0;
            const int decoyFinRc =
                run({player, "--url", httpBase + "/decoyfin", "--frames", "1"}, out, err,
                    &decoyFinRss);
            if (!d4MustStayCapped("finished small TS reset", decoyFinRc, decoyFinRss, readFile(out),
                                  readFile(err), false)) {
                d4http.stop();
                return 1;
            }
            long hopTsRss = 0;
            const int hopTsRc =
                run({player, "--url", httpBase + "/hop-ts.m3u8", "--frames", "1"}, out, err,
                    &hopTsRss);
            if (!d4MustStayCapped("302 TS decoy then master", hopTsRc, hopTsRss, readFile(out),
                                  readFile(err), false)) {
                d4http.stop();
                return 1;
            }
            long hopFtypRss = 0;
            const int hopFtypRc =
                run({player, "--url", httpBase + "/hop-ftyp.m3u8", "--frames", "1"}, out, err,
                    &hopFtypRss);
            if (!d4MustStayCapped("302 ftyp decoy then master", hopFtypRc, hopFtypRss, readFile(out),
                                  readFile(err), false)) {
                d4http.stop();
                return 1;
            }
            long hopFinRss = 0;
            const int hopFinRc =
                run({player, "--url", httpBase + "/hop-fin.m3u8", "--frames", "1"}, out, err,
                    &hopFinRss);
            if (!d4MustStayCapped("302 finished-TS decoy then master", hopFinRc, hopFinRss,
                                  readFile(out), readFile(err), false)) {
                d4http.stop();
                return 1;
            }

            {
                std::ifstream src(masterAudio);
                std::ofstream(tmp.path() / "fin-sync") << src.rdbuf();
                std::ifstream src2(masterAudio);
                std::ofstream(tmp.path() / "fin-ftyp") << src2.rdbuf();
                std::ifstream src3(masterAudio);
                std::ofstream(tmp.path() / "fin-small") << src3.rdbuf();
            }
            long finSyncRss = 0;
            const int finSyncRc =
                run({player, "--url", httpBase + "/fin-sync", "--frames", "1"}, out, err,
                    &finSyncRss);
            if (!d4MustStayCapped("finished sync exact-CL then master", finSyncRc, finSyncRss,
                                  readFile(out), readFile(err), false)) {
                d4http.stop();
                return 1;
            }
            long finFtypRss = 0;
            const int finFtypRc =
                run({player, "--url", httpBase + "/fin-ftyp", "--frames", "1"}, out, err,
                    &finFtypRss);
            if (!d4MustStayCapped("finished ftyp exact-CL then master", finFtypRc, finFtypRss,
                                  readFile(out), readFile(err), false)) {
                d4http.stop();
                return 1;
            }
            long finSmallRss = 0;
            const int finSmallRc =
                run({player, "--url", httpBase + "/fin-small", "--frames", "1"}, out, err,
                    &finSmallRss);
            if (!d4MustStayCapped("finished small TS exact-CL then master", finSmallRc, finSmallRss,
                                  readFile(out), readFile(err), false)) {
                d4http.stop();
                return 1;
            }
            long queryRss = 0;
            const int queryRc =
                run({player, "--url", httpBase + "/d4-query.m3u8", "--frames", "1"}, out, err,
                    &queryRss);
            if (!d4MustStayCapped("query-stripped STREAM-INF ID3", queryRc, queryRss, readFile(out),
                                  readFile(err), false)) {
                d4http.stop();
                return 1;
            }
            long dotRss = 0;
            const int dotRc =
                run({player, "--url", httpBase + "/dotdot/master.m3u8", "--frames", "1"}, out, err,
                    &dotRss);
            if (!d4MustStayCapped("relative .. STREAM-INF ID3", dotRc, dotRss, readFile(out),
                                  readFile(err), false)) {
                d4http.stop();
                return 1;
            }
            long flipTsRss = 0;
            const int flipTsRc =
                run({player, "--url", httpBase + "/flip.ts", "--frames", "1"}, out, err, &flipTsRss);
            if (!d4MustStayCapped(".ts finished then master Content-Type flip", flipTsRc, flipTsRss,
                                  readFile(out), readFile(err), false)) {
                d4http.stop();
                return 1;
            }
            long flipMp4Rss = 0;
            const int flipMp4Rc =
                run({player, "--url", httpBase + "/flip.mp4", "--frames", "1"}, out, err,
                    &flipMp4Rss);
            if (!d4MustStayCapped(".mp4 ftyp then master Content-Type flip", flipMp4Rc, flipMp4Rss,
                                  readFile(out), readFile(err), false)) {
                d4http.stop();
                return 1;
            }
            long flipHopRss = 0;
            const int flipHopRc =
                run({player, "--url", httpBase + "/flip-hop.ts", "--frames", "1"}, out, err,
                    &flipHopRss);
            d4http.stop();
            if (!d4MustStayCapped(".ts 302 finished TS then master", flipHopRc, flipHopRss,
                                  readFile(out), readFile(err), false)) {
                return 1;
            }
        }

        // AUDIT9 D1: a 12000x12000 MJPEG still must fail without the ~425 MB convert.
        {
            const std::filesystem::path hugeMp4 = tmp.path() / "huge.mp4";
            const int genHuge = run({ffmpegBinary(), "-y", "-loglevel", "error", "-f", "lavfi", "-i",
                                     "color=c=red:s=12000x12000:d=0.04", "-frames:v", "1", "-c:v",
                                     "mjpeg", "-q:v", "31", hugeMp4.string()},
                                    empty, log);
            if (genHuge != 0) {
                std::cerr << "failed to generate 12000x12000 fixture (exit " << genHuge << ")\n"
                          << readFile(log);
                return 1;
            }
            long maxRssKb = 0;
            const int hugeRc =
                run({player, "--url", hugeMp4.string(), "--frames", "1"}, out, err, &maxRssKb);
            const std::string hugeOut = readFile(out);
            const std::string hugeErr = readFile(err);
            constexpr long kConvertRssKb = 300000;
            if (hugeRc == 0 || hugeOut.find("presented ") != std::string::npos ||
                hugeOut.find("Decoded ") != std::string::npos ||
                hugeOut.find("layout ") != std::string::npos ||
                hugeOut.find("-> yuv420p") != std::string::npos || maxRssKb >= kConvertRssKb) {
                std::cerr << "oversize still --url must fail before convert (exit " << hugeRc
                          << " rss_kb=" << maxRssKb << ")\n"
                          << hugeOut << hugeErr;
                return 1;
            }
        }

        // AUDIT7 B2: --url of an HTTP playlist whose only segment is file:///…
        // must reject the local file, same as MQTT add (flag stays off).
        // hls+http:// is on the protocol whitelist and must not inherit file.
        {
            std::ostringstream playlist;
            playlist << "#EXTM3U\n"
                     << "#EXT-X-VERSION:3\n"
                     << "#EXT-X-TARGETDURATION:1\n"
                     << "#EXT-X-MEDIA-SEQUENCE:0\n"
                     << "#EXTINF:0.400,\n"
                     << "file://" << mp4.string() << "\n"
                     << "#EXT-X-ENDLIST\n";
            LocalHttp server = startPlaylistServer(playlist.str());
            if (server.pid <= 0 || server.port <= 0) {
                std::cerr << "file-segment playlist server failed to start\n";
                return 1;
            }
            const std::string hostPath =
                "127.0.0.1:" + std::to_string(server.port) + "/file_seg.m3u8";
            const char* prefixes[] = {"http://", "hls+http://"};
            for (const char* prefix : prefixes) {
                const std::string evilUrl = std::string(prefix) + hostPath;
                const int leaked = run({player, "--url", evilUrl, "--frames", "1"}, out, err);
                const std::string leakOut = readFile(out);
                const std::string leakErr = readFile(err);
                if (leaked == 0 || leakOut.find("Decoded ") != std::string::npos ||
                    leakOut.find("fmt=yuv420p") != std::string::npos) {
                    server.stop();
                    std::cerr << prefix << " --url with file:// segment must be rejected (exit "
                              << leaked << ")\n"
                              << leakOut << leakErr;
                    return 1;
                }
            }
            server.stop();
        }

        const int fileScheme = run({player, "--url", "file://" + mp4.string(), "--frames", "1"}, out, err);
        if (fileScheme != 0) {
            std::cerr << "file:// --url of a local fixture should still present (exit " << fileScheme
                      << ")\n"
                      << readFile(out) << readFile(err);
            return 1;
        }

        const std::filesystem::path drmOut = tmp.path() / "drm.out";
        const std::filesystem::path drmErr = tmp.path() / "drm.err";
        const int drm = run({player, "--renderer", "drm", "--url", mp4.string(), "--frames", "1"}, drmOut, drmErr);
        const std::string drmErrText = readFile(drmErr);
        if (drm == 0 || drmErrText.find("requires GS-King X / CoreELEC") == std::string::npos) {
            std::cerr << "drm presenter should fail closed without a GS-King X (exit " << drm << ")\n"
                      << readFile(drmOut) << drmErrText;
            return 1;
        }

        const int help = run({player, "--help", "--url", "example"}, out, err);
        const std::string helpText = readFile(out) + readFile(err);
        if (help != 0 || helpText.find("--render-dir") == std::string::npos) {
            std::cerr << "decode help missing --render-dir (exit " << help << ")\n" << helpText;
            return 1;
        }

        const int rendererOnly = run({player, "--renderer", "software"}, out, err);
        if (rendererOnly != 2) {
            std::cerr << "--renderer without --url should exit 2, got " << rendererOnly << "\n"
                      << readFile(out) << readFile(err);
            return 1;
        }

        const int badViewport = run({player, "--viewport", "nope"}, out, err);
        const std::string viewportText = readFile(out) + readFile(err);
        if (badViewport != 1 || viewportText.find("Invalid --viewport") == std::string::npos ||
            viewportText.find("MQTT connected") != std::string::npos) {
            std::cerr << "--viewport nope should fail before connect (exit " << badViewport << ")\n"
                      << viewportText;
            return 1;
        }

        const int urlViewport =
            run({player, "--url", mp4.string(), "--viewport", "160x120", "--frames", "1"}, out, err);
        if (urlViewport != 2) {
            std::cerr << "--viewport is not a --url flag (exit " << urlViewport << ")\n"
                      << readFile(out) << readFile(err);
            return 1;
        }

        const int mqttHelp = run({player, "--help"}, out, err);
        const std::string mqttText = readFile(out) + readFile(err);
        if (mqttHelp != 0 || mqttText.find("MQTT") == std::string::npos ||
            mqttText.find("--viewport") == std::string::npos ||
            mqttText.find("--render-dir") == std::string::npos) {
            std::cerr << "default --help should stay on the MQTT path (exit " << mqttHelp << ")\n" << mqttText;
            return 1;
        }

        std::cout << "present_smoke_test ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL exception " << ex.what() << "\n";
        return 1;
    }
}
