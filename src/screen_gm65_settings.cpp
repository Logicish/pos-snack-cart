#include "screen_gm65_settings.h"
#include "screen_gm65_test.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the GM65 Settings sub-screen declared in
            screen_gm65_settings.h.
*/

extern HardwareSerial scanner;  // owned by main.cpp; this screen only ever writes to it

#define FOOTER_H  52
#define EDIT_ROW_COUNT 3
#define ROW_BUZZER 0
#define ROW_LED    1
#define ROW_FREQ   2

static lv_obj_t *_scr;
static lv_obj_t *_hex_lbl;
static lv_obj_t *_decoded_lbl;      // read-only Working Mode / Light / Aim summary
static lv_obj_t *_rows[EDIT_ROW_COUNT];
static lv_obj_t *_row_lbls[EDIT_ROW_COUNT];
static int        _cursor;
static int        _prev_cursor = -1;

// Last known state of the two registers this screen cares about, decoded from the most
// recent real read (or this project's own locked defaults until the first reply lands, so
// something reasonable shows immediately rather than a blank screen).
static uint8_t _reg0000 = 0x57;
static uint8_t _reg000A = 0x64;

// Sends a single-register write command to the GM65.
static void write_reg(uint8_t addr_hi, uint8_t addr_lo, uint8_t data) {
    const uint8_t cmd[9] = {0x7E, 0x00, 0x08, 0x01, addr_hi, addr_lo, data, 0xAB, 0xCD};
    scanner.write(cmd, 9);
}

// Sends a multi-register read command to the GM65 -- the reply arrives later, over UART.
static void read_reg(uint8_t addr_hi, uint8_t addr_lo, uint8_t count) {
    const uint8_t cmd[9] = {0x7E, 0x00, 0x07, 0x01, addr_hi, addr_lo, count, 0xAB, 0xCD};
    scanner.write(cmd, 9);
}

// Same fixed command as screen_gm65_test.cpp's save_to_eeprom() -- duplicated locally
// rather than shared across files, matching how write_reg()/read_reg() above are already
// duplicated too (this project hasn't factored the GM65 protocol into a shared module).
static void save_to_eeprom() {
    const uint8_t cmd[9] = {0x7E, 0x00, 0x09, 0x01, 0x00, 0x00, 0x00, 0xDE, 0xC8};
    scanner.write(cmd, 9);
}

// ── register 0x0000 field decode ────────────────────────────────────────────────────
// Layout verified on real hardware (see screen_gm65_test.cpp's header comment): working
// mode bits0-1, light bits2-3, aim bits4-5, buzzer-mute bit6, LED indicator bit7.
static inline uint8_t field_working_mode(uint8_t r) { return r & 0x03; }
static inline uint8_t field_light(uint8_t r)         { return (r >> 2) & 0x03; }
static inline uint8_t field_aim(uint8_t r)           { return (r >> 4) & 0x03; }
static inline bool    field_buzzer_on(uint8_t r)     { return (r >> 6) & 0x01; }
static inline bool    field_led_on(uint8_t r)        { return (r >> 7) & 0x01; }

// Sets or clears one bit of a register byte.
static uint8_t with_bit(uint8_t r, uint8_t bit, bool on) {
    return on ? (r | (1 << bit)) : (r & ~(1 << bit));
}

static const char *WORKING_MODE_NAMES[4] = {
    "Manual", "Command Triggered", "Continuous", "Induction",
};
// Values 2 and 3 both decode to "Always On" per the real manual text for Light; Aim is
// assumed symmetric (same 2-bit-field shape, same class of LED) but that assumption is
// NOT independently hardware-verified the way Buzzer/LED-indicator/Frequency below are --
// shown read-only for exactly this reason, not offered as an editable option.
static const char *LIGHT_AIM_NAMES[4] = { "Off", "Standard", "Always On", "Always On" };

