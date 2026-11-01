#pragma once
#include "decoder_backend.h"
#include "stream_manager.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

struct mosquitto;
struct mosquitto_message;
class StreamPlayer;

// Connects to an MQTT broker and applies command topics to StreamManager.
// Broker I/O runs on the mosquitto thread. start() presents on the calling
// thread until SIGINT, SIGTERM, or SIGHUP. Each stream is decoded on its own
// thread, so a stalled read does not block the other tiles or the signal
// wait. A broker that is down at startup is retried with backoff instead of
// exiting. A CONNACK that refuses the username or password exits non-zero
// after a few attempts so scripts/launch-coreelec.sh can start Kodi. That
// give-up uses the same bounded worker drain as a signal and exits 1.
// Hardware decode and DRM/KMS are not started.
class MqttController {
public:
    using DecoderFactory = std::function<std::unique_ptr<DecoderBackend>()>;

    // An empty factory selects FfmpegDecoder. Tests pass a backend whose
    // open() ignores abort(), the stand-in for a stuck getaddrinfo.
    explicit MqttController(StreamManager& manager, DecoderFactory decoderFactory = {});
    ~MqttController();

    MqttController(const MqttController&) = delete;
    MqttController& operator=(const MqttController&) = delete;

    // Read env and CLI, then connect, subscribe, and tick until SIGINT,
    // SIGTERM, or SIGHUP. Returns 0 for --help or a signal before the first
    // connect is queued, and 1 if the client cannot start. Once the network
    // thread is running, start() does not return: a signal _exits 0, and
    // authentication give-up or a failed sigtimedwait _exits 1, whether or not
    // a worker is still blocked. An unreachable broker is retried.
    int start(int argc, char* argv[]);

private:
    struct Config {
        std::string host = "localhost";
        int port = 1883;
        std::string prefix = "amlogic-multistream";
        std::string username;
        std::string password;
        std::string clientId = "amlogic-multistream";
        int keepalive = 60;
        // Software viewport for MQTT mode. --url keeps its own frame-sized path.
        int viewportWidth = 1280;
        int viewportHeight = 720;
        // Empty means SoftwareRenderer writes no PPM files.
        std::string renderDir;
    };

    void loadConfig(int argc, char* argv[]);
    void printUsage() const;
    void handleConnect(struct mosquitto* mosq, int rc);
    void handleMessage(const std::string& topic, const std::string& payload);
    void publishStatus();
    // True when libmosquitto queued the message on a live connection.
    // False when there is no client or the broker is not connected.
    bool publishRaw(const std::string& topic, const std::string& payload, bool retain);
    // Offline snapshot of the current table. libmosquitto sends it on the
    // next CONNECT, so a reconnect after a drop picks up streams added since
    // the previous session. A SIGKILL before that reconnect still publishes
    // the will from the last CONNECT.
    bool refreshWill(bool fatal);
    // Final offline publish, bounded drain, then _exit(exitCode). It never
    // calls mosquitto_loop_stop, which joins a reconnect that can sit in
    // getaddrinfo or a blocking connect() well past launch-coreelec.sh's 10s
    // SIGTERM poll. DISCONNECT is sent only when the offline publish was
    // queued, and its wait is capped at 500ms.
    [[noreturn]] void finishProcess(StreamPlayer& player, int exitCode);
    void shutdown();

    static void onConnect(struct mosquitto* mosq, void* obj, int rc);
    static void onMessage(struct mosquitto* mosq, void* obj, const struct mosquitto_message* msg);
    static void onDisconnect(struct mosquitto* mosq, void* obj, int rc);

    StreamManager& manager_;
    DecoderFactory decoderFactory_;
    Config config_;
    struct mosquitto* mosq_ = nullptr;
    bool libInitialized_ = false;
    bool loopStarted_ = false;
    bool helpRequested_ = false;
    bool configError_ = false;
    // Player-loop flag. The mosquitto thread stores it; the main thread loads it.
    std::atomic<bool> playing_{false};
    // CONNACK "not authorised" / "bad user name or password" count. The
    // mosquitto thread stores these; the main thread loads authGiveUp_.
    std::atomic<int> authRejects_{0};
    std::atomic<bool> authGiveUp_{false};
    // Set by finishProcess before the offline publish. handleMessage and
    // handleConnect hold closeMu_ and stop at closing_, so nothing republishes
    // "online": true that the exit DISCONNECT would then keep retained.
    std::mutex closeMu_;
    bool closing_ = false;
    // onDisconnect sets disconnected_ for finishProcess's bounded wait.
    std::mutex disconnectMu_;
    std::condition_variable disconnectCv_;
    bool disconnected_ = false;
};
