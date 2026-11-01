#pragma once

#include "renderer.h"

#include <cstdint>
#include <string>
#include <vector>

// 8-bit RGB after BT.601 studio-range conversion.
struct Rgb8 {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

// BT.601 studio swing (Y 16..235, chroma 16..240) to full-range RGB.
// Values outside that swing are clamped. Full-range YUV is not remapped.
Rgb8 yuv601ToRgb(uint8_t y, uint8_t u, uint8_t v);

// Software presenter for a host with no DRM device.
//
// init() allocates an RGB viewport. present() letterboxes one YUV420P frame
// into dest with nearest-neighbor sampling and clears that cell first.
// Pixels outside dest are left alone, so a 1x1 tile can sit in a larger
// viewport without painting the other cells.
//
// When ppmDirectory is non-empty, each successful present() also writes
// frame-NNNNNN.ppm (P6, the whole viewport) under that directory. The
// directory is created in init(). This class never opens /dev/dri.
//
// pixels() is tightly packed RGB24, row-major, size width*height*3. It is
// invalidated by the next init(), present(), or shutdown(). checksum() is
// FNV-1a over those bytes (the FNV offset basis when the viewport is empty).
class SoftwareRenderer : public Renderer {
public:
    explicit SoftwareRenderer(std::string ppmDirectory = {});
    ~SoftwareRenderer() override;

    SoftwareRenderer(const SoftwareRenderer&) = delete;
    SoftwareRenderer& operator=(const SoftwareRenderer&) = delete;
    SoftwareRenderer(SoftwareRenderer&&) = delete;
    SoftwareRenderer& operator=(SoftwareRenderer&&) = delete;

    bool init(int viewportWidth, int viewportHeight) override;
    bool present(const DecodedFrame& frame, const LayoutEngine::Rect& dest) override;
    void shutdown() override;

    // Zero the RGB buffer without resetting the frame counter or writing a PPM.
    // No-op before a successful init(). Used when the set of cells changes or
    // playback stops, so a removed stream does not leave a stale cell.
    void clearViewport();

    bool ready() const { return ready_; }
    int viewportWidth() const { return width_; }
    int viewportHeight() const { return height_; }
    int framesPresented() const { return framesPresented_; }
    const std::vector<uint8_t>& pixels() const { return pixels_; }
    uint32_t checksum() const;
    const std::string& ppmDirectory() const { return ppmDirectory_; }
    const std::string& lastPpmPath() const { return lastPpmPath_; }

private:
    bool writePpm(const std::vector<uint8_t>& rgb, const std::string& path) const;

    std::string ppmDirectory_;
    std::string lastPpmPath_;
    std::vector<uint8_t> pixels_;
    int width_ = 0;
    int height_ = 0;
    int framesPresented_ = 0;
    bool ready_ = false;
};
