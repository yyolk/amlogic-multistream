#pragma once
#include <string>
#include <vector>

struct StreamInfo {
    std::string id;
    std::string url;
    bool muted = false;
};

class StreamManager {
public:
    bool addStream(const std::string& id, const std::string& url);
    bool removeStream(const std::string& id);
    std::vector<StreamInfo> list() const;
};