// Active mode (0x00) removed from the cycle 2026-09-14 -- confirmed on real hardware back
// on 2026-08-26 to just produce a click on this unit's buzzer, not a beep, full stop, not
// worth cycling through. If a live register ever reads back 0x00 anyway (e.g. before the
// first Restore Defaults after a factory reset), sound_mode_index_for() below falls back
// to the passive-2000Hz slot rather than a now-nonexistent "Active" entry.
struct SoundMode { uint8_t value; const char *label; };
static const SoundMode SOUND_MODES[] = {
    {0x64, "Passive 2000Hz (default)"},
    {0x32, "Passive 1000Hz"},
    {0x7D, "Passive 2500Hz"},
    {0xC8, "Passive 4000Hz"},
};
#define SOUND_MODE_COUNT (sizeof(SOUND_MODES) / sizeof(SOUND_MODES[0]))

static size_t sound_mode_index_for(uint8_t value) {
    for (size_t i = 0; i < SOUND_MODE_COUNT; i++) {
        if (SOUND_MODES[i].value == value) return i;
    }
    return 0;  // unrecognized raw value (e.g. the old Active/0x00) -- fall back to the locked default's slot
}

// ── back to the parent Scanner screen ───────────────────────────────────────────────
static void cb_back() { screen_gm65_test_push(); }

// ── rendering ────────────────────────────────────────────────────────────────────────
// A real read reply is {Head2: 0x02 0x00} {Types: 1 byte} {Lens: 1 byte} {Datas: N bytes}
// {CRC: 2 bytes} -- 4 header bytes before the actual register data starts, verified
// against the real manual's own worked example (reading 1 byte at 0x000A replies
// "0x02 0x00 0x00 0x01 0x3E 0xE4 0xAC" -- the 0x3E is the 5th byte, index 4). For our
// 14-byte read that's 4 header + 14 data + 2 CRC = 20 bytes total, matching this
// project's own already-documented "20-byte frame" note. REG_DATA_OFFSET was 0 until
// 2026-09-14 (a real bug, not a display quirk -- it decoded header/CRC bytes as if they
// were register 0x0000, producing plausible-looking but wrong settings) -- caught when
// the displayed settings didn't match what was actually locked in.
#define REG_DATA_OFFSET 4

static void refresh_hex_dump(const uint8_t *data, size_t len) {
    char hex[64] = "";
    size_t avail = len > REG_DATA_OFFSET ? len - REG_DATA_OFFSET : 0;
    size_t shown = avail < 14 ? avail : 14;
    for (size_t i = 0; i < shown; i++) {
        char b[4];
        snprintf(b, sizeof(b), "%02X ", data[REG_DATA_OFFSET + i]);
        strncat(hex, b, sizeof(hex) - strlen(hex) - 1);
    }
    char out[96];
    snprintf(out, sizeof(out), "Raw (reg0000-000D): %s", shown > 0 ? hex : "(no reply yet)");
    lv_label_set_text(_hex_lbl, out);
}

// Redraws the read-only Working Mode/Light/Aim summary line.
static void refresh_decoded_summary() {
    char buf[96];
    snprintf(buf, sizeof(buf), "Working Mode: %s\nLight: %s   Aim: %s",
             WORKING_MODE_NAMES[field_working_mode(_reg0000)],
             LIGHT_AIM_NAMES[field_light(_reg0000)],
             LIGHT_AIM_NAMES[field_aim(_reg0000)]);
    lv_label_set_text(_decoded_lbl, buf);
}

// Redraws the three editable row labels (Buzzer/LED/Frequency).
static void refresh_edit_rows() {
    char buf[32];

    snprintf(buf, sizeof(buf), "Buzzer: %s", field_buzzer_on(_reg0000) ? "On" : "Off");
    lv_label_set_text(_row_lbls[ROW_BUZZER], buf);

    snprintf(buf, sizeof(buf), "LED Indicator: %s", field_led_on(_reg0000) ? "On" : "Off");
    lv_label_set_text(_row_lbls[ROW_LED], buf);

    size_t idx = sound_mode_index_for(_reg000A);
    snprintf(buf, sizeof(buf), "Frequency: %s", SOUND_MODES[idx].label);
    lv_label_set_text(_row_lbls[ROW_FREQ], buf);
}

