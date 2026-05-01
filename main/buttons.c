#include "buttons.h"
#include "config_store.h"
#include "light_api.h"
#include "js_api.h"
#include "led_control.h"
#include "factory_reset.h"

#include "iot_button.h"
#include "button_gpio.h"
#include "esp_log.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "buttons";

// Two-stage long-press cascade on the power button (Phase 8 adjustments):
//   10 s — solid green LEDs, factory_reset_wifi() on release.
//   15 s — solid blue  LEDs, factory_reset_full() on release.
// Releasing before 10 s does nothing (single-tap path handles power toggle).
#define LONG_PRESS_WIFI_MS  10000
#define LONG_PRESS_FULL_MS  15000

typedef enum { BTN_PWR, BTN_NEXT, BTN_PREV } btn_kind_t;
typedef enum { RST_NONE, RST_WIFI, RST_FULL } reset_pending_t;

static button_handle_t s_handles[3] = {NULL, NULL, NULL};
static reset_pending_t s_reset_pending = RST_NONE;

// Paint every LED a solid color and force power on. Used as a "reset is
// armed at this level" indicator during the long-press hold — the user
// sees green at 10s, blue at 15s, and that color stays lit until release.
static void paint_solid(uint8_t r, uint8_t g, uint8_t b)
{
    int n = led_control_get_count();
    led_color_t *frame = calloc(n, sizeof(led_color_t));
    if (!frame) return;
    led_color_t c = {r, g, b};
    for (int i = 0; i < n; i++) frame[i] = c;
    led_control_power(true);
    led_control_set_all(frame, n);
    free(frame);
}

static void on_short(void *arg, void *user_data)
{
    btn_kind_t kind = (btn_kind_t)(intptr_t)user_data;
    switch (kind) {
    case BTN_PWR:
        light_api_apply_power(!led_control_is_on());
        break;
    case BTN_NEXT: {
        char chosen[64] = {0};
        esp_err_t err = js_api_play_next(chosen, sizeof(chosen));
        if (err != ESP_OK) ESP_LOGW(TAG, "next failed: %s", esp_err_to_name(err));
        break;
    }
    case BTN_PREV: {
        char chosen[64] = {0};
        esp_err_t err = js_api_play_prev(chosen, sizeof(chosen));
        if (err != ESP_OK) ESP_LOGW(TAG, "prev failed: %s", esp_err_to_name(err));
        break;
    }
    }
}

static void on_long_press_10s(void *arg, void *user_data)
{
    ESP_LOGW(TAG, "Power held >%dms — armed for factory_reset_wifi", LONG_PRESS_WIFI_MS);
    s_reset_pending = RST_WIFI;
    paint_solid(0, 200, 0);
}

static void on_long_press_15s(void *arg, void *user_data)
{
    ESP_LOGW(TAG, "Power held >%dms — escalated to factory_reset_full", LONG_PRESS_FULL_MS);
    s_reset_pending = RST_FULL;
    paint_solid(0, 100, 255);
}

static void on_pwr_release(void *arg, void *user_data)
{
    // BUTTON_PRESS_UP fires on every release. We only commit a reset if the
    // 10s threshold actually armed one — short taps stay handled by on_short.
    switch (s_reset_pending) {
    case RST_FULL:
        s_reset_pending = RST_NONE;
        factory_reset_full(/*reboot=*/true);
        break;
    case RST_WIFI:
        s_reset_pending = RST_NONE;
        factory_reset_wifi(/*reboot=*/true);
        break;
    case RST_NONE:
        break;
    }
}

static esp_err_t make_button(int pin, btn_kind_t kind, button_handle_t *out)
{
    if (pin < 0) { *out = NULL; return ESP_OK; }
    // long_press_time == LONG_PRESS_WIFI_MS sets the *base* threshold the
    // event machine uses internally; per-callback custom times are passed
    // via button_event_args_t below.
    button_config_t cfg = {
        .long_press_time = LONG_PRESS_WIFI_MS,
        .short_press_time = 50,
    };
    button_gpio_config_t gpio_cfg = {
        .gpio_num = pin,
        // Active-low — TTP223 outputs HIGH at rest, pulled LOW on touch
        // when wired with the typical configuration. Same for momentary
        // switches with an internal pull-up.
        .active_level = 0,
        .enable_power_save = false,
        .disable_pull = false,
    };
    button_handle_t h = NULL;
    esp_err_t err = iot_button_new_gpio_device(&cfg, &gpio_cfg, &h);
    if (err != ESP_OK || !h) {
        ESP_LOGE(TAG, "iot_button_new_gpio_device(pin=%d) err=%s", pin, esp_err_to_name(err));
        return ESP_FAIL;
    }
    iot_button_register_cb(h, BUTTON_SINGLE_CLICK, NULL, on_short,
                           (void *)(intptr_t)kind);
    if (kind == BTN_PWR) {
        // Two distinct long-press milestones. press_time on the args struct
        // selects when each callback fires; the indicator stays lit until
        // the user releases (handled by on_pwr_release).
        static button_event_args_t a10 = { .long_press = { .press_time = LONG_PRESS_WIFI_MS } };
        static button_event_args_t a15 = { .long_press = { .press_time = LONG_PRESS_FULL_MS } };
        iot_button_register_cb(h, BUTTON_LONG_PRESS_START, &a10, on_long_press_10s, NULL);
        iot_button_register_cb(h, BUTTON_LONG_PRESS_START, &a15, on_long_press_15s, NULL);
        iot_button_register_cb(h, BUTTON_PRESS_UP, NULL, on_pwr_release, NULL);
    }
    *out = h;
    ESP_LOGI(TAG, "Button kind=%d on GPIO %d ready", (int)kind, pin);
    return ESP_OK;
}

esp_err_t buttons_init(void)
{
    int32_t pwr  = config_get_i32_or(CONFIG_KEY_BTN_PWR_PIN,  CONFIG_PLAIIIN_BTN_PWR_PIN);
    int32_t nxt  = config_get_i32_or(CONFIG_KEY_BTN_NEXT_PIN, CONFIG_PLAIIIN_BTN_NEXT_PIN);
    int32_t prv  = config_get_i32_or(CONFIG_KEY_BTN_PREV_PIN, CONFIG_PLAIIIN_BTN_PREV_PIN);

    if (pwr < 0 && nxt < 0 && prv < 0) {
        ESP_LOGI(TAG, "No buttons configured — button driver idle");
        return ESP_OK;
    }
    make_button((int)pwr, BTN_PWR,  &s_handles[0]);
    make_button((int)nxt, BTN_NEXT, &s_handles[1]);
    make_button((int)prv, BTN_PREV, &s_handles[2]);
    return ESP_OK;
}
