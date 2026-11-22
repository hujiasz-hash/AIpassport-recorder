#include "app_upload.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "app_error.h"
#include "app_net.h"
#include "app_store.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_tls_errors.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "upload"

// 每个请求 32 KB。大于 32 KB 时服务端单段落盘时间变长，掉电重传成本变高。
#define CHUNK_BYTES (32u * 1024)

// 空闲时的轮询周期。设计文档 7.2 定的 60 s：录音时每 60 s 开一次连接批量发完。
#define IDLE_PERIOD_MS (60 * 1000)

// 同一个会话连续冲突 3 次就放弃本轮，避免坏数据把设备卡死在重试循环里。
#define MAX_CONFLICTS 3

// 网络超时。走 FN Connect 中继时往返可能上百毫秒，留足余量。
#define HTTP_TIMEOUT_MS 20000

static TaskHandle_t s_task;
static SemaphoreHandle_t s_kick;
static volatile bool s_enabled = true;
static volatile app_upload_state_t s_state = APP_UP_IDLE;
static volatile uint32_t s_total_bytes;
static char s_summary[64];

static app_session_t s_list;   // 一次只处理最旧的一个会话，省内存
static uint8_t *s_chunk;       // 32 KB 发送缓冲

typedef struct {
    int http_code;      // HTTP 状态码，0 表示连接层失败
    int64_t received;   // 服务端已落盘字节，-1 表示响应里没有
    bool finalized;
    char status[24];    // 服务端返回的 status 字符串
    char message[40];   // 服务端返回的 error 字段
    esp_err_t err;      // 连接层错误码
} resp_t;

// 从 JSON 响应里抠字段。服务端 JSON 是紧凑格式且字段顺序固定，不做完整解析器：
// 只找键名再取后面的值，够用且不占内存。
static bool json_int(const char *js, const char *key, long long *out) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(js, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p == 'n') return false;   // null
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p) return false;
    *out = v;
    return true;
}

static void json_str(const char *js, const char *key, char *dst, size_t cap) {
    dst[0] = 0;
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(js, pat);
    if (!p) return;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p != '"') return;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < cap - 1) {
        dst[i++] = *p++;
    }
    dst[i] = 0;
}

