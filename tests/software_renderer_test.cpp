#include "layout_engine.h"
#include "software_renderer.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

// Software presentation without DRM, FFmpeg, or a GPU. A fake Renderer
// records the cell LayoutEngine produces for one stream. SoftwareRenderer
// checks the RGB buffer, letterboxing, plane stride, and a PPM dump.

namespace {

int g_failures = 0;
const char* g_case = "";

void check(bool cond, const char* expr, int line) {
    if (cond) {
        return;
    }
    std::cerr << "FAIL " << g_case << " " << expr << " (" << __FILE__ << ":" << line << ")\n";
    ++g_failures;
}

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

bool sameRgb(Rgb8 a, Rgb8 b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

Rgb8 pixelAt(const SoftwareRenderer& renderer, int x, int y) {
    const std::vector<uint8_t>& pixels = renderer.pixels();
    const std::size_t index =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(renderer.viewportWidth()) +
         static_cast<std::size_t>(x)) *
        3;
    return Rgb8{pixels[index], pixels[index + 1], pixels[index + 2]};
}

uint32_t fnv1a(const std::vector<uint8_t>& bytes) {
    uint32_t hash = 2166136261u;
    for (uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

// Owns the planes a DecodedFrame points at. Padding bytes stay 0x5A so a
// renderer that ignores linesize fails the solid-color checks.
class YuvImage {
public:
    YuvImage(int width, int height, int yStride, int cStride)
        : width_(width),
          height_(height),
          yStride_(yStride),
          cStride_(cStride),
          chromaW_((width + 1) / 2),
          chromaH_((height + 1) / 2),
          y_(static_cast<std::size_t>(yStride) * static_cast<std::size_t>(height), 0x5A),
          u_(static_cast<std::size_t>(cStride) * static_cast<std::size_t>(chromaH_), 0x5A),
          v_(static_cast<std::size_t>(cStride) * static_cast<std::size_t>(chromaH_), 0x5A) {}

    void fill(uint8_t y, uint8_t u, uint8_t v) {
        for (int row = 0; row < height_; ++row) {
            for (int col = 0; col < width_; ++col) {
                y_[static_cast<std::size_t>(row) * static_cast<std::size_t>(yStride_) +
                   static_cast<std::size_t>(col)] = y;
            }
        }
        for (int row = 0; row < chromaH_; ++row) {
            for (int col = 0; col < chromaW_; ++col) {
                const std::size_t index = static_cast<std::size_t>(row) * static_cast<std::size_t>(cStride_) +
                                          static_cast<std::size_t>(col);
                u_[index] = u;
                v_[index] = v;
            }
        }
    }

    void setY(int x, int y, uint8_t value) {
        y_[static_cast<std::size_t>(y) * static_cast<std::size_t>(yStride_) + static_cast<std::size_t>(x)] = value;
    }

    void setChroma(int x, int y, uint8_t u, uint8_t v) {
        const std::size_t index =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(cStride_) + static_cast<std::size_t>(x);
        u_[index] = u;
        v_[index] = v;
    }

    DecodedFrame frame() const {
        DecodedFrame out;
        out.format = FramePixelFormat::Yuv420p;
        out.width = width_;
        out.height = height_;
        out.data[0] = y_.data();
        out.data[1] = u_.data();
        out.data[2] = v_.data();
        out.linesize[0] = yStride_;
        out.linesize[1] = cStride_;
        out.linesize[2] = cStride_;
        return out;
    }

private:
    int width_ = 0;
    int height_ = 0;
    int yStride_ = 0;
    int cStride_ = 0;
    int chromaW_ = 0;
    int chromaH_ = 0;
    std::vector<uint8_t> y_;
    std::vector<uint8_t> u_;
    std::vector<uint8_t> v_;
};

class RecordingRenderer : public Renderer {
public:
    struct Present {
        int frameWidth = 0;
        int frameHeight = 0;
        LayoutEngine::Rect dest;
    };

    bool init(int viewportWidth, int viewportHeight) override {
        width_ = viewportWidth;
        height_ = viewportHeight;
        ready_ = viewportWidth > 0 && viewportHeight > 0;
        return ready_;
    }

    bool present(const DecodedFrame& frame, const LayoutEngine::Rect& dest) override {
        if (!ready_) {
            return false;
        }
        presents.push_back(Present{frame.width, frame.height, dest});
        return true;
    }

    void shutdown() override { ready_ = false; }

    std::vector<Present> presents;

private:
    int width_ = 0;
    int height_ = 0;
    bool ready_ = false;
};

void expectColor(const SoftwareRenderer& renderer, int x, int y, Rgb8 expected) {
    CHECK(sameRgb(pixelAt(renderer, x, y), expected));
}

void testYuvFormula() {
    g_case = "bt601";
    CHECK(sameRgb(yuv601ToRgb(16, 128, 128), Rgb8{0, 0, 0}));
    CHECK(sameRgb(yuv601ToRgb(235, 128, 128), Rgb8{255, 255, 255}));
    CHECK(sameRgb(yuv601ToRgb(126, 128, 128), Rgb8{128, 128, 128}));
    CHECK(sameRgb(yuv601ToRgb(81, 128, 128), Rgb8{76, 76, 76}));
    CHECK(sameRgb(yuv601ToRgb(81, 90, 240), Rgb8{255, 0, 0}));
    CHECK(sameRgb(yuv601ToRgb(210, 16, 146), Rgb8{255, 255, 0}));
}

void testOneStreamLayout() {
    g_case = "one-stream layout";
    const LayoutEngine::Result layout = LayoutEngine::compute(320, 240, 1);
    CHECK(layout.ok);
    CHECK(layout.name == "1x1");
    CHECK(layout.tiles.size() == 1);
    const LayoutEngine::Rect& tile = layout.tiles[0].rect;
    CHECK(tile.x == 0);
    CHECK(tile.y == 0);
    CHECK(tile.width == 320);
    CHECK(tile.height == 240);

    RecordingRenderer recorder;
    CHECK(recorder.init(320, 240));
    YuvImage image(8, 8, 8, 4);
    image.fill(235, 128, 128);
    CHECK(recorder.present(image.frame(), tile));
    CHECK(recorder.presents.size() == 1);
    CHECK(recorder.presents[0].frameWidth == 8);
    CHECK(recorder.presents[0].frameHeight == 8);
    CHECK(recorder.presents[0].dest.x == 0);
    CHECK(recorder.presents[0].dest.y == 0);
    CHECK(recorder.presents[0].dest.width == 320);
    CHECK(recorder.presents[0].dest.height == 240);
    recorder.shutdown();
    CHECK(!recorder.present(image.frame(), tile));
}

void testSolidAndChecksum() {
    g_case = "solid checksum";
    SoftwareRenderer renderer;
    CHECK(renderer.init(4, 2));
    YuvImage image(4, 2, 8, 4);
    image.fill(126, 128, 128);
    const LayoutEngine::Rect dest{0, 0, 4, 2};
    CHECK(renderer.present(image.frame(), dest));
    CHECK(renderer.framesPresented() == 1);
    const Rgb8 gray = yuv601ToRgb(126, 128, 128);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 4; ++x) {
            expectColor(renderer, x, y, gray);
        }
    }
    CHECK(renderer.checksum() == fnv1a(renderer.pixels()));
    // 4x2 RGB pixels of 128. Locked so a blit or hash change fails loudly.
    CHECK(renderer.checksum() == 0x8874b0a5u);
}

void testCopiesPlanes() {
    g_case = "copy planes";
    SoftwareRenderer renderer;
    CHECK(renderer.init(2, 2));
    YuvImage image(2, 2, 2, 1);
    image.fill(235, 128, 128);
    DecodedFrame frame = image.frame();
    CHECK(renderer.present(frame, LayoutEngine::Rect{0, 0, 2, 2}));
    image.fill(16, 128, 128);
    expectColor(renderer, 0, 0, Rgb8{255, 255, 255});
    expectColor(renderer, 1, 1, Rgb8{255, 255, 255});
}

void testChromaAndStride() {
    g_case = "chroma stride";
    SoftwareRenderer renderer;
    CHECK(renderer.init(4, 2));
    YuvImage image(4, 2, 8, 4);
    image.fill(126, 128, 128);
    image.setChroma(1, 0, 90, 240);
    CHECK(renderer.present(image.frame(), LayoutEngine::Rect{0, 0, 4, 2}));
    const Rgb8 gray = yuv601ToRgb(126, 128, 128);
    const Rgb8 colored = yuv601ToRgb(126, 90, 240);
    expectColor(renderer, 0, 0, gray);
    expectColor(renderer, 1, 1, gray);
    expectColor(renderer, 2, 0, colored);
    expectColor(renderer, 3, 1, colored);
}

void testScaleBlocks() {
    g_case = "scale blocks";
    SoftwareRenderer renderer;
    CHECK(renderer.init(4, 4));
    YuvImage image(2, 2, 4, 2);
    image.fill(16, 128, 128);
    image.setY(1, 0, 235);
    image.setY(0, 1, 126);
    image.setY(1, 1, 81);
    CHECK(renderer.present(image.frame(), LayoutEngine::Rect{0, 0, 4, 4}));
    const Rgb8 black{0, 0, 0};
    const Rgb8 white{255, 255, 255};
    const Rgb8 gray = yuv601ToRgb(126, 128, 128);
    const Rgb8 dim = yuv601ToRgb(81, 128, 128);
    expectColor(renderer, 0, 0, black);
    expectColor(renderer, 1, 1, black);
    expectColor(renderer, 2, 0, white);
    expectColor(renderer, 3, 1, white);
    expectColor(renderer, 0, 2, gray);
    expectColor(renderer, 1, 3, gray);
    expectColor(renderer, 2, 2, dim);
    expectColor(renderer, 3, 3, dim);
}

void testLetterbox() {
    g_case = "letterbox";
    SoftwareRenderer renderer;
    CHECK(renderer.init(4, 4));
    YuvImage wide(4, 2, 4, 2);
    wide.fill(235, 128, 128);
    CHECK(renderer.present(wide.frame(), LayoutEngine::Rect{0, 0, 4, 4}));
    for (int x = 0; x < 4; ++x) {
        expectColor(renderer, x, 0, Rgb8{0, 0, 0});
        expectColor(renderer, x, 1, Rgb8{255, 255, 255});
        expectColor(renderer, x, 2, Rgb8{255, 255, 255});
        expectColor(renderer, x, 3, Rgb8{0, 0, 0});
    }

    YuvImage tall(2, 4, 2, 1);
    tall.fill(235, 128, 128);
    CHECK(renderer.present(tall.frame(), LayoutEngine::Rect{0, 0, 4, 4}));
    for (int y = 0; y < 4; ++y) {
        expectColor(renderer, 0, y, Rgb8{0, 0, 0});
        expectColor(renderer, 1, y, Rgb8{255, 255, 255});
        expectColor(renderer, 2, y, Rgb8{255, 255, 255});
        expectColor(renderer, 3, y, Rgb8{0, 0, 0});
    }
}

void testLayoutCellLeavesNeighbors() {
    g_case = "one cell of 2x2";
    SoftwareRenderer renderer;
    CHECK(renderer.init(8, 8));
    const LayoutEngine::Result layout = LayoutEngine::compute(8, 8, 4, "2x2");
    CHECK(layout.ok);
    CHECK(layout.tiles.size() == 4);
    YuvImage image(4, 4, 4, 2);
    image.fill(235, 128, 128);
    CHECK(renderer.present(image.frame(), layout.tiles[0].rect));
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const bool inside = x < 4 && y < 4;
            expectColor(renderer, x, y, inside ? Rgb8{255, 255, 255} : Rgb8{0, 0, 0});
        }
    }
}

