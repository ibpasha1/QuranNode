// ota_esp32.c — over-the-air firmware update: join Wi-Fi (STA), serve a tiny
// upload page, and flash the posted .bin into the inactive OTA slot.
//
// Flow: ota_start() brings up Wi-Fi and an HTTP server, then returns immediately
// (the render loop keeps running and can show the IP via ota_ip()). Point a phone
// browser at http://<ip>/, pick a firmware.bin, and it streams straight into the
// staging partition via esp_ota_write; on success it sets the boot slot and
// reboots. If the new image fails to boot/verify, the bootloader rolls back.
//
// Wi-Fi credentials come from -DWIFI_SSID / -DWIFI_PASS (platformio.ini).
#include "ota.h"
#include "hal.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

// GitHub repo to pull firmware from (set via -DOTA_REPO="owner/repo").
#ifndef OTA_REPO
#define OTA_REPO "OWNER/REPO"
#endif
#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif
#define OTA_FW_URL  "https://github.com/" OTA_REPO "/releases/latest/download/firmware.bin"
#define OTA_VER_URL "https://github.com/" OTA_REPO "/releases/latest/download/version.txt"

static const char *TAG = "OTA";

#ifndef WIFI_SSID
#define WIFI_SSID "set-WIFI_SSID"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "set-WIFI_PASS"
#endif

static httpd_handle_t s_server = NULL;
static char s_ip[16] = "";
static bool s_started = false;

