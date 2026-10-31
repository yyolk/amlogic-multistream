#include "ffmpeg_decoder.h"
#include <iostream>

bool FfmpegDecoder::open(const std::string& url) {
    std::cout << "FfmpegDecoder::open " << url << " (stub)\n";
    // TODO: avformat_open_input + Amlogic hwaccel if available
    return true;
}

void FfmpegDecoder::close() {
    std::cout << "FfmpegDecoder::close\n";
}
