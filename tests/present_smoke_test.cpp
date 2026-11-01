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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
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

std::string requestPath(const std::string& headers) {
    const auto lineEnd = headers.find("\r\n");
    const std::string line = headers.substr(0, lineEnd == std::string::npos ? headers.size() : lineEnd);
    if (line.compare(0, 4, "GET ") != 0) {
        return {};
    }
    const auto sp = line.find(' ', 4);
    if (sp == std::string::npos) {
        return {};
    }
    std::string url = line.substr(4, sp - 4);
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
