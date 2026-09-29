#pragma once
#include <lvgl.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Shared payment QR builder for the Payment screen (screen_pos.cpp) and Check
            Balance (screen_check_balance.cpp). Replaces the Venmo-only link code both
            screens used to duplicate. Added 2026-09-29 alongside Cash App + Zelle QR support.
  Notes---- Per method:
            venmo   -- venmo.com/<handle>?txn=pay&amount=..&note=#<txn> Snacks!!
            cashapp -- cash.app/$<cashtag>/<amount>. Amount prefills; Cash App links
                       can't carry a note, and the owner reconciles by amount + time.
            zelle   -- enroll.zellepay.com/qr-codes?data=<base64 {"name","token"}>, the
                       same payload bank apps put in their own Zelle QR codes. It's
                       reverse-engineered, not an official API. It carries no amount, and
                       it's scanned with the bank app's Zelle scanner, not the camera.
                       token = the owner's Zelle phone (10 digits) or email. If the
                       handle is a whole scanned bank link ("https://..."), it's shown
                       verbatim instead -- exactly what the bank generated.
*/

// Longest payment_methods.handle this code handles -- a scanned Zelle link is stored
// whole (~130 chars), so every buffer that reads the handle must be at least this big.
#define PAYMENT_HANDLE_MAX 256

// True if the method has a QR format this module knows how to build.
bool payment_has_qr(const char *method);

// Parses a scan of the owner's own "my QR code" from that method's app into what
// payment_methods.handle should store (venmo: username, cashapp: cashtag without the $,
// zelle: the bank's whole enroll.zellepay.com link, verbatim). name_out gets the
// recipient name when the code carries one (Zelle only), else "". Returns false if the
// scan doesn't look like that app's code.
bool payment_parse_scan(const char *method, const char *scan,
                        char *handle_out, size_t handle_len, char *name_out, size_t name_len);

// Zelle: pulls the token (phone/email) and name back out of a stored bank link, for
// showing on screen. Returns false if it isn't a decodable enroll.zellepay.com link.
bool payment_zelle_decode(const char *url, char *token, size_t token_len,
                          char *name, size_t name_len);

// Adds a centered QR code plus a one-line scan hint to a flex-column `parent`. Returns
// false (and adds nothing) if the method has no QR format or the payload wouldn't fit.
// txn_id is only used by Venmo's note.
bool payment_add_qr(lv_obj_t *parent, const char *method, const char *handle,
                    const char *owner, int total_cents, int txn_id);

// Adds a row of tabs, one per enabled payment method, in the same order "Other Payment"
// cycles through them, with active_index highlighted. Adds nothing when fewer than two
// methods are enabled.
void payment_add_tabs(lv_obj_t *parent, int active_index);

// Human-readable method name for on-screen text ("Venmo", "Cash App", "Zelle").
const char *payment_method_label(const char *method);

// Formats a stored handle for on-screen display: "@name" (Venmo), "$tag" (Cash App), or
// the phone/email inside a scanned Zelle link.
void payment_display_handle(const char *method, const char *handle, char *out, size_t out_len);