void testRejectsOversizeFrame() {
    g_case = "oversize frame";
    YuvImage wide(8224, 32, 8224, 4112);
    wide.fill(235, 128, 128);
    CHECK(!SoftwareRenderer::frameAcceptable(wide.frame()));

    YuvImage tall(32, 8224, 32, 16);
    tall.fill(235, 128, 128);
    CHECK(!SoftwareRenderer::frameAcceptable(tall.frame()));

    CHECK(!SoftwareRenderer::sizeAcceptable(8224, 32));
    CHECK(!SoftwareRenderer::sizeAcceptable(32, 8224));
    CHECK(SoftwareRenderer::sizeAcceptable(SoftwareRenderer::kMaxExtent, 32));
    CHECK(SoftwareRenderer::sizeAcceptable(SoftwareRenderer::kMaxExtent,
                                           SoftwareRenderer::kMaxExtent));

    YuvImage ok(SoftwareRenderer::kMaxExtent, 32, SoftwareRenderer::kMaxExtent,
                SoftwareRenderer::kMaxExtent / 2);
    ok.fill(235, 128, 128);
    CHECK(SoftwareRenderer::frameAcceptable(ok.frame()));

    SoftwareRenderer renderer;
    CHECK(renderer.init(4, 4));
    CHECK(!renderer.present(wide.frame(), LayoutEngine::Rect{0, 0, 4, 4}));
    CHECK(renderer.framesPresented() == 0);
}

