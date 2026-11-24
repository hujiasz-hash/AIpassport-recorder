#include "app_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_ble.h"
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

// 保留配对码输入时间；上/下按键仍可主动唤醒。
#define SCREEN_OFF_US (30LL * 1000 * 1000)
#define BACKLIGHT_ON 85

// 中文字形用自带的 14 号子集字体（lv_font_ui_14.c，由 Hiragino Sans GB 生成）。
// 不用 LVGL 内置的 Source Han CJK：它是固定的离散字表，"录/网/缓/错/传" 等常用字都不在
// 里面，屏幕上会变成方框（实测）。数字和大字仍用 Montserrat。
#define FONT_CN &lv_font_ui_14
#define FONT_BIG &lv_font_montserrat_20
#define FONT_TITLE &lv_font_ui_20
#define HOME_BG 0xF6F1E5
#define HOME_MUTED 0x665648
#define OCTO_W 184
#define OCTO_H 192
#define OCTO_COUNT 10
#define OCTO_FRAME_BYTES (OCTO_W * OCTO_H * 2)
#define OCTO_FRAME(i) { .header = { .magic = LV_IMAGE_HEADER_MAGIC, \
    .cf = LV_COLOR_FORMAT_RGB565, .w = OCTO_W, .h = OCTO_H, .stride = OCTO_W * 2 }, \
    .data_size = OCTO_FRAME_BYTES, .data = octobara_frames + (i) * OCTO_FRAME_BYTES }

extern const uint8_t octobara_frames[];
static const lv_image_dsc_t s_octo_frames[OCTO_COUNT] = {
    OCTO_FRAME(0), OCTO_FRAME(1), OCTO_FRAME(2), OCTO_FRAME(3), OCTO_FRAME(4),
    OCTO_FRAME(5), OCTO_FRAME(6), OCTO_FRAME(7), OCTO_FRAME(8), OCTO_FRAME(9),
};

static lv_obj_t *s_scr_home, *s_scr_record, *s_scr_prov, *s_scr_error;
static lv_obj_t *s_home_time, *s_home_batt, *s_home_link, *s_home_link_icon;
static lv_obj_t *s_home_message, *s_home_octo;
static lv_timer_t *s_octo_timer;
static lv_obj_t *s_rec_elapsed, *s_rec_bytes, *s_rec_dot, *s_prov_name, *s_prov_state;
static lv_obj_t *s_error_text;
static app_page_t s_page = APP_PAGE_HOME;
static bool s_ui_ok;
static int64_t s_last_input_us;
static lv_timer_t *s_error_timer;

// ---- 各页构建 ------------------------------------------------------------

static void octo_tick(lv_timer_t *timer) {
    (void)timer;
    static unsigned frame;
    frame = (frame + 1) % OCTO_COUNT;
    lv_image_set_src(s_home_octo, &s_octo_frames[frame]);
}

