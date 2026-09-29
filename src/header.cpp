#include "header.h"
#include "theme.h"
#include <lvgl.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the fixed top status bar declared in header.h -- a single LVGL
            object on lv_layer_top() so it draws above whichever screen is loaded,
            with two labels (current user / current title) and a show/hide flag.
*/

static lv_obj_t *_current_user_lbl;  // left — replaces the old static "MICU" branding
static lv_obj_t *_title_lbl;         // right — current screen/menu name
static lv_obj_t *_hdr;

// Builds the header bar and its two labels once, on the top LVGL layer.
void header_init() {
    lv_obj_t *hdr = lv_obj_create(lv_layer_top());
    _hdr = hdr;
    lv_obj_set_size(hdr, SCREEN_W, HDR_H);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(C_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(hdr, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(hdr, lv_color_hex(C_CYAN), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(hdr, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(hdr, 0, LV_PART_MAIN);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    _current_user_lbl = lv_label_create(hdr);
    lv_label_set_text(_current_user_lbl, "");
    lv_obj_set_style_text_color(_current_user_lbl, lv_color_hex(C_CYAN), LV_PART_MAIN);
    lv_obj_set_style_text_font(_current_user_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(_current_user_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    _title_lbl = lv_label_create(hdr);
    lv_label_set_text(_title_lbl, "");
    lv_obj_set_style_text_color(_title_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(_title_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(_title_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
}

// Sets the right-side screen/menu title.
void header_set_title(const char *title) {
    lv_label_set_text(_title_lbl, title);
    lv_obj_align(_title_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
}

// Sets the left-side current-user name.
void header_set_current_user(const char *name) {
    lv_label_set_text(_current_user_lbl, name);
    lv_obj_align(_current_user_lbl, LV_ALIGN_LEFT_MID, 0, 0);
}

// Shows or hides the whole header bar.
void header_set_visible(bool visible) {
    if (visible) lv_obj_clear_flag(_hdr, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(_hdr, LV_OBJ_FLAG_HIDDEN);
}
