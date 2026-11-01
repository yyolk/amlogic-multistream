#include "drm_renderer.h"

#include <iostream>
#include <string>

// requires GS-King X / CoreELEC
//
// Real /dev/dri use is compiled only with AMLOGIC_DRM_KMS. The default
// player binary has no open() of a DRM node.

#if defined(AMLOGIC_DRM_KMS)

#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include <xf86drm.h>
#include <xf86drmMode.h>
}

namespace {

// requires GS-King X / CoreELEC
// Open card0 and query it. Does not set a mode or commit a plane.
bool openDrmCard(int& fd, std::string& error) {
    const char* node = "/dev/dri/card0";
    if (::access(node, R_OK | W_OK) != 0) {
        error = std::string(node) + " is not available";
        return false;
    }
    fd = ::open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        error = std::string("open ") + node + " failed";
        return false;
    }

    drmVersionPtr version = drmGetVersion(fd);
    if (version != nullptr) {
        const std::string name = version->name != nullptr ? version->name : "";
        std::cout << "DrmRenderer: " << node << " driver=" << name << "\n";
        drmFreeVersion(version);
    }

    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0 ||
        drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        error = "atomic DRM client caps are unavailable";
        return false;
    }

    drmModeRes* resources = drmModeGetResources(fd);
    if (resources == nullptr) {
        error = "drmModeGetResources failed";
        return false;
    }
    std::cout << "DrmRenderer: connectors=" << resources->count_connectors
              << " crtcs=" << resources->count_crtcs
              << " (no modeset, no plane commit)\n";
    drmModeFreeResources(resources);
    return true;
}

}  // namespace

#endif

DrmRenderer::~DrmRenderer() {
    shutdown();
}

bool DrmRenderer::init(int viewportWidth, int viewportHeight) {
    shutdown();
    if (viewportWidth <= 0 || viewportHeight <= 0) {
        std::cerr << "DrmRenderer::init: viewport must be positive\n";
        return false;
    }

#if !defined(AMLOGIC_DRM_KMS)
    std::cerr << "DrmRenderer::init requires GS-King X / CoreELEC "
                 "(built without AMLOGIC_DRM_KMS; /dev/dri is not opened)\n";
    return false;
#else
    std::string error;
    if (!openDrmCard(fd_, error)) {
        std::cerr << "DrmRenderer::init requires GS-King X / CoreELEC: " << error << "\n";
        shutdown();
        return false;
    }
    viewportWidth_ = viewportWidth;
    viewportHeight_ = viewportHeight;
    ready_ = true;
    std::cout << "DrmRenderer::init " << viewportWidth_ << "x" << viewportHeight_
              << " requires GS-King X / CoreELEC\n";
    return true;
#endif
}

bool DrmRenderer::present(const DecodedFrame& frame, const LayoutEngine::Rect& dest) {
    if (!ready_) {
        std::cerr << "DrmRenderer::present requires GS-King X / CoreELEC "
                     "(renderer is not initialized; /dev/dri was not opened)\n";
        return false;
    }
    // Atomic YUV plane commit is intentionally not implemented. The frame
    // is not scanned out, on a desktop GPU or on a GS-King X.
    std::cerr << "DrmRenderer::present requires GS-King X / CoreELEC "
                 "(atomic plane commit is not implemented; frame "
              << frame.width << "x" << frame.height << " dest " << dest.width << "x" << dest.height
              << " was not scanned out)\n";
    return false;
}

void DrmRenderer::shutdown() {
#if defined(AMLOGIC_DRM_KMS)
    if (fd_ >= 0) {
        ::close(fd_);
    }
#endif
    fd_ = -1;
    viewportWidth_ = 0;
    viewportHeight_ = 0;
    ready_ = false;
}
