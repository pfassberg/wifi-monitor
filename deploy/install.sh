#!/bin/bash
# Installs the esp-sniffer host-side bridge: copies the serial->TCP bridge
# script and its systemd template unit into place, then registers a udev
# rule for every ESP32 sniffer currently plugged in (matched by USB serial
# number) so each board's bridge starts automatically and independently.
#
# Usage: sudo ./deploy/install.sh
#
# Safe to re-run: only boards it hasn't seen before get a new udev rule, and
# re-copying the bridge script/systemd unit is a no-op if unchanged. Plug in
# another ESP32 sniffer and re-run this script any time to register it too.

set -euo pipefail

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "This installer is Linux-only (needs udev/systemd)." >&2
    exit 1
fi

if [[ $EUID -ne 0 ]]; then
    echo "Run as root: sudo $0" >&2
    exit 1
fi

if ! command -v socat >/dev/null; then
    echo "socat is required but not installed (e.g. apt install socat)." >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BRIDGE_DEST="/usr/local/bin/esp_sniffer_bridge.sh"
SERVICE_DEST="/etc/systemd/system/esp-sniffer@.service"
UDEV_RULES="/etc/udev/rules.d/99-esp-sniffer.rules"
VID="303a"
PID="1001"

echo "==> Installing bridge script to $BRIDGE_DEST"
install -m 755 "$SCRIPT_DIR/esp_sniffer_bridge.sh" "$BRIDGE_DEST"

echo "==> Installing systemd unit to $SERVICE_DEST"
install -m 644 "$SCRIPT_DIR/esp-sniffer@.service" "$SERVICE_DEST"
systemctl daemon-reload

echo "==> Scanning for connected ESP32 sniffers ($VID:$PID)..."
if [[ ! -f "$UDEV_RULES" ]]; then
    echo "# Managed by deploy/install.sh -- one line per registered ESP32 sniffer." > "$UDEV_RULES"
fi

found=0
added=0
for dev in /sys/bus/usb/devices/*/; do
    [[ -f "${dev}idVendor" && -f "${dev}idProduct" && -f "${dev}serial" ]] || continue
    [[ "$(cat "${dev}idVendor")" == "$VID" && "$(cat "${dev}idProduct")" == "$PID" ]] || continue

    serial="$(cat "${dev}serial")"
    found=$((found + 1))

    if grep -qF "ATTRS{serial}==\"$serial\"" "$UDEV_RULES"; then
        echo "    - $serial already registered"
        continue
    fi

    echo "SUBSYSTEM==\"tty\", ATTRS{idVendor}==\"$VID\", ATTRS{idProduct}==\"$PID\", ATTRS{serial}==\"$serial\", TAG+=\"systemd\", ENV{SYSTEMD_WANTS}=\"esp-sniffer@%k.service\"" >> "$UDEV_RULES"
    echo "    + registered $serial"
    added=$((added + 1))
done

if [[ "$found" -eq 0 ]]; then
    echo "    No ESP32 sniffer currently connected. Plug one in (or more) and re-run this script to register it."
fi

if [[ "$added" -gt 0 ]]; then
    echo "==> Reloading udev rules..."
    udevadm control --reload-rules
    udevadm trigger
fi

echo "==> Done."
echo "    Check status with: systemctl status 'esp-sniffer@*'"
