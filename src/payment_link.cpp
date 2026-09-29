#include "payment_link.h"
#include "theme.h"
#include <Arduino.h>
#include <mbedtls/base64.h>
#include <ctype.h>
#include <string.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the shared payment QR builder declared in payment_link.h.
*/

// Percent-encodes everything except unreserved URL characters. Moved here from
// screen_pos.cpp/screen_check_balance.cpp, which each had their own copy.
static void url_encode(const char *in, char *out, size_t out_len) {
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 4 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            snprintf(out + o, out_len - o, "%%%02X", c);
            o += 3;
        }
    }
    out[o] = '\0';
}

// Copies `in` into a JSON string body, dropping the two characters that would need
// escaping. The on-device wheels can't produce either, so this only guards web-entered values.
static void json_safe(const char *in, char *out, size_t out_len) {
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 1 < out_len; i++) {
        if (in[i] == '"' || in[i] == '\\') continue;
        out[o++] = in[i];
    }
    out[o] = '\0';
}

// Builds the QR payload string for a method. Returns false if unknown or it didn't fit.
static bool build_payload(const char *method, const char *handle, const char *owner,
                          int cents, int txn_id, char *out, size_t out_len) {
    int n = -1;
    if (strcmp(method, "venmo") == 0) {
        // The note carries the transaction number so the owner can match a Venmo payment
        // to a checkout at a glance. URL-encoded because '#' would otherwise end the URL.
        char note_raw[32], note_enc[96];
        snprintf(note_raw, sizeof(note_raw), "#%d Snacks!!", txn_id);
        url_encode(note_raw, note_enc, sizeof(note_enc));
        n = snprintf(out, out_len, "https://venmo.com/%s?txn=pay&amount=%d.%02d&note=%s",
                     handle, cents / 100, cents % 100, note_enc);
    } else if (strcmp(method, "cashapp") == 0) {
        n = snprintf(out, out_len, "https://cash.app/$%s/%d.%02d", handle, cents / 100, cents % 100);
    } else if (strcmp(method, "zelle") == 0 && strncmp(handle, "https://", 8) == 0) {
        // A scanned bank link -- reproduce it exactly, no re-encoding.
        n = snprintf(out, out_len, "%s", handle);
    } else if (strcmp(method, "zelle") == 0) {
        char name[48], token[48], json[128];
        json_safe(owner, name, sizeof(name));
        json_safe(handle, token, sizeof(token));
        int jl = snprintf(json, sizeof(json), "{\"name\":\"%s\",\"token\":\"%s\"}", name, token);
        if (jl < 0 || jl >= (int)sizeof(json)) return false;

        unsigned char b64[192];
        size_t b64_len = 0;
        if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len,
                                  (const unsigned char *)json, jl) != 0) return false;
        // Raw base64, NOT percent-encoded -- matches the bank-generated codes and the
        // published working examples; a scanner that doesn't URL-decode would choke on %3D.
        n = snprintf(out, out_len, "https://enroll.zellepay.com/qr-codes?data=%s", (const char *)b64);
    }
    return n > 0 && n < (int)out_len;
}

// Copies the run of [A-Za-z0-9_-] starting at `p` into out. Returns its length.
static size_t copy_ident(const char *p, char *out, size_t out_len) {
    size_t n = 0;
    while (p[n] && (isalnum((unsigned char)p[n]) || p[n] == '_' || p[n] == '-') && n + 1 < out_len) {
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';
    return n;
}

// Case-insensitive strstr -- scanned links can come back in any case.
static const char *find_ci(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    for (const char *h = hay; *h; h++) {
        if (strncasecmp(h, needle, nl) == 0) return h;
    }
    return nullptr;
}

// Copies the JSON string value for "key" out of a flat JSON object. Returns false if absent.
static bool json_get(const char *json, const char *key, char *out, size_t out_len) {
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), '"');
    if (!p) return false;
    p++;
    size_t n = 0;
    while (p[n] && p[n] != '"' && n + 1 < out_len) { out[n] = p[n]; n++; }
    out[n] = '\0';
    return true;
}

bool payment_zelle_decode(const char *url, char *token, size_t token_len,
                          char *name, size_t name_len) {
    token[0] = '\0';
    name[0]  = '\0';
    if (!find_ci(url, "zellepay.com")) return false;
    const char *d = strstr(url, "data=");
    if (!d) return false;
    d += 5;

    // Normalize to standard, padded base64: tolerate the URL-safe alphabet, a
    // percent-encoded '=', and stray whitespace.
    char b64[200];
    size_t n = 0;
    for (const char *p = d; *p && *p != '&' && n + 4 < sizeof(b64); p++) {
        if (p[0] == '%' && p[1] == '3' && (p[2] == 'D' || p[2] == 'd')) { b64[n++] = '='; p += 2; continue; }
        if (isspace((unsigned char)*p)) continue;
        b64[n++] = *p == '-' ? '+' : *p == '_' ? '/' : *p;
    }
    while (n % 4) b64[n++] = '=';
    b64[n] = '\0';

    unsigned char json[160];
    size_t json_len = 0;
    if (mbedtls_base64_decode(json, sizeof(json) - 1, &json_len,
                              (const unsigned char *)b64, n) != 0) return false;
    json[json_len] = '\0';

    json_get((const char *)json, "name", name, name_len);
    return json_get((const char *)json, "token", token, token_len) && token[0];
}

