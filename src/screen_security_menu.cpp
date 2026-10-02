#include "screen_security_menu.h"
#include "screen_settings.h"
#include "screen_wifi_password.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "session_timer.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the Security submenu declared in screen_security_menu.h.
*/

#define MENU_COUNT 2
#define FOOTER_H   52

static lv_obj_t *_scr;
static lv_obj_t *_rows[MENU_COUNT];
static lv_obj_t *_row_lbls[MENU_COUNT];  // row 1 (auto-logout) is dynamic; row 0 is static
static int        _cursor;
static int        _prev_cursor = -1;

static const char *MENU_LABELS[MENU_COUNT] = {
    "1. WiFi Password",
    "2. Auto Logout: - min",  // overwritten immediately by update_logout_row_label()
    // "Admin Password" removed 2026-09-29 along with the web login it set.
};

// Highlights the currently-selected row.
static void refresh_cursor() {
    if (_prev_cursor >= 0 && _prev_cursor != _cursor) {
        lv_obj_set_style_bg_opa(_rows[_prev_cursor], LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(_rows[_cursor], LV_OPA_30, LV_PART_MAIN);
    _prev_cursor = _cursor;
    lv_obj_scroll_to_view(_rows[_cursor], LV_ANIM_OFF);
}

// Up: moves the selection up one row, wrapping.
static void cb_up() {
    _cursor = (_cursor - 1 + MENU_COUNT) % MENU_COUNT;
    refresh_cursor();
}

// Down: moves the selection down one row, wrapping.
static void cb_down() {
    _cursor = (_cursor + 1) % MENU_COUNT;
    refresh_cursor();
}

// Back returns to Settings.
static void cb_back() {
    screen_settings_push();
}

// Redraws the auto-logout row's label with the current timeout.
static void update_logout_row_label() {
    char buf[32];
    snprintf(buf, sizeof(buf), "2. Auto Logout: %d min", session_timer_get_minutes());
    lv_label_set_text(_row_lbls[1], buf);
}

// Enter on row 2: advances the auto-logout timeout by one minute, wrapping 10 -> 1.
static void act_cycle_logout_timeout() {
    session_timer_set_minutes(session_timer_get_minutes() + 1);  // wraps 10 -> 1
    update_logout_row_label();
}

// Enter dispatches to whichever row is selected.
static void cb_enter() {
    switch (_cursor) {
        case 0: screen_wifi_password_push();   break;
        case 1: act_cycle_logout_timeout();    break;
    }
}

// Loads the Security submenu.
void screen_security_menu_push() {
    _cursor = 0;

    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t *list = lv_obj_create(_scr);
        lv_obj_set_size(list, SCREEN_W, SCREEN_H - HDR_H - FOOTER_H);
        lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -FOOTER_H);
        lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(list, 14, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(list, 12, LV_PART_MAIN);  // 12px side inset, same as every screen
        lv_obj_set_style_pad_row(list, 10, LV_PART_MAIN);
        lv_obj_set_layout(list, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

        for (int i = 0; i < MENU_COUNT; i++) {
            lv_obj_t *row = lv_obj_create(list);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
            lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(row, 16, LV_PART_MAIN);
            lv_obj_set_style_bg_color(row, lv_color_hex(C_CYAN), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            _rows[i] = row;

            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, MENU_LABELS[i]);
            lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
            _row_lbls[i] = lbl;
        }

        lv_obj_t *legend = ui_legend(_scr);
        lv_obj_set_width(legend, SCREEN_W - 24);
        lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
        char move_lbl[24];
        snprintf(move_lbl, sizeof(move_lbl), "%s%s Move", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
        ui_legend_row(legend, "", lv_color_hex(C_TEXT), move_lbl, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, "Open", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));
    }

    update_logout_row_label();  // reflects the persisted value, not just the built-in default
    header_set_visible(true);
    header_set_title("SECURITY");
    refresh_cursor();

    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.enter = cb_enter;
    h.back  = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
