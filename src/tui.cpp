#include "tui.hpp"
#include "logger.hpp"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#ifdef HAVE_NCURSES
#include <ncurses.h>
#endif

TUI::TUI(DataStore& store, uint16_t http_port) : store_(store), http_port_(http_port) {}
TUI::~TUI() { stop(); }
void TUI::stop() { running_ = false; }
void TUI::set_connect_callback(std::function<void(const std::string&)> cb) { connect_cb_ = std::move(cb); }

// ---------------------------------------------------------------------------
// Plain-terminal fallback
// ---------------------------------------------------------------------------
void TUI::print_snap(const BatterySnapshot& snap, const std::string& ble_st) {
    if (!snap.valid) {
        std::printf("[limonitor] BLE: %-16s  (no data yet)\n", ble_st.c_str());
        return;
    }
    std::time_t t = std::chrono::system_clock::to_time_t(snap.timestamp);
    char ts[20]; struct tm tm{}; localtime_r(&t, &tm);
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);

    const char* dir = snap.current_a > 0.01 ? "DISCH" : (snap.current_a < -0.01 ? "CHRG " : "IDLE ");
    std::printf("[%s] BLE:%-12s  %s  %.2fV  %+.2fA  %.1f%%  %.2f/%.2fAh  %.1fW  cyc:%u",
        ts, ble_st.c_str(), dir,
        snap.total_voltage_v, snap.current_a, snap.soc_pct,
        snap.remaining_ah, snap.nominal_ah, snap.power_w,
        snap.cycle_count);
    for (size_t i = 0; i < snap.temperatures_c.size(); ++i)
        std::printf("  T%zu:%.1f°C", i + 1, snap.temperatures_c[i]);
    if (snap.protection.any()) std::printf("  [!PROTECTION!]");
    std::printf("\n");
    std::fflush(stdout);
}

