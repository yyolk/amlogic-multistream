#include "mqtt_controller.h"
#include "mqtt_commands.h"
#include "mqtt_json.h"

#include <mosquitto.h>

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <pthread.h>

namespace {

constexpr int kMaxPayloadBytes = 65536;

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

}  // namespace

MqttController::MqttController(StreamManager& manager) : manager_(manager) {}

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

    const std::string statusTopic = config_.prefix + "/status";
    const std::string will = buildStatusJson(false, false, "1x1", "auto", {});
    rc = mosquitto_will_set(mosq_, statusTopic.c_str(), static_cast<int>(will.size()), will.data(), 1, true);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "MQTT will setup failed: " << mosquitto_strerror(rc) << "\n";
        shutdown();
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
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    rc = mosquitto_connect_async(mosq_, config_.host.c_str(), config_.port, config_.keepalive);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "MQTT connect failed: " << mosquitto_strerror(rc) << "\n";
        shutdown();
        return 1;
    }

    rc = mosquitto_loop_start(mosq_);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "MQTT loop failed: " << mosquitto_strerror(rc) << "\n";
        shutdown();
        return 1;
    }
    loopStarted_ = true;

    std::cout << "MQTT running. Commands on " << config_.prefix
              << "/cmd/# . Waiting for SIGINT or SIGTERM.\n";
    std::cout << "This build does not start DRM/KMS or Amlogic hardware decode.\n";

    int sig = 0;
    if (sigwait(&signals, &sig) != 0) {
        std::cerr << "sigwait failed\n";
        shutdown();
        return 1;
    }

    std::cout << "MQTT shutting down on signal " << sig << "\n";
    const std::string offline =
        buildStatusJson(false, false, manager_.layout(), manager_.layoutSetting(), manager_.list());
    publishRaw(statusTopic, offline, true);
    shutdown();
    return 0;
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
        << "  -h, --help\n"
        << "\n"
        << "The process connects, subscribes to <prefix>/cmd/#, and runs until\n"
        << "SIGINT or SIGTERM. DRM/KMS and Amlogic hardware decode are not started.\n"
        << "Command schema: docs/mqtt.md\n";
}

void MqttController::handleConnect(struct mosquitto* mosq, int rc) {
    if (rc != 0) {
        std::cerr << "MQTT connect rejected: " << mosquitto_connack_string(rc) << "\n";
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
    const CommandOutcome outcome = executeCommand(manager_, playing_, command, body);
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
        buildStatusJson(true, playing_, manager_.layout(), manager_.layoutSetting(), manager_.list());
    publishRaw(config_.prefix + "/status", payload, true);
}

void MqttController::publishRaw(const std::string& topic, const std::string& payload, bool retain) {
    if (!mosq_) {
        return;
    }
    const int rc = mosquitto_publish(mosq_, nullptr, topic.c_str(), static_cast<int>(payload.size()),
                                     payload.data(), 1, retain);
    if (rc != MOSQ_ERR_SUCCESS) {
        std::cerr << "MQTT publish failed (" << topic << "): " << mosquitto_strerror(rc) << "\n";
    }
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

void MqttController::onDisconnect(struct mosquitto*, void*, int rc) {
    if (rc == 0) {
        std::cout << "MQTT disconnected\n";
    } else {
        std::cerr << "MQTT connection lost: " << mosquitto_strerror(rc) << " (will retry)\n";
    }
}
