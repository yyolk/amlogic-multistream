#include "stream_player.h"

#include "layout_engine.h"

#include <cstdio>
#include <exception>
#include <iostream>
#include <new>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr std::chrono::milliseconds kRetryDelay{1000};
constexpr int kMaxPlaneStride = 65536;
constexpr int kMaxPlaneRows = 8192;

}  // namespace

bool StreamPlayer::copyOwned(const DecodedFrame& src, OwnedFrame& dst) {
    if (src.format != FramePixelFormat::Yuv420p || src.width <= 0 || src.height <= 0) {
        return false;
    }
    const int chromaRows = (src.height + 1) / 2;
    const int rows[3] = {src.height, chromaRows, chromaRows};
    OwnedFrame copy;
    copy.width = src.width;
    copy.height = src.height;
    copy.pts = src.pts;
    copy.time_base_num = src.time_base_num;
    copy.time_base_den = src.time_base_den;
    try {
        for (int i = 0; i < 3; ++i) {
            if (src.data[i] == nullptr || src.linesize[i] <= 0 || src.linesize[i] > kMaxPlaneStride ||
                rows[i] <= 0 || rows[i] > kMaxPlaneRows) {
                return false;
            }
            const auto bytes = static_cast<size_t>(src.linesize[i]) * static_cast<size_t>(rows[i]);
            copy.plane[i].assign(src.data[i], src.data[i] + bytes);
            copy.linesize[i] = src.linesize[i];
        }
    } catch (const std::bad_alloc&) {
        return false;
    }
    dst = std::move(copy);
    return true;
}

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
    // A worker still inside getaddrinfo cannot be joined here. Signal exit,
    // a failed sigtimedwait, and authentication give-up in MQTT mode all
    // _exit from MqttController::finishProcess, so this destructor does not
    // run there. The joins below only see workers that have already returned.
    for (auto& worker : reaper_) {
        if (worker && worker->thread.joinable()) {
            worker->thread.join();
        }
        if (worker) {
            releaseDecoder(*worker);
        }
    }
    reaper_.clear();
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

void StreamPlayer::requestStop(Worker& worker) {
    {
        std::lock_guard<std::mutex> lock(worker.mu);
        worker.stop = true;
    }
    worker.cv.notify_all();
    if (worker.decoder) {
        worker.decoder->abort();
    }
}

void StreamPlayer::releaseDecoder(Worker& worker) {
    if (!worker.decoder) {
        return;
    }
    if (worker.announcedOpen) {
        std::cout << "StreamPlayer close " << worker.id << "\n";
        worker.announcedOpen = false;
    }
    worker.decoder->close();
    worker.decoder.reset();
}

void StreamPlayer::retireWorker(Session& session) {
    if (!session.worker) {
        return;
    }
    requestStop(*session.worker);
    reaper_.push_back(std::move(session.worker));
}

void StreamPlayer::reapFinished() {
    std::vector<std::unique_ptr<Worker>> pending;
    for (auto& worker : reaper_) {
        if (!worker) {
            continue;
        }
        bool done = false;
        {
            std::lock_guard<std::mutex> lock(worker->mu);
            done = worker->done;
        }
        if (!done) {
            pending.push_back(std::move(worker));
            continue;
        }
        if (worker->thread.joinable()) {
            worker->thread.join();
        }
        releaseDecoder(*worker);
    }
    reaper_ = std::move(pending);
}

bool StreamPlayer::reaperBusy(const std::string& id) const {
    for (const auto& worker : reaper_) {
        if (!worker || worker->id != id) {
            continue;
        }
        std::lock_guard<std::mutex> lock(worker->mu);
        if (!worker->done) {
            return true;
        }
    }
    return false;
}

int StreamPlayer::busyRetiredCount() const {
    int busy = 0;
    for (const auto& worker : reaper_) {
        if (!worker) {
            continue;
        }
        std::lock_guard<std::mutex> lock(worker->mu);
        if (!worker->done) {
            ++busy;
        }
    }
    return busy;
}

