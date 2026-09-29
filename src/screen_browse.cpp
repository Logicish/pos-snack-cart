#include "screen_browse.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "items.h"
#include "buttons.h"
#include "idle_timer.h"
#include "session_timer.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the Browse/Price screen declared in screen_browse.h -- the
            scroll-first catalog list, merged 2026-09-15 with what used to be a separate
            scan-first "Price Check" screen. Scrolling and scanning both work here now.
*/

#define FOOTER_H 52
// Reserved above the list for the scan hint/result line -- see _status_lbl. A fixed
// reserved band, not an overlay on top of the list, so scanning never covers up a row of
// real catalog data even momentarily. Bumped 26->34 2026-09-15 after real hardware showed
// the list running a few pixels taller than its actual box -- Page Down's first press was
// spending a few pixels correcting that before advancing a real page. The extra margin
// here also reads better visually (a gap between the hint line and the list, not the list
// starting flush against it).
#define STATUS_H 34

// Deliberately a touch smaller than the visible row count (~8.5 rows fit in the 392px list
// area at this row height) so successive presses always overlap by a row and never skip
// one — same tuning rationale as ATTACH_PAGE in screen_add_item.cpp.
#define BROWSE_PAGE 7

static lv_obj_t *_scr;
static lv_obj_t *_status_lbl;  // scan hint (dim) by default, scan result (green/red) after a scan
static lv_obj_t *_list;
static lv_obj_t *_rows[MAX_ITEMS];
static int        _cursor;  // scroll-into-view anchor only -- Browse is read-only, no highlight

// Only the first MAX_ITEMS rows are ever built (see screen_browse_push()) — cursor math
// must wrap against that same cap, not the raw DB count, or it indexes _rows[] out of
// bounds. Same crash-cause as screen_pos.cpp's Manual Entry list, fixed 2026-08-25.
// include_hidden=false (2026-09-14) -- Browse is the customer-facing catalog view, a
// hidden ("discontinued") item has no business showing up here, see items.h.
static int visible_count() {
    int n = items_count(false);
    return n > MAX_ITEMS ? MAX_ITEMS : n;
}

// 2026-08-28: Browse has nothing to select (Enter does nothing here), so unlike every
// other list screen there's no highlight box to move -- just scroll position. Settled by
// explicit choice over the alternative (keep a cursor highlight): a plain paging viewport.
static void scroll_to_cursor() {
    int n = visible_count();
    if (n == 0) return;
    lv_obj_scroll_to_view(_rows[_cursor], LV_ANIM_OFF);
}

// LEFT/RIGHT page the viewport a screenful at a time -- Browse has no selection, so
// unlike every other list screen these don't step a cursor, they ARE the page keys.
// Both wrap (Right past the last page lands back on the first, Left before the first
// lands on the last). Remapped from Up/Down 2026-09-15 so Browse and the Item Lookup
// screen under Transaction (which added its own Left/Right page-jump the same day) page
// the same way -- Item Lookup still needs Up/Down for its row cursor, so Left/Right is
// the shared convention between the two, not Up/Down.
static void cb_page_right() {
    int n = visible_count();
    if (n == 0) return;
    if (_cursor >= n - 1) {
        _cursor = 0;
    } else {
        _cursor += BROWSE_PAGE;
        if (_cursor >= n) _cursor = n - 1;
    }
    scroll_to_cursor();
}

static void cb_page_left() {
    int n = visible_count();
    if (n == 0) return;
    if (_cursor <= 0) {
        _cursor = n - 1;
    } else {
        _cursor -= BROWSE_PAGE;
        if (_cursor < 0) _cursor = 0;
    }
    scroll_to_cursor();
}

// Back returns to IDLE.
static void cb_back() {
    screen_idle_load();
}

// Resets the status line to its default scan hint (dim, not a result).
static void set_default_status() {
    lv_label_set_text(_status_lbl, "Scan an item for its price.");
    lv_obj_set_style_text_color(_status_lbl, lv_color_hex(C_DIM), LV_PART_MAIN);
}

