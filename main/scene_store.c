#include "scene_store.h"
#include "esp_log.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "scene_store";
static const char *BASE_PATH = "/storage";   // same mount js_storage uses
static const char *PREFIX = "scene.";
static const char *SUFFIX = ".json";

bool scene_store_name_valid(const char *name)
{
    if (!name || !*name) return false;
    size_t len = strlen(name);
    if (len > SCENE_MAX_NAME) return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

static void full_path(const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s%s%s", BASE_PATH, PREFIX, name, SUFFIX);
}

bool scene_store_exists(const char *name)
{
    if (!scene_store_name_valid(name)) return false;
    char path[96];
    full_path(name, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static void count_cb(const char *name, void *arg) { (void)name; (*(int *)arg)++; }

esp_err_t scene_store_write(const char *name, const char *json, size_t len)
{
    if (!scene_store_name_valid(name) || !json || len == 0 || len > SCENE_MAX_BYTES)
        return ESP_ERR_INVALID_ARG;
    if (!scene_store_exists(name)) {
        int n = 0;
        scene_store_list(count_cb, &n);
        if (n >= SCENE_MAX_COUNT) return ESP_ERR_NO_MEM;
    }
    char path[96];
    full_path(name, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    size_t w = fwrite(json, 1, len, f);
    fclose(f);
    if (w != len) { remove(path); return ESP_FAIL; }
    ESP_LOGI(TAG, "saved scene '%s' (%u B)", name, (unsigned)len);
    return ESP_OK;
}

esp_err_t scene_store_read(const char *name, char **out, size_t *len_out)
{
    if (!scene_store_name_valid(name) || !out) return ESP_ERR_INVALID_ARG;
    char path[96];
    full_path(name, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > SCENE_MAX_BYTES) { fclose(f); return ESP_ERR_INVALID_SIZE; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = 0;
    *out = buf;
    if (len_out) *len_out = n;
    return ESP_OK;
}

esp_err_t scene_store_delete(const char *name)
{
    if (!scene_store_name_valid(name)) return ESP_ERR_INVALID_ARG;
    char path[96];
    full_path(name, path, sizeof(path));
    if (remove(path) != 0) return ESP_ERR_NOT_FOUND;
    ESP_LOGI(TAG, "deleted scene '%s'", name);
    return ESP_OK;
}

int scene_store_list(void (*cb)(const char *name, void *arg), void *arg)
{
    DIR *d = opendir(BASE_PATH);
    if (!d) return 0;
    int count = 0;
    size_t plen = strlen(PREFIX), slen = strlen(SUFFIX);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len <= plen + slen) continue;
        if (strncmp(e->d_name, PREFIX, plen) != 0) continue;
        if (strcmp(e->d_name + len - slen, SUFFIX) != 0) continue;
        char name[SCENE_MAX_NAME + 1];
        size_t nlen = len - plen - slen;
        if (nlen > SCENE_MAX_NAME) continue;
        memcpy(name, e->d_name + plen, nlen);
        name[nlen] = 0;
        count++;
        if (cb) cb(name, arg);
    }
    closedir(d);
    return count;
}
