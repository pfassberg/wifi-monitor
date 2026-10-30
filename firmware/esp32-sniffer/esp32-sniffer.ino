#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h" 
#include "nvs_flash.h" 
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#if CONFIG_IDF_TARGET_ESP32C5
#include "driver/usb_serial_jtag.h"
#include "soc/lp_aon_reg.h"

// Arduino's HWCDC Serial stays silent on the C5, so talk to the USB
// Serial/JTAG driver directly, as the V2X2MAP firmware does. Build with
// CDCOnBoot=default so the core leaves the peripheral alone.
class UsjConsole : public Stream {
    int peeked = -1;
public:
    void begin(size_t tx_buffer_size) {
        usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        cfg.tx_buffer_size = tx_buffer_size;
        usb_serial_jtag_driver_install(&cfg);
    }
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t *buf, size_t len) override {
        int n = usb_serial_jtag_write_bytes(buf, len, pdMS_TO_TICKS(100));
        return n > 0 ? n : 0;
    }
    int available() override {
        uint8_t c;
        if (peeked < 0 && usb_serial_jtag_read_bytes(&c, 1, 0) == 1) peeked = c;
        return peeked >= 0;
    }
    int read() override {
        if (!available()) return -1;
        int c = peeked;
        peeked = -1;
        return c;
    }
    int peek() override {
        available();
        return peeked;
    }
};
UsjConsole Console;
#else
#define Console Serial
#endif

// =========================================================================
// PRODUCTION WORKSPACE OVERRIDES
// =========================================================================
#define SNIFFER_MAGIC 0x5046      // "PF" - Personal verification security key
#define STORAGE_VERSION 1
#define TARGET_CPU_SPEED_MHZ 80   // Low-power 80MHz clock bounds for lower heat

// Guard window after every esp_wifi_set_channel() call. Right at a hop
// boundary, a frame can be reported on the channel being switched to (or
// from) rather than the one it actually arrived on -- most visible between
// adjacent, heavily-overlapping 2.4GHz channels (e.g. 11 reported as 12).
// Dropping captures for a few ms after each hop lets the RF settle before
// pkt->rx_ctrl.channel is trusted. This is the other half of the "trust the
// hardware register" fix below: that one stops a *stale* software channel
// variable from being reported; this one stops the register itself from
// being read mid-transition.
#define CHANNEL_SETTLE_MS 5
// =========================================================================

volatile bool is_scanning = false;
volatile uint8_t min_chan = 1;
volatile uint8_t max_chan = 14;
volatile uint8_t current_chan = 1;

#define MAX_FRAME_LEN 512
struct PacketData {
    uint16_t length;
    uint8_t channel;
    int8_t rssi;
    uint8_t payload[MAX_FRAME_LEN];
};

#define QUEUE_LEN 80
QueueHandle_t packet_queue = nullptr;
volatile TickType_t last_hop_time = 0; // read from ISR context too, hence volatile

// Queue health, reported every QUEUE_REPORT_MS as a "[SYSTEM] queue" line:
// frames queued, frames lost because the queue was full, and the most frames
// waiting at once since the last report.
#define QUEUE_REPORT_MS 10000
volatile uint32_t frames_received = 0;
volatile uint32_t frames_dropped = 0;
UBaseType_t queue_peak = 0;

#define HOP_MS 300

// The die temperature changes slowly; reading it once per frame was wasted work.
#define TEMP_REFRESH_MS 5000
float cached_temp = NAN;
TickType_t last_temp_read = 0;

// One output line: header, two hex digits per frame byte, "]\n".
static char line_buf[96 + 2 * MAX_FRAME_LEN];
static const char HEX_DIGITS[] = "0123456789ABCDEF";

// Formats the whole line first and hands it to Serial in a single write. The
// USB Serial/JTAG driver flushes a USB packet on every write call, so printing
// byte by byte sent two-byte packets and capped output at ~26 frames/s.
void write_frame(const PacketData &p) {
    int n = snprintf(line_buf, 96, "[MGMT][LEN:%d][CHAN:%d][RSSI:%d][INT_TEMP:%.1fC][DATA:",
                     p.length, p.channel, p.rssi, cached_temp);
    char *out = line_buf + n;
    for (uint16_t i = 0; i < p.length; i++) {
        *out++ = HEX_DIGITS[p.payload[i] >> 4];
        *out++ = HEX_DIGITS[p.payload[i] & 0x0f];
    }
    *out++ = ']';
    *out++ = '\n';
    Console.write((const uint8_t *)line_buf, out - line_buf);
}

inline void perform_channel_hop() {
    current_chan++;
    if (current_chan > max_chan || current_chan < min_chan) {
        current_chan = min_chan;
    }
    esp_wifi_set_channel(current_chan, WIFI_SECOND_CHAN_NONE);
    last_hop_time = xTaskGetTickCount();
}