static const char *PAGE =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<body style='font-family:system-ui;max-width:460px;margin:2em auto;padding:0 1em'>"
    "<h2>QuranNode firmware update</h2>"
    "<input type=file id=f accept='.bin'>"
    "<button onclick='up()' style='padding:.5em 1em'>Flash</button>"
    "<pre id=o></pre>"
    "<script>async function up(){let f=document.getElementById('f').files[0];"
    "if(!f){o.textContent='pick a .bin first';return;}"
    "o.textContent='uploading '+f.size+' bytes…';"
    "try{let r=await fetch('/update',{method:'POST',body:f});"
    "o.textContent=await r.text();}catch(e){o.textContent='upload failed: '+e;}}"
    "</script></body>";

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t update_post(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition"); return ESP_FAIL; }
    ESP_LOGW(TAG, "OTA -> %s, %d bytes incoming", part->label, req->content_len);

    esp_ota_handle_t h = 0;
    if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
        return ESP_FAIL;
    }

    char buf[1460];
    int remaining = req->content_len, written = 0;
    while (remaining > 0) {
        int want = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int r = httpd_req_recv(req, buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { esp_ota_abort(h); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv error"); return ESP_FAIL; }
        if (esp_ota_write(h, buf, r) != ESP_OK) { esp_ota_abort(h); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash write failed"); return ESP_FAIL; }
        written += r; remaining -= r;
    }

    if (esp_ota_end(h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "image invalid (bad/truncated .bin)");
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "OTA ok: %d bytes -> %s. Rebooting.", written, part->label);
    httpd_resp_sendstr(req, "OK — flashed, rebooting. Reconnect the reader in ~10s.");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

// --- Remote recitation scoring (Quran Teacher V2) ---------------------------
// POSTs the take to TEACHER_URL (secrets.ini) and parses the CSV verdicts.
// No TEACHER_URL configured -> permanent 0 (pure-offline device).
static bool wifi_up(int timeout_ms);   // defined below with the OTA machinery
static const char *verdict_names[] = { "GOOD", "UNSURE", "MISMATCH", "MISSING", "UNCLEAR" };

int hal_score_remote(const uint8_t *wav, uint32_t wav_len, int surah, int ayah,
                     RemoteWord *out, int max_words)
{
#ifndef TEACHER_URL
    (void)wav; (void)wav_len; (void)surah; (void)ayah; (void)out; (void)max_words;
    return 0;
#else
    if (!wifi_up(8000)) return 0;

    char url[160];
    snprintf(url, sizeof(url), "%s/score?surah=%d&ayah=%d", TEACHER_URL, surah, ayah);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 12000,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return 0;
    esp_http_client_set_header(h, "Content-Type", "audio/wav");

    static char resp[2048];
    int n = 0;
    if (esp_http_client_open(h, (int)wav_len) == ESP_OK) {
        if (esp_http_client_write(h, (const char *)wav, (int)wav_len) == (int)wav_len &&
            esp_http_client_fetch_headers(h) >= 0 &&
            esp_http_client_get_status_code(h) == 200) {
            int got = esp_http_client_read_response(h, resp, sizeof(resp) - 1);
            if (got > 0) {
                resp[got] = 0;
                // Parse "word,VERDICT,score,start,end" rows.
                for (char *line = strtok(resp, "\n"); line && n < max_words;
                     line = strtok(NULL, "\n")) {
                    if (line[0] == '#' || line[0] == 'w') continue;   // comment/header
                    int wi; char vs[12]; float sc; unsigned a, b;
                    if (sscanf(line, "%d,%11[^,],%f,%u,%u", &wi, vs, &sc, &a, &b) == 5) {
                        uint8_t v = 4;
                        for (uint8_t k = 0; k < 5; k++)
                            if (!strcmp(vs, verdict_names[k])) { v = k; break; }
                        out[n++] = (RemoteWord){ v, sc, a, b };
                    }
                }
                ESP_LOGI(TAG, "remote score %d:%d -> %d words", surah, ayah, n);
            }
        } else {
            ESP_LOGW(TAG, "remote score failed (status %d)",
                     esp_http_client_get_status_code(h));
        }
    }
    esp_http_client_cleanup(h);
    return n;
#endif
}

// --- In-RAM blob downloads (/takes) ---------------------------------------
// Training takes live in PSRAM and download over Wi-Fi — the SD-card write
// path proved unreliable on some cards (persistent r2=0x2000 on fresh
// clusters), and the takes only need to reach the host once.
#define BLOB_MAX 24
static struct { const void *data; size_t len; char name[40]; } s_blobs[BLOB_MAX];

void hal_serve_blob(int idx, const char *name, const void *data, size_t len)
{
    if (idx < 0 || idx >= BLOB_MAX) return;
    s_blobs[idx].data = data;
    s_blobs[idx].len = data ? len : 0;
    snprintf(s_blobs[idx].name, sizeof(s_blobs[idx].name), "%s",
             (data && name) ? name : "");
}

static esp_err_t takes_get(httpd_req_t *req)
{
    const char *p = strrchr(req->uri, '/');
    if (p && p[1] >= '0' && p[1] <= '9') {          // /takes/<idx>
        int i = atoi(p + 1);
        if (i < 0 || i >= BLOB_MAX || !s_blobs[i].data) {
            httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such take");
            return ESP_FAIL;
        }
        char cd[64];
        snprintf(cd, sizeof(cd), "attachment; filename=\"%s\"", s_blobs[i].name);
        httpd_resp_set_type(req, "audio/wav");
        httpd_resp_set_hdr(req, "Content-Disposition", cd);
        return httpd_resp_send(req, s_blobs[i].data, s_blobs[i].len);
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, "<h2>Training takes</h2><ul>");
    char line[128];
    int n = 0;
    for (int i = 0; i < BLOB_MAX; i++) {
        if (!s_blobs[i].data) continue;
        snprintf(line, sizeof(line),
                 "<li><a href=\"/takes/%d\" download>%s</a> (%u KB)</li>",
                 i, s_blobs[i].name, (unsigned)(s_blobs[i].len / 1024));
        httpd_resp_sendstr_chunk(req, line);
        n++;
    }
    if (!n) httpd_resp_sendstr_chunk(req, "<li>none yet</li>");
    httpd_resp_sendstr_chunk(req,
        "</ul><p>curl -OJ http://this-ip/takes/N &nbsp;(per file)</p>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip[0] = '\0';
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying…");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGW(TAG, "*** Wi-Fi up. Open http://%s/ to update firmware. ***", s_ip);
    }
}

// Bring up Wi-Fi STA (once) and wait up to timeout_ms for an IP. Idempotent —
// safe to call from both the boot update-check and the manual update path.
static bool s_wifi_inited = false;
static bool wifi_up(int timeout_ms)
{
    if (!s_wifi_inited) {
        esp_netif_init();
        esp_event_loop_create_default();
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
        esp_wifi_init(&wc);
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL);
        wifi_config_t cfg = {0};
        strncpy((char *)cfg.sta.ssid, WIFI_SSID, sizeof(cfg.sta.ssid) - 1);
        strncpy((char *)cfg.sta.password, WIFI_PASS, sizeof(cfg.sta.password) - 1);
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
        s_wifi_inited = true;
    }
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);   // radio always-on (modem-sleep drops HTTP under load)
    ESP_LOGW(TAG, "connecting to Wi-Fi \"%s\"…", WIFI_SSID);
    for (int i = 0; i < timeout_ms / 100 && !s_ip[0]; i++) vTaskDelay(pdMS_TO_TICKS(100));
    return s_ip[0] != 0;
}

static void wifi_down(void) { esp_wifi_stop(); s_ip[0] = '\0'; }

bool ota_start(void)
{
    if (s_started) { ESP_LOGI(TAG, "OTA mode already active (ip=%s)", s_ip[0] ? s_ip : "connecting"); return true; }
    s_started = true;

    wifi_up(15000);   // event-driven; the local upload server also works once up

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 8192;
    hc.lru_purge_enable = true;
    hc.uri_match_fn = httpd_uri_match_wildcard;   // for /takes/*
    if (httpd_start(&s_server, &hc) == ESP_OK) {
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
        httpd_uri_t upd  = { .uri = "/update", .method = HTTP_POST, .handler = update_post };
        httpd_uri_t tks  = { .uri = "/takes*", .method = HTTP_GET, .handler = takes_get };
        httpd_register_uri_handler(s_server, &root);
        httpd_register_uri_handler(s_server, &upd);
        httpd_register_uri_handler(s_server, &tks);
    } else {
        ESP_LOGE(TAG, "http server failed to start");
    }
    return true;
}

const char *ota_ip(void) { return s_ip[0] ? s_ip : NULL; }

// --- HAL interface (called from the portable Settings scene) -------------
void hal_ota_start(void) { ota_start(); }

// --- GitHub Releases pull (self-update over HTTPS) -----------------------
static char s_status[64];
const char *hal_ota_status(void) { return s_status[0] ? s_status : NULL; }
static void set_status(const char *s) { snprintf(s_status, sizeof(s_status), "%s", s); }

// Download the latest firmware.bin from GitHub and flash it (blocking). Reboots
// on success; sets a status string and returns on failure.
void hal_ota_apply(void)
{
    set_status("Downloading update...");
    ESP_LOGW(TAG, "pull: %s", OTA_FW_URL);
    esp_http_client_config_t http = {
        .url = OTA_FW_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,   // Mozilla roots (covers GitHub)
        .timeout_ms = 20000,
        .keep_alive_enable = true,
        // GitHub's CDN (objects.githubusercontent.com) sends large response
        // headers; the default 512B buffer overflows ("HTTP_CLIENT: Out of buffer").
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };
    esp_err_t r = esp_https_ota(&cfg);
    if (r == ESP_OK) {
        set_status("Updated! Rebooting...");
        ESP_LOGW(TAG, "pull OTA ok — rebooting");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "pull OTA failed: %s", esp_err_to_name(r));
        set_status("Update failed");
    }
}

