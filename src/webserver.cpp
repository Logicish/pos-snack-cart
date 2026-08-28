#include "webserver.h"
#include "db.h"
#include "users.h"
#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>
#include <ESPAsyncWebServer.h>
#include <sqlite3.h>
#include <time.h>
#include <esp_random.h>

// TEMPORARY — no login screen yet, this AP password is the only thing keeping a
// stranger off the admin page. Change before this ever leaves the workbench.
#define AP_SSID     "SnackCart"
#define AP_PASSWORD "snackcart123"

static AsyncWebServer _server(80);

// ── sessions (2026-08-25) ───────────────────────────────────────────────────
// Small in-memory table -- this device has 2-3 admin accounts, not thousands of users, so
// a handful of concurrent sessions is more than enough. Login is password-only, identity is
// auto-populated from users_get_current_admin() (the on-device admin session set when an
// admin badge routes to the Admin Menu — see main.cpp's on_scan()) rather than asking for a
// username, since the physical badge scan already proved identity before the web portal is
// even reachable. See snack_cart_pos.md's 2026-08-25 planning round for the full reasoning.

#define MAX_SESSIONS 4
#define SESSION_TOKEN_HEX_LEN 32

struct Session {
    bool active;
    char token[SESSION_TOKEN_HEX_LEN + 1];
    int  user_id;
};
static Session _sessions[MAX_SESSIONS];

static void make_session_token(char *out) {
    static const char hexchars[] = "0123456789abcdef";
    for (int i = 0; i < SESSION_TOKEN_HEX_LEN / 2; i++) {
        uint8_t b = (uint8_t)esp_random();
        out[i * 2]     = hexchars[b >> 4];
        out[i * 2 + 1] = hexchars[b & 0x0F];
    }
    out[SESSION_TOKEN_HEX_LEN] = '\0';
}

// Reuses the oldest slot if the (tiny) table is full rather than failing -- acceptable at
// this scale, just means the least-recently-created session gets logged out.
static int create_session(int user_id) {
    int slot = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!_sessions[i].active) { slot = i; break; }
    }
    make_session_token(_sessions[slot].token);
    _sessions[slot].user_id = user_id;
    _sessions[slot].active  = true;
    return slot;
}

static int find_session_slot(AsyncWebServerRequest *request) {
    if (!request->hasHeader("Cookie")) return -1;
    String cookie = request->header("Cookie");
    int idx = cookie.indexOf("session=");
    if (idx < 0) return -1;
    String token = cookie.substring(idx + 8);
    int end = token.indexOf(';');
    if (end >= 0) token = token.substring(0, end);

    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (_sessions[i].active && token.equals(_sessions[i].token)) return i;
    }
    return -1;
}

// Returns true and fills *out_user_id if the request carries a valid session cookie.
static bool require_auth(AsyncWebServerRequest *request, int *out_user_id = nullptr) {
    int slot = find_session_slot(request);
    if (slot < 0) return false;
    if (out_user_id) *out_user_id = _sessions[slot].user_id;
    return true;
}

static void redirect_to_login(AsyncWebServerRequest *request) {
    request->redirect("/login");
}

// ── shared helpers ──────────────────────────────────────────────────────────

static String html_escape(const String &s) {
    String out = s;
    out.replace("&", "&amp;");
    out.replace("<", "&lt;");
    out.replace(">", "&gt;");
    out.replace("\"", "&quot;");
    return out;
}

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
    ".content{padding:16px;}"
    "h2{margin-top:0;}"
    "table{width:100%;border-collapse:collapse;margin-bottom:24px;}"
    "th,td{padding:8px;border-bottom:1px solid #333;text-align:left;}"
    "input{padding:4px;background:#1E1E1E;color:#EFEFEF;border:1px solid #444;width:70px;}"
    "input[name=name],input[name=first],input[name=last]{width:120px;}"
    "input[name=venmo_handle],input[name=owner_name]{width:220px;}"
    "button{padding:6px 12px;background:#00BCD4;color:#000;border:none;border-radius:4px;cursor:pointer;}"
    "button.del{background:#F44336;color:#fff;}"
    "form{display:inline;}"
    ".card{background:#1E1E1E;border-radius:8px;padding:12px 16px;margin-bottom:12px;}"
    ".stat-row{display:flex;flex-wrap:wrap;gap:12px;margin-bottom:24px;}"
    ".stat{background:#1E1E1E;border-radius:8px;padding:16px;flex:1;min-width:130px;}"
    ".stat .value{font-size:28px;color:#00BCD4;}"
    ".stat .label{font-size:13px;color:#AAA;}"
    ".muted{color:#AAA;font-size:13px;}"
    ".balance{color:#FFB74D;}"
    ".ok{color:#4CAF50;}";

