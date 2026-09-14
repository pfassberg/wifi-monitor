#!/bin/bash
# Compile and flash the ESP32 sniffer sketch with arduino-cli, without needing
# to unplug/replug the board afterwards.
#
# The esp-sniffer bridge (deploy/esp_sniffer_bridge.sh, run via the
# esp-sniffer@<device>.service systemd unit) holds the serial port open, so it
# has to be stopped before esptool can flash, and it needs restarting
# afterwards. A soft reset out of the new firmware does not generate a fresh
# USB add event on this hardware, so udev never restarts the service on its
# own — this script does the stop/start explicitly instead.
#
# Usage: deploy/flash.sh /dev/ttyACM0 [extra arduino-cli upload args...]
set -euo pipefail

DEVICE="${1:?Usage: deploy/flash.sh /dev/ttyACM0 [extra arduino-cli upload args...]}"
shift || true

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SKETCH_DIR="$SCRIPT_DIR/../firmware/esp32-sniffer"
FQBN="$(head -n1 "$SKETCH_DIR/fqbn.txt")"
SERVICE="esp-sniffer@$(basename "$DEVICE").service"

echo "==> Compiling ($FQBN)..."
arduino-cli compile --fqbn "$FQBN" "$SKETCH_DIR"

echo "==> Stopping $SERVICE..."
sudo systemctl stop "$SERVICE"
trap 'echo "==> Restarting $SERVICE..."; sudo systemctl start "$SERVICE"' EXIT

echo "==> Flashing $DEVICE..."
arduino-cli upload -p "$DEVICE" --fqbn "$FQBN" "$SKETCH_DIR" "$@"

echo "==> Done."
