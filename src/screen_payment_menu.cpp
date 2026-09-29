#include "screen_payment_menu.h"
#include "screen_payment_edit.h"
#include "screen_settings.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "ui.h"
#include "db.h"
#include "payment_link.h"
#include <sqlite3.h>
#include <lvgl.h>
#include <Arduino.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the Payment Info submenu declared in screen_payment_menu.h.
*/

#define MENU_COUNT 3
#define FOOTER_H   52

static lv_obj_t *_scr;
static lv_obj_t *_rows[MENU_COUNT];
static lv_obj_t *_status[MENU_COUNT];  // "@emrem" / "Not set" line under each row
static int        _cursor;
static int        _prev_cursor = -1;

static const char *MENU_LABELS[MENU_COUNT] = {
    "1. Venmo",
    "2. Zelle",
    "3. Cash App",
};

// DB `method` key + display label passed to screen_payment_edit_push() for each row --
// method strings are lowercase to match screen_pos.cpp's own payment_methods.method checks.
static const char *METHOD_KEYS[MENU_COUNT]   = { "venmo", "zelle", "cashapp" };
static const char *METHOD_LABELS[MENU_COUNT] = { "Venmo", "Zelle", "Cash App" };

// Rewrites each row's status line from payment_methods -- the saved account, or
// "Not set". Refreshed on every visit so a save/delete shows immediately.
static void refresh_status() {
    for (int i = 0; i < MENU_COUNT; i++) {
        char handle[PAYMENT_HANDLE_MAX] = "";
        sqlite3_stmt *stmt;
        if (db_handle() &&
            sqlite3_prepare_v2(db_handle(), "SELECT handle FROM payment_methods WHERE method=?;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, METHOD_KEYS[i], -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char *hd = sqlite3_column_text(stmt, 0);
                if (hd) strncpy(handle, (const char *)hd, sizeof(handle) - 1);
            }
            sqlite3_finalize(stmt);
        }
        char shown[80];
        if (handle[0]) payment_display_handle(METHOD_KEYS[i], handle, shown, sizeof(shown));
        else           snprintf(shown, sizeof(shown), "Not set");
        lv_label_set_text(_status[i], shown);
        lv_obj_set_style_text_color(_status[i], lv_color_hex(handle[0] ? C_GREEN : C_DIM), LV_PART_MAIN);
    }
}

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

// Enter opens the shared payment editor for the selected method.
static void cb_enter() {
    screen_payment_edit_push(METHOD_KEYS[_cursor], METHOD_LABELS[_cursor]);
}

// Loads the Payment Info submenu.
void screen_payment_menu_push() {
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
            lv_obj_set_style_pad_row(row, 4, LV_PART_MAIN);
            lv_obj_set_layout(row, LV_LAYOUT_FLEX);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            _rows[i] = row;

            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, MENU_LABELS[i]);
            lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);

            _status[i] = lv_label_create(row);
            lv_obj_set_style_text_font(_status[i], &lv_font_montserrat_14, LV_PART_MAIN);
        }

        lv_obj_t *legend = ui_legend(_scr);
        lv_obj_set_width(legend, SCREEN_W - 28);
        lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
        char move_lbl[24];
        snprintf(move_lbl, sizeof(move_lbl), "%s%s Move", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
        ui_legend_row(legend, "", lv_color_hex(C_TEXT), move_lbl, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, "Open", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));
    }

    header_set_visible(true);
    header_set_title("PAYMENT INFO");
    refresh_status();
    refresh_cursor();

    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.enter = cb_enter;
    h.back  = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
