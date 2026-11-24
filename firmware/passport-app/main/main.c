// AI Passport 主流程与统一按键状态机（参见 docs/button-design.md）。
//
// 按键硬件边界（三键共用 GPIO0/ADC1_CH0 分压电阻网络，严禁双键组合）：
//   OK   单击：熄屏仅亮屏看时间；亮屏待机立即开始会议录音；录音中仅亮屏看时长；错误页返回
//   OK   长按：会议录音中按住满 1.5s 停止并保存（防翻页误触）；待机预留语音输入 PTT
//   DOWN 单击：PPT 开启时发下一页（双击兜底连发 2 次，熄屏盲翻不亮屏）；PPT 关闭时点亮屏幕
//   DOWN 长按：按住满 3.0s 开启/关闭 [PPT 翻页] 叠加状态（待机与会议录音中均可随时切换）
//   UP   单击：PPT 开启时发上一页（双击兜底连发 2 次，熄屏盲翻不亮屏）；PPT 关闭时点亮屏幕
//   UP   长按：仅在待机且 PPT 关闭时，按住满 3.0s 触发蓝牙重新配对
//
// 按键回调运行在 button 组件的 esp_timer 任务里，绝对不能阻塞，
// 所以回调只写原子请求标志，真正的动作全部在 50ms 节拍的主循环里做。
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "app_ble.h"
#include "app_config.h"
#include "app_error.h"
#include "app_net.h"
#include "app_prov.h"
#include "app_record.h"
#include "app_store.h"
#include "app_ui.h"
#include "app_upload.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "main"

#define TICK_MS              50
#define UI_REFRESH_US        (1000LL * 1000)
#define BSP_LONG_START_US    (500LL * 1000)
#define HOLD_STOP_REC_US     (1500LL * 1000)
#define HOLD_PPT_TOGGLE_US   (3000LL * 1000)
#define HOLD_BLE_PAIR_US     (3000LL * 1000)
#define REC_HINT_RESET_US    (2200LL * 1000)

// 主循环之外（on_key 回调）只允许写这组原子/易失标志，绝不阻塞或拿 LVGL/BLE 锁。
static volatile bool s_prov_wanted;
static atomic_bool s_ppt_mode;
static atomic_bool s_press_was_screen_off;
static atomic_bool s_req_wake_screen;
static atomic_bool s_req_start_record;
static atomic_bool s_req_rec_short_hint;
static atomic_bool s_req_back_home;
static atomic_bool s_req_cancel_pair;
static atomic_bool s_req_enter_prov;
static atomic_bool s_req_leave_prov;
static atomic_uint s_req_page_next;
static atomic_uint s_req_page_prev;

static volatile bool s_ok_holding_rec;
static volatile int64_t s_ok_hold_start_us;
static volatile bool s_down_holding_ppt;
static volatile int64_t s_down_hold_start_us;
static volatile bool s_up_holding_pair;
static volatile int64_t s_up_hold_start_us;

static int64_t s_rec_hint_until_us;
static uint32_t s_seen_err_seq;   // 已经弹过屏的错误序号

static void record_state_cb(bool active, void *user) {
    (void)user;
    if (active) {
        s_rec_hint_until_us = 0;
        app_ui_set_record_hint(NULL);
        app_ui_set_page(APP_PAGE_RECORDING);
        app_ui_rec_led(true);
        app_log(TAG, "record started session=%s", app_record_session_hex());
#ifndef CONFIG_PASSPORT_WIFI_LEGACY
        app_ble_notify_recording(true, 0);
#endif
    } else {
        s_ok_holding_rec = false;
        s_rec_hint_until_us = 0;
        app_ui_rec_led(false);
        app_ui_set_record_hint(NULL);
        app_ui_set_page(APP_PAGE_HOME);
        app_log(TAG, "record stopped session=%s bytes=%u",
                app_record_session_hex(), (unsigned)app_record_bytes());
#ifdef CONFIG_PASSPORT_WIFI_LEGACY
        // 停止后立刻尝试上传，别等 60 秒周期。
        app_upload_kick();
#else
        app_ble_notify_recording(false, 0);
#endif
    }
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    bool ppt = atomic_load(&s_ppt_mode);
    app_page_t page = app_ui_page();

    if (ev == BSP_BTN_PRESS) {
        bool was_off = !app_ui_is_screen_on();
        atomic_store(&s_press_was_screen_off, was_off);
        // PPT 开启且不在特殊弹窗页时，熄屏按 UP/DOWN 直接盲翻，不亮大屏晃眼。
        bool blind_ppt_key = ppt && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) &&
                             page != APP_PAGE_ERROR && page != APP_PAGE_PROV
#ifndef CONFIG_PASSPORT_WIFI_LEGACY
                             && app_ble_passkey() == UINT32_MAX
#endif
                             ;
        if (!blind_ppt_key) {
            atomic_store(&s_req_wake_screen, true);
        }
        return;
    }

    if (ev == BSP_BTN_RELEASE) {
        if (btn == BSP_BTN_OK && s_ok_holding_rec) {
            s_ok_holding_rec = false;
            atomic_store(&s_req_rec_short_hint, true);
        }
        if (btn == BSP_BTN_DOWN) {
            s_down_holding_ppt = false;
        }
        if (btn == BSP_BTN_UP) {
            s_up_holding_pair = false;
        }
        return;
    }

    // 全局优先级 1：错误页（OK 单击返回，其余按键仅亮屏）
    if (page == APP_PAGE_ERROR) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            atomic_store(&s_req_back_home, true);
        }
        return;
    }