static bool json_bool(const char *js, const char *key) {
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(js, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    return strncmp(p, "true", 4) == 0;
}

// 拼一机一 URL：<base>/v1/upload/<sid>。base 末尾的斜杠多余时去掉。
static void build_url(const char *base, const char *sid, char *out, size_t cap) {
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') n--;
    snprintf(out, cap, "%.*s/v1/upload/%s", (int)n, base, sid);
}

// 发一个 PUT。data 为 NULL 且 len==0 时是探针/零长请求。
static void http_put(const char *url, const char *token, int offset, int seq,
                     const uint8_t *data, size_t len, bool final, uint32_t total,
                     resp_t *r) {
    memset(r, 0, sizeof *r);
    r->received = -1;
    r->http_code = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_PUT,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
    };
    if (strncmp(url, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        r->err = ESP_ERR_NO_MEM;
        return;
    }

    char hdr[96];
    snprintf(hdr, sizeof hdr, "Bearer %s", token);
    esp_http_client_set_header(c, "Authorization", hdr);
    esp_http_client_set_header(c, "Content-Type", "application/octet-stream");
    snprintf(hdr, sizeof hdr, "%d", offset);
    esp_http_client_set_header(c, "X-Offset", hdr);
    snprintf(hdr, sizeof hdr, "%d", seq);
    esp_http_client_set_header(c, "X-Seq", hdr);
    if (final) {
        esp_http_client_set_header(c, "X-Final", "1");
        snprintf(hdr, sizeof hdr, "%u", (unsigned)total);
        esp_http_client_set_header(c, "X-Total-Len", hdr);
    }

    esp_err_t err = esp_http_client_open(c, (int)len);
    if (err != ESP_OK) {
        r->err = err;
        esp_http_client_cleanup(c);
        return;
    }

    if (len) {
        size_t sent = 0;
        while (sent < len) {
            int w = esp_http_client_write(c, (const char *)data + sent, (int)(len - sent));
            if (w <= 0) {
                r->err = ESP_FAIL;
                esp_http_client_close(c);
                esp_http_client_cleanup(c);
                return;
            }
            sent += (size_t)w;
        }
    }

    int64_t content_len = esp_http_client_fetch_headers(c);
    (void)content_len;
    r->http_code = esp_http_client_get_status_code(c);

    char body[512];
    int got = esp_http_client_read(c, body, sizeof body - 1);
    if (got < 0) got = 0;
    body[got] = 0;

    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    long long v;
    if (json_int(body, "received", &v)) r->received = v;
    r->finalized = json_bool(body, "finalized");
    json_str(body, "status", r->status, sizeof r->status);
    json_str(body, "error", r->message, sizeof r->message);

    if (r->http_code == 0) {
        // 连接层失败（DNS/握手/超时）。esp_http_client 把原因编码在返回码里，
        // 这里统一记成 E202，靠 detail 区分。
        r->err = ESP_FAIL;
    }
}

// 把连接层错误翻译成错误码。esp_tls 的错误码都在 0x8000 段：
//   0x8001 域名解析失败；0x8002/0x8004/0x8006 连接建不起来或超时；
//   0x8010 以上是 mbedTLS 具体错误，基本都是证书/握手问题。
static void report_net_failure(const resp_t *r, const char *sid, int offset) {
    esp_err_t e = r->err;
    const char *name = esp_err_to_name(e);

    if (e == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
        app_error_report(APP_E200_DNS_FAIL, APP_SRC_UPLOAD, "%s off=%d %s", sid, offset, name);
        return;
    }
    if (e >= ESP_ERR_ESP_TLS_BASE + 0x10 && e <= ESP_ERR_ESP_TLS_BASE + 0x1F) {
        app_error_report(APP_E201_TLS_FAIL, APP_SRC_UPLOAD, "%s off=%d %s", sid, offset, name);
        return;
    }
    if (e == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST ||
        e == ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET ||
        e == ESP_ERR_HTTP_CONNECT) {
        // 域名解析成功但连不上：端口不通、中继拒绝或对端没起服务。
        app_error_report(APP_E202_TIMEOUT, APP_SRC_UPLOAD, "%s off=%d connect failed %s",
                         sid, offset, name);
        return;
    }
    app_error_report(APP_E202_TIMEOUT, APP_SRC_UPLOAD, "%s off=%d %s", sid, offset, name);
}

static void set_summary(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_summary, sizeof s_summary, fmt, ap);
    va_end(ap);
}

