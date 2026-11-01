#include "mqtt_controller.h"

#include "ffmpeg_decoder.h"
#include "mqtt_commands.h"
#include "mqtt_json.h"
#include "software_renderer.h"
#include "stream_player.h"

#include <mosquitto.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

constexpr int kMaxPayloadBytes = 65536;

// A refused login does not start working on retry the way a down broker can.
// Three CONNACKs is enough to tell a bad password from one lost reply, and
// short enough that launch-coreelec.sh starts Kodi instead of waiting on SSH.
constexpr int kAuthRejectLimit = 3;

bool authenticationRejected(int rc) {
    // MQTT 3.1.1 CONNACK 4 and 5, and the MQTT 5 reason codes for the same
    // refusals (0x86 bad username or password, 0x87 not authorized).
    // Server unavailable (3) and a failed TCP connect keep retrying.
    if (rc == 4 || rc == 5 || rc == 0x86 || rc == 0x87) {
        return true;
    }
    const char* text = mosquitto_connack_string(rc);
    if (text == nullptr) {
        return false;
    }
    std::string lower(text);
    for (char& ch : lower) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lower.find("not authorised") != std::string::npos ||
           lower.find("not authorized") != std::string::npos ||
           lower.find("bad user name or password") != std::string::npos ||
           lower.find("bad username or password") != std::string::npos;
}

int parsePort(const char* text, int fallback, const char* source) {
    if (!text || !*text) {
        return fallback;
    }
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 1 || value > 65535) {
        std::cerr << "Invalid " << source << " '" << text << "', using " << fallback << "\n";
        return fallback;
    }
    return static_cast<int>(value);
}

bool validPrefix(const std::string& prefix) {
    if (prefix.empty() || prefix.front() == '/') {
        return false;
    }
    return prefix.find_first_of("#+ \t\r\n") == std::string::npos;
}

bool validHost(const std::string& host) {
    return !host.empty() && host.find_first_of(" \t\r\n") == std::string::npos;
}

void stripTrailingSlashes(std::string& prefix) {
    while (!prefix.empty() && prefix.back() == '/') {
        prefix.pop_back();
    }
}

// SoftwareRenderer rejects sides above 8192. Match that here so a bad flag
// fails before the broker connect.
constexpr int kMaxViewportSide = 8192;

// Pause between present ticks while waiting for SIGINT, SIGTERM, or SIGHUP.
// Decode runs on each stream's thread, so this wait is not stuck behind a
// socket read.
// Signals are blocked process-wide and delivered here, same as the old
// sigwait, so the mosquitto thread does not race the handler.
constexpr long kTickWaitNs = 20L * 1000L * 1000L;

// libmosquitto calls on_disconnect once the DISCONNECT packet is written.
// A broker that stops reading can hold that past the launch poll, so the
// exit does not wait longer than this.
constexpr std::chrono::milliseconds kDisconnectWait{500};