struct NavTab { const char *label; const char *href; };
static const NavTab NAV_TABS[] = {
    {"Home",    "/"},
    {"Balance", "/balance"},
    {"Items",   "/item"},
    {"Report",  "/report"},
    {"Users",   "/users"},
    {"Admin",   "/admin"},
};

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

static String render_page(const String &title, const char *active, const String &body) {
    String html;
    html += "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<title>" + title + "</title><style>" + String(PAGE_CSS) + "</style></head><body>";
    html += render_nav(active);
    html += "<div class='content'>" + body + "</div>";
    html += "</body></html>";
    return html;
}

// ── Login / Logout / Change Password ────────────────────────────────────────
// Not in NAV_TABS -- reached by redirect from a gated action, or by visiting /login directly.

static String render_login_page(const char *error) {
    int admin_id = users_get_current_admin();
    if (admin_id < 0) {
        return render_page("Login", "Admin",
            "<h2>Login</h2><p class='muted'>No admin is currently signed in on the device. "
            "Scan an admin badge on the cart, then reload this page.</p>");
    }

    const User *u = users_get_by_id(admin_id);
    String name = u ? String(u->first_name) : "Admin";

    String html = "<h2>Login</h2>";
    html += "<p>Signed in on the device as <b>" + html_escape(name) + "</b>.</p>";
    if (error) html += "<p class='balance'>" + String(error) + "</p>";
    html += "<form method='POST' action='/login'>";
    html += "Password: <input type='password' name='password' autofocus required>";
    html += "<button type='submit'>Log In</button></form>";
    return render_page("Login", "Admin", html);
}

static void handle_login(AsyncWebServerRequest *request) {
    request->send(200, "text/html", render_login_page(nullptr));
}

static void handle_login_post(AsyncWebServerRequest *request) {
    int admin_id = users_get_current_admin();
    if (admin_id < 0 || !request->hasParam("password", true)) {
        request->send(200, "text/html", render_login_page("No admin session -- scan an admin badge on the device first."));
        return;
    }

    String password = request->getParam("password", true)->value();
    if (!users_verify_password(admin_id, password.c_str())) {
        request->send(200, "text/html", render_login_page("Wrong password."));
        return;
    }

    int slot = create_session(admin_id);
    String target = users_is_password_default(admin_id) ? "/change-password" : "/";

    AsyncWebServerResponse *response = request->beginResponse(302, "text/plain", "");
    response->addHeader("Location", target);
    char cookie[64];
    snprintf(cookie, sizeof(cookie), "session=%s; Path=/; HttpOnly", _sessions[slot].token);
    response->addHeader("Set-Cookie", cookie);
    request->send(response);
}

static void handle_logout(AsyncWebServerRequest *request) {
    int slot = find_session_slot(request);
    if (slot >= 0) _sessions[slot].active = false;

    AsyncWebServerResponse *response = request->beginResponse(302, "text/plain", "");
    response->addHeader("Location", "/login");
    response->addHeader("Set-Cookie", "session=; Path=/; Max-Age=0");
    request->send(response);
}

static String render_change_password_page(const char *error, bool forced) {
    String html = "<h2>Change Password</h2>";
    if (forced) {
        html += "<p class='balance'>This admin account is still on the shared default "
                "password -- please set a real one now.</p>";
    }
    if (error) html += "<p class='balance'>" + String(error) + "</p>";
    html += "<form method='POST' action='/change-password'>";
    html += "New password: <input type='password' name='password' autofocus required><br><br>";
    html += "Confirm: <input type='password' name='confirm' required><br><br>";
    html += "<button type='submit'>Save</button></form>";
    return render_page("Change Password", "Admin", html);
}

