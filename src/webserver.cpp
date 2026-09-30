#include "webserver.h"
#include "db.h"
#include "users.h"
#include "items.h"
#include "payment_link.h"
#include "report_export.h"
#include "session_timer.h"
#include "idle_timer.h"
#include "backlight.h"
#include "screen_screensaver.h"
#include "rtc.h"
#include <sys/time.h>
#include <Arduino.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- The AP-mode WiFi + admin web portal: every page
            (Home/Items/Balance/Report/Users/Admin) as a hand-built HTML string over
            ESPAsyncWebServer, reading/writing the same SQLite DB the on-device UI
            uses.
  Notes---- No client-side JS/framework -- plain forms POST back to their own
            handler, which redirects back to the page. No web login (removed 2026-09-29,
            owner's call): reaching the portal already takes an admin badge scan on the
            cart to open the Web Portal screen, plus the WiFi password. A calculated risk
            for a local-only AP that's off whenever that screen isn't showing.
*/
#include <WiFi.h>
#include <SD.h>
#include <ESPAsyncWebServer.h>
#include <sqlite3.h>
#include <time.h>
#include <string.h>

#define AP_SSID             "SnackCart"
// 2026-09-14 — the AP password moved from this fixed #define to the config table, editable
// on-device from Settings -> Security (screen_wifi_password.cpp), same as every other
// config-backed setting in this codebase. This stays as the fallback default for a fresh
// DB / before anything's ever been saved -- WPA2-PSK requires 8-63 ASCII chars, so anything
// shorter saved to config is treated as unset (see get_ap_password() below).
#define AP_PASSWORD_DEFAULT "snackcart123"
#define AP_PASSWORD_CONFIG_KEY "ap_password"

static AsyncWebServer _server(80);
static volatile bool  _ap_running;

// Config-backed password with a fallback default; values shorter than min_len count as unset.
static const char *get_config_password(const char *key, const char *fallback, size_t min_len,
                                        char *buf, size_t buf_len) {
    if (db_handle() && db_config_get(key, buf, buf_len) && strlen(buf) >= min_len) {
        return buf;
    }
    return fallback;
}

static const char *get_ap_password() {
    static char buf[32];
    return get_config_password(AP_PASSWORD_CONFIG_KEY, AP_PASSWORD_DEFAULT, 8, buf, sizeof(buf));
}

// ── shared helpers ──────────────────────────────────────────────────────────

// Escapes &/</>/" for safe embedding in HTML.
static String html_escape(const String &s) {
    String out = s;
    out.replace("&", "&amp;");
    out.replace("<", "&lt;");
    out.replace(">", "&gt;");
    out.replace("\"", "&quot;");
    return out;
}

// Formats an integer cents value as "$X.XX".
static String format_cents(int cents) {
    char buf[16];
    snprintf(buf, sizeof(buf), "$%.2f", cents / 100.0);
    return String(buf);
}

// epoch==0 means "never set" (no RTC/NTP time yet) — shown as em-dash rather than
// the misleading 1970-01-01 that strftime would otherwise print.
static String format_epoch(long epoch) {
    if (epoch <= 0) return "&mdash;";
    time_t t = (time_t)epoch;
    struct tm tmval;
    gmtime_r(&t, &tmval);
    char buf[20];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmval);
    return String(buf);
}

// Formats millis()-since-boot as "XhYm".
static String format_uptime() {
    unsigned long s = millis() / 1000;
    unsigned long h = s / 3600;
    unsigned long m = (s % 3600) / 60;
    char buf[24];
    snprintf(buf, sizeof(buf), "%luh %lum", h, m);
    return String(buf);
}

// Shared theme, reused by the physical device's own UI colors (#121212/#1E1E1E/#00BCD4/#EFEFEF)
// — kept in sync with src/theme.h's 2026-08-24 color correction (neutralized the
// blue-tinted surface color, swapped the pastel green/pink-red for sharper Material 500 tones).
static const char *PAGE_CSS =
    "body{font-family:sans-serif;background:#121212;color:#EFEFEF;margin:0;padding:0;}"
    ".navbar{display:flex;background:#1E1E1E;position:sticky;top:0;z-index:1;}"
    ".navtab{flex:1;min-height:48px;display:flex;align-items:center;justify-content:center;"
    "color:#EFEFEF;text-decoration:none;font-size:13px;white-space:nowrap;}"
    ".navtab.active{color:#00BCD4;border-bottom:2px solid #00BCD4;}"
    ".content{padding:12px;}"
    "h2{margin-top:0;}"
    "table{width:100%;border-collapse:collapse;margin-bottom:24px;}"
    "th,td{padding:8px;border-bottom:1px solid #333;text-align:left;}"
    "input{padding:4px;background:#1E1E1E;color:#EFEFEF;border:1px solid #444;width:70px;}"
    "input[name=name],input[name=first],input[name=last]{width:120px;}"
    "button{padding:6px 12px;background:#00BCD4;color:#000;border:none;border-radius:4px;cursor:pointer;}"
    "button.del{background:#F44336;color:#fff;}"
    "form{display:inline;}"
    "form.card{display:block;}"
    ".card{background:#1E1E1E;border-radius:8px;padding:12px 16px;margin-bottom:12px;}"
    ".stat-row{display:flex;flex-wrap:wrap;gap:12px;margin-bottom:24px;}"
    ".stat{background:#1E1E1E;border-radius:8px;padding:16px;flex:1;min-width:130px;}"
    ".stat .value{font-size:28px;color:#00BCD4;}"
    ".stat .label{font-size:13px;color:#AAA;}"
    ".muted{color:#AAA;font-size:13px;}"
    ".balance{color:#FFB74D;}"
    ".ok{color:#4CAF50;}"
    // 2026-09-29 -- compact one-line rows (Items) and per-transaction rows (Balance),
    // sized to fit a ~360px-wide phone without sideways scrolling.
    "a.btn{display:inline-block;padding:6px 10px;background:#333;color:#EFEFEF;border-radius:4px;"
    "text-decoration:none;font-size:14px;}"
    "input.wide{width:220px;}"
    ".row{display:flex;align-items:center;gap:6px;padding:8px 0;border-bottom:1px solid #333;}"
    ".row.dim .nm,.row.dim .pr{opacity:0.4;}"
    ".nm{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;}"
    ".pr{width:46px;text-align:right;color:#AAA;font-size:14px;}"
    ".stk{display:flex;align-items:center;gap:3px;}"
    ".stk button{min-width:28px;padding:6px 3px;font-size:12px;background:#333;color:#EFEFEF;}"
    ".stk b{min-width:28px;text-align:center;}"
    ".txn{display:flex;align-items:center;gap:8px;padding:8px 0;border-top:1px solid #333;}"
    ".txn .info{flex:1;min-width:0;}"
    ".act{white-space:nowrap;}"
    ".c{width:58px;text-align:right;font-size:14px;}"
    ".dl{display:grid;grid-template-columns:auto 1fr 1fr 1fr;gap:8px;align-items:center;}"
    ".dl a.btn{text-align:center;padding:8px 4px;}"
    ".row.dim .c{opacity:0.4;}"
    ".pill{min-width:0;padding:5px 9px;border-radius:14px;background:transparent;color:#777;border:1px solid #444;}"
    ".pill.on{color:#00BCD4;border:2px solid #00BCD4;}"
    ".grid{display:grid;grid-template-columns:1fr 1fr;gap:8px 10px;}"
    ".grid label{font-size:13px;color:#AAA;}"
    "select{padding:4px;background:#1E1E1E;color:#EFEFEF;border:1px solid #444;display:block;width:100%;margin-top:2px;}"
    ".grid input{display:block;width:100%;box-sizing:border-box;margin-top:2px;font-size:15px;}";

