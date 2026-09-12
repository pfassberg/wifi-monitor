#!/bin/bash
DEVICE=$1
NODE_RED_IP="127.0.0.1"
NODE_RED_PORT=9990

if [ -z "$DEVICE" ]; then
    echo "[ERROR] Device path argument required."
    exit 1
fi

# Apply clean hardware line constraints
stty -F "$DEVICE" 115200 cs8 hupcl -clocal -crtscts

# -u forces unidirectional tracking. -T 10 triggers an exit if data drops for 10 seconds.
socat -u -T 10 "$DEVICE,b115200,raw,echo=0" - | while read -r line; do
    if [[ -n "$line" ]]; then
        # Leverages Bash's built-in memory string mapping instead of heavy process forking
        echo "[TS:${EPOCHREALTIME}] ${line}"
    fi
done > "/dev/tcp/${NODE_RED_IP}/${NODE_RED_PORT}" 2>/dev/null