void StreamPlayer::sessionLoop(Worker* worker, const std::string& id, const std::string& url) {
    DecoderBackend* decoder = worker->decoder.get();
    const bool opened = decoder != nullptr && decoder->open(url);
    const bool hardware = opened && decoder->isHardware();
    if (!opened || hardware) {
        std::lock_guard<std::mutex> lock(worker->mu);
        if (!worker->stop) {
            worker->failed = hardware ? WorkerFail::Hardware : WorkerFail::Open;
        }
        worker->cv.notify_all();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(worker->mu);
        if (worker->stop) {
            return;
        }
        worker->announcedOpen = true;
    }
    std::cout << "StreamPlayer open " << id << "\n";

    while (true) {
        {
            std::unique_lock<std::mutex> lock(worker->mu);
            worker->cv.wait(lock, [&] { return worker->stop || !worker->frameReady; });
            if (worker->stop) {
                return;
            }
        }

        DecodedFrame frame;
        const bool got = decoder->readFrame(frame);
        OwnedFrame copy;
        const bool copied = got && copyOwned(frame, copy);
        {
            std::lock_guard<std::mutex> lock(worker->mu);
            if (worker->stop) {
                return;
            }
            if (!copied) {
                worker->failed = WorkerFail::Read;
                worker->cv.notify_all();
                return;
            }
            worker->frame = std::move(copy);
            worker->frameReady = true;
        }
        worker->cv.notify_all();
    }
}

void StreamPlayer::startWorker(const StreamInfo& info, Session& session) {
    if (session.worker) {
        retireWorker(session);
    }
    session.url = info.url;
    session.haveGeneration = true;
    session.appliedGeneration = info.restartGeneration;
    session.framesPresented = 0;
    auto worker = std::make_unique<Worker>();
    worker->id = info.id;
    worker->decoder = factory_ ? factory_() : nullptr;
    if (!worker->decoder) {
        std::cerr << "StreamPlayer: open failed for " << info.id << "\n";
        session.backoff = true;
        session.retryAfter = std::chrono::steady_clock::now() + kRetryDelay;
        return;
    }
    Worker* raw = worker.get();
    const std::string url = info.url;
    const std::string id = info.id;
    try {
        worker->thread = std::thread([this, raw, url, id] {
            try {
                sessionLoop(raw, id, url);
            } catch (const std::exception& ex) {
                std::cerr << "StreamPlayer: worker failed for " << id << ": " << ex.what() << "\n";
            } catch (...) {
                std::cerr << "StreamPlayer: worker failed for " << id << "\n";
            }
            {
                std::lock_guard<std::mutex> lock(raw->mu);
                raw->done = true;
            }
            raw->cv.notify_all();
        });
    } catch (const std::system_error& ex) {
        std::cerr << "StreamPlayer: thread failed for " << id << ": " << ex.what() << "\n";
        worker->decoder->close();
        worker->decoder.reset();
        session.backoff = true;
        session.retryAfter = std::chrono::steady_clock::now() + kRetryDelay;
        return;
    }
    session.worker = std::move(worker);
}

void StreamPlayer::closeAll() {
    for (auto& entry : sessions_) {
        retireWorker(*entry.second);
    }
    sessions_.clear();
    reapFinished();
}

bool StreamPlayer::drain(std::chrono::milliseconds deadline) {
    const auto deadlineAt = std::chrono::steady_clock::now() + deadline;
    while (true) {
        reapFinished();
        if (reaper_.empty()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadlineAt) {
            return false;
        }
        Worker& worker = *reaper_.front();
        std::unique_lock<std::mutex> lock(worker.mu);
        if (worker.done) {
            continue;
        }
        worker.cv.wait_until(lock, deadlineAt, [&] { return worker.done; });
    }
}

