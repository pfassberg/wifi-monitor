# Wi-Fi Monitor

A Node-RED application for **passive Wi-Fi monitoring**, built on one or more
ESP32 boards running in promiscuous-sniffer mode. Captured 802.11 management
frames are parsed into structured data, aggregated per access point, and
served as a live, self-refreshing HTML dashboard — with a bonus decoder for
ASTM/ASD-STAN **Remote ID (Open Drone ID)** broadcasts, so nearby drones and
their pilot ground stations show up on their own tab.

Everything here is **receive-only**. The ESP32 firmware only listens; it
never transmits, associates, or injects frames.

## How it works

```
ESP32 (promiscuous sniffer, one per host)
   │  USB serial, ASCII frames: [TS:...][MGMT][LEN:...][CHAN:...][RSSI:...][INT_TEMP:...][DATA:HEX]
   ▼
esp_sniffer_bridge.sh  (socat: serial → TCP, timestamps each line)
   │  TCP, one line per frame
   ▼
Node-RED "ESP-Sniff" flow
   ├─ Main Wi-Fi Parser        → decodes frame type, MACs, channel, RSSI, SSID,
   │                             encryption, WPS state, AP uptime/reboot time
   ├─ AP aggregator            → keeps a live per-MAC table in flow context
   ├─ Drone RID Decoder        → extracts Open Drone ID telemetry from beacons/
   │                             probe responses carrying the ASD-STAN OUI (FA:0B:BC)
   └─ HTTP endpoint  GET /aps  → renders both tables as a self-refreshing dashboard
```

The ESP32 captures raw 802.11 **management** frames (beacons, probe
requests/responses, (de)authentication, (dis)association, action frames) and
streams them over USB serial as human-readable, hex-encoded lines. A small
host-side bridge script timestamps each line and forwards it over TCP into
Node-RED, which does all of the parsing, state-keeping, and rendering.

### What's tracked today

The **Access Points** tab is built from `BEACON` and `PROBE_RESP` frames:
SSID, BSSID, channel, RSSI, encryption (Open/OWE/WPA/WPA2/WPA3/transition),
WPS status (including a "pairing in progress" state), inferred AP boot time
and uptime (derived from the 802.11 TSF timer), and first/last-seen
timestamps.

The **Drone Radar** tab is built from the same frames when they carry an
Open Drone ID (ASTM F3411 / ASD-STAN) vendor element: UAV serial number,
GPS position, altitude, speed, heading, and — when broadcast — the pilot's
ground station position.

Other management frame types (`PROBE_REQ`, `DEAUTHENTICATION`,
`DISASSOCIATION`, `AUTHENTICATION`, `ACTION_FRAME`, `ASSOCIATION_REQ`) are
already classified by the parser but not yet wired into a table — they're a
starting point for extensions such as deauth-flood or rogue-AP alerting.

## Repository layout

```
firmware/esp32-sniffer/esp32-sniffer.ino   ESP32 promiscuous-mode sniffer (Arduino/ESP-IDF)
node-red/flows.json                        The Node-RED flow (import via the editor menu)
deploy/esp_sniffer_bridge.sh               Serial → TCP bridge (runs on the Node-RED host)
deploy/99-esp-sniffer.rules                udev rule: auto-start the bridge when the ESP32 is plugged in
deploy/esp-sniffer@.service                systemd template unit for the bridge script
package.json                               Node-RED dependency list (node-red-contrib-msg-speed)
```

## Requirements

- One or more ESP32 boards (tested against the Arduino-ESP32 core / ESP-IDF
  Wi-Fi promiscuous APIs).
- A host running [Node-RED](https://nodered.org/) with the
  [`node-red-contrib-msg-speed`](https://flows.nodered.org/node/node-red-contrib-msg-speed)
  node installed (listed in `package.json`).
- `socat` on the bridge host (Debian/Ubuntu: `sudo apt install socat`).
- Linux with `udev`/`systemd` if you want the bridge to start automatically
  when the ESP32 is plugged in (optional — you can also run
  `esp_sniffer_bridge.sh` manually).

## Setup

### 1. Flash the ESP32

Build and flash `firmware/esp32-sniffer/esp32-sniffer.ino` (Arduino IDE or
`arduino-cli`, ESP32 board package). It boots into promiscuous mode
immediately, hopping channels 1–14 every 300 ms, and streams captured
management frames on the USB serial port at 115200 baud.

Serial commands (type into the same serial connection):

- `channel <n>` — lock to a single channel, e.g. `channel 6`
- `channel <a>-<b>` — hop only within a range, e.g. `channel 1-11`
- `reset` — reboot the board

The channel range is persisted to NVS and restored on the next boot.

### 2. Wire up the host bridge (optional but recommended)

`deploy/esp_sniffer_bridge.sh` reads the ESP32's serial port, prefixes each
line with a `[TS:<unix-epoch>]` timestamp, and forwards it to Node-RED over
TCP on port 9990.

To have it start automatically whenever the ESP32 is plugged in:

```bash
sudo cp deploy/esp_sniffer_bridge.sh /usr/local/bin/esp_sniffer_bridge.sh
sudo chmod +x /usr/local/bin/esp_sniffer_bridge.sh

sudo cp deploy/esp-sniffer@.service /etc/systemd/system/
sudo systemctl daemon-reload

# Edit deploy/99-esp-sniffer.rules first: replace the placeholder serial
# with your board's own (see the comment in that file), then:
sudo cp deploy/99-esp-sniffer.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Useful commands once installed:

```bash
# Check status
sudo systemctl status esp-sniffer@ttyACM0.service

# Stop the bridge, e.g. before reflashing the ESP32
sudo systemctl stop esp-sniffer@ttyACM0.service
```

Without udev/systemd, you can just run the script by hand:
`./deploy/esp_sniffer_bridge.sh /dev/ttyACM0`.

### 3. Import the Node-RED flow

1. Install the flow's one dependency:
   `npm install node-red-contrib-msg-speed` inside your Node-RED user
   directory (or use Manage Palette in the editor).
2. In the Node-RED editor: menu → **Import** → paste or upload
   `node-red/flows.json`.
3. Deploy. The flow listens for the bridge on TCP port **9990** and serves
   the dashboard at:

   ```
   http://<node-red-host>:1880/aps
   ```

The dashboard has two tabs (Access Points / Drone Radar), a live filter box,
sortable columns, and a toggle for the 5-second auto-refresh.

To clear the accumulated AP table without restarting Node-RED, trigger the
"Empty AP database" inject node in the flow editor.

## License

MIT — see [LICENSE](LICENSE).
