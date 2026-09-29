/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the first-boot/recovery setup wizard declared in
            screen_setup_wizard.h.
  Notes---- Everything below the character-set section is a self-contained copy of
            screen_enroll.cpp's badge-scan/name-wheel/confirm/re-scan-confirm widget,
            adapted so a successful add always creates an admin and always loops
            back into this screen's own scan prompt instead of returning to Add User.
*/
#include "screen_setup_wizard.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "users.h"
#include "buttons.h"
#include "session_timer.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>
#include <string.h>

// ── character set ─────────────────────────────────────────────────────────────
// Indices: 0-25 = A-Z, 26 = space, 27 = dash -- same as screen_enroll.cpp's wheel.
static const char CHARSET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ -";
#define CHARSET_LEN 28
#define NAME_LEN    11
#define IDX_DASH    27
#define IDX_A       0

// ── state ─────────────────────────────────────────────────────────────────────
// ST_SCAN is this screen's own top-level state (armed for a new badge, not part of
// screen_enroll.cpp's set) -- the rest mirror that file's ST_FIRST/ST_LAST/ST_CONFIRM/
// ST_SCAN (renamed ST_RESCAN here to avoid confusion with this screen's own ST_SCAN).
typedef enum { ST_SCAN, ST_FIRST, ST_LAST, ST_CONFIRM, ST_RESCAN } WizardState;

static char        _badge_id[48];
static int8_t      _idx[2][NAME_LEN];
static uint8_t     _cursor;
static uint8_t     _active_row;
static WizardState _state;
static int          _created_count;  // how many admins this wizard has created this boot

// ── LVGL handles ──────────────────────────────────────────────────────────────
static lv_obj_t *_scr;
static lv_obj_t *_content;
static lv_obj_t *_cell[2][NAME_LEN];
static lv_obj_t *_cell_lbl[2][NAME_LEN];
static lv_obj_t *_scan_status_lbl;   // ST_RESCAN's "wrong badge" status line
static lv_obj_t *_scan_prompt_lbl;   // ST_SCAN's prompt -- also doubles as its status line

// ── helpers ───────────────────────────────────────────────────────────────────

// Reads one name-wheel row into a trimmed string.
static void get_name(uint8_t row, char *out) {
    int last_real = -1;
    for (int i = 0; i < NAME_LEN; i++) {
        char c = CHARSET[(uint8_t)_idx[row][i]];
        if (c != '-' && c != ' ') last_real = i;
    }
    for (int i = 0; i <= last_real; i++) out[i] = CHARSET[(uint8_t)_idx[row][i]];
    out[last_real + 1] = '\0';
}

// True if the given name-wheel row has at least one real character typed.
static bool name_valid(uint8_t row) {
    char buf[NAME_LEN + 1];
    get_name(row, buf);
    return buf[0] != '\0';
}