bool parseViewport(const std::string& text, int& width, int& height) {
    const auto lower = text.find('x');
    const auto upper = text.find('X');
    if ((lower == std::string::npos) == (upper == std::string::npos)) {
        return false;
    }
    const auto pos = lower == std::string::npos ? upper : lower;
    if (pos == 0 || pos + 1 >= text.size()) {
        return false;
    }
    const std::string widthText = text.substr(0, pos);
    const std::string heightText = text.substr(pos + 1);
    try {
        size_t used = 0;
        const int w = std::stoi(widthText, &used);
        if (used != widthText.size()) {
            return false;
        }
        used = 0;
        const int h = std::stoi(heightText, &used);
        if (used != heightText.size()) {
            return false;
        }
        if (w < 1 || h < 1 || w > kMaxViewportSide || h > kMaxViewportSide) {
            return false;
        }
        width = w;
        height = h;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

MqttController::MqttController(StreamManager& manager, DecoderFactory decoderFactory)
    : manager_(manager), decoderFactory_(std::move(decoderFactory)) {
    static_assert(StreamPlayer::kDefaultViewportWidth == 1280, "MQTT default viewport width");
    static_assert(StreamPlayer::kDefaultViewportHeight == 720, "MQTT default viewport height");
    config_.viewportWidth = StreamPlayer::kDefaultViewportWidth;
    config_.viewportHeight = StreamPlayer::kDefaultViewportHeight;
}

MqttController::~MqttController() {
    shutdown();
}

int MqttController::start(int argc, char* argv[]) {
    loadConfig(argc, argv);
    if (helpRequested_) {
        printUsage();
        return 0;
    }
    if (configError_) {
        printUsage();
        return 1;
    }

    SoftwareRenderer renderer(config_.renderDir);
    DecoderFactory factory = decoderFactory_;
    if (!factory) {
        factory = []() -> std::unique_ptr<DecoderBackend> { return std::make_unique<FfmpegDecoder>(); };
    }
    StreamPlayer player(manager_, renderer, std::move(factory), config_.viewportWidth, config_.viewportHeight);
    if (!player.init()) {
        std::cerr << "Software presenter init failed\n";
        return 1;
    }

    std::cout << "MQTT broker " << config_.host << ":" << config_.port << " prefix " << config_.prefix
              << "\n";

    int rc = mosquitto_lib_init();
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "mosquitto_lib_init failed: " << mosquitto_strerror(rc) << "\n";
        return 1;
    }
    libInitialized_ = true;

    mosq_ = mosquitto_new(config_.clientId.c_str(), true, this);
    if (!mosq_) {
        std::cerr << "mosquitto_new failed\n";
        shutdown();
        return 1;
    }

    if (!config_.username.empty()) {
        const char* password = config_.password.empty() ? nullptr : config_.password.c_str();
        rc = mosquitto_username_pw_set(mosq_, config_.username.c_str(), password);
        if (rc != MOSQ_ERR_SUCCESS) {
            std::cerr << "MQTT auth setup failed: " << mosquitto_strerror(rc) << "\n";
            shutdown();
            return 1;
        }
    }

    if (!refreshWill(true)) {
        return 1;
    }

    mosquitto_connect_callback_set(mosq_, &MqttController::onConnect);
    mosquitto_message_callback_set(mosq_, &MqttController::onMessage);
    mosquitto_disconnect_callback_set(mosq_, &MqttController::onDisconnect);
    mosquitto_reconnect_delay_set(mosq_, 1, 30, true);

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    // SoftwareRenderer has no scanout. Say so on stderr (unit-buffered) before
    // any connect attempt, including the Kodi add-on's one-click Launch.
    std::cerr << "MQTT software mode does not drive a display. Nothing from this player "
                 "is shown until a DRM/KMS backend exists. Launch from the Kodi add-on "
                 "stops Kodi with one remote click and puts no picture on screen. "
                 "Stop this process to return to Kodi.\n";

    // libmosquitto 2.0's threaded loop does not recover when the first
    // connect_async fails (connection refused or a lookup error). Retry on a
    // helper with the same 1s..30s backoff. connect_async still calls
    // getaddrinfo on that thread; a silent nameserver would otherwise hold
    // this thread past launch-coreelec.sh's 10s SIGTERM poll (AUDIT6 N1).
    // The helper is not joined on a signal: nothing has been published yet,
    // so flush and _exit. After mosquitto_loop_start the post-connect path
    // in finishProcess already covers a reconnect stuck in getaddrinfo.
    std::atomic<bool> connectReady{false};
    std::thread connector;
    try {
        connector = std::thread([this, &connectReady]() {
            int backoffSec = 1;
            while (true) {
                const int attempt = mosquitto_connect_async(mosq_, config_.host.c_str(),
                                                            config_.port, config_.keepalive);
                if (attempt == MOSQ_ERR_SUCCESS || attempt == MOSQ_ERR_CONN_PENDING) {
                    connectReady.store(true);
                    return;
                }
                std::cerr << "MQTT connect failed: " << mosquitto_strerror(attempt)
                          << " (will retry)\n";
                std::this_thread::sleep_for(std::chrono::seconds(backoffSec));
                if (backoffSec < 30) {
                    backoffSec *= 2;
                    if (backoffSec > 30) {
                        backoffSec = 30;
                    }
                }
            }
        });
    } catch (const std::exception& ex) {
        std::cerr << "MQTT connect thread failed: " << ex.what() << "\n";
        shutdown();
        return 1;
    }

    while (!connectReady.load()) {
        timespec timeout{};
        timeout.tv_sec = 0;
        timeout.tv_nsec = kTickWaitNs;
        const int sig = sigtimedwait(&signals, nullptr, &timeout);
        if (sig > 0) {
            std::cout << "MQTT shutting down on signal " << sig << "\n";
            std::cout.flush();
            std::cerr.flush();
            std::fflush(nullptr);
            _exit(0);
        }
        if (sig < 0 && errno != EAGAIN && errno != EINTR) {
            std::cerr << "sigtimedwait failed: " << std::strerror(errno) << "\n";
            std::cout.flush();
            std::cerr.flush();
            std::fflush(nullptr);
            _exit(1);
        }
    }
    connector.join();

    rc = mosquitto_loop_start(mosq_);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "MQTT loop failed: " << mosquitto_strerror(rc) << "\n";
        shutdown();
        return 1;
    }
    loopStarted_ = true;

    std::cout << "MQTT running. Each stream decodes on its own thread; this thread presents. "
                 "Broker I/O stays on the mosquitto thread. Commands on "
              << config_.prefix << "/cmd/# . Waiting for SIGINT, SIGTERM, or SIGHUP.\n";
    std::cout << "This build does not start DRM/KMS or Amlogic hardware decode.\n";

    while (true) {
        if (authGiveUp_.load()) {
            std::cerr << "MQTT authentication failed after " << authRejects_.load()
                      << " attempts; exiting\n";
            finishProcess(player, 1);
        }

        player.tick(playing_.load());

        timespec timeout{};
        timeout.tv_sec = 0;
        timeout.tv_nsec = kTickWaitNs;
        const int sig = sigtimedwait(&signals, nullptr, &timeout);
        if (sig > 0) {
            std::cout << "MQTT shutting down on signal " << sig << "\n";
            break;
        }
        if (sig < 0 && errno != EAGAIN && errno != EINTR) {
            std::cerr << "sigtimedwait failed: " << std::strerror(errno) << "\n";
            finishProcess(player, 1);
        }
    }

    finishProcess(player, 0);
}

