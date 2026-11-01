#pragma once

#include <cstdint>
#include <string>

// Sentinel for DecodedFrame::pts when the stream did not provide a timestamp.
constexpr int64_t kDecodedFrameNoPts = INT64_MIN;

// CPU image produced by a software decoder.
//
// Plane pointers are owned by the DecoderBackend that filled the struct.
// They stay valid only until the next readFrame() or close() on that same
// instance. A renderer that needs the bytes for longer must copy them.
// Renderer stays free of FFmpeg and DRM types.
//
// Yuv420p layout: data[0] = Y, data[1] = U, data[2] = V, all 8-bit.
// linesize[i] is the byte stride of that plane and may be greater than the
// plane width. U and V are (width+1)/2 by (height+1)/2.
enum class FramePixelFormat {
    Unknown = 0,
    Yuv420p,
};

struct DecodedFrame {
    const uint8_t* data[4] = {};
    int linesize[4] = {};
    int width = 0;
    int height = 0;
    FramePixelFormat format = FramePixelFormat::Unknown;
    // pts is in stream time_base units (time_base_num/time_base_den).
    int64_t pts = kDecodedFrameNoPts;
    // Both zero when the container did not provide a time base.
    int time_base_num = 0;
    int time_base_den = 0;
};

// Demux/decode backend. close() must be safe to call more than once,
// including after a failed open().
//
// open(), close(), readFrame(), and isHardware() run on one thread and must
// not be called concurrently with each other. abort() is the exception: a
// different thread may call it while open() or readFrame() is blocked in
// I/O. abort() latches for the life of the object. The in-flight open() or
// readFrame() then returns, and a later open() or readFrame() fails without
// blocking. close() still runs on the decode thread, after that call has
// returned.
class DecoderBackend {
public:
    virtual ~DecoderBackend() = default;
    virtual bool open(const std::string& url) = 0;
    virtual void close() = 0;
    virtual bool isHardware() const = 0;

    // Unblock an in-flight open() or readFrame() on this instance. Safe to
    // call from another thread, and safe to call more than once. The default
    // does nothing so a backend with no blocking I/O stays unchanged.
    virtual void abort() {}

    // Pull one decoded video frame.
    // true  -> frame is filled; plane pointers stay valid until the next
    //          readFrame() or close() on this instance.
    // false -> end of stream or a hard error. frame is left zeroed.
    // The default returns false so a backend that cannot yield CPU frames
    // (for example a future DRM-prime hardware decoder) stays linkable.
    virtual bool readFrame(DecodedFrame& frame) {
        frame = DecodedFrame{};
        return false;
    }
};
