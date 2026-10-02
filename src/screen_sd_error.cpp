#include "screen_sd_error.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "session_timer.h"
#include "users.h"
#include "db.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>
#include <SD.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the boot-time blocking error screen declared in
            screen_sd_error.h.
*/

static lv_obj_t *_scr;
static lv_obj_t *_lbl;

// True if this screen is the one currently on screen.
bool screen_sd_error_is_active() {
    return _scr && lv_scr_act() == _scr;
}

// Diagnoses which of the three real states this is, rather than a single generic message
// -- "insert a card" is actively wrong advice if a card IS present and it's the database
// itself that's the problem.
static void refresh_message() {
    String msg;

    if (SD.cardType() == CARD_NONE) {
        msg = "No SD card detected.\n\nThis device needs the SD card to do anything -- "
              "insert one and press Enter to retry.";
    } else if (!db_handle()) {
        msg = "SD card found, but the database could not be opened -- even after trying "
              "every backup copy.\n\nTry a different SD card, or press Enter to retry.";
    } else {
        msg = "SD card found and the database opened, but the full check on it still "
              "failed, and no backup copy passed it either.\n\nPress Enter to retry, or try a different SD card.";
    }

    lv_label_set_text(_lbl, msg.c_str());
}

static void cb_retry() {
    if (boot_try_init_db()) {
        // Mirror setup()'s own branching exactly -- a recovered/swapped-in DB can still be
        // admin-less (e.g. a backup that predates the admin row, or a blank card), which
        // would otherwise fall through to screen_splash_push() -> IDLE with no way back to
        // Admin Login. See screen_setup_wizard.h for why that's a real dead end.
        if (!users_has_admin()) {
            screen_setup_wizard_push();
        } else {
            screen_splash_push();  // recovered -- fall through into the normal boot flow
        }
    } else {
        refresh_message();  // still broken -- refresh in case the specific cause changed
                             // (e.g. a different, also-bad card was swapped in)
    }
}

// Loads the blocking error screen and diagnoses the current failure.
void screen_sd_error_push() {
    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        lv_obj_t *content = lv_obj_create(_scr);
        lv_obj_set_size(content, SCREEN_W, SCREEN_H);
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(content, 20, LV_PART_MAIN);
        // The footer legend is this flex column's last child, so pad_all's bottom inset
        // was also its distance from the true screen edge -- overridden separately to
        // match the ~6px margin every explicitly-aligned legend elsewhere uses (see
        // screen_item_edit.cpp's identical fix).
        lv_obj_set_style_pad_bottom(content, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_row(content, 12, LV_PART_MAIN);
        lv_obj_set_layout(content, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

        lv_obj_t *title = lv_label_create(content);
        lv_label_set_text(title, "SD / Database Error");
        lv_obj_set_style_text_color(title, lv_color_hex(C_RED), LV_PART_MAIN);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
        lv_obj_set_width(title, LV_PCT(100));

        _lbl = lv_label_create(content);
        lv_label_set_long_mode(_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_lbl, LV_PCT(100));
        lv_obj_set_style_text_color(_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(_lbl, &lv_font_montserrat_16, LV_PART_MAIN);

        lv_obj_t *grow = lv_obj_create(content);
        lv_obj_set_size(grow, LV_PCT(100), 1);
        lv_obj_set_style_bg_opa(grow, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(grow, 0, LV_PART_MAIN);
        lv_obj_set_flex_grow(grow, 1);

        lv_obj_t *legend = ui_legend(content);
        lv_obj_set_width(legend, LV_PCT(100));
        ui_legend_row(legend, "Retry", lv_color_hex(C_GREEN), "", lv_color_hex(C_TEXT));
    }

    refresh_message();
    header_set_visible(false);  // this screen speaks for itself, no title bar needed

    ButtonHandlers h;
    h.enter = cb_retry;
    buttons_set_handlers(h);
    session_timer_disarm();  // nothing to log out of, and IDLE would be equally broken --
                              // stay put and visible rather than silently swapping away
                              // from the diagnosis after the auto-logout timeout

    lv_scr_load(_scr);
}
