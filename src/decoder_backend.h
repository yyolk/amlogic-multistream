#pragma once
#include <string>

class DecoderBackend {
public:
    virtual ~DecoderBackend() = default;
    virtual bool open(const std::string& url) = 0;
    virtual void close() = 0;
    virtual bool isHardware() const = 0;
};
