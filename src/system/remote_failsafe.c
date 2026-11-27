#include "system/remote_failsafe.h"
#include "eom-hal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "orgasm_control.h"
#include <stdbool.h>

static const char* TAG = "remote_failsafe";

static volatile bool s_armed = false;
static volatile uint8_t s_speed = 0;
static volatile int64_t s_last_us = 0;
static esp_timer_handle_t s_timer = NULL;

void remote_failsafe_note(uint8_t speed) {
    s_speed = speed;
    s_last_us = esp_timer_get_time();
    s_armed = speed > 0;
}

static void remote_failsafe_check(void* arg) {
    if (!s_armed) return;

    // Someone else took over (on-device controls, automatic mode): not ours to stop.
    if (orgasm_control_get_output_mode() != OC_MANUAL_CONTROL ||
        eom_hal_get_motor_speed() != s_speed) {
        s_armed = false;
        return;
    }

    int64_t silent_ms = (esp_timer_get_time() - s_last_us) / 1000;
    if (silent_ms >= REMOTE_FAILSAFE_TIMEOUT_MS) {
        s_armed = false;
        eom_hal_set_motor_speed(0);
        ESP_LOGW(
            TAG, "No motor command from the app for %lld ms: motor stopped.", (long long)silent_ms
        );
    }
}

void remote_failsafe_init(void) {
    if (s_timer != NULL) return;
    const esp_timer_create_args_t args = {
        .callback = &remote_failsafe_check,
        .name = "remote_failsafe",
    };
    if (esp_timer_create(&args, &s_timer) == ESP_OK) {
        esp_timer_start_periodic(s_timer, 250 * 1000);
    }
}