#ifdef CONFIG_PASSPORT_WIFI_LEGACY
    // 旧版 Wi-Fi 配网页兼容分支
    if (page == APP_PAGE_PROV) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_LONG) {
            atomic_store(&s_req_leave_prov, true);
        }
        return;
    }
#else
    // 全局优先级 2：正在显示 6 位配对码或手动开启等待蓝牙配对时，单击取消配对窗口
    if (app_ble_passkey() != UINT32_MAX) {
        if (ev == BSP_BTN_CLICK) {
            atomic_store(&s_req_cancel_pair, true);
        }
        return;
    }
    if (app_ble_pairing_open() && !app_ble_connected()) {
        if (btn == BSP_BTN_OK && ev == BSP_BTN_CLICK) {
            atomic_store(&s_req_cancel_pair, true);
        }
        return;
    }
#endif

    // ---- OK 键分发（零延迟单击 + 录音长按 1.5s 停录） ----------------------
    if (btn == BSP_BTN_OK) {
        if (ev == BSP_BTN_CLICK || ev == BSP_BTN_DOUBLE) {
            if (app_record_active()) {
                // 录音中短按只点亮屏幕看时间/时长，绝不停录（防止翻页误碰断录）
                atomic_store(&s_req_wake_screen, true);
                atomic_store(&s_req_rec_short_hint, true);
                return;
            }
            if (atomic_load(&s_press_was_screen_off)) {
                // 熄屏首按只亮屏看时间，吞掉开录动作
                return;
            }
            atomic_store(&s_req_start_record, true);
            return;
        }
        if (ev == BSP_BTN_LONG) {
            atomic_store(&s_req_wake_screen, true);
            if (app_record_active()) {
                s_ok_hold_start_us = esp_timer_get_time() - BSP_LONG_START_US;
                s_ok_holding_rec = true;
            } else {
#ifdef CONFIG_PASSPORT_WIFI_LEGACY
                atomic_store(&s_req_enter_prov, true);
#endif
                // BLE 模式下待机长按 OK 预留给阶段 3 语音输入 PTT，绝不再触发删绑定重配对
            }
            return;
        }
        return;
    }

    // ---- DOWN 键分发（PPT 下一页 & 长按 3s 开关 PPT 模式） -----------------
    if (btn == BSP_BTN_DOWN) {
        if (ppt && (ev == BSP_BTN_CLICK || ev == BSP_BTN_DOUBLE)) {
            atomic_fetch_add(&s_req_page_next, ev == BSP_BTN_DOUBLE ? 2u : 1u);
            return;
        }
        if (ev == BSP_BTN_LONG) {
            s_down_hold_start_us = esp_timer_get_time() - BSP_LONG_START_US;
            s_down_holding_ppt = true;
        }
        return;
    }

    if (btn == BSP_BTN_UP) {
        if (ppt && (ev == BSP_BTN_CLICK || ev == BSP_BTN_DOUBLE)) {
            atomic_fetch_add(&s_req_page_prev, ev == BSP_BTN_DOUBLE ? 2u : 1u);
            return;
        }
#ifndef CONFIG_PASSPORT_WIFI_LEGACY
        if (ev == BSP_BTN_LONG && !ppt && !app_record_active()) {
            s_up_hold_start_us = esp_timer_get_time() - BSP_LONG_START_US;
            s_up_holding_pair = true;
        }
#endif
    }
}

