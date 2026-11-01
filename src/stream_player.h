#pragma once

#include "decoder_backend.h"
#include "software_renderer.h"
#include "stream_manager.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

// Software decode and present for the streams in a StreamManager.
//
// tick() is the player loop body. It is not thread-safe and does not talk
// to MQTT. The controller calls it on the main thread while the broker
// loop runs elsewhere. DecoderBackend and Renderer stay as they are:
// this class opens a DecoderBackend per visible stream and presents each
// CPU frame through SoftwareRenderer into a LayoutEngine cell.
//
// Only http:// and https:// URLs are opened. A local file URL never reaches
// the factory, and a backend that reports isHardware() is closed.
// restartGeneration on a stream closes and reopens that decoder.
// playing == false closes every decoder and clears the viewport.
class StreamPlayer {
public:
    static constexpr int kDefaultViewportWidth = 1280;
    static constexpr int kDefaultViewportHeight = 720;

    using DecoderFactory = std::function<std::unique_ptr<DecoderBackend>()>;

    StreamPlayer(StreamManager& streams,
                 SoftwareRenderer& renderer,
                 DecoderFactory factory,
                 int viewportWidth = kDefaultViewportWidth,
                 int viewportHeight = kDefaultViewportHeight);
    ~StreamPlayer();

    StreamPlayer(const StreamPlayer&) = delete;
    StreamPlayer& operator=(const StreamPlayer&) = delete;

    // Allocate the RGB viewport. False when the size is rejected or the PPM
    // directory cannot be created. tick() does nothing until this succeeds.
    bool init();

    // Sync decoders with the stream table. When playing is true, pull one
    // frame from each stream that has a layout cell and present it.
    // Returns how many frames were presented.
    int tick(bool playing);

    // Close every decoder. Safe to call more than once.
    void closeAll();

    bool ready() const { return ready_; }

private:
    struct Session {
        std::string url;
        uint64_t appliedGeneration = 0;
        bool haveGeneration = false;
        std::unique_ptr<DecoderBackend> decoder;
        bool open = false;
        bool backoff = false;
        bool loggedRefusal = false;
        int framesPresented = 0;
        std::chrono::steady_clock::time_point retryAfter{};
    };

    static bool urlAllowed(const std::string& url);
    void closeSession(const std::string& id, Session& session);
    bool ensureOpen(const StreamInfo& info, Session& session);
    void dropMissing(const std::vector<StreamInfo>& streams);
    static std::string topologyKey(const LayoutEngine::Result& layout,
                                   const std::vector<StreamInfo>& streams);

    StreamManager& streams_;
    SoftwareRenderer& renderer_;
    DecoderFactory factory_;
    int width_ = 0;
    int height_ = 0;
    bool ready_ = false;
    bool wasPlaying_ = false;
    bool layoutErrorLogged_ = false;
    std::string topology_;
    std::unordered_map<std::string, Session> sessions_;
};
