# Snack Cart POS

A self-contained point-of-sale terminal for an honor-system workplace snack cart. People scan
their badge, scan their snacks, and pay the cart's owner from their own phone with Venmo,
Zelle, or Cash App from a QR code on the screen. Everything is stored on an SD card inside the
cart; an admin website is served over a WiFi hotspot the cart broadcasts only while an admin
has it open.

- **Hardware:** ESP32-S3 (N16R8), 3.5" ILI9488 display, six buttons, GM65 barcode scanner,
  DS3231 real-time clock, microSD, in a 3D-printed PETG enclosure.
- **Firmware:** Arduino framework on PlatformIO, LVGL 8.4, SQLite on the SD card. Every library
  is pinned to an exact version in `platformio.ini`.
- **Manual:** the owner and admin manual is in [`docs/`](docs/), as a web page and a printable
  PDF.

Build with `pio run` from this folder.

## License

Licensed under the [Apache License 2.0](LICENSE). If you redistribute this code, or anything
built from it, the license asks you to keep the copyright notice and include the
[`NOTICE`](NOTICE) file.

## A request (not a requirement)

If you build on this project, a link back to it is appreciated. And if you improve it, I'd
love for those improvements to be shared openly too, so the next person can build on them.