// 处理按键请求与长按计时门限。阻塞操作（启停录音、BLE 控制、LVGL 刷新）只在这里做。
static void handle_requests(void) {
    int64_t now = esp_timer_get_time();

    if (atomic_exchange(&s_req_wake_screen, false)) {
        app_ui_note_input();
#ifndef CONFIG_PASSPORT_WIFI_LEGACY
        if (!app_ble_connected()) {
            app_ble_advertise_for(0);
        }
#endif
    }

    if (atomic_exchange(&s_req_back_home, false)) {
        app_ui_set_page(app_record_active() ? APP_PAGE_RECORDING : APP_PAGE_HOME);
        app_error_clear();
    }

#ifndef CONFIG_PASSPORT_WIFI_LEGACY
    if (atomic_exchange(&s_req_cancel_pair, false)) {
        app_ble_cancel_pairing();
        app_ui_refresh_home();
    }
#endif

    // 3.0 秒长按 DOWN 开启/关闭 PPT 翻页模式（待机与会议录音中均可随时切换，点亮屏幕提示）
    if (s_down_holding_ppt && (now - s_down_hold_start_us >= HOLD_PPT_TOGGLE_US)) {
        s_down_holding_ppt = false;
        bool next_ppt = !atomic_load(&s_ppt_mode);
        atomic_store(&s_ppt_mode, next_ppt);
        app_ui_note_input();
        app_ui_set_ppt_mode(next_ppt);
        if (app_ui_page() == APP_PAGE_HOME) {
            app_ui_refresh_home();
        } else if (app_ui_page() == APP_PAGE_RECORDING) {
            app_ui_refresh_record(app_record_elapsed_sec(), app_record_bytes());
        }
        app_log(TAG, "ppt mode -> %s (after 3.0s DOWN hold)", next_ppt ? "ON" : "OFF");
    }

#ifndef CONFIG_PASSPORT_WIFI_LEGACY
    unsigned next_cnt = atomic_exchange(&s_req_page_next, 0u);
    if (next_cnt > 0) {
        if (next_cnt > 4u) next_cnt = 4u;
        (void)app_ble_notify_key(APP_BLE_KEY_NEXT, (uint8_t)next_cnt);
    }
    unsigned prev_cnt = atomic_exchange(&s_req_page_prev, 0u);
    if (prev_cnt > 0) {
        if (prev_cnt > 4u) prev_cnt = 4u;
        (void)app_ble_notify_key(APP_BLE_KEY_PREV, (uint8_t)prev_cnt);
    }
#endif

    if (atomic_exchange(&s_req_enter_prov, false)) {
        app_log(TAG, "enter prov by long press");
        s_prov_wanted = true;
    }
    if (atomic_exchange(&s_req_leave_prov, false)) {
        app_log(TAG, "leave prov by long press");
        s_prov_wanted = false;
    }

    if (atomic_exchange(&s_req_rec_short_hint, false)) {
        if (app_record_active()) {
            app_ui_set_record_hint("请长按 1.5 秒停止");
            s_rec_hint_until_us = now + REC_HINT_RESET_US;
        }
    }
    if (s_rec_hint_until_us && now >= s_rec_hint_until_us) {
        s_rec_hint_until_us = 0;
        app_ui_set_record_hint(NULL);
    }

    // 1.5 秒长按 OK 停止会议录音（到达门限立即停录，无需等松手）
    if (s_ok_holding_rec) {
        if (!app_record_active()) {
            s_ok_holding_rec = false;
        } else if (now - s_ok_hold_start_us >= HOLD_STOP_REC_US) {
            s_ok_holding_rec = false;
            app_log(TAG, "stop record after 1.5s OK hold");
            app_record_stop();
        }
    }

#ifndef CONFIG_PASSPORT_WIFI_LEGACY
    // 3.0 秒长按 UP 重新配对蓝牙（仅在待机且 PPT 关闭下允许）
    if (s_up_holding_pair) {
        if (app_record_active() || atomic_load(&s_ppt_mode)) {
            s_up_holding_pair = false;
        } else if (now - s_up_hold_start_us >= HOLD_BLE_PAIR_US) {
            s_up_holding_pair = false;
            app_log(TAG, "BLE re-pairing triggered after 3.0s UP hold");
            app_ui_note_input();
            app_ble_allow_pairing();
            app_ui_set_page(APP_PAGE_HOME);
            app_ui_refresh_home();
        }
    }
#endif

    if (atomic_exchange(&s_req_start_record, false)) {
        if (!app_record_active()) {
            esp_err_t err = app_record_start();
            if (err != ESP_OK) {
                // 具体错误码已由 app_record_start 写串口，这里只负责显示在屏幕上。
                app_ui_show_error(app_error_short(app_error_last()));
            } else {
                app_error_clear();
            }
        }
    }
}

// 由主循环驱动配网启停，避免在按键回调里做重活（开热点、起 HTTP 任务都不轻）。
#ifdef CONFIG_PASSPORT_WIFI_LEGACY
static void prov_tick(void) {
    if (s_prov_wanted && !app_prov_active()) {
        app_ui_set_page(APP_PAGE_PROV);
        app_prov_start();
        app_ui_set_prov_name(app_prov_ap_name());
    } else if (!s_prov_wanted && app_prov_active()) {
        app_prov_stop();
        app_ui_set_page(APP_PAGE_HOME);
    }
    app_prov_tick();

    // 配好后 app_prov 自己关热点（手机断开或超时）。此时若用户还停在配网页，
    // 退回主页，否则屏幕会一直显示"网页配置"，用户以为没成功。
    if (s_prov_wanted && app_prov_got_cfg() && !app_prov_active()) {
        s_prov_wanted = false;
        app_ui_set_page(APP_PAGE_HOME);
        app_log(TAG, "prov done, back to home");
    }
}

