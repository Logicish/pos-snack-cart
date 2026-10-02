#include "screen_item_edit.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "items.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>
#include <string.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the shared item view/edit screen declared in
            screen_item_edit.h -- Price/Qty adjust, linked-UPC list with per-UPC
            unlink, a Hide/Unhide toggle, and a confirm-gated Delete Item action.
  Notes---- Row cursor covers three fixed rows (Price/Qty+-1/Qty+-10) followed by
            one row per linked UPC, followed by a Hide/Unhide Item row, followed by
            one final "Delete Item" row -- the total count varies with how many UPCs
            are linked, unlike every other fixed-row list screen in this codebase.
*/

// Real prices in the catalog are all nickel multiples ($0.05 steps), so that's the
// adjustment granularity here too.
#define PRICE_STEP_CENTS 5

enum { ROW_PRICE, ROW_QTY1, ROW_QTY10, ROW_FIXED_COUNT };

static lv_obj_t *_scr;
static lv_obj_t *_name_lbl;
static lv_obj_t *_price_row, *_price_val_lbl;
static lv_obj_t *_qty1_row;
static lv_obj_t *_qty10_row;
static lv_obj_t *_upc_header_lbl;
static lv_obj_t *_upc_list;
static lv_obj_t *_action_hint_lbl;  // Enter's current meaning, if any -- left side of the Enter/Back legend row
static lv_obj_t *_msg_lbl;

// UPC rows live inside _upc_list, rebuilt by refresh_upcs() -- _upc_values[] mirrors
// _upc_rows[] so cb_enter() knows which literal UPC string to unlink for a given row.
static lv_obj_t *_upc_rows[MAX_UPCS_PER_ITEM];
static char       _upc_values[MAX_UPCS_PER_ITEM][UPC_LEN];
static int         _upc_count;
static lv_obj_t *_hide_row;    // also inside _upc_list, second-to-last child
static lv_obj_t *_delete_row;  // also inside _upc_list, always the last child

static void (*_return_cb)();
static int  _item_id;
static int  _price_cents;
static int  _stocked;
static bool _item_hidden;
static int  _row_cursor;

// fwd decls -- the confirm screen and the main row handlers each need to hand control
// back to the other (Yes/No restore the main screen's handlers; Enter on the Delete Item
// row opens the confirm screen), so neither side can be fully defined before the other.
static void cb_row_prev();
static void cb_row_next();
static void cb_adjust_down();
static void cb_adjust_up();
static void cb_enter();
static void cb_back();
static void refresh_row_highlight();

// Returns how many selectable rows currently exist: 3 fixed + one per linked UPC +
// Hide/Unhide Item + Delete Item.
static int total_rows() { return ROW_FIXED_COUNT + _upc_count + 2; }

// Maps a row index to its LVGL row object, across all four row "kinds."
static lv_obj_t *row_at(int idx) {
    if (idx == ROW_PRICE) return _price_row;
    if (idx == ROW_QTY1)  return _qty1_row;
    if (idx == ROW_QTY10) return _qty10_row;
    int rest = idx - ROW_FIXED_COUNT;
    if (rest < _upc_count) return _upc_rows[rest];
    if (rest == _upc_count) return _hide_row;
    return _delete_row;
}

// Formats cents as "$X.XX" (or "-$X.XX" if negative).
static void format_price(int cents, char *out, size_t out_len) {
    bool neg = cents < 0;
    int c = neg ? -cents : cents;
    snprintf(out, out_len, "%s$%d.%02d", neg ? "-" : "", c / 100, c % 100);
}

// Sets (or clears, if null) the status message line.
static void set_msg(const char *text) {
    lv_label_set_text(_msg_lbl, text ? text : "");
}