static void build_home(void) {
    s_scr_home = lv_obj_create(NULL);
    lv_obj_remove_flag(s_scr_home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr_home, lv_color_hex(HOME_BG), 0);
    lv_obj_set_style_border_width(s_scr_home, 0, 0);
    lv_obj_set_style_pad_all(s_scr_home, 0, 0);

#ifdef CONFIG_PASSPORT_WIFI_LEGACY
    const char *link_icon = LV_SYMBOL_WIFI;
    const char *link_text = "网络未连接";
#else
    const char *link_icon = LV_SYMBOL_BLUETOOTH;
    const char *link_text = "蓝牙未连接";
#endif
    // FontAwesome 符号在 Montserrat，不在自带的中文子集里。
    s_home_link_icon = ui_pixel_label(s_scr_home, link_icon, FONT_BIG, HOME_MUTED);
    lv_obj_align(s_home_link_icon, LV_ALIGN_TOP_LEFT, 22, 9);
    s_home_link = ui_pixel_label(s_scr_home, link_text, FONT_CN, HOME_MUTED);
    lv_obj_align(s_home_link, LV_ALIGN_TOP_LEFT, 47, 13);
    s_home_batt = ui_pixel_label(s_scr_home, "-- " LV_SYMBOL_BATTERY_EMPTY,
                                 &lv_font_montserrat_14, HOME_MUTED);
    lv_obj_align(s_home_batt, LV_ALIGN_TOP_RIGHT, -22, 13);

    s_home_time = ui_pixel_label(s_scr_home, "--:--", &lv_font_montserrat_48, UI_INK);
    lv_obj_align(s_home_time, LV_ALIGN_TOP_MID, 0, 34);
    s_home_message = ui_pixel_label(s_scr_home, "时间未同步", FONT_CN, HOME_MUTED);
    lv_obj_align(s_home_message, LV_ALIGN_TOP_MID, 0, 93);

    s_home_octo = lv_image_create(s_scr_home);
    lv_image_set_src(s_home_octo, &s_octo_frames[0]);
    lv_obj_align(s_home_octo, LV_ALIGN_TOP_MID, 0, 114);
    s_octo_timer = lv_timer_create(octo_tick, 100, NULL);
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
    if (s_octo_timer) {
        if (page == APP_PAGE_HOME) lv_timer_resume(s_octo_timer);
        else lv_timer_pause(s_octo_timer);
    }
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
    char time_s[12] = "--:--";
    time_t now = app_net_now();
    const char *message = now ? "" : "时间未同步";
    if (now) {
        // 主页显示北京时间；不改变录音和蓝牙协议使用的 Unix 时间戳。
        now += 8 * 60 * 60;
        struct tm tm;
        gmtime_r(&now, &tm);
        strftime(time_s, sizeof time_s, "%H:%M", &tm);
    }
    if (app_store_full()) message = "缓存已满 等上传";

    char batt[24] = "-- " LV_SYMBOL_BATTERY_EMPTY;
    int soc = bsp_battery_soc();
    if (soc >= 0) {
        const char *icon = soc >= 90 ? LV_SYMBOL_BATTERY_FULL :
                           soc >= 65 ? LV_SYMBOL_BATTERY_3 :
                           soc >= 40 ? LV_SYMBOL_BATTERY_2 :
                           soc >= 15 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        snprintf(batt, sizeof batt, "%d%% %s", soc, icon);
    }

    bool connected;
    const char *status;
#ifdef CONFIG_PASSPORT_WIFI_LEGACY
    connected = app_net_state() == APP_NET_CONNECTED;
    status = connected ? "网络已连接" : "网络未连接";
    if (!app_config_ready()) message = "长按 OK 进入配网";
#else
    connected = app_ble_connected();
    status = connected ? "蓝牙已连接" : "蓝牙未连接";
    uint32_t key = app_ble_passkey();
    if (key != UINT32_MAX) {
        snprintf(time_s, sizeof time_s, "%06u", (unsigned)key);
        message = "配对码 请在 Mac 输入";
    }
#endif

    if (!bsp_lvgl_lock(200)) return;
    set_label(s_home_time, time_s);
    set_label(s_home_batt, batt);
    if (strcmp(lv_label_get_text(s_home_link), status) != 0) {
        set_label(s_home_link, status);
        lv_obj_set_style_text_color(s_home_link_icon,
                                    lv_color_hex(connected ? UI_SKY_DARK : HOME_MUTED), 0);
    }
    set_label(s_home_message, message);
    if (s_octo_timer && s_page == APP_PAGE_HOME &&
        esp_timer_get_time() - s_last_input_us < SCREEN_OFF_US) {
        lv_timer_resume(s_octo_timer);
    }
    bsp_lvgl_unlock();
}

void app_ui_refresh_record(uint32_t elapsed_sec, uint32_t bytes) {
    if (!s_ui_ok) return;
    char t[16], b[48];
    snprintf(t, sizeof t, "%02u:%02u", (unsigned)(elapsed_sec / 60), (unsigned)(elapsed_sec % 60));
#ifdef CONFIG_PASSPORT_WIFI_LEGACY
    snprintf(b, sizeof b, "%u KB", (unsigned)(bytes / 1024));
#else
    uint32_t key = app_ble_passkey();
    if (key != UINT32_MAX) snprintf(b, sizeof b, "配对码 %06u", (unsigned)key);
    else snprintf(b, sizeof b, "%u KB %s", (unsigned)(bytes / 1024), app_ble_ready() ? "蓝牙已连接" : "蓝牙未连接");
#endif
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
    // 录音中不熄屏；其他页到达空闲阈值时停止动画和背光。
    if (s_page == APP_PAGE_RECORDING || idle_us < SCREEN_OFF_US) return;
    if (s_octo_timer && bsp_lvgl_lock(200)) {
        lv_timer_pause(s_octo_timer);
        bsp_lvgl_unlock();
    }
    bsp_display_backlight(0);
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