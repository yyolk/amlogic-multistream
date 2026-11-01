#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct StreamInfo {
    std::string id;
    std::string url;
    bool muted = false;
    // Incremented by restartStream and restartAll. A player loop reopens the
    // decoder when this changes. An unknown id does not change any entry.
    uint64_t restartGeneration = 0;
};

// In-memory stream table for MQTT control.
// add, remove, mute, and layout do not open decoders or DRM/KMS planes.
// restartStream and restartAll only bump restartGeneration.
class StreamManager {
public:
    // Empty id or url is rejected. Repeating the same id and url succeeds.
    // The same id with a different url is rejected.
    bool addStream(const std::string& id, const std::string& url);
    bool removeStream(const std::string& id);
    std::vector<StreamInfo> list() const;

    bool muteStream(const std::string& id, bool muted);
    // Keep the entry (including mute and url) and bump restartGeneration.
    // Unknown id returns false and does not change any other stream.
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
