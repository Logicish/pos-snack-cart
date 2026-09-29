#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Admin web-portal login password editor -- same wheel widget as
            screen_wifi_password.cpp, saves via webserver_set_admin_password().
*/

// Admin web-portal login password editor — Settings -> Security, 2026-09-14, added the
// same day the per-admin salted-SHA-256 login was replaced with one shared plaintext
// password for the whole device (see users.h / webserver.h's webserver_admin_password()).
// Same wheel-cell widget as screen_wifi_password.cpp, just a shorter minimum (4 chars —
// no WPA2-style constraint here, this only gates the web portal's edit actions). Back
// (cancel, or after Save) returns to Security.
void screen_admin_password_push();