static void handle_change_password(AsyncWebServerRequest *request) {
    int user_id;
    if (!require_auth(request, &user_id)) { redirect_to_login(request); return; }
    request->send(200, "text/html", render_change_password_page(nullptr, users_is_password_default(user_id)));
}

static void handle_change_password_post(AsyncWebServerRequest *request) {
    int user_id;
    if (!require_auth(request, &user_id)) { redirect_to_login(request); return; }

    String password = request->hasParam("password", true) ? request->getParam("password", true)->value() : "";
    String confirm   = request->hasParam("confirm", true)  ? request->getParam("confirm", true)->value()  : "";

    if (password.length() < 4) {
        request->send(200, "text/html", render_change_password_page("Password too short.", users_is_password_default(user_id)));
        return;
    }
    if (password != confirm) {
        request->send(200, "text/html", render_change_password_page("Passwords don't match.", users_is_password_default(user_id)));
        return;
    }

    users_set_password(user_id, password.c_str());
    request->redirect("/");
}

// ── Home ─────────────────────────────────────────────────────────────────────

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

static void handle_home(AsyncWebServerRequest *request) {
    request->send(200, "text/html", render_home_page());
}

// ── Items (was the root page) ───────────────────────────────────────────────

static String render_items_page() {
    String html = "<h2>Items</h2><table>";
    html += "<tr><th>Name</th><th>Price ($)</th><th>Stocked</th><th></th></tr>";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_handle(), "SELECT id, name, price_cents, stocked FROM items ORDER BY name COLLATE NOCASE;", -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int id             = sqlite3_column_int(stmt, 0);
            String name        = (const char *)sqlite3_column_text(stmt, 1);
            int price_cents    = sqlite3_column_int(stmt, 2);
            int stocked        = sqlite3_column_int(stmt, 3);

            html += "<tr><form method='POST' action='/item/edit'>";
            html += "<input type='hidden' name='id' value='" + String(id) + "'>";
            html += "<td><input name='name' value='" + html_escape(name) + "'></td>";
            html += "<td><input name='price' type='number' step='0.01' value='" + String(price_cents / 100.0, 2) + "'></td>";
            html += "<td><input name='stocked' type='number' value='" + String(stocked) + "'></td>";
            html += "<td><button type='submit'>Save</button></form> ";
            html += "<form method='POST' action='/item/delete' onsubmit=\"return confirm('Delete this item?');\">";
            html += "<input type='hidden' name='id' value='" + String(id) + "'>";
            html += "<button class='del' type='submit'>Delete</button></form></td></tr>";
        }
        sqlite3_finalize(stmt);
    }

    html += "</table><h3>Add Item</h3>";
    html += "<form method='POST' action='/item/add'>";
    html += "Name: <input name='name' required> ";
    html += "Price ($): <input name='price' type='number' step='0.01' required> ";
    html += "Stocked: <input name='stocked' type='number' value='0'> ";
    html += "<button type='submit'>Add</button></form>";
    return render_page("Items", "Items", html);
}

static void handle_items(AsyncWebServerRequest *request) {
    request->send(200, "text/html", render_items_page());
}

static void handle_item_add(AsyncWebServerRequest *request) {
    if (!require_auth(request)) { redirect_to_login(request); return; }
    if (request->hasParam("name", true) && request->hasParam("price", true)) {
        String name    = request->getParam("name", true)->value();
        float  price   = request->getParam("price", true)->value().toFloat();
        int    stocked = request->hasParam("stocked", true) ? request->getParam("stocked", true)->value().toInt() : 0;

        sqlite3_stmt *stmt;
        const char *sql = "INSERT INTO items (name, price_cents, stocked) VALUES (?, ?, ?);";
        if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, (int)(price * 100 + 0.5));
            sqlite3_bind_int(stmt, 3, stocked);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    request->redirect("/item");
}

