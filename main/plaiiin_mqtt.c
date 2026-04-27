#include "plaiiin_mqtt.h"
#include "config_store.h"
#include "led_control.h"
#include "mqtt_client.h"  // ESP-IDF MQTT
#include "esp_log.h"
#include "esp_event.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

// HSV -> RGB conversion. H: 0-360, S: 0-100, V: 0-100
static void hsv_to_rgb(int h, int s, int v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (h < 0) h = 0;
    if (h > 360) h = 360;
    if (s < 0) s = 0;
    if (s > 100) s = 100;
    if (v < 0) v = 0;
    if (v > 100) v = 100;

    float sf = s / 100.0f, vf = v / 100.0f;
    float c = vf * sf;
    float hf = h / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(hf, 2.0f) - 1.0f));
    float m = vf - c;
    float rf, gf, bf;
    if (hf < 1) { rf = c; gf = x; bf = 0; }
    else if (hf < 2) { rf = x; gf = c; bf = 0; }
    else if (hf < 3) { rf = 0; gf = c; bf = x; }
    else if (hf < 4) { rf = 0; gf = x; bf = c; }
    else if (hf < 5) { rf = x; gf = 0; bf = c; }
    else { rf = c; gf = 0; bf = x; }
    *r = (uint8_t)((rf + m) * 255.0f + 0.5f);
    *g = (uint8_t)((gf + m) * 255.0f + 0.5f);
    *b = (uint8_t)((bf + m) * 255.0f + 0.5f);
}

// RGB -> HSV (for publishing current color as H,S,V)
static void rgb_to_hsv(uint8_t r, uint8_t g, uint8_t b, int *h, int *s, int *v)
{
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float mx = fmaxf(rf, fmaxf(gf, bf));
    float mn = fminf(rf, fminf(gf, bf));
    float d = mx - mn;
    float hf = 0;
    if (d > 0) {
        if (mx == rf)      hf = 60.0f * fmodf((gf - bf) / d, 6.0f);
        else if (mx == gf) hf = 60.0f * ((bf - rf) / d + 2.0f);
        else               hf = 60.0f * ((rf - gf) / d + 4.0f);
    }
    if (hf < 0) hf += 360.0f;
    *h = (int)(hf + 0.5f);
    *s = (mx > 0) ? (int)((d / mx) * 100.0f + 0.5f) : 0;
    *v = (int)(mx * 100.0f + 0.5f);
}

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_client = NULL;

static char s_topic_prefix[96];
static char s_topic_power_set[128];
static char s_topic_power_get[128];
static char s_topic_color_set[128];
static char s_topic_color_get[128];
static char s_topic_brightness_set[128];
static char s_topic_brightness_get[128];
static char s_topic_status[128];

static void build_topics(void)
{
    char node[64];
    config_get_str_or(CONFIG_KEY_NODE_NAME, node, sizeof(node), "PlaiiinLight");

    snprintf(s_topic_prefix, sizeof(s_topic_prefix), "plaiiinlight/%s", node);
    snprintf(s_topic_power_set, sizeof(s_topic_power_set), "%s/power/set", s_topic_prefix);
    snprintf(s_topic_power_get, sizeof(s_topic_power_get), "%s/power/get", s_topic_prefix);
    snprintf(s_topic_color_set, sizeof(s_topic_color_set), "%s/color/set", s_topic_prefix);
    snprintf(s_topic_color_get, sizeof(s_topic_color_get), "%s/color/get", s_topic_prefix);
    snprintf(s_topic_brightness_set, sizeof(s_topic_brightness_set), "%s/brightness/set", s_topic_prefix);
    snprintf(s_topic_brightness_get, sizeof(s_topic_brightness_get), "%s/brightness/get", s_topic_prefix);
    snprintf(s_topic_status, sizeof(s_topic_status), "%s/status", s_topic_prefix);
}

