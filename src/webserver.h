#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- AP-mode WiFi + admin web portal declarations -- radio on/off control and
            the config-backed WiFi AP password that gates it. The HTTP routes/pages
            themselves are all internal to webserver.cpp.
*/

// AP-mode WiFi + admin web portal. No web login (removed 2026-09-29): the AP only runs
// while an admin has the Web Portal screen open on the cart, and the WiFi password (see
// webserver_ap_password() below) is the only gate on the network side.
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


// True while the AP radio is up (between webserver_start_ap() and webserver_stop_ap()).
// main.cpp's loop() uses this to shut the radio off whenever the Web Portal screen isn't
// the one showing, whatever made it leave (2026-09-29 -- auto-logout used to strand it on).
bool webserver_ap_running();
