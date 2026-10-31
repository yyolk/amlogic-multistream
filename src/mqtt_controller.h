#pragma once
#include "stream_manager.h"

class MqttController {
public:
    explicit MqttController(StreamManager& manager);
    void start();
private:
    StreamManager& manager_;
};