void TUI::run_plain() {
    std::printf("limonitor — press Ctrl-C to exit\n");
    while (running_) {
        auto snap = store_.latest();
        print_snap(snap.value_or(BatterySnapshot{}), store_.ble_state());
        for (int i = 0; i < 50 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ---------------------------------------------------------------------------
// ncurses TUI
// ---------------------------------------------------------------------------
#ifdef HAVE_NCURSES

enum { C_NORMAL=0, C_GREEN, C_YELLOW, C_RED, C_CYAN, C_HEADER, C_LOG_WARN, C_LOG_ERR };

static void init_colors() {
    start_color();
    use_default_colors();
    init_pair(C_GREEN,    COLOR_GREEN,  -1);
    init_pair(C_YELLOW,   COLOR_YELLOW, -1);
    init_pair(C_RED,      COLOR_RED,    -1);
    init_pair(C_CYAN,     COLOR_CYAN,   -1);
    init_pair(C_HEADER,   COLOR_BLACK,  COLOR_GREEN);
    init_pair(C_LOG_WARN, COLOR_YELLOW, -1);
    init_pair(C_LOG_ERR,  COLOR_RED,    -1);
}

static void draw_bar(int row, int col, int width, double fraction, int color_pair) {
    int filled = static_cast<int>(fraction * width);
    if (filled < 0) filled = 0;
    if (filled > width) filled = width;
    attron(COLOR_PAIR(color_pair));
    mvhline(row, col, ACS_BLOCK, filled);
    attroff(COLOR_PAIR(color_pair));
    mvhline(row, col + filled, '-', width - filled);
}

// Reserve this many rows at the bottom for the log panel (separator + lines).
static constexpr int LOG_PANEL_ROWS = 6;   // 1 separator + 5 log lines

static void draw_log_panel(int rows, int cols) {
    int sep_row = rows - LOG_PANEL_ROWS;
    if (sep_row < 2) return;

    // Separator
    attron(COLOR_PAIR(C_HEADER));
    mvhline(sep_row, 0, ' ', cols);
    mvprintw(sep_row, 1, " Log ");
    attroff(COLOR_PAIR(C_HEADER));

    auto entries = Logger::instance().recent(LOG_PANEL_ROWS - 1);
    int r = sep_row + 1;
    for (const auto& e : entries) {
        if (r >= rows - 1) break;
        int color = (e.level == LogLevel::ERROR) ? C_LOG_ERR :
                    (e.level == LogLevel::WARN)  ? C_LOG_WARN : C_NORMAL;
        attron(COLOR_PAIR(color));
        mvprintw(r, 1, "%-8s %s", e.time.c_str(), e.msg.c_str());
        clrtoeol();
        attroff(COLOR_PAIR(color));
        ++r;
    }
    // Blank remaining log rows
    while (r < rows - 1) { move(r, 0); clrtoeol(); ++r; }
}

void TUI::draw(const BatterySnapshot& snap, const std::string& ble_st,
               const std::vector<BatterySnapshot>& hist) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    // Usable rows: leave room for header (1), footer (1), log panel
    int content_rows = rows - 2 - LOG_PANEL_ROWS;
    erase();

    // ---- Header ----
    attron(COLOR_PAIR(C_HEADER) | A_BOLD);
    mvhline(0, 0, ' ', cols);
    mvprintw(0, 1, " limonitor  |  BLE: %-14s", ble_st.c_str());
    if (!snap.device_name.empty())
        mvprintw(0, 30, "  %s  %s", snap.device_name.c_str(), snap.ble_address.c_str());
    attroff(COLOR_PAIR(C_HEADER) | A_BOLD);

    if (!snap.valid) {
        mvprintw(2, 2, "Waiting for data...");
        draw_log_panel(rows, cols);
        refresh();
        return;
    }

    std::time_t t = std::chrono::system_clock::to_time_t(snap.timestamp);
    char ts[20]; struct tm tm{}; localtime_r(&t, &tm);
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);

    int row = 2;
    auto content_ok = [&]{ return row < content_rows + 2; };

    // ---- Pack overview ----
    attron(A_BOLD); mvprintw(row, 1, "Pack"); attroff(A_BOLD);
    row++;
    if (content_ok()) mvprintw(row++, 3, "Voltage  : %.3f V", snap.total_voltage_v);
    const char* dir = snap.current_a >  0.01 ? "-> DISCHARGE" :
                      snap.current_a < -0.01 ? "<- CHARGE   " : "   IDLE     ";
    if (content_ok()) mvprintw(row++, 3, "Current  : %+.2f A  %s", snap.current_a, dir);
    if (content_ok()) mvprintw(row++, 3, "Power    : %+.1f W",     snap.power_w);

    if (content_ok()) {
        double soc = snap.soc_pct / 100.0;
        int soc_color = soc > 0.5 ? C_GREEN : (soc > 0.2 ? C_YELLOW : C_RED);
        mvprintw(row, 3, "SoC      : %5.1f %%  [", snap.soc_pct);
        int bar_start = 24, bar_w = std::min(cols - bar_start - 2, 40);
        draw_bar(row, bar_start, bar_w, soc, soc_color);
        mvprintw(row, bar_start + bar_w, "]");
        row++;
    }
    if (content_ok()) mvprintw(row++, 3, "Capacity : %.2f / %.2f Ah", snap.remaining_ah, snap.nominal_ah);
    if (content_ok() && snap.time_remaining_h > 0.0) {
        const char* time_lbl = snap.current_a < -0.01 ? "to full" : "to empty";
        mvprintw(row++, 3, "Est. Time: %.1f h %s", snap.time_remaining_h, time_lbl);
    }
    if (content_ok()) mvprintw(row++, 3, "Cycles   : %u", snap.cycle_count);
    if (content_ok() && (snap.charge_mosfet || snap.discharge_mosfet))
        mvprintw(row++, 3, "MOS      : CHG=%s  DCHG=%s",
            snap.charge_mosfet ? "ON " : "OFF", snap.discharge_mosfet ? "ON " : "OFF");
    if (content_ok()) mvprintw(row++, 3, "Updated  : %s", ts);

    // ---- Voltage sparkline ----
    if (!hist.empty() && content_ok()) {
        int spark_w = std::min((int)hist.size(), std::min(cols - 16, 60));
        int start   = std::max(0, (int)hist.size() - spark_w);
        double vlo = 1e9, vhi = 0;
        for (int i = start; i < (int)hist.size(); ++i) {
            double v = hist[i].total_voltage_v;
            if (v < vlo) vlo = v;
            if (v > vhi) vhi = v;
        }
        double rng = vhi - vlo;
        if (rng < 0.05) rng = 0.05;
        static const char spark_ch[] = {'_', '.', ',', '-', '=', '+', '*', '#'};
        mvprintw(row, 3, "Volt hist: ");
        for (int i = start; i < (int)hist.size(); ++i) {
            int lv = static_cast<int>((hist[i].total_voltage_v - vlo) / rng * 7.0 + 0.5);
            if (lv < 0) lv = 0; if (lv > 7) lv = 7;
            mvaddch(row, 14 + (i - start), spark_ch[lv]);
        }
        row++;
    }

    // ---- Cells ----
    row++;
    if (!snap.cell_voltages_v.empty() && content_ok()) {
        attron(A_BOLD);
        mvprintw(row, 1, "Cells   min=%.3fV  max=%.3fV  delta=%.1fmV",
            snap.cell_min_v, snap.cell_max_v, snap.cell_delta_v * 1000.0);
        attroff(A_BOLD);
        row++;
        int cell_bar_w = std::min(cols / 2 - 14, 30);
        size_t max_cells = (content_rows + 2 > row) ?
            static_cast<size_t>(content_rows + 2 - row) : 0;
        size_t n_show = std::min(snap.cell_voltages_v.size(), max_cells);
        for (size_t i = 0; i < n_show && content_ok(); ++i) {
            double cv = snap.cell_voltages_v[i];
            bool bad_spread = snap.cell_delta_v > 0.010; // only warn if >10mV spread
            bool is_min = bad_spread && (std::abs(cv - snap.cell_min_v) < 0.0005);
            bool is_max = bad_spread && (std::abs(cv - snap.cell_max_v) < 0.0005);
            int cc = is_max ? C_RED : (is_min ? C_YELLOW : C_GREEN);
            mvprintw(row, 3, "C%02zu %.3fV ", i + 1, cv);
            double frac = (cv - 2.8) / (3.65 - 2.8);
            if (frac < 0) frac = 0; if (frac > 1) frac = 1;
            draw_bar(row, 13, cell_bar_w, frac, cc);

            size_t j = i + n_show;
            if (j < snap.cell_voltages_v.size()) {
                double cv2 = snap.cell_voltages_v[j];
                int cc2 = (bad_spread && std::abs(cv2 - snap.cell_max_v) < 0.0005) ? C_RED :
                          (bad_spread && std::abs(cv2 - snap.cell_min_v) < 0.0005) ? C_YELLOW : C_GREEN;
                int col2 = cols / 2;
                mvprintw(row, col2, "C%02zu %.3fV ", j + 1, cv2);
                double frac2 = (cv2 - 2.8) / (3.65 - 2.8);
                if (frac2 < 0) frac2 = 0; if (frac2 > 1) frac2 = 1;
                draw_bar(row, col2 + 10, cell_bar_w, frac2, cc2);
            }
            row++;
        }
    }

    // ---- Temperatures ----
    if (!snap.temperatures_c.empty() && content_ok()) {
        row++;
        attron(A_BOLD); mvprintw(row++, 1, "Temperatures"); attroff(A_BOLD);
        if (content_ok()) {
            std::string tline = "  ";
            for (size_t i = 0; i < snap.temperatures_c.size(); ++i) {
                char buf[24];
                std::snprintf(buf, sizeof(buf), "T%zu: %.1f\u00b0C   ", i + 1, snap.temperatures_c[i]);
                tline += buf;
            }
            mvprintw(row++, 1, "%s", tline.c_str());
        }
    }

    // ---- Protection ----
    if (snap.protection.any() && content_ok()) {
        row++;
        attron(COLOR_PAIR(C_RED) | A_BOLD | A_BLINK);
        mvprintw(row++, 1, "!! PROTECTION ACTIVE !!");
        attroff(COLOR_PAIR(C_RED) | A_BOLD | A_BLINK);
    }

    // ---- Log panel ----
    draw_log_panel(rows, cols);

    // ---- Footer ----
    attron(COLOR_PAIR(C_HEADER));
    mvhline(rows - 1, 0, ' ', cols);
    mvprintw(rows - 1, 1, " q=quit  ?=help  sw_ver=%s  http://localhost:%d/",
             snap.sw_version.c_str(), http_port_);
    attroff(COLOR_PAIR(C_HEADER));

    refresh();
}

// ---------------------------------------------------------------------------
// Device picker screen
// ---------------------------------------------------------------------------
void TUI::draw_picker(const std::vector<DiscoveredDevice>& devs, const std::string& ble_st) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();

    // Header
    attron(COLOR_PAIR(C_HEADER) | A_BOLD);
    mvhline(0, 0, ' ', cols);
    mvprintw(0, 1, " limonitor — BLE Device Picker  |  %s", ble_st.c_str());
    attroff(COLOR_PAIR(C_HEADER) | A_BOLD);

    mvprintw(2, 2, "Select your LiTime battery:");
    mvprintw(3, 2, "(arrow keys / j/k to move, Enter to connect, r to rescan, q to quit)");

    if (devs.empty()) {
        attron(COLOR_PAIR(C_YELLOW));
        mvprintw(5, 4, "Scanning... no devices found yet.");
        attroff(COLOR_PAIR(C_YELLOW));
    } else {
        int max_show = rows - 8 - LOG_PANEL_ROWS;
        if (max_show < 1) max_show = 1;

        // Sort: JBD-service devices first, then by RSSI descending
        std::vector<size_t> idx(devs.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
            if (devs[a].has_target_service != devs[b].has_target_service)
                return devs[a].has_target_service > devs[b].has_target_service;
            return devs[a].rssi > devs[b].rssi;
        });

        // Clamp selection
        if (picker_sel_ < 0) picker_sel_ = 0;
        if (picker_sel_ >= (int)devs.size()) picker_sel_ = (int)devs.size() - 1;

        int show = std::min((int)devs.size(), max_show);
        int row = 5;
        for (int i = 0; i < show; ++i) {
            const auto& d = devs[idx[i]];
            bool sel = (i == picker_sel_);
            std::string name = d.name.empty() ? "(unnamed)" : d.name;
            std::string svc  = d.has_target_service ? " [JBD/LiTime]" : "";
            char rssi_buf[12];
            std::snprintf(rssi_buf, sizeof(rssi_buf), "%d dBm", d.rssi);

            if (sel) {
                attron(COLOR_PAIR(C_HEADER) | A_BOLD);
                mvprintw(row, 2, " ▶ %-36s %8s%s", name.c_str(), rssi_buf, svc.c_str());
                attroff(COLOR_PAIR(C_HEADER) | A_BOLD);
            } else {
                int cc = d.has_target_service ? C_GREEN : C_NORMAL;
                attron(COLOR_PAIR(cc));
                mvprintw(row, 2, "   %-36s %8s%s", name.c_str(), rssi_buf, svc.c_str());
                attroff(COLOR_PAIR(cc));
            }
            // Show device id (UUID/MAC) dimmed below name
            attron(A_DIM);
            mvprintw(row, cols - 40, "%.38s", d.id.c_str());
            attroff(A_DIM);
            ++row;
        }
        if ((int)devs.size() > max_show) {
            mvprintw(row, 4, "... %zu more", devs.size() - max_show);
        }
    }

    draw_log_panel(rows, cols);

    attron(COLOR_PAIR(C_HEADER));
    mvhline(rows - 1, 0, ' ', cols);
    mvprintw(rows - 1, 1, " ↑↓/jk=select  Enter=connect  r=rescan  q=quit");
    attroff(COLOR_PAIR(C_HEADER));

    refresh();
}

