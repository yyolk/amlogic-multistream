#pragma once

#include "decoder_backend.h"
#include "software_renderer.h"
#include "stream_manager.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Software decode and present for the streams in a StreamManager.
//
// tick() is the player loop body. It is called on one thread and does not
// talk to MQTT. Each visible stream is opened and decoded on its own thread.
// tick() presents at most one frame that thread has already produced and
// does not wait on socket I/O, so one stalled or live-edge read cannot
// freeze the other tiles. stop, remove, restart, and shutdown call
// DecoderBackend::abort() so a blocked open or read returns, then join
// that thread.
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

    // Sync decoders with the stream table. When playing is true, present one
    // already-decoded frame from each stream that has a layout cell.
    // Returns how many frames were presented. Zero means nothing was ready
    // this call; it does not mean the decoders are stopped.
    int tick(bool playing);

    // Abort and join every decoder thread. Safe to call more than once.
    void closeAll();

    bool ready() const { return ready_; }

private:
    // Bytes copied out of a DecodedFrame so the decode thread can read the
    // next frame. Plane pointers in view() stay valid for the life of this
    // object.
    struct OwnedFrame {
        int width = 0;
        int height = 0;
        int linesize[3] = {};
        int time_base_num = 0;
        int time_base_den = 0;
        int64_t pts = kDecodedFrameNoPts;
        std::vector<uint8_t> plane[3];

        DecodedFrame view() const {
            DecodedFrame frame;
            frame.format = FramePixelFormat::Yuv420p;
            frame.width = width;
            frame.height = height;
            frame.pts = pts;
            frame.time_base_num = time_base_num;
            frame.time_base_den = time_base_den;
            for (int i = 0; i < 3; ++i) {
                frame.data[i] = plane[i].empty() ? nullptr : plane[i].data();
                frame.linesize[i] = linesize[i];
            }
            return frame;
        }
    };

    enum class WorkerFail { None, Open, Read, Hardware };

    struct Session {
        Session() = default;
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        std::string url;
        uint64_t appliedGeneration = 0;
        bool haveGeneration = false;
        std::unique_ptr<DecoderBackend> decoder;
        bool backoff = false;
        bool loggedRefusal = false;
        int framesPresented = 0;
        std::chrono::steady_clock::time_point retryAfter{};

        std::thread worker;
        std::mutex mu;
        std::condition_variable cv;
        bool workerStop = false;
        bool workerRunning = false;
        bool announcedOpen = false;
        bool frameReady = false;
        WorkerFail workerFailed = WorkerFail::None;
        OwnedFrame frame;
    };

    static bool urlAllowed(const std::string& url);
    static bool copyOwned(const DecodedFrame& src, OwnedFrame& dst);
    void requestStop(Session& session);
    void finishWorker(const std::string& id, Session& session);
    void startWorker(const StreamInfo& info, Session& session);
    void sessionLoop(Session* session, std::string id, std::string url);
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
    std::unordered_map<std::string, std::unique_ptr<Session>> sessions_;
};
