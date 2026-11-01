#include "app.h"

#include "drm_renderer.h"
#include "ffmpeg_decoder.h"
#include "layout_engine.h"
#include "mqtt_controller.h"
#include "software_renderer.h"
#include "stream_manager.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

const char* programName(int argc, char* argv[]) {
    if (argc > 0 && argv != nullptr && argv[0] != nullptr && argv[0][0] != '\0') {
        return argv[0];
    }
    return "amlogic-multistream";
}

enum class PresenterKind { Software, Drm };

void printDecodeUsage(const char* argv0) {
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " [--mqtt-host HOST ...]\n"
        << "      Connect to MQTT and run until SIGINT/SIGTERM. See docs/mqtt.md.\n"
        << "  " << argv0 << " --url <hls-or-media-url> [--frames N]\n"
        << "      [--renderer software|drm] [--render-dir DIR]\n"
        << "      Software-decode one HLS, HTTP, or file URL and present N frames\n"
        << "      (default 5). MQTT is not started.\n"
        << "      The default renderer is software: an RGB buffer, optional PPM\n"
        << "      files in DIR, and no /dev/dri. --renderer drm requires\n"
        << "      GS-King X / CoreELEC and a build with -DAMLOGIC_DRM_KMS=ON.\n";
}

// Decode mode is selected when a decode flag is present. Other arguments are
// left for MqttController::start(), which owns --help and the --mqtt-* flags.
bool parseDecodeArgs(int argc,
                     char* argv[],
                     std::string& url,
                     int& frames,
                     std::string& renderDir,
                     PresenterKind& presenter,
                     bool& help) {
    frames = 5;
    help = false;
    presenter = PresenterKind::Software;
    bool frames_set = false;
    bool presenter_set = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] != nullptr ? argv[i] : "";
        if (arg == "--help" || arg == "-h") {
            help = true;
            continue;
        }
        if (arg == "--url") {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                return false;
            }
            url = argv[++i];
            continue;
        }
        if (arg == "--frames") {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                return false;
            }
            try {
                size_t used = 0;
                const int value = std::stoi(argv[++i], &used);
                if (argv[i][used] != '\0' || value < 1) {
                    return false;
                }
                frames = value;
                frames_set = true;
            } catch (const std::exception&) {
                return false;
            }
            continue;
        }
        if (arg == "--render-dir") {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                return false;
            }
            renderDir = argv[++i];
            if (renderDir.empty()) {
                std::cerr << "--render-dir requires a directory\n";
                return false;
            }
            continue;
        }
        if (arg == "--renderer") {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                return false;
            }
            const std::string value = argv[++i];
            if (value == "software") {
                presenter = PresenterKind::Software;
            } else if (value == "drm") {
                presenter = PresenterKind::Drm;
            } else {
                std::cerr << "Unknown renderer '" << value << "' (expected software or drm)\n";
                return false;
            }
            presenter_set = true;
            continue;
        }
        std::cerr << "Unexpected argument in --url mode: " << arg << "\n";
        return false;
    }
    if (presenter == PresenterKind::Drm && !renderDir.empty()) {
        std::cerr << "--render-dir is only valid with --renderer software\n";
        return false;
    }
    if (frames_set && url.empty() && !help) {
        std::cerr << "--frames requires --url\n";
        return false;
    }
    if ((presenter_set || !renderDir.empty()) && url.empty() && !help) {
        std::cerr << "renderer flags require --url\n";
        return false;
    }
    return true;
}

bool wantsDecodeMode(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr) {
            continue;
        }
        const std::string arg = argv[i];
        if (arg == "--url" || arg == "--frames" || arg == "--render-dir" || arg == "--renderer") {
            return true;
        }
    }
    return false;
}

uint32_t sampleChecksum(const DecodedFrame& frame) {
    uint32_t checksum = 2166136261u;
    const int step_y = frame.height > 16 ? frame.height / 16 : 1;
    const int step_x = frame.width > 16 ? frame.width / 16 : 1;
    for (int y = 0; y < frame.height; y += step_y) {
        const auto* row = frame.data[0] +
            static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.linesize[0]);
        for (int x = 0; x < frame.width; x += step_x) {
            checksum ^= row[x];
            checksum *= 16777619u;
        }
    }
    return checksum;
}

bool frameLooksComplete(const DecodedFrame& frame) {
    if (frame.format != FramePixelFormat::Yuv420p || frame.width <= 0 || frame.height <= 0) {
        return false;
    }
    if (frame.data[0] == nullptr || frame.data[1] == nullptr || frame.data[2] == nullptr) {
        return false;
    }
    const int chroma_w = (frame.width + 1) / 2;
    return frame.linesize[0] >= frame.width &&
           frame.linesize[1] >= chroma_w &&
           frame.linesize[2] >= chroma_w;
}

