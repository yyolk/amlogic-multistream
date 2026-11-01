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

#include <cerrno>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// SURVIVING3 B1: one blackhole or trickle URL, and a live HLS edge, must not
// freeze the other tiles or hold close/shutdown. Exit 77 when the ffmpeg
// CLI is missing.

#ifndef FFMPEG_CLI
#define FFMPEG_CLI ""
#endif

namespace {

constexpr int kSkip = 77;
constexpr int kShutdownBoundMs = 2000;

class TempDir {
public:
    TempDir()
        : path_(std::filesystem::temp_directory_path() /
                ("amlogic-stream-stall-" + std::to_string(static_cast<long long>(::getpid())))) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

struct Child {
    pid_t pid = -1;

    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&& other) noexcept : pid(other.pid) { other.pid = -1; }
    Child& operator=(Child&& other) noexcept {
        if (this != &other) {
            stop();
            pid = other.pid;
            other.pid = -1;
        }
        return *this;
    }
    ~Child() { stop(); }

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

std::string ffmpegBinary() {
    const char* configured = FFMPEG_CLI;
    if (configured != nullptr && configured[0] != '\0') {
        return configured;
    }
    return "ffmpeg";
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
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
        if (::poll(&pfd, 1, 500) <= 0) {
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

int listenLoopback(int& port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    const int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 16) != 0) {
        ::close(fd);
        return -1;
    }
    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
        ::close(fd);
        return -1;
    }
    port = ntohs(bound.sin_port);
    return fd;
}

[[noreturn]] void serveBody(int listenFd, std::string body) {
    ::signal(SIGPIPE, SIG_IGN);
    while (true) {
        const int client = ::accept(listenFd, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            _exit(0);
        }
        readRequest(client);
        std::ostringstream hdr;
        hdr << "HTTP/1.1 200 OK\r\nContent-Length: " << body.size()
            << "\r\nContent-Type: video/mp2t\r\nConnection: close\r\n\r\n";
        const std::string header = hdr.str();
        sendAll(client, header.data(), header.size());
        sendAll(client, body.data(), body.size());
        ::close(client);
    }
}

[[noreturn]] void serveBlackhole(int listenFd) {
    ::signal(SIGPIPE, SIG_IGN);
    while (true) {
        const int client = ::accept(listenFd, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            _exit(0);
        }
        readRequest(client);
        for (;;) {
            ::pause();
        }
    }
}

[[noreturn]] void serveTrickle(int listenFd) {
    ::signal(SIGPIPE, SIG_IGN);
    while (true) {
        const int client = ::accept(listenFd, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            _exit(0);
        }
        readRequest(client);
        const char* hdr =
            "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nConnection: close\r\n\r\n";
        sendAll(client, hdr, std::char_traits<char>::length(hdr));
        for (;;) {
            const char byte = 0x47;
            sendAll(client, &byte, 1);
            ::sleep(5);
        }
    }
}

[[noreturn]] void serveDir(int listenFd, std::filesystem::path root) {
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
        std::string path = "/";
        const auto lineEnd = headers.find("\r\n");
        const std::string line = headers.substr(0, lineEnd == std::string::npos ? headers.size() : lineEnd);
        if (line.compare(0, 4, "GET ") == 0) {
            const auto sp = line.find(' ', 4);
            if (sp != std::string::npos) {
                path = line.substr(4, sp - 4);
                const auto query = path.find('?');
                if (query != std::string::npos) {
                    path = path.substr(0, query);
                }
            }
        }
        std::filesystem::path rel;
        bool safe = !path.empty() && path[0] == '/';
        if (safe) {
            rel = std::filesystem::path(path.substr(1));
            for (const std::filesystem::path& part : rel) {
                if (part == "..") {
                    safe = false;
                    break;
                }
            }
        }
        std::string body;
        if (safe) {
            std::ifstream in(root / rel, std::ios::binary);
            if (!in) {
                safe = false;
            } else {
                std::ostringstream ss;
                ss << in.rdbuf();
                body = ss.str();
            }
        }
        const char* type = "application/octet-stream";
        if (rel.extension() == ".m3u8") {
            type = "application/vnd.apple.mpegurl";
        } else if (rel.extension() == ".ts") {
            type = "video/mp2t";
        }
        std::ostringstream hdr;
        if (!safe) {
            body = "missing\n";
            hdr << "HTTP/1.1 404 Not Found\r\n";
            type = "text/plain";
        } else {
            hdr << "HTTP/1.1 200 OK\r\n";
        }
        hdr << "Content-Length: " << body.size() << "\r\nContent-Type: " << type
            << "\r\nConnection: close\r\n\r\n";
        const std::string header = hdr.str();
        sendAll(client, header.data(), header.size());
        sendAll(client, body.data(), body.size());
        ::close(client);
    }
}

Child forkServe(int listenFd, void (*fn)(int)) {
    Child child;
    const pid_t pid = fork();
    if (pid < 0) {
        ::close(listenFd);
        return child;
    }
    if (pid == 0) {
        fn(listenFd);
    }
    ::close(listenFd);
    child.pid = pid;
    return child;
}

