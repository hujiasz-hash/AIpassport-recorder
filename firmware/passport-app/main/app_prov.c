#include "app_prov.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "app_error.h"
#include "app_net.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "prov"

#define AP_NAME_PREFIX "AIPassport-"
#define AP_MAX_CLIENT 2

// 配好后如果手机一直连着不放，到点也把热点关掉，否则设备一直开着 AP 费电。
#define AP_KEEPALIVE_US (10LL * 60 * 1000 * 1000)

static bool s_active;
static bool s_got_cfg;
static volatile int s_clients;
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_server;
static int64_t s_cfg_at_us;
static char s_ap_name[24];

// ---- URL 与 token 校验 ---------------------------------------------------
// 与 BluFi 那套保持一致：只接受 http(s)://host[:port]，路径由固件自己拼。

static bool url_ok(const char *u) {
    if (strncmp(u, "http://", 7) != 0 && strncmp(u, "https://", 8) != 0) return false;
    const char *p = strstr(u, "://");
    if (!p) return false;
    p += 3;
    if (!*p) return false;
    size_t host_len = strcspn(p, "/");
    if (host_len == 0 || host_len > 100) return false;
    return p[host_len] == 0;
}

// ---- 表单解析 ------------------------------------------------------------
// application/x-www-form-urlencoded。只有四个字段，不引第三方解析器。
// 把 %XX 和 '+' 还原成原字符，就地写回 buf。

static void url_decode(char *buf) {
    char *r = buf, *w = buf;
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%' && r[1] && r[2]) {
            int hi = r[1] <= '9' ? r[1] - '0' : (r[1] | 0x20) - 'a' + 10;
            int lo = r[2] <= '9' ? r[2] - '0' : (r[2] | 0x20) - 'a' + 10;
            *w++ = (char)((hi << 4) | lo);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

// 在 form 里找 key，把值解到 dst。找不到返回 false。
static bool form_get(const char *form, const char *key, char *dst, size_t cap) {
    dst[0] = 0;
    size_t klen = strlen(key);
    const char *p = form;
    while (p && *p) {
        const char *next = strchr(p, '&');
        size_t seg_len = next ? (size_t)(next - p) : strlen(p);
        if (seg_len > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *val = p + klen + 1;
            size_t vlen = seg_len - klen - 1;
            if (vlen >= cap) vlen = cap - 1;
            memcpy(dst, val, vlen);
            dst[vlen] = 0;
            url_decode(dst);
            return true;
        }
        p = next ? next + 1 : NULL;
    }
    return false;
}

// ---- 页面 ----------------------------------------------------------------
// 全部内嵌，不依赖外网。手机连上热点时本来也没有外网，任何 CDN 引用都会白屏。

static const char PAGE_HTML[] =
    "<!DOCTYPE html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>AI PASSPORT</title><style>"
    "*{box-sizing:border-box}"
    "body{margin:0;padding:24px 16px;font:16px/1.5 -apple-system,sans-serif;"
    "background:#f4f1e8;color:#1d1d1b}"
    "h1{font-size:20px;margin:0 0 4px}"
    ".d{font-size:13px;color:#6b6b66;margin-bottom:20px}"
    "label{display:block;font-size:13px;margin:14px 0 6px;color:#454540}"
    "input{width:100%;padding:11px 12px;border:1px solid #c9c5b8;border-radius:8px;"
    "font-size:16px;background:#fff}"
    "button{width:100%;margin-top:24px;padding:14px;border:0;border-radius:8px;"
    "background:#1d1d1b;color:#f4f1e8;font-size:16px}"
    "button:disabled{opacity:.45}"
    "#m{margin-top:16px;padding:12px;border-radius:8px;font-size:14px;display:none}"
    ".ok{display:block!important;background:#dfe9df;color:#1d5c1d}"
    ".er{display:block!important;background:#ecdada;color:#8a1c1c}"
    "</style></head><body>"
    "<h1>AI PASSPORT 配网</h1>"
    "<div class=d>连上本热点后在这里填写，保存即生效</div>"
    "<form id=f>"
    "<label>Wi-Fi 名称</label><input name=ssid required autocapitalize=off>"
    "<label>Wi-Fi 密码</label><input name=pass type=password>"
    "<label>NAS 地址</label><input name=url placeholder='http://192.168.1.10:8000' required>"
    "<label>Token</label><input name=token type=password>"
    "<button id=b>保存</button></form>"
    "<div id=m></div><script>"
    "const f=document.getElementById('f'),b=document.getElementById('b'),m=document.getElementById('m');"
    "f.onsubmit=async e=>{e.preventDefault();b.disabled=true;"
    "m.className='';m.textContent='正在保存…';"
    "try{"
    "const r=await fetch('/save',{method:'POST',body:new URLSearchParams(new FormData(f))});"
    "const j=await r.json();"
    "if(!j.ok){m.className='er';m.textContent='保存失败：'+(j.error||'未知原因');"
    "b.disabled=false;return}"
    "m.textContent='已保存，正在连接 Wi-Fi…';"
    "const t=setInterval(async()=>{"
    "try{const s=await(await fetch('/status')).json();"
    "if(s.done){clearInterval(t);"
    "m.className=s.ok?'ok':'er';"
    "m.textContent=s.ok?'配置完成，设备已联网。可以断开本热点了。':"
    "'连接失败：'+(s.error||'未知原因')+'。';"
    "b.disabled=!s.ok;"
    "if(!s.ok)m.className='er';}"
    "}catch(_){}},1500);"
    "}catch(_){m.className='er';m.textContent='请求失败，请重试';b.disabled=false}}"
    "</script></body></html>";

static esp_err_t root_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

// 保存：校验 → 写 NVS → 交给网络模块连接。不等连接结果，页面随后轮询 /status。
static esp_err_t save_post(httpd_req_t *req) {
    char body[512];
    if (req->content_len == 0 || req->content_len >= sizeof body) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad_len\"}");
    }
    int got = httpd_req_recv(req, body, req->content_len);
    if (got <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"recv\"}");
    }
    body[got] = 0;

    char ssid[CFG_SSID_MAX] = "";
    char pass[CFG_PASS_MAX] = "";
    char url[CFG_URL_MAX] = "";
    char token[CFG_TOKEN_MAX] = "";

    form_get(body, "ssid", ssid, sizeof ssid);
    form_get(body, "pass", pass, sizeof pass);
    form_get(body, "url", url, sizeof url);
    form_get(body, "token", token, sizeof token);

    const char *err = NULL;
    if (!ssid[0]) err = "no_ssid";
    else if (!url_ok(url)) err = "bad_url";
    else if (token[0] && strlen(token) < 8) err = "bad_token";

    if (err) {
        app_error_report(APP_E300_SERVER_REJECT, APP_SRC_PROV, "web cfg rejected: %s", err);
        char out[64];
        snprintf(out, sizeof out, "{\"ok\":false,\"error\":\"%s\"}", err);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, out);
    }

    app_config_set_wifi(ssid, pass);
    app_config_set_upload(url, token[0] ? token : NULL);
    s_got_cfg = true;
    s_cfg_at_us = esp_timer_get_time();
    app_log(TAG, "web cfg saved ssid_len=%u url_len=%u token=%s",
            (unsigned)strlen(ssid), (unsigned)strlen(url), token[0] ? "set" : "none");

    // 不等结果：连 Wi-Fi 要几秒，页面自己轮询 /status。
    app_net_apply_credentials();

    httpd_resp_set_type(req, "application/json");
    char out[96];
    snprintf(out, sizeof out, "{\"ok\":true,\"dev\":\"%s\"}", app_config_dev());
    return httpd_resp_sendstr(req, out);
}

