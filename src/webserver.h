#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- AP-mode WiFi + admin web portal declarations -- radio on/off control and
            the two config-backed passwords (WiFi AP, admin login) that gate it. The
            HTTP routes/pages themselves are all internal to webserver.cpp.
*/

// AP-mode WiFi + admin web portal. Viewing pages is open to anyone on the AP (the AP
// password itself is the first gate, see webserver_ap_password() below); editing anything
// (items, users, settings) requires an admin login — see the session/login section in
// webserver.cpp.
//
// 2026-08-28 — the radio used to come up unconditionally at boot (webserver_init() called
// WiFi.mode(WIFI_AP)/softAP() directly). Split on request: webserver_init() now only
// registers HTTP routes (no RF at all, safe to call once at boot); webserver_start_ap()/
// webserver_stop_ap() actually power the radio on/off, wired to entering/leaving
// screen_webportal.cpp so the AP only broadcasts while that screen is open.
void webserver_init();  // call once from setup(), after db_init() -- route registration only

void webserver_start_ap();  // WiFi.mode(WIFI_AP) + softAP() + server.begin()
void webserver_stop_ap();   // softAPdisconnect() + WiFi.mode(WIFI_OFF) -- radio fully off

// AP_SSID stays #define'd privately in webserver.cpp; the AP password moved to the config
// table 2026-09-14 (editable on-device, Settings -> Security -> screen_wifi_password.cpp) —
// webserver_ap_password() is the single source of truth either way, reading config with a
// built-in fallback default. webserver_ap_ssid() is for screen_webportal.cpp's landing
// screen, which needs to display both without duplicating the constants.
const char *webserver_ap_ssid();
const char *webserver_ap_password();

// Saves a new AP password to config -- returns false (nothing written) if under 8 chars,
// WPA2-PSK's real minimum. Takes effect on the AP's next start (webserver_start_ap()
// re-reads it live), no reboot needed.
bool webserver_set_ap_password(const char *password);

// Admin web-portal login password -- single shared plaintext value for every admin
// (2026-09-14, replacing a per-admin salted-SHA-256 scheme; see users.h). Same
// config-backed/editable-on-device pattern as the AP password above, just gating
// /login instead of the WiFi radio -- screen_admin_password.cpp is its editor
// (Settings -> Security), same wheel widget as screen_wifi_password.cpp.
const char *webserver_admin_password();

// Saves a new admin password to config -- returns false (nothing written) if empty.
// No WPA2-style minimum here; this only gates the web portal's edit actions, not the
// WiFi radio itself, and "no need for real security" is the whole point of this design.
bool webserver_set_admin_password(const char *password);
