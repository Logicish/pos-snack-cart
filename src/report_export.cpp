#include "report_export.h"
#include "db.h"
#include <Arduino.h>
#include <SD.h>
#include <sqlite3.h>
#include <time.h>
#include <string.h>
#include <ctype.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the report downloads declared in report_export.h: a minimal ZIP
            writer, an XLSX sheet writer on top of it, a plain-text table writer, and the
            three report tables, written once through a shared Sink interface.
*/

// ── one table cell ───────────────────────────────────────────────────────────

enum CellType { CELL_TEXT, CELL_INT, CELL_MONEY };

struct Cell {
    CellType    type;
    const char *text;   // CELL_TEXT
    long        num;    // CELL_INT: the value; CELL_MONEY: cents
};

static Cell txt(const char *s)  { return { CELL_TEXT,  s ? s : "", 0 }; }
static Cell num(long v)         { return { CELL_INT,   "", v }; }
static Cell money(long cents)   { return { CELL_MONEY, "", cents }; }

// A table writer. Each report table is: begin(), any number of row(), end().
// widths[] are plain-text column widths; a width of 0 means "print this column on its own
// indented line under the row" (the TXT writer's way of fitting long item lists).
class Sink {
public:
    virtual ~Sink() {}
    virtual void begin(const char *title, const char *const *headers, const uint8_t *widths, int ncols) = 0;
    virtual void row(const Cell *cells, int n, bool bold) = 0;
    virtual void end() = 0;
};

// ── ZIP (stored, no compression) ─────────────────────────────────────────────
// Each entry's local header is written with placeholder CRC/size, the data is streamed
// while both are computed, then the writer seeks back and patches the header. That keeps
// memory flat and avoids ZIP "data descriptors", which some readers handle poorly.

#define ZIP_MAX_ENTRIES 12

struct ZipEntry {
    char     name[40];
    uint32_t offset, crc, size;
};

static File     _zf;
static ZipEntry _zentries[ZIP_MAX_ENTRIES];
static int      _zcount;
static uint32_t _zcrc, _zsize;
static uint16_t _zdos_time, _zdos_date;

// Standard CRC-32 (the zlib/ZIP one), bitwise -- report files are small, speed is fine.
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put16(uint8_t *b, uint16_t v) { b[0] = v; b[1] = v >> 8; }
static void put32(uint8_t *b, uint32_t v) { b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24; }

static void zip_begin_entry(const char *name) {
    ZipEntry &e = _zentries[_zcount++];
    strncpy(e.name, name, sizeof(e.name) - 1);
    e.name[sizeof(e.name) - 1] = '\0';
    e.offset = _zf.position();
    _zcrc = 0;
    _zsize = 0;

    uint8_t h[30] = {};
    put32(h + 0, 0x04034b50);      // local file header signature
    put16(h + 4, 20);              // version needed
    put16(h + 8, 0);               // method 0 = stored
    put16(h + 10, _zdos_time);
    put16(h + 12, _zdos_date);
    put16(h + 26, strlen(e.name)); // CRC (14) and sizes (18, 22) patched in zip_end_entry()
    _zf.write(h, sizeof(h));
    _zf.write((const uint8_t *)e.name, strlen(e.name));
}

static void zip_write(const char *s, size_t n) {
    _zf.write((const uint8_t *)s, n);
    _zcrc = crc32_update(_zcrc, (const uint8_t *)s, n);
    _zsize += n;
}
static void zip_write(const char *s) { zip_write(s, strlen(s)); }

static void zip_end_entry() {
    ZipEntry &e = _zentries[_zcount - 1];
    e.crc  = _zcrc;
    e.size = _zsize;
    uint32_t end = _zf.position();
    uint8_t p[12];
    put32(p + 0, e.crc);
    put32(p + 4, e.size);
    put32(p + 8, e.size);
    _zf.seek(e.offset + 14);
    _zf.write(p, sizeof(p));
    _zf.seek(end);
}