// Redraws the item name/quantity header and the price value. Also refreshes _item_hidden
// from the DB -- the source of truth for the Hide/Unhide row's label and the [HIDDEN] tag,
// so every caller that changes hidden state must call this (or refresh_upcs() will build
// the toggle row with a stale label).
static void refresh_header() {
    const Item *it = items_get_by_id(_item_id);
    char buf[80];
    if (it) {
        _item_hidden = it->hidden;
        snprintf(buf, sizeof(buf), _item_hidden ? "%s   x%d   [HIDDEN]" : "%s   x%d", it->name, _stocked);
    } else {
        _item_hidden = false;
        snprintf(buf, sizeof(buf), "(item not found)");
    }
    lv_label_set_text(_name_lbl, buf);

    char price_buf[16];
    format_price(_price_cents, price_buf, sizeof(price_buf));
    lv_label_set_text(_price_val_lbl, price_buf);
}

// Builds one selectable row inside _upc_list -- a UPC entry, the Hide/Unhide Item row, or
// the trailing Delete Item row. Same visual shape as make_row() below, just without a hint column.
static lv_obj_t *make_list_row(lv_obj_t *parent, const char *text, lv_color_t text_color) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CYAN), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, text_color, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);

    return row;
}

// Rebuilds the linked-UPC rows plus the trailing Hide/Unhide + Delete Item rows. Called
// after every link/unlink/hide-toggle so the row count (and therefore total_rows()) and
// the Hide/Unhide row's label always match reality. Relies on _item_hidden already being
// current -- callers must refresh_header() first if hidden state may have just changed.
static void refresh_upcs() {
    lv_obj_clean(_upc_list);
    memset(_upc_rows, 0, sizeof(_upc_rows));

    _upc_count = items_get_upcs(_item_id, _upc_values, MAX_UPCS_PER_ITEM);

    if (_upc_count == 0) {
        lv_obj_t *lbl = lv_label_create(_upc_list);
        lv_label_set_text(lbl, "(none yet -- scan to link one)");
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_DIM), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    } else {
        for (int i = 0; i < _upc_count; i++) {
            _upc_rows[i] = make_list_row(_upc_list, _upc_values[i], lv_color_hex(C_TEXT));
        }
    }

    _hide_row = make_list_row(_upc_list, _item_hidden ? "Unhide Item" : "Hide Item", lv_color_hex(C_ORANGE));
    _delete_row = make_list_row(_upc_list, "Delete Item", lv_color_hex(C_RED));
}

// Redraws the left side of the Enter/Back legend row, describing what Enter currently
// does, if anything. No "Enter:" prefix -- it's already drawn in Enter's own green. Also
// no separate line for this (an earlier version used one) -- that made the footer three
// lines tall instead of the usual two, so this shares the same row as Back instead.
static void update_action_hint() {
    int hide_idx = ROW_FIXED_COUNT + _upc_count;
    if (_row_cursor < ROW_FIXED_COUNT) {
        lv_label_set_text(_action_hint_lbl, "");
    } else if (_row_cursor < hide_idx) {
        lv_label_set_text(_action_hint_lbl, "Unlink UPC");
    } else if (_row_cursor == hide_idx) {
        lv_label_set_text(_action_hint_lbl, _item_hidden ? "Unhide Item" : "Hide Item");
    } else {
        lv_label_set_text(_action_hint_lbl, "Delete Item");
    }
}

// Highlights the currently-selected row, across all four row kinds.
static void refresh_row_highlight() {
    int n = total_rows();
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = row_at(i);
        if (row) lv_obj_set_style_bg_opa(row, i == _row_cursor ? LV_OPA_30 : LV_OPA_TRANSP, LV_PART_MAIN);
    }
    update_action_hint();
}

// Writes the current price/stock to the DB and redraws the header.
static void save_and_refresh() {
    items_update(_item_id, _price_cents, _stocked);
    refresh_header();
}

// Up: moves the row cursor up, wrapping across all current rows.
static void cb_row_prev() {
    int n = total_rows();
    _row_cursor = (_row_cursor - 1 + n) % n;
    refresh_row_highlight();
}

// Down: moves the row cursor down, wrapping across all current rows.
static void cb_row_next() {
    int n = total_rows();
    _row_cursor = (_row_cursor + 1) % n;
    refresh_row_highlight();
}

