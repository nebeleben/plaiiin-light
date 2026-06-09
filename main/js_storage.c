#include "js_storage.h"

#include "esp_spiffs.h"
#include "esp_log.h"
#include "plbc.h"  /* PLBC_MAGIC / PLBC_VERSION for the .bc freshness check */

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "js_storage";
static const char *BASE_PATH = "/storage";
static const char *FS_LABEL = "storage";

#define MAX_NAME 48
#define MAX_SCRIPT_BYTES (48 * 1024)
/* Upper bound on a serialized .bc — matches the serialize buffer used by the
 * boot compile pass (main.c). A program maxes out well under this. Used by
 * js_storage_bc_current() to read a whole .bc for plbc_load() validation. */
#define BC_VALIDATE_MAX 8192

static bool name_is_valid(const char *name)
{
    if (!name || !*name) return false;
    size_t len = strlen(name);
    if (len > MAX_NAME) return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.')) return false;
    }
    // Forbid "." / ".." / leading dot
    if (name[0] == '.') return false;
    return true;
}

static void full_path(const char *name, char *out, size_t max_len)
{
    snprintf(out, max_len, "%s/%s.js", BASE_PATH, name);
}

static void full_path_bc(const char *name, char *out, size_t max_len)
{
    snprintf(out, max_len, "%s/%s.bc", BASE_PATH, name);
}

esp_err_t js_storage_write_bc(const char *name, const void *buf, size_t len)
{
    if (!name_is_valid(name) || !buf) return ESP_ERR_INVALID_ARG;
    if (len > MAX_SCRIPT_BYTES) return ESP_ERR_INVALID_SIZE;
    char path[96];
    full_path_bc(name, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return ESP_FAIL;
    size_t wrote = fwrite(buf, 1, len, f);
    fclose(f);
    return wrote == len ? ESP_OK : ESP_FAIL;
}

esp_err_t js_storage_read_bc(const char *name, void **out, size_t *len_out)
{
    if (!name_is_valid(name) || !out) return ESP_ERR_INVALID_ARG;
    char path[96];
    full_path_bc(name, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > MAX_SCRIPT_BYTES) { fclose(f); return ESP_ERR_INVALID_SIZE; }
    void *buf = malloc(sz);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t n = fread(buf, 1, sz, f);
    fclose(f);
    *out = buf;
    if (len_out) *len_out = n;
    return ESP_OK;
}

esp_err_t js_storage_remove_bc(const char *name)
{
    if (!name_is_valid(name)) return ESP_ERR_INVALID_ARG;
    char path[96];
    full_path_bc(name, path, sizeof(path));
    if (unlink(path) != 0) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}

esp_err_t js_storage_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = BASE_PATH,
        .partition_label = FS_LABEL,
        .max_files = 6,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info(FS_LABEL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS mounted at %s (%u / %u bytes used)", BASE_PATH, used, total);
    return ESP_OK;
}

esp_err_t js_storage_list(char *out, size_t max_len)
{
    if (!out || max_len == 0) return ESP_ERR_INVALID_ARG;
    DIR *d = opendir(BASE_PATH);
    if (!d) {
        // Empty dir is fine.
        snprintf(out, max_len, "[]");
        return ESP_OK;
    }
    size_t pos = 0;
    int count = 0;
    pos += snprintf(out + pos, max_len - pos, "[");
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len < 4 || strcmp(entry->d_name + len - 3, ".js") != 0) continue;
        char name[MAX_NAME + 1];
        size_t base_len = len - 3;
        if (base_len > MAX_NAME) base_len = MAX_NAME;
        memcpy(name, entry->d_name, base_len);
        name[base_len] = 0;
        pos += snprintf(out + pos, max_len - pos,
                        "%s\"%s\"", count ? "," : "", name);
        count++;
        if (pos >= max_len) break;
    }
    snprintf(out + pos, max_len - pos, "]");
    closedir(d);
    return ESP_OK;
}

esp_err_t js_storage_read(const char *name, char **out, size_t *len_out)
{
    if (!name_is_valid(name) || !out) return ESP_ERR_INVALID_ARG;
    char path[96];
    full_path(name, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > MAX_SCRIPT_BYTES) { fclose(f); return ESP_ERR_INVALID_SIZE; }
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t n = fread(buf, 1, sz, f);
    fclose(f);
    buf[n] = 0;
    *out = buf;
    if (len_out) *len_out = n;
    return ESP_OK;
}