// Abort, queue the offline status, then join up to the deadline. getaddrinfo
// ignores abort, so the publish is queued before the wait (AUDIT4 A1 / R1).
// closing_ is set under closeMu_ with that publish, so a command or reconnect
// on the mosquitto thread cannot republish "online": true after it (AUDIT5
// F1). DISCONNECT only when the publish was queued on a live connection, so
// the broker discards the CONNECT will instead of overwriting the table. Then
// _exit without mosquitto_loop_stop: the network thread may be inside a
// reconnect's getaddrinfo or a blocking connect() to a broker that drops
// packets, whether the broker went away before the signal or during the
// drain (AUDIT5 X1). exitCode is 0 for a signal and 1 for authentication
// give-up or a failed sigtimedwait (AUDIT4 A2).
void MqttController::finishProcess(StreamPlayer& player, int exitCode) {
    bool sent = false;
    {
        std::lock_guard<std::mutex> lock(closeMu_);
        closing_ = true;
        player.closeAll();
        const std::string offline = buildStatusJson(false, playing_.load(), manager_.layout(),
                                                    manager_.layoutSetting(), manager_.list());
        sent = publishRaw(config_.prefix + "/status", offline, true);
    }
    player.drain(StreamPlayer::kExitJoinDeadline);
    if (sent) {
        {
            std::lock_guard<std::mutex> lock(disconnectMu_);
            disconnected_ = false;
        }
        if (mosquitto_disconnect(mosq_) == MOSQ_ERR_SUCCESS) {
            std::unique_lock<std::mutex> lock(disconnectMu_);
            disconnectCv_.wait_for(lock, kDisconnectWait, [this] { return disconnected_; });
        }
    }
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    _exit(exitCode);
}

void MqttController::loadConfig(int argc, char* argv[]) {
    if (const char* value = std::getenv("MQTT_HOST")) {
        config_.host = value;
    }
    if (const char* value = std::getenv("MQTT_PORT")) {
        config_.port = parsePort(value, config_.port, "MQTT_PORT");
    }
    if (const char* value = std::getenv("MQTT_TOPIC_PREFIX")) {
        config_.prefix = value;
    }
    if (const char* value = std::getenv("MQTT_USERNAME")) {
        config_.username = value;
    }
    if (const char* value = std::getenv("MQTT_PASSWORD")) {
        config_.password = value;
    }
    if (const char* value = std::getenv("MQTT_CLIENT_ID")) {
        config_.clientId = value;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        auto take = [&](std::string& dest) {
            if (i + 1 >= argc || !argv[i + 1]) {
                std::cerr << "Missing value for " << arg << "\n";
                configError_ = true;
                return;
            }
            dest = argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            helpRequested_ = true;
        } else if (arg == "--mqtt-host") {
            take(config_.host);
        } else if (arg == "--mqtt-port") {
            if (i + 1 >= argc || !argv[i + 1]) {
                std::cerr << "Missing value for --mqtt-port\n";
                configError_ = true;
            } else {
                config_.port = parsePort(argv[++i], config_.port, "--mqtt-port");
            }
        } else if (arg == "--mqtt-prefix") {
            take(config_.prefix);
        } else if (arg == "--mqtt-user") {
            take(config_.username);
        } else if (arg == "--mqtt-pass") {
            take(config_.password);
        } else if (arg == "--mqtt-client-id") {
            take(config_.clientId);
        } else if (arg == "--viewport") {
            if (i + 1 >= argc || !argv[i + 1]) {
                std::cerr << "Missing value for --viewport\n";
                configError_ = true;
            } else if (!parseViewport(argv[++i], config_.viewportWidth, config_.viewportHeight)) {
                std::cerr << "Invalid --viewport (expected WxH, for example 1280x720)\n";
                configError_ = true;
            }
        } else if (arg == "--render-dir") {
            if (i + 1 >= argc || !argv[i + 1] || argv[i + 1][0] == '\0') {
                std::cerr << "--render-dir requires a directory\n";
                configError_ = true;
            } else {
                config_.renderDir = argv[++i];
            }
        }
    }

    stripTrailingSlashes(config_.prefix);
    if (!validPrefix(config_.prefix)) {
        std::cerr << "Invalid MQTT topic prefix, using amlogic-multistream\n";
        config_.prefix = "amlogic-multistream";
    }
    if (!validHost(config_.host)) {
        std::cerr << "Invalid MQTT host, using localhost\n";
        config_.host = "localhost";
    }
    if (config_.clientId.empty()) {
        config_.clientId = "amlogic-multistream";
    }
}