// 传一个会话。返回 true 表示这个会话已完成（finalized 或没有数据可传）。
static bool upload_one(const app_session_t *s) {
    char sid[32];
    snprintf(sid, sizeof sid, "%s-%010lu-%08x", app_config_dev(), (unsigned long)s->start_ts,
             (unsigned)s->session);

    const app_config_t *cfg = app_config_get();
    char url[160];
    build_url(cfg->url, sid, url, sizeof url);

    app_log(TAG, "session=%08x begin bytes=%u sealed=%d", (unsigned)s->session,
            (unsigned)s->bytes, (int)s->sealed);

    resp_t r;
    // 先用零长请求问服务端收到哪了。服务端对零长 offset=0 只回报状态，不建文件。
    http_put(url, cfg->token, 0, 0, NULL, 0, false, 0, &r);
    if (r.err != ESP_OK) {
        report_net_failure(&r, sid, 0);
        return false;
    }
    if (r.http_code == 401 || r.http_code == 403) {
        app_error_report(APP_E301_TOKEN_INVALID, APP_SRC_UPLOAD, "%s http=%d %s",
                         sid, r.http_code, r.message);
        return false;
    }
    if (r.http_code >= 400) {
        app_error_report(APP_E300_SERVER_REJECT, APP_SRC_UPLOAD, "%s probe http=%d %s %s",
                         sid, r.http_code, r.status, r.message);
        return false;
    }
    if (r.finalized) {
        app_log(TAG, "session=%08x already finalized, mark done", (unsigned)s->session);
        app_store_mark_done(s->session);
        return true;
    }

    uint32_t off = (r.received > 0) ? (uint32_t)r.received : 0;
    if (off > s->bytes) {
        // 服务端比本地长：本地缓存被擦过或会话号撞了。这种情况没法自动修，报出来。
        app_error_report(APP_E300_SERVER_REJECT, APP_SRC_UPLOAD,
                         "%s server received=%u > local=%u, skip", sid, (unsigned)off,
                         (unsigned)s->bytes);
        return false;
    }
    app_log(TAG, "session=%08x resume at %u/%u", (unsigned)s->session, (unsigned)off,
            (unsigned)s->bytes);

    int conflicts = 0;
    uint32_t seq = 0;
    while (off < s->bytes) {
        uint32_t n = s->bytes - off;
        if (n > CHUNK_BYTES) n = CHUNK_BYTES;

        uint32_t got = 0;
        int rc = app_store_read(s, off, s_chunk, n, &got);
        if (rc < 0 || got == 0) {
            app_error_report(APP_E501_FLASH, APP_SRC_UPLOAD, "%s read cache at %u failed",
                             sid, (unsigned)off);
            return false;
        }

        bool last = (off + got >= s->bytes);
        bool final = last && s->sealed;

        http_put(url, cfg->token, (int)off, (int)seq++, s_chunk, got, final,
                 final ? s->bytes : 0, &r);
        if (r.err != ESP_OK) {
            report_net_failure(&r, sid, (int)off);
            return false;
        }
        if (r.http_code == 401 || r.http_code == 403) {
            app_error_report(APP_E301_TOKEN_INVALID, APP_SRC_UPLOAD, "%s off=%u http=%d %s",
                             sid, (unsigned)off, r.http_code, r.message);
            return false;
        }
        if (r.http_code == 409) {
            // 409 是重同步指令，不是故障：把读指针挪到服务端说的地方。
            conflicts++;
            if (r.received >= 0 && (uint32_t)r.received <= s->bytes) {
                app_log(TAG, "session=%08x conflict #%d at off=%u -> %lld (%s)",
                        (unsigned)s->session, conflicts, (unsigned)off,
                        (long long)r.received, r.status);
                off = (uint32_t)r.received;
            } else {
                app_error_report(APP_E300_SERVER_REJECT, APP_SRC_UPLOAD,
                                 "%s 409 without usable received (%lld) status=%s",
                                 sid, (long long)r.received, r.status);
                return false;
            }
            if (conflicts >= MAX_CONFLICTS) {
                // 本地缓存内容和服务端已存内容对不上，继续重试只会空转。
                app_error_report(APP_E300_SERVER_REJECT, APP_SRC_UPLOAD,
                                 "%s %d conflicts, abandon this session (server has %u of %u)",
                                 sid, conflicts, (unsigned)off, (unsigned)s->bytes);
                return false;
            }
            continue;
        }
        if (r.http_code == 413) {
            app_error_report(APP_E300_SERVER_REJECT, APP_SRC_UPLOAD,
                             "%s 413 session too large %s", sid, r.message);
            return false;
        }
        if (r.http_code >= 400) {
            app_error_report(APP_E300_SERVER_REJECT, APP_SRC_UPLOAD, "%s off=%u http=%d %s %s",
                             sid, (unsigned)off, r.http_code, r.status, r.message);
            return false;
        }
        if (r.received < 0) {
            app_error_report(APP_E300_SERVER_REJECT, APP_SRC_UPLOAD,
                             "%s off=%u http=%d no received field", sid, (unsigned)off,
                             r.http_code);
            return false;
        }

        uint32_t prev = off;
        off = (uint32_t)r.received;
        s_total_bytes += (off > prev ? off - prev : 0);

        app_log(TAG, "session=%08x off=%u->%u/%u status=%s", (unsigned)s->session,
                (unsigned)prev, (unsigned)off, (unsigned)s->bytes, r.status);

        if (r.finalized) {
            app_log(TAG, "session=%08x finalized by server", (unsigned)s->session);
            app_store_mark_done(s->session);
            return true;
        }
    }

    if (s->sealed && off >= s->bytes) {
        // 数据发光了还需要一次显式 final：服务端在这一步改名并写 finalized。
        http_put(url, cfg->token, (int)off, (int)seq, NULL, 0, true, s->bytes, &r);
        if (r.err != ESP_OK) {
            report_net_failure(&r, sid, (int)off);
            return false;
        }
        if (r.http_code == 401 || r.http_code == 403) {
            app_error_report(APP_E301_TOKEN_INVALID, APP_SRC_UPLOAD, "%s final http=%d",
                             sid, r.http_code);
            return false;
        }
        if (r.http_code >= 400) {
            app_error_report(APP_E300_SERVER_REJECT, APP_SRC_UPLOAD, "%s final http=%d %s %s",
                             sid, r.http_code, r.status, r.message);
            return false;
        }
        if (r.finalized) {
            app_log(TAG, "session=%08x finalized total=%u", (unsigned)s->session,
                    (unsigned)s->bytes);
            app_store_mark_done(s->session);
            return true;
        }
        app_error_report(APP_E300_SERVER_REJECT, APP_SRC_UPLOAD,
                         "%s final returned 2xx but finalized=false %s", sid, r.status);
        return false;
    }

    // 走到这里说明数据全传完了但会话还没封闭（正在录），下一轮再收尾。
    return false;
}

