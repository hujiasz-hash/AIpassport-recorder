#include "app_ble.h"
#include "app_store.h"
#include "app_record.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include <stdatomic.h>
#include <string.h>
#include <sys/time.h>

#define TAG "passport_ble"
#define WINDOW 2048u
#define NO_CONN BLE_HS_CONN_HANDLE_NONE
#define PASSPORT_NAME "AI Passport"

#define PASSPORT_UUID(n) BLE_UUID128_INIT(0xdd,0x50,0xd6,0x53,0xef,0x1d,0x71,0xa5,0xb9,0x45,0xa4,0x68,n,0x01,0x16,0x94)
static const ble_uuid128_t service = PASSPORT_UUID(1);
static const ble_uuid128_t tx_uuid = PASSPORT_UUID(2);
static const ble_uuid128_t rx_uuid = PASSPORT_UUID(3);
static uint16_t tx_handle;
static atomic_uint conn = NO_CONN;
static atomic_bool ready;
static atomic_bool subscribed;
static atomic_uint epoch;
static atomic_uint passkey = UINT32_MAX;
static atomic_uint pair_until;// boot-relative seconds
static atomic_bool adv_kick;  // 立刻重试广播，跳过节流
static uint8_t addr_type, device[6];
static QueueHandle_t inbox;
static app_session_t sessions[96];
static uint8_t window[WINDOW];

typedef struct { unsigned gen; uint32_t value; uint8_t kind; } control_t;
void ble_store_config_init(void);

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) p[i] = v >> (i * 8);
}

bool app_ble_ready(void) { return atomic_load(&ready); }
uint32_t app_ble_passkey(void) {
    if (esp_timer_get_time() / 1000000ULL >= atomic_load(&pair_until)) return UINT32_MAX;
    return atomic_load(&passkey);
}
void app_ble_advertise_for(uint32_t seconds) {
    // 广播一直开着，这个接口只剩「马上重试一次」的意义，用于把快速广播期重新拉起来。
    (void)seconds;
    atomic_store(&adv_kick, true);
}
void app_ble_allow_pairing(void) {
    atomic_store(&passkey, UINT32_MAX);
    atomic_store(&pair_until, esp_timer_get_time() / 1000000ULL + 120);
    // A long press explicitly authorizes replacing a stale bond. Boot keeps it.
    ble_addr_t peer;
    int count = 0;
    int rc = ble_store_util_bonded_peers(&peer, &count, 1);
    if (rc) ESP_LOGW(TAG, "read bonded peer failed: %d", rc);
    for (int i = 0; i < count; i++) {
        rc = ble_store_util_delete_peer(&peer);
        ESP_LOGI(TAG, "deleted old bond for re-pairing: %d", rc);
    }
    uint16_t handle = atomic_load(&conn);
    if (handle != NO_CONN) ble_gap_terminate(handle, BLE_ERR_REM_USER_CONN_TERM);
}

static bool secure(uint16_t handle) {
    struct ble_gap_conn_desc desc;
    return ble_gap_conn_find(handle, &desc) == 0 && desc.sec_state.encrypted && desc.sec_state.authenticated && desc.sec_state.bonded;
}

static int gatt(uint16_t handle, uint16_t attr, struct ble_gatt_access_ctxt *ctx, void *arg) {
    (void)attr;
    (void)arg;
    if (ctx->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    if (!secure(handle) || !atomic_load(&subscribed)) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    uint8_t frame[5];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctx->om, frame, sizeof frame, &len) != 0 || len != sizeof frame)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint32_t n = get32(frame + 1);
    if (frame[0] == 'T') {
        if (n < 1735689600u) return BLE_ATT_ERR_UNLIKELY;
        struct timeval tv = { .tv_sec = n };
        settimeofday(&tv, NULL);
        atomic_store(&ready, true);
        ESP_LOGI(TAG, "BLE receiver ready, clock set");
        return 0;
    }
    if (!atomic_load(&ready) || (frame[0] != 'A' && frame[0] != 'F')) return BLE_ATT_ERR_UNLIKELY;
    control_t ctl = { .gen = atomic_load(&epoch), .value = n, .kind = frame[0] };
    return xQueueSend(inbox, &ctl, 0) == pdTRUE ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def services[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &service.u,
      .characteristics = (struct ble_gatt_chr_def[]) {
          { .uuid = &tx_uuid.u, .access_cb = gatt, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &tx_handle },
          { .uuid = &rx_uuid.u, .access_cb = gatt, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_AUTHEN },
          { 0 }
      } },
    { 0 }
};