// Left/Right adjust the selected row's value -- swapped 2026-08-25 from an initial
// Up/Down-adjusts/Left-Right-navigates layout, to match this screen's own navigation
// convention with the rest of the codebase (Up/Down = move, Left/Right = act). Only
// meaningful on the three fixed rows -- a no-op on a UPC/Delete Item row, since those
// have no adjustable value, just an Enter action (see cb_enter() below). Note Left/Right
// don't auto-repeat on hold the way Up/Down do (see buttons.cpp) -- cranking a value
// through many steps now takes individual presses.
static void cb_adjust_down() {
    if (_row_cursor >= ROW_FIXED_COUNT) return;
    set_msg(nullptr);
    switch (_row_cursor) {
        // Price shouldn't go negative -- a real dollar amount below zero makes no sense.
        // Stock deliberately CAN go negative (see items.h) -- that's a different, intentional
        // signal, not a bug, so it's not clamped here.
        case ROW_PRICE: if (_price_cents >= PRICE_STEP_CENTS) _price_cents -= PRICE_STEP_CENTS; break;
        case ROW_QTY1:  _stocked -= 1;  break;
        case ROW_QTY10: _stocked -= 10; break;
    }
    save_and_refresh();
}

static void cb_adjust_up() {
    if (_row_cursor >= ROW_FIXED_COUNT) return;
    set_msg(nullptr);
    switch (_row_cursor) {
        case ROW_PRICE: _price_cents += PRICE_STEP_CENTS; break;
        case ROW_QTY1:  _stocked += 1;  break;
        case ROW_QTY10: _stocked += 10; break;
    }
    save_and_refresh();
}

// Re-registers this screen's normal button handlers -- shared by the main view and both
// delete-confirm outcomes, since all three need to land back in the same state.
static void restore_item_handlers() {
    header_set_title("ITEM");
    ButtonHandlers h;
    h.up    = cb_row_prev;
    h.down  = cb_row_next;
    h.left  = cb_adjust_down;
    h.right = cb_adjust_up;
    h.enter = cb_enter;
    h.back  = cb_back;
    h.wantsScanner = true;  // scanning another UPC here links it to this item
    buttons_set_handlers(h);
}

// ============ Delete Item confirm ============

static lv_obj_t *_confirm_scr;
static lv_obj_t *_confirm_lbl;
static void build_delete_confirm_ui();

// Enter on the confirm screen: deletes the item (and its UPC links) if nothing blocks
// it, then leaves this screen entirely -- there's nothing left here to show. If real
// checkout history blocks the delete (foreign_keys=ON, see items_delete()'s comment),
// stays on the item view with an explanatory message instead of pretending it worked.
static void cb_delete_confirm_yes() {
    bool ok = items_delete(_item_id);
    if (ok) {
        if (_return_cb) _return_cb();
        return;
    }
    lv_scr_load(_scr);
    restore_item_handlers();
    set_msg("Can't delete -- this item has\nreal checkout history.");
    refresh_row_highlight();
}

// Back on the confirm screen: cancels, returns to the item view unchanged.
static void cb_delete_confirm_no() {
    lv_scr_load(_scr);
    restore_item_handlers();
    refresh_row_highlight();
}

// Enter on the Delete Item row: opens the confirm screen.
static void open_delete_confirm() {
    if (!_confirm_scr) build_delete_confirm_ui();

    const Item *it = items_get_by_id(_item_id);
    char buf[128];
    snprintf(buf, sizeof(buf),
        "Delete \"%s\"?\n\n"
        "This cannot be undone. Blocked\n"
        "automatically if it has real\n"
        "checkout history.",
        it ? it->name : "this item");
    lv_label_set_text(_confirm_lbl, buf);

    header_set_title("CONFIRM");
    ButtonHandlers h;
    h.enter = cb_delete_confirm_yes;
    h.back  = cb_delete_confirm_no;
    buttons_set_handlers(h);

    lv_scr_load(_confirm_scr);
}

