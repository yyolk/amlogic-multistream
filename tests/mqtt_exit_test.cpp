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

#include "decoder_backend.h"
#include "mqtt_controller.h"
#include "stream_manager.h"

#include <mosquitto.h>

#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
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
                  const std::string& errPath) {
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
        "127.0.0.1",
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
