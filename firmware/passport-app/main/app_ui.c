#include "app_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "app_error.h"
#include "app_net.h"
#include "app_store.h"
#include "app_upload.h"
#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ui_pixel.h"

// 自带中文字体（lv_font_ui_14.c / lv_font_ui_20.c 生成）。
extern const lv_font_t lv_font_ui_14;
extern const lv_font_t lv_font_ui_20;

#define TAG "ui"

// 屏幕 240×320，UI 按这个尺寸摆。
#define SCREEN_W 240
#define SCREEN_H 320

// 错误页 10 秒后自动回主页。
#define ERROR_AUTO_BACK_MS 10000

// 无操作 10 秒熄屏；有按键立刻点亮。
#define SCREEN_OFF_US (10LL * 1000 * 1000)
#define BACKLIGHT_ON 85

// 中文字形用自带的 14 号子集字体（lv_font_ui_14.c，由 Hiragino Sans GB 生成）。
// 不用 LVGL 内置的 Source Han CJK：它是固定的离散字表，"录/网/缓/错/传" 等常用字都不在
// 里面，屏幕上会变成方框（实测）。数字和大字仍用 Montserrat。
#define FONT_CN &lv_font_ui_14
#define FONT_BIG &lv_font_montserrat_20
#define FONT_TITLE &lv_font_ui_20

static lv_obj_t *s_scr_home, *s_scr_record, *s_scr_prov, *s_scr_error;
static lv_obj_t *s_home_time, *s_home_date, *s_home_batt, *s_home_pending, *s_home_hint;
static lv_obj_t *s_rec_elapsed, *s_rec_bytes, *s_rec_dot, *s_prov_name, *s_prov_state;
static lv_obj_t *s_error_text;
static app_page_t s_page = APP_PAGE_HOME;
static bool s_ui_ok;
static int64_t s_last_input_us;
static lv_timer_t *s_error_timer;

// ---- 各页构建 ------------------------------------------------------------

static void build_home(void) {
    s_scr_home = ui_pixel_screen_create("AI PASSPORT", FONT_BIG);

    lv_obj_t *panel = ui_pixel_panel_create(s_scr_home, 16, 58, 208, 126, UI_PAPER);

    s_home_time = ui_pixel_label(panel, "--:--", FONT_BIG, UI_INK);
    lv_obj_set_pos(s_home_time, 6, 2);

    s_home_date = ui_pixel_label(panel, "时间未同步", FONT_CN, UI_INK);
    lv_obj_set_pos(s_home_date, 6, 36);

    s_home_batt = ui_pixel_label(panel, "电量 --", FONT_CN, UI_INK);
    lv_obj_set_pos(s_home_batt, 6, 62);

    s_home_pending = ui_pixel_label(panel, "待传 --", FONT_CN, UI_INK);
    lv_obj_set_pos(s_home_pending, 6, 88);

    ui_pixel_mascot_create(s_scr_home, 186, 196);

    s_home_hint = ui_pixel_label(s_scr_home, "OK 开始录音", FONT_CN, UI_PAPER);
    lv_obj_set_pos(s_home_hint, 16, 208);
}

static void build_record(void) {
    s_scr_record = ui_pixel_screen_create("录音", FONT_TITLE);

    lv_obj_t *panel = ui_pixel_panel_create(s_scr_record, 16, 58, 208, 126, UI_PAPER);
    s_rec_elapsed = ui_pixel_label(panel, "00:00", FONT_BIG, UI_INK);
    lv_obj_set_pos(s_rec_elapsed, 6, 2);

    // 红点：不闪烁时完全透明，录音时由 app_ui_rec_led 打开。
    s_rec_dot = ui_pixel_label(panel, "REC", FONT_CN, UI_RED);
    lv_obj_set_pos(s_rec_dot, 150, 6);
    lv_obj_set_style_opa(s_rec_dot, LV_OPA_TRANSP, 0);

    s_rec_bytes = ui_pixel_label(panel, "0 KB", FONT_CN, UI_INK);
    lv_obj_set_pos(s_rec_bytes, 6, 36);

    lv_obj_t *hint = ui_pixel_label(s_scr_record, "OK 停止保存", FONT_CN, UI_PAPER);
    lv_obj_set_pos(hint, 16, 208);
}

