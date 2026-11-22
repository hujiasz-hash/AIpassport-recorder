// AI Passport 主流程：主页 → 录音 → 上传；长按 OK 进网页配网。
//
// 按键约定（三键共用一个 ADC 引脚，不能同时按）：
//   OK   单击  开始/停止录音；错误页上返回主页
//   OK   长按  进入网页配网（设备开热点，手机连上后填表；配网中长按退出）
//   上/下      预留（当前无动作）
//
// 按键回调运行在 button 组件的 esp_timer 任务里，绝对不能阻塞，
// 所以回调只置请求标志，真正的动作在主循环里做。
#include <stdio.h>
#include <string.h>

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

#define TICK_MS 1000

// 主循环之外只允许写这几个标志。
static volatile bool s_prov_wanted;
static volatile bool s_req_toggle_record;
static volatile bool s_req_back_home;
static volatile bool s_req_enter_prov;
static volatile bool s_req_leave_prov;
static uint32_t s_seen_err_seq;   // 已经弹过屏的错误序号

static void record_state_cb(bool active, void *user) {
    (void)user;
    if (active) {
        app_ui_set_page(APP_PAGE_RECORDING);
        app_ui_rec_led(true);
        app_log(TAG, "record started session=%s", app_record_session_hex());
    } else {
        app_ui_rec_led(false);
        app_ui_set_page(APP_PAGE_HOME);
        app_log(TAG, "record stopped session=%s bytes=%u",
                app_record_session_hex(), (unsigned)app_record_bytes());
        // 停止后立刻尝试上传，别等 60 秒周期。
        app_upload_kick();
    }
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (ev == BSP_BTN_PRESS) {
        // 按下就点亮屏幕。
        app_ui_note_input();
        return;
    }
    if (btn != BSP_BTN_OK) return;

    if (ev == BSP_BTN_LONG) {
        if (app_ui_page() == APP_PAGE_PROV) {
            s_req_leave_prov = true;
        } else {
            s_req_enter_prov = true;
        }
        return;
    }
    if (ev != BSP_BTN_CLICK) return;
    if (app_ui_page() == APP_PAGE_ERROR) {
        s_req_back_home = true;
        return;
    }
    if (app_ui_page() == APP_PAGE_PROV) return;   // 配网页由手机端驱动
    s_req_toggle_record = true;
}

// 处理按键请求。阻塞操作（停录音、开热点）只在这里做。
static void handle_requests(void) {
    if (s_req_back_home) {
        s_req_back_home = false;
        app_ui_set_page(APP_PAGE_HOME);
        app_error_clear();
    }
    if (s_req_enter_prov) {
        s_req_enter_prov = false;
        app_log(TAG, "enter prov by long press");
        s_prov_wanted = true;
    }
    if (s_req_leave_prov) {
        s_req_leave_prov = false;
        app_log(TAG, "leave prov by long press");
        s_prov_wanted = false;
    }
    if (s_req_toggle_record) {
        s_req_toggle_record = false;
        if (app_record_active()) {
            app_log(TAG, "stop requested by key");
            app_record_stop();
        } else {
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

    app_net_init();
    if (app_config_ready()) {
        app_net_apply_credentials();
    } else {
        // 没配网：直接进配网模式。这个产品第一次上手就这一条路。
        app_log(TAG, "not provisioned, entering prov");
        s_prov_wanted = true;
    }

    app_record_init();
    app_record_set_callback(record_state_cb, NULL);
    app_upload_init();

    esp_err_t err = bsp_button_init(on_key, NULL);
    if (err != ESP_OK) {
        app_error_report(APP_E400_INIT, APP_SRC_BOOT, "button init: %s",
                         esp_err_to_name(err));
    }

    bsp_battery_init();   // 失败不影响录音，只是电量显示不出来
    app_log(TAG, "boot done dev=%s ready=%d token=%d", app_config_dev(),
            (int)app_config_ready(), (int)app_config_has_token());

    bool net_started = app_config_ready();
    int64_t last_upload_kick = 0;

    for (;;) {
        handle_requests();
        prov_tick();

        // 配网刚写完配置：由 app_prov 调 apply_credentials，这里只补一次保险。
        if (!net_started && app_config_ready()) {
            app_net_apply_credentials();
            net_started = true;
        }

        // 联网后每 60 秒踢一次上传（上传任务自身也有周期，这里保证刚联网时尽快开始）。
        if (net_started && app_net_state() == APP_NET_CONNECTED) {
            int64_t now = esp_timer_get_time();
            if (now - last_upload_kick > 60LL * 1000 * 1000) {
                app_upload_kick();
                last_upload_kick = now;
            }
        }

        error_tick();
        ui_tick();
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}