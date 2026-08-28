// "Advanced Tools" sub-menu, 2026-08-26 — split out of the main Admin Menu
// (screen_menu.cpp) to keep that list under 10 everyday-use items, per explicit
// direction: "soft guard the tech stuff in a sub menu." Screensaver moved out to the
// Settings submenu 2026-08-28 (Admin Menu reorg) — it's a normal setting, not a
// technical/builder-only tool like the rest of this list.
//
// Restructured again same day: Reset DB was deleted outright (real risk of an accidental
// press wiping the DB, not worth keeping reachable at all); a new DB sub-menu
// (screen_db_menu.cpp/screen_db_view.cpp) was added for read-only table inspection;
// GM65 Test's on-screen name shortened to "Scanner" (the file/function names stay
// GM65-specific, only the label changed); a DS3231 Test screen was added once the RTC
// landed.
//
// Setup SD (and the "Reset Tools" sub-menu built to house it) removed entirely the same
// day, once every real provisioning path it existed to bootstrap actually existed
// on-device: it hardcoded real names/badge numbers/a Venmo handle as C++ string literals
// purely because there was no other way to get people/payment info onto a fresh device
// (no USB mass storage, no web upload) — see project memory feedback-no-pii-in-firmware.
// Add User + Edit Users (real enrollment) and the Venmo Payment Info editor (Settings)
// now cover everything it did, with zero PII in source. Its own code comment said to
// delete it "once a real provisioning path exists for all of this" — that's now true.
#include "screen_admin_tools.h"
#include "screens.h"
#include "screen_sdinfo.h"
#include "screen_db_menu.h"
#include "screen_ds3231_test.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>

#define MENU_COUNT 4
#define FOOTER_H   52

// Deliberately a touch smaller than the visible row count so successive presses always
// overlap by a row and never skip one — same tuning rationale as ATTACH_PAGE in
// screen_add_item.cpp.
#define TOOLS_PAGE 4

static lv_obj_t *_scr;
static lv_obj_t *_rows[MENU_COUNT];
static int        _cursor;
static int        _prev_cursor = -1;

static const char *MENU_LABELS[MENU_COUNT] = {
    "1. SD Info",
    "2. DB",
    "3. Scanner",
    "4. DS3231 Test",
};

static void refresh_cursor() {
    if (_prev_cursor >= 0 && _prev_cursor != _cursor) {
        lv_obj_set_style_bg_opa(_rows[_prev_cursor], LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(_rows[_cursor], LV_OPA_30, LV_PART_MAIN);
    _prev_cursor = _cursor;
    lv_obj_scroll_to_view(_rows[_cursor], LV_ANIM_OFF);
}

static void cb_up() {
    _cursor = (_cursor - 1 + MENU_COUNT) % MENU_COUNT;
    refresh_cursor();
}

static void cb_down() {
    _cursor = (_cursor + 1) % MENU_COUNT;
    refresh_cursor();
}

// LEFT: jump ~a screenful down the list, same wrap-at-the-end behavior as
// screen_add_item.cpp's ATTACH_PAGE/cb_attach_pagedown.
static void cb_page_down() {
    if (_cursor >= MENU_COUNT - 1) {
        _cursor = 0;
    } else {
        _cursor += TOOLS_PAGE;
        if (_cursor >= MENU_COUNT) _cursor = MENU_COUNT - 1;
    }
    refresh_cursor();
}

static void cb_back() {
    screen_menu_push();  // up one level to the main Admin Menu, not a full logout
}

static void cb_enter() {
    switch (_cursor) {
        case 0: screen_sdinfo_push();      break;
        case 1: screen_db_menu_push();     break;
        case 2: screen_gm65_test_push();   break;
        case 3: screen_ds3231_test_push(); break;
    }
}

void screen_admin_tools_push() {
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
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            _rows[i] = row;

            lv_obj_t *lbl = lv_label_create(row);
            lv_label_set_text(lbl, MENU_LABELS[i]);
            lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
        }

        lv_obj_t *legend = ui_legend(_scr);
        lv_obj_set_width(legend, SCREEN_W - 28);
        lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
        char left_arrow[24], move_lbl[24];
        snprintf(left_arrow, sizeof(left_arrow), "%s Next Screen", LV_SYMBOL_LEFT);
        snprintf(move_lbl, sizeof(move_lbl), "%s%s Move", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
        ui_legend_row(legend, left_arrow, lv_color_hex(C_YELLOW), move_lbl, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, "Open", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));
    }

    header_set_visible(true);
    header_set_title("ADVANCED TOOLS");
    refresh_cursor();

    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.left  = cb_page_down;
    h.enter = cb_enter;
    h.back  = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
