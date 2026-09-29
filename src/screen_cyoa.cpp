#include "screen_cyoa.h"
#include "screens.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>
#include <string.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the shared CYOA runner declared in screen_cyoa.h.
  Notes---- First pass -- built to prove the engine (cyoa_engine.h) actually works, not
            yet run against real story content. See story_test_data.h for the smoke-test
            story this was built and tested against; Quest for Coffee/Jinx/Dragons (the
            real planned stories, see snack_cart_pos.md) are still unwritten.
*/

static lv_obj_t *_scr;
static lv_obj_t *_content;

static const CyoaStory *_story;
static bool _has_item[CYOA_MAX_ITEMS];
static int  _stat_value[CYOA_MAX_STATS];
static int  _current_node;

// Active encounter pool, if any -- see cyoa_engine.h's CyoaPool comment. _pool_index is
// the room about to be (or just) resolved; _pool_hits_left counts down as rooms roll hits.
static const CyoaPool *_pool;
static int              _pool_index;
static int              _pool_hits_left;

static void render_node();

// Rolls the pool's room at _pool_index using the guaranteed-ratio formula (chance =
// hits_remaining / rooms_remaining), forcing a hit once hits_remaining catches up to
// rooms_remaining so the pool's target_hits is always hit exactly, never over/under.
// Advances _pool_index/_pool_hits_left and returns the node id to load.
static int pool_advance() {
    int rooms_left = _pool->room_count - _pool_index;
    bool hit;
    if (_pool_hits_left <= 0)               hit = false;
    else if (_pool_hits_left >= rooms_left)  hit = true;
    else                                     hit = (int)random(rooms_left) < _pool_hits_left;

    const CyoaPoolRoom &room = _pool->rooms[_pool_index];
    int node = hit ? room.hit_node : room.skip_node;
    if (hit) _pool_hits_left--;
    _pool_index++;
    return node;
}

static bool requirement_met(const CyoaRequirement &req) {
    switch (req.type) {
        case CYOA_REQ_HAS_ITEM: return req.index >= 0 && req.index < CYOA_MAX_ITEMS && _has_item[req.index];
        case CYOA_REQ_STAT_GTE: return req.index >= 0 && req.index < CYOA_MAX_STATS && _stat_value[req.index] >= req.value;
        default:                return true;  // CYOA_REQ_NONE
    }
}

static void apply_effect(const CyoaEffect &eff) {
    switch (eff.type) {
        case CYOA_EFF_GRANT_ITEM:
            if (eff.index >= 0 && eff.index < CYOA_MAX_ITEMS) _has_item[eff.index] = true;
            break;
        case CYOA_EFF_CONSUME_ITEM:
            if (eff.index >= 0 && eff.index < CYOA_MAX_ITEMS) _has_item[eff.index] = false;
            break;
        case CYOA_EFF_ADJUST_STAT:
            if (eff.index >= 0 && eff.index < CYOA_MAX_STATS) _stat_value[eff.index] += eff.value;
            break;
        default: break;  // CYOA_EFF_NONE
    }
}

// One shared handler for all 5 choice buttons -- looks up which of the current node's
// choices is bound to the button just pressed, applies its effect, and moves on.
// render_node() only ever binds a button when a matching, requirement-passing choice
// really exists, so this lookup failing would mean a wiring bug, not normal play.
static void choose(CyoaButton btn) {
    const CyoaNode &node = _story->nodes[_current_node];
    for (int i = 0; i < CYOA_MAX_CHOICES; i++) {
        const CyoaChoice &c = node.choices[i];
        if (c.button != btn) continue;
        if (!requirement_met(c.requirement)) continue;
        apply_effect(c.effect);

        if (c.enter_pool) {
            _pool           = c.enter_pool;
            _pool_index     = 0;
            _pool_hits_left = _pool->target_hits;
            _current_node   = pool_advance();
        } else if (c.next_node == CYOA_POOL_CONTINUE && _pool) {
            if (_pool_index < _pool->room_count) {
                _current_node = pool_advance();
            } else {
                _current_node = _pool->exit_node;
                _pool = nullptr;
            }
        } else {
            _current_node = c.next_node;
        }
        render_node();
        return;
    }
}