bool TUI::poll_input(const std::vector<DiscoveredDevice>& devs) {
    int ch = getch();
    switch (ch) {
        case 'q': case 'Q': case 3:
            running_ = false;
            return true;
        case KEY_UP: case 'k':
            picker_sel_--;
            break;
        case KEY_DOWN: case 'j':
            picker_sel_++;
            break;
        case '?':
            show_help_ = !show_help_;
            break;
        case '\n': case '\r': case KEY_ENTER: {
            // Sort same way as draw_picker to map visual index to device
            if (devs.empty()) break;
            std::vector<size_t> idx(devs.size());
            for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
            std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
                if (devs[a].has_target_service != devs[b].has_target_service)
                    return devs[a].has_target_service > devs[b].has_target_service;
                return devs[a].rssi > devs[b].rssi;
            });
            int sel = picker_sel_;
            if (sel < 0) sel = 0;
            if (sel >= (int)idx.size()) sel = (int)idx.size() - 1;
            const auto& chosen = devs[idx[sel]];
            LOG_INFO("Picker: connecting to '%s' (%s)",
                chosen.name.empty() ? "(unnamed)" : chosen.name.c_str(),
                chosen.id.c_str());
            if (connect_cb_) connect_cb_(chosen.id);
            break;
        }
        default: break;
    }
    return false;
}

