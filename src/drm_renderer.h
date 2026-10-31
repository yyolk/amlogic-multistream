#pragma once
#include "renderer.h"

class DrmRenderer : public Renderer {
public:
    bool init() override;
    void present() override;
};
