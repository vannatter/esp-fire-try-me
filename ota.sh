#!/usr/bin/env bash
# Wireless (OTA) reflash for the fire board (192.168.71.204).
#
# Builds with the normal env, then uploads over WiFi with espota.py. We do NOT
# use a PlatformIO `upload_protocol = espota` env: the espressif32 platform
# compiles much larger in OTA mode. Uploading the normal binary avoids that.
#
# Usage:  ./ota.sh
# Password: $OTA_PASSWORD (defaults to frankenlab; must match OTA_PASSWORD in code).

set -euo pipefail

IP=192.168.71.204
PW="${OTA_PASSWORD:-frankenlab}"
ESPOTA="$(ls "$HOME"/.platformio/packages/framework-arduinoespressif32/tools/espota.py 2>/dev/null | head -1)"
[ -n "$ESPOTA" ] || { echo "espota.py not found (build once with PlatformIO first)"; exit 1; }

echo "Building ..."
pio run -e esp32

echo "OTA upload -> $IP ..."
python3 "$ESPOTA" -i "$IP" -p 3232 --auth="$PW" -f ".pio/build/esp32/firmware.bin"
echo "done — fire board reflashed over WiFi."
