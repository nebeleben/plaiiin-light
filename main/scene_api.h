#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_http_server.h"
// Scenes — named, saved snapshots of the lamp's user-facing state, recalled
// in one call (see scene_api.c for the document and the activation order).
esp_err_t scene_api_register(httpd_handle_t server);

/** Activate the saved scene `name` — what POST /api/scenes/<name>/activate
 *  does. Returns 0, or an HTTP status (404 no such scene, ...) with *why set.
 *  On success the scene becomes the lamp's current scene and is announced to
 *  the swarm (a no-op while a swarm packet is being applied). */
int scene_api_activate(const char *name, const char **why);

/** True if a scene named `name` is saved on this lamp. */
bool scene_api_exists(const char *name);

/** Copies the current scene's name into `out` and returns true while the
 *  lamp still shows what that scene put up (same mode and effect / image).
 *  Colour, brightness and tune changes don't end it — clients show those as
 *  drift; switching mode, effect or image does (it's cleared then). */
bool scene_api_current(char *out, size_t cap);

/** A user picked a mode explicitly (HTTP / BLE / MQTT): that ends the current
 *  scene even when the mode doesn't change (Scene -> JS on a lamp already in
 *  js). The end is announced to the swarm, whose peers end theirs. */
void scene_api_end(void);
