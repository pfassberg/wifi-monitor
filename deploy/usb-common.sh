#!/bin/bash
# Shared helper for finding connected ESP32 sniffers, sourced by
# deploy/install.sh and deploy/flash.sh. Not meant to be run directly.

ESP_SNIFFER_VID="303a"
ESP_SNIFFER_PID="1001"

# Prints one line per connected ESP32 sniffer (VID:PID match), as
# "<serial>|<tty name, or empty if not found>|<sysfs device dir>".
# <tty name> is bare (e.g. "ttyACM0"), not a full /dev/ path. Fields are
# "|"-delimited rather than tab/space-delimited: with an all-whitespace IFS,
# bash's `read` collapses adjacent delimiters, which would silently swallow
# an empty tty-name field -- "|" isn't whitespace, so it doesn't.
find_esp32_devices() {
    local dev serial ttypath ttyname
    for dev in /sys/bus/usb/devices/*/; do
        [[ -f "${dev}idVendor" && -f "${dev}idProduct" && -f "${dev}serial" ]] || continue
        [[ "$(cat "${dev}idVendor")" == "$ESP_SNIFFER_VID" && "$(cat "${dev}idProduct")" == "$ESP_SNIFFER_PID" ]] || continue

        serial="$(cat "${dev}serial")"
        ttypath="$(find "$dev" -maxdepth 4 -type d -name 'ttyACM*' 2>/dev/null | head -n1)"
        ttyname=""
        [[ -n "$ttypath" ]] && ttyname="$(basename "$ttypath")"
        printf '%s|%s|%s\n' "$serial" "$ttyname" "$dev"
    done
}

# Resolves a serial number to its /dev/ttyACMx path. Prints nothing and
# returns non-zero if no connected ESP32 sniffer has that serial, or it
# doesn't have an associated tty node (e.g. still enumerating).
find_esp32_device_by_serial() {
    local target="$1" serial ttyname _syspath
    while IFS='|' read -r serial ttyname _syspath; do
        if [[ "$serial" == "$target" ]]; then
            [[ -n "$ttyname" ]] || return 1
            echo "/dev/$ttyname"
            return 0
        fi
    done < <(find_esp32_devices)
    return 1
}