esp_err_t js_storage_write(const char *name, const char *source, size_t len)
{
    if (!name_is_valid(name) || !source) return ESP_ERR_INVALID_ARG;
    if (len > MAX_SCRIPT_BYTES) return ESP_ERR_INVALID_SIZE;
    char path[96];
    full_path(name, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Can't open %s: %s", path, strerror(errno));
        return ESP_FAIL;
    }
    size_t wrote = fwrite(source, 1, len, f);
    fclose(f);
    if (wrote != len) return ESP_FAIL;
    return ESP_OK;
}

esp_err_t js_storage_info(size_t *total, size_t *used, size_t *file_count)
{
    if (total)      *total = 0;
    if (used)       *used = 0;
    if (file_count) *file_count = 0;
    size_t t = 0, u = 0;
    esp_err_t err = esp_spiffs_info(FS_LABEL, &t, &u);
    if (err != ESP_OK) return err;
    if (total) *total = t;
    if (used)  *used = u;
    if (file_count) {
        DIR *d = opendir(BASE_PATH);
        if (d) {
            struct dirent *entry;
            size_t count = 0;
            while ((entry = readdir(d)) != NULL) {
                size_t len = strlen(entry->d_name);
                if (len >= 4 && strcmp(entry->d_name + len - 3, ".js") == 0) count++;
            }
            closedir(d);
            *file_count = count;
        }
    }
    return ESP_OK;
}

esp_err_t js_storage_remove(const char *name)
{
    if (!name_is_valid(name)) return ESP_ERR_INVALID_ARG;
    char path[96];
    full_path(name, path, sizeof(path));
    if (unlink(path) != 0) {
        if (errno == ENOENT) return ESP_ERR_NOT_FOUND;
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool js_storage_exists(const char *name)
{
    if (!name_is_valid(name)) return false;
    char path[96];
    full_path(name, path, sizeof(path));
    struct stat st;
    return stat(path, &st) == 0;
}

bool js_storage_bc_exists(const char *name)
{
    if (!name_is_valid(name)) return false;
    char path[96];
    full_path_bc(name, path, sizeof(path));
    struct stat st;
    return stat(path, &st) == 0;
}

bool js_storage_bc_current(const char *name)
{
    if (!name_is_valid(name)) return false;
    char path[96];
    full_path_bc(name, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    /* Cheap reject first: magic(4) + version(1). */
    unsigned char hdr[5];
    size_t n = fread(hdr, 1, sizeof(hdr), f);
    if (n != sizeof(hdr) ||
        memcmp(hdr, PLBC_MAGIC, 4) != 0 ||
        hdr[4] != PLBC_VERSION) {
        fclose(f);
        return false;
    }

    /* Magic + version alone don't prove the .bc is loadable: the on-disk
     * layout can change *within* a version (e.g. a new per-param field), and
     * the old gate (version byte only) then passed a stale-but-same-version
     * .bc that the player later rejected at play time with "bytecode
     * truncated". So validate it the only way that can't drift: parse it with
     * plbc_load() — the exact reader the player uses. If it doesn't load, the
     * .bc is stale; return false so the boot pass recompiles it from .js. */
    uint8_t *buf = (uint8_t *)malloc(BC_VALIDATE_MAX);
    if (!buf) { fclose(f); return true; }   /* OOM: assume current, don't churn */
    rewind(f);
    size_t len = fread(buf, 1, BC_VALIDATE_MAX, f);
    fclose(f);

    plbc_program_t *prog = (plbc_program_t *)malloc(sizeof(*prog));
    if (!prog) { free(buf); return true; }  /* OOM: assume current */
    bool ok = (plbc_load(buf, len, prog, NULL, 0) == ESP_OK);
    free(prog);
    free(buf);
    return ok;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int js_storage_collect_sorted(char (*out)[64], int max_names)
{
    if (!out || max_names <= 0) return 0;
    DIR *d = opendir(BASE_PATH);
    if (!d) return 0;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && count < max_names) {
        size_t len = strlen(entry->d_name);
        if (len < 4 || strcmp(entry->d_name + len - 3, ".js") != 0) continue;
        size_t base = len - 3;
        if (base >= 64) base = 63;
        memcpy(out[count], entry->d_name, base);
        out[count][base] = 0;
        count++;
    }
    closedir(d);
    qsort(out, count, sizeof(out[0]), cmp_names);
    return count;
}
