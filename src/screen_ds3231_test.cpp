#include "screen_ds3231_test.h"
#include "screen_admin_tools.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "rtc.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>
#include <time.h>

#define FOOTER_H 52

static lv_obj_t *_scr;
static lv_obj_t *_lbl;

static void format_tm(const struct tm &t, char *out, size_t out_len) {
    strftime(out, out_len, "%Y-%m-%d %H:%M:%S", &t);
}

static void refresh_report() {
    String out;

    if (!rtc_available()) {
        out += "DS3231: NOT FOUND\n\n"
               "Not responding on I2C\n"
               "(SDA=5, SCL=6). Check\n"
               "wiring/pull-ups/VCC.\n\n";
    } else {
        out += "DS3231: Found\n";
        out += rtc_lost_power() ? "Lost Power: YES (time not\ntrustworthy -- Sync To RTC\nto fix)\n\n"
                                 : "Lost Power: No\n\n";

        struct tm rtc_tm;
        if (rtc_read(&rtc_tm)) {
            char buf[24];
            format_tm(rtc_tm, buf, sizeof(buf));
            out += "RTC time:\n" + String(buf) + "\n\n";
        }
    }

    time_t now = time(nullptr);
    struct tm sys_tm;
    gmtime_r(&now, &sys_tm);
    char buf[24];
    format_tm(sys_tm, buf, sizeof(buf));
    out += "System time:\n" + String(buf);

    lv_label_set_text(_lbl, out.c_str());
}

// Re-probes the chip and re-syncs the system clock from it -- same thing rtc_init() does
// at boot, just callable again live so this screen can test it without a reboot.
static void cb_sync_from() {
    rtc_init();
    refresh_report();
}

static void cb_sync_to() {
    rtc_sync_from_system();
    refresh_report();
}

static void cb_back() {
    screen_admin_tools_push();
}

void screen_ds3231_test_push() {
    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        _lbl = lv_label_create(_scr);
        lv_label_set_long_mode(_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_lbl, SCREEN_W - 40);
        lv_obj_set_style_text_color(_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_align(_lbl, LV_ALIGN_TOP_LEFT, 20, HDR_H + 20);

        lv_obj_t *legend = ui_legend(_scr);
        lv_obj_set_width(legend, SCREEN_W - 24);
        lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
        char right_arrow[24];
        snprintf(right_arrow, sizeof(right_arrow), "Sync To RTC %s", LV_SYMBOL_RIGHT);
        ui_legend_row(legend, "", lv_color_hex(C_TEXT), right_arrow, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, "Sync From RTC", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));
    }

    header_set_visible(true);
    header_set_title("DS3231 TEST");
    refresh_report();

    ButtonHandlers h;
    h.right = cb_sync_to;
    h.enter = cb_sync_from;
    h.back  = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
