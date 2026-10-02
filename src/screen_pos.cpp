#include "screen_pos.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "items.h"
#include "checkouts.h"
#include "db.h"
#include "buttons.h"
#include "ui.h"
#include "payment_link.h"
#include <lvgl.h>
#include <Arduino.h>
#include <string.h>
#include <sqlite3.h>
#include <ctype.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the transaction screen declared in screen_pos.h -- one file,
            four states (ST_LIST/ST_LOGOUT_CONFIRM/ST_MANUAL_ENTRY/ST_PAYMENT).
*/

// ── cart (RAM only — nothing touches the DB until Payment "Complete + Logout") ─────
// Grouping key for merging an add into an existing line is (item_id, price_cents) —
// currently always equivalent to just item_id, since every line (scanned or added via
// Item Lookup) carries the item's real catalog price. Kept as a pair instead of just
// item_id in case a future non-catalog price source (e.g. a real numeric override) needs
// to coexist with catalog lines of the same item_id without merging into them.
#define MAX_CART_LINES 32

struct CartLine {
    int  item_id;
    char name[ITEM_NAME_LEN];
    int  price_cents;
    int  quantity;
};

static CartLine _cart[MAX_CART_LINES];
static int      _cart_count;
static int      _total_cents;
static int      _user_id;

// ST_PAYMENT — which enabled payment_methods row is currently displayed. Reset to 0
// (→ venmo-first, see get_payment_method_at()'s ORDER BY) each time Payment is freshly
// entered from the cart; RIGHT cycles it without resetting, wrapping via the row count.
static int _payment_method_index;

typedef enum { ST_LIST, ST_LOGOUT_CONFIRM, ST_MANUAL_ENTRY, ST_PAYMENT } PosState;
static PosState _state;

// ── LVGL handles ──────────────────────────────────────────────────────────────────
static lv_obj_t *_scr;
static lv_obj_t *_content;
static lv_obj_t *_total_lbl;     // ST_LIST's total bar

// ST_LIST's cart cursor — Up/Down highlight one line at a time (same row-pool +
// partial-refresh pattern as screen_browse.cpp) so it's visually unambiguous which line
// LEFT (delete) is about to act on. Added by request — the original draft only ever
// deleted the most-recently-added line with no on-screen indication of what "last" meant.
static lv_obj_t *_cart_rows[MAX_CART_LINES];
static int        _cart_cursor;
static int        _cart_prev_cursor;

// One-shot "scan didn't match anything" message -- set by screen_pos_on_scan() on an
// unrecognized UPC, shown once at the top of the list by build_list_ui(), then cleared
// immediately after rendering so it doesn't linger through the next unrelated rebuild
// (adding/deleting a line, etc). Empty string = nothing to show.
static char _scan_msg[48];

// Set when Confirm's checkout_save() fails; build_payment_ui() shows it once, then clears it.
static bool _save_failed;

// ST_MANUAL_ENTRY's catalog picker — GM65 isn't wired to the device yet, so this is also
// how the whole flow gets exercised without hardware: pick items from the same list
// Price Check uses instead of scanning them. Row-pool + partial-refresh pattern matches
// screen_browse.cpp (a full-list restyle on every keypress was a measured source of
// input lag there — see Button timing tuning notes in snack_cart_pos.md).
static lv_obj_t *_manual_rows[MAX_ITEMS];
static int        _manual_cursor;
static int        _manual_prev_cursor;

// ── forward declarations ─────────────────────────────────────────────────────────
static void build_list_ui();
static void build_logout_confirm_ui();
static void build_manual_entry_ui();
static void build_payment_ui();

// Formats an integer cents value as "$X.XX".
static void format_cents(int cents, char *out, size_t out_len) {
    snprintf(out, out_len, "$%d.%02d", cents / 100, cents % 100);
}

// ── cart mutation ─────────────────────────────────────────────────────────────────

