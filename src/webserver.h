#pragma once

// AP-mode WiFi + admin web portal. Viewing pages is open to anyone on the AP (the AP
// password itself is the first gate, see AP_PASSWORD below); editing anything (items,
// users, settings) requires an admin login — see the session/login section in webserver.cpp.
//
// 2026-08-28 — the radio used to come up unconditionally at boot (webserver_init() called
// WiFi.mode(WIFI_AP)/softAP() directly). Split on request: webserver_init() now only
// registers HTTP routes (no RF at all, safe to call once at boot); webserver_start_ap()/
// webserver_stop_ap() actually power the radio on/off, wired to entering/leaving
// screen_webportal.cpp so the AP only broadcasts while that screen is open.
void webserver_init();  // call once from setup(), after db_init() -- route registration only

void webserver_start_ap();  // WiFi.mode(WIFI_AP) + softAP() + server.begin()
void webserver_stop_ap();   // softAPdisconnect() + WiFi.mode(WIFI_OFF) -- radio fully off

// AP_SSID/AP_PASSWORD stay #define'd privately in webserver.cpp (single source of truth
// for WiFi.softAP()) — these getters are for screen_webportal.cpp's landing screen, which
// needs to display them without duplicating the constants.
const char *webserver_ap_ssid();
const char *webserver_ap_password();