// Redraws one name-wheel cell, highlighting it if it's the cursor.
static void refresh_cell(uint8_t row, uint8_t col) {
    if (!_cell_lbl[row][col]) return;

    char buf[2] = { CHARSET[(uint8_t)_idx[row][col]], '\0' };
    lv_label_set_text(_cell_lbl[row][col], buf);

    bool is_cursor = ((int)_active_row == row && (int)_cursor == col);
    bool is_active = ((int)_active_row == row);

    lv_color_t color = is_cursor ? lv_color_hex(C_CYAN)
                     : is_active  ? lv_color_hex(C_TEXT)
                                  : lv_color_hex(C_DIM);
    lv_obj_set_style_text_color(_cell_lbl[row][col], color, LV_PART_MAIN);

    lv_border_side_t side = is_cursor ? LV_BORDER_SIDE_BOTTOM : LV_BORDER_SIDE_NONE;
    lv_obj_set_style_border_side(_cell[row][col], side, LV_PART_MAIN);
    lv_obj_set_style_border_width(_cell[row][col], is_cursor ? 2 : 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(_cell[row][col], lv_color_hex(C_CYAN), LV_PART_MAIN);
}

// Redraws every name-wheel cell.
static void refresh_cells() {
    for (int row = 0; row < 2; row++)
        for (int col = 0; col < NAME_LEN; col++)
            refresh_cell(row, col);
}

// ── forward declarations ──────────────────────────────────────────────────────
static void build_scan_ui();
static void build_input_ui();
static void build_confirm_ui();
static void build_rescan_ui();

static void clear_content() {
    lv_obj_clean(_content);
    memset(_cell,     0, sizeof(_cell));
    memset(_cell_lbl, 0, sizeof(_cell_lbl));
    _scan_status_lbl = nullptr;
    _scan_prompt_lbl = nullptr;
}

// Shared by build_scan_ui() (fresh) and the "already enrolled" rejection (in place, no
// rebuild) -- the normal prompt text for wherever setup currently stands.
static void set_default_scan_prompt() {
    if (!_scan_prompt_lbl) return;
    if (_created_count == 0) {
        lv_label_set_text(_scan_prompt_lbl,
            "No admin account exists yet.\n\n"
            "Scan a badge to create\nthe first admin.");
    } else {
        char buf[96];
        snprintf(buf, sizeof(buf),
            "%d admin%s created.\n\n"
            "Scan another badge to add\nanother, or press Back\nto finish setup.",
            _created_count, _created_count == 1 ? "" : "s");
        lv_label_set_text(_scan_prompt_lbl, buf);
    }
}

// ============ ST_SCAN (top-level scan prompt) ============

static void cb_scan_back() {
    // Inert until at least one admin exists -- see the header comment for why. Once safe,
    // Back hands off to the normal boot flow instead of leaving this screen reachable again.
    if (_created_count > 0) screen_splash_push();
}

// Builds the ST_SCAN prompt UI.
static void build_scan_ui() {
    _scan_prompt_lbl = lv_label_create(_content);
    lv_label_set_long_mode(_scan_prompt_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(_scan_prompt_lbl, LV_PCT(100));
    lv_obj_set_style_text_color(_scan_prompt_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(_scan_prompt_lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_align(_scan_prompt_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    set_default_scan_prompt();

    lv_obj_t *grow = lv_obj_create(_content);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);

    // No legend at all until it's actually safe to leave -- an empty footer here is
    // deliberate, not an oversight (see cb_scan_back()).
    if (_created_count > 0) {
        lv_obj_t *legend = ui_legend(_content);
        ui_legend_row(legend, "", lv_color_hex(C_TEXT), "Finish Setup", lv_color_hex(C_RED));
    }

    ButtonHandlers h;
    h.back = cb_scan_back;
    h.wantsScanner = true;  // waiting for the recovery admin's new badge
    buttons_set_handlers(h);
    session_timer_disarm();  // recovery/first-boot wizard, nothing logged in to auto-log-out
                              // of -- an idle timeout here would strand the device admin-less
                              // exactly like screen_idle_load() would (see screens.h)
}

// ============ ST_FIRST / ST_LAST (name wheel) ============

// Builds one "Enter First/Last Name:" hint label.
static lv_obj_t *make_section_label(lv_obj_t *parent, const char *text) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(lbl, LV_PCT(100));
    return lbl;
}

// Builds one row of character cells for the name wheel.
static void build_char_row(lv_obj_t *parent, uint8_t row) {
    lv_obj_t *row_cont = lv_obj_create(parent);
    lv_obj_set_size(row_cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row_cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row_cont, 0, LV_PART_MAIN);
    lv_obj_set_layout(row_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_cont, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row_cont, LV_OBJ_FLAG_SCROLLABLE);

    for (int col = 0; col < NAME_LEN; col++) {
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

// Left: moves the name-wheel cursor left within the current row.
static void cb_left() {
    if (_cursor > 0) {
        uint8_t old = _cursor;
        _cursor--;
        refresh_cell(_active_row, old);
        refresh_cell(_active_row, _cursor);
    }
}

// Right: moves the name-wheel cursor right within the current row.
static void cb_right() {
    if (_cursor < NAME_LEN - 1) {
        uint8_t old = _cursor;
        _cursor++;
        refresh_cell(_active_row, old);
        refresh_cell(_active_row, _cursor);
    }
}

// Up: cycles the letter under the cursor forward.
static void cb_up() {
    _idx[_active_row][_cursor] = (_idx[_active_row][_cursor] + 1) % CHARSET_LEN;
    refresh_cell(_active_row, _cursor);
}

// Down: cycles the letter under the cursor backward.
static void cb_down() {
    _idx[_active_row][_cursor] = (_idx[_active_row][_cursor] + CHARSET_LEN - 1) % CHARSET_LEN;
    refresh_cell(_active_row, _cursor);
}

// Enter: advances first name -> last name -> confirm.
static void cb_next() {
    if (_active_row == 0) {
        if (!name_valid(0)) return;
        _active_row = 1;
        _cursor = 0;
        refresh_cells();
    } else {
        if (!name_valid(1)) return;
        _state = ST_CONFIRM;
        clear_content();
        build_confirm_ui();
    }
}

// From the first field, cancels this add attempt entirely and returns to the scan prompt
// (not screen_idle_load() -- that would re-hit the deadlock this screen exists to prevent).
static void cb_name_back() {
    if (_active_row == 1) {
        _active_row = 0;
        _cursor = 0;
        refresh_cells();
    } else {
        _state = ST_SCAN;
        clear_content();
        build_scan_ui();
    }
}

// Builds the ST_FIRST/ST_LAST (name wheel) UI.
static void build_input_ui() {
    make_section_label(_content, "Enter First Name:");
    build_char_row(_content, 0);

    lv_obj_t *spacer = lv_obj_create(_content);
    lv_obj_set_size(spacer, LV_PCT(100), 16);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(spacer, 0, LV_PART_MAIN);

    make_section_label(_content, "Enter Last Name:");
    build_char_row(_content, 1);

    lv_obj_t *grow = lv_obj_create(_content);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);

    lv_obj_t *legend = ui_legend(_content);
    char cursor_lbl[28], letter_lbl[28];
    snprintf(cursor_lbl, sizeof(cursor_lbl), "%s%s Cursor", LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT);
    snprintf(letter_lbl, sizeof(letter_lbl), "Letter %s%s", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
    ui_legend_row(legend, cursor_lbl, lv_color_hex(C_YELLOW), letter_lbl, lv_color_hex(C_YELLOW));
    ui_legend_row(legend, "Next", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));

    refresh_cells();

    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.left  = cb_left;
    h.right = cb_right;
    h.enter = cb_next;
    h.back  = cb_name_back;
    buttons_set_handlers(h);
    session_timer_disarm();  // see build_scan_ui()'s comment
}

// ============ ST_CONFIRM ============

// Enter: proceeds to the re-scan confirm step.
static void cb_confirm_yes() {
    _state = ST_RESCAN;
    clear_content();
    build_rescan_ui();
}

// Back: returns to editing the last name.
static void cb_confirm_no() {
    _state = ST_LAST;
    _active_row = 1;
    _cursor = 0;
    clear_content();
    build_input_ui();
}

// Builds the ST_CONFIRM UI.
static void build_confirm_ui() {
    char first[NAME_LEN + 1], last[NAME_LEN + 1];
    get_name(0, first);
    get_name(1, last);

    char full[NAME_LEN * 2 + 4];
    snprintf(full, sizeof(full), "%s %s", first, last);

    lv_obj_t *prompt = lv_label_create(_content);
    lv_label_set_text(prompt, "Make this person an admin?");
    lv_obj_set_style_text_color(prompt, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(prompt, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(prompt, LV_PCT(100));
    lv_obj_set_style_text_align(prompt, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *name_lbl = lv_label_create(_content);
    lv_label_set_text(name_lbl, full);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(C_CYAN), LV_PART_MAIN);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_width(name_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *grow = lv_obj_create(_content);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);

    lv_obj_t *legend = ui_legend(_content);
    ui_legend_row(legend, "Yes", lv_color_hex(C_GREEN), "No", lv_color_hex(C_RED));

    ButtonHandlers h;
    h.enter = cb_confirm_yes;
    h.back  = cb_confirm_no;
    buttons_set_handlers(h);
    session_timer_disarm();  // see build_scan_ui()'s comment
}

// ============ ST_RESCAN ============

// Back: returns to the confirm step.
static void cb_rescan_back() {
    _state = ST_CONFIRM;
    clear_content();
    build_confirm_ui();
}

// Builds the ST_RESCAN UI.
static void build_rescan_ui() {
    _scan_status_lbl = nullptr;

    lv_obj_t *prompt = lv_label_create(_content);
    lv_label_set_text(prompt, "Scan the badge again to confirm");
    lv_obj_set_style_text_color(prompt, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(prompt, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_width(prompt, LV_PCT(100));
    lv_obj_set_style_text_align(prompt, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *grow = lv_obj_create(_content);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);

    _scan_status_lbl = lv_label_create(_content);
    lv_label_set_text(_scan_status_lbl, "");
    lv_obj_set_style_text_color(_scan_status_lbl, lv_color_hex(C_ORANGE), LV_PART_MAIN);
    lv_obj_set_style_text_font(_scan_status_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(_scan_status_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(_scan_status_lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *legend = ui_legend(_content);
    ui_legend_row(legend, "", lv_color_hex(C_TEXT), "Cancel", lv_color_hex(C_RED));

    ButtonHandlers h;
    h.back = cb_rescan_back;
    h.wantsScanner = true;  // waiting for the same badge again, to confirm it
    buttons_set_handlers(h);
    session_timer_disarm();  // see build_scan_ui()'s comment
}

// ============ public ============

// True if this screen is the one currently on screen.
bool screen_setup_wizard_is_active() {
    return _scr && lv_scr_act() == _scr;
}

// Consumes a scan while this screen is active -- routes it per the current sub-state
// (new badge in ST_SCAN, confirm-match in ST_RESCAN, swallowed otherwise).
bool screen_setup_wizard_on_scan(const char *badge_id) {
    if (!_scr || lv_scr_act() != _scr) return false;

    if (_state == ST_SCAN) {
        if (users_find_by_badge(badge_id)) {
            // Same rejection as Add User's identical check -- an already-enrolled badge
            // (e.g. a leftover non-admin row from before every admin got deleted) can be
            // promoted from Edit Users once setup finishes; this wizard only creates new
            // admins from new badges, to keep the recovery path simple.
            if (_scan_prompt_lbl) {
                lv_label_set_text(_scan_prompt_lbl,
                    "That badge is already\nenrolled.\n\nScan a different badge.");
            }
            return true;
        }

        strncpy(_badge_id, badge_id, sizeof(_badge_id) - 1);
        _badge_id[sizeof(_badge_id) - 1] = '\0';

        _state      = ST_FIRST;
        _active_row = 0;
        _cursor     = 0;
        for (int row = 0; row < 2; row++) {
            _idx[row][0] = IDX_A;
            for (int col = 1; col < NAME_LEN; col++) _idx[row][col] = IDX_DASH;
        }
        clear_content();
        build_input_ui();
        return true;
    }

    if (_state == ST_RESCAN) {
        if (strcmp(badge_id, _badge_id) == 0) {
            char first[NAME_LEN + 1], last[NAME_LEN + 1];
            get_name(0, first);
            get_name(1, last);
            int user_id = users_create(_badge_id, first, last, /*admin=*/true);
            if (user_id < 0) {
                Serial.println("[SETUP] admin create failed");
                if (_scan_status_lbl) lv_label_set_text(_scan_status_lbl, "Save failed - try again");
                return true;
            }
            Serial.printf("[SETUP] Created admin %d: %s %s\n", user_id, first, last);
            _created_count++;
            _state = ST_SCAN;
            clear_content();
            build_scan_ui();
        } else {
            Serial.println("[SETUP] Badge mismatch during confirm scan");
            if (_scan_status_lbl) lv_label_set_text(_scan_status_lbl, "Wrong badge - try again");
        }
        return true;
    }

    // ST_FIRST / ST_LAST / ST_CONFIRM -- not waiting on a scan, but this screen owns every
    // scan while it's active, so swallow it rather than letting it fall through to
    // main.cpp's normal badge lookup.
    return true;
}

// Loads the setup wizard, always starting at ST_SCAN with the created-count reset.
void screen_setup_wizard_push() {
    _state         = ST_SCAN;
    _created_count = 0;

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
        lv_obj_set_flex_align(_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(_content, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        clear_content();
    }

    header_set_visible(true);
    header_set_title("SETUP");
    header_set_current_user("");
    build_scan_ui();

    lv_scr_load(_scr);
}