struct NavTab { const char *label; const char *href; };
static const NavTab NAV_TABS[] = {
    {"Home",    "/"},
    {"Balance", "/balance"},
    {"Items",   "/item"},
    {"Report",  "/report"},
    {"Users",   "/users"},
    {"Admin",   "/admin"},
};

// Renders the top navbar, highlighting whichever tab is "active".
static String render_nav(const char *active) {
    String html = "<nav class='navbar'>";
    for (const NavTab &t : NAV_TABS) {
        bool isActive = strcmp(active, t.label) == 0;
        html += "<a class='navtab";
        if (isActive) html += " active";
        html += "' href='" + String(t.href) + "'>" + t.label + "</a>";
    }
    html += "</nav>";
    return html;
}

// Wraps a page body in the shared HTML shell (doctype/CSS/navbar).
static String render_page(const String &title, const char *active, const String &body) {
    session_timer_reset();  // every page view keeps the on-device auto-logout from firing
    String html;
    html += "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>" + title + "</title><style>" + String(PAGE_CSS) + "</style></head><body>";
    html += render_nav(active);
    html += "<div class='content'>" + body + "</div>";
    html += "</body></html>";
    return html;
}

// ── Home ─────────────────────────────────────────────────────────────────────

// Renders the Home dashboard (outstanding balance/checkout/user/item stats).
static String render_home_page() {
    int outstanding_cents = 0, outstanding_count = 0, user_count = 0, item_count = 0;

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_handle(),
        "SELECT COALESCE(SUM(total_price_cents),0), COUNT(*) FROM checkouts WHERE cleared_at IS NULL;",
        -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            outstanding_cents = sqlite3_column_int(stmt, 0);
            outstanding_count = sqlite3_column_int(stmt, 1);
        }
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(db_handle(), "SELECT COUNT(*) FROM users;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) user_count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(db_handle(), "SELECT COUNT(*) FROM items;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) item_count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    String html = "<h2>Snack Cart</h2><div class='stat-row'>";
    html += "<div class='stat'><div class='value balance'>" + format_cents(outstanding_cents) + "</div>"
            "<div class='label'>Outstanding balance</div></div>";
    html += "<div class='stat'><div class='value'>" + String(outstanding_count) + "</div>"
            "<div class='label'>Uncleared checkouts</div></div>";
    html += "<div class='stat'><div class='value'>" + String(user_count) + "</div>"
            "<div class='label'>Registered users</div></div>";
    html += "<div class='stat'><div class='value'>" + String(item_count) + "</div>"
            "<div class='label'>Catalog items</div></div>";
    html += "</div>";
    html += "<p class='muted'>Balance page has the full clearable list grouped by person. "
            "Report has lifetime per-item and per-user totals.</p>";
    return render_page("Snack Cart", "Home", html);
}

// GET / -- the Home dashboard.
static void handle_home(AsyncWebServerRequest *request) {
    request->send(200, "text/html", render_home_page());
}

// ── Items ───────────────────────────────────────────────────────────────────
// 2026-09-29 rework (owner's layout): one compact line per item -- name, price, stock
// with -10/-1/+1/+10, Edit -- instead of a wide table of inputs that scrolled sideways on
// a phone. Everything shown is plain text; the +/- buttons are the only in-row action.
// Name/price/hidden/delete live on the separate Edit page (/item-edit), which has its own
// path because "/item" also matches every "/item/..." URL for the same method.

// Renders the Items page. `err` is a short message from the last action, or nullptr.
static String render_items_page(const char *err) {
    String html = "<h2>Items</h2>";
    if (err) html += "<p class='balance'>" + String(err) + "</p>";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_handle(), "SELECT id, name, price_cents, stocked, hidden FROM items ORDER BY name COLLATE NOCASE;", -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            String id      = String(sqlite3_column_int(stmt, 0));
            String name    = (const char *)sqlite3_column_text(stmt, 1);
            int    price   = sqlite3_column_int(stmt, 2);
            int    stocked = sqlite3_column_int(stmt, 3);
            bool   hidden  = sqlite3_column_int(stmt, 4) != 0;

            // id='i<N>' is the scroll target every +/- and Save redirects back to.
            html += "<div class='row" + String(hidden ? " dim" : "") + "' id='i" + id + "'>";
            html += "<span class='nm'>" + html_escape(name) + "</span>";
            html += "<span class='pr'>" + format_cents(price) + "</span>";
            html += "<form method='POST' action='/item/stock' class='stk'>"
                    "<input type='hidden' name='id' value='" + id + "'>"
                    "<button name='d' value='-10'>-10</button><button name='d' value='-1'>-1</button>"
                    "<b>" + String(stocked) + "</b>"
                    "<button name='d' value='1'>+1</button><button name='d' value='10'>+10</button></form>";
            html += "<a class='btn' href='/item-edit?id=" + id + "'>Edit</a></div>";
        }
        sqlite3_finalize(stmt);
    }

    html += "<div class='card' style='margin-top:20px'><h3>Add Item</h3>";
    html += "<form method='POST' action='/item/add'>";
    html += "<p>Name<br><input class='wide' name='name' required></p>";
    html += "<p>Price ($)<br><input name='price' type='number' step='0.01' min='0' required></p>";
    html += "<p>Stocked<br><input name='stocked' type='number' value='0'></p>";
    html += "<button type='submit'>Add</button></form></div>";
    html += "<p class='muted'>Dimmed items are hidden: off Browse, and their barcode scans as "
            "unknown. Barcodes are linked on the cart (Inventory &gt; Add/Attach Item).</p>";
    return render_page("Items", "Items", html);
}

// GET /item -- the Items page.
static void handle_items(AsyncWebServerRequest *request) {
    request->send(200, "text/html", render_items_page(nullptr));
}

// Reads one item for the Edit page. Returns false if it doesn't exist.
static bool load_item(int id, String &name, int &price, bool &hidden) {
    bool found = false;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_handle(), "SELECT name, price_cents, hidden FROM items WHERE id=?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            name   = (const char *)sqlite3_column_text(stmt, 0);
            price  = sqlite3_column_int(stmt, 1);
            hidden = sqlite3_column_int(stmt, 2) != 0;
            found  = true;
        }
        sqlite3_finalize(stmt);
    }
    return found;
}

