// AUDIT4 A1 / R1 and A2. A decoder whose open() ignores abort() stands in for
// a getaddrinfo that never returns.
//
// With a live local broker, SIGTERM must exit inside the join deadline and
// leave the stream id in the retained offline status. Disconnect runs only
// because the final publish was queued; dropping the TCP session without
// DISCONNECT would let the broker publish the empty CONNECT will over it.
//
// Authentication give-up with that decoder still in open() must exit 1
// within the same deadline, not join the worker from ~StreamPlayer. A
// startup refusal with no streams must still exit 1 (K1).
//
// AUDIT5 F1: a command delivered during the exit drain must not republish
// "online": true, which the DISCONNECT would then keep retained.
//
// AUDIT5 X1: the exit must not join libmosquitto's network thread while a
// reconnect is stuck in getaddrinfo (broker gone before SIGTERM, or during
// the drain after the offline publish went out) or in connect() to a broker
// that drops packets.

#include "decoder_backend.h"
#include "mqtt_controller.h"
#include "stream_manager.h"

#include <mosquitto.h>

#include <arpa/inet.h>
#include <csignal>
#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

// Broker name for the X1 lookup cases. It resolves to 127.0.0.1 until
// g_dnsStallPath exists. After that, a lookup writes g_dnsEnteredPath and
// blocks like a resolver whose nameservers never answer.
constexpr const char* kStallHost = "broker.stall.test";
std::string g_dnsStallPath;
std::string g_dnsEnteredPath;

}  // namespace

// Defined in the executable so it interposes libmosquitto's getaddrinfo.
extern "C" int getaddrinfo(const char* node,
                           const char* service,
                           const struct addrinfo* hints,
                           struct addrinfo** res) {
    using Real = int (*)(const char*, const char*, const struct addrinfo*, struct addrinfo**);
    static const Real real = reinterpret_cast<Real>(dlsym(RTLD_NEXT, "getaddrinfo"));
    if (node != nullptr && std::strcmp(node, kStallHost) == 0) {
        if (!g_dnsStallPath.empty() && access(g_dnsStallPath.c_str(), F_OK) == 0) {
            {
                std::ofstream out(g_dnsEnteredPath);
                out << "stalled\n";
            }
            std::this_thread::sleep_for(std::chrono::seconds(60));
            return EAI_AGAIN;
        }
        node = "127.0.0.1";
    }
    return real(node, service, hints, res);
}

namespace {

int g_failures = 0;

void fail(const std::string& message) {
    std::cerr << "FAIL " << message << "\n";
    ++g_failures;
}

std::string mosquittoBin() {
    const char* candidates[] = {"/usr/sbin/mosquitto", "/usr/bin/mosquitto", nullptr};
    for (const char** path = candidates; *path != nullptr; ++path) {
        if (access(*path, X_OK) == 0) {
            return *path;
        }
    }
    return {};
}

int freePort() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        close(fd);
        return -1;
    }
    const int port = ntohs(addr.sin_port);
    close(fd);
    return port;
}

bool portOpen(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    const int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    close(fd);
    return rc == 0;
}

bool waitUntil(const std::function<bool()>& pred, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

bool fileContains(const std::string& path, const std::string& needle) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return body.find(needle) != std::string::npos;
}

void dumpFile(const std::string& path) {
    std::cerr << "--- " << path << " ---\n";
    std::ifstream in(path);
    if (!in) {
        std::cerr << "(missing)\n";
        return;
    }
    std::cerr << in.rdbuf() << "\n";
}

class Broker {
public:
    explicit Broker(std::string dir) : dir_(std::move(dir)) {}

    ~Broker() { stop(); }

