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
    std::cout << "Restart stream: " << stream.id << " -> " << stream.url
              << " (reconnect requested, generation " << stream.restartGeneration << ")\n";
}

bool layoutAccepted(const std::string& layout) {
    if (layout == "auto") {
        return true;
    }
    int rows = 0;
    int cols = 0;
    return LayoutEngine::parseLayout(layout, rows, cols);
}

bool rejectAdd(AddFailure* failure, AddFailure why) {
    if (failure != nullptr) {
        *failure = why;
    }
    return false;
}

}  // namespace

bool StreamManager::addStream(const std::string& id, const std::string& url, AddFailure* failure) {
    if (failure != nullptr) {
        *failure = AddFailure::None;
    }
    if (id.empty() || url.empty()) {
        std::cerr << "Rejecting stream with empty id or url\n";
        return rejectAdd(failure, AddFailure::Empty);
    }

    std::lock_guard<std::mutex> lock(mu_);
    auto it = findStream(streams_, id);
    if (it != streams_.end()) {
        if (it->url == url) {
            std::cout << "Stream already present: " << id << "\n";
            return true;
        }
        std::cerr << "Stream already exists with a different url: " << id << "\n";
        return rejectAdd(failure, AddFailure::Conflict);
    }

    if (streams_.size() >= static_cast<size_t>(LayoutEngine::kMaxStreams)) {
        std::cerr << "Rejecting stream " << id << ": limit of " << LayoutEngine::kMaxStreams
                  << " reached\n";
        return rejectAdd(failure, AddFailure::Limit);
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
    ++it->restartGeneration;
    logRestart(*it);
    return true;
}

int StreamManager::restartAll() {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& stream : streams_) {
        ++stream.restartGeneration;
        logRestart(stream);
    }
    return static_cast<int>(streams_.size());
}

bool StreamManager::setLayout(const std::string& layout) {
    if (!layoutAccepted(layout)) {
        std::cerr << "Rejecting layout \"" << layout << "\"\n";
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