void testReplaceAndReject() {
    g_case = "replace and reject";
    SoftwareRenderer renderer;
    CHECK(!renderer.present(DecodedFrame{}, LayoutEngine::Rect{0, 0, 1, 1}));
    CHECK(!renderer.init(0, 10));
    CHECK(!renderer.init(SoftwareRenderer::kMaxExtent + 1, 8));
    CHECK(renderer.init(4, 4));

    YuvImage image(4, 4, 4, 2);
    image.fill(235, 128, 128);
    CHECK(renderer.present(image.frame(), LayoutEngine::Rect{0, 0, 4, 4}));
    const uint32_t checksum = renderer.checksum();
    CHECK(renderer.framesPresented() == 1);

    image.fill(16, 128, 128);
    CHECK(renderer.present(image.frame(), LayoutEngine::Rect{0, 0, 4, 4}));
    expectColor(renderer, 2, 2, Rgb8{0, 0, 0});
    CHECK(renderer.framesPresented() == 2);

    DecodedFrame bad = image.frame();
    bad.format = FramePixelFormat::Unknown;
    CHECK(!renderer.present(bad, LayoutEngine::Rect{0, 0, 4, 4}));
    bad = image.frame();
    bad.data[1] = nullptr;
    CHECK(!renderer.present(bad, LayoutEngine::Rect{0, 0, 4, 4}));
    bad = image.frame();
    bad.linesize[0] = -1;
    CHECK(!renderer.present(bad, LayoutEngine::Rect{0, 0, 4, 4}));
    CHECK(!renderer.present(image.frame(), LayoutEngine::Rect{-1, 0, 4, 4}));
    CHECK(!renderer.present(image.frame(), LayoutEngine::Rect{2, 2, 4, 4}));
    CHECK(renderer.framesPresented() == 2);
    expectColor(renderer, 0, 0, Rgb8{0, 0, 0});
    CHECK(renderer.checksum() != checksum);

    renderer.shutdown();
    CHECK(!renderer.ready());
    CHECK(renderer.pixels().empty());
    CHECK(renderer.checksum() == 2166136261u);
}