// Builds the Delete Item confirm screen's content, once.
static void build_delete_confirm_ui() {
    _confirm_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(_confirm_scr, lv_color_hex(C_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(_confirm_scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *content = lv_obj_create(_confirm_scr);
    lv_obj_set_size(content, SCREEN_W, SCREEN_H - HDR_H);
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(content, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(content, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(content, 10, LV_PART_MAIN);
    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    _confirm_lbl = lv_label_create(content);
    lv_label_set_long_mode(_confirm_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_confirm_lbl, LV_PCT(100));
    lv_obj_set_style_text_color(_confirm_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(_confirm_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

    lv_obj_t *grow = lv_obj_create(content);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);

    lv_obj_t *legend = ui_legend(content);
    ui_legend_row(legend, "Confirm", lv_color_hex(C_GREEN), "Cancel", lv_color_hex(C_RED));
}

// ============ back on the main screen ============

// Enter: unlinks the selected UPC, toggles Hide/Unhide, or opens the Delete Item confirm --
// a no-op on the three fixed (Price/Qty) rows, which have nothing for Enter to do.
static void cb_enter() {
    if (_row_cursor < ROW_FIXED_COUNT) return;

    int upc_idx = _row_cursor - ROW_FIXED_COUNT;
    if (upc_idx < _upc_count) {
        item_upcs_unlink(_item_id, _upc_values[upc_idx]);
        set_msg("Unlinked.");
        refresh_upcs();
        if (_row_cursor >= total_rows()) _row_cursor = total_rows() - 1;
        refresh_row_highlight();
        return;
    }

    if (_row_cursor == ROW_FIXED_COUNT + _upc_count) {
        bool new_hidden = !_item_hidden;
        items_set_hidden(_item_id, new_hidden);
        set_msg(new_hidden
            ? "Hidden -- won't show in Browse\nor be found by scanning."
            : "Unhidden.");
        refresh_header();  // updates _item_hidden + the [HIDDEN] tag
        refresh_upcs();     // rebuilds the Hide/Unhide row's label from the new state
        refresh_row_highlight();
        return;
    }

    open_delete_confirm();
}

// Back clears the status message and returns to whichever screen opened this one.
static void cb_back() {
    set_msg(nullptr);
    if (_return_cb) _return_cb();
}

// value_lbl_out is optional (nullptr for the two quantity rows, which don't need their own
// value display since the live count is already shown in the header) -- when given, the
// value label is created between the row label and the hint, matching the mockup's layout.
static lv_obj_t *make_row(lv_obj_t *parent, const char *label, const char *hint, lv_obj_t **value_lbl_out) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 10, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CYAN), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);

    if (value_lbl_out) {
        lv_obj_t *val_lbl = lv_label_create(row);
        lv_obj_set_style_text_color(val_lbl, lv_color_hex(C_GREEN), LV_PART_MAIN);
        lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        *value_lbl_out = val_lbl;
    }

    lv_obj_t *hint_lbl = lv_label_create(row);
    lv_label_set_text(hint_lbl, hint);
    lv_obj_set_style_text_color(hint_lbl, lv_color_hex(C_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint_lbl, &lv_font_montserrat_14, LV_PART_MAIN);

    return row;
}

// Consumes a scan while this screen is active -- links the UPC to the item being viewed.
bool screen_item_edit_on_scan(const char *upc) {
    if (!_scr || lv_scr_act() != _scr) return false;

    ItemUpcLinkResult r = item_upcs_link(_item_id, upc);
    switch (r) {
        case ITEM_UPC_LINKED:
            set_msg("Linked.");
            refresh_upcs();
            refresh_row_highlight();
            break;
        case ITEM_UPC_ALREADY_HERE:
            set_msg("Already linked to this item.");
            break;
        case ITEM_UPC_COLLISION: {
            const Item *other = items_find_by_upc(upc);
            char buf[64];
            snprintf(buf, sizeof(buf), "Already linked to: %s", other ? other->name : "another item");
            set_msg(buf);
            break;
        }
        case ITEM_UPC_ERROR:
            set_msg("Scan failed -- try again.");
            break;
    }
    return true;
}

// Loads the item view/edit screen for item_id; on_back is called when Back is pressed.
void screen_item_edit_push(int item_id, void (*on_back)()) {
    _item_id    = item_id;
    _return_cb  = on_back;
    _row_cursor = ROW_PRICE;

    const Item *it = items_get_by_id(item_id);
    _price_cents = it ? it->price_cents : 0;
    _stocked     = it ? it->stocked     : 0;

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
        lv_obj_set_style_pad_hor(content, 12, LV_PART_MAIN);  // 12px side inset, same as every screen
        // Overridden separately from pad_all -- the legend is the last child of this flex
        // column, so pad_all's bottom inset was also the legend's distance from the true
        // screen edge, leaving it floating ~14px up instead of the ~6px margin every other
        // screen's explicitly-aligned legend uses. Top/sides keep the full 14px.
        lv_obj_set_style_pad_bottom(content, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_row(content, 10, LV_PART_MAIN);
        lv_obj_set_layout(content, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

        _name_lbl = lv_label_create(content);
        lv_label_set_long_mode(_name_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_name_lbl, LV_PCT(100));
        lv_obj_set_style_text_color(_name_lbl, lv_color_hex(C_CYAN), LV_PART_MAIN);
        lv_obj_set_style_text_font(_name_lbl, &lv_font_montserrat_20, LV_PART_MAIN);

        _price_row = make_row(content, "Price", "+- $0.05", &_price_val_lbl);
        _qty1_row  = make_row(content, "Quantity", "+- 1", nullptr);
        _qty10_row = make_row(content, "Quantity", "+- 10", nullptr);

        _upc_header_lbl = lv_label_create(content);
        lv_label_set_text(_upc_header_lbl, "UPC codes   (scan to link)");
        lv_obj_set_style_text_color(_upc_header_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(_upc_header_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

        _upc_list = lv_obj_create(content);
        lv_obj_set_size(_upc_list, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(_upc_list, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(_upc_list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(_upc_list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_row(_upc_list, 4, LV_PART_MAIN);
        lv_obj_set_layout(_upc_list, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(_upc_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(_upc_list, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *grow = lv_obj_create(content);
        lv_obj_set_size(grow, LV_PCT(100), 1);
        lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
        lv_obj_set_flex_grow(grow, 1);

        _msg_lbl = lv_label_create(content);
        lv_label_set_long_mode(_msg_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_msg_lbl, LV_PCT(100));
        lv_obj_set_style_text_color(_msg_lbl, lv_color_hex(C_ORANGE), LV_PART_MAIN);
        lv_obj_set_style_text_font(_msg_lbl, &lv_font_montserrat_14, LV_PART_MAIN);

        lv_obj_t *legend = ui_legend(content);
        char row_lbl[24], adj_lbl[24];
        snprintf(row_lbl, sizeof(row_lbl), "%s%s Row", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
        snprintf(adj_lbl, sizeof(adj_lbl), "Adjust %s%s", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
        ui_legend_row(legend, row_lbl, lv_color_hex(C_YELLOW), adj_lbl, lv_color_hex(C_YELLOW));

        // Enter/Back row, built by hand rather than via ui_legend_row() -- that helper
        // takes plain strings with no way to update them later, and Enter's meaning here
        // changes with the selected row (blank on the fixed rows, "Unlink UPC"/"Hide
        // Item"/etc. otherwise -- see update_action_hint()). An earlier version put this on
        // its own 3rd legend line instead of sharing Enter's usual spot; that made the
        // footer three lines tall instead of the normal two, so it was folded back in here.
        lv_obj_t *action_row = lv_obj_create(legend);
        lv_obj_set_size(action_row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(action_row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(action_row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(action_row, 0, LV_PART_MAIN);
        lv_obj_set_layout(action_row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(action_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(action_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(action_row, LV_OBJ_FLAG_SCROLLABLE);

        _action_hint_lbl = lv_label_create(action_row);
        lv_label_set_text(_action_hint_lbl, "");
        lv_obj_set_style_text_color(_action_hint_lbl, lv_color_hex(C_GREEN), LV_PART_MAIN);
        lv_obj_set_style_text_font(_action_hint_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

        lv_obj_t *back_lbl = lv_label_create(action_row);
        lv_label_set_text(back_lbl, "Back");
        lv_obj_set_style_text_color(back_lbl, lv_color_hex(C_RED), LV_PART_MAIN);
        lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    }

    set_msg(nullptr);
    refresh_header();
    refresh_upcs();
    restore_item_handlers();
    refresh_row_highlight();

    header_set_visible(true);

    lv_scr_load(_scr);
}
