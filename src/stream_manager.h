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

// Why addStream returned false. None is only written on success when the
// caller passes a pointer.
enum class AddFailure {
    None,
    Empty,
    Conflict,
    Limit,
};

// In-memory stream table for MQTT control.
// add, remove, mute, and layout do not open decoders or DRM/KMS planes.
// restartStream and restartAll only bump restartGeneration.
class StreamManager {
public:
    // Empty id or url is rejected. Repeating the same id and url succeeds.
    // The same id with a different url is rejected.
    // A new id is rejected once LayoutEngine::kMaxStreams (9) entries are stored.
    // `failure` is optional and receives the reason when the call returns false.
    bool addStream(const std::string& id, const std::string& url, AddFailure* failure = nullptr);
    bool removeStream(const std::string& id);
    std::vector<StreamInfo> list() const;

    bool muteStream(const std::string& id, bool muted);
    // Keep the entry (including mute and url) and bump restartGeneration.
    // Unknown id returns false and does not change any other stream.
    bool restartStream(const std::string& id);
    int restartAll();

    // "auto" follows the stream count. A "<rows>x<cols>" token is stored when
    // LayoutEngine::parseLayout accepts it (lowercase 'x', each side 1..3).
    // Anything else, including empty, "banana", "4", and "2X2", is rejected
    // and the previous setting is left unchanged.
    bool setLayout(const std::string& layout);
    std::string layout() const;
    std::string layoutSetting() const;

private:
    mutable std::mutex mu_;
    std::vector<StreamInfo> streams_;
    std::string layout_ = "auto";
};
