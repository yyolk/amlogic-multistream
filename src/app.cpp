#include "app.h"
#include "mqtt_controller.h"
#include "stream_manager.h"
#include <iostream>

int App::run(int argc, char* argv[]) {
    std::cout << "App::run \u2014 skeleton\n";
    // TODO: parse args, start MQTT, init DRM, enter main loop
    StreamManager manager;
    MqttController mqtt(manager);
    // mqtt.start();
    return 0;
}
