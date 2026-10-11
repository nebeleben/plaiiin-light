#include "boot_guard.h"
#include "config_store.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "boot_guard";

// The 3rd consecutive un-stable boot enters safe boot.
#define BOOT_GUARD_THRESHOLD 3
// A boot that stays up this long is stable and clears the streak.
#define BOOT_GUARD_STABLE_MS 10000

static bool s_safe_boot = false;

void boot_guard_begin(void)
{
    // A clean power-on (user plugged the lamp in) is never a failure, so it
    // neither counts nor clears: leave the streak as the last un-stable boot
    // left it. Only boots that arrived some other way — a brown-out software
    // reset, a panic, a watchdog — advance the streak.
    if (esp_reset_reason() == ESP_RST_POWERON) {
        int32_t cnt = config_get_i32_or(CONFIG_KEY_BOOT_FAIL, 0);
        s_safe_boot = (cnt >= BOOT_GUARD_THRESHOLD);
        if (s_safe_boot) {
            ESP_LOGW(TAG, "SAFE BOOT (streak %ld, power-on) — panel held off",
                     (long)cnt);
        }
        return;
    }

    int32_t cnt = config_get_i32_or(CONFIG_KEY_BOOT_FAIL, 0) + 1;
    config_store_set_i32(CONFIG_KEY_BOOT_FAIL, cnt);
    s_safe_boot = (cnt >= BOOT_GUARD_THRESHOLD);
    if (s_safe_boot) {
        ESP_LOGW(TAG, "SAFE BOOT — %ld un-stable boots in a row; panel held "
                      "off, lamp stays dark but reachable. Fix power or set a "
                      "current limit, then POST /api/power to resume.",
                 (long)cnt);
    } else {
        ESP_LOGW(TAG, "Un-stable boot %ld/%d (reset reason %d) — %d more "
                      "without reaching %d s uptime enters safe boot",
                 (long)cnt, BOOT_GUARD_THRESHOLD, (int)esp_reset_reason(),
                 BOOT_GUARD_THRESHOLD - (int)cnt, BOOT_GUARD_STABLE_MS / 1000);
    }
}

bool boot_guard_is_safe(void)
{
    return s_safe_boot;
}

static void boot_guard_clear_cb(void *arg)
{
    (void)arg;
    int32_t cnt = config_get_i32_or(CONFIG_KEY_BOOT_FAIL, 0);
    if (cnt != 0) {
        config_store_set_i32(CONFIG_KEY_BOOT_FAIL, 0);
        ESP_LOGI(TAG, "Uptime reached %d ms — boot-fail streak cleared (was %ld)",
                 BOOT_GUARD_STABLE_MS, (long)cnt);
    }
}

void boot_guard_arm_clear(void)
{
    // A safe boot is latched: it stays tripped (panel off, reachable) until
    // the user takes control with a power command, so do NOT auto-clear it on
    // uptime — otherwise the next boot would try the panel again and relapse.
    if (s_safe_boot) return;

    const esp_timer_create_args_t args = {
        .callback = boot_guard_clear_cb,
        .name = "bootguard_clear",
    };
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, (uint64_t)BOOT_GUARD_STABLE_MS * 1000ULL);
    }
}

void boot_guard_user_ok(void)
{
    // An explicit power command means the user is driving the lamp (and, if
    // we were in safe boot, has presumably fixed the power or capped the
    // current). Clear the streak so the next boot restores normally.
    if (config_get_i32_or(CONFIG_KEY_BOOT_FAIL, 0) != 0) {
        config_store_set_i32(CONFIG_KEY_BOOT_FAIL, 0);
        ESP_LOGI(TAG, "User power command — boot-fail streak cleared");
    }
    s_safe_boot = false;
}