// Leaves the cart cursor pointing at whichever line this touched, so a scan (or an Item
// Lookup add) immediately highlights itself — the common case is "I just added the wrong
// thing, delete it," and this means LEFT does the right thing without the cashier having
// to scroll to find it first.
static void add_line(int item_id, const char *name, int price_cents) {
    for (int i = 0; i < _cart_count; i++) {
        if (_cart[i].item_id == item_id && _cart[i].price_cents == price_cents) {
            _cart[i].quantity++;
            _total_cents += price_cents;
            _cart_cursor = i;
            return;
        }
    }
    if (_cart_count >= MAX_CART_LINES) {
        Serial.println("[POS] cart full, ignoring scan");
        return;
    }
    CartLine &line = _cart[_cart_count++];
    line.item_id = item_id;
    strncpy(line.name, name, ITEM_NAME_LEN - 1);
    line.name[ITEM_NAME_LEN - 1] = '\0';
    line.price_cents = price_cents;
    line.quantity = 1;
    _total_cents += price_cents;
    _cart_cursor = _cart_count - 1;
}

// Adds a catalog item to the cart (by scan or Item Lookup) and rebuilds the list if visible.
static void add_catalog_item(const Item *it) {
    Serial.printf("[POS] Adding item %d: %s ($%d)\n", it->id, it->name, it->price_cents);
    add_line(it->id, it->name, it->price_cents);
    if (_state == ST_LIST) {
        lv_obj_clean(_content);
        build_list_ui();
    }
}

// ── ST_LIST callbacks ────────────────────────────────────────────────────────────