void testPpmDump() {
    g_case = "ppm dump";
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("amlogic-software-renderer-" + std::to_string(static_cast<long long>(::getpid())));
    std::filesystem::remove_all(dir);
    SoftwareRenderer renderer(dir.string());
    CHECK(renderer.init(4, 2));
    YuvImage image(4, 2, 4, 2);
    image.fill(126, 128, 128);
    CHECK(renderer.present(image.frame(), LayoutEngine::Rect{0, 0, 4, 2}));
    CHECK(renderer.present(image.frame(), LayoutEngine::Rect{0, 0, 4, 2}));
    const std::filesystem::path first = dir / "frame-000000.ppm";
    const std::filesystem::path second = dir / "frame-000001.ppm";
    CHECK(renderer.lastPpmPath() == second.string());
    CHECK(std::filesystem::is_regular_file(first));
    CHECK(std::filesystem::is_regular_file(second));

    std::ifstream in(first, std::ios::binary);
    std::string header(32, '\0');
    in.read(header.data(), static_cast<std::streamsize>(header.size()));
    const auto n = in.gcount();
    header.resize(static_cast<std::size_t>(n > 0 ? n : 0));
    const std::string prefix = "P6\n4 2\n255\n";
    CHECK(header.compare(0, prefix.size(), prefix) == 0);
    in.clear();
    in.seekg(static_cast<std::streamoff>(prefix.size()));
    std::vector<char> payload(renderer.pixels().size());
    in.read(payload.data(), static_cast<std::streamsize>(payload.size()));
    CHECK(in.gcount() == static_cast<std::streamsize>(payload.size()));
    CHECK(std::string(payload.begin(), payload.end()) ==
          std::string(reinterpret_cast<const char*>(renderer.pixels().data()), renderer.pixels().size()));
    std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
    try {
        testYuvFormula();
        testOneStreamLayout();
        testSolidAndChecksum();
        testCopiesPlanes();
        testChromaAndStride();
        testScaleBlocks();
        testLetterbox();
        testLayoutCellLeavesNeighbors();
        testRejectsOversizeFrame();
        testReplaceAndReject();
        testPpmDump();
    } catch (const std::exception& ex) {
        std::cerr << "FAIL exception " << ex.what() << "\n";
        return 1;
    }
    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "software_renderer_tests ok\n";
    return 0;
}