bool payment_parse_scan(const char *method, const char *scan,
                        char *handle_out, size_t handle_len, char *name_out, size_t name_len) {
    handle_out[0] = '\0';
    name_out[0]   = '\0';
    const char *p;

    if (strcmp(method, "venmo") == 0) {
        // venmo.com/u/NAME (personal code), ...recipients=NAME (app deep link), or venmo.com/NAME.
        if      ((p = find_ci(scan, "venmo.com/u/")))  p += 12;
        else if ((p = find_ci(scan, "recipients=")))   p += 11;
        else if ((p = find_ci(scan, "venmo.com/")))    p += 10;
        else return false;
        if (*p == '@') p++;
        if (!copy_ident(p, handle_out, handle_len)) return false;
        // "venmo.com/code?user_id=..." carries only a numeric account id, not a username.
        return strcasecmp(handle_out, "code") != 0 && strcasecmp(handle_out, "u") != 0;
    }

    if (strcmp(method, "cashapp") == 0) {
        // cash.app/$CASHTAG (or the older cash.me/$CASHTAG), possibly with a path before the $.
        const char *host = find_ci(scan, "cash.app/");
        if (!host) host = find_ci(scan, "cash.me/");
        if (!host || !(p = strchr(host, '$'))) return false;
        return copy_ident(p + 1, handle_out, handle_len) > 0;
    }

    if (strcmp(method, "zelle") == 0) {
        char token[64];
        if (!payment_zelle_decode(scan, token, sizeof(token), name_out, name_len)) return false;
        if (strlen(scan) + 1 > handle_len) return false;
        strcpy(handle_out, scan);  // store the bank's link verbatim
        return true;
    }
    return false;
}

bool payment_has_qr(const char *method) {
    return strcmp(method, "venmo") == 0 || strcmp(method, "cashapp") == 0 ||
           strcmp(method, "zelle") == 0;
}

const char *payment_method_label(const char *method) {
    if (strcmp(method, "venmo") == 0)   return "Venmo";
    if (strcmp(method, "cashapp") == 0) return "Cash App";
    if (strcmp(method, "zelle") == 0)   return "Zelle";
    return method;
}

void payment_display_handle(const char *method, const char *handle, char *out, size_t out_len) {
    char name_unused[48];
    if (strcmp(method, "zelle") == 0 &&
        payment_zelle_decode(handle, out, out_len, name_unused, sizeof(name_unused))) return;

    if (strcmp(method, "venmo") == 0)        snprintf(out, out_len, "@%s", handle);
    else if (strcmp(method, "cashapp") == 0) snprintf(out, out_len, "$%s", handle);
    else                                     snprintf(out, out_len, "%s", handle);
}

bool payment_add_qr(lv_obj_t *parent, const char *method, const char *handle,
                    const char *owner, int total_cents, int txn_id) {
    char url[320];
    if (!build_payload(method, handle, owner, total_cents, txn_id, url, sizeof(url))) return false;

    lv_obj_t *qr_row = lv_obj_create(parent);
    lv_obj_set_size(qr_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(qr_row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(qr_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(qr_row, 0, LV_PART_MAIN);
    lv_obj_set_layout(qr_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(qr_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(qr_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(qr_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *qr = lv_qrcode_create(qr_row, 200, lv_color_hex(C_BG), lv_color_hex(C_TEXT));
    lv_qrcode_update(qr, url, strlen(url));

    // Venmo: its in-app scanner only reads other users' in-app codes, not a plain link
    // (confirmed on real hardware 2026-08-24). Zelle: the phone camera just opens a
    // "find your bank" page; the bank app's Zelle scanner is what reads this payload.
    const char *hint =
        strcmp(method, "venmo") == 0 ? "Use your phone's Camera app\n(not the Venmo app's scanner)" :
        strcmp(method, "zelle") == 0 ? "Scan with the Zelle scanner\nin your bank's app" :
                                       "Use your phone's Camera app";
    lv_obj_t *scan_hint = lv_label_create(parent);
    lv_label_set_text(scan_hint, hint);
    lv_label_set_long_mode(scan_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(scan_hint, lv_color_hex(C_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(scan_hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_width(scan_hint, LV_PCT(100));
    lv_obj_set_style_text_align(scan_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    return true;
}