// Highlights the currently-selected cart line.
static void refresh_cart_cursor() {
    if (_cart_count == 0) return;

    if (_cart_prev_cursor >= 0 && _cart_prev_cursor < _cart_count && _cart_prev_cursor != _cart_cursor) {
        lv_obj_set_style_bg_opa(_cart_rows[_cart_prev_cursor], LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(_cart_rows[_cart_cursor], LV_OPA_30, LV_PART_MAIN);
    _cart_prev_cursor = _cart_cursor;

    lv_obj_scroll_to_view(_cart_rows[_cart_cursor], LV_ANIM_OFF);
}

// Up: moves the cart selection up one line, wrapping.
static void cb_cart_up() {
    if (_cart_count == 0) return;
    _cart_cursor = (_cart_cursor - 1 + _cart_count) % _cart_count;
    refresh_cart_cursor();
}

// Down: moves the cart selection down one line, wrapping.
static void cb_cart_down() {
    if (_cart_count == 0) return;
    _cart_cursor = (_cart_cursor + 1) % _cart_count;
    refresh_cart_cursor();
}

// Removes one unit of whichever line the cursor is on (decrements its quantity, or drops
// the line entirely once it's down to 1) — row count changes, so this rebuilds the list
// rather than doing a partial refresh like cb_cart_up/down do.
static void cb_delete_item() {
    if (_cart_count == 0) return;
    int i = _cart_cursor;
    _total_cents -= _cart[i].price_cents;
    if (_cart[i].quantity > 1) {
        _cart[i].quantity--;
    } else {
        for (int j = i; j < _cart_count - 1; j++) _cart[j] = _cart[j + 1];
        _cart_count--;
        if (_cart_cursor >= _cart_count && _cart_cursor > 0) _cart_cursor--;
    }
    lv_obj_clean(_content);
    build_list_ui();
}

// Back: opens the logout confirm.
static void cb_logout() {
    _state = ST_LOGOUT_CONFIRM;
    lv_obj_clean(_content);
    build_logout_confirm_ui();
}

// Right: opens Item Lookup (Manual Entry).
static void cb_manual_open() {
    _state = ST_MANUAL_ENTRY;
    _manual_cursor = 0;
    _manual_prev_cursor = -1;
    memset(_manual_rows, 0, sizeof(_manual_rows));
    lv_obj_clean(_content);
    build_manual_entry_ui();
}

// Enter: proceeds to Payment, if the cart isn't empty.
static void cb_finish() {
    if (_cart_count == 0) return;  // nothing to pay for
    _state = ST_PAYMENT;
    _payment_method_index = 0;
    lv_obj_clean(_content);
    build_payment_ui();
}

// ── ST_LOGOUT_CONFIRM callbacks ──────────────────────────────────────────────────

// Enter: confirms logout, discarding the cart with no DB write.
static void cb_logout_yes() {
    _cart_count  = 0;
    _total_cents = 0;
    screen_idle_load();  // no DB write — matches the state machine in snack_cart_pos.md
}

// Back: cancels the logout confirm, returning to the cart.
static void cb_logout_no() {
    _state = ST_LIST;
    lv_obj_clean(_content);
    build_list_ui();
}

// ── ST_MANUAL_ENTRY callbacks ────────────────────────────────────────────────────

// Left/Right jump ~a screenful at a time, same tuning rationale (and same wrap-at-the-
// end behavior) as screen_browse.cpp's BROWSE_PAGE -- added 2026-09-15 alongside Browse's
// own Up/Down -> Left/Right remap, so paging feels identical on both screens. Up/Down
// stay bound to the single-row cursor here (unlike Browse, this screen has something to
// select), so Left/Right were free to take over paging instead.
#define MANUAL_PAGE 7

// Only the first MAX_ITEMS rows are ever built (see build_manual_entry_ui()) — cursor math
// must wrap against that same cap, not the raw DB count, or it indexes _manual_rows[] out
// of bounds. (This is exactly what crashed the device 2026-08-25: items_count() outgrew
// MAX_ITEMS once the real 53-item catalog replaced the old 10-item test data.)
// include_hidden=false (2026-09-14) -- this is the checkout-time picker, a hidden item
// can't be sold, see items.h.
static int visible_count() {
    int n = items_count(false);
    return n > MAX_ITEMS ? MAX_ITEMS : n;
}

// Highlights the currently-selected Item Lookup row.
static void refresh_manual_cursor() {
    int n = visible_count();
    if (n == 0) return;

    if (_manual_prev_cursor >= 0 && _manual_prev_cursor < n && _manual_prev_cursor != _manual_cursor) {
        lv_obj_set_style_bg_opa(_manual_rows[_manual_prev_cursor], LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(_manual_rows[_manual_cursor], LV_OPA_30, LV_PART_MAIN);
    _manual_prev_cursor = _manual_cursor;

    lv_obj_scroll_to_view(_manual_rows[_manual_cursor], LV_ANIM_OFF);
}

// Up: moves the Item Lookup selection up one row, wrapping.
static void cb_manual_up() {
    int n = visible_count();
    if (n == 0) return;
    _manual_cursor = (_manual_cursor - 1 + n) % n;
    refresh_manual_cursor();
}

// Down: moves the Item Lookup selection down one row, wrapping.
static void cb_manual_down() {
    int n = visible_count();
    if (n == 0) return;
    _manual_cursor = (_manual_cursor + 1) % n;
    refresh_manual_cursor();
}

// Left: jumps the Item Lookup selection back ~a screenful, wrapping to the last row.
static void cb_manual_page_left() {
    int n = visible_count();
    if (n == 0) return;
    if (_manual_cursor <= 0) {
        _manual_cursor = n - 1;
    } else {
        _manual_cursor -= MANUAL_PAGE;
        if (_manual_cursor < 0) _manual_cursor = 0;
    }
    refresh_manual_cursor();
}

// Right: jumps the Item Lookup selection forward ~a screenful, wrapping to the first row.
static void cb_manual_page_right() {
    int n = visible_count();
    if (n == 0) return;
    if (_manual_cursor >= n - 1) {
        _manual_cursor = 0;
    } else {
        _manual_cursor += MANUAL_PAGE;
        if (_manual_cursor >= n) _manual_cursor = n - 1;
    }
    refresh_manual_cursor();
}

// Enter: adds the highlighted item to the cart and returns to the cart list.
static void cb_manual_select() {
    const Item *it = items_get(_manual_cursor, false);
    if (it) add_line(it->id, it->name, it->price_cents);
    _state = ST_LIST;
    lv_obj_clean(_content);
    build_list_ui();
}

// Back: returns to the cart list without adding anything.
static void cb_manual_back() {
    _state = ST_LIST;
    lv_obj_clean(_content);
    build_list_ui();
}

// ── ST_PAYMENT callbacks ─────────────────────────────────────────────────────────

// Enter: saves the checkout to the DB and returns to IDLE. Cart is kept if the save fails.
static void cb_pay_complete() {
    CheckoutLine lines[MAX_CART_LINES];
    for (int i = 0; i < _cart_count; i++) {
        lines[i].item_id     = _cart[i].item_id;
        lines[i].price_cents = _cart[i].price_cents;
        lines[i].quantity    = _cart[i].quantity;
    }
    int id = checkout_save(_user_id, lines, _cart_count, _total_cents);
    if (id < 0) {
        Serial.println("[POS] checkout_save() failed — cart kept, staying on Payment");
        _save_failed = true;  // don't lose the cart on a DB failure -- say so on screen
        lv_obj_clean(_content);
        build_payment_ui();
        return;
    }
    _cart_count  = 0;
    _total_cents = 0;
    screen_idle_load();
}

// Back: cancels payment, discarding the cart with no DB write.
static void cb_pay_cancel() {
    _cart_count  = 0;
    _total_cents = 0;
    screen_idle_load();  // no DB write
}

// Distinct from Cancel — returns to the cart to keep shopping/fix something, without
// discarding it or logging out. Cart is untouched either way (Payment never mutates it).
static void cb_pay_back_to_list() {
    _state = ST_LIST;
    lv_obj_clean(_content);
    build_list_ui();
}

// Right: cycles to the next enabled payment method.
static void cb_pay_other() {
    _payment_method_index++;  // build_payment_ui() wraps this against the real row count
    lv_obj_clean(_content);
    build_payment_ui();
}

// ── UI builders ───────────────────────────────────────────────────────────────────

// Builds the ST_LIST (cart) UI.
static void build_list_ui() {
    // Explicit height is a placeholder — flex_grow(1) takes over main-axis sizing and
    // expands this to fill whatever space is left after the total bar + footer below,
    // same pattern as the "grow" spacers elsewhere in this codebase (e.g. screen_enroll.cpp).
    lv_obj_t *list = lv_obj_create(_content);
    lv_obj_set_size(list, LV_PCT(100), 1);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(list, 0, LV_PART_MAIN);  // 12px side inset, same as every screen
    lv_obj_set_style_pad_row(list, 6, LV_PART_MAIN);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    if (_scan_msg[0] != '\0') {
        // Shown regardless of cart state -- a scan can fail whether the cart is empty or
        // already has lines in it, unlike the plain empty-cart hint below.
        lv_obj_t *msg = lv_label_create(list);
        lv_label_set_text(msg, _scan_msg);
        lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(msg, LV_PCT(100));
        lv_obj_set_style_text_color(msg, lv_color_hex(C_RED), LV_PART_MAIN);
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        _scan_msg[0] = '\0';  // one-shot -- see the declaration comment
    } else if (_cart_count == 0) {
        lv_obj_t *hint = lv_label_create(list);
        lv_label_set_text(hint, "Scan an item to begin");
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_set_style_text_color(hint, lv_color_hex(C_DIM), LV_PART_MAIN);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }

    // Same row-pool + cursor-highlight pattern as screen_browse.cpp: base rows are
    // transparent, the cart cursor's row gets a cyan tint toggled on by
    // refresh_cart_cursor() — so it's unambiguous which line LEFT (delete) will act on.
    for (int i = 0; i < _cart_count; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 10, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_CYAN), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);  // refresh_cart_cursor() toggles this
        lv_obj_set_layout(row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        _cart_rows[i] = row;

        char name_buf[ITEM_NAME_LEN + 8];
        if (_cart[i].quantity > 1) snprintf(name_buf, sizeof(name_buf), "%s x%d", _cart[i].name, _cart[i].quantity);
        else                       snprintf(name_buf, sizeof(name_buf), "%s", _cart[i].name);
        lv_obj_t *name_lbl = lv_label_create(row);
        lv_label_set_text(name_lbl, name_buf);
        lv_obj_set_style_text_color(name_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

        char price_buf[12];
        format_cents(_cart[i].price_cents * _cart[i].quantity, price_buf, sizeof(price_buf));
        lv_obj_t *price_lbl = lv_label_create(row);
        lv_label_set_text(price_lbl, price_buf);
        lv_obj_set_style_text_color(price_lbl, lv_color_hex(C_GREEN), LV_PART_MAIN);
        lv_obj_set_style_text_font(price_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    }

    if (_cart_cursor >= _cart_count) _cart_cursor = _cart_count > 0 ? _cart_count - 1 : 0;
    _cart_prev_cursor = -1;
    refresh_cart_cursor();

    // total bar
    lv_obj_t *total_bar = lv_obj_create(_content);
    lv_obj_set_size(total_bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(total_bar, lv_color_hex(C_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_border_width(total_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(total_bar, 10, LV_PART_MAIN);
    lv_obj_clear_flag(total_bar, LV_OBJ_FLAG_SCROLLABLE);

    char total_buf[24];
    char total_price[12];
    format_cents(_total_cents, total_price, sizeof(total_price));
    snprintf(total_buf, sizeof(total_buf), "Total:  %s", total_price);
    _total_lbl = lv_label_create(total_bar);
    lv_label_set_text(_total_lbl, total_buf);
    lv_obj_set_style_text_color(_total_lbl, lv_color_hex(C_GREEN), LV_PART_MAIN);
    lv_obj_set_style_text_font(_total_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(_total_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    // footer — an explicit two-line legend rather than just two on-screen buttons, so
    // every one of the four active buttons is spelled out (not just Enter/Back). Each
    // line pins its label to the physical edge the button actually sits at: arrows for
    // Left/Right, and the color name itself (rendered in that color) for Enter/Back.
    lv_obj_t *legend = ui_legend(_content);
    char left_arrow[24], right_arrow[24];
    snprintf(left_arrow, sizeof(left_arrow), "%s Delete Item", LV_SYMBOL_LEFT);
    snprintf(right_arrow, sizeof(right_arrow), "Item Lookup %s", LV_SYMBOL_RIGHT);
    ui_legend_row(legend, left_arrow, lv_color_hex(C_YELLOW), right_arrow, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "Pay", lv_color_hex(C_GREEN), "Cancel", lv_color_hex(C_RED));

    ButtonHandlers h;
    h.up    = cb_cart_up;
    h.down  = cb_cart_down;
    h.left  = cb_delete_item;
    h.right = cb_manual_open;
    h.enter = cb_finish;
    h.back  = cb_logout;
    h.wantsScanner = true;  // scanning another item's UPC adds it to the cart -- see
                             // screen_pos_on_scan()'s ST_LIST-only gate above
    buttons_set_handlers(h);
}

// Builds the ST_LOGOUT_CONFIRM UI.
static void build_logout_confirm_ui() {
    lv_obj_t *prompt = lv_label_create(_content);
    lv_label_set_text(prompt, "Logout and Cancel\nTransaction?");
    lv_label_set_long_mode(prompt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(prompt, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(prompt, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(prompt, LV_PCT(100));
    lv_obj_set_style_text_align(prompt, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *grow = lv_obj_create(_content);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);

    lv_obj_t *legend = ui_legend(_content);
    ui_legend_row(legend, "Yes", lv_color_hex(C_GREEN), "No", lv_color_hex(C_RED));

    ButtonHandlers h;
    h.enter = cb_logout_yes;  // convention: Enter = affirmative, Back = negative
    h.back  = cb_logout_no;
    buttons_set_handlers(h);
}

// Builds the ST_MANUAL_ENTRY (Item Lookup) UI.
static void build_manual_entry_ui() {
    lv_obj_t *hint = lv_label_create(_content);
    lv_label_set_text(hint, "Item Lookup - select an item to add:");
    lv_obj_set_style_text_color(hint, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(hint, LV_PCT(100));

    // Same row-pool + partial-refresh list pattern as screen_browse.cpp.
    lv_obj_t *list = lv_obj_create(_content);
    lv_obj_set_size(list, LV_PCT(100), 1);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(list, 0, LV_PART_MAIN);  // 12px side inset, same as every screen
    lv_obj_set_style_pad_row(list, 6, LV_PART_MAIN);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    int n = items_count(false);
    for (int i = 0; i < n && i < MAX_ITEMS; i++) {
        const Item *it = items_get(i, false);

        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 10, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_CYAN), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);  // refresh_manual_cursor() toggles this
        lv_obj_set_layout(row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        _manual_rows[i] = row;

        // it can be null if the underlying query failed -- see screen_inventory.cpp's
        // matching guard for why this isn't just theoretical caution.
        lv_obj_t *name_lbl = lv_label_create(row);
        lv_label_set_text(name_lbl, it ? it->name : "(error loading item)");
        lv_obj_set_style_text_color(name_lbl, it ? lv_color_hex(C_TEXT) : lv_color_hex(C_RED), LV_PART_MAIN);
        lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

        char price_buf[12];
        if (it) format_cents(it->price_cents, price_buf, sizeof(price_buf));
        else    snprintf(price_buf, sizeof(price_buf), "--");
        lv_obj_t *price_lbl = lv_label_create(row);
        lv_label_set_text(price_lbl, price_buf);
        lv_obj_set_style_text_color(price_lbl, lv_color_hex(C_GREEN), LV_PART_MAIN);
        lv_obj_set_style_text_font(price_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    }

    lv_obj_t *legend = ui_legend(_content);
    char move_lbl[24], page_lbl[24];
    snprintf(move_lbl, sizeof(move_lbl), "Move %s%s", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
    snprintf(page_lbl, sizeof(page_lbl), "Page %s%s", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
    ui_legend_row(legend, move_lbl, lv_color_hex(C_YELLOW), page_lbl, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "Add", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));

    refresh_manual_cursor();

    ButtonHandlers h;
    h.up    = cb_manual_up;
    h.down  = cb_manual_down;
    h.left  = cb_manual_page_left;
    h.right = cb_manual_page_right;
    h.enter = cb_manual_select;
    h.back  = cb_manual_back;
    buttons_set_handlers(h);
}

// Payment recipient info lives in its own `payment_methods` table (one row per method —
// venmo, eventually zelle, etc.), not `config` — see db.cpp's schema comment for the full
// reasoning. RIGHT on the Payment screen cycles through whichever methods are enabled;
// venmo is always shown first (see ORDER BY below) regardless of how many others exist.

// Returns how many payment_methods rows are enabled.
static int count_enabled_payment_methods() {
    sqlite3_stmt *stmt;
    int count = 0;
    if (sqlite3_prepare_v2(db_handle(), "SELECT COUNT(*) FROM payment_methods WHERE enabled = 1;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return count;
}

// Reads the index'th enabled payment method (venmo always sorted first). Returns true if
// it has a real handle set.
static bool get_payment_method_at(int index, char *method, size_t method_len,
                                   char *display_name, size_t display_name_len,
                                   char *handle, size_t handle_len) {
    method[0] = '\0';
    display_name[0] = '\0';
    handle[0] = '\0';

    sqlite3_stmt *stmt;
    bool have_handle = false;
    const char *sql =
        "SELECT method, display_name, handle FROM payment_methods WHERE enabled = 1 "
        "ORDER BY CASE WHEN method = 'venmo' THEN 0 ELSE 1 END, method LIMIT 1 OFFSET ?;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, index);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *mt = sqlite3_column_text(stmt, 0);
            const unsigned char *dn = sqlite3_column_text(stmt, 1);
            const unsigned char *hd = sqlite3_column_text(stmt, 2);
            if (mt) { strncpy(method, (const char *)mt, method_len - 1); method[method_len - 1] = '\0'; }
            if (dn) { strncpy(display_name, (const char *)dn, display_name_len - 1); display_name[display_name_len - 1] = '\0'; }
            if (hd && hd[0] != '\0') {
                strncpy(handle, (const char *)hd, handle_len - 1);
                handle[handle_len - 1] = '\0';
                have_handle = true;
            }
        }
        sqlite3_finalize(stmt);
    }
    return have_handle;
}

// checkouts.id doesn't exist yet at this point — Payment shows before the checkout is
// ever saved (see cb_pay_complete()), so this predicts what checkout_save() will assign.
// Safe on this specific device: single cashier, single writer, and checkouts are never
// deleted (see checkouts schema note), so MAX(id)+1 always matches the real next id.
static int predict_next_checkout_id() {
    sqlite3_stmt *stmt;
    int next_id = 1;
    if (sqlite3_prepare_v2(db_handle(), "SELECT COALESCE(MAX(id), 0) + 1 FROM checkouts;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) next_id = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return next_id;
}

// Builds the ST_PAYMENT UI -- a payment QR code (payment_link.cpp), a plain-text fallback,
// or a "not set up" warning, plus the total and footer.
static void build_payment_ui() {
    int method_count = count_enabled_payment_methods();
    if (method_count > 0 && _payment_method_index >= method_count) _payment_method_index = 0;

    char method[16] = "";
    char owner[32]  = "";
    char handle[PAYMENT_HANDLE_MAX] = "";
    bool have_method = method_count > 0 &&
        get_payment_method_at(_payment_method_index, method, sizeof(method), owner, sizeof(owner), handle, sizeof(handle));
    if (owner[0] == '\0') strncpy(owner, "Owner", sizeof(owner));

    // Tabs show which method this QR is for and what "Other Payment" goes to next.
    payment_add_tabs(_content, _payment_method_index);

    char to_buf[64];
    snprintf(to_buf, sizeof(to_buf), "Payment to: %s", owner);
    lv_obj_t *to_lbl = lv_label_create(_content);
    lv_label_set_text(to_lbl, to_buf);
    lv_obj_set_style_text_color(to_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(to_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(to_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(to_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    char total_price[12];
    format_cents(_total_cents, total_price, sizeof(total_price));

    // Venmo/Cash App/Zelle QR codes all come from payment_link.cpp (2026-09-29). Only
    // Venmo's note carries the predicted transaction number; the other two can't hold one.
    if (!have_method) {
        lv_obj_t *warn = lv_label_create(_content);
        lv_label_set_text(warn, "Payment info not set yet\n(Admin > Settings > Payment Info)");
        lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(warn, lv_color_hex(C_ORANGE), LV_PART_MAIN);
        lv_obj_set_style_text_font(warn, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_width(warn, LV_PCT(100));
        lv_obj_set_style_text_align(warn, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    } else if (!payment_add_qr(_content, method, handle, owner, _total_cents, predict_next_checkout_id())) {
        // A method with no QR format (or a payload too long to fit) -- show it as text.
        char fallback_buf[80];
        snprintf(fallback_buf, sizeof(fallback_buf), "%s: %s", payment_method_label(method), handle);
        lv_obj_t *fallback = lv_label_create(_content);
        lv_label_set_text(fallback, fallback_buf);
        lv_label_set_long_mode(fallback, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(fallback, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(fallback, &lv_font_montserrat_20, LV_PART_MAIN);
        lv_obj_set_width(fallback, LV_PCT(100));
        lv_obj_set_style_text_align(fallback, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }

    char total_buf[24];
    snprintf(total_buf, sizeof(total_buf), "Total:  %s", total_price);
    lv_obj_t *total_lbl = lv_label_create(_content);
    lv_label_set_text(total_lbl, total_buf);
    lv_obj_set_style_text_color(total_lbl, lv_color_hex(C_GREEN), LV_PART_MAIN);
    lv_obj_set_style_text_font(total_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(total_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(total_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    if (_save_failed) {
        lv_obj_t *err = lv_label_create(_content);
        lv_label_set_text(err, "Couldn't save. Press Confirm\nto try again.");
        lv_label_set_long_mode(err, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(err, lv_color_hex(C_RED), LV_PART_MAIN);
        lv_obj_set_style_text_font(err, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_width(err, LV_PCT(100));
        lv_obj_set_style_text_align(err, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        _save_failed = false;  // one-shot, like _scan_msg
    }

    lv_obj_t *grow = lv_obj_create(_content);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);

    // Footer — same 2-line color-coded legend style as TRANSACTION's, by request: Left/
    // Right on top (arrows), Enter/Back (green/red) below. Left returns to the cart
    // without cancelling (distinct from the red Cancel/Logout); Right cycles payment
    // methods (Venmo first, then Cash App/Zelle if enabled).
    lv_obj_t *legend = ui_legend(_content);
    char left_arrow[16], right_arrow[24];
    snprintf(left_arrow, sizeof(left_arrow), "%s Back", LV_SYMBOL_LEFT);
    snprintf(right_arrow, sizeof(right_arrow), "Other Payment %s", LV_SYMBOL_RIGHT);
    ui_legend_row(legend, left_arrow, lv_color_hex(C_YELLOW), right_arrow, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "Confirm", lv_color_hex(C_GREEN), "Cancel", lv_color_hex(C_RED));

    ButtonHandlers h;
    h.left  = cb_pay_back_to_list;
    h.right = cb_pay_other;
    h.enter = cb_pay_complete;
    h.back  = cb_pay_cancel;
    buttons_set_handlers(h);
}

// ── public ────────────────────────────────────────────────────────────────────────

// Consumes a scan while this screen is active -- adds a known item to the cart in
// ST_LIST; swallowed (ignored) in every other sub-state.
bool screen_pos_on_scan(const char *upc) {
    if (!_scr || lv_scr_act() != _scr) return false;
    if (_state != ST_LIST) return true;  // consumed but ignored mid sub-screen

    const Item *it = items_find_by_upc(upc);
    if (it) {
        add_catalog_item(it);
    } else {
        Serial.printf("[POS] Unknown item UPC: %s\n", upc);
        // Previously silent -- a real scan that just doesn't match anything looked
        // identical to no scan happening at all. See _scan_msg's declaration comment.
        snprintf(_scan_msg, sizeof(_scan_msg), "Item not found.\nRescan or use Lookup.");
        lv_obj_clean(_content);
        build_list_ui();
    }
    return true;
}

// Loads the transaction screen for user_id, always starting with a fresh empty cart.
void screen_pos_push(int user_id) {
    _user_id     = user_id;
    _cart_count  = 0;
    _total_cents = 0;
    _cart_cursor = 0;
    _state       = ST_LIST;

    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);
    }

    if (!_content) {
        _content = lv_obj_create(_scr);
        lv_obj_set_size(_content, SCREEN_W, SCREEN_H - HDR_H);
        lv_obj_align(_content, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_opa(_content, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(_content, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(_content, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(_content, 12, LV_PART_MAIN);
        // The footer legend is this flex column's last child, so pad_ver's bottom inset
        // was also its distance from the true screen edge -- overridden separately to
        // match the ~6px margin every explicitly-aligned legend elsewhere uses (see
        // screen_item_edit.cpp's identical fix). Covers all four of this screen's states
        // (ST_LIST/LOGOUT_CONFIRM/MANUAL_ENTRY/PAYMENT) since they share this one _content.
        lv_obj_set_style_pad_bottom(_content, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_row(_content, 8, LV_PART_MAIN);
        lv_obj_set_layout(_content, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(_content, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(_content, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_clean(_content);
    }

    header_set_visible(true);
    header_set_title("TRANSACTION");  // 2026-08-26 — was never set here, so a stale title
                                       // (e.g. "ADMIN MENU") lingered from whatever screen
                                       // was up before a regular user's badge scan
    build_list_ui();
    lv_scr_load(_scr);
}