// Single-stream software decode, then one Renderer::present() per frame.
// The default presenter is SoftwareRenderer, which does not open /dev/dri.
// DrmRenderer is selected only with --renderer drm and requires GS-King X /
// CoreELEC. MQTT is not started on this path.
int decodeUrl(const std::string& url, int frames, PresenterKind presenter, const std::string& renderDir) {
    std::cout << "Software decode isHardware=0 url=" << url << "\n";
    FfmpegDecoder decoder;
    if (!decoder.open(url)) {
        return 1;
    }
    if (decoder.isHardware()) {
        std::cerr << "FfmpegDecoder reported hardware; the software path must not\n";
        decoder.close();
        return 1;
    }

    std::unique_ptr<Renderer> renderer;
    SoftwareRenderer* software = nullptr;
    if (presenter == PresenterKind::Drm) {
        std::cout << "Presenter drm requires GS-King X / CoreELEC\n";
        renderer = std::make_unique<DrmRenderer>();
    } else {
        auto owned = std::make_unique<SoftwareRenderer>(renderDir);
        software = owned.get();
        renderer = std::move(owned);
        std::cout << "Presenter software (no /dev/dri)\n";
    }

    DecodedFrame frame;
    LayoutEngine::Rect tile;
    bool rendererReady = false;
    int got = 0;
    for (int i = 0; i < frames; ++i) {
        if (!decoder.readFrame(frame)) {
            break;
        }
        if (!frameLooksComplete(frame)) {
            std::cerr << "Decoded frame " << i << " is incomplete\n";
            renderer->shutdown();
            decoder.close();
            return 1;
        }
        std::cout << "frame " << got << " " << frame.width << "x" << frame.height
                  << " fmt=yuv420p pts=";
        if (frame.pts == kDecodedFrameNoPts) {
            std::cout << "none";
        } else {
            std::cout << frame.pts;
        }
        std::cout << " tb=" << frame.time_base_num << "/" << frame.time_base_den
                  << " ysum=" << sampleChecksum(frame) << "\n";

        if (!rendererReady) {
            if (!renderer->init(frame.width, frame.height)) {
                renderer->shutdown();
                decoder.close();
                return 1;
            }
            const LayoutEngine::Result layout = LayoutEngine::compute(frame.width, frame.height, 1);
            if (!layout.ok || layout.tiles.size() != 1) {
                std::cerr << "LayoutEngine failed for one stream\n";
                renderer->shutdown();
                decoder.close();
                return 1;
            }
            tile = layout.tiles[0].rect;
            rendererReady = true;
            std::cout << "layout " << layout.name << " tile " << tile.x << "," << tile.y << "+"
                      << tile.width << "x" << tile.height << "\n";
        }
        if (!renderer->present(frame, tile)) {
            std::cerr << "present failed for frame " << got << "\n";
            renderer->shutdown();
            decoder.close();
            return 1;
        }
        std::cout << "presented " << got << " tile=" << tile.x << "," << tile.y << "+" << tile.width
                  << "x" << tile.height;
        if (software != nullptr) {
            std::cout << " rgbsum=" << software->checksum();
            if (!software->lastPpmPath().empty()) {
                std::cout << " ppm=" << software->lastPpmPath();
            }
        }
        std::cout << "\n";
        ++got;
    }
    renderer->shutdown();
    decoder.close();
    if (got < 1) {
        std::cerr << "Opened but decoded 0 frames from " << url << "\n";
        return 1;
    }
    std::cout << "Decoded " << got << " frame(s)\n";
    return 0;
}

}  // namespace

int App::run(int argc, char* argv[]) {
    if (wantsDecodeMode(argc, argv)) {
        std::string url;
        int frames = 5;
        std::string renderDir;
        PresenterKind presenter = PresenterKind::Software;
        bool help = false;
        if (!parseDecodeArgs(argc, argv, url, frames, renderDir, presenter, help)) {
            printDecodeUsage(programName(argc, argv));
            return 2;
        }
        if (help) {
            printDecodeUsage(programName(argc, argv));
            return 0;
        }
        if (url.empty()) {
            std::cerr << "Empty URL after --url\n";
            printDecodeUsage(programName(argc, argv));
            return 2;
        }
        return decodeUrl(url, frames, presenter, renderDir);
    }

    std::cout << "App::run\n";
    StreamManager manager;
    MqttController mqtt(manager);
    return mqtt.start(argc, argv);
}