static void zip_finish() {
    uint32_t cd_start = _zf.position();
    for (int i = 0; i < _zcount; i++) {
        const ZipEntry &e = _zentries[i];
        uint8_t h[46] = {};
        put32(h + 0, 0x02014b50);  // central directory header signature
        put16(h + 4, 20);          // version made by
        put16(h + 6, 20);          // version needed
        put16(h + 12, _zdos_time);
        put16(h + 14, _zdos_date);
        put32(h + 16, e.crc);
        put32(h + 20, e.size);
        put32(h + 24, e.size);
        put16(h + 28, strlen(e.name));
        put32(h + 42, e.offset);
        _zf.write(h, sizeof(h));
        _zf.write((const uint8_t *)e.name, strlen(e.name));
    }
    uint32_t cd_size = _zf.position() - cd_start;
    uint8_t eocd[22] = {};
    put32(eocd + 0, 0x06054b50);   // end of central directory signature
    put16(eocd + 8, _zcount);
    put16(eocd + 10, _zcount);
    put32(eocd + 12, cd_size);
    put32(eocd + 16, cd_start);
    _zf.write(eocd, sizeof(eocd));
}

// ── XLSX ─────────────────────────────────────────────────────────────────────
// One worksheet per table. Styles: 0 normal, 1 bold, 2 currency, 3 bold currency.

#define XLSX_MAX_SHEETS 3

static const char *XLSX_NS = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";

// Appends s to the current ZIP entry with XML special characters escaped.
static void zip_write_xml_text(const char *s) {
    for (; *s; s++) {
        switch (*s) {
            case '&':  zip_write("&amp;");  break;
            case '<':  zip_write("&lt;");   break;
            case '>':  zip_write("&gt;");   break;
            case '"':  zip_write("&quot;"); break;
            default:
                // XML 1.0 forbids most control characters; drop them rather than corrupt the file.
                if ((unsigned char)*s >= 0x20 || *s == '\t') zip_write(s, 1);
        }
    }
}

class XlsxSink : public Sink {
public:
    char sheet_names[XLSX_MAX_SHEETS][24];
    int  sheets = 0;

    void begin(const char *title, const char *const *headers, const uint8_t *widths, int ncols) override {
        strncpy(sheet_names[sheets], title, sizeof(sheet_names[0]) - 1);
        sheet_names[sheets][sizeof(sheet_names[0]) - 1] = '\0';
        char entry[40];
        snprintf(entry, sizeof(entry), "xl/worksheets/sheet%d.xml", ++sheets);
        zip_begin_entry(entry);

        char buf[160];
        snprintf(buf, sizeof(buf), "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                 "<worksheet xmlns=\"%s\">", XLSX_NS);
        zip_write(buf);
        // Header row stays pinned while scrolling.
        zip_write("<sheetViews><sheetView workbookViewId=\"0\"><pane ySplit=\"1\" topLeftCell=\"A2\" "
                  "activePane=\"bottomLeft\" state=\"frozen\"/></sheetView></sheetViews><cols>");
        for (int i = 0; i < ncols; i++) {
            // Text widths are in characters; the long "items" column (width 0 in TXT) gets 40.
            int w = widths[i] ? widths[i] + 2 : 40;
            snprintf(buf, sizeof(buf), "<col min=\"%d\" max=\"%d\" width=\"%d\" customWidth=\"1\"/>", i + 1, i + 1, w);
            zip_write(buf);
        }
        zip_write("</cols><sheetData>");
        _row = 0;

        Cell hdr[8];
        for (int i = 0; i < ncols && i < 8; i++) hdr[i] = txt(headers[i]);
        row(hdr, ncols, true);
    }

