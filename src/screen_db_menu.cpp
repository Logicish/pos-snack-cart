#include "screen_db_menu.h"
#include "screen_db_view.h"
#include "screen_admin_tools.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "db.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the DB submenu declared in screen_db_menu.h -- the table list
            plus Backup Now / Restore Backup.
*/

// Two rows beyond the table list, added 2026-09-14: Backup Now and Restore Backup, a raw
// file copy of pos.db <-> pos_backup.db (see db.h) rather than any SQLite-level mechanism
// -- this build has already been caught silently not supporting features twice (UPSERT,
// PRAGMA integrity_check, see project memory feedback-no-upsert-syntax), so the backup
// itself deliberately avoids trusting any SQLite API and uses plain SD.h file I/O instead.
// A "Check Integrity" row lived here even more briefly the same day -- removed once
// integrity_check was confirmed to be exactly that kind of silent no-op.
#define MENU_COUNT (DB_TABLE_COUNT + 2)
#define ROW_BACKUP  DB_TABLE_COUNT
#define ROW_RESTORE (DB_TABLE_COUNT + 1)
#define FOOTER_H    52

static lv_obj_t *_scr;
static lv_obj_t *_rows[MENU_COUNT];
static lv_obj_t *_row_lbls[MENU_COUNT];  // ROW_BACKUP/ROW_RESTORE are rewritten in place
static int        _cursor;
static int        _prev_cursor = -1;

// Order for the first DB_TABLE_COUNT rows matches screen_db_view.h's DbTable enum -- same
// index used for both the label here and the query/title in screen_db_view.cpp.
static const char *MENU_LABELS[MENU_COUNT] = {
    "1. Users",
    "2. Items",
    "3. Checkouts",
    "4. Payment Methods",
    "5. Config",
    "6. Backup Now",      // overwritten immediately by update_backup_row_label()
    "7. Restore Backup",
};

// -- restore confirm, a separate small screen (same two-screen idiom as
// screen_gm65_test.cpp's _confirm_scr) since this list is built once with static rows,
// not the clear-and-rebuild pattern used elsewhere for multi-state screens --
static lv_obj_t *_confirm_scr;
static lv_obj_t *_confirm_result_lbl;

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

// Back returns to Advanced Tools.
static void cb_back() {
    screen_admin_tools_push();
}

// Rewrites the Backup Now row's label with the last result, or the default text.
static void update_backup_row_label(const char *result) {
    char buf[40];
    if (result) snprintf(buf, sizeof(buf), "6. Backup: %s", result);
    else        snprintf(buf, sizeof(buf), "6. Backup Now");
    lv_label_set_text(_row_lbls[ROW_BACKUP], buf);
}

// Enter on the Backup Now row: runs the backup and updates its own label with the result.
static void act_backup_now() {
    bool ok = db_backup_now();
    update_backup_row_label(ok ? "done" : "FAILED");
}

static void cb_enter();  // fwd decl -- defined further down, needed by show_db_menu() below

// Switches to (or re-shows) the table-list view.
static void show_db_menu() {
    header_set_visible(true);
    header_set_title("DATABASE");
    refresh_cursor();

    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.enter = cb_enter;
    h.back  = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}

// -- restore confirm screen --

// Back cancels the restore confirm and returns to the table list.
static void cb_restore_confirm_back() {
    show_db_menu();
}

// Overwrites the live pos.db with pos_backup.db -- real data loss for anything written
// since the last backup, hence the confirm gate. Closes/reopens the DB handle around the
// raw file copy, same as the boot-time self-healing path in main.cpp.
static void cb_restore_confirm_yes() {
    db_close();
    bool ok = db_restore_from_backup() && db_init() && db_sanity_check();
    lv_label_set_text(_confirm_result_lbl,
                      ok ? "Restored. Press Back to return."
                         : "Restore failed -- DB may be unavailable. Press Back.");
    lv_obj_set_style_text_color(_confirm_result_lbl,
                                lv_color_hex(ok ? C_GREEN : C_RED), LV_PART_MAIN);
    lv_label_set_text(_row_lbls[ROW_RESTORE], ok ? "7. Restore: done" : "7. Restore: FAILED");

    ButtonHandlers h;
    h.back = cb_restore_confirm_back;  // Yes/No both spent -- only a way back remains
    buttons_set_handlers(h);
}

// Builds the restore-confirm screen's content, once.
static void build_restore_confirm_screen() {
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
    // The footer legend is this flex column's last child, so pad_ver's bottom inset was
    // also its distance from the true screen edge -- overridden separately to match the
    // ~6px margin every explicitly-aligned legend elsewhere uses (see
    // screen_item_edit.cpp's identical fix). This screen's own main menu uses
    // ui_legend(_scr) instead, which was never affected.
    lv_obj_set_style_pad_bottom(content, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(content, 10, LV_PART_MAIN);
    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

    lv_obj_t *prompt = lv_label_create(content);
    lv_label_set_long_mode(prompt, LV_LABEL_LONG_WRAP);
    lv_label_set_text(prompt,
        "Restore the newest backup that passes the full check? Anything written "
        "since that backup (a checkout, an edit) will be lost.");
    lv_obj_set_style_text_color(prompt, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(prompt, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(prompt, LV_PCT(100));

    _confirm_result_lbl = lv_label_create(content);
    lv_label_set_long_mode(_confirm_result_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(_confirm_result_lbl, "");
    lv_obj_set_style_text_font(_confirm_result_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_width(_confirm_result_lbl, LV_PCT(100));

    lv_obj_t *grow = lv_obj_create(content);
    lv_obj_set_size(grow, LV_PCT(100), 1);
    lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
    lv_obj_set_flex_grow(grow, 1);

    lv_obj_t *legend = ui_legend(content);
    ui_legend_row(legend, "Restore", lv_color_hex(C_GREEN), "Cancel", lv_color_hex(C_RED));
}

// Enter on the Restore Backup row: opens the confirm screen.
static void cb_restore_open() {
    if (!_confirm_scr) build_restore_confirm_screen();

    lv_label_set_text(_confirm_result_lbl, "");
    header_set_visible(true);
    header_set_title("RESTORE BACKUP");

    ButtonHandlers h;
    h.enter = cb_restore_confirm_yes;
    h.back  = cb_restore_confirm_back;
    buttons_set_handlers(h);

    lv_scr_load(_confirm_scr);
}

// Enter dispatches to whichever row is selected (a table view, backup, or restore).
static void cb_enter() {
    if (_cursor == ROW_BACKUP)       act_backup_now();
    else if (_cursor == ROW_RESTORE) cb_restore_open();
    else                              screen_db_view_push((DbTable)_cursor);
}

// Loads the DB submenu.
void screen_db_menu_push() {
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
            _row_lbls[i] = lbl;
        }

        lv_obj_t *legend = ui_legend(_scr);
        lv_obj_set_width(legend, SCREEN_W - 24);
        lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
        char move_lbl[24];
        snprintf(move_lbl, sizeof(move_lbl), "%s%s Move", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
        ui_legend_row(legend, "", lv_color_hex(C_TEXT), move_lbl, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, "Open", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));
    }

    show_db_menu();
}