// Optional RF switch control, for boards that select their antenna with a GPIO
// (the Waveshare ESP32-C5-Zero switches between its chip antenna and U.FL on
// GPIO26). Off by default, so a board without one has no pin driven.
#define ANT_GPIO_NONE 0xFF
uint8_t ant_gpio = ANT_GPIO_NONE;
uint8_t ant_level = 0;

void apply_antenna() {
    if (ant_gpio == ANT_GPIO_NONE) return;
    pinMode(ant_gpio, OUTPUT);
    digitalWrite(ant_gpio, ant_level);
}

void save_antenna_to_nvs() {
    nvs_handle_t h;
    if (nvs_open("sniffer_conf", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "ant_gpio", ant_gpio);
        nvs_set_u8(h, "ant_level", ant_level);
        nvs_commit(h);
        nvs_close(h);
    }
}

void load_antenna_from_nvs() {
    nvs_handle_t h;
    if (nvs_open("sniffer_conf", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "ant_gpio", &ant_gpio);
        nvs_get_u8(h, "ant_level", &ant_level);
        nvs_close(h);
    }
}

void save_channels_to_nvs(uint8_t min_ch, uint8_t max_ch) {
    nvs_handle_t my_handle;
    if (nvs_open("sniffer_conf", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_u16(my_handle, "magic", SNIFFER_MAGIC);
        nvs_set_u8(my_handle, "version", STORAGE_VERSION);
        nvs_set_u8(my_handle, "min_chan", min_ch);
        nvs_set_u8(my_handle, "max_chan", max_ch);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

void load_channels_from_nvs() {
    nvs_handle_t my_handle;
    uint16_t stored_magic = 0;
    uint8_t stored_version = 0;

    if (nvs_open("sniffer_conf", NVS_READONLY, &my_handle) == ESP_OK) {
        nvs_get_u16(my_handle, "magic", &stored_magic);
        nvs_get_u8(my_handle, "version", &stored_version);

        if (stored_magic == SNIFFER_MAGIC && stored_version == STORAGE_VERSION) {
            nvs_get_u8(my_handle, "min_chan", (uint8_t *)&min_chan);
            nvs_get_u8(my_handle, "max_chan", (uint8_t *)&max_chan);
        } else {
            min_chan = 1;
            max_chan = 14;
            nvs_close(my_handle);
            save_channels_to_nvs(min_chan, max_chan);
            return;
        }
        nvs_close(my_handle);
    } else {
        min_chan = 1;
        max_chan = 14;
        save_channels_to_nvs(min_chan, max_chan);
    }
}

void wifi_packet_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT || packet_queue == nullptr || !is_scanning) return;

    // Runs in WiFi RX ISR context, so the ISR-safe tick call is required --
    // see CHANNEL_SETTLE_MS above for why this check exists at all.
    if ((xTaskGetTickCountFromISR() - last_hop_time) < pdMS_TO_TICKS(CHANNEL_SETTLE_MS)) {
        return;
    }

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    uint16_t len = pkt->rx_ctrl.sig_len;
    if (len > MAX_FRAME_LEN) len = MAX_FRAME_LEN;

    PacketData data;
    data.length = len;
    
    // HARDWARE REG FIX: Trust the physical radio register to stop channel bleeding artifacts
    data.channel = pkt->rx_ctrl.channel; 
    
    data.rssi = pkt->rx_ctrl.rssi; 
    memcpy(data.payload, pkt->payload, len);

    if (xQueueSendFromISR(packet_queue, &data, NULL) == pdTRUE) frames_received++;
    else frames_dropped++;
}

void parse_serial_command(String cmd) {
    cmd.trim();
    if (cmd.length() == 0) return;

    if (cmd.equalsIgnoreCase("reset")) {
        Console.println("[SYSTEM] Rebooting...");
        delay(200);
        esp_restart();
    }
#if CONFIG_IDF_TARGET_ESP32C5
    else if (cmd.equalsIgnoreCase("bootloader")) {
        // Restart into the ROM download mode, like holding BOOT. esptool's own
        // reset into download mode leaves the C5 ROM unresponsive.
        Console.println("[SYSTEM] Rebooting into download mode...");
        delay(200);
        REG_SET_FIELD(LP_AON_SYS_CFG_REG, LP_AON_FORCE_DOWNLOAD_BOOT, 1);
        esp_restart();
    }
#endif
    else if (cmd.startsWith("antenna ")) {
        // "antenna <gpio> <0|1>" drives an RF switch pin, "antenna off" releases it.
        String arg = cmd.substring(8);
        arg.trim();
        int sp = arg.indexOf(' ');
        if (arg.equalsIgnoreCase("off")) {
            if (ant_gpio != ANT_GPIO_NONE) pinMode(ant_gpio, INPUT);
            ant_gpio = ANT_GPIO_NONE;
            save_antenna_to_nvs();
            Console.println("[SYSTEM] Antenna switch released");
        } else if (sp > 0 && GPIO_IS_VALID_OUTPUT_GPIO(arg.substring(0, sp).toInt())) {
            ant_gpio = arg.substring(0, sp).toInt();
            ant_level = arg.substring(sp + 1).toInt() ? 1 : 0;
            apply_antenna();
            save_antenna_to_nvs();
            Console.printf("[SYSTEM] Antenna switch GPIO%d = %d\n", ant_gpio, ant_level);
        } else {
            Console.println("[ERROR] Use 'antenna <gpio> <0|1>' or 'antenna off'");
        }
    }
    else if (cmd.startsWith("channel ")) {
        String range = cmd.substring(8);
        range.trim();
        
        uint8_t new_min = 1;
        uint8_t new_max = 14;
        bool valid = false;

        int dash_idx = range.indexOf('-');
        if (dash_idx != -1) {
            new_min = range.substring(0, dash_idx).toInt();
            new_max = range.substring(dash_idx + 1).toInt();
            if (new_min >= 1 && new_max <= 14 && new_min <= new_max) {
                valid = true;
            }
        } else {
            new_min = range.toInt();
            new_max = new_min;
            if (new_min >= 1 && new_min <= 14) {
                valid = true;
            }
        }

        if (valid) {
            min_chan = new_min;
            max_chan = new_max;
            current_chan = min_chan;

            esp_wifi_set_channel(current_chan, WIFI_SECOND_CHAN_NONE);
            last_hop_time = xTaskGetTickCount();

            save_channels_to_nvs(min_chan, max_chan);

            if (min_chan == max_chan) {
                Console.printf("[SYSTEM] Range adjusted. Static tuned to channel %d\n", min_chan);
            } else {
                Console.printf("[SYSTEM] Range adjusted. Scan span changed to %d-%d\n", min_chan, max_chan);
            }
        } else {
            Console.println("[ERROR] Parameter error. Use 'channel X' or 'channel X-Y' (1-14)");
        }
    }
}

void sniffer_worker_task(void *pvParameters) {
    last_hop_time = xTaskGetTickCount();
    TickType_t last_report = xTaskGetTickCount();
    PacketData dequeued_pkt;

    while (is_scanning) {
        TickType_t now = xTaskGetTickCount();

        UBaseType_t waiting = uxQueueMessagesWaiting(packet_queue);
        if (waiting > queue_peak) queue_peak = waiting;
        if ((now - last_report) >= pdMS_TO_TICKS(QUEUE_REPORT_MS)) {
            last_report = now;
            Console.printf("[SYSTEM] queue received=%lu dropped=%lu peak=%u/%u\n",
                          (unsigned long)frames_received, (unsigned long)frames_dropped,
                          (unsigned)queue_peak, QUEUE_LEN);
            queue_peak = 0;
        }

        if (isnan(cached_temp) || (now - last_temp_read) >= pdMS_TO_TICKS(TEMP_REFRESH_MS)) {
            cached_temp = temperatureRead();
            last_temp_read = now;
        }

        if (min_chan != max_chan && (now - last_hop_time) >= pdMS_TO_TICKS(HOP_MS)) {
            perform_channel_hop();
        }

        // Sleep until a frame arrives instead of polling, but wake in time for
        // the next hop and the serial command check.
        if (xQueueReceive(packet_queue, &dequeued_pkt, pdMS_TO_TICKS(10)) == pdTRUE) {
            write_frame(dequeued_pkt);
        }

        if (Console.available() > 0) {
            String incoming = Console.readStringUntil('\n');
            parse_serial_command(incoming);
        }
    }
    vTaskDelete(NULL);
}

void setup() {
#if !CONFIG_IDF_TARGET_ESP32C5
    // Lock in power savings early at the boot initialization block. The
    // single-core C5 keeps its default clock: it also serves Wi-Fi and USB.
    setCpuFrequencyMhz(TARGET_CPU_SPEED_MHZ);
#endif
    // Room for a few full lines, so a short USB stall does not block the worker.
#if CONFIG_IDF_TARGET_ESP32C5
    Console.begin(8192);
#else
    Console.setTxBufferSize(8192);
    Console.begin(115200);
#endif

    esp_event_loop_create_default(); 

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    
    load_channels_from_nvs();
    load_antenna_from_nvs();
    apply_antenna();
    current_chan = min_chan;

    packet_queue = xQueueCreate(QUEUE_LEN, sizeof(PacketData));
    is_scanning = true;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_NULL); 
    esp_wifi_start();

    wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(&wifi_packet_cb);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(current_chan, WIFI_SECOND_CHAN_NONE);
    
    // Last core: core 1 on the S3 (Wi-Fi runs on core 0), core 0 on the single-core C5.
    xTaskCreatePinnedToCore(sniffer_worker_task, "sniffer_worker", 4096, NULL, 5, NULL, portNUM_PROCESSORS - 1);
}

void loop() {}
