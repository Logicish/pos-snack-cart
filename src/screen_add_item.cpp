#include "screen_add_item.h"
#include "screen_item_edit.h"
#include "screen_inventory_menu.h"
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
  Function- Implements the Add/Attach Item flow declared in screen_add_item.h --
            three states: ST_SCAN (waiting for a barcode), ST_ATTACH (catalog picker
            for an unrecognized UPC), ST_NAME (character wheel for a brand-new item).
*/

// Character wheel for a new item's name. Cell dimensions are screen_enroll.cpp's
// already-hardware-proven sizing (not shared code, just the same tested visuals); at that
// width only 11 fit across the 320px screen, and real names run longer ("Starbucks
// Espresso" is 19), so the field wraps onto a second, shorter row. It reads as ONE
// continuous line -- the cursor auto-crosses the row boundary with Left/Right -- and the
// short row 2 caps the name at 18 chars, matching the ~17-char convention the seeded
// catalog already follows (keeps new on-device items visually consistent in every list).
static const char CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ -";
#define CHARSET_LEN 28
#define NAME_LEN    11          // widest row / array width
#define ROW0_LEN    11
#define ROW1_LEN     7
#define TOTAL_NAME_LEN (ROW0_LEN + ROW1_LEN)
#define IDX_DASH    27
#define IDX_A       0

// Returns how many cells the given wheel row has.
static inline uint8_t row_len(uint8_t row) { return row == 0 ? ROW0_LEN : ROW1_LEN; }

// ST_ATTACH's LEFT button jumps the highlight ~one screenful down the catalog list, so an
// admin can page through a long catalog eyeballing each screen rather than single-stepping
// past 100 rows. Deliberately a touch smaller than the visible row count so successive
// presses always overlap by a row and never skip one; bump it if paging ever feels slow.
#define ATTACH_PAGE 6

typedef enum { ST_SCAN, ST_ATTACH, ST_NAME } State;

static lv_obj_t *_scr;
static lv_obj_t *_content;
static State      _state;
static char       _pending_upc[UPC_LEN];

// -- ST_NAME (wheel) --
static int8_t    _idx[2][NAME_LEN];
static uint8_t    _wcursor;
static uint8_t    _wrow;
static lv_obj_t  *_cell[2][NAME_LEN];
static lv_obj_t  *_cell_lbl[2][NAME_LEN];

// -- ST_ATTACH (catalog list) --
static lv_obj_t *_pick_rows[MAX_ITEMS];
static int        _pick_cursor;
static int        _pick_prev_cursor = -1;

static void build_scan_ui();
static void build_attach_ui();
static void build_name_ui();

// Clears the content area between states.
static void clear_content() {
    lv_obj_clean(_content);
    memset(_cell,     0, sizeof(_cell));
    memset(_cell_lbl, 0, sizeof(_cell_lbl));
    _pick_prev_cursor = -1;
}

// ============ ST_SCAN ============

// Back returns to the Inventory submenu.
static void cb_scan_back() {
    screen_inventory_menu_push();  // 2026-08-28 reorg -- was screen_menu_push()
}

// Consumes a scan while this screen is active -- opens Item Edit for a known UPC,
// otherwise remembers it and moves into the attach/new-item picker.
bool screen_add_item_on_scan(const char *upc) {
    if (!_scr || lv_scr_act() != _scr) return false;
    // Consume (and ignore) a stray scan during the wheel/attach sub-states rather than
    // letting it fall through to badge routing in main.cpp's on_scan() -- this screen owns
    // the display either way, a scan here should never be reinterpreted as a badge.
    if (_state != ST_SCAN) return true;

    const Item *it = items_find_by_upc(upc);
    if (it) {
        screen_item_edit_push(it->id, screen_add_item_push);
        return true;
    }

    strncpy(_pending_upc, upc, UPC_LEN - 1);
    _pending_upc[UPC_LEN - 1] = '\0';
    _state = ST_ATTACH;
    clear_content();
    build_attach_ui();
    return true;
}

// Builds the ST_SCAN prompt UI.
static void build_scan_ui() {
    lv_obj_t *lbl = lv_label_create(_content);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(lbl, "Scan an item's barcode to add or attach it to the list.");

    lv_obj_t *grow = lv_obj_create(_content);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);

    // Just the one active button here (Back) -- still spell it out bottom-right in red,
    // same as every other screen's Cancel, rather than leaving it to a text hint.
    ui_legend_row(_content, "", lv_color_hex(C_TEXT), "Cancel", lv_color_hex(C_RED));

    ButtonHandlers h;
    h.back = cb_scan_back;
    h.wantsScanner = true;  // waiting for the item's barcode
    buttons_set_handlers(h);
}

