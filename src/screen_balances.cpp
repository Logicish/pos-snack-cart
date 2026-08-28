#include "screen_balances.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "checkouts.h"
#include "users.h"
#include "buttons.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>
#include <time.h>
#include <string.h>

#define FOOTER_H 52

// Deliberately a touch smaller than the visible row count so successive presses always
// overlap by a row and never skip one — same tuning rationale as ATTACH_PAGE in
// screen_add_item.cpp.
#define BALANCES_PAGE 7

static lv_obj_t *_scr;
static lv_obj_t *_list;
static lv_obj_t *_rows[MAX_BALANCES];
static OutstandingCheckout _data[MAX_BALANCES];
static int        _count;
static int        _cursor;
static int        _prev_cursor = -1;

static void format_row(char *out, size_t out_len, const OutstandingCheckout &c) {
    const User *u = users_get_by_id(c.user_id);
    char last_initial = (u && u->last_name[0]) ? u->last_name[0] : '?';

    char date_buf[8] = "--";
    if (c.created_at > 0) {
        time_t t = (time_t)c.created_at;
        struct tm tmval;
        gmtime_r(&t, &tmval);
        strftime(date_buf, sizeof(date_buf), "%m/%d", &tmval);
    }

    snprintf(out, out_len, "#%d  %s %c.   $%d.%02d   %s",
             c.id, u ? u->first_name : "?", last_initial,
             c.total_price_cents / 100, c.total_price_cents % 100, date_buf);
}

static void build_row(int i) {
    lv_obj_t *row = lv_obj_create(_list);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CYAN), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    _rows[i] = row;

    char buf[64];
    format_row(buf, sizeof(buf), _data[i]);
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
}

static void refresh_cursor() {
    if (_count == 0) return;
    if (_prev_cursor >= 0 && _prev_cursor < _count && _prev_cursor != _cursor) {
        lv_obj_set_style_bg_opa(_rows[_prev_cursor], LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(_rows[_cursor], LV_OPA_30, LV_PART_MAIN);
    _prev_cursor = _cursor;
    lv_obj_scroll_to_view(_rows[_cursor], LV_ANIM_OFF);
}

// Re-queries and fully rebuilds the row pool -- unlike Browse/Inventory's catalog (stable
// membership visit to visit), the outstanding list shrinks every time Clear is pressed, so
// this can't be a build-once-then-restyle screen.
static void rebuild_rows() {
    lv_obj_clean(_list);
    memset(_rows, 0, sizeof(_rows));
    _prev_cursor = -1;

    _count = checkouts_get_outstanding(_data, MAX_BALANCES);

    if (_count == 0) {
        lv_obj_t *lbl = lv_label_create(_list);
        lv_label_set_text(lbl, "No outstanding balances.");
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_DIM), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        _cursor = 0;
        return;
    }

    for (int i = 0; i < _count; i++) build_row(i);
    if (_cursor >= _count) _cursor = _count - 1;
    refresh_cursor();
}

static void cb_up() {
    if (_count == 0) return;
    _cursor = (_cursor - 1 + _count) % _count;
    refresh_cursor();
}

static void cb_down() {
    if (_count == 0) return;
    _cursor = (_cursor + 1) % _count;
    refresh_cursor();
}

// LEFT: jump ~a screenful down the list, same wrap-at-the-end behavior as
// screen_add_item.cpp's ATTACH_PAGE/cb_attach_pagedown.
static void cb_page_down() {
    if (_count == 0) return;
    if (_cursor >= _count - 1) {
        _cursor = 0;
    } else {
        _cursor += BALANCES_PAGE;
        if (_cursor >= _count) _cursor = _count - 1;
    }
    refresh_cursor();
}

// GREEN: marks the highlighted transaction cleared (the Venmo payment landed) and drops
// it off the list -- no confirmation step, matching how Clear works everywhere else in
// this codebase that isn't destructive of real history (cleared_at is UPDATE-only, never
// a delete — see checkouts.h).
static void cb_clear() {
    if (_count == 0) return;
    checkouts_clear(_data[_cursor].id);
    rebuild_rows();
}

static void cb_back() {
    screen_menu_push();
}

void screen_balances_push() {
    _cursor = 0;

    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        _list = lv_obj_create(_scr);
        lv_obj_set_size(_list, SCREEN_W, SCREEN_H - HDR_H - FOOTER_H);
        lv_obj_align(_list, LV_ALIGN_BOTTOM_MID, 0, -FOOTER_H);
        lv_obj_set_style_bg_opa(_list, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(_list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(_list, 8, LV_PART_MAIN);
        lv_obj_set_style_pad_row(_list, 6, LV_PART_MAIN);
        lv_obj_set_layout(_list, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);

        lv_obj_t *legend = ui_legend(_scr);
        lv_obj_set_width(legend, SCREEN_W - 24);
        lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
        char left_arrow[24], move_lbl[24];
        snprintf(left_arrow, sizeof(left_arrow), "%s Next Screen", LV_SYMBOL_LEFT);
        snprintf(move_lbl, sizeof(move_lbl), "%s%s Move", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
        ui_legend_row(legend, left_arrow, lv_color_hex(C_YELLOW), move_lbl, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, "Clear", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));
    }

    header_set_visible(true);
    header_set_title("BALANCES");
    rebuild_rows();  // data changes visit to visit (and after every Clear), always re-query

    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.left  = cb_page_down;
    h.enter = cb_clear;
    h.back  = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