// GET /item-edit?id=N -- the less-common edits for one item: name, price, hidden, delete.
// ?err=history comes back from a refused delete.
static void handle_item_edit_page(AsyncWebServerRequest *request) {
    int id = request->hasParam("id") ? request->getParam("id")->value().toInt() : 0;
    String name;
    int price = 0;
    bool hidden = false;
    if (!load_item(id, name, price, hidden)) { request->redirect("/item"); return; }

    String sid = String(id);
    String html = "<h2>Edit Item</h2>";
    if (request->hasParam("err") && request->getParam("err")->value() == "history") {
        html += "<p class='balance'>This item has sales history, so it can't be deleted. "
                "Tick Hidden and Save instead.</p>";
    }
    html += "<div class='card'><form method='POST' action='/item/edit'>";
    html += "<input type='hidden' name='id' value='" + sid + "'>";
    html += "<p>Name<br><input class='wide' name='name' value='" + html_escape(name) + "' required></p>";
    html += "<p>Price ($)<br><input name='price' type='number' step='0.01' min='0' value='" + String(price / 100.0, 2) + "' required></p>";
    html += "<p><label><input type='checkbox' name='hidden' value='1'" + String(hidden ? " checked" : "") +
            "> Hidden</label><br><span class='muted'>Off Browse, and its barcode scans as unknown. "
            "Sales history is kept.</span></p>";
    html += "<button type='submit'>Save</button> <a class='btn' href='/item#i" + sid + "'>Cancel</a></form></div>";

    html += "<div class='card'><form method='POST' action='/item/delete' onsubmit=\"return confirm('Delete this item?');\">";
    html += "<input type='hidden' name='id' value='" + sid + "'>";
    html += "<button class='del' type='submit'>Delete Item</button></form>";
    html += "<p class='muted'>Only for items that have never sold; otherwise use Hidden.</p></div>";
    request->send(200, "text/html", render_page("Edit Item", "Items", html));
}

// POST /item/add -- creates a new item from the Add Item form.
static void handle_item_add(AsyncWebServerRequest *request) {
    session_timer_reset();  // web activity keeps the cart from auto-logging out
    if (request->hasParam("name", true) && request->hasParam("price", true)) {
        String name    = request->getParam("name", true)->value();
        float  price   = request->getParam("price", true)->value().toFloat();
        int    stocked = request->hasParam("stocked", true) ? request->getParam("stocked", true)->value().toInt() : 0;
        name.trim();
        if (name.length()) items_create(name.c_str(), (int)(price * 100 + 0.5), stocked);
    }
    request->redirect("/item");
}