static void handle_item_edit(AsyncWebServerRequest *request) {
    if (!require_auth(request)) { redirect_to_login(request); return; }
    if (request->hasParam("id", true) && request->hasParam("name", true) && request->hasParam("price", true)) {
        int    id      = request->getParam("id", true)->value().toInt();
        String name    = request->getParam("name", true)->value();
        float  price   = request->getParam("price", true)->value().toFloat();
        int    stocked = request->hasParam("stocked", true) ? request->getParam("stocked", true)->value().toInt() : 0;

        sqlite3_stmt *stmt;
        const char *sql = "UPDATE items SET name=?, price_cents=?, stocked=? WHERE id=?;";
        if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 2, (int)(price * 100 + 0.5));
            sqlite3_bind_int(stmt, 3, stocked);
            sqlite3_bind_int(stmt, 4, id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    request->redirect("/item");
}

static void handle_item_delete(AsyncWebServerRequest *request) {
    if (!require_auth(request)) { redirect_to_login(request); return; }
    if (request->hasParam("id", true)) {
        int id = request->getParam("id", true)->value().toInt();
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db_handle(), "DELETE FROM items WHERE id=?;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    request->redirect("/item");
}

// ── Balance ("the clearable list") ─────────────────────────────────────────

static String render_balance_page() {
    String html = "<h2>Balance</h2>";
    html += "<form method='POST' action='/checkout/clear'>";

    sqlite3_stmt *stmt;
    const char *sql =
        "SELECT c.id, c.user_id, u.first_name, u.last_name, c.total_price_cents, c.created_at "
        "FROM checkouts c JOIN users u ON u.id = c.user_id "
        "WHERE c.cleared_at IS NULL "
        "ORDER BY u.first_name COLLATE NOCASE, u.last_name COLLATE NOCASE, c.created_at;";

    bool any = false;
    int open_user_id = -1;

    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            any = true;
            int    checkout_id = sqlite3_column_int(stmt, 0);
            int    user_id     = sqlite3_column_int(stmt, 1);
            String first       = (const char *)sqlite3_column_text(stmt, 2);
            String last        = (const char *)sqlite3_column_text(stmt, 3);
            int    total       = sqlite3_column_int(stmt, 4);
            long   created_at  = sqlite3_column_int64(stmt, 5);

            if (user_id != open_user_id) {
                if (open_user_id != -1) html += "</table></div>";
                html += "<div class='card'><h3>" + html_escape(first) + " " + html_escape(last) + "</h3><table>";
                html += "<tr><th></th><th>#</th><th>Date</th><th>Items</th><th>Total</th></tr>";
                open_user_id = user_id;
            }

            String items;
            sqlite3_stmt *istmt;
            const char *isql =
                "SELECT i.name, ci.quantity FROM checkout_items ci "
                "JOIN items i ON i.id = ci.item_id WHERE ci.checkout_id=?;";
            if (sqlite3_prepare_v2(db_handle(), isql, -1, &istmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(istmt, 1, checkout_id);
                while (sqlite3_step(istmt) == SQLITE_ROW) {
                    if (items.length()) items += ", ";
                    String iname = (const char *)sqlite3_column_text(istmt, 0);
                    int qty = sqlite3_column_int(istmt, 1);
                    items += html_escape(iname);
                    if (qty > 1) items += " x" + String(qty);
                }
                sqlite3_finalize(istmt);
            }

            html += "<tr><td><input type='checkbox' name='id' value='" + String(checkout_id) + "'></td>";
            html += "<td>#" + String(checkout_id) + "</td>";
            html += "<td>" + format_epoch(created_at) + "</td>";
            html += "<td>" + (items.length() ? items : String("&mdash;")) + "</td>";
            html += "<td>" + format_cents(total) + "</td></tr>";
        }
        sqlite3_finalize(stmt);
    }
    if (open_user_id != -1) html += "</table></div>";

    if (!any) {
        html += "<p class='muted'>No outstanding balances.</p>";
    } else {
        html += "<button type='submit' onsubmit=\"return confirm('Mark selected checkouts as paid?');\">Clear Selected</button>";
    }
    html += "</form>";
    return render_page("Balance", "Balance", html);
}

static void handle_balance(AsyncWebServerRequest *request) {
    request->send(200, "text/html", render_balance_page());
}

static void handle_checkout_clear(AsyncWebServerRequest *request) {
    if (!require_auth(request)) { redirect_to_login(request); return; }
    // No RTC/NTP wired yet (see project notes) — time(nullptr) is a placeholder that will
    // read real wall-clock time once that lands, with no schema/query changes needed here.
    long now = (long)time(nullptr);
    int params = request->params();
    for (int i = 0; i < params; i++) {
        const AsyncWebParameter *p = request->getParam(i);
        if (p->name() != "id") continue;
        int id = p->value().toInt();
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db_handle(), "UPDATE checkouts SET cleared_at=? WHERE id=? AND cleared_at IS NULL;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, now);
            sqlite3_bind_int(stmt, 2, id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    request->redirect("/balance");
}

// ── Report (read-only lifetime totals) ──────────────────────────────────────

static String render_report_page() {
    String html = "<h2>Report</h2>";

    html += "<h3>Per-item</h3><table>";
    html += "<tr><th>Item</th><th>Sold</th><th>Accounted for</th><th>Stocked</th></tr>";
    sqlite3_stmt *stmt;
    const char *item_sql =
        "SELECT i.name, i.stocked, "
        "  COALESCE(SUM(ci.quantity),0), "
        "  COALESCE(SUM(CASE WHEN c.cleared_at IS NOT NULL THEN ci.quantity ELSE 0 END),0) "
        "FROM items i "
        "LEFT JOIN checkout_items ci ON ci.item_id = i.id "
        "LEFT JOIN checkouts c ON c.id = ci.checkout_id "
        "GROUP BY i.id ORDER BY i.name COLLATE NOCASE;";
    if (sqlite3_prepare_v2(db_handle(), item_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            String name       = (const char *)sqlite3_column_text(stmt, 0);
            int    stocked    = sqlite3_column_int(stmt, 1);
            int    sold       = sqlite3_column_int(stmt, 2);
            int    accounted  = sqlite3_column_int(stmt, 3);
            html += "<tr><td>" + html_escape(name) + "</td><td>" + String(sold) + "</td><td>" +
                    String(accounted) + "</td><td>" + String(stocked) + "</td></tr>";
        }
        sqlite3_finalize(stmt);
    }
    html += "</table>";

    html += "<h3>Per-user</h3><table>";
    html += "<tr><th>User</th><th>Items bought</th><th>Total spent</th><th>Balance</th></tr>";
    const char *user_sql =
        "SELECT u.first_name, u.last_name, "
        "  COALESCE(SUM(ci.quantity),0), "
        "  COALESCE(SUM(ci.price_cents*ci.quantity),0), "
        "  COALESCE(SUM(CASE WHEN c.cleared_at IS NULL THEN ci.price_cents*ci.quantity ELSE 0 END),0) "
        "FROM users u "
        "LEFT JOIN checkouts c ON c.user_id = u.id "
        "LEFT JOIN checkout_items ci ON ci.checkout_id = c.id "
        "GROUP BY u.id ORDER BY u.first_name COLLATE NOCASE, u.last_name COLLATE NOCASE;";
    if (sqlite3_prepare_v2(db_handle(), user_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            String first   = (const char *)sqlite3_column_text(stmt, 0);
            String last    = (const char *)sqlite3_column_text(stmt, 1);
            int    bought  = sqlite3_column_int(stmt, 2);
            int    spent   = sqlite3_column_int(stmt, 3);
            int    balance = sqlite3_column_int(stmt, 4);
            html += "<tr><td>" + html_escape(first) + " " + html_escape(last) + "</td><td>" +
                    String(bought) + "</td><td>" + format_cents(spent) + "</td><td>" +
                    (balance > 0 ? "<span class='balance'>" + format_cents(balance) + "</span>" : "<span class='ok'>$0.00</span>") +
                    "</td></tr>";
        }
        sqlite3_finalize(stmt);
    }
    html += "</table>";

    return render_page("Report", "Report", html);
}

static void handle_report(AsyncWebServerRequest *request) {
    request->send(200, "text/html", render_report_page());
}

// ── Users ────────────────────────────────────────────────────────────────────

static String render_users_page() {
    String html = "<h2>Users</h2><table>";
    html += "<tr><th>First</th><th>Last</th><th>Badge</th><th>Registered</th><th>Admin</th><th>Active</th><th></th></tr>";

    sqlite3_stmt *stmt;
    const char *sql = "SELECT id, first_name, last_name, badge_barcode, created_at, active, admin, password_is_default FROM users ORDER BY first_name COLLATE NOCASE, last_name COLLATE NOCASE;";
    if (sqlite3_prepare_v2(db_handle(), sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int    id       = sqlite3_column_int(stmt, 0);
            String first    = (const char *)sqlite3_column_text(stmt, 1);
            String last     = (const char *)sqlite3_column_text(stmt, 2);
            String badge    = (const char *)sqlite3_column_text(stmt, 3);
            long   created  = sqlite3_column_int64(stmt, 4);
            bool   active   = sqlite3_column_int(stmt, 5) != 0;
            bool   admin    = sqlite3_column_int(stmt, 6) != 0;
            bool   is_default = sqlite3_column_int(stmt, 7) != 0;

            html += "<tr" + String(active ? "" : " style='opacity:0.5'") + "><form method='POST' action='/user/edit'>";
            html += "<input type='hidden' name='id' value='" + String(id) + "'>";
            html += "<td><input name='first' value='" + html_escape(first) + "'></td>";
            html += "<td><input name='last' value='" + html_escape(last) + "'></td>";
            html += "<td class='muted'>" + html_escape(badge) + "</td>";
            html += "<td class='muted'>" + format_epoch(created) + "</td>";
            html += "<td>" + String(admin ? "yes" : "&mdash;") + "</td>";
            html += "<td><input type='checkbox' name='active' value='1'" + String(active ? " checked" : "") + "></td>";
            html += "<td><button type='submit'>Save</button></form> ";
            html += "<form method='POST' action='/user/delete' onsubmit=\"return confirm('Delete this user? Their checkout history stays but will show as an unknown user.');\">";
            html += "<input type='hidden' name='id' value='" + String(id) + "'>";
            html += "<button class='del' type='submit'>Delete</button></form>";
            if (admin) {
                html += " <form method='POST' action='/user/reset-password' onsubmit=\"return confirm('Reset password to the shared default?');\">";
                html += "<input type='hidden' name='id' value='" + String(id) + "'>";
                html += "<button type='submit'>Reset PW" + String(is_default ? " (default)" : "") + "</button></form>";
            }
            html += "</td></tr>";
        }
        sqlite3_finalize(stmt);
    }
    html += "</table><p class='muted'>Regular users register by scanning an unrecognized badge on the device "
            "and having an admin walk them through Add User (Admin Menu) — no self-service, no add-user form "
            "here. Unchecking Active turns a badge away at the device (with a message) instead of starting a "
            "transaction, without deleting them or touching their checkout history — use this instead of "
            "Delete for e.g. an outstanding balance. \"Reset PW\" puts an admin account back on the shared "
            "default password (forces them to set a new one on next login) — any logged-in admin can do this "
            "to any admin, including themselves, on the assumption this is a small trust-based system.</p>";
    return render_page("Users", "Users", html);
}

static void handle_users(AsyncWebServerRequest *request) {
    request->send(200, "text/html", render_users_page());
}

static void handle_user_edit(AsyncWebServerRequest *request) {
    if (!require_auth(request)) { redirect_to_login(request); return; }
    if (request->hasParam("id", true) && request->hasParam("first", true) && request->hasParam("last", true)) {
        int    id     = request->getParam("id", true)->value().toInt();
        String first  = request->getParam("first", true)->value();
        String last   = request->getParam("last", true)->value();
        // Unchecked checkboxes aren't sent at all in an HTML form POST — presence, not
        // value, is what "checked" means here.
        bool   active = request->hasParam("active", true);

        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db_handle(), "UPDATE users SET first_name=?, last_name=?, active=? WHERE id=?;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, first.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, last.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 3, active ? 1 : 0);
            sqlite3_bind_int(stmt, 4, id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    request->redirect("/users");
}

static void handle_user_delete(AsyncWebServerRequest *request) {
    if (!require_auth(request)) { redirect_to_login(request); return; }
    if (request->hasParam("id", true)) {
        int id = request->getParam("id", true)->value().toInt();
        // checkouts.user_id REFERENCES users(id) with no ON DELETE CASCADE and
        // foreign_keys=ON — deleting a user with existing checkouts fails silently
        // here (rc != SQLITE_DONE, no error surfaced) rather than orphaning history.
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db_handle(), "DELETE FROM users WHERE id=?;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    request->redirect("/users");
}

static void handle_user_reset_password(AsyncWebServerRequest *request) {
    if (!require_auth(request)) { redirect_to_login(request); return; }
    if (request->hasParam("id", true)) {
        int id = request->getParam("id", true)->value().toInt();
        users_reset_password(id);
    }
    request->redirect("/users");
}

// ── Admin ────────────────────────────────────────────────────────────────────

static String render_admin_page(AsyncWebServerRequest *request) {
    String html = "<h2>Admin</h2>";

    int auth_user_id = -1;
    bool logged_in = require_auth(request, &auth_user_id);

    html += "<div class='card'><h3>Device</h3><table>";
    html += "<tr><td>Uptime</td><td>" + format_uptime() + "</td></tr>";
    html += "<tr><td>Free heap</td><td>" + String(ESP.getFreeHeap() / 1024) + " KB</td></tr>";
    html += "<tr><td>Free PSRAM</td><td>" + String(ESP.getFreePsram() / 1024) + " KB</td></tr>";
    if (SD.cardType() != CARD_NONE) {
        html += "<tr><td>SD card size</td><td>" + String((unsigned long)(SD.cardSize() / (1024ULL * 1024ULL))) + " MB</td></tr>";
    } else {
        html += "<tr><td>SD card</td><td class='balance'>not detected</td></tr>";
    }
    html += "</table></div>";

    html += "<div class='card'><h3>WiFi</h3><table>";
    html += "<tr><td>Mode</td><td>Access Point</td></tr>";
    html += "<tr><td>SSID</td><td>" + String(AP_SSID) + "</td></tr>";
    html += "<tr><td>IP</td><td>" + WiFi.softAPIP().toString() + "</td></tr>";
    html += "<tr><td>Connected devices</td><td>" + String(WiFi.softAPgetStationNum()) + "</td></tr>";
    html += "</table></div>";

    char venmo[48] = "";
    char owner[32] = "";
    sqlite3_stmt *pm_stmt;
    if (sqlite3_prepare_v2(db_handle(), "SELECT display_name, handle FROM payment_methods WHERE method='venmo';", -1, &pm_stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(pm_stmt) == SQLITE_ROW) {
            const unsigned char *dn = sqlite3_column_text(pm_stmt, 0);
            const unsigned char *hd = sqlite3_column_text(pm_stmt, 1);
            if (dn) strncpy(owner, (const char *)dn, sizeof(owner) - 1);
            if (hd) strncpy(venmo, (const char *)hd, sizeof(venmo) - 1);
        }
        sqlite3_finalize(pm_stmt);
    }
    html += "<div class='card'><h3>Payment Settings</h3>";
    html += "<form method='POST' action='/admin/settings'>";
    html += "Venmo handle: <input name='venmo_handle' value='" + html_escape(String(venmo)) +
            "' placeholder='your-venmo-username'><br><br>";
    html += "Owner name: <input name='owner_name' value='" + html_escape(String(owner)) +
            "' placeholder='shown as \"Payment to: ...\"'><br><br>";
    html += "<button type='submit'>Save</button></form>";
    html += "<p class='muted'>Used by the on-device Payment screen to build the Venmo QR "
            "code (venmo.com/&lt;handle&gt;) and the \"Payment to:\" label. Leave the handle "
            "blank and Payment falls back to a \"not configured\" message instead of a QR. "
            "Stored in its own <code>payment_methods</code> table (not tied to any user row) "
            "so this can be cleared cleanly if the cart ever changes hands, without touching "
            "anyone's enrollment.</p>";
    html += "</div>";

    html += "<div class='card'><h3>Security</h3>";
    if (logged_in) {
        const User *u = users_get_by_id(auth_user_id);
        html += "<p>Logged in as <b>" + html_escape(u ? String(u->first_name) : "admin") + "</b>. ";
        html += "<a href='/change-password'>Change password</a> &middot; ";
        html += "<form method='POST' action='/logout' style='display:inline'><button type='submit'>Log out</button></form></p>";
    } else {
        html += "<p><a href='/login'>Log in</a> to edit items, users, or these settings.</p>";
    }
    html += "<p class='muted'>Editing items/users/settings requires an admin login (salted SHA-256, "
            "password-only — identity comes from whichever admin badge was last scanned on the device, "
            "see users_get_current_admin()). Viewing pages doesn't require login — the WiFi AP password "
            "is still the first gate for reaching this portal at all. Change the placeholder AP "
            "password in <code>webserver.cpp</code> before this leaves the workbench.</p></div>";

    return render_page("Admin", "Admin", html);
}

static void handle_admin(AsyncWebServerRequest *request) {
    request->send(200, "text/html", render_admin_page(request));
}

static void handle_admin_settings(AsyncWebServerRequest *request) {
    if (!require_auth(request)) { redirect_to_login(request); return; }
    String handle = request->hasParam("venmo_handle", true) ? request->getParam("venmo_handle", true)->value() : "";
    String owner  = request->hasParam("owner_name", true)   ? request->getParam("owner_name", true)->value()   : "";

    // Venmo's own UI always shows the handle as "@something" — strip a leading '@' so a
    // pasted handle doesn't silently break the venmo.com/<handle> URL.
    handle.trim();
    if (handle.startsWith("@")) handle = handle.substring(1);

    // Check-then-branch, not `ON CONFLICT...DO UPDATE` — that UPSERT syntax was found
    // (2026-08-24, pulling the SD card and inspecting pos.db directly) to silently no-op
    // on this SQLite build on real hardware. See screen_menu.cpp's upsert_venmo() for
    // the same fix applied to Setup SD's identical need.
    sqlite3_stmt *stmt;
    bool exists = false;
    if (sqlite3_prepare_v2(db_handle(), "SELECT 1 FROM payment_methods WHERE method='venmo';", -1, &stmt, nullptr) == SQLITE_OK) {
        exists = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
    }

    if (exists) {
        if (sqlite3_prepare_v2(db_handle(), "UPDATE payment_methods SET display_name=?, handle=?, enabled=1 WHERE method='venmo';", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, handle.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    } else if (sqlite3_prepare_v2(db_handle(), "INSERT INTO payment_methods (method, display_name, handle, enabled) VALUES ('venmo', ?, ?, 1);", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, owner.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, handle.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    request->redirect("/admin");
}

// ── init ─────────────────────────────────────────────────────────────────────

const char *webserver_ap_ssid()     { return AP_SSID; }
const char *webserver_ap_password() { return AP_PASSWORD; }

void webserver_start_ap() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[WEB] AP \"%s\" up — connect, then browse to http://%s/\n", AP_SSID, ip.toString().c_str());
    _server.begin();  // radio was off (WIFI_OFF drops the listening socket) -- re-bind every time
}

void webserver_stop_ap() {
    WiFi.softAPdisconnect(true);  // kick any connected clients, tear down the AP
    WiFi.mode(WIFI_OFF);          // power the radio down fully, not just disconnect
    Serial.println("[WEB] AP down, radio off");
}

void webserver_init() {
    _server.on("/login", HTTP_GET, handle_login);
    _server.on("/login", HTTP_POST, handle_login_post);
    _server.on("/logout", HTTP_POST, handle_logout);
    _server.on("/change-password", HTTP_GET, handle_change_password);
    _server.on("/change-password", HTTP_POST, handle_change_password_post);
    _server.on("/user/reset-password", HTTP_POST, handle_user_reset_password);
    _server.on("/", HTTP_GET, handle_home);
    _server.on("/balance", HTTP_GET, handle_balance);
    _server.on("/checkout/clear", HTTP_POST, handle_checkout_clear);
    _server.on("/item", HTTP_GET, handle_items);
    _server.on("/item/add", HTTP_POST, handle_item_add);
    _server.on("/item/edit", HTTP_POST, handle_item_edit);
    _server.on("/item/delete", HTTP_POST, handle_item_delete);
    _server.on("/report", HTTP_GET, handle_report);
    _server.on("/users", HTTP_GET, handle_users);
    _server.on("/user/edit", HTTP_POST, handle_user_edit);
    _server.on("/user/delete", HTTP_POST, handle_user_delete);
    _server.on("/admin", HTTP_GET, handle_admin);
    _server.on("/admin/settings", HTTP_POST, handle_admin_settings);
    // No _server.begin() here -- the radio (and the listening socket that depends on it)
    // only comes up in webserver_start_ap(), called when screen_webportal.cpp is entered.
}