void StreamPlayer::exitIfBusy(std::chrono::milliseconds deadline, int exitCode) {
    if (drain(deadline)) {
        return;
    }
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
    _exit(exitCode);
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
        retireWorker(*it->second);
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

int StreamPlayer::tick(bool playing) {
    if (!ready_) {
        return 0;
    }
    reapFinished();
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
        topology_ = key;
    }

    dropMissing(streams);

    int presented = 0;
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < streams.size(); ++i) {
        const StreamInfo& info = streams[i];
        auto it = sessions_.find(info.id);
        if (it == sessions_.end()) {
            it = sessions_.emplace(info.id, std::make_unique<Session>()).first;
        }
        Session& session = *it->second;

        if (i >= layout.tiles.size()) {
            retireWorker(session);
            continue;
        }

        const bool generationChanged =
            !session.haveGeneration || session.appliedGeneration != info.restartGeneration;
        const bool urlChanged = session.haveGeneration && session.url != info.url;
        if (session.worker && (generationChanged || urlChanged)) {
            retireWorker(session);
        }

        if (!session.worker) {
            // AUDIT4 A3: generationChanged used to skip backoff and start a
            // replacement in this same tick. A retired worker stuck in
            // getaddrinfo ignores abort and stays on the reaper until libc
            // returns, so each restart stacked another thread. Wait until
            // that worker is done. appliedGeneration is left unchanged, so
            // the restart is still pending and runs on the tick after the
            // lookup finishes. A restart cannot beat the resolver anyway.
            if (reaperBusy(info.id)) {
                continue;
            }
            // AUDIT5 F3: the check above is per id, so add/remove churn under
            // new ids still stacked one stuck lookup each. Leave the session
            // pending, like the per-id case, until the reaper drains.
            const int busyRetired = busyRetiredCount();
            if (busyRetired >= kMaxBusyRetired) {
                if (!retiredLimitLogged_) {
                    std::cerr << "StreamPlayer: " << busyRetired
                              << " retired decoders still blocked; new opens wait\n";
                    retiredLimitLogged_ = true;
                }
                continue;
            }
            retiredLimitLogged_ = false;
            if (!generationChanged && !urlChanged && session.backoff && now < session.retryAfter) {
                continue;
            }
            if (generationChanged) {
                session.loggedRefusal = false;
            }
            session.url = info.url;
            session.haveGeneration = true;
            session.appliedGeneration = info.restartGeneration;
            if (!urlAllowed(info.url)) {
                if (!session.loggedRefusal) {
                    std::cerr << "StreamPlayer: refusing url for " << info.id
                              << " (http:// or https:// required)\n";
                    session.loggedRefusal = true;
                }
                session.backoff = true;
                session.retryAfter = now + kRetryDelay;
                continue;
            }
            startWorker(info, session);
        }

        if (!session.worker) {
            continue;
        }

        OwnedFrame taken;
        bool haveFrame = false;
        WorkerFail failed = WorkerFail::None;
        {
            std::lock_guard<std::mutex> lock(session.worker->mu);
            if (session.worker->frameReady) {
                taken = std::move(session.worker->frame);
                session.worker->frameReady = false;
                haveFrame = true;
            } else if (session.worker->failed != WorkerFail::None) {
                failed = session.worker->failed;
            }
        }
        if (haveFrame) {
            session.worker->cv.notify_all();
            const LayoutEngine::Rect& dest = layout.tiles[i].rect;
            const DecodedFrame view = taken.view();
            if (!renderer_.present(view, dest)) {
                std::cerr << "StreamPlayer: present failed for " << info.id << "\n";
                continue;
            }
            if (session.framesPresented % 60 == 0) {
                std::cout << "presented " << info.id << " frame " << session.framesPresented << " "
                          << view.width << "x" << view.height << " tile " << dest.x << "," << dest.y
                          << "+" << dest.width << "x" << dest.height << "\n";
            }
            ++session.framesPresented;
            ++presented;
            continue;
        }
        if (failed == WorkerFail::None) {
            continue;
        }
        if (failed == WorkerFail::Hardware) {
            std::cerr << "StreamPlayer: refusing hardware decoder for " << info.id
                      << " (requires GS-King X / CoreELEC)\n";
        } else if (failed == WorkerFail::Open) {
            std::cerr << "StreamPlayer: open failed for " << info.id << "\n";
        } else {
            std::cerr << "StreamPlayer: no frame from " << info.id << " (will retry)\n";
        }
        retireWorker(session);
        session.backoff = true;
        session.retryAfter = std::chrono::steady_clock::now() + kRetryDelay;
    }
    reapFinished();
    return presented;
}