// ============ ST_NAME (character wheel) ============

// Concatenates row 0 (11) + row 1 (7) into one string, then trims only the trailing run of
// dashes/spaces off the combined end -- a dash/space typed anywhere before the last real
// character is kept literally (this project's catalog has real names that need a literal
// dash, e.g. "Cheez-It", "K-Cups"). Same trim philosophy as screen_enroll.cpp's get_name().
static void get_name(char *out, size_t out_len) {
    char buf[TOTAL_NAME_LEN + 1];
    int pos = 0;
    for (int i = 0; i < ROW0_LEN; i++) buf[pos++] = CHARSET[(uint8_t)_idx[0][i]];
    for (int i = 0; i < ROW1_LEN; i++) buf[pos++] = CHARSET[(uint8_t)_idx[1][i]];
    buf[pos] = '\0';

    while (pos > 0 && (buf[pos - 1] == '-' || buf[pos - 1] == ' ')) buf[--pos] = '\0';

    strncpy(out, buf, out_len - 1);
    out[out_len - 1] = '\0';
}

// Redraws every wheel cell, highlighting the cursor cell.
static void refresh_wheel_cells() {
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < row_len(row); col++) {
            if (!_cell_lbl[row][col]) continue;

            char buf[2] = { CHARSET[(uint8_t)_idx[row][col]], '\0' };
            lv_label_set_text(_cell_lbl[row][col], buf);

            // Both rows are always full-brightness -- it's one wrapped name, not two
            // separate fields; only the cursor cell stands out.
            bool is_cursor = ((int)_wrow == row && (int)_wcursor == col);
            lv_color_t color = is_cursor ? lv_color_hex(C_CYAN) : lv_color_hex(C_TEXT);
            lv_obj_set_style_text_color(_cell_lbl[row][col], color, LV_PART_MAIN);

            lv_border_side_t side = is_cursor ? LV_BORDER_SIDE_BOTTOM : LV_BORDER_SIDE_NONE;
            lv_obj_set_style_border_side(_cell[row][col], side, LV_PART_MAIN);
            lv_obj_set_style_border_width(_cell[row][col], is_cursor ? 2 : 0, LV_PART_MAIN);
            lv_obj_set_style_border_color(_cell[row][col], lv_color_hex(C_CYAN), LV_PART_MAIN);
        }
    }
}

// Left/Right walk the cursor across the whole name as if it were one line -- crossing the
// row boundary automatically rather than needing a separate "next row" key, so the two
// rows read as a single wrapped field.
static void cb_wheel_left() {
    if (_wcursor > 0) {
        _wcursor--;
    } else if (_wrow == 1) {
        _wrow = 0;
        _wcursor = ROW0_LEN - 1;
    } else {
        return;  // already at the very start
    }
    refresh_wheel_cells();
}

static void cb_wheel_right() {
    if (_wcursor < row_len(_wrow) - 1) {
        _wcursor++;
    } else if (_wrow == 0) {
        _wrow = 1;
        _wcursor = 0;
    } else {
        return;  // already at the very end
    }
    refresh_wheel_cells();
}

static void cb_wheel_up() {
    _idx[_wrow][_wcursor] = (_idx[_wrow][_wcursor] + 1) % CHARSET_LEN;
    refresh_wheel_cells();
}

static void cb_wheel_down() {
    _idx[_wrow][_wcursor] = (_idx[_wrow][_wcursor] + CHARSET_LEN - 1) % CHARSET_LEN;
    refresh_wheel_cells();
}

// Enter always confirms now (single-stage) -- Left/Right handle row movement, so there's
// no longer a "commit row 1" step for Enter to mean on row 0.
static void cb_name_confirm() {
    char name[TOTAL_NAME_LEN + 1];
    get_name(name, sizeof(name));
    if (name[0] == '\0') return;  // nothing typed at all -- ignore, stay on the wheel

    int new_id = items_create(name, 0, 0);
    if (new_id < 0) return;  // DB error -- stay put rather than pretend it worked

    item_upcs_link(new_id, _pending_upc);
    screen_item_edit_push(new_id, screen_add_item_push);
}

// Back returns to the attach list (one level up) from anywhere in the name -- the pending
// barcode is still in hand, the admin may want to pick an existing item after all.
static void cb_wheel_back() {
    _state = ST_ATTACH;
    clear_content();
    build_attach_ui();
}

