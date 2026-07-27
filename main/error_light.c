#include "error_light.h"
#include "led_control.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "error_light";
static TaskHandle_t s_task = NULL;
static error_light_pattern_t s_pattern = ERROR_LIGHT_NONE;
static volatile bool s_running = false;

static void set_first_n(uint8_t r, uint8_t g, uint8_t b, int n)
{
    int count = led_control_get_count();
    if (n > count) n = count;
    for (int i = 0; i < n; i++) {
        led_control_set_pixel(i, r, g, b);
    }
    led_control_refresh();
}

static void error_light_task(void *arg)
{
    s_running = true;
    error_light_pattern_t last_pattern = ERROR_LIGHT_NONE;
    while (s_running) {
        if (s_pattern != last_pattern) {
            // Pattern (re)activated — wipe so indicator pixels show against
            // a blank strip. apply_last_color() on boot would otherwise leave
            // every pixel painted with the restored color, and the patterns
            // below only overwrite pixels 0..2 — so 3..N would shine through.
            led_control_clear();
            last_pattern = s_pattern;
        }
        switch (s_pattern) {
            case ERROR_LIGHT_NO_WIFI:
                // Slow red blink
                set_first_n(80, 0, 0, 3);
                vTaskDelay(pdMS_TO_TICKS(1000));
                led_control_clear();
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;

            case ERROR_LIGHT_CONFIG_ERROR:
                // Fast red-yellow alternating
                set_first_n(80, 0, 0, 3);
                vTaskDelay(pdMS_TO_TICKS(200));
                set_first_n(80, 60, 0, 3);
                vTaskDelay(pdMS_TO_TICKS(200));
                break;

            case ERROR_LIGHT_AP_MODE:
                // Slow blue pulse
                for (int brightness = 0; brightness <= 40 && s_running; brightness += 2) {
                    set_first_n(0, 0, brightness, 3);
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                for (int brightness = 40; brightness >= 0 && s_running; brightness -= 2) {
                    set_first_n(0, 0, brightness, 3);
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                vTaskDelay(pdMS_TO_TICKS(200));
                break;

            case ERROR_LIGHT_NONE:
            default:
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
        }
    }
    // The task owns the exit repaint (it may sleep in a case body above for
    // up to ~1 s after clear() signals, and would otherwise blank the strip
    // after the caller already repainted). Settle back to the real state:
    // power_snap(true) repaints the frame buffer / last color — without it an
    // api-mode lamp, which has no player to repaint, stays dark; when the
    // lamp is off, blank IS the real state.
    if (led_control_is_on()) {
        led_control_power_snap(true);
    } else {
        led_control_clear();
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t error_light_init(void)
{
    return ESP_OK;  // Task created on first set()
}

void error_light_set(error_light_pattern_t pattern)
{
    s_pattern = pattern;
    ESP_LOGI(TAG, "Error light pattern: %d", pattern);

    if (!s_task && pattern != ERROR_LIGHT_NONE) {
        xTaskCreate(error_light_task, "error_light", 2048, NULL, 2, &s_task);
    }
}

void error_light_clear(void)
{
    s_pattern = ERROR_LIGHT_NONE;
    // No pattern was ever displayed — the strip still shows the restored
    // state from boot (api-mode static color has no player to repaint it),
    // so it must not be touched.
    if (!s_task) return;
    s_running = false;
    // Wait for the task to wind down and repaint the real state (its case
    // bodies sleep up to ~1 s between s_running checks), so callers observe
    // the settled strip. It nulls s_task as its last act.
    for (int i = 0; i < 30 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

error_light_pattern_t error_light_get(void)
{
    return s_pattern;
}
