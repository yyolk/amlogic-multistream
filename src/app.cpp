#include "app.h"

#include "ffmpeg_decoder.h"
#include "mqtt_controller.h"
#include "stream_manager.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

const char* programName(int argc, char* argv[]) {
    if (argc > 0 && argv != nullptr && argv[0] != nullptr && argv[0][0] != '\0') {
        return argv[0];
    }
    return "amlogic-multistream";
}

void printDecodeUsage(const char* argv0) {
    std::cerr
        << "Usage:\n"
        << "  " << argv0 << " [--mqtt-host HOST ...]\n"
        << "      Connect to MQTT and run until SIGINT/SIGTERM. See docs/mqtt.md.\n"
        << "  " << argv0 << " --url <hls-or-media-url> [--frames N]\n"
        << "      Software-decode one HLS, HTTP, or file URL and print N frames\n"
        << "      (default 5). MQTT is not started. DRM present is skipped.\n";
}

// Decode mode is selected only when --url is present. Other arguments are
// left for MqttController::start(), which owns --help and the --mqtt-* flags.
bool parseDecodeArgs(int argc, char* argv[], std::string& url, int& frames, bool& help) {
    frames = 5;
    help = false;
    bool frames_set = false;
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
        std::cerr << "Unexpected argument in --url mode: " << arg << "\n";
        return false;
    }
    if (frames_set && url.empty() && !help) {
        std::cerr << "--frames requires --url\n";
        return false;
    }
    return true;
}

bool wantsDecodeMode(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr && std::string(argv[i]) == "--url") {
            return true;
        }
        if (argv[i] != nullptr && std::string(argv[i]) == "--frames") {
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

// Single-stream software decode. DrmRenderer::present() is not called: it
// does not consume DecodedFrame yet, and opening a DRM device belongs to
// that workstream. MQTT is not started on this path.
int decodeUrl(const std::string& url, int frames) {
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

    DecodedFrame frame;
    int got = 0;
    for (int i = 0; i < frames; ++i) {
        if (!decoder.readFrame(frame)) {
            break;
        }
        if (!frameLooksComplete(frame)) {
            std::cerr << "Decoded frame " << i << " is incomplete\n";
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
        ++got;
    }
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
        bool help = false;
        if (!parseDecodeArgs(argc, argv, url, frames, help)) {
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
        return decodeUrl(url, frames);
    }

    std::cout << "App::run\n";
    StreamManager manager;
    MqttController mqtt(manager);
    return mqtt.start(argc, argv);
}