// Redraws both the decoded summary and the editable rows.
static void refresh_all() {
    refresh_decoded_summary();
    refresh_edit_rows();
}

// ── row edit actions ─────────────────────────────────────────────────────────────────
// Each writes the whole byte back live (RAM-only, same as this project's other GM65
// toggles) and updates the in-memory copy + its own row label immediately -- optimistic,
// not re-read-verified, matching how Toggle Scan Mode/Sound Mode already work on the
// parent Scanner screen. Right (Save to ROM) is the separate, explicit step that persists
// whatever's currently set here across a real power cycle.
static void act_toggle_buzzer() {
    _reg0000 = with_bit(_reg0000, 6, !field_buzzer_on(_reg0000));
    write_reg(0x00, 0x00, _reg0000);
    refresh_all();
}

static void act_toggle_led() {
    _reg0000 = with_bit(_reg0000, 7, !field_led_on(_reg0000));
    write_reg(0x00, 0x00, _reg0000);
    refresh_all();
}

static void act_cycle_freq() {
    size_t idx = (sound_mode_index_for(_reg000A) + 1) % SOUND_MODE_COUNT;
    _reg000A = SOUND_MODES[idx].value;
    write_reg(0x00, 0x0A, _reg000A);
    refresh_all();
}

typedef void (*RowAction)();
static RowAction ROW_ACTIONS[EDIT_ROW_COUNT] = { act_toggle_buzzer, act_toggle_led, act_cycle_freq };

// ── refresh (re-read from hardware) + save to ROM ───────────────────────────────────
static void act_refresh() {
    refresh_hex_dump(nullptr, 0);
    read_reg(0x00, 0x00, 0x0E);  // registers 0x0000-0x000D in one frame
}

// Right: persists the current live register values to the module's flash.
static void act_save_to_rom() {
    save_to_eeprom();
    refresh_hex_dump(nullptr, 0);  // stale until the next explicit refresh -- avoid implying this re-read anything
    lv_label_set_text(_hex_lbl, "Saved current settings to flash. Left = Refresh to confirm.");
}

// ── list plumbing ────────────────────────────────────────────────────────────────────
// Highlights the currently-selected editable row.
static void refresh_cursor() {
    if (_prev_cursor >= 0 && _prev_cursor != _cursor) {
        lv_obj_set_style_bg_opa(_rows[_prev_cursor], LV_OPA_TRANSP, LV_PART_MAIN);
    }
    lv_obj_set_style_bg_opa(_rows[_cursor], LV_OPA_30, LV_PART_MAIN);
    _prev_cursor = _cursor;
}

static void cb_up()    { _cursor = (_cursor - 1 + EDIT_ROW_COUNT) % EDIT_ROW_COUNT; refresh_cursor(); }  // Up: previous row, wrapping
static void cb_down()  { _cursor = (_cursor + 1) % EDIT_ROW_COUNT; refresh_cursor(); }                   // Down: next row, wrapping
static void cb_enter() { ROW_ACTIONS[_cursor](); }                                                       // Enter: runs the selected row's toggle/cycle action

// Consumes one raw reply off the scanner UART while this screen is active, decoding it
// into the hex dump + summary + edit rows. See screen_gm65_settings.h for why this exists.
bool screen_gm65_settings_capture(const char *data, size_t len) {
    if (!_scr || lv_scr_act() != _scr) return false;

    if (len >= (size_t)(REG_DATA_OFFSET + 11)) {  // header + at least through register 0x000A
        _reg0000 = (uint8_t)data[REG_DATA_OFFSET + 0];
        _reg000A = (uint8_t)data[REG_DATA_OFFSET + 10];
    }
    refresh_hex_dump((const uint8_t *)data, len);
    refresh_all();
    return true;
}

