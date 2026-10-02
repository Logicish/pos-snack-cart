#include "screen_extras.h"
#include "screens.h"
#include "screen_blocked.h"
#include "screen_check_balance.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "ui.h"
#include "users.h"
#include <lvgl.h>
#include <Arduino.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the Extras screen declared in screen_extras.h -- a badge-scan
            gate (_scan_scr) followed by the actual menu (_list_scr). Two separate
            top-level screens rather than one shared _content, since which screen is
            currently loaded already tells screen_extras_on_scan() everything it needs
            to know -- no separate state enum required.
*/

#define FOOTER_H   52
#define MENU_COUNT 3

static lv_obj_t *_scan_scr;
static lv_obj_t *_list_scr;
static lv_obj_t *_rows[MENU_COUNT];
static int        _cursor;
static int        _prev_cursor = -1;
static int         _user_id = -1;

static const char *MENU_LABELS[MENU_COUNT] = {
    "1. Check Balance",
    "2. Laggy Fish",
    "3. Slide Free",
};

static void build_scan_ui();
static void build_list_ui();
static void show_list();

// The currently-identified Extras user, or -1 if nothing's been scanned in this session.
int screen_extras_current_user_id() {
    return _user_id;
}

// Highlights the currently-selected menu row.
static void refresh_cursor() {
    if (_prev_cursor >= 0 && _prev_cursor != _cursor) {
        lv_obj_set_style_bg_opa(_rows[_prev_cursor], LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(_rows[_cursor], LV_OPA_30, LV_PART_MAIN);
    _prev_cursor = _cursor;
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

// Back from either screen returns straight to IDLE -- Extras is reached directly from the
// Start screen, not from a submenu parent, so there's no "up one level" to go to first.
static void cb_back() {
    screen_idle_load();
}

// Enter opens whichever screen the selected row names.
static void cb_enter() {
    switch (_cursor) {
        case 0: screen_check_balance_push();  break;
        case 1: screen_laggy_fish_push();     break;
        case 2: screen_slide_free_push();     break;
    }
}

// Consumes a scan while the badge gate is up -- looks up the badge and either proceeds
// into the menu (known, active user) or turns it away, same as every other badge-scan
// gate in this codebase (Admin Login, Add User, etc).
bool screen_extras_on_scan(const char *badge_id) {
    if (!_scan_scr || lv_scr_act() != _scan_scr) return false;

    const User *u = users_find_by_badge(badge_id);
    if (!u) {
        screen_blocked_push("Badge not recognized.\n\nPlease see an admin to\nget enrolled.");
        return true;
    }
    if (!u->active) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Sorry %s,\nplease see the cart\nowner to continue.", u->first_name);
        screen_blocked_push(msg);
        return true;
    }

    _user_id = u->id;
    header_set_current_user(u->first_name);
    show_list();
    return true;
}

// Builds the badge-gate screen once.
static void build_scan_ui() {
    _scan_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_scan_scr, lv_color_hex(C_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_scan_scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *lbl = lv_label_create(_scan_scr);
    lv_label_set_text(lbl, "Scan your badge\nfor Extras.");
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, 260);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

    ui_footer_cancel(_scan_scr);
}

// Builds the menu screen once.
static void build_list_ui() {
    _list_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_list_scr, lv_color_hex(C_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_list_scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *list = lv_obj_create(_list_scr);
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
    }

    lv_obj_t *legend = ui_legend(_list_scr);
    lv_obj_set_width(legend, SCREEN_W - 24);
    lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
    char move_lbl[24];
    snprintf(move_lbl, sizeof(move_lbl), "%s%s Move", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), move_lbl, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "Open", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));
}

// Shared by the fresh-scan path and screen_extras_return_to_list() -- (re)loads the menu
// screen for whichever user_id is already identified.
static void show_list() {
    _cursor = 0;

    // Clear any stale highlight left over from before this screen was last backed out
    // of. Resetting _prev_cursor to -1 below makes refresh_cursor() skip clearing
    // whichever row was lit when we left (its "nothing to clear yet" branch, meant for
    // the very first build) -- without this, that row and the new cursor's row (0) both
    // ended up highlighted at once on a return visit. Only meaningful once the rows
    // actually exist, hence the _list_scr guard.
    if (_list_scr) {
        for (int i = 0; i < MENU_COUNT; i++) {
            lv_obj_set_style_bg_opa(_rows[i], LV_OPA_TRANSP, LV_PART_MAIN);
        }
    }
    _prev_cursor = -1;
    if (!_list_scr) build_list_ui();

    header_set_visible(true);
    header_set_title("EXTRAS");
    refresh_cursor();

    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.enter = cb_enter;
    h.back  = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_list_scr);
}

// Re-shows the already-identified menu directly -- the Back target for Check Balance and
// Laggy Fish, which shouldn't force a re-scan just to return to this list.
void screen_extras_return_to_list() {
    show_list();
}

// Loads the badge gate, always starting over -- entering fresh from IDLE never trusts a
// stale identity from a previous visit.
void screen_extras_push() {
    _user_id = -1;
    if (!_scan_scr) build_scan_ui();

    header_set_visible(true);
    header_set_title("EXTRAS");

    ButtonHandlers h;
    h.back = cb_back;
    h.wantsScanner = true;  // waiting for a badge to identify who's using Extras
    buttons_set_handlers(h);

    lv_scr_load(_scan_scr);
}