void MqttController::printUsage() const {
    std::cout
        << "amlogic-multistream MQTT control\n"
        << "\n"
        << "Environment (CLI flags override these):\n"
        << "  MQTT_HOST           broker host (default localhost)\n"
        << "  MQTT_PORT           broker port (default 1883)\n"
        << "  MQTT_TOPIC_PREFIX   topic prefix (default amlogic-multistream)\n"
        << "  MQTT_USERNAME       optional username\n"
        << "  MQTT_PASSWORD       optional password\n"
        << "  MQTT_CLIENT_ID      client id (default amlogic-multistream)\n"
        << "\n"
        << "Flags:\n"
        << "  --mqtt-host HOST\n"
        << "  --mqtt-port PORT\n"
        << "  --mqtt-prefix PREFIX\n"
        << "  --mqtt-user USER\n"
        << "  --mqtt-pass PASS\n"
        << "  --mqtt-client-id ID\n"
        << "  --viewport WxH       software viewport (default 1280x720)\n"
        << "  --render-dir DIR     write PPM frames (default off)\n"
        << "  -h, --help\n"
        << "\n"
        << "The process connects, subscribes to <prefix>/cmd/#, and software-decodes\n"
        << "added http:// and https:// streams into a SoftwareRenderer viewport until\n"
        << "SIGINT, SIGTERM, or SIGHUP. If the broker is down it retries with backoff\n"
        << "from 1s to 30s. A CONNACK of not authorised or bad user name or password\n"
        << "exits non-zero after three attempts. Broker I/O runs on a background thread.\n"
        << "start/play opens decoders and presents; stop retires them. restart with an id\n"
        << "reopens that decoder. An unknown id does not restart other streams.\n"
        << "Software mode does not drive a display. DRM/KMS and Amlogic hardware\n"
        << "decode are not started, so nothing is shown until a DRM backend exists.\n"
        << "Command schema: docs/mqtt.md\n";
}

void MqttController::handleConnect(struct mosquitto* mosq, int rc) {
    if (rc != 0) {
        std::cerr << "MQTT connect rejected: " << mosquitto_connack_string(rc) << "\n";
        if (authenticationRejected(rc)) {
            const int attempts = authRejects_.fetch_add(1) + 1;
            if (attempts >= kAuthRejectLimit) {
                authGiveUp_.store(true);
            }
        }
        return;
    }
    authRejects_.store(0);
    std::lock_guard<std::mutex> lock(closeMu_);
    if (closing_) {
        return;
    }
    const std::string filter = config_.prefix + "/cmd/#";
    const int sub = mosquitto_subscribe(mosq, nullptr, filter.c_str(), 1);
    if (sub != MOSQ_ERR_SUCCESS) {
        std::cerr << "MQTT subscribe failed: " << mosquitto_strerror(sub) << "\n";
        return;
    }
    std::cout << "MQTT connected, subscribed to " << filter << "\n";
    publishStatus();
}

