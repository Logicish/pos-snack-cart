#include "ui.h"
#include "theme.h"

lv_obj_t *ui_legend(lv_obj_t *parent) {
    lv_obj_t *legend = lv_obj_create(parent);
    lv_obj_set_size(legend, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(legend, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(legend, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(legend, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(legend, 4, LV_PART_MAIN);
    lv_obj_set_layout(legend, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(legend, LV_OBJ_FLAG_SCROLLABLE);
    return legend;
}

// One label pinned to each edge, matching where the physical button it documents sits.
void ui_legend_row(lv_obj_t *legend, const char *left, lv_color_t left_color,
                   const char *right, lv_color_t right_color) {
    lv_obj_t *row = lv_obj_create(legend);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(row);
    lv_label_set_text(l, left);
    lv_obj_set_style_text_color(l, left_color, LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);

    lv_obj_t *r = lv_label_create(row);
    lv_label_set_text(r, right);
    lv_obj_set_style_text_color(r, right_color, LV_PART_MAIN);
    lv_obj_set_style_text_font(r, &lv_font_montserrat_16, LV_PART_MAIN);
}

void ui_footer_cancel(lv_obj_t *parent) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "Cancel");
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_RED), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_RIGHT, -16, -14);
}