    void row(const Cell *cells, int n, bool bold) override {
        char buf[96];
        snprintf(buf, sizeof(buf), "<row r=\"%d\">", ++_row);
        zip_write(buf);
        for (int i = 0; i < n; i++) {
            char ref[8];
            snprintf(ref, sizeof(ref), "%c%d", 'A' + i, _row);
            const Cell &c = cells[i];
            if (c.type == CELL_TEXT) {
                snprintf(buf, sizeof(buf), "<c r=\"%s\" t=\"inlineStr\"%s><is><t xml:space=\"preserve\">",
                         ref, bold ? " s=\"1\"" : "");
                zip_write(buf);
                zip_write_xml_text(c.text);
                zip_write("</t></is></c>");
            } else if (c.type == CELL_INT) {
                snprintf(buf, sizeof(buf), "<c r=\"%s\"%s><v>%ld</v></c>", ref, bold ? " s=\"1\"" : "", c.num);
                zip_write(buf);
            } else {
                long a = c.num < 0 ? -c.num : c.num;
                snprintf(buf, sizeof(buf), "<c r=\"%s\" s=\"%d\"><v>%s%ld.%02ld</v></c>",
                         ref, bold ? 3 : 2, c.num < 0 ? "-" : "", a / 100, a % 100);
                zip_write(buf);
            }
        }
        zip_write("</row>");
    }

    void end() override {
        zip_write("</sheetData></worksheet>");
        zip_end_entry();
    }

private:
    int _row = 0;
};

// Writes the fixed XLSX package parts that surround the worksheets.
static void xlsx_write_package(const XlsxSink &x) {
    char buf[256];

    zip_begin_entry("[Content_Types].xml");
    zip_write("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
              "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
              "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
              "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
              "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
              "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>");
    for (int i = 1; i <= x.sheets; i++) {
        snprintf(buf, sizeof(buf), "<Override PartName=\"/xl/worksheets/sheet%d.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>", i);
        zip_write(buf);
    }
    zip_write("</Types>");
    zip_end_entry();

    zip_begin_entry("_rels/.rels");
    zip_write("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
              "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
              "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
              "</Relationships>");
    zip_end_entry();

    zip_begin_entry("xl/workbook.xml");
    snprintf(buf, sizeof(buf), "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
             "<workbook xmlns=\"%s\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets>", XLSX_NS);
    zip_write(buf);
    for (int i = 0; i < x.sheets; i++) {
        zip_write("<sheet name=\"");
        zip_write_xml_text(x.sheet_names[i]);
        snprintf(buf, sizeof(buf), "\" sheetId=\"%d\" r:id=\"rId%d\"/>", i + 1, i + 1);
        zip_write(buf);
    }
    zip_write("</sheets></workbook>");
    zip_end_entry();

    zip_begin_entry("xl/_rels/workbook.xml.rels");
    zip_write("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
              "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");
    for (int i = 1; i <= x.sheets; i++) {
        snprintf(buf, sizeof(buf), "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet%d.xml\"/>", i, i);
        zip_write(buf);
    }
    snprintf(buf, sizeof(buf), "<Relationship Id=\"rId%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>", x.sheets + 1);
    zip_write(buf);
    zip_write("</Relationships>");
    zip_end_entry();

    zip_begin_entry("xl/styles.xml");
    snprintf(buf, sizeof(buf), "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><styleSheet xmlns=\"%s\">", XLSX_NS);
    zip_write(buf);
    zip_write("<numFmts count=\"1\"><numFmt numFmtId=\"164\" formatCode=\"&quot;$&quot;#,##0.00\"/></numFmts>"
              "<fonts count=\"2\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font>"
              "<font><b/><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
              "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill>"
              "<fill><patternFill patternType=\"gray125\"/></fill></fills>"
              "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
              "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
              "<cellXfs count=\"4\">"
              "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
              "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyFont=\"1\"/>"
              "<xf numFmtId=\"164\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\"/>"
              "<xf numFmtId=\"164\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyNumberFormat=\"1\" applyFont=\"1\"/>"
              "</cellXfs>"
              "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
              "</styleSheet>");
    zip_end_entry();
}

