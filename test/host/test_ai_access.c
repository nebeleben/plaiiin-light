#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ai_access.h"

#define R(m, u, p) assert(ai_route_perm(AI_M_##m, u) == (p))

int main(void)
{
    static const uint32_t bits[] = {AI_PERM_CONTROL, AI_PERM_EFFECTS, AI_PERM_IMAGES, AI_PERM_SCENES,
                                    AI_PERM_STREAM,  AI_PERM_OTA,     AI_PERM_RENAME, AI_PERM_SWARM};
    for (unsigned i = 0; i < sizeof bits / sizeof bits[0]; i++) {
        const char *n = ai_perm_name(bits[i]);
        assert(n && ai_perm_from_name(n) == bits[i]);
    }
    assert(ai_perm_from_name("admin") == 0);
    assert(ai_perm_name(AI_PERM_CONTROL | AI_PERM_OTA) == NULL);
    assert(ai_perm_name(0) == NULL);
    assert(AI_PERM_ALL == 0xFFu);

    R(POST, "/api/power", AI_PERM_CONTROL);
    R(POST, "/api/color", AI_PERM_CONTROL);
    R(POST, "/api/brightness", AI_PERM_CONTROL);
    R(POST, "/api/play", AI_PERM_CONTROL);
    R(POST, "/api/play/next", AI_PERM_CONTROL);
    R(POST, "/api/play/prev", AI_PERM_CONTROL);
    R(POST, "/api/stop", AI_PERM_CONTROL);
    R(POST, "/api/fade", AI_PERM_CONTROL);
    R(PUT, "/api/mode", AI_PERM_CONTROL);
    R(POST, "/api/mode", AI_PERM_CONTROL);
    R(POST, "/api/scenes/evening/activate", AI_PERM_CONTROL);
    R(PUT, "/api/js/rain", AI_PERM_EFFECTS);
    R(DELETE, "/api/js/rain", AI_PERM_EFFECTS);
    R(PUT, "/api/js/rain/params", AI_PERM_EFFECTS);
    R(POST, "/api/js/validate", AI_PERM_EFFECTS);
    R(POST, "/api/js/compile", AI_PERM_EFFECTS);
    R(POST, "/api/frame", AI_PERM_IMAGES);
    R(POST, "/api/images", AI_PERM_IMAGES);
    R(POST, "/api/images/img-001/show", AI_PERM_IMAGES);
    R(POST, "/api/images/img-001/label", AI_PERM_IMAGES);
    R(DELETE, "/api/images/img-001", AI_PERM_IMAGES);
    R(PUT, "/api/scenes/evening", AI_PERM_SCENES);
    R(DELETE, "/api/scenes/evening", AI_PERM_SCENES);
    R(POST, "/api/scenes/evening/capture", AI_PERM_SCENES);
    R(GET, "/ws", AI_PERM_STREAM);
    R(POST, "/api/ota", AI_PERM_OTA);
    R(POST, "/api/name", AI_PERM_RENAME);
    R(GET, "/api/swarm", AI_PERM_SWARM);
    R(GET, "/api/swarm/stats", AI_PERM_SWARM);
    R(DELETE, "/api/swarm", AI_ROUTE_DENIED);
    R(POST, "/api/swarm/enable", AI_ROUTE_DENIED);
    R(POST, "/api/swarm/ping", AI_ROUTE_DENIED);

    R(GET, "/api/state", AI_ROUTE_READ);
    R(GET, "/api/js/rain", AI_ROUTE_READ);
    R(GET, "/api/scenes", AI_ROUTE_READ);
    R(GET, "/api/images/img-001", AI_ROUTE_READ);

    static const char *denied_post[] = {"/api/pair", "/api/keys", "/api/limits", "/api/grid", "/api/reset",
                                        "/config",   "/network",  "/api/ai/key", "/api/swarm"};
    for (unsigned i = 0; i < sizeof denied_post / sizeof denied_post[0]; i++)
        R(POST, denied_post[i], AI_ROUTE_DENIED);
    R(PUT, "/api/form-prompt", AI_ROUTE_DENIED);
    R(DELETE, "/api/pair", AI_ROUTE_DENIED);
    R(PATCH, "/api/keys/ab12cd34", AI_ROUTE_DENIED);

    /* query strings and trailing slash */
    R(POST, "/api/power/", AI_PERM_CONTROL);
    R(PUT, "/api/js/rain/params?x=1", AI_PERM_EFFECTS);
    R(GET, "/api/state?x=1", AI_ROUTE_READ);

    puts("ai_access: ok");
    return 0;
}