// Consumes a scan while this screen is active -- shows the scanned item's price (or "not
// recognized") in the status line, without disturbing scroll position or the list itself.
// Merged in 2026-09-15 from what used to be a separate "Price Check" screen -- scanning
// now works on the same screen as scrolling instead of needing a second one.
bool screen_browse_on_scan(const char *upc) {
    if (!_scr || lv_scr_act() != _scr) return false;

    const Item *it = items_find_by_upc(upc);
    char buf[48];
    if (it) {
        // CLIP not WRAP -- a long name + price could overflow one line, and clipping beats
        // silently growing this reserved band taller than STATUS_H (see feedback from the
        // same issue on screen_item_edit.cpp's action-hint line).
        snprintf(buf, sizeof(buf), "%s -- $%d.%02d", it->name, it->price_cents / 100, it->price_cents % 100);
        lv_label_set_text(_status_lbl, buf);
        lv_obj_set_style_text_color(_status_lbl, lv_color_hex(C_GREEN), LV_PART_MAIN);
    } else {
        lv_label_set_text(_status_lbl, "Not recognized -- scan another.");
        lv_obj_set_style_text_color(_status_lbl, lv_color_hex(C_RED), LV_PART_MAIN);
    }
    return true;
}

// Loads Browse/Price, rebuilding the row list from the current catalog.
void screen_browse_push() {
    _cursor = 0;

    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        _status_lbl = lv_label_create(_scr);
        lv_label_set_long_mode(_status_lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(_status_lbl, SCREEN_W - 24);
        lv_obj_align(_status_lbl, LV_ALIGN_TOP_MID, 0, HDR_H + 6);
        lv_obj_set_style_text_font(_status_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

        _list = lv_obj_create(_scr);
        lv_obj_set_size(_list, SCREEN_W, SCREEN_H - HDR_H - STATUS_H - FOOTER_H);
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
        char page_left_lbl[24], page_right_lbl[24];
        snprintf(page_left_lbl,  sizeof(page_left_lbl),  "%s Page Up",   LV_SYMBOL_LEFT);
        snprintf(page_right_lbl, sizeof(page_right_lbl), "%s Page Down", LV_SYMBOL_RIGHT);
        ui_legend_row(legend, page_left_lbl, lv_color_hex(C_YELLOW), page_right_lbl, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, "", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));
    }

    // Rows are rebuilt every visit, not just the first -- items_count() (and any item's
    // name/price) can change between visits via Add/Attach Item or the web Items page,
    // and the old build-once approach kept showing whatever the catalog looked like the
    // first time this screen was ever opened, silently stale for the rest of the device's
    // uptime (same class of bug fixed in screen_inventory.cpp's row cache, just cosmetic
    // here rather than a crash since Browse never re-touches a label after building it).
    lv_obj_clean(_list);
    int n = items_count(false);
    if (n > MAX_ITEMS) n = MAX_ITEMS;
    for (int i = 0; i < n; i++) {
        const Item *it = items_get(i, false);

        lv_obj_t *row = lv_obj_create(_list);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 10, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_layout(row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        _rows[i] = row;

        // it can be null if the underlying query failed -- see screen_inventory.cpp's
        // matching guard for why this isn't just theoretical caution.
        lv_obj_t *name_lbl = lv_label_create(row);
        lv_label_set_text(name_lbl, it ? it->name : "(error loading item)");
        lv_obj_set_style_text_color(name_lbl, it ? lv_color_hex(C_TEXT) : lv_color_hex(C_RED), LV_PART_MAIN);
        lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

        char price_buf[8];
        if (it) snprintf(price_buf, sizeof(price_buf), "$%d.%02d", it->price_cents / 100, it->price_cents % 100);
        else    snprintf(price_buf, sizeof(price_buf), "--");
        lv_obj_t *price_lbl = lv_label_create(row);
        lv_label_set_text(price_lbl, price_buf);
        lv_obj_set_style_text_color(price_lbl, lv_color_hex(C_GREEN), LV_PART_MAIN);
        lv_obj_set_style_text_font(price_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    }

    set_default_status();
    scroll_to_cursor();
    header_set_title("Browse/Price");

    ButtonHandlers h;
    h.left  = cb_page_left;
    h.right = cb_page_right;
    h.back  = cb_back;
    h.wantsScanner = true;  // scan-to-price is this screen's whole other half
    buttons_set_handlers(h);
    idle_timer_arm();
    session_timer_disarm();  // read-only browsing, not a "logged in" session to auto-log-out of

    lv_scr_load(_scr);
}
