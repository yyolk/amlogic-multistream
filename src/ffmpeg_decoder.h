#pragma once
#include "decoder_backend.h"

class FfmpegDecoder : public DecoderBackend {
public:
    bool open(const std::string& url) override;
    void close() override;
    bool isHardware() const override { return hw_; }
private:
    bool hw_ = false;
};
