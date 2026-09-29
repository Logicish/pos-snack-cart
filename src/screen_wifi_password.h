#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- WiFi AP password editor -- a wheel widget saving via
            webserver_set_ap_password().
*/

// WiFi AP password editor — Settings -> Security, 2026-09-14. Same wheel-cell widget as
// screen_payment_edit.cpp's handle field, single password field instead of a pair. Saves
// via webserver_set_ap_password() (rejects anything under 8 chars — WPA2-PSK's real
// minimum, not a policy choice). Back (cancel, or after Save) returns to Security.
void screen_wifi_password_push();
