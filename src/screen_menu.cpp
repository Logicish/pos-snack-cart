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
#include "system_alerts.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the Admin Menu declared in screen_menu.h -- six rows plus a
            conditional device-health alert banner.
*/

#define MENU_COUNT 6
#define FOOTER_H   52
// Alert banner height -- only takes real screen space when a warning is actually active
// (see refresh_alert_banner() below, which resizes/repositions the list around it each
// time this screen loads), so the common no-warnings case looks exactly like it always
// has rather than permanently losing a row's worth of space to an empty strip.
#define BANNER_H   26

// Deliberately a touch smaller than the visible row count so successive presses always
// overlap by a row and never skip one — same tuning rationale as ATTACH_PAGE in
// screen_add_item.cpp.
#define MENU_PAGE 4

static lv_obj_t *_scr;
static lv_obj_t *_list;
static lv_obj_t *_banner;
static lv_obj_t *_banner_lbl;
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

// LEFT/RIGHT jump ~a screenful at a time, both directions wrapping -- remapped
// 2026-09-15 from a Left-only forward accelerator to match Browse/Price, Item Lookup,
// Balances, and Inventory's paging convention.
static void cb_page_left() {
    if (_cursor <= 0) {
        _cursor = MENU_COUNT - 1;
    } else {
        _cursor -= MENU_PAGE;
        if (_cursor < 0) _cursor = 0;
    }
    refresh_cursor();
}

static void cb_page_right() {
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

// Enter opens whichever submenu/screen the selected row names.
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

// Recomputes alert state and shows/hides the banner accordingly, resizing the list to
// fill whatever's left either way -- called fresh every time this screen loads, not just
// on first build, so a warning that appeared (or got fixed) since the last visit is
// reflected immediately rather than needing a reboot to notice.
static void refresh_alert_banner() {
    system_alerts_refresh();

    if (system_alerts_active()) {
        lv_label_set_text(_banner_lbl, system_alerts_message());
        lv_obj_clear_flag(_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(_list, SCREEN_W, SCREEN_H - HDR_H - BANNER_H - FOOTER_H);
        lv_obj_align(_list, LV_ALIGN_BOTTOM_MID, 0, -FOOTER_H);
    } else {
        lv_obj_add_flag(_banner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(_list, SCREEN_W, SCREEN_H - HDR_H - FOOTER_H);
        lv_obj_align(_list, LV_ALIGN_BOTTOM_MID, 0, -FOOTER_H);
    }
}

// Loads the Admin Menu.
void screen_menu_push() {
    _cursor = 0;
    // _prev_cursor is NOT reset here — refresh_cursor() needs to remember which row was
    // lit on the previous visit so it can clear it, not just light row 0 on top of it.

    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        // Yellow alert banner, hidden by default -- refresh_alert_banner() shows/sizes it
        // (and resizes _list around it) fresh every time this screen loads. Dark text on
        // the yellow fill for contrast, matching this project's own button-color legend
        // convention elsewhere (colored fill/text, not colored-on-dark like everything else).
        _banner = lv_obj_create(_scr);
        lv_obj_set_size(_banner, SCREEN_W, BANNER_H);
        lv_obj_align(_banner, LV_ALIGN_TOP_MID, 0, HDR_H);
        lv_obj_set_style_bg_color(_banner, lv_color_hex(C_YELLOW), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_banner, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(_banner, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(_banner, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(_banner, 4, LV_PART_MAIN);
        lv_obj_clear_flag(_banner, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(_banner, LV_OBJ_FLAG_HIDDEN);

        _banner_lbl = lv_label_create(_banner);
        lv_label_set_long_mode(_banner_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(_banner_lbl, SCREEN_W - 8);
        lv_obj_set_style_text_color(_banner_lbl, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_text_font(_banner_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_center(_banner_lbl);

        _list = lv_obj_create(_scr);
        lv_obj_set_size(_list, SCREEN_W, SCREEN_H - HDR_H - FOOTER_H);
        lv_obj_align(_list, LV_ALIGN_BOTTOM_MID, 0, -FOOTER_H);
        lv_obj_set_style_bg_opa(_list, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(_list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(_list, 14, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(_list, 12, LV_PART_MAIN);  // 12px side inset, same as every screen
        lv_obj_set_style_pad_row(_list, 10, LV_PART_MAIN);
        lv_obj_set_layout(_list, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);

        for (int i = 0; i < MENU_COUNT; i++) {
            lv_obj_t *row = lv_obj_create(_list);
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
        lv_obj_set_width(legend, SCREEN_W - 24);
        lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
        char move_lbl[24], page_lbl[24];
        snprintf(move_lbl, sizeof(move_lbl), "Move %s%s", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
        snprintf(page_lbl, sizeof(page_lbl), "Page %s%s", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
        ui_legend_row(legend, move_lbl, lv_color_hex(C_YELLOW), page_lbl, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, "Open", lv_color_hex(C_GREEN), "Log Out", lv_color_hex(C_RED));
    }

    header_set_visible(true);
    header_set_title("ADMIN MENU");
    refresh_alert_banner();
    refresh_cursor();

    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.left  = cb_page_left;
    h.right = cb_page_right;
    h.enter = cb_enter;
    h.back  = cb_logout;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
