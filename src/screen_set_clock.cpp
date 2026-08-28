#include "screen_set_clock.h"
#include "screen_settings.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "rtc.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>
#include <time.h>
#include <sys/time.h>

enum Row { ROW_YEAR, ROW_MONTH, ROW_DAY, ROW_HOUR, ROW_MINUTE, ROW_COUNT };

static lv_obj_t *_scr;
static lv_obj_t *_rows[ROW_COUNT];
static lv_obj_t *_val_lbls[ROW_COUNT];
static int        _row_cursor;

// Editable fields, seeded from the current system clock each time the screen opens (see
// screen_set_clock_push()) so a small correction only needs a few nudges, not dialing in
// from scratch. Day isn't clamped per-month (no calendar-aware max-day logic) -- a
// deliberate simplification for a manual stopgap screen, not worth a date library here.
static int _year, _month, _day, _hour, _minute;

static void refresh_values() {
    char buf[8];
    snprintf(buf, sizeof(buf), "%04d", _year);   lv_label_set_text(_val_lbls[ROW_YEAR],   buf);
    snprintf(buf, sizeof(buf), "%02d", _month);  lv_label_set_text(_val_lbls[ROW_MONTH],  buf);
    snprintf(buf, sizeof(buf), "%02d", _day);    lv_label_set_text(_val_lbls[ROW_DAY],    buf);
    snprintf(buf, sizeof(buf), "%02d", _hour);   lv_label_set_text(_val_lbls[ROW_HOUR],   buf);
    snprintf(buf, sizeof(buf), "%02d", _minute); lv_label_set_text(_val_lbls[ROW_MINUTE], buf);
}

static void refresh_row_highlight() {
    for (int i = 0; i < ROW_COUNT; i++) {
        lv_obj_set_style_bg_opa(_rows[i], i == _row_cursor ? LV_OPA_30 : LV_OPA_TRANSP, LV_PART_MAIN);
    }
}

static void cb_row_prev() {
    _row_cursor = (_row_cursor - 1 + ROW_COUNT) % ROW_COUNT;
    refresh_row_highlight();
}

static void cb_row_next() {
    _row_cursor = (_row_cursor + 1) % ROW_COUNT;
    refresh_row_highlight();
}

static void adjust(int delta) {
    switch (_row_cursor) {
        case ROW_YEAR:
            _year += delta;
            if (_year < 2020) _year = 2099;
            if (_year > 2099) _year = 2020;
            break;
        case ROW_MONTH:
            _month += delta;
            if (_month < 1)  _month = 12;
            if (_month > 12) _month = 1;
            break;
        case ROW_DAY:
            _day += delta;
            if (_day < 1)  _day = 31;
            if (_day > 31) _day = 1;
            break;
        case ROW_HOUR:
            _hour += delta;
            if (_hour < 0)  _hour = 23;
            if (_hour > 23) _hour = 0;
            break;
        case ROW_MINUTE:
            _minute += delta;
            if (_minute < 0)  _minute = 59;
            if (_minute > 59) _minute = 0;
            break;
    }
    refresh_values();
}

static void cb_adjust_down() { adjust(-1); }
static void cb_adjust_up()   { adjust(1);  }

// No timezone handling anywhere in this codebase (see webserver.cpp's format_epoch,
// checkouts.cpp) -- mktime() interprets the struct as local time under the C library's
// TZ setting, which is UTC by default on this build (no TZ env var is ever set), so this
// stores exactly the wall-clock the admin typed, same convention as everywhere else.
static void cb_set() {
    struct tm tmval = {};
    tmval.tm_year = _year - 1900;
    tmval.tm_mon  = _month - 1;
    tmval.tm_mday = _day;
    tmval.tm_hour = _hour;
    tmval.tm_min  = _minute;
    tmval.tm_sec  = 0;

    time_t t = mktime(&tmval);
    struct timeval tv = { t, 0 };
    settimeofday(&tv, nullptr);

    // Also push the correction into the DS3231 if it's wired -- with no NTP path on this
    // AP-only device (see rtc.h), a manual Set Clock is the only way the chip ever gets
    // updated after its first setting. No-op if the chip isn't present.
    rtc_sync_from_system();

    Serial.printf("[CLOCK] Set to %04d-%02d-%02d %02d:%02d\n", _year, _month, _day, _hour, _minute);
    screen_settings_push();
}

static void cb_cancel() {
    screen_settings_push();
}

static lv_obj_t *make_row(lv_obj_t *parent, const char *label, lv_obj_t **val_lbl_out) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CYAN), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);

    lv_obj_t *val_lbl = lv_label_create(row);
    lv_obj_set_style_text_color(val_lbl, lv_color_hex(C_GREEN), LV_PART_MAIN);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    *val_lbl_out = val_lbl;

    return row;
}

void screen_set_clock_push() {
    _row_cursor = ROW_YEAR;

    time_t now = time(nullptr);
    struct tm tmval;
    gmtime_r(&now, &tmval);
    _year   = tmval.tm_year + 1900;
    _month  = tmval.tm_mon + 1;
    _day    = tmval.tm_mday;
    _hour   = tmval.tm_hour;
    _minute = tmval.tm_min;

    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t *content = lv_obj_create(_scr);
        lv_obj_set_size(content, SCREEN_W, SCREEN_H - HDR_H);
        lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(content, 14, LV_PART_MAIN);
        lv_obj_set_style_pad_row(content, 10, LV_PART_MAIN);
        lv_obj_set_layout(content, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

        _rows[ROW_YEAR]   = make_row(content, "Year",   &_val_lbls[ROW_YEAR]);
        _rows[ROW_MONTH]  = make_row(content, "Month",  &_val_lbls[ROW_MONTH]);
        _rows[ROW_DAY]    = make_row(content, "Day",    &_val_lbls[ROW_DAY]);
        _rows[ROW_HOUR]   = make_row(content, "Hour",   &_val_lbls[ROW_HOUR]);
        _rows[ROW_MINUTE] = make_row(content, "Minute", &_val_lbls[ROW_MINUTE]);

        lv_obj_t *grow = lv_obj_create(content);
        lv_obj_set_size(grow, LV_PCT(100), 1);
        lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
        lv_obj_set_flex_grow(grow, 1);

        lv_obj_t *legend = ui_legend(content);
        char row_lbl[24], adj_lbl[24];
        snprintf(row_lbl, sizeof(row_lbl), "%s%s Row", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
        snprintf(adj_lbl, sizeof(adj_lbl), "Adjust %s%s", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
        ui_legend_row(legend, row_lbl, lv_color_hex(C_YELLOW), adj_lbl, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, "Set", lv_color_hex(C_GREEN), "Cancel", lv_color_hex(C_RED));
    }

    refresh_values();
    refresh_row_highlight();

    header_set_visible(true);
    header_set_title("SET CLOCK");

    ButtonHandlers h;
    h.up    = cb_row_prev;
    h.down  = cb_row_next;
    h.left  = cb_adjust_down;
    h.right = cb_adjust_up;
    h.enter = cb_set;
    h.back  = cb_cancel;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