Child forkBody(int listenFd, const std::string& body) {
    Child child;
    const pid_t pid = fork();
    if (pid < 0) {
        ::close(listenFd);
        return child;
    }
    if (pid == 0) {
        serveBody(listenFd, body);
    }
    ::close(listenFd);
    child.pid = pid;
    return child;
}

Child forkDir(int listenFd, const std::filesystem::path& root) {
    Child child;
    const pid_t pid = fork();
    if (pid < 0) {
        ::close(listenFd);
        return child;
    }
    if (pid == 0) {
        serveDir(listenFd, root);
    }
    ::close(listenFd);
    child.pid = pid;
    return child;
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
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

Child spawn(const std::vector<std::string>& args, const std::filesystem::path& errFile) {
    Child child;
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (const std::string& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) {
        return child;
    }
    if (pid == 0) {
        const int fd = ::open(errFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDERR_FILENO);
            dup2(fd, STDOUT_FILENO);
            if (fd != STDERR_FILENO) {
                ::close(fd);
            }
        }
        execvp(argv[0], argv.data());
        _exit(127);
    }
    child.pid = pid;
    return child;
}

struct Sample {
    int iterations = 0;
    int presents = 0;
    int maxTickMs = 0;
    int elapsedMs = 0;
};

Sample sampleTicks(StreamPlayer& player, std::chrono::milliseconds window) {
    Sample sample;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < window) {
        const auto tickStart = std::chrono::steady_clock::now();
        sample.presents += player.tick(true);
        const auto tickMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - tickStart)
                                .count();
        if (tickMs > sample.maxTickMs) {
            sample.maxTickMs = static_cast<int>(tickMs);
        }
        ++sample.iterations;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    sample.elapsedMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count());
    return sample;
}

int shutdownMs(StreamPlayer& player) {
    const auto start = std::chrono::steady_clock::now();
    // Stop must return without joining an in-flight open. drain() then waits
    // only until abort() has unblocked the worker, which a socket stall does
    // and a stuck getaddrinfo does not.
    player.tick(false);
    if (!player.drain(std::chrono::milliseconds(kShutdownBoundMs))) {
        return kShutdownBoundMs + 1;
    }
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count());
}

