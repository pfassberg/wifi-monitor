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
# Usage: deploy/flash.sh <device path or serial number> [extra arduino-cli upload args...]
#
# The first argument can be either a device path (e.g. /dev/ttyACM0) or an
# ESP32 sniffer's USB serial number (as printed by deploy/install.sh or
# deploy/99-esp-sniffer.rules) -- useful since which /dev/ttyACMx a given
# board gets can change across replugs, especially with several on one hub.
set -euo pipefail

ARG="${1:?Usage: deploy/flash.sh <device path or serial number> [extra arduino-cli upload args...]}"
shift || true

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=deploy/usb-common.sh
source "$SCRIPT_DIR/usb-common.sh"

if [[ -e "$ARG" ]]; then
    DEVICE="$ARG"
else
    DEVICE="$(find_esp32_device_by_serial "$ARG")" || {
        echo "No connected ESP32 sniffer found with serial '$ARG' (and '$ARG' isn't an existing device file either)." >&2
        exit 1
    }
    echo "==> Resolved serial $ARG -> $DEVICE"
fi

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
