#include "mdns_service.h"
#include "config_store.h"
#include "mdns.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include <ctype.h>
#include <string.h>

static const char *TAG = "mdns_svc";

// mDNS hostnames must be a subset of [a-z0-9-]. Sanitise the configured
// node name in place — anything else becomes a dash, uppercase becomes lowercase.
static void sanitize_hostname(char *s)
{
    for (char *p = s; *p; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') *p = (char)(c - 'A' + 'a');
        else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) *p = '-';
    }
}

esp_err_t mdns_service_start(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }

    char node_name[64];
    char vendor[64];
    char api_ver[32];
    char lamp_type[32];
    config_get_str_or(CONFIG_KEY_NODE_NAME,   node_name,  sizeof(node_name),  CONFIG_PLAIIIN_NODE_NAME);
    config_get_str_or(CONFIG_KEY_VENDOR_NAME, vendor,     sizeof(vendor),     CONFIG_PLAIIIN_VENDOR_NAME);
    config_get_str_or(CONFIG_KEY_API_VERSION, api_ver,    sizeof(api_ver),    CONFIG_PLAIIIN_API_VERSION);
    config_get_str_or(CONFIG_KEY_LAMP_TYPE,   lamp_type,  sizeof(lamp_type),  CONFIG_PLAIIIN_LAMP_TYPE);

    char hostname[64];
    snprintf(hostname, sizeof(hostname), "%s", node_name);
    sanitize_hostname(hostname);
    if (hostname[0] == '\0') snprintf(hostname, sizeof(hostname), "plaiiinlight");

    mdns_hostname_set(hostname);
    mdns_instance_name_set(node_name);

    // Two TXT record sets — same content. Lets clients filter by service.
    mdns_txt_item_t txt[] = {
        {"vendor",   vendor},
        {"node",     node_name},
        {"fw",       (char *)CONFIG_PLAIIIN_FIRMWARE_VERSION},
        {"api",      api_ver},
        {"lamp",     lamp_type},
        {"path",     "/api"},
    };
    const size_t txt_count = sizeof(txt) / sizeof(txt[0]);

    uint16_t port = (uint16_t)CONFIG_PLAIIIN_HTTP_PORT;

    mdns_service_add(NULL, "_http",          "_tcp", port, txt, txt_count);
    mdns_service_add(NULL, "_plaiiinlight",  "_tcp", port, txt, txt_count);

    ESP_LOGI(TAG, "mDNS up: %s.local on _http._tcp + _plaiiinlight._tcp:%u",
             hostname, port);
    return ESP_OK;
}