// ── plain text ───────────────────────────────────────────────────────────────

class TxtSink : public Sink {
public:
    void begin(const char *title, const char *const *headers, const uint8_t *widths, int ncols) override {
        _ncols = ncols;
        memcpy(_widths, widths, ncols);
        char upper[32];
        size_t i = 0;
        for (; title[i] && i + 1 < sizeof(upper); i++) upper[i] = toupper((unsigned char)title[i]);
        upper[i] = '\0';
        _zf.printf("\n%s\n", upper);

        Cell hdr[8];
        for (int c = 0; c < ncols && c < 8; c++) hdr[c] = txt(headers[c]);
        row(hdr, ncols, false);
        int total = 0;
        for (int c = 0; c < ncols; c++) total += _widths[c] ? _widths[c] + 2 : 0;
        for (int c = 0; c < total - 2; c++) _zf.write('-');
        _zf.write('\n');
    }

    void row(const Cell *cells, int n, bool bold) override {
        (void)bold;
        const char *wrapped = nullptr;
        for (int i = 0; i < n; i++) {
            char val[64];
            fmt(cells[i], val, sizeof(val));
            if (_widths[i] == 0) { wrapped = cells[i].text; continue; }
            // Text left-aligned, numbers right-aligned, cut to fit the column.
            if (cells[i].type == CELL_TEXT) _zf.printf("%-*.*s", _widths[i], _widths[i], val);
            else                            _zf.printf("%*.*s", _widths[i], _widths[i], val);
            if (i + 1 < n) _zf.print("  ");
        }
        _zf.write('\n');
        if (wrapped && *wrapped) _zf.printf("    %s\n", wrapped);
    }

    void end() override {}

private:
    int     _ncols = 0;
    uint8_t _widths[8] = {};

    static void fmt(const Cell &c, char *out, size_t n) {
        if (c.type == CELL_TEXT)     snprintf(out, n, "%s", c.text);
        else if (c.type == CELL_INT) snprintf(out, n, "%ld", c.num);
        else {
            long a = c.num < 0 ? -c.num : c.num;
            snprintf(out, n, "%s$%ld.%02ld", c.num < 0 ? "-" : "", a / 100, a % 100);
        }
    }
};

// ── the three report tables ─────────────────────────────────────────────────

// SQL condition on checkouts `c` for a range; `since` binds to '?' for REPORT_LAST30.
static const char *range_where(ReportRange r) {
    switch (r) {
        case REPORT_CURRENT:   return "c.cleared_at IS NULL";
        case REPORT_LAST30: return "c.created_at >= ?";
        default:            return "1";
    }
}

static long _since;  // start of the 30-day window (local-as-UTC epoch, like created_at)

static void bind_range(sqlite3_stmt *stmt, ReportRange r, int idx) {
    if (r == REPORT_LAST30) sqlite3_bind_int64(stmt, idx, _since);
}

static void format_date(long epoch, char *out, size_t n) {
    if (epoch <= 0) { snprintf(out, n, "-"); return; }
    time_t t = (time_t)epoch;
    struct tm tmval;
    gmtime_r(&t, &tmval);  // stored as local wall-clock time labeled UTC -- see webserver.cpp
    strftime(out, n, "%Y-%m-%d %H:%M", &tmval);
}

static const char *col_text(sqlite3_stmt *s, int i) {
    const unsigned char *t = sqlite3_column_text(s, i);
    return t ? (const char *)t : "";
}

