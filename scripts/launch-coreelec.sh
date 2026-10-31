#!/bin/sh
# RetroArch-style launch for CoreELEC
# Stop Kodi, run the multistream app, restart Kodi

systemctl stop kodi

# Give the display a moment
sleep 1

/storage/amlogic-multistream || true

systemctl start kodi
