#include "app_config.h"

#include <stdio.h>
#include <string.h>

#include "app_error.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

// 编译期兜底配置。有这个文件就填，没有也不影响构建。
// 仅开发期方便：量产一律走网页配网。
#if __has_include("device_secret.h")
#include "device_secret.h"
#endif

#ifndef DEVICE_SECRET_WIFI_SSID
#define DEVICE_SECRET_WIFI_SSID ""
#endif
#ifndef DEVICE_SECRET_WIFI_PASS
#define DEVICE_SECRET_WIFI_PASS ""
#endif
#ifndef DEVICE_SECRET_NAS_URL
#define DEVICE_SECRET_NAS_URL ""
#endif
#ifndef DEVICE_SECRET_TOKEN
#define DEVICE_SECRET_TOKEN ""
#endif

#define NVS_NS "appcfg"

// 键名最长 15 字符，全部保持短名便于人工用 nvs_tool 排查。
#define K_SSID  "ssid"
#define K_PASS  "pass"
#define K_URL   "url"
#define K_TOKEN "token"
#define K_DEV   "dev"

static app_config_t s_cfg;
static bool s_nvs_ok;

// 往 char 数组里写字符串，超长截断，始终以 0 结尾。空串表示"不修改"，由调用方先判断。
static void store_str(char *dst, size_t cap, const char *src) {
    if (!src) return;
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static bool nvs_read_str(nvs_handle_t h, const char *key, char *dst, size_t cap) {
    size_t len = cap;
    esp_err_t err = nvs_get_str(h, key, dst, &len);
    if (err == ESP_OK && dst[0]) return true;
    dst[0] = 0;
    return false;
}

static void nvs_write_str(nvs_handle_t h, const char *key, const char *val) {
    if (nvs_set_str(h, key, val) != ESP_OK) {
        app_error_report(APP_E501_FLASH, APP_SRC_CONFIG, "nvs_set_str(%s) failed", key);
    }
}

// dev8 = MAC 低 32 位十六进制。不是随机值：同一台设备重刷固件后保持不变，
// 服务端 tokens.json 里的绑定关系才不会失效。
static void ensure_dev(void) {
    if (s_cfg.dev[0]) return;

    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        // 读不到 MAC 时用随机数兜底，保证 session_id 能拼出来，日志里会写明。
        app_error_report(APP_E501_FLASH, APP_SRC_CONFIG, "esp_read_mac failed, use random dev");
        snprintf(s_cfg.dev, sizeof s_cfg.dev, "%08x", (unsigned)esp_random());
    } else {
        uint32_t low = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                       ((uint32_t)mac[4] << 8) | mac[5];
        snprintf(s_cfg.dev, sizeof s_cfg.dev, "%08x", (unsigned)low);
    }

    if (s_nvs_ok) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_write_str(h, K_DEV, s_cfg.dev);
            nvs_commit(h);
            nvs_close(h);
        }
    }
}

void app_config_init(void) {
    memset(&s_cfg, 0, sizeof s_cfg);

    esp_err_t err = nvs_flash_init();
    // NVS 也存录音状态和绑定密钥；异常时保留原分区供恢复，不自动擦除。
    if (err != ESP_OK) {
        app_error_report(APP_E501_FLASH, APP_SRC_CONFIG, "nvs_flash_init failed: %s",
                         esp_err_to_name(err));
        // NVS 不可用时仍用编译期值启动，能录音但掉电后配置丢。
    } else {
        s_nvs_ok = true;
    }

    if (s_nvs_ok) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            nvs_read_str(h, K_SSID, s_cfg.ssid, sizeof s_cfg.ssid);
            nvs_read_str(h, K_PASS, s_cfg.pass, sizeof s_cfg.pass);
            nvs_read_str(h, K_URL, s_cfg.url, sizeof s_cfg.url);
            nvs_read_str(h, K_TOKEN, s_cfg.token, sizeof s_cfg.token);
            nvs_read_str(h, K_DEV, s_cfg.dev, sizeof s_cfg.dev);
            nvs_close(h);
        }
    }

    // NVS 为空时用编译期值填充，内容不落 NVS（避免把开发凭据写进 flash）。
    if (!s_cfg.ssid[0]) store_str(s_cfg.ssid, sizeof s_cfg.ssid, DEVICE_SECRET_WIFI_SSID);
    if (!s_cfg.pass[0]) store_str(s_cfg.pass, sizeof s_cfg.pass, DEVICE_SECRET_WIFI_PASS);
    if (!s_cfg.url[0]) store_str(s_cfg.url, sizeof s_cfg.url, DEVICE_SECRET_NAS_URL);
    if (!s_cfg.token[0]) store_str(s_cfg.token, sizeof s_cfg.token, DEVICE_SECRET_TOKEN);

    ensure_dev();

    // 只打印长度，不打印内容，避免凭据进日志。
    app_log("config", "dev=%s ssid=%s(%u) url=%s(%u) token=%s nvs=%d",
            s_cfg.dev, s_cfg.ssid[0] ? "set" : "none", (unsigned)strlen(s_cfg.ssid),
            s_cfg.url[0] ? "set" : "none", (unsigned)strlen(s_cfg.url),
            s_cfg.token[0] ? "set" : "none", (int)s_nvs_ok);
}

const app_config_t *app_config_get(void) {
    return &s_cfg;
}

bool app_config_ready(void) {
    return s_cfg.ssid[0] && s_cfg.url[0];
}

bool app_config_has_token(void) {
    return s_cfg.token[0] != 0;
}

const char *app_config_dev(void) {
    return s_cfg.dev;
}

void app_config_set_wifi(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0]) return;
    store_str(s_cfg.ssid, sizeof s_cfg.ssid, ssid);
    // 空密码是合法配置（开放网络），所以密码不做"空串跳过"判断，只在调用方明确传 NULL 时保留。
    if (pass) store_str(s_cfg.pass, sizeof s_cfg.pass, pass);

    if (!s_nvs_ok) {
        app_error_report(APP_E501_FLASH, APP_SRC_CONFIG, "wifi saved in RAM only, nvs unavailable");
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        app_error_report(APP_E501_FLASH, APP_SRC_CONFIG, "nvs_open failed");
        return;
    }
    nvs_write_str(h, K_SSID, s_cfg.ssid);
    nvs_write_str(h, K_PASS, s_cfg.pass);
    nvs_commit(h);
    nvs_close(h);
    app_log("config", "wifi saved ssid_len=%u", (unsigned)strlen(s_cfg.ssid));
}

void app_config_set_upload(const char *url, const char *token) {
    if (url && url[0]) store_str(s_cfg.url, sizeof s_cfg.url, url);
    if (token && token[0]) store_str(s_cfg.token, sizeof s_cfg.token, token);

    if (!s_nvs_ok) {
        app_error_report(APP_E501_FLASH, APP_SRC_CONFIG, "upload cfg saved in RAM only");
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        app_error_report(APP_E501_FLASH, APP_SRC_CONFIG, "nvs_open failed");
        return;
    }
    if (url && url[0]) nvs_write_str(h, K_URL, s_cfg.url);
    if (token && token[0]) nvs_write_str(h, K_TOKEN, s_cfg.token);
    nvs_commit(h);
    nvs_close(h);
    app_log("config", "upload saved url_len=%u token=%s", (unsigned)strlen(s_cfg.url),
            s_cfg.token[0] ? "set" : "none");
}

void app_config_erase_all(void) {
    if (!s_nvs_ok) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    app_log("config", "all config erased");
}