static void pull_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 200 && !s_ip[0]; i++) vTaskDelay(pdMS_TO_TICKS(100));  // await IP
    if (!s_ip[0]) { set_status("Wi-Fi failed"); vTaskDelete(NULL); return; }
    hal_ota_apply();   // reboots on success; sets status on failure
    vTaskDelete(NULL);
}

void hal_ota_pull(void)
{
    set_status("Connecting to Wi-Fi...");
    ota_start();   // Wi-Fi STA + local /update server (fallback for a same-network push)
    xTaskCreate(pull_task, "ota_pull", 8192, NULL, 5, NULL);
}

// --- Boot-time auto-update: fetch the latest version tag and compare ------
typedef struct { char *buf; int len, cap; } TextBuf;
static esp_err_t ver_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data) {
        TextBuf *t = (TextBuf *)e->user_data;
        int n = e->data_len;
        if (n > t->cap - 1 - t->len) n = t->cap - 1 - t->len;
        if (n > 0) { memcpy(t->buf + t->len, e->data, n); t->len += n; t->buf[t->len] = '\0'; }
    }
    return ESP_OK;
}

static bool http_get_text(const char *url, char *out, int cap)
{
    TextBuf t = { out, 0, cap };
    out[0] = '\0';
    esp_http_client_config_t c = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000, .event_handler = ver_evt, .user_data = &t,
        .buffer_size = 4096, .buffer_size_tx = 2048,   // GitHub CDN has big headers
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    esp_err_t e = esp_http_client_perform(h);   // follows the GitHub redirect
    int sc = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    return e == ESP_OK && sc == 200 && t.len > 0;
}

// Connect Wi-Fi, compare the latest release's version.txt to this build's
// FW_VERSION. Returns true (Wi-Fi left up) if a different version is available;
// otherwise tears Wi-Fi back down and returns false. Called once at boot.
bool hal_ota_boot_check(void)
{
    if (!wifi_up(6000)) { wifi_down(); return false; }   // no Wi-Fi → just boot

    char latest[48];
    if (!http_get_text(OTA_VER_URL, latest, sizeof(latest))) {
        ESP_LOGW(TAG, "version check failed — skipping");
        wifi_down();
        return false;
    }
    for (int i = (int)strlen(latest) - 1; i >= 0 &&
         (latest[i] == '\n' || latest[i] == '\r' || latest[i] == ' ' || latest[i] == '\t'); i--)
        latest[i] = '\0';

    ESP_LOGW(TAG, "auto-update: installed=%s latest=%s", FW_VERSION, latest);
    if (strcmp(latest, FW_VERSION) == 0) { wifi_down(); return false; }  // up to date
    return true;   // newer/different available — leave Wi-Fi up for hal_ota_apply()
}

const char *hal_ota_url(void)
{
    static char url[32];
    if (!s_ip[0]) return NULL;              // still connecting
    snprintf(url, sizeof(url), "http://%s/", s_ip);
    return url;
}

void ota_mark_valid(void)
{
    // Cancel the pending rollback once we've booted + initialised successfully.
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "running from %s — OTA image marked valid (rollback cancelled)", run->label);
    }
}
