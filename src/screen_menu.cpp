#include "screen_menu.h"
#include "screens.h"
#include "screen_inventory_menu.h"
#include "screen_user_menu.h"
#include "screen_webportal.h"
#include "screen_settings.h"
#include "screen_admin_tools.h"
#include "screen_balances.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>

#define MENU_COUNT 6
#define FOOTER_H   52

// Deliberately a touch smaller than the visible row count so successive presses always
// overlap by a row and never skip one — same tuning rationale as ATTACH_PAGE in
// screen_add_item.cpp.
#define MENU_PAGE 4

static lv_obj_t *_scr;
static lv_obj_t *_rows[MENU_COUNT];
static int        _cursor;
static int        _prev_cursor = -1;

// 2026-08-28 reorg — was a flat list of individual actions (Restock/Add-Attach/Inventory
// Count/Add User/Advanced Tools) that was about to overflow once Edit Users landed.
// Regrouped into task-oriented submenus instead of shrinking row size: Inventory and
// Users each wrap what used to be top-level entries, Settings and Balances are new. No
// "Make Transaction" entry — an admin who wants to buy something just scans their badge
// at the Start screen (on_scan() routes any known active user, admin included, straight
// to their own cart, 2026-08-26). Admin Login (button + scan) is only for reaching this
// menu. Ordered most-to-least used per explicit direction (Balances first, Advanced
// Tools last); "Inventory Management"/"User Management" shortened to "Inventory"/"Users"
// same day — the longer labels were crowding the header in their own sub-menus.
static const char *MENU_LABELS[MENU_COUNT] = {
    "1. Balances",
    "2. Inventory",
    "3. Users",
    "4. Web Portal",
    "5. Settings",
    "6. Advanced Tools",
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
        _cursor += MENU_PAGE;
        if (_cursor >= MENU_COUNT) _cursor = MENU_COUNT - 1;
    }
    refresh_cursor();
}

// Admin Menu's Back always logs out fully — this is the top level of admin navigation.
// Advanced Tools (one level down) backs up to here instead; see screen_admin_tools.cpp.
static void cb_logout() {
    screen_idle_load();
}

static void cb_enter() {
    switch (_cursor) {
        case 0: screen_balances_push();       break;
        case 1: screen_inventory_menu_push(); break;
        case 2: screen_user_menu_push();      break;
        case 3: screen_webportal_push();      break;
        case 4: screen_settings_push();       break;
        case 5: screen_admin_tools_push();    break;
    }
}

void screen_menu_push() {
    _cursor = 0;
    // _prev_cursor is NOT reset here — refresh_cursor() needs to remember which row was
    // lit on the previous visit so it can clear it, not just light row 0 on top of it.

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
        ui_legend_row(legend, "Open", lv_color_hex(C_GREEN), "Log Out", lv_color_hex(C_RED));
    }

    header_set_visible(true);
    header_set_title("ADMIN MENU");
    refresh_cursor();

    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.left  = cb_page_down;
    h.enter = cb_enter;
    h.back  = cb_logout;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