// Loads the GM65 Settings screen and kicks off a live register read.
void screen_gm65_settings_push() {
    _cursor = 0;

    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_clear_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);

        // One flex column spanning header to footer, hex dump + decoded summary at their
        // natural (wrapped) height, then the edit-row list with flex_grow=1 to soak up
        // whatever's left -- guarantees everything fits with no scrolling regardless of
        // how many lines the hex dump wraps to, rather than the fixed-140px guess this
        // had originally (which could run short and force the whole screen to scroll).
        lv_obj_t *content = lv_obj_create(_scr);
        lv_obj_set_size(content, SCREEN_W, SCREEN_H - HDR_H - FOOTER_H);
        lv_obj_align(content, LV_ALIGN_TOP_MID, 0, HDR_H);
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(content, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(content, 12, LV_PART_MAIN);
        lv_obj_set_style_pad_row(content, 8, LV_PART_MAIN);
        lv_obj_set_layout(content, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

        _hex_lbl = lv_label_create(content);
        lv_label_set_long_mode(_hex_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_hex_lbl, SCREEN_W - 24);
        lv_obj_set_style_text_color(_hex_lbl, lv_color_hex(C_DIM), LV_PART_MAIN);
        lv_obj_set_style_text_font(_hex_lbl, &lv_font_montserrat_14, LV_PART_MAIN);

        _decoded_lbl = lv_label_create(content);
        lv_label_set_long_mode(_decoded_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(_decoded_lbl, SCREEN_W - 24);
        lv_obj_set_style_text_color(_decoded_lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_style_text_font(_decoded_lbl, &lv_font_montserrat_14, LV_PART_MAIN);

        lv_obj_t *list = lv_obj_create(content);
        lv_obj_set_width(list, LV_PCT(100));
        lv_obj_set_flex_grow(list, 1);
        lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_row(list, 8, LV_PART_MAIN);
        lv_obj_set_layout(list, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

        for (int i = 0; i < EDIT_ROW_COUNT; i++) {
            lv_obj_t *row = lv_obj_create(list);
            lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
            lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
            lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(row, 12, LV_PART_MAIN);
            lv_obj_set_style_bg_color(row, lv_color_hex(C_CYAN), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
            _rows[i] = row;

            lv_obj_t *lbl = lv_label_create(row);
            lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), LV_PART_MAIN);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
            _row_lbls[i] = lbl;
        }

        lv_obj_t *legend = ui_legend(_scr);
        lv_obj_set_width(legend, SCREEN_W - 24);
        lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
        char move_lbl[24], refresh_lbl[24], save_lbl[24];
        snprintf(move_lbl, sizeof(move_lbl), "%s%s Move", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
        snprintf(refresh_lbl, sizeof(refresh_lbl), "%s Refresh", LV_SYMBOL_LEFT);
        snprintf(save_lbl, sizeof(save_lbl), "Save ROM %s", LV_SYMBOL_RIGHT);
        ui_legend_row(legend, "", lv_color_hex(C_TEXT), move_lbl, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, refresh_lbl, lv_color_hex(C_YELLOW), save_lbl, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, "Change", lv_color_hex(C_GREEN), "Back", lv_color_hex(C_RED));
    }

    header_set_visible(true);
    header_set_title("GM65 SETTINGS");
    refresh_hex_dump(nullptr, 0);
    refresh_all();
    refresh_cursor();

    ButtonHandlers h;
    h.up    = cb_up;
    h.down  = cb_down;
    h.left  = act_refresh;
    h.right = act_save_to_rom;
    h.enter = cb_enter;
    h.back  = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);

    // Kick off a real read every time this screen opens -- shows the locked-default
    // assumption immediately (above) but replaces it with the actual live values the
    // moment a reply lands, same "assume then confirm" pattern as elsewhere in this file.
    read_reg(0x00, 0x00, 0x0E);
}
