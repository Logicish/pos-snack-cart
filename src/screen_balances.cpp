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

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the Balances screen declared in screen_balances.h.
*/

#define FOOTER_H 52

// Deliberately a touch smaller than the visible row count so successive presses always
// overlap by a row and never skip one — same tuning rationale as ATTACH_PAGE in
// screen_add_item.cpp.
#define BALANCES_PAGE 7

static lv_obj_t *_scr;
static lv_obj_t *_list;
static lv_obj_t *_legend;
static bool       _confirming;  // true while the "Confirm balance clear?" footer is up
static lv_obj_t *_rows[MAX_BALANCES];
static OutstandingCheckout _data[MAX_BALANCES];
static int        _count;
static int        _cursor;
static int        _prev_cursor = -1;

// Formats one checkout as "#id First L.  $total  MM/DD".
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

// Builds one list row for _data[i].
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

// Highlights the currently-selected row.
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

static void cb_up();
static void cb_down();
static void cb_page_left();
static void cb_page_right();
static void cb_clear_start();
static void cb_confirm_yes();
static void cb_confirm_no();
static void cb_back();

// Rebuilds the footer for normal browsing mode.
static void build_normal_legend() {
    lv_obj_clean(_legend);
    char move_lbl[24], page_lbl[24];
    snprintf(move_lbl, sizeof(move_lbl), "Move %s%s", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
    snprintf(page_lbl, sizeof(page_lbl), "Page %s%s", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
    ui_legend_row(_legend, move_lbl, lv_color_hex(C_YELLOW), page_lbl, lv_color_hex(C_YELLOW));
    ui_legend_row(_legend, "Clear", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));
}

// Rebuilds the footer for the Clear confirm step.
static void build_confirm_legend() {
    lv_obj_clean(_legend);
    ui_legend_row(_legend, "", lv_color_hex(C_TEXT), "Confirm balance clear", lv_color_hex(C_ORANGE));
    ui_legend_row(_legend, "Yes", lv_color_hex(C_GREEN), "No", lv_color_hex(C_RED));
}

// Restores normal browsing mode -- footer + button handlers -- shared by both confirm outcomes.
static void restore_normal_handlers() {
    _confirming = false;
    build_normal_legend();

    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.left  = cb_page_left;
    h.right = cb_page_right;
    h.enter = cb_clear_start;
    h.back  = cb_back;
    buttons_set_handlers(h);
}

// Up: moves the selection to the previous row, wrapping.
static void cb_up() {
    if (_count == 0) return;
    _cursor = (_cursor - 1 + _count) % _count;
    refresh_cursor();
}

// Down: moves the selection to the next row, wrapping.
static void cb_down() {
    if (_count == 0) return;
    _cursor = (_cursor + 1) % _count;
    refresh_cursor();
}

// LEFT: jump ~a screenful back up the list, wrapping to the last row. RIGHT does the
// same forward. Remapped from a Left-only "Next Screen" accelerator 2026-09-15 to match
// Browse/Price and Item Lookup's Left/Right paging convention.
static void cb_page_left() {
    if (_count == 0) return;
    if (_cursor <= 0) {
        _cursor = _count - 1;
    } else {
        _cursor -= BALANCES_PAGE;
        if (_cursor < 0) _cursor = 0;
    }
    refresh_cursor();
}

static void cb_page_right() {
    if (_count == 0) return;
    if (_cursor >= _count - 1) {
        _cursor = 0;
    } else {
        _cursor += BALANCES_PAGE;
        if (_cursor >= _count) _cursor = _count - 1;
    }
    refresh_cursor();
}

// GREEN while browsing normally: arms the Clear confirm step instead of clearing right
// away. Added 2026-09-15 -- Clear is UPDATE-only, never a delete (see checkouts.h), so
// it was never destructive of real history, but a stray Enter press could still drop a
// real outstanding balance off the list with no warning, which is worth a confirm on its
// own merits.
static void cb_clear_start() {
    if (_count == 0) return;
    _confirming = true;
    build_confirm_legend();

    ButtonHandlers h;
    h.enter = cb_confirm_yes;
    h.back  = cb_confirm_no;
    buttons_set_handlers(h);
}

// GREEN during confirm: actually clears the highlighted transaction.
static void cb_confirm_yes() {
    checkouts_clear(_data[_cursor].id);
    rebuild_rows();
    restore_normal_handlers();
}

// RED during confirm: cancels, nothing cleared.
static void cb_confirm_no() {
    restore_normal_handlers();
}

// Back returns to the main Admin Menu.
static void cb_back() {
    screen_menu_push();
}

// Loads the Balances screen.
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
        lv_obj_set_style_pad_hor(_list, 12, LV_PART_MAIN);  // 12px side inset, same as every screen
        lv_obj_set_style_pad_row(_list, 6, LV_PART_MAIN);
        lv_obj_set_layout(_list, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(_list, LV_FLEX_FLOW_COLUMN);

        _legend = ui_legend(_scr);
        lv_obj_set_width(_legend, SCREEN_W - 24);
        lv_obj_align(_legend, LV_ALIGN_BOTTOM_MID, 0, -6);
        build_normal_legend();
    }

    header_set_visible(true);
    header_set_title("BALANCES");
    rebuild_rows();  // data changes visit to visit (and after every Clear), always re-query

    // Always land back in normal (not confirm) mode -- a fresh visit shouldn't be able to
    // arrive mid-confirm from a previous visit's state.
    restore_normal_handlers();

    lv_scr_load(_scr);
}