// 跑一轮：从最旧的会话开始，成功就继续下一个，出错就停下来等下一轮。
static void run_once(void) {
    if (!s_enabled) return;
    const app_config_t *cfg = app_config_get();
    if (!cfg->ssid[0] || !cfg->url[0]) {
        s_state = APP_UP_IDLE;
        return;
    }
    if (!cfg->token[0]) {
        // 报错一次就够，然后停手。app_error_report 对同一错误码的去重窗口只有 5 秒，
        // 而上传任务每 60 秒跑一轮，所以不这样做的话它会每分钟报一次 E301，
        // 整份日志能被一条"没填 token"刷满，真问题反而看不见。
        // 等用户在配网页填了 token（app_config_set_upload 会把 s_enabled 打开）再恢复。
        app_error_report(APP_E301_TOKEN_INVALID, APP_SRC_UPLOAD,
                         "no token configured, upload paused until cfg updated");
        app_log(TAG, "no token, pause upload");
        s_state = APP_UP_IDLE;
        s_enabled = false;
        return;
    }
    if (!app_net_wait_time(20000)) {
        s_state = APP_UP_WAITING;
        return;
    }

    int n = app_store_list(&s_list, 1);
    if (n == 0) {
        s_state = APP_UP_DONE;
        set_summary("无待传数据");
        // 已经没有待传的了，把已完成会话占的扇区还回去。缓存只有 3.6 MB，
        // 不回收的话录满一次就永久 E500。
        app_store_reclaim();
        return;
    }

    s_state = APP_UP_RUNNING;
    app_net_set_power_save(false);   // 上传期间关省电，避免 DTIM 拖慢往返
    bool ok = upload_one(&s_list);
    app_net_set_power_save(true);

    if (ok) {
        s_state = APP_UP_DONE;
        set_summary("全部上传完成");
        // 传完一个就回收一次：长时间录音时空间才跟得上。
        app_store_reclaim();
    } else {
        s_state = APP_UP_WAITING;
        set_summary("等待上传 %u KB", (unsigned)(s_list.bytes / 1024));
    }
    app_log(TAG, "round done ok=%d state=%d summary=%s", (int)ok, (int)s_state, s_summary);
}

static void upload_task(void *arg) {
    (void)arg;
    for (;;) {
        // 有 kick 就立刻跑，没有就等一个周期。
        if (xSemaphoreTake(s_kick, pdMS_TO_TICKS(IDLE_PERIOD_MS)) == pdTRUE) {
            // 连按几下只跑一轮：把队列里堆积的 token 清掉。
            while (xSemaphoreTake(s_kick, 0) == pdTRUE) {}
        }
        run_once();
    }
}

void app_upload_init(void) {
    if (s_task) return;
    s_kick = xSemaphoreCreateBinary();
    s_chunk = malloc(CHUNK_BYTES);
    if (!s_chunk) {
        app_error_report(APP_E402_NO_SPACE, APP_SRC_UPLOAD, "chunk buffer alloc failed");
        return;
    }
    // 优先级 3，低于录音任务（5）：编码是硬实时，上传可以慢。
    if (xTaskCreate(upload_task, "upload", 8192, NULL, 3, &s_task) != pdPASS) {
        s_task = NULL;
        app_error_report(APP_E402_NO_SPACE, APP_SRC_UPLOAD, "task create failed");
        return;
    }
    app_log(TAG, "task started chunk=%u", (unsigned)CHUNK_BYTES);
}

void app_upload_kick(void) {
    if (s_kick) xSemaphoreGive(s_kick);
}

void app_upload_set_enabled(bool on) {
    s_enabled = on;
    if (on) app_upload_kick();
}

app_upload_state_t app_upload_state(void) {
    return s_state;
}

const char *app_upload_summary(void) {
    return s_summary;
}

uint32_t app_upload_total_bytes(void) {
    return s_total_bytes;
}