    bool start(int port, bool anonymous) {
        stop();
        const std::string conf = dir_ + "/mosquitto.conf";
        {
            std::ofstream out(conf);
            out << "listener " << port << " 127.0.0.1\n";
            out << "allow_anonymous " << (anonymous ? "true" : "false") << "\n";
            out << "persistence false\n";
        }
        const std::string logPath = dir_ + "/mosquitto.log";
        const pid_t pid = fork();
        if (pid < 0) {
            return false;
        }
        if (pid == 0) {
            const int fd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if (fd > 2) {
                    close(fd);
                }
            }
            execl(bin_.c_str(), "mosquitto", "-c", conf.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        pid_ = pid;
        if (!waitUntil([&] { return portOpen(port) || !alive(); }, 3000) || !portOpen(port)) {
            std::cerr << "broker did not listen on " << port << "\n";
            dumpFile(logPath);
            stop();
            return false;
        }
        return true;
    }

    void stop() {
        if (pid_ <= 0) {
            return;
        }
        kill(pid_, SIGTERM);
        for (int i = 0; i < 50; ++i) {
            int status = 0;
            const pid_t got = waitpid(pid_, &status, WNOHANG);
            if (got == pid_) {
                pid_ = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        kill(pid_, SIGKILL);
        int status = 0;
        waitpid(pid_, &status, 0);
        pid_ = -1;
    }

    bool alive() const { return pid_ > 0 && kill(pid_, 0) == 0; }

    static void setBin(std::string bin) { bin_ = std::move(bin); }

private:
    std::string dir_;
    pid_t pid_ = -1;
    static std::string bin_;
};

std::string Broker::bin_;

// open() ignores abort() and sleeps, the same shape as a stuck resolver.
class StallingDecoder : public DecoderBackend {
public:
    explicit StallingDecoder(std::string readyPath) : readyPath_(std::move(readyPath)) {}

    bool open(const std::string&) override {
        {
            std::ofstream out(readyPath_);
            out << "open\n";
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

    void close() override {}

    bool isHardware() const override { return false; }

    void abort() override {}

    bool readFrame(DecodedFrame& frame) override {
        frame = DecodedFrame{};
        return false;
    }

private:
    std::string readyPath_;
};

struct Exit {
    bool exited = false;
    int code = -1;
    int elapsedMs = 0;
};

Exit waitExit(pid_t pid, int timeoutMs) {
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(timeoutMs);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t got = waitpid(pid, &status, WNOHANG);
        if (got == pid) {
            Exit result;
            result.exited = true;
            result.elapsedMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                     std::chrono::steady_clock::now() - start)
                                                     .count());
            if (WIFEXITED(status)) {
                result.code = WEXITSTATUS(status);
            }
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return {};
}

pid_t spawnPlayer(int port,
                  const std::string& prefix,
                  const std::string& clientId,
                  const std::string& readyPath,
                  const std::string& outPath,
                  const std::string& errPath,
                  const std::string& host = "127.0.0.1") {
    // Flush before fork. A child that freopen()s stdout would otherwise
    // write the parent's still-buffered lines a second time.
    std::cout.flush();
    std::cerr.flush();
    fflush(nullptr);
    const pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }
    FILE* out = freopen(outPath.c_str(), "w", stdout);
    FILE* err = freopen(errPath.c_str(), "w", stderr);
    if (out == nullptr || err == nullptr) {
        _exit(9);
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    std::vector<std::string> args = {
        "mqtt-exit-test",
        "--mqtt-host",
        host,
        "--mqtt-port",
        std::to_string(port),
        "--mqtt-prefix",
        prefix,
        "--mqtt-client-id",
        clientId,
        "--viewport",
        "32x16",
    };
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    StreamManager manager;
    MqttController mqtt(manager, [readyPath]() -> std::unique_ptr<DecoderBackend> {
        return std::make_unique<StallingDecoder>(readyPath);
    });
    const int rc = mqtt.start(static_cast<int>(argv.size() - 1), argv.data());
    _exit(rc);
}

bool publish(int port, const std::string& topic, const std::string& payload) {
    mosquitto* mosq = mosquitto_new(nullptr, true, nullptr);
    if (mosq == nullptr) {
        return false;
    }
    const int connected = mosquitto_connect(mosq, "127.0.0.1", port, 30);
    if (connected != MOSQ_ERR_SUCCESS) {
        mosquitto_destroy(mosq);
        return false;
    }
    const int published = mosquitto_publish(mosq, nullptr, topic.c_str(), static_cast<int>(payload.size()),
                                            payload.data(), 1, false);
    for (int i = 0; i < 30; ++i) {
        if (mosquitto_loop(mosq, 50, 1) != MOSQ_ERR_SUCCESS) {
            break;
        }
    }
    mosquitto_disconnect(mosq);
    mosquitto_loop(mosq, 50, 1);
    mosquitto_destroy(mosq);
    return published == MOSQ_ERR_SUCCESS;
}

std::string readRetained(int port, const std::string& topic) {
    struct Ctx {
        std::string payload;
        bool got = false;
    } ctx;
    mosquitto* mosq = mosquitto_new(nullptr, true, &ctx);
    if (mosq == nullptr) {
        return {};
    }
    mosquitto_message_callback_set(mosq, [](mosquitto*, void* obj, const mosquitto_message* msg) {
        auto* held = static_cast<Ctx*>(obj);
        if (msg != nullptr && msg->payload != nullptr && msg->payloadlen > 0) {
            held->payload.assign(static_cast<const char*>(msg->payload),
                                 static_cast<size_t>(msg->payloadlen));
        }
        held->got = true;
    });
    if (mosquitto_connect(mosq, "127.0.0.1", port, 30) != MOSQ_ERR_SUCCESS) {
        mosquitto_destroy(mosq);
        return {};
    }
    if (mosquitto_subscribe(mosq, nullptr, topic.c_str(), 1) != MOSQ_ERR_SUCCESS) {
        mosquitto_destroy(mosq);
        return {};
    }
    for (int i = 0; i < 40 && !ctx.got; ++i) {
        if (mosquitto_loop(mosq, 50, 1) != MOSQ_ERR_SUCCESS) {
            break;
        }
    }
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    return ctx.payload;
}

void testSigtermKeepsStreams(Broker& broker, int port, const std::string& dir) {
    const std::string prefix = "audit4-term";
    const std::string ready = dir + "/stall-term";
    const std::string outPath = dir + "/term.out";
    const std::string errPath = dir + "/term.err";
    unlink(ready.c_str());
    const pid_t pid = spawnPlayer(port, prefix, "audit4-term-client", ready, outPath, errPath);
    if (pid <= 0) {
        fail("SIGTERM: fork failed");
        return;
    }
    if (!waitUntil([&] { return fileContains(outPath, "subscribed to"); }, 5000)) {
        fail("SIGTERM: player did not subscribe");
        dumpFile(outPath);
        dumpFile(errPath);
        kill(pid, SIGKILL);
        waitExit(pid, 1000);
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const std::string base = prefix + "/cmd/";
    if (!publish(port, base + "add", "{\"id\":\"cam\",\"url\":\"http://example/stall.ts\"}") ||
        !publish(port, base + "start", "{}")) {
        fail("SIGTERM: command publish failed");
        kill(pid, SIGKILL);
        waitExit(pid, 1000);
        return;
    }
    if (!waitUntil([&] { return access(ready.c_str(), F_OK) == 0; }, 5000)) {
        fail("SIGTERM: stalling open() did not start");
        dumpFile(outPath);
        dumpFile(errPath);
        kill(pid, SIGKILL);
        waitExit(pid, 1000);
        return;
    }
    if (kill(pid, SIGTERM) != 0) {
        fail("SIGTERM: kill failed");
        waitExit(pid, 1000);
        return;
    }
    const Exit result = waitExit(pid, 6000);
    std::cout << "sigterm elapsedMs=" << result.elapsedMs << " code=" << result.code << "\n";
    if (!result.exited || result.code != 0 || result.elapsedMs < 1500 || result.elapsedMs > 5000) {
        fail("SIGTERM: expected exit 0 between 1500 and 5000 ms, got exited=" +
             std::to_string(result.exited) + " code=" + std::to_string(result.code) +
             " elapsedMs=" + std::to_string(result.elapsedMs));
        dumpFile(outPath);
        dumpFile(errPath);
    }
    const std::string status = readRetained(port, prefix + "/status");
    std::cout << "sigterm retained=" << status << "\n";
    if (status.find("\"online\":false") == std::string::npos || status.find("\"playing\":true") == std::string::npos ||
        status.find("\"id\":\"cam\"") == std::string::npos || status.find("\"streams\":[]") != std::string::npos) {
        fail("SIGTERM: retained status lost the live stream table: " + status);
        dumpFile(outPath);
        dumpFile(errPath);
    }
    if (!broker.alive()) {
        fail("SIGTERM: broker exited");
    }
}

int msSince(std::chrono::steady_clock::time_point start) {
    return static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}

struct Player {
    pid_t pid = -1;
    std::string prefix;
    std::string ready;
    std::string out;
    std::string err;
};

// Spawn a player and wait for its subscribe. With stuckStream, also add and
// start a stream whose open() never returns, so drain() sits out its deadline.
bool launchPlayer(Player& player,
                  int port,
                  const std::string& name,
                  const std::string& dir,
                  const std::string& host,
                  bool stuckStream) {
    player.prefix = "audit5-" + name;
    player.ready = dir + "/stall-" + name;
    player.out = dir + "/" + name + ".out";
    player.err = dir + "/" + name + ".err";
    unlink(player.ready.c_str());
    player.pid = spawnPlayer(port, player.prefix, player.prefix + "-client", player.ready, player.out,
                             player.err, host);
    if (player.pid <= 0) {
        fail(name + ": fork failed");
        return false;
    }
    auto abandon = [&](const std::string& why) {
        fail(name + ": " + why);
        dumpFile(player.out);
        dumpFile(player.err);
        kill(player.pid, SIGKILL);
        waitExit(player.pid, 1000);
        return false;
    };
    if (!waitUntil([&] { return fileContains(player.out, "subscribed to"); }, 5000)) {
        return abandon("player did not subscribe");
    }
    if (!stuckStream) {
        return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const std::string base = player.prefix + "/cmd/";
    if (!publish(port, base + "add", "{\"id\":\"cam\",\"url\":\"http://example/stall.ts\"}") ||
        !publish(port, base + "start", "{}")) {
        return abandon("command publish failed");
    }
    if (!waitUntil([&] { return access(player.ready.c_str(), F_OK) == 0; }, 5000)) {
        return abandon("stalling open() did not start");
    }
    return true;
}

void touch(const std::string& path) {
    std::ofstream out(path);
    out << "1\n";
}

// AUDIT5 F1. `list` during the 2s drain used to publish a retained
// "online": true, and the DISCONNECT then told the broker to drop the will.
void testCommandDuringDrain(int port, const std::string& dir) {
    const int failuresBefore = g_failures;
    Player player;
    if (!launchPlayer(player, port, "drain-cmd", dir, "127.0.0.1", true)) {
        return;
    }
    kill(player.pid, SIGTERM);
    const auto signalled = std::chrono::steady_clock::now();
    const std::string offlineLine = "MQTT status {\"online\":false";
    if (!waitUntil([&] { return fileContains(player.out, offlineLine); }, 1500)) {
        fail("drain-cmd: no offline status after SIGTERM");
    }
    if (!publish(port, player.prefix + "/cmd/list", "{}")) {
        fail("drain-cmd: list publish failed");
    }
    const Exit result = waitExit(player.pid, 6000);
    const int elapsedMs = msSince(signalled);
    std::cout << "drain-cmd elapsedMs=" << elapsedMs << " code=" << result.code << "\n";
    if (!result.exited || result.code != 0 || elapsedMs < 1500 || elapsedMs > 5000) {
        fail("drain-cmd: expected exit 0 between 1500 and 5000 ms, got exited=" +
             std::to_string(result.exited) + " code=" + std::to_string(result.code) +
             " elapsedMs=" + std::to_string(elapsedMs));
    }
    if (!fileContains(player.out, "MQTT list ignored: shutting down")) {
        fail("drain-cmd: list was not refused during the drain");
    }
    std::ifstream in(player.out);
    const std::string log((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto offlineAt = log.find(offlineLine);
    if (offlineAt != std::string::npos &&
        log.find("MQTT status {\"online\":true", offlineAt) != std::string::npos) {
        fail("drain-cmd: online status published after the offline status");
    }
    const std::string status = readRetained(port, player.prefix + "/status");
    std::cout << "drain-cmd retained=" << status << "\n";
    if (status.find("\"online\":false") == std::string::npos || status.find("\"id\":\"cam\"") == std::string::npos) {
        fail("drain-cmd: retained status is not the offline table: " + status);
    }
    if (g_failures != failuresBefore) {
        dumpFile(player.out);
        dumpFile(player.err);
    }
}

// AUDIT5 X1 (Gemini G1). No stuck worker, so drain() returns at once. The
// broker went away first and the reconnect is blocked in getaddrinfo. The
// exit used to join that thread in mosquitto_loop_stop.
void testBrokerGoneStalledLookup(Broker& broker, int port, const std::string& dir) {
    g_dnsStallPath = dir + "/dns-stall";
    g_dnsEnteredPath = dir + "/dns-entered";
    unlink(g_dnsStallPath.c_str());
    unlink(g_dnsEnteredPath.c_str());
    Player player;
    if (launchPlayer(player, port, "gone-dns", dir, kStallHost, false)) {
        touch(g_dnsStallPath);
        broker.stop();
        if (!waitUntil([&] { return access(g_dnsEnteredPath.c_str(), F_OK) == 0; }, 5000)) {
            fail("gone-dns: reconnect did not reach the stalled lookup");
            dumpFile(player.out);
            dumpFile(player.err);
            kill(player.pid, SIGKILL);
            waitExit(player.pid, 1000);
        } else {
            kill(player.pid, SIGTERM);
            const Exit result = waitExit(player.pid, 5000);
            std::cout << "gone-dns elapsedMs=" << result.elapsedMs << " code=" << result.code << "\n";
            if (!result.exited || result.code != 0 || result.elapsedMs > 1500) {
                fail("gone-dns: expected exit 0 within 1500 ms, got exited=" + std::to_string(result.exited) +
                     " code=" + std::to_string(result.code) + " elapsedMs=" + std::to_string(result.elapsedMs));
                dumpFile(player.out);
                dumpFile(player.err);
            }
        }
    }
    unlink(g_dnsStallPath.c_str());
    if (!broker.start(port, true)) {
        fail("gone-dns: broker did not restart");
    }
}

// Listens on the broker's port with a full accept queue. Linux drops further
// SYNs, so a blocking connect() hangs as it would against a broker address
// that silently drops packets.
class Blackhole {
public:
    ~Blackhole() { stop(); }

    bool start(int port) {
        listener_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0) {
            return false;
        }
        const int one = 1;
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener_, 0) != 0) {
            stop();
            return false;
        }
        for (int i = 0; i < 4; ++i) {
            const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
            if (fd < 0) {
                stop();
                return false;
            }
            connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            fillers_.push_back(fd);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return true;
    }

    void stop() {
        for (const int fd : fillers_) {
            close(fd);
        }
        fillers_.clear();
        if (listener_ >= 0) {
            close(listener_);
            listener_ = -1;
        }
    }

private:
    int listener_ = -1;
    std::vector<int> fillers_;
};

// AUDIT5 X1, no DNS involved: the reconnect is blocked in connect().
void testBlackholedBroker(Broker& broker, int port, const std::string& dir) {
    Player player;
    if (launchPlayer(player, port, "gone-blackhole", dir, "127.0.0.1", false)) {
        broker.stop();
        Blackhole hole;
        if (!hole.start(port)) {
            fail("gone-blackhole: could not listen on the broker port");
            kill(player.pid, SIGKILL);
            waitExit(player.pid, 1000);
        } else if (!waitUntil([&] { return fileContains(player.err, "MQTT connection lost"); }, 3000)) {
            fail("gone-blackhole: player did not notice the broker drop");
            dumpFile(player.out);
            dumpFile(player.err);
            kill(player.pid, SIGKILL);
            waitExit(player.pid, 1000);
        } else {
            // libmosquitto waits 1s before the first reconnect.
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            kill(player.pid, SIGTERM);
            const Exit result = waitExit(player.pid, 5000);
            std::cout << "gone-blackhole elapsedMs=" << result.elapsedMs << " code=" << result.code << "\n";
            if (!result.exited || result.code != 0 || result.elapsedMs > 1500) {
                fail("gone-blackhole: expected exit 0 within 1500 ms, got exited=" +
                     std::to_string(result.exited) + " code=" + std::to_string(result.code) +
                     " elapsedMs=" + std::to_string(result.elapsedMs));
                dumpFile(player.out);
                dumpFile(player.err);
            }
        }
    }
    if (!broker.start(port, true)) {
        fail("gone-blackhole: broker did not restart");
    }
}

// AUDIT5 X1 (my F2). The offline publish went out, so DISCONNECT is due,
// but the broker drops during the drain and the reconnect blocks in
// getaddrinfo before the deadline.
void testBrokerDropDuringDrain(Broker& broker, int port, const std::string& dir) {
    g_dnsStallPath = dir + "/dns-stall";
    g_dnsEnteredPath = dir + "/dns-entered";
    unlink(g_dnsStallPath.c_str());
    unlink(g_dnsEnteredPath.c_str());
    Player player;
    if (launchPlayer(player, port, "drop-drain", dir, kStallHost, true)) {
        touch(g_dnsStallPath);
        kill(player.pid, SIGTERM);
        const auto signalled = std::chrono::steady_clock::now();
        if (!waitUntil([&] { return fileContains(player.out, "MQTT status {\"online\":false"); }, 1500)) {
            fail("drop-drain: no offline status after SIGTERM");
        }
        broker.stop();
        const Exit result = waitExit(player.pid, 6000);
        const int elapsedMs = msSince(signalled);
        std::cout << "drop-drain elapsedMs=" << elapsedMs << " code=" << result.code << "\n";
        if (!result.exited || result.code != 0 || elapsedMs < 1500 || elapsedMs > 4000) {
            fail("drop-drain: expected exit 0 between 1500 and 4000 ms, got exited=" +
                 std::to_string(result.exited) + " code=" + std::to_string(result.code) +
                 " elapsedMs=" + std::to_string(elapsedMs));
            dumpFile(player.out);
            dumpFile(player.err);
        }
        if (access(g_dnsEnteredPath.c_str(), F_OK) != 0) {
            fail("drop-drain: reconnect did not reach the stalled lookup during the drain");
            dumpFile(player.out);
            dumpFile(player.err);
        }
    }
    unlink(g_dnsStallPath.c_str());
    if (!broker.start(port, true)) {
        fail("drop-drain: broker did not restart");
    }
}

void testAuthGiveUpBounded(Broker& broker, int port, const std::string& dir) {
    const std::string prefix = "audit4-auth";
    const std::string ready = dir + "/stall-auth";
    const std::string outPath = dir + "/auth.out";
    const std::string errPath = dir + "/auth.err";
    unlink(ready.c_str());
    const pid_t pid = spawnPlayer(port, prefix, "audit4-auth-client", ready, outPath, errPath);
    if (pid <= 0) {
        fail("auth: fork failed");
        return;
    }
    if (!waitUntil([&] { return fileContains(outPath, "subscribed to"); }, 5000)) {
        fail("auth: player did not subscribe");
        dumpFile(outPath);
        dumpFile(errPath);
        kill(pid, SIGKILL);
        waitExit(pid, 1000);
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const std::string base = prefix + "/cmd/";
    if (!publish(port, base + "add", "{\"id\":\"cam\",\"url\":\"http://example/stall.ts\"}") ||
        !publish(port, base + "start", "{}")) {
        fail("auth: command publish failed");
        kill(pid, SIGKILL);
        waitExit(pid, 1000);
        return;
    }
    if (!waitUntil([&] { return access(ready.c_str(), F_OK) == 0; }, 5000)) {
        fail("auth: stalling open() did not start");
        dumpFile(outPath);
        dumpFile(errPath);
        kill(pid, SIGKILL);
        waitExit(pid, 1000);
        return;
    }
    broker.stop();
    // Give the client time to notice the drop before the refusing broker is up,
    // so the next CONNACKs are authentication failures rather than a race with
    // the old process still in TIME_WAIT.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (!broker.start(port, false)) {
        fail("auth: refusing broker did not start");
        kill(pid, SIGKILL);
        waitExit(pid, 1000);
        return;
    }
    const auto giveUpStart = std::chrono::steady_clock::now();
    const bool gaveUp = waitUntil(
        [&] { return fileContains(errPath, "MQTT authentication failed after 3 attempts; exiting"); }, 30000);
    if (!gaveUp) {
        fail("auth: player did not give up after three refusals");
        dumpFile(outPath);
        dumpFile(errPath);
        dumpFile(dir + "/mosquitto.log");
        kill(pid, SIGKILL);
        waitExit(pid, 1000);
        return;
    }
    const Exit result = waitExit(pid, 5000);
    const int sinceLine = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now() - giveUpStart)
                                                .count());
    std::cout << "auth give-up to exit observedMs=" << sinceLine << " waitExitMs=" << result.elapsedMs
              << " code=" << result.code << "\n";
    // The line is written, then drain waits kExitJoinDeadline. Polling can
    // see the line a little late, so the floor is under 2s. Joining the
    // 120s stall would blow the ceiling.
    if (!result.exited || result.code != 1 || result.elapsedMs > 4000) {
        fail("auth: expected exit 1 within 4000 ms of the deadline wait, got exited=" +
             std::to_string(result.exited) + " code=" + std::to_string(result.code) +
             " elapsedMs=" + std::to_string(result.elapsedMs));
        dumpFile(outPath);
        dumpFile(errPath);
    }
    if (result.elapsedMs < 1200) {
        fail("auth: exited in " + std::to_string(result.elapsedMs) +
             " ms, before the stall deadline; the worker was not still blocked");
        dumpFile(outPath);
        dumpFile(errPath);
    }
}

void testStartupAuthExitCode(Broker& broker, int port, const std::string& dir) {
    if (!broker.start(port, false)) {
        fail("startup auth: broker did not start");
        return;
    }
    const std::string outPath = dir + "/startup.out";
    const std::string errPath = dir + "/startup.err";
    const pid_t pid =
        spawnPlayer(port, "audit4-startup", "audit4-startup-client", dir + "/stall-startup", outPath, errPath);
    if (pid <= 0) {
        fail("startup auth: fork failed");
        return;
    }
    const bool gaveUp = waitUntil(
        [&] { return fileContains(errPath, "MQTT authentication failed after 3 attempts; exiting"); }, 30000);
    if (!gaveUp) {
        fail("startup auth: player did not give up");
        dumpFile(outPath);
        dumpFile(errPath);
        kill(pid, SIGKILL);
        waitExit(pid, 1000);
        return;
    }
    const Exit result = waitExit(pid, 3000);
    std::cout << "startup auth waitExitMs=" << result.elapsedMs << " code=" << result.code << "\n";
    if (!result.exited || result.code != 1) {
        fail("startup auth: expected exit 1, got exited=" + std::to_string(result.exited) +
             " code=" + std::to_string(result.code));
        dumpFile(outPath);
        dumpFile(errPath);
    }
    // No decode thread exists yet, so the drain is empty and must not sit out
    // kExitJoinDeadline or _exit(0).
    if (result.elapsedMs > 1500) {
        fail("startup auth: empty reaper took " + std::to_string(result.elapsedMs) + " ms");
        dumpFile(outPath);
        dumpFile(errPath);
    }
}

}  // namespace

int main() {
    const std::string bin = mosquittoBin();
    if (bin.empty()) {
        std::cout << "SKIP mosquitto daemon not found\n";
        return 77;
    }
    Broker::setBin(bin);
    char tmpl[] = "/tmp/amlogic-mqtt-exit.XXXXXX";
    char* made = mkdtemp(tmpl);
    if (made == nullptr) {
        std::cerr << "mkdtemp failed\n";
        return 1;
    }
    const std::string dir = made;
    const int port = freePort();
    if (port <= 0) {
        std::cerr << "no free port\n";
        return 1;
    }
    mosquitto_lib_init();
    {
        Broker broker(dir);
        if (!broker.start(port, true)) {
            std::cerr << "anonymous broker failed\n";
            mosquitto_lib_cleanup();
            return 1;
        }
        testSigtermKeepsStreams(broker, port, dir);
        testCommandDuringDrain(port, dir);
        testBrokerGoneStalledLookup(broker, port, dir);
        testBlackholedBroker(broker, port, dir);
        testBrokerDropDuringDrain(broker, port, dir);
        testAuthGiveUpBounded(broker, port, dir);
        testStartupAuthExitCode(broker, port, dir);
    }
    mosquitto_lib_cleanup();
    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        std::cerr << "logs kept in " << dir << "\n";
        return 1;
    }
    const std::string cleanup = "rm -rf " + dir;
    if (std::system(cleanup.c_str()) != 0) {
        std::cerr << "cleanup failed for " << dir << "\n";
    }
    std::cout << "ok\n";
    return 0;
}
