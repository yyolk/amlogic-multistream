#include "mqtt_commands.h"
#include "mqtt_json.h"
#include "software_renderer.h"
#include "stream_manager.h"
#include "stream_player.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int g_failed = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::cerr << "FAIL " << file << ":" << line << " " << expr << "\n";
        ++g_failed;
    }
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

struct Stats {
    std::atomic<int> attempts{0};
    std::atomic<int> opened{0};
    std::atomic<int> closed{0};
    std::atomic<int> reads{0};
    std::atomic<int> failRemaining{0};
    std::atomic<bool> hardware{false};
};

class FakeDecoder : public DecoderBackend {
public:
    explicit FakeDecoder(std::shared_ptr<Stats> stats) : stats_(std::move(stats)) {
        y_.assign(16 * 16, 235);
        u_.assign(8 * 8, 128);
        v_.assign(8 * 8, 128);
    }

    ~FakeDecoder() override { close(); }

    bool open(const std::string& url) override {
        stats_->attempts.fetch_add(1);
        int remaining = stats_->failRemaining.load();
        if (remaining > 0 && stats_->failRemaining.compare_exchange_strong(remaining, remaining - 1)) {
            return false;
        }
        if (url.find("right") != std::string::npos) {
            std::fill(y_.begin(), y_.end(), static_cast<uint8_t>(128));
            std::fill(u_.begin(), u_.end(), static_cast<uint8_t>(240));
            std::fill(v_.begin(), v_.end(), static_cast<uint8_t>(128));
        } else {
            std::fill(y_.begin(), y_.end(), static_cast<uint8_t>(235));
            std::fill(u_.begin(), u_.end(), static_cast<uint8_t>(128));
            std::fill(v_.begin(), v_.end(), static_cast<uint8_t>(128));
        }
        opened_ = true;
        stats_->opened.fetch_add(1);
        return true;
    }

    void close() override {
        if (!opened_) {
            return;
        }
        opened_ = false;
        stats_->closed.fetch_add(1);
    }

    bool isHardware() const override { return stats_->hardware.load(); }

    bool readFrame(DecodedFrame& frame) override {
        if (!opened_) {
            return false;
        }
        frame = DecodedFrame{};
        frame.format = FramePixelFormat::Yuv420p;
        frame.width = 16;
        frame.height = 16;
        frame.data[0] = y_.data();
        frame.data[1] = u_.data();
        frame.data[2] = v_.data();
        frame.linesize[0] = 16;
        frame.linesize[1] = 8;
        frame.linesize[2] = 8;
        stats_->reads.fetch_add(1);
        return true;
    }

private:
    std::shared_ptr<Stats> stats_;
    bool opened_ = false;
    std::vector<uint8_t> y_;
    std::vector<uint8_t> u_;
    std::vector<uint8_t> v_;
};

struct Harness {
    std::shared_ptr<Stats> stats = std::make_shared<Stats>();
    StreamManager streams;
    SoftwareRenderer renderer;
    StreamPlayer player;

    Harness(std::string ppmDir, int width, int height)
        : renderer(std::move(ppmDir)),
          player(streams, renderer,
                 [this]() -> std::unique_ptr<DecoderBackend> {
                     return std::make_unique<FakeDecoder>(stats);
                 },
                 width, height) {}
};

Rgb8 at(const SoftwareRenderer& renderer, int x, int y) {
    const auto& pixels = renderer.pixels();
    const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(renderer.viewportWidth()) +
                      static_cast<size_t>(x)) *
                     3;
    return Rgb8{pixels[i], pixels[i + 1], pixels[i + 2]};
}