// POST /item/stock -- adds d (-10/-1/+1/+10) to one item's stock count.
static void handle_item_stock(AsyncWebServerRequest *request) {
    session_timer_reset();  // web activity keeps the cart from auto-logging out
    int id = request->hasParam("id", true) ? request->getParam("id", true)->value().toInt() : 0;
    int d  = request->hasParam("d", true)  ? request->getParam("d", true)->value().toInt()  : 0;
    if (id > 0 && d != 0) {
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db_handle(), "UPDATE items SET stocked = stocked + ? WHERE id=?;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, d);
            sqlite3_bind_int(stmt, 2, id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    request->redirect("/item#i" + String(id));
}

// POST /item/edit -- saves name/price/hidden from the Edit page. Stock is deliberately not
// touched here; it only changes through the +/- buttons.
static void handle_item_edit(AsyncWebServerRequest *request) {
    session_timer_reset();  // web activity keeps the cart from auto-logging out
    int id = request->hasParam("id", true) ? request->getParam("id", true)->value().toInt() : 0;
    if (id > 0 && request->hasParam("name", true) && request->hasParam("price", true)) {
        String name  = request->getParam("name", true)->value();
        float  price = request->getParam("price", true)->value().toFloat();
        name.trim();
        // Unchecked checkboxes aren't sent at all -- presence is what "checked" means.
        bool hidden = request->hasParam("hidden", true);

        sqlite3_stmt *stmt;
        if (name.length() &&
            sqlite3_prepare_v2(db_handle(), "UPDATE items SET name=?, price_cents=? WHERE id=?;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, (int)(price * 100 + 0.5));
            sqlite3_bind_int(stmt, 3, id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        items_set_hidden(id, hidden);
    }
    request->redirect("/item#i" + String(id));
}

// POST /item/delete -- deletes one item via items_delete() (same rules as the cart: its
// barcodes go with it, and an item with sales history is refused).
static void handle_item_delete(AsyncWebServerRequest *request) {
    session_timer_reset();  // web activity keeps the cart from auto-logging out
    int id = request->hasParam("id", true) ? request->getParam("id", true)->value().toInt() : 0;
    if (id > 0 && !items_delete(id)) {
        request->redirect("/item-edit?id=" + String(id) + "&err=history");
        return;
    }
    request->redirect("/item");
}

// ── Balance ("the clearable list") ─────────────────────────────────────────
// 2026-09-29: one Clear button per transaction with an in-place confirm, matching the
// cart's Balances screen -- replaces checkboxes + "Clear Selected". The confirm is a page
// reload with ?confirm=<id>, so it works without any JavaScript.

// Renders the Balance page: every outstanding checkout, grouped by person. confirm_id is
// the transaction currently showing "Clear? Yes / No", or 0.
static String render_balance_page(int confirm_id) {
    String html = "<h2>Balance</h2>";

    sqlite3_stmt *stmt;
    const char *sql =
        "SELECT c.id, c.user_id, u.first_name, u.last_name, c.total_price_cents, c.created_at "
        "FROM checkouts c JOIN users u ON u.id = c.user_id "
        "WHERE c.cleared_at IS NULL "
        "ORDER BY u.first_name COLLATE NOCASE, u.last_name COLLATE NOCASE, c.created_at;";

    int open_user_id = -1;
    int person_total = 0;
    String person_rows;
    String person_name;

    // Closes out one person's card -- their name + running total, then their rows.
    auto flush_person = [&]() {
        if (open_user_id == -1) return;
        html += "<div class='card'><h3>" + person_name + " <span class='balance'>" +
                format_cents(person_total) + "</span></h3>" + person_rows + "</div>";
    };

    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int    checkout_id = sqlite3_column_int(stmt, 0);
            int    user_id     = sqlite3_column_int(stmt, 1);
            int    total       = sqlite3_column_int(stmt, 4);
            long   created_at  = sqlite3_column_int64(stmt, 5);

            if (user_id != open_user_id) {
                flush_person();
                open_user_id = user_id;
                person_total = 0;
                person_rows  = "";
                person_name  = html_escape((const char *)sqlite3_column_text(stmt, 2)) + " " +
                               html_escape((const char *)sqlite3_column_text(stmt, 3));
            }
            person_total += total;

            String items;
            sqlite3_stmt *istmt;
            const char *isql =
                "SELECT i.name, ci.quantity FROM checkout_items ci "
                "JOIN items i ON i.id = ci.item_id WHERE ci.checkout_id=?;";
            if (sqlite3_prepare_v2(db_handle(), isql, -1, &istmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(istmt, 1, checkout_id);
                while (sqlite3_step(istmt) == SQLITE_ROW) {
                    if (items.length()) items += ", ";
                    items += html_escape((const char *)sqlite3_column_text(istmt, 0));
                    int qty = sqlite3_column_int(istmt, 1);
                    if (qty > 1) items += " x" + String(qty);
                }
                sqlite3_finalize(istmt);
            }

            String cid = String(checkout_id);
            person_rows += "<div class='txn' id='c" + cid + "'><div class='info'><b>#" + cid + "</b> " +
                           format_cents(total) + " <span class='muted'>" + format_epoch(created_at) +
                           "</span><br><span class='muted'>" + (items.length() ? items : String("&mdash;")) +
                           "</span></div>";
            if (checkout_id == confirm_id) {
                person_rows += "<div class='act'><span class='balance'>Paid?</span> "
                               "<form method='POST' action='/checkout/clear'><input type='hidden' name='id' value='" + cid +
                               "'><button>Yes</button></form> <a class='btn' href='/balance#c" + cid + "'>No</a></div>";
            } else {
                person_rows += "<div class='act'><a class='btn' href='/balance?confirm=" + cid + "#c" + cid + "'>Clear</a></div>";
            }
            person_rows += "</div>";
        }
        sqlite3_finalize(stmt);
    }
    flush_person();

    if (open_user_id == -1) html += "<p class='muted'>No outstanding balances.</p>";
    else html += "<p class='muted'>Clear marks a transaction as paid. It stays in Report's lifetime totals.</p>";
    return render_page("Balance", "Balance", html);
}

// GET /balance -- the Balance page; ?confirm=<id> shows that row's inline Yes/No.
static void handle_balance(AsyncWebServerRequest *request) {
    int confirm_id = request->hasParam("confirm") ? request->getParam("confirm")->value().toInt() : 0;
    request->send(200, "text/html", render_balance_page(confirm_id));
}

// POST /checkout/clear -- marks one checkout cleared (paid).
static void handle_checkout_clear(AsyncWebServerRequest *request) {
    session_timer_reset();  // web activity keeps the cart from auto-logging out
    // DS3231 RTC wired and confirmed 2026-08-28/2026-09-14 (see rtc.cpp) -- time(nullptr)
    // reads real wall-clock time, synced from the chip at boot (this device is AP-only,
    // no NTP path at all -- see rtc.h).
    long now = (long)time(nullptr);
    int id = request->hasParam("id", true) ? request->getParam("id", true)->value().toInt() : 0;
    sqlite3_stmt *stmt;
    if (id > 0 && sqlite3_prepare_v2(db_handle(), "UPDATE checkouts SET cleared_at=? WHERE id=? AND cleared_at IS NULL;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, now);
        sqlite3_bind_int(stmt, 2, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    request->redirect("/balance");
}

// ── Report (read-only lifetime totals) ──────────────────────────────────────
// 2026-09-29 rework: three totals up top (same stat tiles as Home), then compact one-line
// rows per person and per item instead of wide tables. Everything comes from
// checkout_items (price at time of sale), so a later price change never rewrites history.

// Sums sold value (cents) for checkouts matching `where` (a SQL condition on c).
static int report_sum(const char *where, long param) {
    int cents = 0;
    String sql = "SELECT COALESCE(SUM(ci.price_cents*ci.quantity),0) FROM checkout_items ci "
                 "JOIN checkouts c ON c.id = ci.checkout_id WHERE " + String(where) + ";";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_handle(), sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        if (strchr(where, '?')) sqlite3_bind_int64(stmt, 1, param);
        if (sqlite3_step(stmt) == SQLITE_ROW) cents = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return cents;
}

// Renders the Report page.
static String render_report_page() {
    // Start of this month, in the device's local-as-UTC convention (see format_epoch()).
    time_t now = time(nullptr);
    struct tm tmval;
    gmtime_r(&now, &tmval);
    tmval.tm_mday = 1;
    tmval.tm_hour = tmval.tm_min = tmval.tm_sec = 0;
    long month_start = (long)mktime(&tmval);

    String html = "<h2>Report</h2>";
    // Downloads (2026-09-29): same data as this page, as a 3-tab spreadsheet or plain text.
    html += "<div class='card'><h3>Download</h3><div class='dl'>";
    static const char *FMT[][2]   = { { "xlsx", "Spreadsheet:" }, { "txt", "Plain text:" } };
    static const char *RANGE[][2] = { { "current", "Current" }, { "last30", "30 days" }, { "lifetime", "Lifetime" } };
    for (auto &f : FMT) {
        html += "<span class='muted'>" + String(f[1]) + "</span>";
        for (auto &r : RANGE) {
            html += "<a class='btn' href='/report-download?fmt=" + String(f[0]) + "&range=" + String(r[0]) + "'>" + String(r[1]) + "</a>";
        }
    }
    html += "</div></div><div class='stat-row'>";
    html += "<div class='stat'><div class='value'>" + format_cents(report_sum("1", 0)) +
            "</div><div class='label'>All-time sales</div></div>";
    html += "<div class='stat'><div class='value'>" + format_cents(report_sum("c.created_at >= ?", month_start)) +
            "</div><div class='label'>This month</div></div>";
    html += "<div class='stat'><div class='value balance'>" + format_cents(report_sum("c.cleared_at IS NULL", 0)) +
            "</div><div class='label'>Outstanding</div></div></div>";

    // Per person: lifetime spent, and what they still owe.
    html += "<h3>People</h3><div class='row muted'><span class='nm'>Name</span>"
            "<span class='c'>Spent</span><span class='c'>Owes</span></div>";
    sqlite3_stmt *stmt;
    const char *user_sql =
        "SELECT u.first_name, u.last_name, "
        "  COALESCE(SUM(ci.price_cents*ci.quantity),0), "
        "  COALESCE(SUM(CASE WHEN c.cleared_at IS NULL THEN ci.price_cents*ci.quantity ELSE 0 END),0) "
        "FROM users u "
        "LEFT JOIN checkouts c ON c.user_id = u.id "
        "LEFT JOIN checkout_items ci ON ci.checkout_id = c.id "
        "GROUP BY u.id ORDER BY u.first_name COLLATE NOCASE, u.last_name COLLATE NOCASE;";
    if (sqlite3_prepare_v2(db_handle(), user_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int spent = sqlite3_column_int(stmt, 2);
            int owes  = sqlite3_column_int(stmt, 3);
            html += "<div class='row'><span class='nm'>" +
                    html_escape((const char *)sqlite3_column_text(stmt, 0)) + " " +
                    html_escape((const char *)sqlite3_column_text(stmt, 1)) + "</span>"
                    "<span class='c'>" + format_cents(spent) + "</span>"
                    "<span class='c" + String(owes > 0 ? " balance" : " muted") + "'>" + format_cents(owes) + "</span></div>";
        }
        sqlite3_finalize(stmt);
    }

    // Per item: units sold, revenue, and current stock (hidden items dimmed, as on Items).
    html += "<h3 style='margin-top:24px'>Items</h3><div class='row muted'><span class='nm'>Item</span>"
            "<span class='c'>Sold</span><span class='c'>Sales</span><span class='c'>Stock</span></div>";
    const char *item_sql =
        "SELECT i.name, i.stocked, i.hidden, COALESCE(SUM(ci.quantity),0), "
        "  COALESCE(SUM(ci.price_cents*ci.quantity),0) "
        "FROM items i LEFT JOIN checkout_items ci ON ci.item_id = i.id "
        "GROUP BY i.id ORDER BY i.name COLLATE NOCASE;";
    if (sqlite3_prepare_v2(db_handle(), item_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            bool hidden = sqlite3_column_int(stmt, 2) != 0;
            html += "<div class='row" + String(hidden ? " dim" : "") + "'><span class='nm'>" +
                    html_escape((const char *)sqlite3_column_text(stmt, 0)) + "</span>"
                    "<span class='c'>" + String(sqlite3_column_int(stmt, 3)) + "</span>"
                    "<span class='c'>" + format_cents(sqlite3_column_int(stmt, 4)) + "</span>"
                    "<span class='c'>" + String(sqlite3_column_int(stmt, 1)) + "</span></div>";
        }
        sqlite3_finalize(stmt);
    }
    return render_page("Report", "Report", html);
}

// GET /report -- the Report page.
static void handle_report(AsyncWebServerRequest *request) {
    request->send(200, "text/html", render_report_page());
}

// GET /report-download?fmt=xlsx|txt&range=current|last30|lifetime -- builds the file on the SD
// card (report_export.cpp) and sends it as a download. Own path: "/report" would catch
// "/report/...".
static void handle_report_download(AsyncWebServerRequest *request) {
    session_timer_reset();
    String f = request->hasParam("fmt")   ? request->getParam("fmt")->value()   : "";
    String r = request->hasParam("range") ? request->getParam("range")->value() : "";
    ReportFormat fmt   = f == "txt" ? REPORT_TXT : REPORT_XLSX;
    ReportRange  range = r == "current" ? REPORT_CURRENT : r == "last30" ? REPORT_LAST30 : REPORT_LIFETIME;

    if (!report_export(range, fmt)) {
        request->send(500, "text/plain", "Couldn't build the report. Check the SD card.");
        return;
    }
    char name[64];
    report_filename(range, fmt, name, sizeof(name));
    // The library always adds its own Content-Disposition, named after the path it's
    // given -- adding a second one broke downloads (ERR_RESPONSE_HEADERS_MULTIPLE_CONTENT_
    // DISPOSITION). So open the scratch file ourselves and hand over the download name
    // as the "path"; download=true makes it "attachment".
    File report = SD.open(REPORT_TMP_PATH);
    AsyncWebServerResponse *resp = request->beginResponse(report, "/" + String(name),
        fmt == REPORT_XLSX ? "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"
                           : "text/plain; charset=utf-8",
        true);
    request->send(resp);
}

// ── Users ────────────────────────────────────────────────────────────────────
// 2026-09-29 rework (owner's layout, matching Items): one line per person -- name, Admin
// and Active toggles, Edit -- with every field editable on /user-edit. That path is not
// "/users/..." because "/users" would catch it.
//
// Admin Login needs a badge that is admin AND active. The owner's rule (2026-09-29): the
// logged-in admin can't change or delete their own account -- same on the cart's Edit
// Users -- so there's always at least one active admin. usable_admins_except() is kept
// as a backstop in case that ever changes.

// Counts users who could pass Admin Login (admin AND active), not counting `except_id`.
static int usable_admins_except(int except_id) {
    int n = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_handle(), "SELECT COUNT(*) FROM users WHERE admin=1 AND active=1 AND id<>?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, except_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return n;
}

// Message for a ?err= code on either Users page, or nullptr.
static const char *user_err_text(AsyncWebServerRequest *request) {
    if (!request->hasParam("err")) return nullptr;
    String e = request->getParam("err")->value();
    if (e == "self")      return "That's you. You can't change your own account while you're using the portal.";
    if (e == "lastadmin") return "That would leave no active admin, and nobody could open the Admin Menu. Make someone else an active admin first.";
    if (e == "admin")     return "Admins can't be deleted. Turn off Admin first (someone else must still be an active admin).";
    if (e == "history")   return "This person has purchase history, so they can't be deleted. Turn off Active instead.";
    if (e == "badge")     return "That badge number already belongs to someone else.";
    if (e == "blank")     return "First name, last name and badge can't be blank.";
    return nullptr;
}

// Epoch -> "YYYY-MM-DD" for a date input ("" if never set). The device stores local
// wall-clock time as if it were UTC (see format_epoch()), so gmtime is the right read.
static String epoch_to_date(long epoch) {
    if (epoch <= 0) return "";
    time_t t = (time_t)epoch;
    struct tm tmval;
    gmtime_r(&t, &tmval);
    char buf[12];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tmval);
    return String(buf);
}

// "YYYY-MM-DD" -> epoch at 00:00 in the same local-as-UTC convention. -1 if unparseable.
static long date_to_epoch(const String &date) {
    int y, m, d;
    if (sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return -1;
    // Days since 1970-01-01 (Howard Hinnant's days_from_civil).
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (era * 146097 + doe - 719468) * 86400L;
}

// Renders the Users page: one compact line per person. self_id is the logged-in admin
// (the admin who opened the Web Portal on the cart), whose row is read-only.
static String render_users_page(const char *err, int self_id) {
    String html = "<h2>Users</h2>";
    if (err) html += "<p class='balance'>" + String(err) + "</p>";

    sqlite3_stmt *stmt;
    const char *sql = "SELECT id, first_name, last_name, active, admin FROM users ORDER BY first_name COLLATE NOCASE, last_name COLLATE NOCASE;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            String id     = String(sqlite3_column_int(stmt, 0));
            String name   = html_escape((const char *)sqlite3_column_text(stmt, 1)) + " " +
                            html_escape((const char *)sqlite3_column_text(stmt, 2));
            bool   active = sqlite3_column_int(stmt, 3) != 0;
            bool   admin  = sqlite3_column_int(stmt, 4) != 0;

            html += "<div class='row" + String(active ? "" : " dim") + "' id='u" + id + "'>";
            if (sqlite3_column_int(stmt, 0) == self_id) {
                // Own row: same pills, but plain labels -- nothing to click.
                html += "<span class='nm'>" + name + " <span class='muted'>(you)</span></span>";
                html += "<span class='stk'><span class='pill" + String(admin ? " on" : "") + "'>Admin</span>"
                        "<span class='pill" + String(active ? " on" : "") + "'>Active</span></span></div>";
                continue;
            }
            html += "<span class='nm'>" + name + "</span>";
            html += "<form method='POST' action='/user/toggle' class='stk'>"
                    "<input type='hidden' name='id' value='" + id + "'>"
                    "<button class='pill" + String(admin ? " on" : "") + "' name='f' value='admin'>Admin</button>"
                    "<button class='pill" + String(active ? " on" : "") + "' name='f' value='active'>Active</button></form>";
            html += "<a class='btn' href='/user-edit?id=" + id + "'>Edit</a></div>";
        }
        sqlite3_finalize(stmt);
    }
    html += "<p class='muted'>Tap Admin or Active to switch it. Your own account (the admin who "
            "opened the portal) can't be changed here. Inactive people are dimmed; the "
            "cart turns their badge away. New people are added on the cart (Users &gt; Add User, "
            "or Auto Enroll).</p>";
    return render_page("Users", "Users", html);
}

// GET /users -- the Users page.
static void handle_users(AsyncWebServerRequest *request) {
    request->send(200, "text/html", render_users_page(user_err_text(request), users_get_current_admin()));
}

// True (and redirects back to /users with an explanation) if `id` is the logged-in admin.
static bool refuse_self(AsyncWebServerRequest *request, int id) {
    if (id != users_get_current_admin()) return false;
    request->redirect("/users?err=self#u" + String(id));
    return true;
}

// GET /user-edit?id=N -- every field for one person, on one phone screen.
static void handle_user_edit_page(AsyncWebServerRequest *request) {
    int id = request->hasParam("id") ? request->getParam("id")->value().toInt() : 0;
    if (refuse_self(request, id)) return;
    const User *u = users_get_by_id(id);
    if (!u) { request->redirect("/users"); return; }

    long created = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_handle(), "SELECT created_at FROM users WHERE id=?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        if (sqlite3_step(stmt) == SQLITE_ROW) created = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }

    String sid = String(id);
    String html = "<h2>Edit User</h2>";
    const char *err = user_err_text(request);
    if (err) html += "<p class='balance'>" + String(err) + "</p>";

    html += "<form method='POST' action='/user/edit' class='card'>";
    html += "<input type='hidden' name='id' value='" + sid + "'>";
    html += "<div class='grid'>";
    html += "<label>First<input name='first' value='" + html_escape(u->first_name) + "' required></label>";
    html += "<label>Last<input name='last' value='" + html_escape(u->last_name) + "' required></label>";
    html += "<label>Badge<input name='badge' value='" + html_escape(u->badge_id) + "' required></label>";
    html += "<label>Registered<input type='date' name='registered' value='" + epoch_to_date(created) + "'></label>";
    html += "</div><p>";
    html += "<label><input type='checkbox' name='admin' value='1'" + String(u->admin ? " checked" : "") + "> Admin</label> &nbsp; ";
    html += "<label><input type='checkbox' name='active' value='1'" + String(u->active ? " checked" : "") + "> Active</label></p>";
    html += "<button type='submit'>Save</button> <a class='btn' href='/users#u" + sid + "'>Cancel</a></form>";

    html += "<form method='POST' action='/user/delete' onsubmit=\"return confirm('Delete this person?');\">";
    html += "<input type='hidden' name='id' value='" + sid + "'>";
    html += "<button class='del' type='submit'>Delete</button> <span class='muted'>Only for people who never bought anything.</span></form>";
    request->send(200, "text/html", render_page("Edit User", "Users", html));
}

// POST /user/toggle -- flips admin or active for one person, guarding the last usable admin.
static void handle_user_toggle(AsyncWebServerRequest *request) {
    session_timer_reset();  // web activity keeps the cart from auto-logging out
    int id = request->hasParam("id", true) ? request->getParam("id", true)->value().toInt() : 0;
    String f = request->hasParam("f", true) ? request->getParam("f", true)->value() : "";
    if (refuse_self(request, id)) return;
    const User *u = users_get_by_id(id);
    if (!u || (f != "admin" && f != "active")) { request->redirect("/users"); return; }

    bool admin  = u->admin;
    bool active = u->active;
    if (f == "admin") admin = !admin; else active = !active;

    if (!(admin && active) && u->admin && u->active && usable_admins_except(id) == 0) {
        request->redirect("/users?err=lastadmin#u" + String(id));
        return;
    }
    if (f == "admin") users_set_admin(id, admin); else users_set_active(id, active);
    request->redirect("/users#u" + String(id));
}

// POST /user/edit -- saves every field from the Edit page.
static void handle_user_edit(AsyncWebServerRequest *request) {
    session_timer_reset();  // web activity keeps the cart from auto-logging out
    int id = request->hasParam("id", true) ? request->getParam("id", true)->value().toInt() : 0;
    if (refuse_self(request, id)) return;
    if (!users_get_by_id(id)) { request->redirect("/users"); return; }
    String back = "/user-edit?id=" + String(id);

    String first = request->hasParam("first", true) ? request->getParam("first", true)->value() : "";
    String last  = request->hasParam("last", true)  ? request->getParam("last", true)->value()  : "";
    String badge = request->hasParam("badge", true) ? request->getParam("badge", true)->value() : "";
    String reg   = request->hasParam("registered", true) ? request->getParam("registered", true)->value() : "";
    // Unchecked checkboxes aren't sent at all -- presence is what "checked" means.
    bool admin  = request->hasParam("admin", true);
    bool active = request->hasParam("active", true);
    first.trim(); last.trim(); badge.trim();
    // Names stay all-caps everywhere, matching the on-device wheel (see users.cpp).
    first.toUpperCase();
    last.toUpperCase();

    if (!first.length() || !last.length() || !badge.length()) { request->redirect(back + "&err=blank"); return; }
    if (!(admin && active) && usable_admins_except(id) == 0) { request->redirect(back + "&err=lastadmin"); return; }
    const User *other = users_find_by_badge(badge.c_str());
    if (other && other->id != id) { request->redirect(back + "&err=badge"); return; }

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_handle(), "UPDATE users SET first_name=?, last_name=?, badge_barcode=?, admin=?, active=? WHERE id=?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, first.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, last.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, badge.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, admin ? 1 : 0);
        sqlite3_bind_int(stmt, 5, active ? 1 : 0);
        sqlite3_bind_int(stmt, 6, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Registered is only rewritten when the date was actually changed, so saving an
    // untouched form never resets an exact timestamp to midnight.
    long created = 0;
    if (sqlite3_prepare_v2(db_handle(), "SELECT created_at FROM users WHERE id=?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        if (sqlite3_step(stmt) == SQLITE_ROW) created = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    long new_created = date_to_epoch(reg);
    if (reg.length() && reg != epoch_to_date(created) && new_created >= 0 &&
        sqlite3_prepare_v2(db_handle(), "UPDATE users SET created_at=? WHERE id=?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, new_created);
        sqlite3_bind_int(stmt, 2, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    request->redirect("/users#u" + String(id));
}

// POST /user/delete -- deletes one person. Refuses admins (turn Admin off first, which
// itself guards the last usable admin) and anyone with checkouts.
static void handle_user_delete(AsyncWebServerRequest *request) {
    session_timer_reset();  // web activity keeps the cart from auto-logging out
    int id = request->hasParam("id", true) ? request->getParam("id", true)->value().toInt() : 0;
    if (refuse_self(request, id)) return;
    const User *u = users_get_by_id(id);
    if (!u) { request->redirect("/users"); return; }
    String back = "/user-edit?id=" + String(id);
    if (u->admin) { request->redirect(back + "&err=admin"); return; }

    // checkouts.user_id REFERENCES users(id) with no ON DELETE CASCADE and
    // foreign_keys=ON -- deleting someone with checkouts fails rather than orphaning
    // their history. Reported back to the page instead of failing silently.
    bool ok = false;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_handle(), "DELETE FROM users WHERE id=?;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
    }
    if (!ok) { request->redirect(back + "&err=history"); return; }
    request->redirect("/users");
}

// ── Admin ────────────────────────────────────────────────────────────────────
// 2026-09-29 rework: every on-device Settings value is editable here (payment accounts
// stay scan-on-the-cart, shown read-only). No login/Security card: the web login was
// removed, and the portal is gated by the cart's Admin Login + the WiFi password.
// None of the setters used here touch LVGL, so calling them from the web task is safe.

// Builds a <select> of lo..hi in `step`s with `current` selected, each shown as "<n><unit>".
static String number_select(const char *name, int lo, int hi, int step, int current, const char *unit) {
    String html = "<select name='" + String(name) + "'>";
    for (int v = lo; v <= hi; v += step) {
        html += "<option value='" + String(v) + "'" + String(v == current ? " selected" : "") + ">" +
                String(v) + unit + "</option>";
    }
    return html + "</select>";
}

// Message for an ?ok= / ?err= code on the Admin page, or "".
static String admin_notice(AsyncWebServerRequest *request) {
    if (request->hasParam("ok")) {
        String k = request->getParam("ok")->value();
        const char *msg = k == "settings" ? "Settings saved." :
                          k == "clock"    ? "Clock set." :
                          k == "pw"       ? "Password saved." :
                          k == "backup"   ? "Backup saved to the SD card." : "Saved.";
        return "<p class='ok'>" + String(msg) + "</p>";
    }
    if (request->hasParam("err")) {
        String k = request->getParam("err")->value();
        const char *msg = k == "wifipw" ? "WiFi password must be at least 8 characters." :
                          k == "clock"  ? "That date/time didn't look right." :
                          k == "backup" ? "Backup failed. Check the SD card." : "That didn't work.";
        return "<p class='balance'>" + String(msg) + "</p>";
    }
    return "";
}

// Renders the Admin page.
static String render_admin_page(AsyncWebServerRequest *request) {
    String html = "<h2>Admin</h2>";

    html += admin_notice(request);

    // Cart settings -- same ranges the on-device Settings screens cycle through.
    html += "<form method='POST' action='/admin/settings' class='card'><h3>Cart Settings</h3><div class='grid'>";
    html += "<label>Brightness" + number_select("brightness", 10, 100, 10, backlight_normal_get_pct(), "%") + "</label>";
    html += "<label>Screensaver after" + number_select("idle", 1, 10, 1, idle_timer_get_minutes(), " min") + "</label>";
    html += "<label>Screensaver dim" + number_select("dim", 10, 100, 10, screensaver_dim_get_pct(), "%") + "</label>";
    html += "<label>Auto logout after" + number_select("logout", 1, 10, 1, session_timer_get_minutes(), " min") + "</label>";
    html += "</div><p><label><input type='checkbox' name='enroll' value='1'" +
            String(users_auto_enroll_enabled() ? " checked" : "") +
            "> Auto Enroll <span class='muted'>(unknown badges can sign themselves up)</span></label></p>";
    html += "<button type='submit'>Save</button></form>";

    // Clock -- prefilled with the cart's own time (local wall-clock stored as UTC, see
    // format_epoch()), so the admin just corrects what's wrong.
    time_t now = time(nullptr);
    struct tm tmval;
    gmtime_r(&now, &tmval);
    char dt[20];
    strftime(dt, sizeof(dt), "%Y-%m-%dT%H:%M", &tmval);
    html += "<form method='POST' action='/admin/clock' class='card'><h3>Clock</h3>";
    html += "<input class='wide' type='datetime-local' name='dt' value='" + String(dt) + "'> ";
    html += "<button type='submit'>Set</button></form>";

    // WiFi password -- never echoed back into the page.
    html += "<form method='POST' action='/admin/passwords' class='card'><h3>WiFi Password</h3>";
    html += "<input class='wide' name='wifi' minlength='8' placeholder='unchanged'> <button type='submit'>Save</button>";
    html += "<p class='muted'>At least 8 characters. Applies the next time the Web Portal is opened on the cart.</p></form>";

    // Payment accounts -- read-only: set up on the cart by scanning the owner's own app QR.
    html += "<div class='card'><h3>Payment Accounts</h3><table>";
    static const char *METHODS[] = { "venmo", "cashapp", "zelle" };
    for (const char *m : METHODS) {
        char handle[PAYMENT_HANDLE_MAX] = "", owner[64] = "";
        sqlite3_stmt *pm_stmt;
        if (sqlite3_prepare_v2(db_handle(), "SELECT display_name, handle FROM payment_methods WHERE method=?;", -1, &pm_stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(pm_stmt, 1, m, -1, SQLITE_STATIC);
            if (sqlite3_step(pm_stmt) == SQLITE_ROW) {
                const unsigned char *dn = sqlite3_column_text(pm_stmt, 0);
                const unsigned char *hd = sqlite3_column_text(pm_stmt, 1);
                if (dn) strncpy(owner,  (const char *)dn, sizeof(owner) - 1);
                if (hd) strncpy(handle, (const char *)hd, sizeof(handle) - 1);
            }
            sqlite3_finalize(pm_stmt);
        }
        html += "<tr><td>" + String(payment_method_label(m)) + "</td><td>";
        if (handle[0]) {
            char shown[80];
            payment_display_handle(m, handle, shown, sizeof(shown));
            html += html_escape(String(shown)) + " <span class='muted'>(" + html_escape(String(owner)) + ")</span>";
        } else {
            html += "<span class='muted'>Not set</span>";
        }
        html += "</td></tr>";
    }
    html += "</table><p class='muted'>Changed on the cart: Settings &gt; Payment Info.</p></div>";

    // Device status + manual backup.
    html += "<div class='card'><h3>Device</h3><table>";
    html += "<tr><td>Uptime</td><td>" + format_uptime() + "</td></tr>";
    if (SD.cardType() != CARD_NONE) {
        html += "<tr><td>SD card</td><td>" + String((unsigned long)(SD.usedBytes() / 1024)) + " KB used of " +
                String((unsigned long)(SD.totalBytes() / (1024ULL * 1024ULL))) + " MB</td></tr>";
    } else {
        html += "<tr><td>SD card</td><td class='balance'>not detected</td></tr>";
    }
    html += "<tr><td>Free memory</td><td>" + String(ESP.getFreeHeap() / 1024) + " KB</td></tr>";
    html += "<tr><td>Phones connected</td><td>" + String(WiFi.softAPgetStationNum()) + "</td></tr>";
    html += "</table><form method='POST' action='/admin/backup'><button>Back up now</button></form> "
            "<span class='muted'>Also happens automatically when the Web Portal closes.</span></div>";

    return render_page("Admin", "Admin", html);
}

// GET /admin -- the Admin page.
static void handle_admin(AsyncWebServerRequest *request) {
    request->send(200, "text/html", render_admin_page(request));
}

// Reads an int form field, or `fallback` if it's missing.
static int form_int(AsyncWebServerRequest *request, const char *name, int fallback) {
    return request->hasParam(name, true) ? request->getParam(name, true)->value().toInt() : fallback;
}

// POST /admin/settings -- brightness, screensaver, auto logout, auto enroll. Each setter
// clamps/persists on its own, and brightness applies live.
static void handle_admin_settings(AsyncWebServerRequest *request) {
    session_timer_reset();  // web activity keeps the cart from auto-logging out
    backlight_normal_set_pct(form_int(request, "brightness", backlight_normal_get_pct()));
    idle_timer_set_minutes(form_int(request, "idle", idle_timer_get_minutes()));
    screensaver_dim_set_pct(form_int(request, "dim", screensaver_dim_get_pct()));
    session_timer_set_minutes(form_int(request, "logout", session_timer_get_minutes()));
    // Unchecked checkboxes aren't sent at all -- presence is what "checked" means.
    users_set_auto_enroll(request->hasParam("enroll", true));
    request->redirect("/admin?ok=settings");
}

// POST /admin/clock -- sets the system clock and the DS3231, same as on-device Set Clock
// (see screen_set_clock.cpp's cb_set() for the no-timezone convention).
static void handle_admin_clock(AsyncWebServerRequest *request) {
    session_timer_reset();  // web activity keeps the cart from auto-logging out
    String dt = request->hasParam("dt", true) ? request->getParam("dt", true)->value() : "";
    struct tm tmval = {};
    int y, mo, d, h, mi;
    if (sscanf(dt.c_str(), "%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) != 5 || y < 2024 || mo < 1 || mo > 12) {
        request->redirect("/admin?err=clock");
        return;
    }
    tmval.tm_year = y - 1900;
    tmval.tm_mon  = mo - 1;
    tmval.tm_mday = d;
    tmval.tm_hour = h;
    tmval.tm_min  = mi;
    struct timeval tv = { mktime(&tmval), 0 };
    settimeofday(&tv, nullptr);
    rtc_sync_from_system();
    Serial.printf("[CLOCK] Set from web to %s\n", dt.c_str());
    request->redirect("/admin?ok=clock");
}

// POST /admin/passwords -- the WiFi password; blank leaves it unchanged.
static void handle_admin_passwords(AsyncWebServerRequest *request) {
    session_timer_reset();  // web activity keeps the cart from auto-logging out
    String wifi = request->hasParam("wifi", true) ? request->getParam("wifi", true)->value() : "";
    wifi.trim();
    if (wifi.length() && !webserver_set_ap_password(wifi.c_str())) { request->redirect("/admin?err=wifipw"); return; }
    request->redirect("/admin?ok=pw");
}

// POST /admin/backup -- copies pos.db to pos_backup.db right now.
static void handle_admin_backup(AsyncWebServerRequest *request) {
    session_timer_reset();  // web activity keeps the cart from auto-logging out
    request->redirect(db_backup_now() ? "/admin?ok=backup" : "/admin?err=backup");
}

// ── init ─────────────────────────────────────────────────────────────────────

// Returns the fixed AP network name.
const char *webserver_ap_ssid()     { return AP_SSID; }
// Returns the current AP password (config-backed with a fallback default).
const char *webserver_ap_password() { return get_ap_password(); }

// Called from screen_wifi_password.cpp's editor. Rejects anything under 8 chars outright
// (WPA2-PSK's real minimum, not a policy choice) rather than silently saving something
// softAP() would just fail on later.
bool webserver_set_ap_password(const char *password) {
    if (!password || strlen(password) < 8) return false;
    return db_config_set(AP_PASSWORD_CONFIG_KEY, password);
}

// Powers the WiFi radio on in AP mode and starts listening for HTTP connections.
void webserver_start_ap() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, get_ap_password());  // re-read every time -- picks up a change with no reboot needed
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[WEB] AP \"%s\" up — connect, then browse to http://%s/\n", AP_SSID, ip.toString().c_str());
    _server.begin();  // radio was off (WIFI_OFF drops the listening socket) -- re-bind every time
    _ap_running = true;
}

bool webserver_ap_running() { return _ap_running; }

// Disconnects any clients and powers the WiFi radio off entirely.
// Idempotent -- called from the Web Portal screen's Back and from main.cpp's loop() guard.
void webserver_stop_ap() {
    if (!_ap_running) return;
    _ap_running = false;
    WiFi.softAPdisconnect(true);  // kick any connected clients, tear down the AP
    WiFi.mode(WIFI_OFF);          // power the radio down fully, not just disconnect
    // Web edits (bulk item entry etc.) aren't covered by the boot/checkout backups, so
    // back up as the portal closes.
    if (!db_backup_now()) Serial.println("[WEB] backup on portal close FAILED");
    Serial.println("[WEB] AP down, radio off");
}

// Registers every HTTP route. Call once at boot -- does not touch the radio.
void webserver_init() {
    _server.on("/", HTTP_GET, handle_home);
    _server.on("/balance", HTTP_GET, handle_balance);
    _server.on("/checkout/clear", HTTP_POST, handle_checkout_clear);
    _server.on("/item-edit", HTTP_GET, handle_item_edit_page);  // not "/item/edit" -- "/item" would catch it
    _server.on("/item", HTTP_GET, handle_items);
    _server.on("/item/stock", HTTP_POST, handle_item_stock);
    _server.on("/item/add", HTTP_POST, handle_item_add);
    _server.on("/item/edit", HTTP_POST, handle_item_edit);
    _server.on("/item/delete", HTTP_POST, handle_item_delete);
    _server.on("/report-download", HTTP_GET, handle_report_download);  // not "/report/..." -- "/report" would catch it
    _server.on("/report", HTTP_GET, handle_report);
    _server.on("/user-edit", HTTP_GET, handle_user_edit_page);  // not "/users/..." -- "/users" would catch it
    _server.on("/users", HTTP_GET, handle_users);
    _server.on("/user/toggle", HTTP_POST, handle_user_toggle);
    _server.on("/user/edit", HTTP_POST, handle_user_edit);
    _server.on("/user/delete", HTTP_POST, handle_user_delete);
    _server.on("/admin", HTTP_GET, handle_admin);
    _server.on("/admin/settings", HTTP_POST, handle_admin_settings);
    _server.on("/admin/clock", HTTP_POST, handle_admin_clock);
    _server.on("/admin/passwords", HTTP_POST, handle_admin_passwords);
    _server.on("/admin/backup", HTTP_POST, handle_admin_backup);
    // No _server.begin() here -- the radio (and the listening socket that depends on it)
    // only comes up in webserver_start_ap(), called when screen_webportal.cpp is entered.
}
