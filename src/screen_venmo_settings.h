#pragma once

// "Venmo Payment Info" editor, 2026-08-28 — moved on-device from the web-only Admin
// Settings form (webserver.cpp's Payment Settings card). Edits the same payment_methods
// 'venmo' row (display_name + handle) the on-device Payment screen already reads to build
// the QR code. Back from here returns to Settings.
void screen_venmo_settings_push();