static void cb_up()    { choose(CYOA_BTN_UP); }
static void cb_down()  { choose(CYOA_BTN_DOWN); }
static void cb_left()  { choose(CYOA_BTN_LEFT); }
static void cb_right() { choose(CYOA_BTN_RIGHT); }
static void cb_enter() { choose(CYOA_BTN_ENTER); }

// Back always leaves the story outright -- never an in-story choice, same safe
// convention every other Extras screen uses (see cyoa_engine.h's CyoaButton comment).
static void cb_exit() {
    screen_extras_return_to_list();
}

static const char *button_symbol(CyoaButton btn) {
    switch (btn) {
        case CYOA_BTN_UP:    return LV_SYMBOL_UP;
        case CYOA_BTN_DOWN:  return LV_SYMBOL_DOWN;
        case CYOA_BTN_LEFT:  return LV_SYMBOL_LEFT;
        case CYOA_BTN_RIGHT: return LV_SYMBOL_RIGHT;
        default:             return "";  // Enter reads fine on its own, no arrow needed
    }
}

// Renders the current node: body text, then one row per available choice (button
// symbol + its label), then a permanent Exit row -- reuses ui_legend()/ui_legend_row()
// (one choice per row, not paired) rather than a custom widget, per the original design
// note's "reuse ui_legend for the labeled choice buttons." Rebuilds _content from
// scratch every node, same clean-and-rebuild pattern as every other multi-state screen
// in this codebase (screen_pos.cpp, screen_check_balance.cpp, etc).
static void render_node() {
    lv_obj_clean(_content);

    const CyoaNode &node = _story->nodes[_current_node];

    lv_obj_t *body = lv_label_create(_content);
    lv_label_set_text(body, node.body);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_style_text_color(body, lv_color_hex(C_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_flex_grow(body, 1);

    lv_obj_t *legend = ui_legend(_content);

    ButtonHandlers h;
    h.back = cb_exit;

    bool any_choice = false;
    for (int i = 0; i < CYOA_MAX_CHOICES; i++) {
        const CyoaChoice &c = node.choices[i];
        if (c.button == CYOA_BTN_NONE) continue;
        if (!requirement_met(c.requirement)) continue;
        any_choice = true;

        char label_buf[40];
        const char *sym = button_symbol(c.button);
        if (sym[0]) snprintf(label_buf, sizeof(label_buf), "%s %s", sym, c.label);
        else        snprintf(label_buf, sizeof(label_buf), "%s", c.label);
        ui_legend_row(legend, "", lv_color_hex(C_TEXT), label_buf, lv_color_hex(C_YELLOW));

        switch (c.button) {
            case CYOA_BTN_UP:    h.up    = cb_up;    break;
            case CYOA_BTN_DOWN:  h.down  = cb_down;  break;
            case CYOA_BTN_LEFT:  h.left  = cb_left;  break;
            case CYOA_BTN_RIGHT: h.right = cb_right; break;
            case CYOA_BTN_ENTER: h.enter = cb_enter; break;
            default: break;
        }
    }

    ui_legend_row(legend, "", lv_color_hex(C_TEXT),
                  any_choice ? "Exit" : "The End - Exit", lv_color_hex(C_RED));

    buttons_set_handlers(h);
}

// Loads the CYOA runner for the given story, always starting fresh from its start node
// with an empty inventory/stat block -- no save/resume, matches every other Extras
// minigame's "each visit is a new run" convention.
void screen_cyoa_push(const CyoaStory *story) {
    _story        = story;
    _current_node = story->start_node;
    memset(_has_item, 0, sizeof(_has_item));
    memset(_stat_value, 0, sizeof(_stat_value));
    _pool = nullptr;  // never inherit an active pool from a previous run/story

    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        _content = lv_obj_create(_scr);
        lv_obj_set_size(_content, SCREEN_W, SCREEN_H - HDR_H);
        lv_obj_align(_content, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_opa(_content, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(_content, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(_content, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_ver(_content, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(_content, 6, LV_PART_MAIN);
        lv_obj_set_style_pad_row(_content, 8, LV_PART_MAIN);
        lv_obj_set_layout(_content, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(_content, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(_content, LV_OBJ_FLAG_SCROLLABLE);
    }

    header_set_visible(true);
    header_set_title(story->title);
    render_node();

    lv_scr_load(_scr);
}
