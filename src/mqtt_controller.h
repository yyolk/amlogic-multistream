#pragma once
#include "stream_manager.h"

#include <string>

struct mosquitto;
struct mosquitto_message;

// Connects to an MQTT broker and applies command topics to StreamManager.
// Hardware decode and DRM/KMS are out of scope for this controller.
class MqttController {
public:
    explicit MqttController(StreamManager& manager);
    ~MqttController();

    MqttController(const MqttController&) = delete;
    MqttController& operator=(const MqttController&) = delete;

    // Read env and CLI, then connect, subscribe, and block until SIGINT/SIGTERM.
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
    // Player-loop flag. Touched only on the mosquitto network thread.
    bool playing_ = false;
};
