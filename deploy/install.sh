#!/bin/bash
# Installs the esp-sniffer host-side bridge: copies the serial->TCP bridge
# script and its systemd template unit into place, then registers a udev
# rule for every ESP32 sniffer currently plugged in (matched by USB serial
# number) so each board's bridge starts automatically and independently.
#
# Usage: ./deploy/install.sh
# (uses sudo itself for the steps that need root -- no need to run the
# whole script as root)
#
# Safe to re-run: only boards it hasn't seen before get a new udev rule, and
# re-copying the bridge script/systemd unit is a no-op if unchanged. Plug in
# another ESP32 sniffer and re-run this script any time to register it too.

set -euo pipefail

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This installer is Linux-only (needs udev/systemd)." >&2
    exit 1
fi

if ! command -v socat >/dev/null; then
    echo "socat is required but not installed (e.g. apt install socat)." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=deploy/usb-common.sh
source "$SCRIPT_DIR/usb-common.sh"

BRIDGE_DEST="/usr/local/bin/esp_sniffer_bridge.sh"
SERVICE_DEST="/etc/systemd/system/esp-sniffer@.service"
UDEV_RULES="/etc/udev/rules.d/99-esp-sniffer.rules"

echo "==> Installing bridge script to $BRIDGE_DEST"
sudo install -m 755 "$SCRIPT_DIR/esp_sniffer_bridge.sh" "$BRIDGE_DEST"

echo "==> Installing systemd unit to $SERVICE_DEST"
sudo install -m 644 "$SCRIPT_DIR/esp-sniffer@.service" "$SERVICE_DEST"
sudo systemctl daemon-reload

echo "==> Scanning for connected ESP32 sniffers ($ESP_SNIFFER_VID:$ESP_SNIFFER_PID)..."
if [[ ! -f "$UDEV_RULES" ]]; then
    echo "# Managed by deploy/install.sh -- one line per registered ESP32 sniffer." | sudo tee "$UDEV_RULES" >/dev/null
fi

found=0
added=0
while IFS='|' read -r serial ttyname _syspath; do
    found=$((found + 1))
    device="${ttyname:+/dev/$ttyname}"
    device="${device:-unknown, no tty node found}"

    if grep -qF "ATTRS{serial}==\"$serial\"" "$UDEV_RULES"; then
        echo "    - $serial ($device) already registered"
        continue
    fi

    rule="SUBSYSTEM==\"tty\", ATTRS{idVendor}==\"$ESP_SNIFFER_VID\", ATTRS{idProduct}==\"$ESP_SNIFFER_PID\", ATTRS{serial}==\"$serial\", TAG+=\"systemd\", ENV{SYSTEMD_WANTS}=\"esp-sniffer@%k.service\""
    echo "$rule" | sudo tee -a "$UDEV_RULES" >/dev/null
    echo "    + registered $serial ($device)"
    added=$((added + 1))
done < <(find_esp32_devices)

if [[ "$found" -eq 0 ]]; then
    echo "    No ESP32 sniffer currently connected. Plug one in (or more) and re-run this script to register it."
fi

if [[ "$added" -gt 0 ]]; then
    echo "==> Reloading udev rules..."
    sudo udevadm control --reload-rules
    sudo udevadm trigger
fi

echo "==> Done."
echo "    Check status with: systemctl status 'esp-sniffer@*'"
echo "    Flash a board with either its device path or serial number shown above, e.g.:"
echo "        ./deploy/flash.sh /dev/ttyACM0"