void MqttController::handleMessage(const std::string& topic, const std::string& payload) {
    const TopicCommand routed = commandFromTopic(config_.prefix, topic);
    if (!routed.matched) {
        return;
    }

    const JsonObject body = parseFlatJsonObject(payload);
    if (!body.valid) {
        const std::string name = routed.fromPayload ? std::string() : resolveCommandName(routed, JsonObject{});
        publishRaw(config_.prefix + "/response", buildResponseJson(false, name, "", "invalid json"), false);
        std::cerr << "MQTT invalid JSON on " << topic << "\n";
        return;
    }

    const std::string command = resolveCommandName(routed, body);
    std::lock_guard<std::mutex> lock(closeMu_);
    if (closing_) {
        publishRaw(config_.prefix + "/response", buildResponseJson(false, command, "", "shutting down"),
                   false);
        std::cout << "MQTT " << command << " ignored: shutting down\n";
        return;
    }
    bool playing = playing_.load();
    const CommandOutcome outcome = executeCommand(manager_, playing, command, body);
    playing_.store(playing);
    publishRaw(config_.prefix + "/response",
               buildResponseJson(outcome.ok, outcome.command, outcome.id, outcome.message), false);
    if (outcome.publishStatus) {
        publishStatus();
    }
    std::cout << "MQTT " << outcome.command << " ok=" << (outcome.ok ? "true" : "false") << " id="
              << outcome.id << " " << outcome.message << "\n";
}

void MqttController::publishStatus() {
    const std::string payload =
        buildStatusJson(true, playing_.load(), manager_.layout(), manager_.layoutSetting(), manager_.list());
    publishRaw(config_.prefix + "/status", payload, true);
    refreshWill(false);
}

bool MqttController::refreshWill(bool fatal) {
    if (!mosq_) {
        return false;
    }
    const std::string topic = config_.prefix + "/status";
    const std::string offline = buildStatusJson(false, playing_.load(), manager_.layout(),
                                                manager_.layoutSetting(), manager_.list());
    const int rc = mosquitto_will_set(mosq_, topic.c_str(), static_cast<int>(offline.size()),
                                      offline.data(), 1, true);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "MQTT will update failed: " << mosquitto_strerror(rc) << "\n";
        if (fatal) {
            shutdown();
        }
        return false;
    }
    return true;
}

bool MqttController::publishRaw(const std::string& topic, const std::string& payload, bool retain) {
    if (!mosq_) {
        return false;
    }
    const int rc = mosquitto_publish(mosq_, nullptr, topic.c_str(), static_cast<int>(payload.size()),
                                     payload.data(), 1, retain);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "MQTT publish failed (" << topic << "): " << mosquitto_strerror(rc) << "\n";
        return false;
    }
    // One insertion so the line is flushed (stdout is unit-buffered). A
    // later SIGKILL still leaves this last retained payload in the log.
    if (retain) {
        std::cout << "MQTT status " + payload + "\n";
    }
    return true;
}

void MqttController::shutdown() {
    if (mosq_) {
        mosquitto_disconnect(mosq_);
        if (loopStarted_) {
            mosquitto_loop_stop(mosq_, false);
            loopStarted_ = false;
        }
        mosquitto_destroy(mosq_);
        mosq_ = nullptr;
    }
    if (libInitialized_) {
        mosquitto_lib_cleanup();
        libInitialized_ = false;
    }
}

void MqttController::onConnect(struct mosquitto* mosq, void* obj, int rc) {
    static_cast<MqttController*>(obj)->handleConnect(mosq, rc);
}

void MqttController::onMessage(struct mosquitto*, void* obj, const struct mosquitto_message* msg) {
    if (!obj || !msg || !msg->topic) {
        return;
    }
    auto* self = static_cast<MqttController*>(obj);
    // Retained deliveries are replays from the broker on subscribe. Commands are live-only.
    if (msg->retain) {
        return;
    }
    if (msg->payloadlen > kMaxPayloadBytes) {
        self->publishRaw(self->config_.prefix + "/response",
                         buildResponseJson(false, "", "", "payload too large"), false);
        std::cerr << "MQTT payload too large on " << msg->topic << "\n";
        return;
    }
    std::string payload;
    if (msg->payload && msg->payloadlen > 0) {
        payload.assign(static_cast<const char*>(msg->payload), static_cast<size_t>(msg->payloadlen));
    }
    self->handleMessage(msg->topic, payload);
}

void MqttController::onDisconnect(struct mosquitto*, void* obj, int rc) {
    if (obj != nullptr) {
        auto* self = static_cast<MqttController*>(obj);
        {
            std::lock_guard<std::mutex> lock(self->disconnectMu_);
            self->disconnected_ = true;
        }
        self->disconnectCv_.notify_all();
    }
    if (rc == 0) {
        std::cout << "MQTT disconnected\n";
        return;
    }
    const bool giveUp = obj != nullptr && static_cast<MqttController*>(obj)->authGiveUp_.load();
    std::cerr << "MQTT connection lost: " << mosquitto_strerror(rc);
    if (giveUp) {
        std::cerr << "\n";
    } else {
        std::cerr << " (will retry)\n";
    }
}
