#include "stream_manager.h"
#include "layout_engine.h"

#include <algorithm>
#include <iostream>

namespace {

std::vector<StreamInfo>::iterator findStream(std::vector<StreamInfo>& streams, const std::string& id) {
    return std::find_if(streams.begin(), streams.end(),
                        [&](const StreamInfo& stream) { return stream.id == id; });
}

std::string effectiveLayout(const std::string& setting, int streamCount) {
    if (setting.empty() || setting == "auto") {
        return LayoutEngine::suggestLayout(streamCount);
    }
    return setting;
}

void logRestart(const StreamInfo& stream) {
    // Decoder open/close lands in a later milestone. Callers still get a stable id/url/mute entry.
    std::cout << "Restart stream: " << stream.id << " -> " << stream.url
              << " (reconnect requested; decoder not started in this build)\n";
}

}  // namespace

bool StreamManager::addStream(const std::string& id, const std::string& url) {
    if (id.empty() || url.empty()) {
        std::cerr << "Rejecting stream with empty id or url\n";
        return false;
    }

    std::lock_guard<std::mutex> lock(mu_);
    auto it = findStream(streams_, id);
    if (it != streams_.end()) {
        if (it->url == url) {
            std::cout << "Stream already present: " << id << "\n";
            return true;
        }
        std::cerr << "Stream already exists with a different url: " << id << "\n";
        return false;
    }

    streams_.push_back(StreamInfo{id, url, false});
    std::cout << "Adding stream: " << id << " -> " << url << "\n";
    return true;
}

bool StreamManager::removeStream(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = findStream(streams_, id);
    if (it == streams_.end()) {
        std::cerr << "Stream not found: " << id << "\n";
        return false;
    }
    std::cout << "Removing stream: " << id << "\n";
    streams_.erase(it);
    return true;
}

std::vector<StreamInfo> StreamManager::list() const {
    std::lock_guard<std::mutex> lock(mu_);
    return streams_;
}

bool StreamManager::muteStream(const std::string& id, bool muted) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = findStream(streams_, id);
    if (it == streams_.end()) {
        std::cerr << "Stream not found: " << id << "\n";
        return false;
    }
    it->muted = muted;
    std::cout << "Mute stream: " << id << " muted=" << (muted ? "true" : "false") << "\n";
    return true;
}

bool StreamManager::restartStream(const std::string& id) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = findStream(streams_, id);
    if (it == streams_.end()) {
        std::cerr << "Stream not found: " << id << "\n";
        return false;
    }
    logRestart(*it);
    return true;
}

int StreamManager::restartAll() {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto& stream : streams_) {
        logRestart(stream);
    }
    return static_cast<int>(streams_.size());
}

bool StreamManager::setLayout(const std::string& layout) {
    if (layout.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    layout_ = layout;
    std::cout << "Layout setting: " << layout_ << "\n";
    return true;
}

std::string StreamManager::layout() const {
    std::lock_guard<std::mutex> lock(mu_);
    return effectiveLayout(layout_, static_cast<int>(streams_.size()));
}

std::string StreamManager::layoutSetting() const {
    std::lock_guard<std::mutex> lock(mu_);
    return layout_;
}