void TUI::draw_help() {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    static const char* lines[] = {
        "  limonitor — field reference                              ",
        "                                                           ",
        "  PACK                                                     ",
        "  Voltage   Total pack voltage (sum of all cells)         ",
        "  Current   Amps in/out. Negative = charging,             ",
        "            positive = discharging                        ",
        "  Power     Voltage × Current (negative while charging)   ",
        "  SoC       State of Charge — % of capacity remaining     ",
        "  Capacity  Remaining Ah / Nominal (rated) Ah             ",
        "  Est.Time  Time to empty (discharging) or full (charging)",
        "  Cycles    Full charge cycles counted by the BMS         ",
        "  MOS       MOSFET switches inside the BMS (shown only     ",
        "    CHG     when the BMS reports them). CHG controls the  ",
        "    DCHG    charge circuit; DCHG controls the load circuit",
        "                                                           ",
        "  CELLS                                                    ",
        "  C01..N    Individual cell voltages.                     ",
        "            LiFePO4 healthy range: ~3.0 V – 3.65 V        ",
        "  min/max   Lowest and highest cell in the pack           ",
        "  delta     Spread between min and max.                   ",
        "            <5 mV = excellent   10–50 mV = monitor        ",
        "            >50 mV = rebalance needed                     ",
        "  Colors    All green = balanced (delta ≤10 mV)           ",
        "            Yellow = lowest cell, Red = highest cell      ",
        "            (only shown when delta >10 mV)                ",
        "                                                           ",
        "  TEMPERATURES                                             ",
        "  T1        Cell/pack temperature sensor                  ",
        "  T2        BMS board temperature sensor                  ",
        "                                                           ",
        "  KEYS                                                     ",
        "  q         Quit                                           ",
        "  ?         Toggle this help overlay                      ",
        "  r         Rescan for BLE devices (picker screen)        ",
        "  ↑↓ / jk   Move selection in device picker              ",
        "  Enter      Connect to selected device                   ",
    };
    const int nlines = static_cast<int>(sizeof(lines) / sizeof(lines[0]));

    int box_h = nlines + 2;
    int box_w = 62;
    int y = std::max(0, (rows - box_h) / 2);
    int x = std::max(0, (cols - box_w) / 2);

    // Shadow
    attron(A_DIM);
    for (int i = 1; i < box_h + 1 && y + i < rows; ++i)
        mvhline(y + i, x + 2, ' ', box_w);
    attroff(A_DIM);

    // Box background
    attron(COLOR_PAIR(C_HEADER) | A_BOLD);
    mvhline(y, x, ' ', box_w);
    mvprintw(y, x + 2, " Help — press ? to close ");
    attroff(COLOR_PAIR(C_HEADER) | A_BOLD);

    for (int i = 0; i < nlines && y + 1 + i < rows; ++i) {
        // Dim section headers (lines that start with "  " then uppercase word)
        const char* l = lines[i];
        bool is_section = (l[2] != ' ' && l[2] >= 'A' && l[2] <= 'Z' &&
                           l[0] == ' ' && l[1] == ' ');
        if (is_section) attron(A_BOLD | COLOR_PAIR(C_CYAN));
        else            attron(COLOR_PAIR(C_NORMAL));
        mvprintw(y + 1 + i, x, "%-*s", box_w, l);
        if (is_section) attroff(A_BOLD | COLOR_PAIR(C_CYAN));
        else            attroff(COLOR_PAIR(C_NORMAL));
    }
    // Bottom border
    if (y + box_h < rows) {
        attron(COLOR_PAIR(C_HEADER));
        mvhline(y + box_h, x, ' ', box_w);
        attroff(COLOR_PAIR(C_HEADER));
    }
    refresh();
}

void TUI::run_ncurses() {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    if (has_colors()) init_colors();
    Logger::instance().set_tui_mode(true);

    while (running_) {
        auto devs = store_.discovered_devices();
        auto snap = store_.latest().value_or(BatterySnapshot{});
        std::string ble_st = store_.ble_state();

        // Show picker when scanning and no battery data yet
        bool show_picker = !snap.valid && !devs.empty();
        // Also show picker during scanning even if we just lost connection
        if (ble_st.find("scanning") != std::string::npos && !snap.valid)
            show_picker = true;

        if (poll_input(devs)) break;

        if (show_picker) {
            draw_picker(devs, ble_st);
        } else {
            auto hist = store_.history(60);
            draw(snap, ble_st, hist);
        }
        if (show_help_) draw_help();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    Logger::instance().set_tui_mode(false);
    endwin();
}

#endif // HAVE_NCURSES

void TUI::run() {
#ifdef HAVE_NCURSES
    run_ncurses();
#else
    run_plain();
#endif
}
