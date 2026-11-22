#include "app_net.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "app_config.h"
#include "app_error.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"

#define BIT_IP   BIT0
#define BIT_FAIL BIT1

// 断线后的重连退避：1、2、4、8、16、30 秒封顶。失败次数只影响日志密度，不影响重连本身。
#define RETRY_MIN_MS 1000
#define RETRY_MAX_MS 30000

static EventGroupHandle_t s_eg;
static esp_netif_t *s_netif;
static volatile app_net_state_t s_state = APP_NET_OFF;
static volatile bool s_time_valid;
static volatile uint32_t s_fail_count;
static volatile int s_last_reason;
static uint8_t s_retry_delay_ms_div;   // 指数退避的档位
static esp_timer_handle_t s_retry_timer;
static volatile bool s_manual_connect;  // 主动切换凭据期间，忽略这次断开

static const char *TAG = "net";

static void got_ip_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id; (void)data;
    xEventGroupSetBits(s_eg, BIT_IP);
    s_state = APP_NET_CONNECTED;
    s_fail_count = 0;
    s_retry_delay_ms_div = 0;
    s_last_reason = 0;
}

static void retry_cb(void *arg) {
    (void)arg;
    esp_wifi_connect();
}

// 重连不能在 Wi-Fi 事件回调里 vTaskDelay：那个任务同时负责派发 GOT_IP 等其他事件，
// 阻塞它会把整条事件链卡住。改成一次性 esp_timer，回调里只发起连接。
static void schedule_retry(void) {
    uint32_t delay_ms = RETRY_MIN_MS;
    for (uint8_t i = 0; i < s_retry_delay_ms_div && delay_ms < RETRY_MAX_MS; i++) delay_ms *= 2;
    if (delay_ms > RETRY_MAX_MS) delay_ms = RETRY_MAX_MS;
    if (s_retry_delay_ms_div < 5) s_retry_delay_ms_div++;

    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base != WIFI_EVENT || id != WIFI_EVENT_STA_DISCONNECTED) return;

    wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
    xEventGroupClearBits(s_eg, BIT_IP);
    s_state = APP_NET_CONNECTING;

    if (s_manual_connect) {
        // 主动换凭据触发的断开，调用方会自己连，不要插一脚。
        s_manual_connect = false;
        return;
    }

    s_fail_count++;
    s_last_reason = ev->reason;

    // reason 是 wifi_err_reason_t。常见值：1/201 找不到 AP，2/200 密码错，15 四次握手超时。
    bool fatal = (ev->reason == 2 || ev->reason == 15 || ev->reason == 200 ||
                  ev->reason == 201 || ev->reason == 1);
    if (s_fail_count == 1 || (s_fail_count % 10) == 0 || fatal) {
        app_error_report(APP_E101_WIFI_FAIL, APP_SRC_NET, "reason=%d rssi=%d fail=%u",
                         ev->reason, ev->rssi, (unsigned)s_fail_count);
    }
    schedule_retry();
}

void app_net_init(void) {
    if (s_eg) return;

    s_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    // 事件循环可能已由别处创建，忽略重复错误。
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        app_error_report(APP_E101_WIFI_FAIL, APP_SRC_NET, "event loop: %s", esp_err_to_name(err));
        return;
    }

    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) {
        app_error_report(APP_E101_WIFI_FAIL, APP_SRC_NET, "create sta netif failed");
        return;
    }

    const esp_timer_create_args_t targs = {
        .callback = retry_cb,
        .name = "wifi_retry",
    };
    esp_timer_create(&targs, &s_retry_timer);

    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                        wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        got_ip_handler, NULL, NULL);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        app_error_report(APP_E101_WIFI_FAIL, APP_SRC_NET, "esp_wifi_init: %s", esp_err_to_name(err));
        return;
    }
    // 别让 Wi-Fi 驱动自己往 NVS 写凭据：我们的配置只有一份，由 app_config 管。
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    // SNTP 用三组服务器轮询，国内网络访问国内服务器更稳。
    // 宏第二个参数是初始化列表，整体要再加一层花括号。
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        3, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "cn.pool.ntp.org", "time.google.com"));
    err = esp_netif_sntp_init(&sntp);
    if (err != ESP_OK) {
        app_error_report(APP_E203_NO_TIME, APP_SRC_NET, "sntp init: %s", esp_err_to_name(err));
    }

    app_log(TAG, "stack ready dev=%s ssid=%s", app_config_dev(),
            app_config_get()->ssid[0] ? "set" : "none");
}

