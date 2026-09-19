#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h" 
#include "nvs_flash.h" 
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

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

QueueHandle_t packet_queue = nullptr;
volatile TickType_t last_hop_time = 0; // read from ISR context too, hence volatile

inline void perform_channel_hop() {
    current_chan++;
    if (current_chan > max_chan || current_chan < min_chan) {
        current_chan = min_chan;
    }
    esp_wifi_set_channel(current_chan, WIFI_SECOND_CHAN_NONE);
    last_hop_time = xTaskGetTickCount();
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

    xQueueSendFromISR(packet_queue, &data, NULL);
}

void parse_serial_command(String cmd) {
    cmd.trim();
    if (cmd.length() == 0) return;

    if (cmd.equalsIgnoreCase("reset")) {
        Serial.println("[SYSTEM] Rebooting...");
        delay(200);
        esp_restart();
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
                Serial.printf("[SYSTEM] Range adjusted. Static tuned to channel %d\n", min_chan);
            } else {
                Serial.printf("[SYSTEM] Range adjusted. Scan span changed to %d-%d\n", min_chan, max_chan);
            }
        } else {
            Serial.println("[ERROR] Parameter error. Use 'channel X' or 'channel X-Y' (1-14)");
        }
    }
}

void sniffer_worker_task(void *pvParameters) {
    last_hop_time = xTaskGetTickCount();
    PacketData dequeued_pkt;

    while (is_scanning) {
        if (min_chan != max_chan && (xTaskGetTickCount() - last_hop_time) >= pdMS_TO_TICKS(300)) {
            perform_channel_hop();
        }

        uint8_t packets_drained = 0;
        while (packets_drained < 5 && xQueueReceive(packet_queue, &dequeued_pkt, 0) == pdTRUE) {
            packets_drained++;

            float current_temp = temperatureRead();

            Serial.printf("[MGMT][LEN:%d][CHAN:%d][RSSI:%d][INT_TEMP:%.1fC][DATA:", 
                          dequeued_pkt.length, dequeued_pkt.channel, dequeued_pkt.rssi, current_temp);
            
            for (uint16_t i = 0; i < dequeued_pkt.length; i++) {
                Serial.printf("%02X", dequeued_pkt.payload[i]);
            }
            Serial.print("]\n");

            if (min_chan != max_chan && (xTaskGetTickCount() - last_hop_time) >= pdMS_TO_TICKS(300)) {
                perform_channel_hop();
            }
        }

        if (Serial.available() > 0) {
            String incoming = Serial.readStringUntil('\n');
            parse_serial_command(incoming);
        }
        
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    vTaskDelete(NULL);
}

void setup() {
    // Lock in power savings early at the boot initialization block
    setCpuFrequencyMhz(TARGET_CPU_SPEED_MHZ);
    Serial.begin(115200);

    esp_event_loop_create_default(); 

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    
    load_channels_from_nvs();
    current_chan = min_chan;

    packet_queue = xQueueCreate(40, sizeof(PacketData)); 
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
    
    xTaskCreatePinnedToCore(sniffer_worker_task, "sniffer_worker", 4096, NULL, 5, NULL, 1);
}

void loop() {}
