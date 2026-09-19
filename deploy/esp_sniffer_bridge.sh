#!/bin/bash
DEVICE=$1
# Overridable via the environment -- deploy/install.sh wires this up through
# /etc/esp-sniffer/env (see deploy/esp-sniffer@.service's EnvironmentFile)
# for setups where Node-RED isn't running on the same host as the bridge.
NODE_RED_IP="${NODE_RED_IP:-127.0.0.1}"
NODE_RED_PORT=9990

if [ -z "$DEVICE" ]; then
    echo "[ERROR] Device path argument required."
    exit 1
fi

# Apply clean hardware line constraints
stty -F "$DEVICE" 115200 cs8 hupcl -clocal -crtscts

# -u forces unidirectional tracking. -T 10 triggers an exit if data drops for 10 seconds.
#
# The `|| exit 1` on the write is load-bearing, not decoration: this loop's
# self-healing depends entirely on the whole script dying the moment the TCP
# side breaks, so systemd's Restart=always can bring it back with a fresh
# connection. Without it, a failed `echo` just prints "write error: Broken
# pipe" and the loop happily continues forever, discarding every line -- the
# service sits there "active (running)" but permanently disconnected, since
# systemd has nothing to restart. (The unit file also sets IgnoreSIGPIPE=no,
# since systemd ignores SIGPIPE for services by default, which would
# otherwise silently swallow the same failure at the signal level before it
# ever reaches this check.)
socat -u -T 10 "$DEVICE,b115200,raw,echo=0" - | while read -r line; do
    if [[ -n "$line" ]]; then
        # Leverages Bash's built-in memory string mapping instead of heavy process forking
        echo "[TS:${EPOCHREALTIME}] ${line}" || exit 1
    fi
done > "/dev/tcp/${NODE_RED_IP}/${NODE_RED_PORT}" 2>/dev/null
