#pragma once
#include <stddef.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Downloadable reports for the web portal's Report page (2026-09-29): an .xlsx
            with three tabs (Inventory / Users / Transactions) or a plain .txt with the
            same three sections, for one of three time ranges.
  Notes---- Everything streams straight to one scratch file on the SD card (overwritten
            every time), so RAM use stays flat no matter how much history there is. The
            .xlsx is written by hand: an uncompressed ("stored") ZIP of a few small XML
            files, with text cells inline -- no library.
*/

enum ReportRange {
    REPORT_CURRENT,   // current inventory + unpaid (pending) transactions only
    REPORT_LAST30,    // current inventory + the last 30 days of transactions
    REPORT_LIFETIME   // current inventory + every transaction ever
};

enum ReportFormat { REPORT_XLSX, REPORT_TXT };

#define REPORT_TMP_PATH "/report.tmp"

// Writes the report to REPORT_TMP_PATH. Returns false if the SD card or DB isn't usable.
bool report_export(ReportRange range, ReportFormat fmt);

// Download file name, e.g. "snackcart-last30-2026-09-29.xlsx".
void report_filename(ReportRange range, ReportFormat fmt, char *out, size_t out_len);
