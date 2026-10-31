#pragma once
#include <mutex>
#include <string>
#include <vector>

struct StreamInfo {
    std::string id;
    std::string url;
    bool muted = false;
};

// In-memory stream table for MQTT control.
// These methods do not open decoders or DRM/KMS planes.
class StreamManager {
public:
    // Empty id or url is rejected. Repeating the same id and url succeeds.
    // The same id with a different url is rejected.
    bool addStream(const std::string& id, const std::string& url);
    bool removeStream(const std::string& id);
    std::vector<StreamInfo> list() const;

    bool muteStream(const std::string& id, bool muted);
    // Keep the entry (including mute) and record a reconnect request.
    bool restartStream(const std::string& id);
    int restartAll();

    // "auto" follows the stream count. Any other non-empty token is stored as-is.
    bool setLayout(const std::string& layout);
    std::string layout() const;
    std::string layoutSetting() const;

private:
    mutable std::mutex mu_;
    std::vector<StreamInfo> streams_;
    std::string layout_ = "auto";
};
