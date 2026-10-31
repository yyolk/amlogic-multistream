#include "mqtt_controller.h"
#include <iostream>

MqttController::MqttController(StreamManager& manager) : manager_(manager) {}

void MqttController::start() {
    std::cout << "MQTT controller stub (connect to broker and subscribe)\n";
    // TODO: Paho / mosquitto client
}