// 把 Wi-Fi 断开原因翻成用户能照做的话。reason 见 wifi_err_reason_t。
static const char *reason_text(int reason) {
    switch (reason) {
    case 1:    // NO_AP_FOUND
    case 201:  // NO_AP_FOUND 在 4.4 之后的编号
        return "找不到这个 Wi-Fi，请检查名称";
    case 2:    // AUTH_EXPIRE，密码错最常见
    case 15:   // 四次握手超时，也是密码错
    case 200:
    case 204:  // HANDSHAKE_TIMEOUT
        return "密码不对，请重新输入";
    case 3:    // ASSOC_LEAVE
    case 8:    // ASSOC_FAIL
    case 203:
        return "设备拒绝接入，请重试";
    case 205:  // CONNECTION_FAIL
        return "信号太弱或路由器拒绝连接";
    default:
        return "连不上，请检查 Wi-Fi 名称和密码";
    }
}

// 轮询：done=1 后页面停止轮询。失败时必须给出可读原因，否则用户不知道改什么。
//
// "失败"的判定不能只看 APP_NET_FAILED：那只是"发连接命令就报错"。密码错这类
// 情况是断开事件反复触发、状态一直在 CONNECTING，永远等不到终态。所以这里
// 再看失败次数：重试 3 次还没上就认定这次配错了，让用户改。
static esp_err_t status_get(httpd_req_t *req) {
    app_net_state_t st = app_net_state();
    uint32_t fails = app_net_fail_count();

    bool ok = (st == APP_NET_CONNECTED);
    bool failed = (st == APP_NET_FAILED) || (!ok && fails >= 3);
    bool done = ok || failed;

    httpd_resp_set_type(req, "application/json");
    char out[192];
    if (failed) {
        // JSON 里直接放中文，页面拿去当提示用，不用在前端再映射一遍。
        snprintf(out, sizeof out, "{\"done\":true,\"ok\":false,\"error\":\"%s\",\"fails\":%u}",
                 reason_text(app_net_last_reason()), (unsigned)fails);
    } else {
        snprintf(out, sizeof out, "{\"done\":%s,\"ok\":%s,\"error\":\"\",\"fails\":%u}",
                 done ? "true" : "false", ok ? "true" : "false", (unsigned)fails);
    }
    return httpd_resp_sendstr(req, out);
}