static void handle_message(const char *topic, const char *data, int data_len)
{
    char val[32] = {0};
    if (data_len > (int)sizeof(val) - 1) data_len = sizeof(val) - 1;
    memcpy(val, data, data_len);

    if (strcmp(topic, s_topic_power_set) == 0) {
        // Accept "0" (off) or "1" (on)
        bool on = (val[0] == '1');
        led_control_power(on);
        ESP_LOGI(TAG, "Power %s via MQTT", on ? "ON" : "OFF");
        mqtt_client_publish_state();

    } else if (strcmp(topic, s_topic_color_set) == 0) {
        // Parse HSV "h,s,v" where H: 0-360, S: 0-100, V: 0-100
        int h = 0, s = 0, v = 0;
        if (sscanf(val, "%d,%d,%d", &h, &s, &v) == 3) {
            uint8_t r, g, b;
            hsv_to_rgb(h, s, v, &r, &g, &b);
            int count = led_control_get_count();
            led_color_t *colors = calloc(count, sizeof(led_color_t));
            if (colors) {
                for (int i = 0; i < count; i++) {
                    colors[i].r = r;
                    colors[i].g = g;
                    colors[i].b = b;
                }
                led_control_set_all(colors, count);
                free(colors);
                ESP_LOGI(TAG, "Color HSV(%d,%d,%d) = RGB(%d,%d,%d) via MQTT", h, s, v, r, g, b);
                mqtt_client_publish_state();
            }
        } else {
            ESP_LOGW(TAG, "Invalid HSV format: '%s' (expected 'h,s,v')", val);
        }

    } else if (strcmp(topic, s_topic_brightness_set) == 0) {
        int bri = atoi(val);
        if (bri < 0) bri = 0;
        if (bri > 255) bri = 255;
        led_control_set_brightness((uint8_t)bri);
        ESP_LOGI(TAG, "Brightness %d via MQTT", bri);
        mqtt_client_publish_state();
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected to broker");
            esp_mqtt_client_subscribe(s_client, s_topic_power_set, 1);
            esp_mqtt_client_subscribe(s_client, s_topic_color_set, 1);
            esp_mqtt_client_subscribe(s_client, s_topic_brightness_set, 1);
            esp_mqtt_client_publish(s_client, s_topic_status, "online", 0, 1, 1);
            mqtt_client_publish_state();
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected from broker");
            break;

        case MQTT_EVENT_DATA: {
            char topic[128] = {0};
            int tlen = event->topic_len;
            if (tlen > (int)sizeof(topic) - 1) tlen = (int)sizeof(topic) - 1;
            memcpy(topic, event->topic, tlen);
            handle_message(topic, event->data, event->data_len);
            break;
        }

        default:
            break;
    }
}

esp_err_t mqtt_client_start(void)
{
    int32_t active = config_get_i32_or(CONFIG_KEY_MQTT_ACTIVE, 0);
    if (!active) {
        ESP_LOGI(TAG, "MQTT disabled");
        return ESP_OK;
    }

    char host[128] = {0};
    config_get_str_or(CONFIG_KEY_MQTT_HOST, host, sizeof(host), "");
    if (strlen(host) == 0) {
        ESP_LOGW(TAG, "MQTT enabled but no host configured");
        return ESP_OK;
    }

    int32_t port = config_get_i32_or(CONFIG_KEY_MQTT_PORT, 1883);
    build_topics();

    char uri[192];
    snprintf(uri, sizeof(uri), "mqtt://%s:%ld", host, (long)port);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .session.last_will = {
            .topic = s_topic_status,
            .msg = "offline",
            .msg_len = 7,
            .qos = 1,
            .retain = 1,
        },
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);

    ESP_LOGI(TAG, "MQTT -> %s (topics: %s/...)", uri, s_topic_prefix);
    return err;
}

void mqtt_client_stop(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
}

void mqtt_client_publish_state(void)
{
    if (!s_client) return;

    // Power: "0" or "1"
    esp_mqtt_client_publish(s_client, s_topic_power_get,
                            led_control_is_on() ? "1" : "0", 0, 0, 1);

    // Color: current last_color as HSV "h,s,v"
    led_color_t c = led_control_get_last_color();
    int h, s, v;
    rgb_to_hsv(c.r, c.g, c.b, &h, &s, &v);
    char hsv[24];
    snprintf(hsv, sizeof(hsv), "%d,%d,%d", h, s, v);
    esp_mqtt_client_publish(s_client, s_topic_color_get, hsv, 0, 0, 1);

    // Brightness: 0-255
    char bri[8];
    snprintf(bri, sizeof(bri), "%d", led_control_get_brightness());
    esp_mqtt_client_publish(s_client, s_topic_brightness_get, bri, 0, 0, 1);
}