bool same(Rgb8 a, Rgb8 b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

bool allZero(const SoftwareRenderer& renderer) {
    for (uint8_t byte : renderer.pixels()) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

// Decode runs on a stream thread, so a single tick() can return before the
// fake decoder has published. Poll until the predicate holds.
template <typename Pred>
bool until(Pred pred, int timeoutMs = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

int pump(StreamPlayer& player, int minPresented, int timeoutMs = 2000) {
    int last = 0;
    until([&] {
        last = player.tick(true);
        return last >= minPresented;
    }, timeoutMs);
    return last;
}

}  // namespace

int main() {
    const Rgb8 leftColor = yuv601ToRgb(235, 128, 128);
    const Rgb8 rightColor = yuv601ToRgb(128, 240, 128);

    {
        SoftwareRenderer renderer;
        StreamManager streams;
        StreamPlayer player(streams, renderer, []() -> std::unique_ptr<DecoderBackend> {
            return std::make_unique<FakeDecoder>(std::make_shared<Stats>());
        }, 0, 16);
        CHECK(!player.init());
        CHECK(!player.ready());
        CHECK(player.tick(true) == 0);
    }

    {
        Harness harness({}, 32, 16);
        CHECK(harness.player.init());
        CHECK(harness.player.tick(false) == 0);
        CHECK(harness.stats->attempts.load() == 0);
        CHECK(harness.streams.addStream("left", "http://example/left.ts"));
        CHECK(harness.player.tick(false) == 0);
        CHECK(harness.stats->attempts.load() == 0);
        CHECK(harness.renderer.framesPresented() == 0);
        CHECK(harness.renderer.lastPpmPath().empty());

        CHECK(pump(harness.player, 1) == 1);
        CHECK(harness.stats->opened.load() == 1);
        CHECK(harness.stats->reads.load() >= 1);
        CHECK(harness.renderer.framesPresented() >= 1);
        CHECK(same(at(harness.renderer, 8, 8), leftColor));

        const int presentedAfterFirst = harness.renderer.framesPresented();
        CHECK(harness.streams.addStream("left", "http://example/left.ts"));
        CHECK(pump(harness.player, 1) == 1);
        CHECK(harness.stats->opened.load() == 1);
        CHECK(harness.renderer.framesPresented() > presentedAfterFirst);

        CHECK(harness.streams.addStream("right", "http://example/right.ts"));
        CHECK(pump(harness.player, 2) == 2);
        CHECK(harness.stats->opened.load() == 2);
        CHECK(same(at(harness.renderer, 8, 8), leftColor));
        CHECK(same(at(harness.renderer, 24, 8), rightColor));

        const int opens = harness.stats->opened.load();
        CHECK(!harness.streams.restartStream("typo"));
        CHECK(harness.streams.list()[0].restartGeneration == 0);
        CHECK(harness.streams.list()[1].restartGeneration == 0);
        CHECK(pump(harness.player, 2) == 2);
        CHECK(harness.stats->opened.load() == opens);

        CHECK(harness.streams.restartStream("left"));
        CHECK(harness.streams.list()[0].restartGeneration == 1);
        CHECK(harness.streams.list()[1].restartGeneration == 0);
        CHECK(until([&] {
            return pump(harness.player, 2) == 2 && harness.stats->opened.load() == opens + 1;
        }));
        CHECK(harness.stats->opened.load() == opens + 1);
        CHECK(same(at(harness.renderer, 8, 8), leftColor));
        CHECK(same(at(harness.renderer, 24, 8), rightColor));

        CHECK(harness.streams.restartAll() == 2);
        CHECK(until([&] {
            return pump(harness.player, 2) == 2 && harness.stats->opened.load() == opens + 1 + 2;
        }));
        CHECK(harness.stats->opened.load() == opens + 1 + 2);

        CHECK(harness.streams.removeStream("right"));
        CHECK(until([&] {
            harness.player.tick(true);
            return same(at(harness.renderer, 8, 8), leftColor) &&
                   same(at(harness.renderer, 24, 8), Rgb8{});
        }));
        CHECK(same(at(harness.renderer, 8, 8), leftColor));
        CHECK(same(at(harness.renderer, 24, 8), Rgb8{}));

        const int presented = harness.renderer.framesPresented();
        harness.player.tick(false);
        harness.player.tick(false);
        CHECK(harness.renderer.framesPresented() == presented);
        CHECK(allZero(harness.renderer));
        const int openedBeforeResume = harness.stats->opened.load();
        CHECK(pump(harness.player, 1) == 1);
        CHECK(harness.stats->opened.load() == openedBeforeResume + 1);
        CHECK(same(at(harness.renderer, 8, 8), leftColor));
    }

    {
        Harness harness({}, 32, 32);
        CHECK(harness.player.init());
        CHECK(harness.streams.addStream("left", "http://example/left.ts"));
        CHECK(harness.streams.addStream("right", "http://example/right.ts"));
        CHECK(harness.streams.setLayout("2x2"));
        CHECK(pump(harness.player, 2) == 2);
        CHECK(same(at(harness.renderer, 8, 8), leftColor));
        CHECK(same(at(harness.renderer, 24, 8), rightColor));
        CHECK(same(at(harness.renderer, 8, 24), Rgb8{}));
        CHECK(same(at(harness.renderer, 24, 24), Rgb8{}));

        CHECK(harness.streams.setLayout("1x1"));
        const int opened = harness.stats->opened.load();
        CHECK(until([&] {
            harness.player.tick(true);
            return same(at(harness.renderer, 8, 8), leftColor);
        }));
        CHECK(harness.stats->opened.load() == opened);
        CHECK(same(at(harness.renderer, 8, 8), leftColor));

        // An invalid token is not stored, so the next tick keeps the 1x1 wall.
        const int presented = harness.renderer.framesPresented();
        CHECK(!harness.streams.setLayout("nope"));
        CHECK(harness.streams.layout() == "1x1");
        CHECK(until([&] {
            harness.player.tick(true);
            return harness.renderer.framesPresented() > presented;
        }));
        CHECK(harness.renderer.framesPresented() > presented);
        CHECK(same(at(harness.renderer, 8, 8), leftColor));
        CHECK(!allZero(harness.renderer));
    }

    {
        Harness harness({}, 32, 16);
        CHECK(harness.player.init());
        harness.stats->failRemaining.store(1);
        CHECK(harness.streams.addStream("left", "https://example/left.ts"));
        CHECK(until([&] {
            harness.player.tick(true);
            return harness.stats->attempts.load() >= 1;
        }));
        for (int i = 0; i < 20; ++i) {
            CHECK(harness.player.tick(true) == 0);
        }
        CHECK(harness.stats->attempts.load() == 1);
        CHECK(harness.stats->opened.load() == 0);
        CHECK(harness.streams.restartStream("left"));
        CHECK(pump(harness.player, 1) == 1);
        CHECK(harness.stats->attempts.load() == 2);
        CHECK(harness.stats->opened.load() == 1);
    }

    {
        Harness harness({}, 32, 16);
        CHECK(harness.player.init());
        harness.stats->hardware.store(true);
        CHECK(harness.streams.addStream("left", "http://example/left.ts"));
        CHECK(until([&] {
            harness.player.tick(true);
            return harness.stats->closed.load() >= 1;
        }));
        CHECK(harness.stats->opened.load() == 1);
        CHECK(harness.stats->closed.load() == 1);
        CHECK(harness.renderer.framesPresented() == 0);
        for (int i = 0; i < 20; ++i) {
            CHECK(harness.player.tick(true) == 0);
        }
        CHECK(harness.stats->attempts.load() == 1);
    }

    {
        Harness harness({}, 32, 16);
        CHECK(harness.player.init());
        CHECK(harness.streams.addStream("bad", "file:///tmp/x.ts"));
        std::string nul = "http://example/a";
        nul.push_back('\0');
        CHECK(harness.streams.addStream("nul", nul));
        CHECK(harness.player.tick(true) == 0);
        CHECK(harness.stats->attempts.load() == 0);
        CHECK(harness.renderer.framesPresented() == 0);
    }

    {
        const auto dir = std::filesystem::temp_directory_path() /
                         ("amlogic-stream-player-" + std::to_string(static_cast<long long>(::getpid())));
        std::filesystem::remove_all(dir);
        Harness harness(dir.string(), 32, 16);
        CHECK(harness.player.init());
        CHECK(harness.streams.addStream("left", "http://example/left.ts"));
        CHECK(pump(harness.player, 1) == 1);
        CHECK(!harness.renderer.lastPpmPath().empty());
        CHECK(std::filesystem::exists(harness.renderer.lastPpmPath()));
        std::filesystem::remove_all(dir);
    }

    {
        // Command path used by the MQTT callback, with no broker socket.
        Harness harness({}, 32, 16);
        CHECK(harness.player.init());
        bool playing = false;
        const JsonObject fileUrl = parseFlatJsonObject(R"({"id":"left","url":"file:///tmp/x.ts"})");
        CHECK(fileUrl.valid);
        auto denied = executeCommand(harness.streams, playing, "add", fileUrl);
        CHECK(!denied.ok);
        CHECK(harness.player.tick(true) == 0);
        CHECK(harness.stats->attempts.load() == 0);
        CHECK(harness.streams.list().empty());

        const JsonObject httpUrl =
            parseFlatJsonObject(R"({"id":"left","url":"http://example/left.ts"})");
        CHECK(httpUrl.valid);
        auto added = executeCommand(harness.streams, playing, "add", httpUrl);
        CHECK(added.ok && added.message == "added");
        CHECK(harness.player.tick(false) == 0);
        CHECK(harness.stats->attempts.load() == 0);

        auto play = executeCommand(harness.streams, playing, "play", parseFlatJsonObject("{}"));
        CHECK(play.ok && playing);
        CHECK(pump(harness.player, 1) == 1);
        CHECK(same(at(harness.renderer, 8, 8), leftColor));

        auto missing = executeCommand(harness.streams, playing, "restart",
                                      parseFlatJsonObject(R"({"id":"nope"})"));
        CHECK(!missing.ok && missing.message == "stream not found");
        CHECK(harness.streams.list()[0].restartGeneration == 0);
        const int opened = harness.stats->opened.load();
        auto restart = executeCommand(harness.streams, playing, "restart",
                                      parseFlatJsonObject(R"({"id":"left"})"));
        CHECK(restart.ok && restart.message == "restart requested");
        CHECK(until([&] {
            return pump(harness.player, 1) == 1 && harness.stats->opened.load() == opened + 1;
        }));
        CHECK(harness.stats->opened.load() == opened + 1);
        CHECK(harness.streams.list()[0].restartGeneration == 1);

        auto stop = executeCommand(harness.streams, playing, "stop", parseFlatJsonObject("{}"));
        CHECK(stop.ok && !playing);
        const int presented = harness.renderer.framesPresented();
        CHECK(harness.player.tick(playing) == 0);
        CHECK(harness.renderer.framesPresented() == presented);
        CHECK(allZero(harness.renderer));
    }

    if (g_failed != 0) {
        std::cerr << g_failed << " check(s) failed\n";
        return 1;
    }
    std::cout << "stream player test passed\n";
    return 0;
}
