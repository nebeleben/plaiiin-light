#include "js_api.h"
#include "js_storage.h"
#include "js_player.h"

#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "js_api";

#define MAX_UPLOAD_BYTES (48 * 1024)

/** Extract name from "/api/js/<name>". Returns pointer into req->uri or NULL. */
static const char *name_from_uri(const char *uri)
{
    const char *prefix = "/api/js/";
    if (strncmp(uri, prefix, strlen(prefix)) != 0) return NULL;
    const char *name = uri + strlen(prefix);
    if (*name == 0) return NULL;
    return name;
}

static esp_err_t send_err_json(httpd_req_t *req, int status, const char *msg)
{
    httpd_resp_set_status(req, status == 404 ? "404 Not Found"
                              : status == 400 ? "400 Bad Request"
                              : "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    char resp[192];
    snprintf(resp, sizeof(resp), "{\"status\":\"error\",\"message\":\"%s\"}", msg);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// GET /api/js -> {"scripts":[...],"playing":"name"|null}
static esp_err_t list_handler(httpd_req_t *req)
{
    char scripts[768];
    esp_err_t err = js_storage_list(scripts, sizeof(scripts));
    if (err != ESP_OK) return send_err_json(req, 500, "list failed");

    const char *playing = js_player_current_name();
    char resp[900];
    if (playing) {
        snprintf(resp, sizeof(resp), "{\"scripts\":%s,\"playing\":\"%s\"}", scripts, playing);
    } else {
        snprintf(resp, sizeof(resp), "{\"scripts\":%s,\"playing\":null}", scripts);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// GET /api/js/<name> -> script source as text/javascript
static esp_err_t read_handler(httpd_req_t *req)
{
    const char *name = name_from_uri(req->uri);
    if (!name) return send_err_json(req, 400, "missing name");
    char *source = NULL;
    size_t len = 0;
    esp_err_t err = js_storage_read(name, &source, &len);
    if (err == ESP_ERR_NOT_FOUND) return send_err_json(req, 404, "not found");
    if (err != ESP_OK) return send_err_json(req, 500, "read failed");
    httpd_resp_set_type(req, "text/javascript");
    httpd_resp_send(req, source, len);
    free(source);
    return ESP_OK;
}

static esp_err_t receive_body(httpd_req_t *req, char **out, size_t *len_out)
{
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > MAX_UPLOAD_BYTES) return ESP_ERR_INVALID_SIZE;
    char *buf = (char *)malloc(content_len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    int received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, buf + received, content_len - received);
        if (ret <= 0) { free(buf); return ESP_FAIL; }
        received += ret;
    }
    buf[content_len] = 0;
    *out = buf;
    *len_out = content_len;
    return ESP_OK;
}

// PUT /api/js/<name>  body: raw JS source
static esp_err_t write_handler(httpd_req_t *req)
{
    const char *name = name_from_uri(req->uri);
    if (!name) return send_err_json(req, 400, "missing name");
    char *body = NULL; size_t body_len = 0;
    esp_err_t err = receive_body(req, &body, &body_len);
    if (err != ESP_OK) return send_err_json(req, 400, "bad body");
    const char *verr = NULL;
    err = js_player_validate(body, &verr);
    if (err != ESP_OK) {
        char msg[160];
        snprintf(msg, sizeof(msg), "validation failed: %s", verr ? verr : "?");
        free(body);
        return send_err_json(req, 400, msg);
    }
    err = js_storage_write(name, body, body_len);
    free(body);
    if (err != ESP_OK) return send_err_json(req, 500, "write failed");
    httpd_resp_set_type(req, "application/json");
    char resp[96];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"name\":\"%s\"}", name);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// DELETE /api/js/<name>
static esp_err_t delete_handler(httpd_req_t *req)
{
    const char *name = name_from_uri(req->uri);
    if (!name) return send_err_json(req, 400, "missing name");
    esp_err_t err = js_storage_remove(name);
    if (err == ESP_ERR_NOT_FOUND) return send_err_json(req, 404, "not found");
    if (err != ESP_OK) return send_err_json(req, 500, "delete failed");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

// POST /api/play  body: {"file":"sparkle","fps":10}
static esp_err_t play_handler(httpd_req_t *req)
{
    char buf[192] = {0};
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return send_err_json(req, 400, "no body");

    // Parse "file":"name"
    const char *p = strstr(buf, "\"file\"");
    if (!p) return send_err_json(req, 400, "file missing");
    p = strchr(p, ':');
    if (!p) return send_err_json(req, 400, "file missing");
    p = strchr(p, '"');
    if (!p) return send_err_json(req, 400, "file missing");
    p++;
    const char *end = strchr(p, '"');
    if (!end) return send_err_json(req, 400, "file missing");
    char name[64] = {0};
    size_t n = end - p;
    if (n >= sizeof(name)) return send_err_json(req, 400, "name too long");
    memcpy(name, p, n);

    int fps = 10;
    const char *fp = strstr(buf, "\"fps\"");
    if (fp) {
        fp = strchr(fp, ':');
        if (fp) fps = atoi(fp + 1);
    }

    char *source = NULL;
    size_t len = 0;
    esp_err_t err = js_storage_read(name, &source, &len);
    if (err == ESP_ERR_NOT_FOUND) return send_err_json(req, 404, "script not found");
    if (err != ESP_OK) return send_err_json(req, 500, "read failed");
    err = js_player_start(source, fps);
    free(source);
    if (err != ESP_OK) return send_err_json(req, 500, "start failed");
    js_player_set_current_name(name);

    httpd_resp_set_type(req, "application/json");
    char resp[160];
    snprintf(resp, sizeof(resp), "{\"status\":\"ok\",\"playing\":\"%s\",\"fps\":%d}", name, fps);
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// POST /api/stop
static esp_err_t stop_handler(httpd_req_t *req)
{
    js_player_stop();
    js_player_set_current_name(NULL);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

esp_err_t js_api_register(httpd_handle_t server)
{
    httpd_uri_t list = {.uri="/api/js", .method=HTTP_GET, .handler=list_handler};
    httpd_register_uri_handler(server, &list);

    httpd_uri_t read_one = {.uri="/api/js/*", .method=HTTP_GET, .handler=read_handler};
    httpd_register_uri_handler(server, &read_one);

    httpd_uri_t write_one = {.uri="/api/js/*", .method=HTTP_PUT, .handler=write_handler};
    httpd_register_uri_handler(server, &write_one);

    httpd_uri_t delete_one = {.uri="/api/js/*", .method=HTTP_DELETE, .handler=delete_handler};
    httpd_register_uri_handler(server, &delete_one);

    httpd_uri_t play = {.uri="/api/play", .method=HTTP_POST, .handler=play_handler};
    httpd_register_uri_handler(server, &play);

    httpd_uri_t stop = {.uri="/api/stop", .method=HTTP_POST, .handler=stop_handler};
    httpd_register_uri_handler(server, &stop);

    ESP_LOGI(TAG, "JS API registered");
    return ESP_OK;
}
