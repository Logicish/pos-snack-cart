#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- "Web Portal" -- landing screen showing the AP's WiFi credentials and a
            QR code for the portal URL. Powers the radio on while open, off on exit.
*/

// "Web Portal" — landing screen, 2026-08-28. Shows the AP's WiFi name/password to connect
// to, plus a QR code for the portal's URL (scan once already on the AP's WiFi). The radio
// itself only runs while this screen is open — webserver_start_ap() fires on entry,
// webserver_stop_ap() fires on Back — so the cart isn't broadcasting a WiFi network the
// rest of the time. Back returns to the main Admin Menu.
void screen_webportal_push();
