#include "ffmpeg_decoder.h"
#include "software_renderer.h"
#include "stream_manager.h"
#include "stream_player.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// Broker-free smoke test: one local HTTP file, StreamPlayer, FfmpegDecoder,
// SoftwareRenderer. No MQTT socket. Exit 77 when the ffmpeg CLI is missing.

#ifndef FFMPEG_CLI
#define FFMPEG_CLI ""
#endif

namespace {

constexpr int kSkip = 77;

class TempDir {
public:
    TempDir()
        : path_(std::filesystem::temp_directory_path() /
                ("amlogic-stream-player-http-" + std::to_string(static_cast<long long>(::getpid())))) {
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

int run(const std::vector<std::string>& args, const std::filesystem::path& errFile) {
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
        const int fd = ::open(errFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
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
        const std::string headers = readRequest(client);
        const std::string path = requestPath(headers);
        if (path != "/clip.ts") {
            const char* missing = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            sendAll(client, missing, std::char_traits<char>::length(missing));
            ::close(client);
            continue;
        }
        std::ostringstream hdr;
        hdr << "HTTP/1.1 200 OK\r\nContent-Length: " << body.size()
            << "\r\nContent-Type: video/mp2t\r\nConnection: close\r\n\r\n";
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

LocalHttp startServer(const std::string& body) {
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

bool waitForFrames(StreamPlayer& player, SoftwareRenderer& renderer, int target, int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (renderer.framesPresented() < target && std::chrono::steady_clock::now() < deadline) {
        player.tick(true);
    }
    return renderer.framesPresented() >= target;
}

}  // namespace

int main() {
    try {
        TempDir tmp;
        const auto clip = tmp.path() / "clip.ts";
        const auto log = tmp.path() / "ffmpeg.log";
        const int gen = run({ffmpegBinary(), "-y", "-loglevel", "error", "-f", "lavfi", "-i",
                             "testsrc=size=160x120:rate=10:duration=0.4", "-c:v", "mpeg4", "-f", "mpegts",
                             clip.string()},
                            log);
        if (gen == 127) {
            std::cout << "SKIP ffmpeg CLI not found\n";
            return kSkip;
        }
        if (gen != 0) {
            std::cerr << "failed to generate fixture (exit " << gen << ")\n" << readFile(log);
            return 1;
        }

        std::ifstream in(clip, std::ios::binary);
        std::ostringstream bodyStream;
        bodyStream << in.rdbuf();
        const std::string body = bodyStream.str();
        if (body.size() < 64) {
            std::cerr << "fixture is empty\n";
            return 1;
        }

        LocalHttp server = startServer(body);
        if (server.pid <= 0 || server.port <= 0) {
            std::cerr << "local http server failed to start\n";
            return 1;
        }

        const std::string url = "http://127.0.0.1:" + std::to_string(server.port) + "/clip.ts";
        StreamManager streams;
        SoftwareRenderer renderer;
        StreamPlayer player(
            streams, renderer,
            []() -> std::unique_ptr<DecoderBackend> { return std::make_unique<FfmpegDecoder>(); }, 160, 120);
        if (!player.init()) {
            std::cerr << "renderer init failed\n";
            return 1;
        }
        if (!streams.addStream("cam1", url)) {
            std::cerr << "addStream failed\n";
            return 1;
        }

        player.tick(false);
        if (renderer.framesPresented() != 0) {
            std::cerr << "stopped player presented a frame\n";
            return 1;
        }
        if (!waitForFrames(player, renderer, 1, 20)) {
            std::cerr << "no frame presented from " << url << "\n";
            return 1;
        }
        bool painted = false;
        for (uint8_t byte : renderer.pixels()) {
            if (byte != 0) {
                painted = true;
                break;
            }
        }
        if (!painted || renderer.viewportWidth() != 160 || renderer.viewportHeight() != 120 ||
            !renderer.lastPpmPath().empty()) {
            std::cerr << "software present did not fill the viewport\n";
            return 1;
        }

        if (streams.restartStream("typo") || streams.list()[0].restartGeneration != 0) {
            std::cerr << "unknown restart changed the live stream\n";
            return 1;
        }
        const int before = renderer.framesPresented();
        if (!streams.restartStream("cam1") || streams.list()[0].restartGeneration != 1) {
            std::cerr << "restart did not bump generation\n";
            return 1;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (renderer.framesPresented() <= before && std::chrono::steady_clock::now() < deadline) {
            player.tick(true);
        }
        if (renderer.framesPresented() <= before) {
            std::cerr << "restart did not present another frame\n";
            return 1;
        }

        const int frozen = renderer.framesPresented();
        player.tick(false);
        player.tick(false);
        if (renderer.framesPresented() != frozen) {
            std::cerr << "stop still presented\n";
            return 1;
        }
        for (uint8_t byte : renderer.pixels()) {
            if (byte != 0) {
                std::cerr << "stop left a stale viewport\n";
                return 1;
            }
        }

        std::cout << "stream player http test passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL exception " << ex.what() << "\n";
        return 1;
    }
}