#endif

// 任何模块报了新错误（未被去重吞掉）就弹错误页，用户能看到屏幕上那句短话。
// 录音中不抢屏：录音页要一直显示计时；错误仍完整写在串口。
static void error_tick(void) {
    uint32_t seq = app_error_seq();
    if (seq == s_seen_err_seq) return;
    s_seen_err_seq = seq;
    if (app_record_active()) return;
    if (app_ui_page() == APP_PAGE_PROV) return;   // 配网页由手机端驱动，不打断
    app_ui_show_error(app_error_short(app_error_last()));
}

static void ui_tick(void) {
    switch (app_ui_page()) {
    case APP_PAGE_HOME:
        app_ui_refresh_home();
        break;
    case APP_PAGE_RECORDING:
        app_ui_refresh_record(app_record_elapsed_sec(), app_record_bytes());
        break;
    case APP_PAGE_PROV:
        app_ui_refresh_prov(app_prov_client_connected(), app_config_ready(),
                            app_net_state() == APP_NET_FAILED);
        break;
    default:
        break;
    }
    app_ui_idle_tick();
}

void app_main(void) {
    app_log(TAG, "boot firmware=%s %s", __DATE__, __TIME__);

    app_config_init();
    app_error_clear();
    s_seen_err_seq = app_error_seq();   // 初始化阶段的历史错误不在开机后弹窗

    // 屏幕先起来，后面初始化慢时用户也有东西可看。
    if (app_ui_init() != ESP_OK) {
        app_log(TAG, "no display, continue headless");
    }

    app_store_init();
    app_store_seal();     // 掉电中断的会话标封闭，之后才能被 final

    app_record_init();
    app_record_set_callback(record_state_cb, NULL);
#ifdef CONFIG_PASSPORT_WIFI_LEGACY
    app_net_init();
    if (app_config_ready()) app_net_apply_credentials();
    else s_prov_wanted = true;
    app_upload_init();
#else
    app_ble_init();
#endif

    esp_err_t err = bsp_button_init(on_key, NULL);
    if (err != ESP_OK) {
        app_error_report(APP_E400_INIT, APP_SRC_BOOT, "button init: %s",
                         esp_err_to_name(err));
    }

    bsp_battery_init();   // 失败不影响录音，只是电量显示不出来
    app_log(TAG, "boot done dev=%s ready=%d token=%d", app_config_dev(),
            (int)app_config_ready(), (int)app_config_has_token());

#ifdef CONFIG_PASSPORT_WIFI_LEGACY
    bool net_started = app_config_ready();
    int64_t last_upload_kick = 0;
#endif

    int64_t last_slow_tick_us = 0;

    for (;;) {
        handle_requests();
        int64_t now_us = esp_timer_get_time();
        bool do_slow = (now_us - last_slow_tick_us >= UI_REFRESH_US);
#ifdef CONFIG_PASSPORT_WIFI_LEGACY
        if (do_slow) {
            prov_tick();
            if (!net_started && app_config_ready()) {
                app_net_apply_credentials();
                net_started = true;
            }
            if (net_started && !last_upload_kick && app_net_state() == APP_NET_CONNECTED) {
                app_upload_kick();
                last_upload_kick = now_us;
            }
        }
#else
        if (do_slow) {
            app_ble_reclaim_if_idle();
        }
        static uint32_t last_key = UINT32_MAX;
        static bool last_ble_conn = false;
        static bool last_ready = false;
        uint32_t key = app_ble_passkey();
        bool ble_conn = app_ble_connected();
        bool ready = app_ble_ready();
        if (key != UINT32_MAX && key != last_key) {
            app_ui_set_page(APP_PAGE_HOME);
            app_ui_note_input();
            app_ui_refresh_home();
            app_log(TAG, "showing pairing code on home page");
        } else if (ble_conn != last_ble_conn) {
            app_ui_note_input();
            do_slow = true;
        }
        if (ready && !last_ready) {
            app_ble_notify_recording(app_record_active(), app_record_elapsed_sec());
            do_slow = true;
        } else if (key == UINT32_MAX && last_key != UINT32_MAX) {
            do_slow = true;
        }
        last_key = key;
        last_ble_conn = ble_conn;
        last_ready = ready;
#endif
        error_tick();
        if (do_slow) {
            last_slow_tick_us = now_us;
            ui_tick();
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}