// Inventory: every item, current stock and price; Sold for the range (not for Current).
static void write_inventory(Sink &out, ReportRange r) {
    bool with_sold = (r != REPORT_CURRENT);
    static const char *H4[] = { "Item", "In stock", "Sold", "Price" };
    static const char *H3[] = { "Item", "In stock", "Price" };
    static const uint8_t W4[] = { 28, 8, 6, 8 };
    static const uint8_t W3[] = { 28, 8, 8 };
    out.begin("Inventory", with_sold ? H4 : H3, with_sold ? W4 : W3, with_sold ? 4 : 3);

    String sql = "SELECT i.name, i.hidden, i.stocked, i.price_cents, "
                 "(SELECT COALESCE(SUM(ci.quantity),0) FROM checkout_items ci JOIN checkouts c ON c.id = ci.checkout_id "
                 " WHERE ci.item_id = i.id AND " + String(range_where(r)) + ") "
                 "FROM items i ORDER BY i.name COLLATE NOCASE;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_handle(), sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) { out.end(); return; }
    bind_range(stmt, r, 1);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        char name[80];
        snprintf(name, sizeof(name), "%s%s", col_text(stmt, 0), sqlite3_column_int(stmt, 1) ? " (hidden)" : "");
        if (with_sold) {
            Cell c[] = { txt(name), num(sqlite3_column_int(stmt, 2)), num(sqlite3_column_int(stmt, 4)),
                         money(sqlite3_column_int(stmt, 3)) };
            out.row(c, 4, false);
        } else {
            Cell c[] = { txt(name), num(sqlite3_column_int(stmt, 2)), money(sqlite3_column_int(stmt, 3)) };
            out.row(c, 3, false);
        }
    }
    sqlite3_finalize(stmt);
    out.end();
}

// Users: one row per person + item bought in the range, then a bold TOTAL row per person.
static void write_users(Sink &out, ReportRange r) {
    static const char *H[] = { "Person", "Item", "Qty", "Amount" };
    static const uint8_t W[] = { 22, 24, 5, 9 };
    out.begin("Users", H, W, 4);

    String sql = "SELECT u.id, u.first_name, u.last_name, i.name, SUM(ci.quantity), SUM(ci.price_cents*ci.quantity) "
                 "FROM checkouts c JOIN users u ON u.id = c.user_id "
                 "JOIN checkout_items ci ON ci.checkout_id = c.id JOIN items i ON i.id = ci.item_id "
                 "WHERE " + String(range_where(r)) + " "
                 "GROUP BY u.id, i.id "
                 "ORDER BY u.first_name COLLATE NOCASE, u.last_name COLLATE NOCASE, u.id, i.name COLLATE NOCASE;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_handle(), sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) { out.end(); return; }
    bind_range(stmt, r, 1);

    int  cur_user = -1;
    char cur_name[64] = "";
    long qty_total = 0, amt_total = 0;
    auto flush = [&]() {
        if (cur_user < 0) return;
        Cell c[] = { txt(cur_name), txt("TOTAL"), num(qty_total), money(amt_total) };
        out.row(c, 4, true);
    };
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int uid = sqlite3_column_int(stmt, 0);
        if (uid != cur_user) {
            flush();
            cur_user = uid;
            qty_total = amt_total = 0;
            snprintf(cur_name, sizeof(cur_name), "%s %s", col_text(stmt, 1), col_text(stmt, 2));
        }
        long q = sqlite3_column_int(stmt, 4), a = sqlite3_column_int(stmt, 5);
        qty_total += q;
        amt_total += a;
        Cell c[] = { txt(cur_name), txt(col_text(stmt, 3)), num(q), money(a) };
        out.row(c, 4, false);
    }
    flush();
    sqlite3_finalize(stmt);
    out.end();
}

