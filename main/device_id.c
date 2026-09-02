#include "device_id.h"
#include "esp_mac.h"
#include <stdio.h>

static char s_id[DEVICE_ID_LEN + 1];

const char *device_id_get(void)
{
    if (s_id[0] == '\0') {
        uint8_t mac[6] = {0};
        esp_efuse_mac_get_default(mac);
        snprintf(s_id, sizeof(s_id), "%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return s_id;
}
