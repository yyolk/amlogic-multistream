#pragma once

#include "decoder_backend.h"
#include "layout_engine.h"

// Presents one CPU frame into a layout cell.
//
// Implementations copy plane bytes during present(). DecodedFrame pointers
// expire on the next DecoderBackend::readFrame() or close() for that decoder.
// This header does not include FFmpeg or libdrm types.
//
// The caller places the stream. LayoutEngine::compute() supplies dest for a
// single cell; a renderer does not build the grid. One present() draws one
// stream. Unused cells stay untouched.
//
// init() prepares a viewport of the given size in pixels. shutdown() releases
// that viewport and is safe to call more than once. Not thread-safe.
class Renderer {
public:
    virtual ~Renderer() = default;

    virtual bool init(int viewportWidth, int viewportHeight) = 0;
    virtual bool present(const DecodedFrame& frame, const LayoutEngine::Rect& dest) = 0;
    virtual void shutdown() = 0;
};
