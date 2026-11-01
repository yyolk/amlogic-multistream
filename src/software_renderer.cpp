#include "software_renderer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace {

constexpr int kMaxExtent = 8192;
constexpr int kMaxStride = 65536;
constexpr uint32_t kFnvOffset = 2166136261u;
constexpr uint32_t kFnvPrime = 16777619u;

int floorDiv256(int n) {
    if (n >= 0) {
        return n / 256;
    }
    return -((-n + 255) / 256);
}

int clamp8(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return value;
}

// (coeff + 128) / 256, rounding toward -inf, then clamped to a byte.
// The +128 matches the usual BT.601 integer recipe.
int scaleChannel(int coeff) {
    return clamp8(floorDiv256(coeff + 128));
}

bool rectInside(int viewportWidth, int viewportHeight, const LayoutEngine::Rect& dest) {
    if (dest.x < 0 || dest.y < 0 || dest.width < 0 || dest.height < 0) {
        return false;
    }
    const long long x1 = static_cast<long long>(dest.x) + dest.width;
    const long long y1 = static_cast<long long>(dest.y) + dest.height;
    return x1 <= viewportWidth && y1 <= viewportHeight;
}

struct DrawnRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// Largest axis-aligned box with the frame's aspect ratio that fits in dest.
// Centered. A box smaller than one pixel is returned empty (the cell stays
// the cleared color).
DrawnRect fitInside(const LayoutEngine::Rect& dest, int srcWidth, int srcHeight) {
    DrawnRect drawn;
    if (dest.width <= 0 || dest.height <= 0 || srcWidth <= 0 || srcHeight <= 0) {
        return drawn;
    }
    const long long widthLimited = static_cast<long long>(dest.width) * srcHeight;
    const long long heightLimited = static_cast<long long>(dest.height) * srcWidth;
    long long drawW = 0;
    long long drawH = 0;
    if (widthLimited <= heightLimited) {
        drawW = dest.width;
        drawH = widthLimited / srcWidth;
    } else {
        drawH = dest.height;
        drawW = heightLimited / srcHeight;
    }
    if (drawW < 1 || drawH < 1 || drawW > dest.width || drawH > dest.height) {
        return drawn;
    }
    drawn.width = static_cast<int>(drawW);
    drawn.height = static_cast<int>(drawH);
    drawn.x = dest.x + (dest.width - drawn.width) / 2;
    drawn.y = dest.y + (dest.height - drawn.height) / 2;
    return drawn;
}

bool planeOk(const uint8_t* data, int linesize, int width, int height) {
    if (data == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    if (linesize < width || linesize > kMaxStride) {
        return false;
    }
    return true;
}

uint8_t loadPlane(const uint8_t* data, int linesize, int x, int y) {
    const std::size_t offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(linesize) +
                               static_cast<std::size_t>(x);
    return data[offset];
}

void clearRect(std::vector<uint8_t>& rgb, int viewportWidth, const LayoutEngine::Rect& dest) {
    for (int y = dest.y; y < dest.y + dest.height; ++y) {
        uint8_t* row = rgb.data() +
                       (static_cast<std::size_t>(y) * static_cast<std::size_t>(viewportWidth) +
                        static_cast<std::size_t>(dest.x)) *
                           3;
        for (int x = 0; x < dest.width; ++x) {
            row[static_cast<std::size_t>(x) * 3] = 0;
            row[static_cast<std::size_t>(x) * 3 + 1] = 0;
            row[static_cast<std::size_t>(x) * 3 + 2] = 0;
        }
    }
}

void blitYuv420(std::vector<uint8_t>& rgb,
                int viewportWidth,
                const DecodedFrame& frame,
                const DrawnRect& drawn) {
    if (drawn.width <= 0 || drawn.height <= 0) {
        return;
    }
    for (int y = 0; y < drawn.height; ++y) {
        const int sy = static_cast<int>((static_cast<long long>(y) * frame.height) / drawn.height);
        const int cy = sy / 2;
        uint8_t* row = rgb.data() +
                       (static_cast<std::size_t>(drawn.y + y) * static_cast<std::size_t>(viewportWidth) +
                        static_cast<std::size_t>(drawn.x)) *
                           3;
        for (int x = 0; x < drawn.width; ++x) {
            const int sx = static_cast<int>((static_cast<long long>(x) * frame.width) / drawn.width);
            const int cx = sx / 2;
            const uint8_t Y = loadPlane(frame.data[0], frame.linesize[0], sx, sy);
            const uint8_t U = loadPlane(frame.data[1], frame.linesize[1], cx, cy);
            const uint8_t V = loadPlane(frame.data[2], frame.linesize[2], cx, cy);
            const Rgb8 px = yuv601ToRgb(Y, U, V);
            uint8_t* dst = row + static_cast<std::size_t>(x) * 3;
            dst[0] = px.r;
            dst[1] = px.g;
            dst[2] = px.b;
        }
    }
}

}  // namespace

