#include "stream_player.h"

#include "layout_engine.h"

#include <iostream>
#include <utility>
#include <vector>

namespace {

constexpr std::chrono::milliseconds kRetryDelay{1000};

}  // namespace

StreamPlayer::StreamPlayer(StreamManager& streams,
                           SoftwareRenderer& renderer,
                           DecoderFactory factory,
                           int viewportWidth,
                           int viewportHeight)
    : streams_(streams),
      renderer_(renderer),
      factory_(std::move(factory)),
      width_(viewportWidth),
      height_(viewportHeight) {}

StreamPlayer::~StreamPlayer() {
    closeAll();
}

bool StreamPlayer::init() {
    ready_ = false;
    if (!factory_) {
        std::cerr << "StreamPlayer: no decoder factory\n";
        return false;
    }
    ready_ = renderer_.init(width_, height_);
    if (!ready_) {
        return false;
    }
    std::cout << "StreamPlayer viewport " << width_ << "x" << height_;
    if (renderer_.ppmDirectory().empty()) {
        std::cout << " ppm=off\n";
    } else {
        std::cout << " ppm=" << renderer_.ppmDirectory() << "\n";
    }
    return true;
}

bool StreamPlayer::urlAllowed(const std::string& url) {
    if (url.find('\0') != std::string::npos) {
        return false;
    }
    return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
}

void StreamPlayer::closeSession(const std::string& id, Session& session) {
    if (!session.decoder) {
        session.open = false;
        return;
    }
    if (session.open) {
        std::cout << "StreamPlayer close " << id << "\n";
    }
    session.decoder->close();
    session.decoder.reset();
    session.open = false;
}

void StreamPlayer::closeAll() {
    for (auto& entry : sessions_) {
        closeSession(entry.first, entry.second);
    }
    sessions_.clear();
}

void StreamPlayer::dropMissing(const std::vector<StreamInfo>& streams) {
    std::vector<std::string> stale;
    for (const auto& entry : sessions_) {
        bool found = false;
        for (const auto& info : streams) {
            if (info.id == entry.first) {
                found = true;
                break;
            }
        }
        if (!found) {
            stale.push_back(entry.first);
        }
    }
    for (const auto& id : stale) {
        auto it = sessions_.find(id);
        if (it == sessions_.end()) {
            continue;
        }
        closeSession(id, it->second);
        sessions_.erase(it);
    }
}

std::string StreamPlayer::topologyKey(const LayoutEngine::Result& layout,
                                      const std::vector<StreamInfo>& streams) {
    std::string key = layout.name;
    key.push_back('|');
    const size_t n = streams.size() < layout.tiles.size() ? streams.size() : layout.tiles.size();
    for (size_t i = 0; i < n; ++i) {
        const LayoutEngine::Rect& rect = layout.tiles[i].rect;
        key += streams[i].id;
        key.push_back('@');
        key += std::to_string(rect.x);
        key.push_back(',');
        key += std::to_string(rect.y);
        key.push_back(',');
        key += std::to_string(rect.width);
        key.push_back(',');
        key += std::to_string(rect.height);
        key.push_back(';');
    }
    return key;
}

bool StreamPlayer::ensureOpen(const StreamInfo& info, Session& session) {
    const bool generationChanged =
        !session.haveGeneration || session.appliedGeneration != info.restartGeneration;
    const bool urlChanged = session.url != info.url;
    if (session.open && !generationChanged && !urlChanged) {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!generationChanged && !urlChanged && session.backoff && now < session.retryAfter) {
        return false;
    }

    closeSession(info.id, session);
    session.url = info.url;
    session.haveGeneration = true;
    session.appliedGeneration = info.restartGeneration;
    session.framesPresented = 0;
    if (generationChanged) {
        session.loggedRefusal = false;
    }

    if (!urlAllowed(info.url)) {
        if (!session.loggedRefusal) {
            std::cerr << "StreamPlayer: refusing url for " << info.id
                      << " (http:// or https:// required)\n";
            session.loggedRefusal = true;
        }
        session.backoff = true;
        session.retryAfter = now + kRetryDelay;
        return false;
    }

    session.decoder = factory_ ? factory_() : nullptr;
    if (!session.decoder || !session.decoder->open(info.url) || session.decoder->isHardware()) {
        if (session.decoder && session.decoder->isHardware()) {
            std::cerr << "StreamPlayer: refusing hardware decoder for " << info.id
                      << " (requires GS-King X / CoreELEC)\n";
        } else {
            std::cerr << "StreamPlayer: open failed for " << info.id << "\n";
        }
        closeSession(info.id, session);
        session.backoff = true;
        session.retryAfter = std::chrono::steady_clock::now() + kRetryDelay;
        return false;
    }

    session.open = true;
    session.backoff = false;
    std::cout << "StreamPlayer open " << info.id << "\n";
    return true;
}

int StreamPlayer::tick(bool playing) {
    if (!ready_) {
        return 0;
    }
    if (!playing) {
        if (wasPlaying_) {
            closeAll();
            renderer_.clearViewport();
            topology_.clear();
            wasPlaying_ = false;
        }
        return 0;
    }
    wasPlaying_ = true;

    const std::vector<StreamInfo> streams = streams_.list();
    const std::string setting = streams_.layoutSetting();
    const std::string overrideLayout =
        (setting.empty() || setting == "auto") ? std::string() : setting;
    const LayoutEngine::Result layout = LayoutEngine::compute(
        width_, height_, static_cast<int>(streams.size()), overrideLayout);
    // setLayout only stores "auto" or a name parseLayout accepts, so MQTT
    // cannot reach this branch. It still clears the wall if a setting
    // compute() cannot parse is already stored.
    if (!layout.ok) {
        if (!layoutErrorLogged_) {
            std::cerr << "StreamPlayer: layout '" << setting << "' rejected";
            if (!layout.error.empty()) {
                std::cerr << ": " << layout.error;
            }
            std::cerr << "\n";
            layoutErrorLogged_ = true;
        }
        closeAll();
        renderer_.clearViewport();
        topology_.clear();
        return 0;
    }
    layoutErrorLogged_ = false;

    const std::string key = topologyKey(layout, streams);
    if (key != topology_) {
        renderer_.clearViewport();
        topology_ = std::move(key);
    }

    dropMissing(streams);

    int presented = 0;
    for (size_t i = 0; i < streams.size(); ++i) {
        const StreamInfo& info = streams[i];
        Session& session = sessions_[info.id];
        if (i >= layout.tiles.size()) {
            closeSession(info.id, session);
            continue;
        }
        if (!ensureOpen(info, session)) {
            continue;
        }
        DecodedFrame frame;
        if (!session.decoder->readFrame(frame)) {
            std::cerr << "StreamPlayer: no frame from " << info.id << " (will retry)\n";
            closeSession(info.id, session);
            session.backoff = true;
            session.retryAfter = std::chrono::steady_clock::now() + kRetryDelay;
            continue;
        }
        const LayoutEngine::Rect& dest = layout.tiles[i].rect;
        if (!renderer_.present(frame, dest)) {
            std::cerr << "StreamPlayer: present failed for " << info.id << "\n";
            continue;
        }
        if (session.framesPresented % 60 == 0) {
            std::cout << "presented " << info.id << " frame " << session.framesPresented << " "
                      << frame.width << "x" << frame.height << " tile " << dest.x << "," << dest.y
                      << "+" << dest.width << "x" << dest.height << "\n";
        }
        ++session.framesPresented;
        ++presented;
    }
    return presented;
}
