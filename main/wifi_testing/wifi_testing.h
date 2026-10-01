#ifndef WIFI_TESTING_H
#define WIFI_TESTING_H

#include <stdint.h>
#include "esp_err.h"

// Hydra32 Authentication & Metadata
#define ENGINE_VERSION "Hydra32-v1.0"
#define PASS_KEY "nawazali"

// Structure to pass attack configurations dynamically from Web UI
typedef struct {
    uint8_t target_mac[6];
    uint8_t ap_mac[6];
    int frame_count;
    int delay_ms;
} hydra32_config_t;

// Function Prototypes
int authenticate_operator(const char *input_password);
esp_err_t hydra32_inject_deauth(const hydra32_config_t *config);

#endif // WIFI_TESTING_H
