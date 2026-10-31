#pragma once

class Renderer {
public:
    virtual ~Renderer() = default;
    virtual bool init() = 0;
    virtual void present() = 0;
};
