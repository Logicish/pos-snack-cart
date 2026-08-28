#pragma once
#include <lvgl.h>

// Shared bottom-legend widgets. Every screen spells out what the physical buttons do right
// now, colour-matched to the button itself (see feedback-ux-explicit-legends): the
// Left/Right (and Up/Down) directional pair in C_YELLOW on the top row, Enter in C_GREEN /
// Back in C_RED on the bottom row. Pass "" for a side with no active button.
//
// Was three identical copies of make_legend_row() (screen_pos / screen_add_item / main)
// plus assorted one-off footer labels; consolidated here 2026-08-27.

// Styled transparent flex-column to hold ui_legend_row()s. Caller positions it -- put a
// grow spacer before it in a flex-column layout, or lv_obj_align() it on a plain screen
// (and lv_obj_set_width() if it shouldn't be full width).
lv_obj_t *ui_legend(lv_obj_t *parent);

// One legend row inside a ui_legend() container: a label pinned to each physical edge.
void ui_legend_row(lv_obj_t *legend, const char *left, lv_color_t left_color,
                   const char *right, lv_color_t right_color);

// The ubiquitous scan-prompt footer: a lone red "Cancel" pinned bottom-right, for screens
// where Back is the only active button. Standalone -- hand it the screen object.
void ui_footer_cancel(lv_obj_t *parent);