static void build_prov(void) {
    s_scr_prov = ui_pixel_screen_create("配网", FONT_TITLE);

    lv_obj_t *panel = ui_pixel_panel_create(s_scr_prov, 16, 70, 208, 140, UI_PAPER);
    lv_obj_t *l1 = ui_pixel_label(panel, "手机连接设备", FONT_CN, UI_INK);
    lv_obj_set_pos(l1, 4, 2);

    s_prov_name = ui_pixel_label(panel, "AIPassport", FONT_BIG, UI_SKY_DARK);
    lv_obj_set_pos(s_prov_name, 4, 24);

    lv_obj_t *l3 = ui_pixel_label(panel, "地址 192.168.4.1", FONT_CN, UI_INK);
    lv_obj_set_pos(l3, 4, 56);

    lv_obj_t *l4 = ui_pixel_label(panel, "网页配置", FONT_CN, UI_INK);
    lv_obj_set_pos(l4, 4, 82);

    // 连接状态：手机连上后变成"已连接"，否则用户不知道该不该重试。
    s_prov_state = ui_pixel_label(panel, "等待手机连接", FONT_CN, UI_RED);
    lv_obj_set_pos(s_prov_state, 4, 112);

    lv_obj_t *hint = ui_pixel_label(s_scr_prov, "长按 OK 回主页", FONT_CN, UI_PAPER);
    lv_obj_set_pos(hint, 16, 216);
}

static void build_error(void) {
    s_scr_error = ui_pixel_screen_create("出错", FONT_TITLE);

    lv_obj_t *panel = ui_pixel_panel_create(s_scr_error, 16, 80, 208, 120, UI_PAPER);
    s_error_text = ui_pixel_label(panel, "E?", FONT_CN, UI_RED);
    lv_obj_set_pos(s_error_text, 4, 4);
    lv_label_set_long_mode(s_error_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_error_text, 176);

    lv_obj_t *hint = ui_pixel_label(s_scr_error, "按 OK 返回", FONT_CN, UI_PAPER);
    lv_obj_set_pos(hint, 16, 214);
}

// ---- 公共 ----------------------------------------------------------------

static void error_back_cb(lv_timer_t *t) {
    (void)t;
    app_ui_set_page(APP_PAGE_HOME);
}

esp_err_t app_ui_init(void) {
    if (bsp_display_init() != ESP_OK) {
        app_error_report(APP_E400_INIT, APP_SRC_UI, "display init failed");
        return ESP_FAIL;
    }
    if (!bsp_lvgl_init()) {
        app_error_report(APP_E400_INIT, APP_SRC_UI, "lvgl init failed");
        return ESP_FAIL;
    }
    if (!bsp_lvgl_lock(2000)) {
        app_error_report(APP_E202_TIMEOUT, APP_SRC_UI, "lvgl lock timeout at init");
        return ESP_ERR_TIMEOUT;
    }
    build_home();
    build_record();
    build_prov();
    build_error();
    lv_screen_load(s_scr_home);

    s_error_timer = lv_timer_create(error_back_cb, ERROR_AUTO_BACK_MS, NULL);
    lv_timer_pause(s_error_timer);
    bsp_lvgl_unlock();

    bsp_display_backlight(BACKLIGHT_ON);
    s_last_input_us = esp_timer_get_time();
    s_page = APP_PAGE_HOME;
    s_ui_ok = true;
    app_log(TAG, "ui ready %dx%d", SCREEN_W, SCREEN_H);
    return ESP_OK;
}

void app_ui_set_page(app_page_t page) {
    if (!s_ui_ok) return;
    if (!bsp_lvgl_lock(500)) return;
    lv_obj_t *scr = s_scr_home;
    switch (page) {
    case APP_PAGE_RECORDING: scr = s_scr_record; break;
    case APP_PAGE_PROV: scr = s_scr_prov; break;
    case APP_PAGE_ERROR: scr = s_scr_error; break;
    default: scr = s_scr_home; break;
    }
    lv_screen_load(scr);
    s_page = page;
    if (page == APP_PAGE_ERROR && s_error_timer) {
        lv_timer_reset(s_error_timer);
        lv_timer_resume(s_error_timer);
    } else if (s_error_timer) {
        lv_timer_pause(s_error_timer);
    }
    bsp_lvgl_unlock();
    app_ui_note_input();
}

app_page_t app_ui_page(void) {
    return s_page;
}

// 内容没变就不碰 LVGL：主页每秒刷一次，避免无谓重绘。
static void set_label(lv_obj_t *label, const char *text) {
    if (!label) return;
    const char *cur = lv_label_get_text(label);
    if (cur && strcmp(cur, text) == 0) return;
    lv_label_set_text(label, text);
}

