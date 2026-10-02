#include "screen_boot_log.h"
#include "screen_admin_tools.h"
#include "boot_log.h"
#include "header.h"
#include "theme.h"
#include "buttons.h"
#include "ui.h"
#include <lvgl.h>
#include <Arduino.h>
#include <SD.h>
#include <string.h>

/*
  Author--- LogicishDesigns
  Date----- October 2026
  Function- Implements the Boot Log viewer declared in screen_boot_log.h.
*/

#define FOOTER_H          52
#define BOOT_LOG_VIEW_MAX 100   // newest boots indexed per visit
#define LINE_MAX_LEN      1024  // a boot line is ~300 bytes; anything longer is truncated
#define SCROLL_STEP       60

static lv_obj_t *_scr;
static lv_obj_t *_content;

static uint32_t _offsets[BOOT_LOG_VIEW_MAX];  // ring of line-start offsets, oldest overwritten
static int      _total;     // boot lines in the file (not counting the header)
static int      _indexed;   // how many of those are in _offsets (newest ones)
static int      _head;      // ring slot the NEXT offset would go in
static int      _cur;       // 0 = newest boot shown

// ── reading ──────────────────────────────────────────────────────────────────

// One pass over the file, keeping the start offset of the newest BOOT_LOG_VIEW_MAX lines.
static void index_file() {
    _total = _indexed = _head = 0;
    File f = SD.open(BOOT_LOG_PATH, FILE_READ);
    if (!f) return;

    uint8_t buf[512];
    uint32_t pos = 0;
    bool at_line_start = true;
    bool first_line = true;  // the "time,restart_reason,details" header
    while (true) {
        int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n; i++, pos++) {
            if (at_line_start && buf[i] != '\n' && buf[i] != '\r') {
                if (first_line) {
                    first_line = false;
                } else {
                    _offsets[_head] = pos;
                    _head = (_head + 1) % BOOT_LOG_VIEW_MAX;
                    _total++;
                    if (_indexed < BOOT_LOG_VIEW_MAX) _indexed++;
                }
                at_line_start = false;
            }
            if (buf[i] == '\n') at_line_start = true;
        }
    }
    f.close();
}

