#include "plug_reset.h"
#include "config_store.h"
#include "factory_reset.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "plug_reset";

// The 5th consecutive fast power-cycle triggers the reset.
#define PLUG_RESET_THRESHOLD 5
// A boot that stays powered this long is a normal boot and ends the streak.
#define PLUG_RESET_WINDOW_MS 10000

static void plug_reset_clear_cb(void *arg)
{
    (void)arg;
    int32_t cnt = config_get_i32_or(CONFIG_KEY_PLUG_CNT, 0);
    if (cnt != 0) {
        config_store_set_i32(CONFIG_KEY_PLUG_CNT, 0);
        ESP_LOGI(TAG, "Uptime reached %d ms — plug-cycle streak cleared (was %ld)",
                 PLUG_RESET_WINDOW_MS, (long)cnt);
    }
}

void plug_reset_check(void)
{
    if (esp_reset_reason() == ESP_RST_POWERON) {
        int32_t cnt = config_get_i32_or(CONFIG_KEY_PLUG_CNT, 0) + 1;
        if (cnt >= PLUG_RESET_THRESHOLD) {
            // Zero the streak BEFORE the wipe: the post-reset boot is a SW
            // reset and must never see a stale >= threshold count.
            config_store_set_i32(CONFIG_KEY_PLUG_CNT, 0);
            ESP_LOGW(TAG, "%ld fast power-cycles — factory reset over plug",
                     (long)cnt);
            factory_reset_full(/*reboot=*/true);   // blue flash + esp_restart()
            return;                                 // not reached
        }
        config_store_set_i32(CONFIG_KEY_PLUG_CNT, cnt);
        ESP_LOGI(TAG, "Fast power-on boot %ld/%d — power-cycle within %d s to continue the reset sequence",
                 (long)cnt, PLUG_RESET_THRESHOLD, PLUG_RESET_WINDOW_MS / 1000);
    }
    // Every boot (any reset reason) arms the clear timer, so an abandoned
    // 4-cycle streak from days ago is wiped by 10 s of uptime and can never
    // ambush the next innocent power-on.
    const esp_timer_create_args_t args = {
        .callback = plug_reset_clear_cb,
        .name = "plug_reset_clear",
    };
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, (uint64_t)PLUG_RESET_WINDOW_MS * 1000ULL);
    }
}
