#include "config_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "config_store";
static const char *NVS_NAMESPACE = "plaiiin_cfg";

esp_err_t config_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition issue, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t config_store_get_str(const char *key, char *out, size_t max_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_str(handle, key, out, &max_len);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_set_str(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved %s", key);
    }
    return err;
}

esp_err_t config_store_get_i32(const char *key, int32_t *out)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_i32(handle, key, out);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_set_i32(const char *key, int32_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_i32(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t config_store_erase_keys(const char *const *keys, int count)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    for (int i = 0; i < count; i++) {
        esp_err_t e = nvs_erase_key(handle, keys[i]);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "erase %s: %s", keys[i], esp_err_to_name(e));
        }
    }
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bool config_store_has_wifi(void)
{
    char ssid[64] = {0};
    esp_err_t err = config_store_get_str(CONFIG_KEY_WIFI_SSID, ssid, sizeof(ssid));
    return (err == ESP_OK && strlen(ssid) > 0);
}

void config_get_str_or(const char *key, char *out, size_t max_len, const char *fallback)
{
    if (config_store_get_str(key, out, max_len) != ESP_OK) {
        strncpy(out, fallback, max_len - 1);
        out[max_len - 1] = '\0';
    }
}

int32_t config_get_i32_or(const char *key, int32_t fallback)
{
    int32_t val;
    if (config_store_get_i32(key, &val) == ESP_OK) {
        return val;
    }
    return fallback;
}