// Builds one row of character cells for the name wheel.
static void build_wheel_row(lv_obj_t *parent, uint8_t row) {
    // Width-to-content + a fixed inter-cell gap (not SPACE_BETWEEN) so the shorter row 2
    // left-aligns directly under row 1's first cells -- the two rows share one column grid,
    // reinforcing that it's a single wrapped line. The whole block is centred by _content's
    // cross-axis align.
    lv_obj_t *row_cont = lv_obj_create(parent);
    lv_obj_set_size(row_cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row_cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row_cont, 7, LV_PART_MAIN);
    lv_obj_set_layout(row_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_cont, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row_cont, LV_OBJ_FLAG_SCROLLABLE);

    for (int col = 0; col < row_len(row); col++) {
        lv_obj_t *cell = lv_obj_create(row_cont);
        lv_obj_set_size(cell, 20, 28);
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        _cell[row][col] = cell;

        lv_obj_t *lbl = lv_label_create(cell);
        char buf[2] = { CHARSET[(uint8_t)_idx[row][col]], '\0' };
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_center(lbl);
        _cell_lbl[row][col] = lbl;
    }
}

// Builds the ST_NAME (character wheel) UI.
static void build_name_ui() {
    _wrow    = 0;
    _wcursor = 0;
    for (int col = 0; col < ROW0_LEN; col++) _idx[0][col] = (col == 0) ? IDX_A : IDX_DASH;
    for (int col = 0; col < ROW1_LEN; col++) _idx[1][col] = IDX_DASH;

    lv_obj_t *hint = lv_label_create(_content);
    lv_label_set_text(hint, "Item name:");
    lv_obj_set_style_text_color(hint, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(hint, LV_PCT(100));

    build_wheel_row(_content, 0);
    build_wheel_row(_content, 1);

    lv_obj_t *grow = lv_obj_create(_content);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);

    // Same two-line edge-pinned legend style as the attach screen: row 1 (yellow) is the
    // directional pair -- Left/Right walk the cursor across the wrapped name, Up/Down
    // change the letter under it; row 2 is Green (confirm) / Red (back to the list).
    lv_obj_t *legend = ui_legend(_content);
    char cursor_lbl[28], letter_lbl[28];
    snprintf(cursor_lbl, sizeof(cursor_lbl), "%s%s Cursor", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
    snprintf(letter_lbl, sizeof(letter_lbl), "Letter %s%s", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
    ui_legend_row(legend, cursor_lbl, lv_color_hex(C_YELLOW), letter_lbl, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "Confirm", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));

    refresh_wheel_cells();

    ButtonHandlers h;
    h.up    = cb_wheel_up;
    h.down  = cb_wheel_down;
    h.left  = cb_wheel_left;
    h.right = cb_wheel_right;
    h.enter = cb_name_confirm;
    h.back  = cb_wheel_back;
    buttons_set_handlers(h);
}

// ============ ST_ATTACH (catalog picker + new-item / cancel) ============
// Reached straight off an unrecognized scan -- one screen that either attaches the pending
// barcode to an existing catalog item (Green), starts a brand-new item (Right), or backs
// out (Red). Same row-pool + partial-refresh list pattern as screen_browse.cpp /
// screen_pos.cpp's Manual Entry -- see items.h's MAX_ITEMS comment for the cap.

static int visible_count() {
    int n = items_count();
    return n > MAX_ITEMS ? MAX_ITEMS : n;
}

// Highlights the currently-selected catalog row.
static void refresh_pick_cursor() {
    int n = visible_count();
    if (n == 0) return;

    if (_pick_prev_cursor >= 0 && _pick_prev_cursor < n && _pick_prev_cursor != _pick_cursor) {
        lv_obj_set_style_bg_opa(_pick_rows[_pick_prev_cursor], LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(_pick_rows[_pick_cursor], LV_OPA_30, LV_PART_MAIN);
    _pick_prev_cursor = _pick_cursor;

    lv_obj_scroll_to_view(_pick_rows[_pick_cursor], LV_ANIM_OFF);
}

// Up: moves the catalog selection up one row, wrapping.
static void cb_pick_up() {
    int n = visible_count();
    if (n == 0) return;
    _pick_cursor = (_pick_cursor - 1 + n) % n;
    refresh_pick_cursor();
}

// Down: moves the catalog selection down one row, wrapping.
static void cb_pick_down() {
    int n = visible_count();
    if (n == 0) return;
    _pick_cursor = (_pick_cursor + 1) % n;
    refresh_pick_cursor();
}

// LEFT: jump ~a screenful down the list. Clamps onto the last item on the final (partial)
// page so nothing at the end gets skipped, then wraps to the top on the next press.
static void cb_attach_pagedown() {
    int n = visible_count();
    if (n == 0) return;
    if (_pick_cursor >= n - 1) {
        _pick_cursor = 0;
    } else {
        _pick_cursor += ATTACH_PAGE;
        if (_pick_cursor >= n) _pick_cursor = n - 1;
    }
    refresh_pick_cursor();
}

// Right: switches to the new-item name wheel.
static void cb_attach_new() {
    _state = ST_NAME;
    clear_content();
    build_name_ui();
}

// Back cancels the whole flow.
static void cb_attach_cancel() {
    _state = ST_SCAN;
    clear_content();
    build_scan_ui();
}

// Enter: links the pending UPC to the highlighted catalog item.
static void cb_attach_select() {
    const Item *it = items_get(_pick_cursor);
    if (!it) return;
    item_upcs_link(it->id, _pending_upc);
    screen_item_edit_push(it->id, screen_add_item_push);
}

// Builds the ST_ATTACH (catalog picker) UI.
static void build_attach_ui() {
    _pick_cursor = 0;

    lv_obj_t *hint = lv_label_create(_content);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_label_set_text(hint, "Not recognized. Attach it to an item below, or make a new one.");
    lv_obj_set_style_text_color(hint, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(hint, LV_PCT(100));

    lv_obj_t *list = lv_obj_create(_content);
    lv_obj_set_size(list, LV_PCT(100), 1);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 6, LV_PART_MAIN);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    int n = items_count();
    for (int i = 0; i < n && i < MAX_ITEMS; i++) {
        const Item *it = items_get(i);

        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_radius(row, 6, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 10, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_CYAN), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        _pick_rows[i] = row;

        // it can be null if the underlying query failed -- see screen_inventory.cpp's
        // matching guard for why this isn't just theoretical caution. Hidden items are
        // deliberately still offered here (see items.h) -- attaching a new barcode to a
        // discontinued-but-not-deleted item is a real use case -- but flagged so it's not
        // confused with an active catalog entry.
        lv_obj_t *lbl = lv_label_create(row);
        if (!it) {
            lv_label_set_text(lbl, "(error loading item)");
            lv_obj_set_style_text_color(lbl, lv_color_hex(C_RED), LV_PART_MAIN);
        } else if (it->hidden) {
            char name_buf[ITEM_NAME_LEN + 12];
            snprintf(name_buf, sizeof(name_buf), "%s [HIDDEN]", it->name);
            lv_label_set_text(lbl, name_buf);
            lv_obj_set_style_text_color(lbl, lv_color_hex(C_DIM), LV_PART_MAIN);
        } else {
            lv_label_set_text(lbl, it->name);
            lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
        }
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    }

    // Explicit, color-matched two-line legend -- spells out all four active buttons, not
    // just Enter/Back (see feedback-ux-explicit-legends). Arrows point at Left/Right;
    // Green/Red rendered in the physical button colors.
    lv_obj_t *legend = ui_legend(_content);
    char left_arrow[28], right_arrow[28];
    snprintf(left_arrow,  sizeof(left_arrow),  "%s Next Screen", LV_SYMBOL_LEFT);
    snprintf(right_arrow, sizeof(right_arrow), "New Item %s", LV_SYMBOL_RIGHT);
    ui_legend_row(legend, left_arrow, lv_color_hex(C_YELLOW), right_arrow, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "Add to Selected", lv_color_hex(C_GREEN), "Cancel", lv_color_hex(C_RED));

    refresh_pick_cursor();

    ButtonHandlers h;
    h.up    = cb_pick_up;
    h.down  = cb_pick_down;
    h.left  = cb_attach_pagedown;
    h.enter = cb_attach_select;
    h.right = cb_attach_new;
    h.back  = cb_attach_cancel;
    buttons_set_handlers(h);
}

// ============ public ============

// Loads the Add/Attach Item flow, always starting at ST_SCAN.
void screen_add_item_push() {
    _state = ST_SCAN;

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
        // screen_item_edit.cpp's identical fix).
        lv_obj_set_style_pad_bottom(_content, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_row(_content, 8, LV_PART_MAIN);
        lv_obj_set_layout(_content, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(_content, LV_FLEX_FLOW_COLUMN);
        // Cross-axis centre: only affects width-to-content children (the name wheel's two
        // rows) -- every other child is width 100% and fills the axis regardless.
        lv_obj_set_flex_align(_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_START);
    } else {
        clear_content();
    }

    header_set_visible(true);
    header_set_title("ADD/ATTACH ITEM");
    build_scan_ui();

    lv_scr_load(_scr);
}
