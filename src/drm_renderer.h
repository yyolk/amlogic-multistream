#pragma once

#include "renderer.h"

// DRM/KMS presenter. requires GS-King X / CoreELEC.
//
// The default build (AMLOGIC_DRM_KMS unset) does not open /dev/dri. init()
// fails immediately so Codespaces and CI never touch a GPU node.
//
// -DAMLOGIC_DRM_KMS=ON compiles the device hook: open /dev/dri/card0, read
// the DRM version and resources, and enable the atomic client cap. That
// hook still does not modeset and does not commit a plane. present() reports
// that scanout is not implemented.
class DrmRenderer : public Renderer {
public:
    DrmRenderer() = default;
    ~DrmRenderer() override;

    DrmRenderer(const DrmRenderer&) = delete;
    DrmRenderer& operator=(const DrmRenderer&) = delete;
    DrmRenderer(DrmRenderer&&) = delete;
    DrmRenderer& operator=(DrmRenderer&&) = delete;

    bool init(int viewportWidth, int viewportHeight) override;
    bool present(const DecodedFrame& frame, const LayoutEngine::Rect& dest) override;
    void shutdown() override;

private:
    int viewportWidth_ = 0;
    int viewportHeight_ = 0;
    int fd_ = -1;
    bool ready_ = false;
};
