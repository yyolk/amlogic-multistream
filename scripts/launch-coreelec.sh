#!/bin/sh
# requires GS-King X / CoreELEC
# RetroArch-style launch for CoreELEC
# Stop Kodi, run the multistream app, restart Kodi
#
# The app blocks in its MQTT loop until SIGINT/SIGTERM, so Kodi stays
# stopped until this process exits. Broker defaults: localhost:1883,
# topic prefix amlogic-multistream (MQTT_HOST, MQTT_PORT, MQTT_TOPIC_PREFIX).
# This binary does not program DRM/KMS planes or Amlogic hardware decode.

systemctl stop kodi

# Give the display a moment
sleep 1

/storage/amlogic-multistream || true

systemctl start kodi
