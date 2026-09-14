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
firmware/esp32-sniffer/fqbn.txt            Board FQBN used by deploy/flash.sh and CI
node-red/flows.json                        The Node-RED flow (import via the editor menu)
deploy/install.sh                          Installs the bridge + registers a udev rule per connected ESP32
deploy/flash.sh                            Compile + flash a board (by device path or serial) via arduino-cli
deploy/usb-common.sh                       Shared helper: finds connected ESP32 sniffers (used by both scripts above)
deploy/esp_sniffer_bridge.sh               Serial → TCP bridge (runs on the Node-RED host)
deploy/99-esp-sniffer.rules                Example udev rule (deploy/install.sh generates these for you)
deploy/esp-sniffer@.service                systemd template unit for the bridge script
.github/workflows/firmware-build.yml       CI: compiles the sketch on every push/PR (build check only)
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
- [`arduino-cli`](https://arduino.github.io/arduino-cli/) with the ESP32
  board package installed, if you're using `deploy/flash.sh` (installation
  steps in Setup, below). The Arduino IDE works too for a one-off flash, but
  `deploy/flash.sh` assumes `arduino-cli` since it needs to script the
  compile/upload.

## Setup

### 1. Flash the ESP32

**Install `arduino-cli`:**

```bash
# Linux/macOS — installs the arduino-cli binary into ./bin
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh
sudo mv bin/arduino-cli /usr/local/bin/

# or via Homebrew (macOS/Linux)
brew install arduino-cli
```

Windows: `choco install arduino-cli` (Chocolatey) or `scoop install arduino-cli`
(Scoop), or download the binary from the
[release page](https://github.com/arduino/arduino-cli/releases/latest).
Full instructions: [arduino-cli installation docs](https://arduino.github.io/arduino-cli/latest/installation/).

Then install the ESP32 board package:

```bash
arduino-cli config init
arduino-cli config set board_manager.additional_urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
```

`firmware/esp32-sniffer/fqbn.txt` holds the board's FQBN — check it matches
your board/core version (`arduino-cli board details -b esp32:esp32:esp32s3`
lists the available options) before your first flash.

Once the bridge (step 2) is set up and running, flash a board with either
its device path or its USB serial number (both are printed by
`deploy/install.sh`, see step 2):

```bash
./deploy/flash.sh /dev/ttyACM0
./deploy/flash.sh 3C:0F:02:E4:7D:B4
```

Flashing by serial is handy with several ESP32s on one hub, since which
`/dev/ttyACMx` a given board lands on can shift across replugs, while its
serial number doesn't.

This compiles the sketch, stops that device's `esp-sniffer@<device>.service`
so `esptool` can access the port, flashes, and restarts the service
afterwards — no manual `systemctl stop`/unplug-replug cycle needed, and it
works the same way regardless of how many ESP32s are on the hub, since it
only touches the one device you pass it. Before the bridge is set up, or for
a one-off flash, `arduino-cli upload`/the Arduino IDE work as normal.

The sketch boots into promiscuous mode immediately, hopping channels 1–14
every 300 ms, and streams captured management frames on the USB serial port
at 115200 baud.

Serial commands (type into the same serial connection):

- `channel <n>` — lock to a single channel, e.g. `channel 6`
- `channel <a>-<b>` — hop only within a range, e.g. `channel 1-11`
- `reset` — reboot the board

The channel range is persisted to NVS and restored on the next boot.

`.github/workflows/firmware-build.yml` compiles the sketch on every push/PR
that touches `firmware/`, as a build-only check — GitHub-hosted runners have
no USB access to real hardware, so flashing still has to happen locally via
`deploy/flash.sh`.

### 2. Wire up the host bridge (optional but recommended)

`deploy/esp_sniffer_bridge.sh` reads an ESP32's serial port, prefixes each
line with a `[TS:<unix-epoch>]` timestamp, and forwards it to Node-RED over
TCP on port 9990.

To have it start automatically whenever an ESP32 sniffer is plugged in, plug
in your board(s) and run:

```bash
./deploy/install.sh
```

This copies `esp_sniffer_bridge.sh` to `/usr/local/bin`, installs the
`esp-sniffer@.service` systemd template, and adds a udev rule for every
ESP32 sniffer it finds currently connected (matched by USB vendor/product ID
and each board's own serial number), then reloads udev so they start right
away. For each board it prints its serial number and the `/dev/ttyACMx`
path it currently maps to — useful for `deploy/flash.sh` (step 1) or
`systemctl status`. Run it as your normal user; it calls `sudo` itself for
the handful of steps that need root, so expect a password prompt.

**It only registers boards that are plugged in at the time you run it.**
Got a second (or third...) ESP32 to add later? Plug it in and run
`./deploy/install.sh` again — it's safe to re-run any time, since boards
already registered are left alone. You shouldn't need to hand-edit the udev
rules file yourself; if you find yourself doing that, it almost always means
that board wasn't plugged in yet on the last run.

Useful commands once installed:

```bash
# Check status of one board, or every registered board
sudo systemctl status esp-sniffer@ttyACM0.service
sudo systemctl status 'esp-sniffer@*'

# Stop/start the bridge by hand (deploy/flash.sh does this for you
# automatically around a flash — see step 1)
sudo systemctl stop esp-sniffer@ttyACM0.service
sudo systemctl start esp-sniffer@ttyACM0.service
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
