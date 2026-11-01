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
// freeze the other tiles.
//
// stop, remove, restart, a layout cell that hides a tile, and shutdown call
// DecoderBackend::abort() and move that worker onto a reaper list. tick()
// joins a worker only after its loop has returned. It never joins an
// in-flight open or read. FFmpeg's getaddrinfo ignores AVIOInterruptCB, so
// abort() does not unblock a stuck lookup; that worker stays on the reaper
// until the libc call returns. Process exit publishes offline status first,
// waits kExitJoinDeadline, then flushes and _exit(0) if a worker is still
// inside that call.
//
// Only http:// and https:// URLs are opened. A local file URL never reaches
// the factory, and a backend that reports isHardware() is closed.
// restartGeneration on a stream retires that decoder and opens another.
// playing == false retires every decoder and clears the viewport.
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

    // Abort every decoder and move it onto the reaper. Does not join a worker
    // whose loop has not returned. Safe to call more than once.
    void closeAll();

    // Join workers already on the reaper. Wait up to deadline for any whose
    // loop has not returned. False means one is still inside open or read.
    // Sessions still in the map are left alone; call closeAll() first.
    bool drain(std::chrono::milliseconds deadline);

    // drain(). If a worker is still blocked, flush stdio and _exit(0).
    // Call this on process exit after the offline status has been published.
    void exitIfBusy(std::chrono::milliseconds deadline);

    // Shorter than scripts/launch-coreelec.sh's 10s SIGTERM poll. A glibc
    // resolver stall is about 10s per nameserver, so waiting that poll out
    // is what turns into SIGKILL.
    static constexpr std::chrono::milliseconds kExitJoinDeadline{2000};

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

    // One decode thread. The object stays put for the life of the thread:
    // the thread holds a raw pointer, and the unique_ptr that owns it moves
    // from the session onto the reaper.
    struct Worker {
        std::string id;
        std::unique_ptr<DecoderBackend> decoder;
        std::thread thread;
        std::mutex mu;
        std::condition_variable cv;
        bool stop = false;
        // Set only after sessionLoop has returned, on every path.
        bool done = false;
        bool announcedOpen = false;
        bool frameReady = false;
        WorkerFail failed = WorkerFail::None;
        OwnedFrame frame;

        ~Worker() {
            if (thread.joinable()) {
                thread.join();
            }
        }

        Worker() = default;
        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;
    };

    // Bookkeeping that stays with the stream id across decoder restarts.
    // The worker is null once it has been retired.
    struct Session {
        std::string url;
        uint64_t appliedGeneration = 0;
        bool haveGeneration = false;
        bool backoff = false;
        bool loggedRefusal = false;
        int framesPresented = 0;
        std::chrono::steady_clock::time_point retryAfter{};
        std::unique_ptr<Worker> worker;
    };

    static bool urlAllowed(const std::string& url);
    static bool copyOwned(const DecodedFrame& src, OwnedFrame& dst);
    void requestStop(Worker& worker);
    void retireWorker(Session& session);
    void reapFinished();
    void releaseDecoder(Worker& worker);
    void startWorker(const StreamInfo& info, Session& session);
    void sessionLoop(Worker* worker, const std::string& id, const std::string& url);
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
    std::vector<std::unique_ptr<Worker>> reaper_;
};