// 当前配置回显（同网段调试用）。token 不回全文，只说有没有。
static esp_err_t info_get(httpd_req_t *req) {
    const app_config_t *c = app_config_get();
    httpd_resp_set_type(req, "application/json");
    char out[320];
    snprintf(out, sizeof out,
             "{\"dev\":\"%s\",\"url\":\"%s\",\"token\":%s,\"ssid\":%s}",
             c->dev, c->url,
             c->token[0] ? "true" : "false", c->ssid[0] ? "true" : "false");
    return httpd_resp_sendstr(req, out);
}

// ---- AP 事件 -------------------------------------------------------------

static void ap_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)data;
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        s_clients++;
        app_log(TAG, "phone joined ap, clients=%d", s_clients);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_clients > 0) s_clients--;
        app_log(TAG, "phone left ap, clients=%d", s_clients);
    }
}

static httpd_handle_t start_server(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    // 页面和接口都很小，够用即可；max_open_sockets 默认 7，改小省内存。
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        app_error_report(APP_E400_INIT, APP_SRC_PROV, "httpd_start failed");
        return NULL;
    }

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    httpd_uri_t stat = { .uri = "/status", .method = HTTP_GET, .handler = status_get };
    httpd_uri_t info = { .uri = "/info", .method = HTTP_GET, .handler = info_get };

    const httpd_uri_t *all[] = {&root, &save, &stat, &info};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        esp_err_t err = httpd_register_uri_handler(server, all[i]);
        if (err != ESP_OK) {
            app_error_report(APP_E400_INIT, APP_SRC_PROV, "register %s: %s",
                             all[i]->uri, esp_err_to_name(err));
        }
    }
    return server;
}

void app_prov_start(void) {
    if (s_active) return;

    const char *dev = app_config_dev();
    size_t dev_len = strlen(dev);
    size_t start = dev_len > 4 ? dev_len - 4 : 0;
    snprintf(s_ap_name, sizeof s_ap_name, AP_NAME_PREFIX "%s", dev + start);

    app_log(TAG, "starting ap name=%s", s_ap_name);

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            app_error_report(APP_E400_INIT, APP_SRC_PROV, "create ap netif failed");
            return;
        }
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED,
                                            ap_event_cb, NULL, NULL);
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED,
                                            ap_event_cb, NULL, NULL);
    }

    // 无密码热点：用户连上就能开网页，少一道门槛。配网阶段短暂开放，配完即关。
    wifi_config_t wc = {0};
    memcpy(wc.ap.ssid, s_ap_name, strlen(s_ap_name));
    wc.ap.ssid_len = (uint8_t)strlen(s_ap_name);
    wc.ap.channel = 6;
    wc.ap.max_connection = AP_MAX_CLIENT;
    wc.ap.authmode = WIFI_AUTH_OPEN;
    wc.ap.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        app_error_report(APP_E400_INIT, APP_SRC_PROV, "set APSTA: %s", esp_err_to_name(err));
        return;
    }
    err = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (err != ESP_OK) {
        app_error_report(APP_E400_INIT, APP_SRC_PROV, "set ap cfg: %s", esp_err_to_name(err));
        return;
    }

    s_server = start_server();
    if (!s_server) {
        // 热点照开，页面没了也要能恢复：报错后由 tick 重试。
        return;
    }

    s_active = true;
    s_clients = 0;
    app_log(TAG, "ap ready ssid=%s ip=192.168.4.1", s_ap_name);
}

void app_prov_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    if (s_active) {
        s_active = false;
        // 回 STA 单模式：AP 开着会持续占用射频和内存。
        esp_wifi_set_mode(WIFI_MODE_STA);
        app_log(TAG, "stopped");
    }
}

bool app_prov_active(void) { return s_active; }
bool app_prov_got_cfg(void) { return s_got_cfg; }
bool app_prov_client_connected(void) { return s_clients > 0; }
const char *app_prov_ap_name(void) { return s_ap_name[0] ? s_ap_name : "AIPassport"; }

void app_prov_tick(void) {
    if (!s_active) return;

    // 配好了并且手机离开了，就可以关热点。不立刻关：页面还在轮询 /status，
    // 立刻断会让用户看不到"连接成功"的结果。
    // 收到配置时手机必然连着（是它提交的表单），所以 s_clients 变 0 就是"它走了"。
    if (s_got_cfg && s_clients == 0 && app_net_state() == APP_NET_CONNECTED) {
        app_log(TAG, "provisioned and phone left, stopping ap");
        app_prov_stop();
        return;
    }

    // 兜底：配完很久还连着，也关掉。结果已经写进 NVS，用户下次开机就是配好的状态。
    if (s_got_cfg) {
        int64_t now = esp_timer_get_time();
        if (s_cfg_at_us && now - s_cfg_at_us > AP_KEEPALIVE_US) {
            app_log(TAG, "ap keepalive expired, stopping");
            app_prov_stop();
        }
    }
}