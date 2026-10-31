#include "app.h"
#include "mqtt_controller.h"
#include "stream_manager.h"

#include <iostream>

int App::run(int argc, char* argv[]) {
    std::cout << "App::run\n";
    // Stay in the MQTT loop. DRM/KMS and Amlogic hardware decode are later milestones.
    StreamManager manager;
    MqttController mqtt(manager);
    return mqtt.start(argc, argv);
}