void app_net_apply_credentials(void) {
    const app_config_t *c = app_config_get();
    if (!s_eg || !c->ssid[0]) {
        s_state = APP_NET_OFF;
        return;
    }

    wifi_config_t wc = {0};
    size_t ssid_len = strlen(c->ssid);
    size_t pass_len = strlen(c->pass);
    if (ssid_len > sizeof wc.sta.ssid) ssid_len = sizeof wc.sta.ssid;
    if (pass_len > sizeof wc.sta.password) pass_len = sizeof wc.sta.password;
    memcpy(wc.sta.ssid, c->ssid, ssid_len);
    memcpy(wc.sta.password, c->pass, pass_len);
    // 开放网络密码为空，必须放开 authmode 阈值，否则驱动拒绝连接。
    wc.sta.threshold.authmode = pass_len ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;

    s_retry_delay_ms_div = 0;
    s_fail_count = 0;
    s_state = APP_NET_CONNECTING;
    xEventGroupClearBits(s_eg, BIT_IP);
    esp_timer_stop(s_retry_timer);

    // 先置标记再断开：随后到达的 STA_DISCONNECTED 事件会被这个标记吞掉，
    // 否则重连定时器会和下面的主动连接撞在一起。
    // 只在真的连着时断开：没连的时候 esp_wifi_disconnect 不产生事件，
    // 置了标记就会被下一次真实断开白白吃掉，重连退避从此失效。
    if (s_state == APP_NET_CONNECTED || (xEventGroupGetBits(s_eg) & BIT_IP)) {
        s_manual_connect = true;
        esp_wifi_disconnect();
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        s_manual_connect = false;
        app_error_report(APP_E101_WIFI_FAIL, APP_SRC_NET, "set_config: %s", esp_err_to_name(err));
        s_state = APP_NET_FAILED;
        return;
    }
    err = esp_wifi_connect();
    s_manual_connect = false;
    if (err != ESP_OK) {
        app_error_report(APP_E101_WIFI_FAIL, APP_SRC_NET, "connect: %s", esp_err_to_name(err));
        s_state = APP_NET_FAILED;
        return;
    }
    app_log(TAG, "connecting ssid_len=%u", (unsigned)ssid_len);
}

bool app_net_wait_ip(int timeout_ms) {
    if (!s_eg) return false;
    if (xEventGroupGetBits(s_eg) & BIT_IP) return true;
    TickType_t ticks = timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : 0;
    EventBits_t bits = xEventGroupWaitBits(s_eg, BIT_IP, pdFALSE, pdTRUE, ticks);
    return (bits & BIT_IP) != 0;
}

bool app_net_wait_time(int timeout_ms) {
    if (s_time_valid) return true;
    if (!app_net_wait_ip(timeout_ms)) return false;

    esp_err_t err = esp_netif_sntp_sync_wait(timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : 0);
    if (err == ESP_OK) {
        s_time_valid = true;
        time_t now = 0;
        time(&now);
        char iso[24];
        app_net_now_iso(iso, sizeof iso);
        app_log(TAG, "time synced %s (epoch=%ld)", iso, (long)now);
        return true;
    }
    app_error_report(APP_E203_NO_TIME, APP_SRC_NET, "sntp wait: %s", esp_err_to_name(err));
    return false;
}

app_net_state_t app_net_state(void) {
    return s_state;
}

int app_net_last_reason(void) {
    return s_last_reason;
}

uint32_t app_net_fail_count(void) {
    return s_fail_count;
}

bool app_net_time_valid(void) {
    return app_net_now() != 0;
}

time_t app_net_now(void) {
    // 直接看系统时钟，不依赖 s_time_valid：SNTP 同步成功后会自己写入系统时间，
    // 而这个标志只有 app_net_wait_time 会置真，它此前只在少数路径被调用，
    // 结果录音前一直认为"时间未同步"，连上网也录不了（实测 E203）。
    // 少一个需要手动跟新的状态，就少一处不同步的地方。
    time_t now = 0;
    time(&now);
    // 小于 2020 年的时间说明 SNTP 还没同步成功，当成无效。
    return now > 1577836800 ? now : 0;
}

void app_net_now_iso(char *buf, size_t cap) {
    if (!buf || cap < 20) return;
    time_t now = app_net_now();
    if (!now) {
        snprintf(buf, cap, "1970-01-01T00:00:00");
        return;
    }
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, cap, "%Y-%m-%dT%H:%M:%S", &tm);
}

void app_net_set_power_save(bool on) {
    esp_wifi_set_ps(on ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}