bool waitForPresent(StreamPlayer& player, SoftwareRenderer& renderer, int seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (renderer.framesPresented() < 1 && std::chrono::steady_clock::now() < deadline) {
        player.tick(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return renderer.framesPresented() >= 1;
}

}  // namespace

int main() {
    try {
        TempDir tmp;
        const auto clip = tmp.path() / "clip.ts";
        const auto log = tmp.path() / "ffmpeg.log";
        const int gen = run({ffmpegBinary(), "-y", "-loglevel", "error", "-f", "lavfi", "-i",
                             "testsrc=size=160x120:rate=10:duration=2", "-c:v", "mpeg4", "-f", "mpegts",
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
        const std::string body = readFile(clip);
        if (body.size() < 64) {
            std::cerr << "fixture is empty\n";
            return 1;
        }

        int healthyPort = 0;
        int blackPort = 0;
        int tricklePort = 0;
        const int healthyFd = listenLoopback(healthyPort);
        const int blackFd = listenLoopback(blackPort);
        const int trickleFd = listenLoopback(tricklePort);
        if (healthyFd < 0 || blackFd < 0 || trickleFd < 0) {
            std::cerr << "listen failed\n";
            return 1;
        }
        Child healthy = forkBody(healthyFd, body);
        Child blackhole = forkServe(blackFd, serveBlackhole);
        Child trickle = forkServe(trickleFd, serveTrickle);
        if (healthy.pid <= 0 || blackhole.pid <= 0 || trickle.pid <= 0) {
            std::cerr << "server fork failed\n";
            return 1;
        }

        StreamManager streams;
        SoftwareRenderer renderer;
        StreamPlayer player(
            streams, renderer,
            []() -> std::unique_ptr<DecoderBackend> { return std::make_unique<FfmpegDecoder>(); }, 160, 120);
        if (!player.init()) {
            std::cerr << "renderer init failed\n";
            return 1;
        }
        const std::string healthyUrl = "http://127.0.0.1:" + std::to_string(healthyPort) + "/clip.ts";
        if (!streams.addStream("cam", healthyUrl) || !waitForPresent(player, renderer, 15)) {
            std::cerr << "healthy stream did not present\n";
            return 1;
        }
        if (!streams.addStream("black", "http://127.0.0.1:" + std::to_string(blackPort) + "/stall.ts") ||
            !streams.addStream("trickle", "http://127.0.0.1:" + std::to_string(tricklePort) + "/drip.ts")) {
            std::cerr << "failed to add stalled streams\n";
            return 1;
        }

        const Sample stalled = sampleTicks(player, std::chrono::milliseconds(2500));
        std::cout << "blackhole+trickle iterations=" << stalled.iterations
                  << " presents=" << stalled.presents << " maxTickMs=" << stalled.maxTickMs
                  << " elapsedMs=" << stalled.elapsedMs << "\n";
        if (stalled.iterations < 25 || stalled.presents < 8 || stalled.maxTickMs > 800) {
            std::cerr << "stalled neighbors froze the healthy stream\n";
            return 1;
        }
        const int shutStalled = shutdownMs(player);
        std::cout << "blackhole+trickle shutdownMs=" << shutStalled << "\n";
        if (shutStalled > kShutdownBoundMs) {
            std::cerr << "shutdown blocked on a stalled stream (" << shutStalled << " ms)\n";
            return 1;
        }

        // Live HLS at its edge next to VOD. libx264 is optional; the blackhole
        // case above is the required regression.
        const auto liveDir = tmp.path() / "live";
        std::filesystem::create_directories(liveDir);
        const auto liveLog = tmp.path() / "live-ffmpeg.log";
        Child liveEnc = spawn({ffmpegBinary(), "-loglevel", "error", "-re", "-f", "lavfi", "-i",
                               "testsrc=size=160x120:rate=10", "-c:v", "libx264", "-preset", "ultrafast",
                               "-tune", "zerolatency", "-pix_fmt", "yuv420p", "-g", "10", "-f", "hls",
                               "-hls_time", "1", "-hls_list_size", "3", "-hls_flags",
                               "delete_segments+omit_endlist", "-hls_segment_filename",
                               (liveDir / "seg%d.ts").string(), (liveDir / "index.m3u8").string()},
                              liveLog);
        if (liveEnc.pid <= 0) {
            std::cerr << "failed to start live encoder\n";
            return 1;
        }
        bool playlistReady = false;
        for (int i = 0; i < 80; ++i) {
            std::error_code ec;
            const auto playlist = liveDir / "index.m3u8";
            if (std::filesystem::is_regular_file(playlist, ec) && std::filesystem::file_size(playlist, ec) > 0) {
                for (const auto& entry : std::filesystem::directory_iterator(liveDir, ec)) {
                    if (entry.path().extension() == ".ts" && entry.file_size() > 500) {
                        playlistReady = true;
                        break;
                    }
                }
            }
            if (playlistReady) {
                break;
            }
            int status = 0;
            const pid_t done = waitpid(liveEnc.pid, &status, WNOHANG);
            if (done == liveEnc.pid) {
                liveEnc.pid = -1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!playlistReady) {
            const std::string encLog = readFile(liveLog);
            if (encLog.find("libx264") != std::string::npos || encLog.find("Unknown encoder") != std::string::npos) {
                std::cout << "live+vod case skipped (libx264 unavailable)\n";
                std::cout << "stream player stall test passed\n";
                return 0;
            }
            std::cerr << "live playlist was not produced\n" << encLog;
            return 1;
        }

        int livePort = 0;
        int vodPort = 0;
        const int liveFd = listenLoopback(livePort);
        const int vodFd = listenLoopback(vodPort);
        if (liveFd < 0 || vodFd < 0) {
            std::cerr << "live listen failed\n";
            return 1;
        }
        Child liveHttp = forkDir(liveFd, liveDir);
        Child vodHttp = forkBody(vodFd, body);
        if (liveHttp.pid <= 0 || vodHttp.pid <= 0) {
            std::cerr << "live server fork failed\n";
            return 1;
        }

        StreamManager liveStreams;
        SoftwareRenderer liveRenderer;
        StreamPlayer livePlayer(
            liveStreams, liveRenderer,
            []() -> std::unique_ptr<DecoderBackend> { return std::make_unique<FfmpegDecoder>(); }, 160, 120);
        if (!livePlayer.init()) {
            std::cerr << "live renderer init failed\n";
            return 1;
        }
        const std::string vodUrl = "http://127.0.0.1:" + std::to_string(vodPort) + "/clip.ts";
        const std::string liveUrl = "http://127.0.0.1:" + std::to_string(livePort) + "/index.m3u8";
        if (!liveStreams.addStream("vod", vodUrl) || !liveStreams.addStream("live", liveUrl)) {
            std::cerr << "failed to add live/vod\n";
            return 1;
        }
        if (!waitForPresent(livePlayer, liveRenderer, 15)) {
            std::cerr << "vod next to live HLS did not present\n";
            return 1;
        }
        const Sample liveSample = sampleTicks(livePlayer, std::chrono::milliseconds(6000));
        std::cout << "live+vod iterations=" << liveSample.iterations << " presents=" << liveSample.presents
                  << " maxTickMs=" << liveSample.maxTickMs << " elapsedMs=" << liveSample.elapsedMs << "\n";
        if (liveSample.iterations < 30 || liveSample.presents < 8 || liveSample.maxTickMs > 800) {
            std::cerr << "live HLS edge froze the VOD tile\n";
            return 1;
        }
        const int shutLive = shutdownMs(livePlayer);
        std::cout << "live+vod shutdownMs=" << shutLive << "\n";
        if (shutLive > kShutdownBoundMs) {
            std::cerr << "shutdown blocked on live HLS (" << shutLive << " ms)\n";
            return 1;
        }

        std::cout << "stream player stall test passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL exception " << ex.what() << "\n";
        return 1;
    }
}
