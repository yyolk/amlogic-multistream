#include "drm_renderer.h"
#include <iostream>

bool DrmRenderer::init() {
    std::cout << "DrmRenderer::init (stub \u2014 open /dev/dri/card0)\n";
    return true;
}

void DrmRenderer::present() {
    // TODO: atomic commit planes
}
