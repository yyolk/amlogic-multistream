#include "layout_engine.h"
#include "stream_manager.h"

#include <iostream>
#include <string>

namespace {

int g_failed = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::cerr << "FAIL " << file << ":" << line << " " << expr << "\n";
        ++g_failed;
    }
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

}  // namespace

// In-memory stream table only. No MQTT broker and no decoder.
int main() {
    StreamManager streams;
    CHECK(streams.list().empty());
    CHECK(streams.layoutSetting() == "auto");
    CHECK(streams.layout() == "1x1");

    CHECK(!streams.addStream("", "http://example/a.m3u8"));
    CHECK(!streams.addStream("cam1", ""));
    CHECK(streams.list().empty());

    CHECK(streams.addStream("cam1", "http://example/a.m3u8"));
    CHECK(streams.addStream("cam1", "http://example/a.m3u8"));
    CHECK(streams.list().size() == 1);
    CHECK(!streams.addStream("cam1", "http://example/other.m3u8"));
    CHECK(streams.list().size() == 1);
    CHECK(streams.list()[0].id == "cam1");
    CHECK(streams.list()[0].url == "http://example/a.m3u8");
    CHECK(!streams.list()[0].muted);

    CHECK(streams.addStream("cam2", "http://example/b.m3u8"));
    CHECK(streams.addStream("cam1", "http://example/a.m3u8"));
    auto listed = streams.list();
    CHECK(listed.size() == 2);
    CHECK(listed[0].id == "cam1");
    CHECK(listed[1].id == "cam2");
    listed.clear();
    listed.push_back(StreamInfo{"nope", "nope", true});
    CHECK(streams.list().size() == 2);
    CHECK(streams.list()[0].id == "cam1");
    CHECK(streams.list()[1].id == "cam2");

    CHECK(streams.layout() == "1x2");
    CHECK(!streams.setLayout(""));
    CHECK(streams.layoutSetting() == "auto");
    CHECK(streams.setLayout("2x2"));
    CHECK(streams.layout() == "2x2");
    CHECK(streams.layoutSetting() == "2x2");
    CHECK(!streams.setLayout("banana"));
    CHECK(!streams.setLayout("4"));
    CHECK(!streams.setLayout("2X2"));
    CHECK(!streams.setLayout("AUTO"));
    CHECK(!streams.setLayout("1x4"));
    CHECK(!streams.setLayout("2x2 "));
    CHECK(streams.layout() == "2x2");
    CHECK(streams.layoutSetting() == "2x2");
    CHECK(streams.removeStream("cam2"));
    CHECK(streams.layout() == "2x2");
    CHECK(streams.layoutSetting() == "2x2");
    CHECK(streams.addStream("cam2", "http://example/b.m3u8"));
    CHECK(streams.setLayout("auto"));
    CHECK(streams.layoutSetting() == "auto");
    CHECK(streams.layout() == "1x2");

    CHECK(!streams.muteStream("missing", true));
    CHECK(!streams.list()[0].muted);
    CHECK(streams.muteStream("cam1", true));
    CHECK(streams.list()[0].muted);
    CHECK(!streams.list()[1].muted);
    CHECK(streams.muteStream("cam1", true));
    CHECK(streams.muteStream("cam1", false));
    CHECK(!streams.list()[0].muted);

    CHECK(!streams.restartStream("missing"));
    CHECK(streams.list().size() == 2);
    CHECK(streams.list()[0].restartGeneration == 0);
    CHECK(streams.list()[1].restartGeneration == 0);
    CHECK(streams.muteStream("cam2", true));
    CHECK(streams.restartStream("cam2"));
    CHECK(streams.list().size() == 2);
    CHECK(streams.list()[0].id == "cam1");
    CHECK(streams.list()[1].id == "cam2");
    CHECK(streams.list()[1].muted);
    CHECK(streams.list()[1].url == "http://example/b.m3u8");
    CHECK(streams.list()[0].restartGeneration == 0);
    CHECK(streams.list()[1].restartGeneration == 1);
    CHECK(streams.addStream("cam2", "http://example/b.m3u8"));
    CHECK(streams.list()[1].restartGeneration == 1);
    CHECK(streams.restartAll() == 2);
    CHECK(streams.list().size() == 2);
    CHECK(streams.list()[1].muted);
    CHECK(streams.list()[1].url == "http://example/b.m3u8");
    CHECK(streams.list()[0].restartGeneration == 1);
    CHECK(streams.list()[1].restartGeneration == 2);

    CHECK(!streams.removeStream("missing"));
    CHECK(streams.removeStream("cam1"));
    CHECK(streams.list().size() == 1);
    CHECK(streams.list()[0].id == "cam2");
    CHECK(streams.list()[0].muted);
    CHECK(streams.layout() == "1x1");
    CHECK(streams.removeStream("cam2"));
    CHECK(streams.list().empty());
    CHECK(!streams.removeStream("cam2"));
    CHECK(streams.restartAll() == 0);
    CHECK(!streams.restartStream("cam2"));
    CHECK(!streams.muteStream("cam2", false));

    StreamManager grid;
    for (int i = 0; i < 5; ++i) {
        CHECK(grid.addStream("s" + std::to_string(i), "http://example/" + std::to_string(i)));
    }
    CHECK(grid.list().size() == 5);
    CHECK(grid.layout() == "2x3");
    CHECK(grid.addStream("s5", "http://example/5"));
    CHECK(grid.addStream("s6", "http://example/6"));
    CHECK(grid.list().size() == 7);
    CHECK(grid.layout() == "3x3");
    CHECK(grid.setLayout("1x2"));
    CHECK(grid.layout() == "1x2");
    CHECK(grid.layoutSetting() == "1x2");
    CHECK(grid.setLayout("2x1"));
    CHECK(grid.layout() == "2x1");
    CHECK(grid.layoutSetting() == "2x1");
    CHECK(!grid.setLayout("nope"));
    CHECK(grid.layoutSetting() == "2x1");

    CHECK(grid.addStream("s7", "http://example/7"));
    CHECK(grid.addStream("s8", "http://example/8"));
    CHECK(grid.list().size() == static_cast<size_t>(LayoutEngine::kMaxStreams));
    AddFailure limit = AddFailure::None;
    CHECK(!grid.addStream("s9", "http://example/9", &limit));
    CHECK(limit == AddFailure::Limit);
    CHECK(grid.list().size() == static_cast<size_t>(LayoutEngine::kMaxStreams));
    CHECK(grid.addStream("s0", "http://example/0"));
    AddFailure conflict = AddFailure::None;
    CHECK(!grid.addStream("s0", "http://example/changed", &conflict));
    CHECK(conflict == AddFailure::Conflict);
    CHECK(grid.list()[0].url == "http://example/0");
    CHECK(grid.removeStream("s8"));
    CHECK(grid.addStream("s9", "http://example/9"));
    CHECK(grid.list().size() == static_cast<size_t>(LayoutEngine::kMaxStreams));

    if (g_failed != 0) {
        std::cerr << g_failed << " check(s) failed\n";
        return 1;
    }
    std::cout << "stream manager test passed\n";
    return 0;
}