// Transactions: one row per checkout in the range, oldest first.
static void write_transactions(Sink &out, ReportRange r) {
    static const char *H[] = { "Date", "#", "Person", "Items", "Total", "Paid" };
    static const uint8_t W[] = { 16, 5, 22, 0, 8, 4 };  // Items: own line in TXT
    out.begin("Transactions", H, W, 6);

    String sql = "SELECT c.id, c.created_at, u.first_name, u.last_name, c.total_price_cents, c.cleared_at "
                 "FROM checkouts c JOIN users u ON u.id = c.user_id "
                 "WHERE " + String(range_where(r)) + " ORDER BY c.created_at, c.id;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db_handle(), sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) { out.end(); return; }
    bind_range(stmt, r, 1);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);

        String items;
        sqlite3_stmt *istmt;
        if (sqlite3_prepare_v2(db_handle(), "SELECT i.name, ci.quantity FROM checkout_items ci "
                               "JOIN items i ON i.id = ci.item_id WHERE ci.checkout_id=?;", -1, &istmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(istmt, 1, id);
            while (sqlite3_step(istmt) == SQLITE_ROW) {
                if (items.length()) items += ", ";
                items += col_text(istmt, 0);
                int q = sqlite3_column_int(istmt, 1);
                if (q > 1) items += " x" + String(q);
            }
            sqlite3_finalize(istmt);
        }

        char date[20], person[64];
        format_date(sqlite3_column_int64(stmt, 1), date, sizeof(date));
        snprintf(person, sizeof(person), "%s %s", col_text(stmt, 2), col_text(stmt, 3));
        bool paid = sqlite3_column_type(stmt, 5) != SQLITE_NULL;
        Cell c[] = { txt(date), num(id), txt(person), txt(items.c_str()),
                     money(sqlite3_column_int(stmt, 4)), txt(paid ? "Yes" : "No") };
        out.row(c, 6, false);
    }
    sqlite3_finalize(stmt);
    out.end();
}

static const char *range_slug(ReportRange r) {
    return r == REPORT_CURRENT ? "current" : r == REPORT_LAST30 ? "last30" : "lifetime";
}

static const char *range_label(ReportRange r) {
    return r == REPORT_CURRENT ? "Current balances" : r == REPORT_LAST30 ? "Last 30 days" : "Lifetime";
}

// ── public ───────────────────────────────────────────────────────────────────

void report_filename(ReportRange range, ReportFormat fmt, char *out, size_t out_len) {
    time_t now = time(nullptr);
    struct tm tmval;
    gmtime_r(&now, &tmval);
    char date[12];
    strftime(date, sizeof(date), "%Y-%m-%d", &tmval);
    snprintf(out, out_len, "snackcart-%s-%s.%s", range_slug(range), date, fmt == REPORT_XLSX ? "xlsx" : "txt");
}

bool report_export(ReportRange range, ReportFormat fmt) {
    if (!db_handle()) return false;
    time_t now = time(nullptr);
    _since = (long)now - 30L * 86400L;

    SD.remove(REPORT_TMP_PATH);
    _zf = SD.open(REPORT_TMP_PATH, FILE_WRITE);
    if (!_zf) return false;

    if (fmt == REPORT_XLSX) {
        // DOS timestamp for the ZIP entries (years since 1980).
        struct tm tmval;
        gmtime_r(&now, &tmval);
        _zdos_time = (tmval.tm_hour << 11) | (tmval.tm_min << 5) | (tmval.tm_sec / 2);
        _zdos_date = ((tmval.tm_year - 80) << 9) | ((tmval.tm_mon + 1) << 5) | tmval.tm_mday;
        _zcount = 0;

        XlsxSink x;
        write_inventory(x, range);
        write_users(x, range);
        write_transactions(x, range);
        xlsx_write_package(x);
        zip_finish();
    } else {
        char date[20], since[20];
        format_date((long)now, date, sizeof(date));
        _zf.printf("SNACK CART REPORT - %s\n", range_label(range));
        if (range == REPORT_LAST30) {
            format_date(_since, since, sizeof(since));
            _zf.printf("%s to %s\n", since, date);
        } else {
            _zf.printf("As of %s\n", date);
        }
        TxtSink t;
        write_inventory(t, range);
        write_users(t, range);
        write_transactions(t, range);
    }

    _zf.close();
    return true;
}