static int gap(struct ble_gap_event *ev, void *arg) {
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        // 连接事件必须留日志：排查「连不上」时，设备侧有没有收到 CONNECT 是分水岭。
        ESP_LOGI(TAG, "connect status=%d handle=%d", ev->connect.status, ev->connect.conn_handle);
        if (ev->connect.status == 0) {
            atomic_store(&conn, ev->connect.conn_handle);
            // The Mac initiates both connection and pairing. Do not send a security request here.
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        // reason 是关键：19(REM_USER_CONN_TERM) 是我们自己断的，13/8 是配对或超时问题。
        ESP_LOGW(TAG, "disconnect reason=%d handle=%d", ev->disconnect.reason, ev->disconnect.conn.conn_handle);
        atomic_store(&ready, false);
        atomic_store(&subscribed, false);
        atomic_store(&conn, NO_CONN);
        // Keep the code visible if the central disconnects while pairing is pending.
        atomic_fetch_add(&epoch, 1);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        // The sender task restarts advertising after timeout or preemption.
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == tx_handle) {
            atomic_store(&subscribed, ev->subscribe.cur_notify != 0);
            if (!ev->subscribe.cur_notify) {
                atomic_store(&ready, false);
                atomic_fetch_add(&epoch, 1);
            }
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change status=%d", ev->enc_change.status);
        if (ev->enc_change.status == 0) atomic_store(&passkey, UINT32_MAX);
        else ble_gap_terminate(ev->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        int count = -1;
        int count_rc = ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &count);
        long long left = (long long)atomic_load(&pair_until) - (long long)(esp_timer_get_time() / 1000000ULL);
        ESP_LOGI(TAG, "passkey action=%d bonds=%d count_rc=%d window_left=%lld",
                 ev->passkey.params.action, count, count_rc, left);
        if (ev->passkey.params.action != BLE_SM_IOACT_DISP || count_rc || count != 0 || left <= 0) {
            ble_gap_terminate(ev->passkey.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            break;
        }
        struct ble_sm_io io = { .action = BLE_SM_IOACT_DISP, .passkey = esp_random() % 1000000u };
        atomic_store(&passkey, io.passkey);
        int rc = ble_sm_inject_io(ev->passkey.conn_handle, &io);
        if (rc) {
            ESP_LOGW(TAG, "inject passkey failed: %d", rc);
            ble_gap_terminate(ev->passkey.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        if (esp_timer_get_time() / 1000000ULL >= atomic_load(&pair_until)) {
            ESP_LOGW(TAG, "repeat pairing ignored outside pairing window");
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc);
        if (!rc) rc = ble_store_util_delete_peer(&desc.peer_id_addr);
        ESP_LOGI(TAG, "repeat pairing: delete old bond rc=%d", rc);
        return rc ? BLE_GAP_REPEAT_PAIRING_IGNORE : BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        break;
    }
    return 0;
}

static void ensure_advertising(void) {
    static int64_t next_attempt, fast_until;
    if (!ble_hs_synced() || atomic_load(&conn) != NO_CONN) return;
    // 一直广播，重连才能发生。配对窗口只管「允不允许新配对」，不管「广不广播」：
    // 之前把广播也绑在窗口上，窗口一过设备彻底静默，已绑定的 Mac 再也连不上。
    if (ble_gap_adv_active()) return;
    int64_t now = esp_timer_get_time();
    if (!atomic_exchange(&adv_kick, false) && now < next_attempt) return;
    next_attempt = now + 300000;

    int rc = ble_hs_util_ensure_addr(0);
    if (rc) {
        ESP_LOGW(TAG, "ensure BLE address failed: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &addr_type);
    if (rc) {
        ESP_LOGW(TAG, "infer BLE address failed: %d", rc);
        return;
    }

    // Flags + one 128-bit UUID use 21 bytes in the primary packet. The complete
    // name uses 13 bytes in the scan response; each legacy packet is limited to 31.
    struct ble_hs_adv_fields fields = { .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .uuids128 = &service, .num_uuids128 = 1, .uuids128_is_complete = 1 };
    // 实验：在扫描响应里加 Appearance（4 字节），名字 13 字节，合计仍在 31 字节内。
    struct ble_hs_adv_fields rsp = { .name = (const uint8_t *)PASSPORT_NAME,
        .name_len = sizeof PASSPORT_NAME - 1, .name_is_complete = 1,
        .appearance = 0x0200, .appearance_is_present = 1 };
    rc = ble_gap_adv_set_fields(&fields);
    if (rc) {
        ESP_LOGW(TAG, "set advertising fields failed: %d", rc);
        return;
    }
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc) {
        ESP_LOGW(TAG, "set scan response failed: %d", rc);
        return;
    }

    // Apple QA1931: start at exactly 20 ms for at least 30 s, then use 152.5 ms.
    bool fast = !fast_until || now < fast_until;
    uint16_t interval = fast ? 32 : 244; // 0.625 ms units
    int duration = BLE_HS_FOREVER;
    if (fast) duration = fast_until ? (int)((fast_until - now + 999) / 1000) : 30000;
    struct ble_gap_adv_params params = { .conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = interval, .itvl_max = interval };
    rc = ble_gap_adv_start(addr_type, NULL, duration, &params, gap, NULL);
    if (rc) {
        ESP_LOGW(TAG, "start advertising failed: %d", rc);
        return;
    }
    if (fast && !fast_until) fast_until = esp_timer_get_time() + 30000000;
    ESP_LOGI(TAG, "advertising %s at %s ms (service UUID in primary packet)",
             PASSPORT_NAME, fast ? "20" : "152.5");
}

static void sync(void) {
    ESP_LOGI(TAG, "BLE host synced");
}

static void host(void *unused) {
    (void)unused;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static bool send(unsigned gen, const uint8_t *data, uint16_t len) {
    for (int tries = 0; tries < 100; tries++) {
        uint16_t h = atomic_load(&conn);
        if (!app_ble_ready() || gen != atomic_load(&epoch) || h == NO_CONN) return false;
        struct os_mbuf *msg = ble_hs_mbuf_from_flat(data, len);
        if (msg && ble_gatts_notify_custom(h, tx_handle, msg) == 0) return true;
        vTaskDelay(pdMS_TO_TICKS(8));
    }
    return false;
}

static bool ack(unsigned gen, uint8_t kind, uint32_t *value) {
    int64_t until = esp_timer_get_time() + 2500000;
    while (app_ble_ready() && gen == atomic_load(&epoch) && esp_timer_get_time() < until) {
        control_t ctl;
        if (xQueueReceive(inbox, &ctl, pdMS_TO_TICKS(100)) && ctl.gen == gen && ctl.kind == kind) {
            *value = ctl.value;
            return true;
        }
    }
    return false;
}

static bool upload(unsigned gen, const app_session_t *s) {
    uint8_t frame[246] = {'S'};
    memcpy(frame + 1, device, sizeof device);
    frame[7] = 1;
    put32(frame + 9, s->session);
    put32(frame + 13, s->start_ts);
    uint32_t off;
    if (!send(gen, frame, 17) || !ack(gen, 'A', &off) || off > s->bytes) return false;
    while (off < s->bytes) {
        uint32_t start = off;
        uint32_t length = s->bytes - off < WINDOW ? s->bytes - off : WINDOW;
        uint32_t got = 0;
        if (app_store_read(s, off, window, length, &got) || got != length) return false;
        uint32_t end = off + length;
        while (off < end) {
            uint16_t h = atomic_load(&conn);
            if (h == NO_CONN || gen != atomic_load(&epoch)) return false;
            uint16_t mtu = ble_att_mtu(h);
            uint32_t max = mtu > 8 ? mtu - 8 : 15;
            if (max > sizeof frame - 5) max = sizeof frame - 5;
            uint32_t limit = end - off < max ? end - off : max;
            frame[0] = 'D';
            put32(frame + 1, off);
            memcpy(frame + 5, window + off - start, limit);
            if (!send(gen, frame, limit + 5)) return false;
            off += limit;
            vTaskDelay(pdMS_TO_TICKS(3));
        }
        frame[0] = 'W';
        put32(frame + 1, off);
        uint32_t committed;
        if (!send(gen, frame, 5) || !ack(gen, 'A', &committed) || committed != off) return false;
    }
    if (s->sealed) {
        frame[0] = 'E';
        put32(frame + 1, s->bytes);
        put32(frame + 5, s->duration_ms);
        frame[9] = s->end_reason;
        uint32_t final;
        if (!send(gen, frame, 10) || !ack(gen, 'F', &final) || final != off) return false;
        app_store_mark_done(s->session);
        ESP_LOGI(TAG, "stored on Mac session=%08lx bytes=%lu", (unsigned long)s->session, (unsigned long)off);
    }
    return true;
}

static SemaphoreHandle_t transfer;
static atomic_bool reclaim_pending;
static void sender(void *unused) {
    (void)unused;
    unsigned seen_gen = UINT32_MAX;
    uint32_t seen_session = 0, seen_bytes = 0;
    for (;;) {
        ensure_advertising();
        if (app_ble_ready() && xSemaphoreTake(transfer, pdMS_TO_TICKS(50))) {
            unsigned gen = atomic_load(&epoch);
            int n = app_store_list(sessions, sizeof sessions / sizeof sessions[0]);
            for (int i = 0; i < n; i++) {
                const app_session_t *s = &sessions[i];
                if (!s->ble_eligible) continue;
                if (gen == seen_gen && s->session == seen_session && s->bytes == seen_bytes && !s->sealed) continue;
                if (!upload(gen, s)) {
                    uint16_t h = atomic_load(&conn);
                    if (app_ble_ready() && gen == atomic_load(&epoch) && h != NO_CONN)
                        ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
                    break;
                }
                seen_gen = gen;
                seen_session = s->session;
                seen_bytes = s->bytes;
                if (s->sealed) atomic_store(&reclaim_pending, true);
            }
            xSemaphoreGive(transfer);
        }
        vTaskDelay(pdMS_TO_TICKS(450));
    }
}

void app_ble_reclaim_if_idle(void) {
    if (transfer && atomic_load(&reclaim_pending) && !app_record_active() && xSemaphoreTake(transfer, 0)) {
        // Do not compact pending/legacy sessions. Reclaim only when every byte is confirmed.
        if (app_store_pending_bytes() == 0) {
            app_store_reclaim();
            atomic_store(&reclaim_pending, false);
        }
        xSemaphoreGive(transfer);
    }
}

void app_ble_init(void) {
    inbox = xQueueCreate(8, sizeof(control_t));
    transfer = xSemaphoreCreateMutex();
    if (!inbox || !transfer || esp_read_mac(device, ESP_MAC_BT) != ESP_OK || nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed; local recording still available");
        return;
    }
    atomic_store(&pair_until, esp_timer_get_time() / 1000000ULL + 120);
    ble_hs_cfg.sync_cb = sync;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gap_device_name_set(PASSPORT_NAME);
    ble_store_config_init();
    if (ble_gatts_count_cfg(services) || ble_gatts_add_svcs(services)) {
        ESP_LOGE(TAG, "GATT registration failed");
        return;
    }
    if (xTaskCreate(sender, "ble-send", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "BLE upload task allocation failed");
        return;
    }
    nimble_port_freertos_init(host);
}
