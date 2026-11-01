#pragma once
#include "stream_manager.h"

#include <atomic>
#include <string>

struct mosquitto;
struct mosquitto_message;

// Connects to an MQTT broker and applies command topics to StreamManager.
// Broker I/O runs on the mosquitto thread. start() ticks software decode and
// present on the calling thread until SIGINT/SIGTERM, so the process is not
// blocked in sigwait with no decode loop. Hardware decode and DRM/KMS are
// not started.
class MqttController {
public:
    explicit MqttController(StreamManager& manager);
    ~MqttController();

    MqttController(const MqttController&) = delete;
    MqttController& operator=(const MqttController&) = delete;

    // Read env and CLI, then connect, subscribe, and tick until SIGINT/SIGTERM.
    // Returns 0 after a clean shutdown or --help, and 1 if the client cannot start.
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
    void publishRaw(const std::string& topic, const std::string& payload, bool retain);
    void shutdown();

    static void onConnect(struct mosquitto* mosq, void* obj, int rc);
    static void onMessage(struct mosquitto* mosq, void* obj, const struct mosquitto_message* msg);
    static void onDisconnect(struct mosquitto* mosq, void* obj, int rc);

    StreamManager& manager_;
    Config config_;
    struct mosquitto* mosq_ = nullptr;
    bool libInitialized_ = false;
    bool loopStarted_ = false;
    bool helpRequested_ = false;
    bool configError_ = false;
    // Player-loop flag. The mosquitto thread stores it; the main thread loads it.
    std::atomic<bool> playing_{false};
};