Rgb8 yuv601ToRgb(uint8_t y, uint8_t u, uint8_t v) {
    const int c = static_cast<int>(y) - 16;
    const int d = static_cast<int>(u) - 128;
    const int e = static_cast<int>(v) - 128;
    Rgb8 out;
    out.r = static_cast<uint8_t>(scaleChannel(298 * c + 409 * e));
    out.g = static_cast<uint8_t>(scaleChannel(298 * c - 100 * d - 208 * e));
    out.b = static_cast<uint8_t>(scaleChannel(298 * c + 516 * d));
    return out;
}

SoftwareRenderer::SoftwareRenderer(std::string ppmDirectory)
    : ppmDirectory_(std::move(ppmDirectory)) {}

SoftwareRenderer::~SoftwareRenderer() {
    shutdown();
}

bool SoftwareRenderer::init(int viewportWidth, int viewportHeight) {
    shutdown();
    if (viewportWidth <= 0 || viewportHeight <= 0 || viewportWidth > kMaxExtent ||
        viewportHeight > kMaxExtent) {
        std::cerr << "SoftwareRenderer::init: viewport must be 1.." << kMaxExtent << " on each side\n";
        return false;
    }
    if (!ppmDirectory_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(ppmDirectory_, ec);
        if (ec) {
            std::cerr << "SoftwareRenderer::init: cannot create " << ppmDirectory_ << ": " << ec.message()
                      << "\n";
            return false;
        }
    }
    width_ = viewportWidth;
    height_ = viewportHeight;
    pixels_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_) * 3, 0);
    framesPresented_ = 0;
    lastPpmPath_.clear();
    ready_ = true;
    return true;
}

bool SoftwareRenderer::present(const DecodedFrame& frame, const LayoutEngine::Rect& dest) {
    if (!ready_) {
        std::cerr << "SoftwareRenderer::present called before a successful init\n";
        return false;
    }
    if (frame.format != FramePixelFormat::Yuv420p) {
        std::cerr << "SoftwareRenderer::present: only yuv420p is supported\n";
        return false;
    }
    if (frame.width <= 0 || frame.height <= 0 || frame.width > kMaxExtent || frame.height > kMaxExtent) {
        std::cerr << "SoftwareRenderer::present: frame size must be 1.." << kMaxExtent << "\n";
        return false;
    }
    if (!rectInside(width_, height_, dest)) {
        std::cerr << "SoftwareRenderer::present: dest is outside the viewport\n";
        return false;
    }
    const int chromaW = (frame.width + 1) / 2;
    const int chromaH = (frame.height + 1) / 2;
    if (!planeOk(frame.data[0], frame.linesize[0], frame.width, frame.height) ||
        !planeOk(frame.data[1], frame.linesize[1], chromaW, chromaH) ||
        !planeOk(frame.data[2], frame.linesize[2], chromaW, chromaH)) {
        std::cerr << "SoftwareRenderer::present: yuv420p planes are missing or have a bad stride\n";
        return false;
    }

    std::vector<uint8_t> next = pixels_;
    clearRect(next, width_, dest);
    const DrawnRect drawn = fitInside(dest, frame.width, frame.height);
    blitYuv420(next, width_, frame, drawn);

    std::string ppmPath;
    if (!ppmDirectory_.empty()) {
        std::ostringstream name;
        name << "frame-" << std::setw(6) << std::setfill('0') << framesPresented_ << ".ppm";
        const std::filesystem::path path = std::filesystem::path(ppmDirectory_) / name.str();
        ppmPath = path.string();
        if (!writePpm(next, ppmPath)) {
            return false;
        }
    }

    pixels_.swap(next);
    lastPpmPath_ = std::move(ppmPath);
    ++framesPresented_;
    return true;
}

void SoftwareRenderer::clearViewport() {
    if (!ready_) {
        return;
    }
    std::fill(pixels_.begin(), pixels_.end(), static_cast<uint8_t>(0));
}

void SoftwareRenderer::shutdown() {
    pixels_.clear();
    width_ = 0;
    height_ = 0;
    framesPresented_ = 0;
    lastPpmPath_.clear();
    ready_ = false;
}

uint32_t SoftwareRenderer::checksum() const {
    uint32_t hash = kFnvOffset;
    for (uint8_t byte : pixels_) {
        hash ^= byte;
        hash *= kFnvPrime;
    }
    return hash;
}

bool SoftwareRenderer::writePpm(const std::vector<uint8_t>& rgb, const std::string& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "SoftwareRenderer: cannot write " << path << "\n";
        return false;
    }
    out << "P6\n" << width_ << " " << height_ << "\n255\n";
    out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    if (!out) {
        std::cerr << "SoftwareRenderer: short write " << path << "\n";
        out.close();
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return false;
    }
    return true;
}