void app_ui_refresh_home(void) {
    if (!s_ui_ok) return;
    char time_s[8] = "--:--";
    char date_s[32] = "时间未同步";
    time_t now = app_net_now();
    if (now) {
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(time_s, sizeof time_s, "%H:%M", &tm);
        // strftime 的 %A 是英文星期，中文星期自己拼。
        static const char *WD[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
        snprintf(date_s, sizeof date_s, "%d月%d日 %s", tm.tm_mon + 1, tm.tm_mday, WD[tm.tm_wday]);
    }

    char batt[24];
    int soc = bsp_battery_soc();
    if (soc >= 0) {
        snprintf(batt, sizeof batt, "电量 %d%%", soc);
    } else {
        snprintf(batt, sizeof batt, "电量 --");
    }

    char pend[32];
    uint32_t pending = app_store_pending_bytes();
    if (pending == 0) {
        snprintf(pend, sizeof pend, "均已上传");
    } else if (pending >= 1024) {
        snprintf(pend, sizeof pend, "待传 %u KB", (unsigned)(pending / 1024));
    } else {
        snprintf(pend, sizeof pend, "待传 %u B", (unsigned)pending);
    }

    // 提示行优先显示最需要处理的一件事。
    char hint[48];
    if (!app_config_ready()) {
        snprintf(hint, sizeof hint, "长按 OK 进入配网");
    } else if (app_store_full()) {
        snprintf(hint, sizeof hint, "缓存已满 等上传");
    } else if (app_upload_state() == APP_UP_RUNNING) {
        snprintf(hint, sizeof hint, "上传中");
    } else {
        snprintf(hint, sizeof hint, "OK 开始录音");
    }

    if (!bsp_lvgl_lock(200)) return;
    set_label(s_home_time, time_s);
    set_label(s_home_date, date_s);
    set_label(s_home_batt, batt);
    set_label(s_home_pending, pend);
    set_label(s_home_hint, hint);
    bsp_lvgl_unlock();
}

void app_ui_refresh_record(uint32_t elapsed_sec, uint32_t bytes) {
    if (!s_ui_ok) return;
    char t[16], b[24];
    snprintf(t, sizeof t, "%02u:%02u", (unsigned)(elapsed_sec / 60), (unsigned)(elapsed_sec % 60));
    snprintf(b, sizeof b, "%u KB", (unsigned)(bytes / 1024));
    if (!bsp_lvgl_lock(200)) return;
    set_label(s_rec_elapsed, t);
    set_label(s_rec_bytes, b);
    bsp_lvgl_unlock();
}

void app_ui_show_error(const char *short_text) {
    if (!s_ui_ok) return;
    if (!bsp_lvgl_lock(500)) return;
    lv_label_set_text(s_error_text, short_text ? short_text : "E? 未知错误");
    bsp_lvgl_unlock();
    app_ui_set_page(APP_PAGE_ERROR);
}

void app_ui_backlight(uint8_t percent) {
    bsp_display_backlight(percent);
}

void app_ui_note_input(void) {
    s_last_input_us = esp_timer_get_time();
    bsp_display_backlight(BACKLIGHT_ON);
}

int64_t app_ui_last_input_us(void) {
    return s_last_input_us;
}

void app_ui_idle_tick(void) {
    if (!s_ui_ok) return;
    int64_t idle_us = esp_timer_get_time() - s_last_input_us;
    // 录音中不熄屏：用户要能看到计时。其他页 10 秒熄屏。
    if (s_page == APP_PAGE_RECORDING) return;
    if (idle_us >= SCREEN_OFF_US) {
        bsp_display_backlight(0);
    }
}

static void rec_dot_opa(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

void app_ui_rec_led(bool active) {
    if (!s_ui_ok) return;
    if (!bsp_lvgl_lock(200)) return;
    if (active) {
        lv_anim_del(s_rec_dot, rec_dot_opa);
        lv_obj_set_style_opa(s_rec_dot, LV_OPA_COVER, 0);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_rec_dot);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
        lv_anim_set_duration(&a, 600);
        lv_anim_set_playback_duration(&a, 600);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a, rec_dot_opa);
        lv_anim_start(&a);
    } else {
        lv_anim_del(s_rec_dot, rec_dot_opa);
        lv_obj_set_style_opa(s_rec_dot, LV_OPA_TRANSP, 0);
    }
    bsp_lvgl_unlock();
}

void app_ui_set_prov_name(const char *name) {
    if (!s_ui_ok || !name) return;
    if (!bsp_lvgl_lock(200)) return;
    set_label(s_prov_name, name);
    bsp_lvgl_unlock();
}

// 三种情况按用户下一步该做什么排序：连上并配好 > 连上了等填表 > 还没连 > 连 Wi-Fi 失败。
void app_ui_refresh_prov(bool client_connected, bool cfg_ready, bool connect_failed) {
    if (!s_ui_ok) return;
    const char *text;
    uint32_t color;
    if (connect_failed) {
        text = "网络连接失败 请重试";
        color = UI_RED;
    } else if (cfg_ready) {
        text = "配置完成";
        color = UI_SKY_DARK;
    } else if (client_connected) {
        text = "已连接 请设置";
        color = UI_SKY_DARK;
    } else {
        text = "等待手机连接";
        color = UI_RED;
    }
    if (!bsp_lvgl_lock(200)) return;
    if (s_prov_state) {
        set_label(s_prov_state, text);
        lv_obj_set_style_text_color(s_prov_state, lv_color_hex(color), 0);
    }
    bsp_lvgl_unlock();
}