// Reads boot `idx` (0 = newest) into out. False if it isn't indexed.
static bool read_boot(int idx, char *out, size_t out_len) {
    if (idx < 0 || idx >= _indexed) return false;
    int slot = (_head - 1 - idx + BOOT_LOG_VIEW_MAX * 2) % BOOT_LOG_VIEW_MAX;
    File f = SD.open(BOOT_LOG_PATH, FILE_READ);
    if (!f) return false;
    f.seek(_offsets[slot]);
    size_t n = 0;
    while (n + 1 < out_len) {
        int c = f.read();
        if (c < 0 || c == '\n' || c == '\r') break;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    f.close();
    return n > 0;
}

// ── field naming / colouring ─────────────────────────────────────────────────

struct FieldName { const char *key; const char *label; const char *unit; };
static const FieldName FIELD_NAMES[] = {
    { "db",                 "Database",        "" },
    { "data",               "Data check",      "" },
    { "slide_free",         "Slide Free files","" },
    { "slide_free_missing", "Slide Free missing", "" },
    { "temp_files_removed", "Temp files removed", "" },
    { "sd_write",           "SD write test",   "" },
    { "sd_write_ms",        "SD write time",   " ms" },
    { "sd_used_kb",         "SD used",         " KB" },
    { "sd_total_mb",        "SD size",         " MB" },
    { "backup",             "Backup",          "" },
    { "backup_ms",          "Backup time",     " ms" },
    { "clock",              "Clock",           "" },
    { "scanner",            "Scanner",         "" },
    { "scanner_regs",       "Scanner regs",    "" },
    { "scanner_raw",        "Scanner reply",   "" },
    { "qr_config_lock",     "Setup-code lock", "" },
    { "stuck_buttons",      "Stuck buttons",   "" },
    { "free_heap_kb",       "Free memory",     " KB" },
    { "boot_ms",            "Boot time",       " ms" },
};

static const FieldName *field_name(const char *key) {
    for (const FieldName &f : FIELD_NAMES) {
        if (strcmp(f.key, key) == 0) return &f;
    }
    return nullptr;
}

enum Severity { SEV_OK, SEV_WARN, SEV_BAD };

// Decides how a field should read: red for a real failure, orange for something to act
// on that isn't broken (the setup-code lock still off, a restore, leftover temp files).
static Severity field_severity(const char *key, const char *val) {
    if (strstr(val, "FAILED") || strstr(val, "NOT_RESPONDING") || strstr(val, "MISMATCH") ||
        strstr(val, "query_failed") || strstr(val, "no_readback")) return SEV_BAD;
    if (strcmp(key, "data") == 0 || strcmp(key, "clock") == 0 || strcmp(key, "slide_free") == 0)
        return strcmp(val, "ok") == 0 ? SEV_OK : SEV_BAD;
    if (strcmp(key, "stuck_buttons") == 0 || strcmp(key, "slide_free_missing") == 0 ||
        strcmp(key, "scanner_raw") == 0) return SEV_BAD;
    if (strcmp(key, "db") == 0) return strcmp(val, "ok") == 0 ? SEV_OK : SEV_WARN;  // a restore
    if (strcmp(key, "qr_config_lock") == 0) return strcmp(val, "on") == 0 ? SEV_OK : SEV_WARN;
    if (strcmp(key, "temp_files_removed") == 0) return SEV_WARN;
    return SEV_OK;
}

static Severity reason_severity(const char *reason) {
    if (strcmp(reason, "BROWNOUT") == 0 || strcmp(reason, "CRASH") == 0 ||
        strncmp(reason, "WATCHDOG", 8) == 0) return SEV_BAD;
    return SEV_OK;
}

static lv_color_t sev_color(Severity s) {
    return lv_color_hex(s == SEV_BAD ? C_RED : s == SEV_WARN ? C_ORANGE : C_TEXT);
}

// ── rendering ────────────────────────────────────────────────────────────────

static void add_line(const char *text, lv_color_t color, const lv_font_t *font) {
    lv_obj_t *lbl = lv_label_create(_content);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);  // long values (data problems) wrap
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_color(lbl, color, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
}

// Rebuilds the page for boot _cur.
static void show_boot() {
    lv_obj_clean(_content);
    lv_obj_scroll_to_y(_content, 0, LV_ANIM_OFF);

    if (_indexed == 0) {
        add_line("No boot log on the SD card yet.", lv_color_hex(C_DIM), &lv_font_montserrat_16);
        return;
    }

    static char line[LINE_MAX_LEN];
    char buf[96];
    snprintf(buf, sizeof(buf), "Boot %d of %d%s", _cur + 1, _total, _cur == 0 ? " (newest)" : "");
    add_line(buf, lv_color_hex(C_CYAN), &lv_font_montserrat_16);

    if (!read_boot(_cur, line, sizeof(line))) {
        add_line("Couldn't read this entry.", lv_color_hex(C_RED), &lv_font_montserrat_16);
        return;
    }

    // time,reason,"details" -- split on the first two commas, then strip the quotes.
    char *time_s = line;
    char *reason = strchr(time_s, ',');
    char *details = reason ? strchr(reason + 1, ',') : nullptr;
    if (!reason || !details) {
        add_line(line, lv_color_hex(C_TEXT), &lv_font_montserrat_14);  // unexpected shape: show raw
        return;
    }
    *reason++ = '\0';
    *details++ = '\0';
    if (*details == '"') details++;
    size_t dl = strlen(details);
    if (dl && details[dl - 1] == '"') details[dl - 1] = '\0';

    add_line(time_s, lv_color_hex(C_TEXT), &lv_font_montserrat_20);
    snprintf(buf, sizeof(buf), "Restart: %s", reason);
    add_line(buf, sev_color(reason_severity(reason)), &lv_font_montserrat_16);

    // key=value;key=value -- values may themselves contain '=' (data problems), so split
    // each pair on its FIRST '=' only.
    for (char *pair = strtok(details, ";"); pair; pair = strtok(nullptr, ";")) {
        char *eq = strchr(pair, '=');
        const char *key = pair, *val = "";
        if (eq) { *eq = '\0'; val = eq + 1; }
        const FieldName *fn = field_name(key);
        char text[160];
        snprintf(text, sizeof(text), "%s: %s%s", fn ? fn->label : key, val, fn ? fn->unit : "");
        add_line(text, sev_color(field_severity(key, val)), &lv_font_montserrat_16);
    }
}

// ── buttons ──────────────────────────────────────────────────────────────────

// Left: one boot older, wrapping to the newest.
static void cb_older() {
    if (_indexed == 0) return;
    _cur = (_cur + 1) % _indexed;
    show_boot();
}

// Right: one boot newer, wrapping to the oldest indexed.
static void cb_newer() {
    if (_indexed == 0) return;
    _cur = (_cur - 1 + _indexed) % _indexed;
    show_boot();
}

static void cb_scroll_up()   { lv_obj_scroll_by_bounded(_content, 0, SCROLL_STEP, LV_ANIM_OFF); }
static void cb_scroll_down() { lv_obj_scroll_by_bounded(_content, 0, -SCROLL_STEP, LV_ANIM_OFF); }

// Back returns to Advanced Tools.
static void cb_back() {
    screen_admin_tools_push();
}

// Loads the Boot Log viewer on the newest boot.
void screen_boot_log_push() {
    if (!_scr) {
        _scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(_scr, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, LV_PART_MAIN);

        _content = lv_obj_create(_scr);
        lv_obj_set_size(_content, SCREEN_W, SCREEN_H - HDR_H - FOOTER_H);
        lv_obj_align(_content, LV_ALIGN_BOTTOM_MID, 0, -FOOTER_H);
        lv_obj_set_style_bg_opa(_content, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(_content, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(_content, 12, LV_PART_MAIN);  // 12px side inset, same as every screen
        lv_obj_set_style_pad_row(_content, 6, LV_PART_MAIN);
        lv_obj_set_layout(_content, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(_content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scrollbar_mode(_content, LV_SCROLLBAR_MODE_AUTO);

        lv_obj_t *legend = ui_legend(_scr);
        lv_obj_set_width(legend, SCREEN_W - 24);
        lv_obj_align(legend, LV_ALIGN_BOTTOM_MID, 0, -6);
        char older[24], newer[24], scroll[24];
        snprintf(older, sizeof(older), "%s Older", LV_SYMBOL_LEFT);
        snprintf(newer, sizeof(newer), "Newer %s", LV_SYMBOL_RIGHT);
        snprintf(scroll, sizeof(scroll), "Scroll %s%s", LV_SYMBOL_UP, LV_SYMBOL_DOWN);
        ui_legend_row(legend, older, lv_color_hex(C_YELLOW), newer, lv_color_hex(C_YELLOW));
        ui_legend_row(legend, scroll, lv_color_hex(C_YELLOW), "Back", lv_color_hex(C_RED));
    }

    header_set_visible(true);
    header_set_title("BOOT LOG");

    index_file();
    _cur = 0;
    show_boot();

    ButtonHandlers h;
    h.left  = cb_older;
    h.right = cb_newer;
    h.up    = cb_scroll_up;
    h.down  = cb_scroll_down;
    h.back  = cb_back;
    buttons_set_handlers(h);

    lv_scr_load(_scr);
}
