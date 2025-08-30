#include "http_server.hpp"
#include "logger.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

static std::string jstr(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        if (c == '"') r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else r += c;
    }
    r += "\"";
    return r;
}
static std::string jbool(bool b) { return b ? "true" : "false"; }
static std::string jdbl(double v, int prec = 3) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
    return buf;
}
static std::string iso8601(std::chrono::system_clock::time_point tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    char buf[32];
    struct tm tm_buf{};
    gmtime_r(&t, &tm_buf);
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

HttpServer::HttpServer(DataStore& store, const std::string& bind_addr, uint16_t port,
                       int poll_interval_s)
    : store_(store), bind_addr_(bind_addr), port_(port),
      poll_interval_s_(std::max(1, poll_interval_s)) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        LOG_ERROR("HTTP: socket() failed: %s", strerror(errno));
        return false;
    }
    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port_);
    if (bind_addr_ == "0.0.0.0" || bind_addr_.empty())
        addr.sin_addr.s_addr = INADDR_ANY;
    else
        inet_pton(AF_INET, bind_addr_.c_str(), &addr.sin_addr);

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("HTTP: bind(%s:%d) failed: %s", bind_addr_.c_str(), port_, strerror(errno));
        close(listen_fd_); listen_fd_ = -1;
        return false;
    }
    if (listen(listen_fd_, 8) < 0) {
        LOG_ERROR("HTTP: listen() failed: %s", strerror(errno));
        close(listen_fd_); listen_fd_ = -1;
        return false;
    }

    running_ = true;
    thread_ = std::thread(&HttpServer::serve_loop, this);
    LOG_INFO("HTTP: listening on %s:%d", bind_addr_.c_str(), port_);
    return true;
}

void HttpServer::stop() {
    running_ = false;
    if (listen_fd_ >= 0) { shutdown(listen_fd_, SHUT_RDWR); close(listen_fd_); listen_fd_ = -1; }
    if (thread_.joinable()) thread_.join();
}

void HttpServer::serve_loop() {
    while (running_) {
        struct sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client = accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client < 0) {
            if (running_) LOG_DEBUG("HTTP: accept error: %s", strerror(errno));
            continue;
        }
        struct timeval tv{5, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        handle(client);
        close(client);
    }
}

void HttpServer::handle(int fd) {
    char buf[4096] = {};
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    std::string req(buf, static_cast<size_t>(n));

    // Parse first line: METHOD PATH HTTP/...
    auto lf = req.find('\n');
    if (lf == std::string::npos) { send_response(fd, 400, "text/plain", "Bad Request"); return; }
    std::string line = req.substr(0, lf);
    if (line.back() == '\r') line.pop_back();

    std::istringstream ss(line);
    std::string method, path, proto;
    ss >> method >> path >> proto;

    if (method != "GET") { send_response(fd, 405, "text/plain", "Method Not Allowed"); return; }

    // Strip query string for routing
    std::string query;
    auto qpos = path.find('?');
    if (qpos != std::string::npos) { query = path.substr(qpos + 1); path = path.substr(0, qpos); }

    auto snap_opt = store_.latest();
    BatterySnapshot snap;
    if (snap_opt) snap = *snap_opt;
    std::string ble_st = store_.ble_state();

    auto pg_opt = store_.latest_pwrgate();
    PwrGateSnapshot pg;
    if (pg_opt) pg = *pg_opt;

    // Parse ?h= time-range param (hours). Default 1h, cap 6min–7days.
    double h = 1.0;
    {
        auto hi = query.find("h=");
        if (hi != std::string::npos) {
            try { h = std::stod(query.substr(hi + 2)); } catch (...) {}
        }
        h = std::max(0.1, std::min(h, 168.0));
    }
    if (path == "/" || path == "/index.html") {
        send_response(fd, 200, "text/html",
                      html_dashboard(snap, ble_st, pg,
                                     store_.purchase_date(), h, poll_interval_s_));
    } else if (path == "/api/charger") {
        send_response(fd, 200, "application/json", charger_json(pg));
    } else if (path == "/api/status") {
        send_response(fd, 200, "application/json", status_json(snap, ble_st));
    } else if (path == "/api/cells") {
        send_response(fd, 200, "application/json", cells_json(snap));
    } else if (path == "/api/history") {
        size_t count = 100;
        auto ni = query.find("n=");
        if (ni != std::string::npos) { try { count = std::stoul(query.substr(ni + 2)); } catch (...) {} }
        send_response(fd, 200, "application/json", history_json(store_.history(count)));
    } else if (path == "/api/charger/history") {
        size_t count = 300;
        auto ni = query.find("n=");
        if (ni != std::string::npos) { try { count = std::stoul(query.substr(ni + 2)); } catch (...) {} }
        send_response(fd, 200, "application/json",
                      charger_history_json(store_.pwrgate_history(count)));
    } else if (path == "/api/flow") {
        send_response(fd, 200, "application/json", flow_json(snap, pg));
    } else if (path == "/api/tx_events") {
        size_t count = 100;
        auto ni = query.find("n=");
        if (ni != std::string::npos) { try { count = std::stoul(query.substr(ni + 2)); } catch (...) {} }
        send_response(fd, 200, "application/json", tx_events_json(store_.tx_events(count)));
    } else if (path == "/api/analytics") {
        send_response(fd, 200, "application/json",
                      analytics_json(store_.analytics()));
    } else if (path == "/metrics") {
        send_response(fd, 200, "text/plain; version=0.0.4",
                      prometheus(snap) + prometheus_charger(pg));
    } else {
        send_response(fd, 404, "text/plain", "Not Found");
    }
}

std::string HttpServer::status_json(const BatterySnapshot& s, const std::string& ble_st) {
    std::string o = "{\n";
    o += "  \"timestamp\": " + jstr(s.valid ? iso8601(s.timestamp) : "") + ",\n";
    o += "  \"ble_state\": " + jstr(ble_st) + ",\n";
    o += "  \"device_name\": " + jstr(s.device_name) + ",\n";
    o += "  \"device_address\": " + jstr(s.ble_address) + ",\n";
    o += "  \"valid\": " + jbool(s.valid) + ",\n";
    o += "  \"voltage_v\": " + jdbl(s.total_voltage_v) + ",\n";
    o += "  \"current_a\": " + jdbl(s.current_a) + ",\n";
    o += "  \"soc_pct\": " + jdbl(s.soc_pct, 1) + ",\n";
    o += "  \"remaining_ah\": " + jdbl(s.remaining_ah) + ",\n";
    o += "  \"nominal_ah\": " + jdbl(s.nominal_ah) + ",\n";
    o += "  \"power_w\": " + jdbl(s.power_w) + ",\n";
    o += "  \"time_remaining_h\": " + jdbl(s.time_remaining_h) + ",\n";
    o += "  \"cycle_count\": " + std::to_string(s.cycle_count) + ",\n";
    o += "  \"charge_mosfet\": " + jbool(s.charge_mosfet) + ",\n";
    o += "  \"discharge_mosfet\": " + jbool(s.discharge_mosfet) + ",\n";
    o += "  \"cell_min_v\": " + jdbl(s.cell_min_v) + ",\n";
    o += "  \"cell_max_v\": " + jdbl(s.cell_max_v) + ",\n";
    o += "  \"cell_delta_v\": " + jdbl(s.cell_delta_v) + ",\n";
    // Cells
    o += "  \"cells\": [";
    for (size_t i = 0; i < s.cell_voltages_v.size(); ++i) {
        if (i) o += ", ";
        o += "{\"index\":" + std::to_string(i) + ",\"voltage_v\":" + jdbl(s.cell_voltages_v[i]) + "}";
    }
    o += "],\n";
    // Temperatures
    o += "  \"temperatures\": [";
    for (size_t i = 0; i < s.temperatures_c.size(); ++i) {
        if (i) o += ", ";
        o += "{\"index\":" + std::to_string(i) + ",\"temp_c\":" + jdbl(s.temperatures_c[i], 1) + "}";
    }
    o += "],\n";
    // Protection
    const auto& p = s.protection;
    o += "  \"protection\": {\n";
    o += "    \"cell_overvoltage\": "     + jbool(p.cell_overvoltage)     + ",\n";
    o += "    \"cell_undervoltage\": "    + jbool(p.cell_undervoltage)    + ",\n";
    o += "    \"pack_overvoltage\": "     + jbool(p.pack_overvoltage)     + ",\n";
    o += "    \"pack_undervoltage\": "    + jbool(p.pack_undervoltage)    + ",\n";
    o += "    \"charge_overtemp\": "      + jbool(p.charge_overtemp)      + ",\n";
    o += "    \"charge_undertemp\": "     + jbool(p.charge_undertemp)     + ",\n";
    o += "    \"discharge_overtemp\": "   + jbool(p.discharge_overtemp)   + ",\n";
    o += "    \"discharge_undertemp\": "  + jbool(p.discharge_undertemp)  + ",\n";
    o += "    \"charge_overcurrent\": "   + jbool(p.charge_overcurrent)   + ",\n";
    o += "    \"discharge_overcurrent\": "+ jbool(p.discharge_overcurrent)+ ",\n";
    o += "    \"short_circuit\": "        + jbool(p.short_circuit)        + ",\n";
    o += "    \"front_end_ic_error\": "   + jbool(p.front_end_ic_error)   + ",\n";
    o += "    \"mosfet_software_lock\": " + jbool(p.mosfet_software_lock) + "\n";
    o += "  }\n";
    o += "}\n";
    return o;
}

std::string HttpServer::cells_json(const BatterySnapshot& s) {
    std::string o = "{\"cells\": [";
    for (size_t i = 0; i < s.cell_voltages_v.size(); ++i) {
        if (i) o += ", ";
        o += jdbl(s.cell_voltages_v[i]);
    }
    o += "], \"min_v\": " + jdbl(s.cell_min_v) +
         ", \"max_v\": " + jdbl(s.cell_max_v) +
         ", \"delta_v\": " + jdbl(s.cell_delta_v) + "}\n";
    return o;
}

std::string HttpServer::history_json(const std::vector<BatterySnapshot>& snaps) {
    std::string o = "[";
    bool first = true;
    for (const auto& s : snaps) {
        if (!s.valid) continue;
        if (!first) o += ",";
        first = false;
        o += "\n  {\"ts\":" + jstr(iso8601(s.timestamp)) +
             ",\"v\":" + jdbl(s.total_voltage_v) +
             ",\"a\":" + jdbl(s.current_a) +
             ",\"soc\":" + jdbl(s.soc_pct, 1) +
             ",\"w\":" + jdbl(s.power_w) + "}";
    }
    o += "\n]\n";
    return o;
}

std::string HttpServer::prometheus(const BatterySnapshot& s) {
    if (!s.valid) return "# No data yet\n";
    std::string o;
    auto g = [&](const char* name, const char* help, double val, int prec = 3) {
        o += "# HELP "; o += name; o += " "; o += help; o += "\n";
        o += "# TYPE "; o += name; o += " gauge\n";
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.*f", prec, val);
        o += name; o += " "; o += buf; o += "\n";
    };
    g("battery_voltage_volts",      "Pack voltage",        s.total_voltage_v);
    g("battery_current_amps",       "Pack current (+discharge/-charge)", s.current_a);
    g("battery_soc_percent",        "State of charge",     s.soc_pct, 1);
    g("battery_remaining_ah",       "Remaining capacity Ah", s.remaining_ah);
    g("battery_nominal_ah",         "Nominal capacity Ah", s.nominal_ah);
    g("battery_power_watts",        "Power (+ discharge / - charge)", s.power_w);
    g("battery_time_remaining_hours","Estimated time remaining", s.time_remaining_h);
    g("battery_cycle_count",        "Charge cycles",       s.cycle_count, 0);
    g("battery_cell_min_volts",     "Minimum cell voltage", s.cell_min_v);
    g("battery_cell_max_volts",     "Maximum cell voltage", s.cell_max_v);
    g("battery_cell_delta_volts",   "Cell imbalance",       s.cell_delta_v);

    for (size_t i = 0; i < s.cell_voltages_v.size(); ++i) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "battery_cell_voltage_volts{cell=\"%zu\"} %.3f\n", i, s.cell_voltages_v[i]);
        o += buf;
    }
    for (size_t i = 0; i < s.temperatures_c.size(); ++i) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "battery_temperature_celsius{sensor=\"%zu\"} %.1f\n", i, s.temperatures_c[i]);
        o += buf;
    }
    return o;
}

std::string HttpServer::prometheus_charger(const PwrGateSnapshot& pg) {
    if (!pg.valid) return "";
    std::string o;
    auto g = [&](const char* name, const char* help, double val, int prec = 3) {
        o += "# HELP "; o += name; o += " "; o += help; o += "\n";
        o += "# TYPE "; o += name; o += " gauge\n";
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.*f", prec, val);
        o += name; o += " "; o += buf; o += "\n";
    };
    g("charger_ps_volts",       "Charger power-supply input voltage", pg.ps_v);
    g("charger_bat_volts",      "Battery voltage (charger-measured)",  pg.bat_v);
    g("charger_bat_amps",       "Charge current",                      pg.bat_a);
    g("charger_solar_volts",    "Solar panel voltage",                 pg.sol_v);
    g("charger_target_volts",   "Target absorption voltage",           pg.target_v);
    g("charger_target_amps",    "Max charge current",                  pg.target_a);
    g("charger_pwm",            "PWM duty cycle (0-1023)",             pg.pwm, 0);
    g("charger_elapsed_minutes","Elapsed charge time minutes",         pg.minutes, 0);
    return o;
}

std::string HttpServer::analytics_json(const AnalyticsSnapshot& a) {
    std::string o = "{\n";
    o += "  \"energy_charged_today_wh\": "    + jdbl(a.energy_charged_today_wh, 1)    + ",\n";
    o += "  \"energy_discharged_today_wh\": " + jdbl(a.energy_discharged_today_wh, 1) + ",\n";
    o += "  \"solar_energy_today_wh\": "      + jdbl(a.solar_energy_today_wh, 1)      + ",\n";
    o += "  \"net_energy_today_wh\": "        + jdbl(a.net_energy_today_wh, 1)        + ",\n";
    o += "  \"battery_age_years\": "          + jdbl(a.battery_age_years, 2)          + ",\n";
    o += "  \"battery_health_pct\": "         + jdbl(a.battery_health_pct, 1)         + ",\n";
    o += "  \"years_remaining_low\": "        + jdbl(a.years_remaining_low, 0)        + ",\n";
    o += "  \"years_remaining_high\": "       + jdbl(a.years_remaining_high, 0)       + ",\n";
    o += "  \"battery_replace_warn\": "       + jbool(a.battery_replace_warn)         + ",\n";
    o += "  \"charging_stage\": "             + jstr(a.charging_stage)               + ",\n";
    o += "  \"usable_capacity_ah\": "         + jdbl(a.usable_capacity_ah, 1)        + ",\n";
    o += "  \"rated_capacity_ah\": "          + jdbl(a.rated_capacity_ah, 1)         + ",\n";
    o += "  \"capacity_health_pct\": "        + jdbl(a.capacity_health_pct, 1)       + ",\n";
    o += "  \"cell_delta_mv\": "              + jdbl(a.cell_delta_mv, 1)             + ",\n";
    o += "  \"cell_balance_status\": "        + jstr(a.cell_balance_status)          + ",\n";
    o += "  \"temp1_c\": "                    + jdbl(a.temp1_c, 1)                   + ",\n";
    o += "  \"temp2_c\": "                    + jdbl(a.temp2_c, 1)                   + ",\n";
    o += "  \"temp_valid\": "                 + jbool(a.temp_valid)                  + ",\n";
    o += "  \"temp_status\": "                + jstr(a.temp_status)                  + ",\n";
    o += "  \"charger_efficiency\": "         + jdbl(a.charger_efficiency, 3)        + ",\n";
    o += "  \"efficiency_valid\": "           + jbool(a.efficiency_valid)            + ",\n";
    o += "  \"solar_voltage_v\": "            + jdbl(a.solar_voltage_v, 2)           + ",\n";
    o += "  \"solar_power_w\": "              + jdbl(a.solar_power_w, 1)             + ",\n";
    o += "  \"solar_active\": "               + jbool(a.solar_active)                + ",\n";
    o += "  \"depth_of_discharge_pct\": "     + jdbl(a.depth_of_discharge_pct, 1)   + ",\n";
    o += "  \"dod_status\": "                 + jstr(a.dod_status)                   + ",\n";
    o += "  \"avg_load_watts\": "             + jdbl(a.avg_load_watts, 1)            + ",\n";
    o += "  \"peak_load_watts\": "            + jdbl(a.peak_load_watts, 1)           + ",\n";
    o += "  \"idle_load_watts\": "            + jdbl(a.idle_load_watts, 1)           + "\n";
    o += "}\n";
    return o;
}

// SVG time-axis helper
static std::string svg_time_ticks(
        std::chrono::system_clock::time_point t_start,
        std::chrono::system_clock::time_point t_end,
        int pl, int cw, int chart_top, int chart_h, int label_y) {
    long span_s = std::chrono::duration_cast<std::chrono::seconds>(t_end - t_start).count();
    if (span_s < 30) return "";

    long tick_s;
    if      (span_s <=  10 * 60)   tick_s =      120;  // ≤10 min  → 2 min
    else if (span_s <=  30 * 60)   tick_s =      300;  // ≤30 min  → 5 min
    else if (span_s <=  90 * 60)   tick_s =      900;  // ≤90 min  → 15 min
    else if (span_s <=   6 * 3600) tick_s =     3600;  // ≤6 h     → 1 h
    else if (span_s <=  24 * 3600) tick_s = 4 * 3600;  // ≤24 h    → 4 h
    else                            tick_s =12 * 3600;  // >24 h    → 12 h

    time_t t0 = std::chrono::system_clock::to_time_t(t_start);
    time_t t1 = std::chrono::system_clock::to_time_t(t_end);
    time_t first_tick = ((t0 / tick_s) + 1) * tick_s;

    std::string o;
    char tbuf[24], sbuf[160];
    const char* fmt = (span_s > 20 * 3600) ? "%m/%d %H:%M" : "%H:%M";

    o += "<g stroke='#2c2c3c' stroke-width='1' stroke-dasharray='2,4'>\n";
    for (time_t tk = first_tick; tk < t1; tk += tick_s) {
        double frac = (double)(tk - t0) / span_s;
        if (frac < 0.01 || frac > 0.99) continue;
        double x = pl + frac * cw;
        std::snprintf(sbuf, sizeof(sbuf), "<line x1='%.1f' y1='%d' x2='%.1f' y2='%d'/>\n",
                      x, chart_top, x, chart_top + chart_h);
        o += sbuf;
    }
    o += "</g>\n";

    o += "<g fill='#50505e' font-size='9' font-family='monospace' text-anchor='middle'>\n";
    for (time_t tk = first_tick; tk < t1; tk += tick_s) {
        double frac = (double)(tk - t0) / span_s;
        if (frac < 0.01 || frac > 0.99) continue;
        double x = pl + frac * cw;
        struct tm tm_b{};
        localtime_r(&tk, &tm_b);
        std::strftime(tbuf, sizeof(tbuf), fmt, &tm_b);
        std::snprintf(sbuf, sizeof(sbuf), "<text x='%.1f' y='%d'>%s</text>\n", x, label_y, tbuf);
        o += sbuf;
    }
    o += "</g>\n";
    return o;
}

std::string HttpServer::svg_history_chart(const std::vector<BatterySnapshot>& hist) {
    if (hist.size() < 2)
        return "<p style='color:#555;font-family:monospace'>Collecting history...</p>";

    const int W = 800, H = 200;
    const int PL = 52, PR = 48, PT = 14, PB = 28;
    const int CW = W - PL - PR, CH = H - PT - PB;

    // Voltage range with margin
    double vlo = 1e9, vhi = -1e9;
    for (auto& s : hist) { if (s.total_voltage_v < vlo) vlo = s.total_voltage_v;
                            if (s.total_voltage_v > vhi) vhi = s.total_voltage_v; }
    double vrng = vhi - vlo; if (vrng < 0.1) vrng = 0.1;
    vlo -= vrng * 0.08; vhi += vrng * 0.08; vrng = vhi - vlo;

    int N = static_cast<int>(hist.size());
    auto xp = [&](int i) { return PL + static_cast<double>(i) / (N - 1) * CW; };
    auto yv = [&](double v) { return PT + CH - (v - vlo) / vrng * CH; };
    auto ys = [&](double s) { return PT + CH - (s / 100.0) * CH; };

    std::string o;
    char buf[256];

    o += "<svg viewBox='0 0 800 200' style='width:100%;height:200px;display:block;background:#111;border-radius:4px'>\n";

    // Horizontal grid + voltage labels
    o += "<g stroke='#2a2a2a' stroke-width='1'>\n";
    for (int i = 0; i <= 4; ++i) {
        double gy = PT + CH * i / 4.0;
        std::snprintf(buf, sizeof(buf), "<line x1='%d' y1='%.1f' x2='%d' y2='%.1f'/>\n",
                      PL, gy, W - PR, gy);
        o += buf;
    }
    o += "</g>\n";

    // Voltage axis labels (left, green)
    o += "<g fill='#4caf50' font-size='11' font-family='monospace' text-anchor='end'>\n";
    for (int i = 0; i <= 4; ++i) {
        double v = vhi - vrng * i / 4.0;
        double gy = PT + CH * i / 4.0;
        std::snprintf(buf, sizeof(buf), "<text x='%d' y='%.1f'>%.2fV</text>\n", PL - 3, gy + 4, v);
        o += buf;
    }
    o += "</g>\n";

    // SoC axis labels (right, blue)
    o += "<g fill='#2196f3' font-size='11' font-family='monospace' text-anchor='start'>\n";
    for (int i = 0; i <= 4; ++i) {
        double s = 100.0 - 100.0 * i / 4.0;
        double gy = PT + CH * i / 4.0;
        std::snprintf(buf, sizeof(buf), "<text x='%d' y='%.1f'>%.0f%%</text>\n", W - PR + 3, gy + 4, s);
        o += buf;
    }
    o += "</g>\n";

    // Voltage polyline
    o += "<polyline fill='none' stroke='#4caf50' stroke-width='2' points='";
    for (int i = 0; i < N; ++i) {
        std::snprintf(buf, sizeof(buf), "%.1f,%.1f ", xp(i), yv(hist[i].total_voltage_v));
        o += buf;
    }
    o += "'/>\n";

    // SoC polyline (dashed blue)
    o += "<polyline fill='none' stroke='#2196f3' stroke-width='1.5' stroke-dasharray='5,3' points='";
    for (int i = 0; i < N; ++i) {
        std::snprintf(buf, sizeof(buf), "%.1f,%.1f ", xp(i), ys(hist[i].soc_pct));
        o += buf;
    }
    o += "'/>\n";

    // Time axis: intermediate ticks + edge labels from actual timestamps
    o += svg_time_ticks(hist.front().timestamp, hist.back().timestamp,
                        PL, CW, PT, CH, H - 4);
    {
        char tlbuf[16];
        struct tm tm_b{};
        time_t t0 = std::chrono::system_clock::to_time_t(hist.front().timestamp);
        localtime_r(&t0, &tm_b);
        std::strftime(tlbuf, sizeof(tlbuf), "%H:%M", &tm_b);
        std::snprintf(buf, sizeof(buf),
            "<text x='%d' y='%d' fill='#50505e' font-size='9' font-family='monospace'>%s</text>\n",
            PL, H - 4, tlbuf);
        o += buf;
        o += "<text x='"; o += std::to_string(W - PR);
        o += "' y='"; o += std::to_string(H - 4);
        o += "' fill='#50505e' font-size='9' font-family='monospace' text-anchor='end'>now</text>\n";
    }

    // Legend
    o += "<g font-size='11' font-family='monospace'>"
         "<line x1='60' y1='8' x2='74' y2='8' stroke='#4caf50' stroke-width='2'/>"
         "<text x='78' y='12' fill='#4caf50'>Voltage</text>"
         "<line x1='140' y1='8' x2='154' y2='8' stroke='#2196f3' stroke-width='1.5' stroke-dasharray='5,3'/>"
         "<text x='158' y='12' fill='#2196f3'>SoC</text>"
         "</g>\n";

    o += "</svg>\n";
    return o;
}

std::string HttpServer::svg_charger_chart(const std::vector<PwrGateSnapshot>& hist) {
    if (hist.size() < 2)
        return "<p style='color:#555;font-family:monospace'>Collecting charger history...</p>";

    // Downsample to at most 90 points, averaging each group to smooth noise
    const size_t TARGET = 90;
    std::vector<PwrGateSnapshot> sampled;
    if (hist.size() > TARGET) {
        size_t grp = hist.size() / TARGET;
        for (size_t i = 0; i + grp <= hist.size(); i += grp) {
            PwrGateSnapshot avg = hist[i];
            double sv = 0, sa = 0, ssol = 0;
            for (size_t j = i; j < i + grp; ++j) { sv += hist[j].bat_v; sa += hist[j].bat_a; ssol += hist[j].sol_v; }
            avg.bat_v = sv / grp; avg.bat_a = sa / grp; avg.sol_v = ssol / grp;
            sampled.push_back(avg);
        }
        if (sampled.back().timestamp != hist.back().timestamp) sampled.push_back(hist.back());
    } else {
        sampled = hist;
    }
    const auto& display = sampled;

    const int W = 800, H = 180;
    const int PL = 52, PR = 48, PT = 14, PB = 28;
    const int CW = W - PL - PR, CH = H - PT - PB;

    double vlo = 1e9, vhi = -1e9, alo = 1e9, ahi = -1e9;
    bool has_solar = false;
    for (auto& p : display) {
        double vmax = std::max(p.bat_v, p.sol_v > 0.1 ? p.sol_v : p.bat_v);
        double vmin = p.bat_v;
        if (vmin < vlo) vlo = vmin; if (vmax > vhi) vhi = vmax;
        if (p.bat_a < alo) alo = p.bat_a; if (p.bat_a > ahi) ahi = p.bat_a;
        if (p.sol_v > 0.1) has_solar = true;
    }
    double vrng = vhi - vlo; if (vrng < 0.1) vrng = 0.1;
    double arng = ahi - alo; if (arng < 0.1) arng = 0.1;
    vlo -= vrng * 0.08; vhi += vrng * 0.08; vrng = vhi - vlo;
    alo -= arng * 0.08; ahi += arng * 0.08; arng = ahi - alo;

    int N = static_cast<int>(display.size());
    auto xp = [&](int i) { return PL + static_cast<double>(i) / (N - 1) * CW; };
    auto yv = [&](double v) { return PT + CH - (v - vlo) / vrng * CH; };
    auto ya = [&](double a) { return PT + CH - (a - alo) / arng * CH; };

    std::string o;
    char buf[256];

    o += "<svg viewBox='0 0 800 180' style='width:100%;height:180px;display:block;background:#111;border-radius:4px'>\n";

    o += "<g stroke='#2a2a2a' stroke-width='1'>\n";
    for (int i = 0; i <= 4; ++i) {
        double gy = PT + CH * i / 4.0;
        std::snprintf(buf, sizeof(buf), "<line x1='%d' y1='%.1f' x2='%d' y2='%.1f'/>\n", PL, gy, W-PR, gy);
        o += buf;
    }
    o += "</g>\n";

    // Voltage labels (left, green)
    o += "<g fill='#4caf50' font-size='11' font-family='monospace' text-anchor='end'>\n";
    for (int i = 0; i <= 4; ++i) {
        double v = vhi - vrng * i / 4.0;
        double gy = PT + CH * i / 4.0;
        std::snprintf(buf, sizeof(buf), "<text x='%d' y='%.1f'>%.2fV</text>\n", PL-3, gy+4, v);
        o += buf;
    }
    o += "</g>\n";

    // Current labels (right, orange)
    o += "<g fill='#ff9800' font-size='11' font-family='monospace' text-anchor='start'>\n";
    for (int i = 0; i <= 4; ++i) {
        double a = ahi - arng * i / 4.0;
        double gy = PT + CH * i / 4.0;
        std::snprintf(buf, sizeof(buf), "<text x='%d' y='%.1f'>%.1fA</text>\n", W-PR+3, gy+4, a);
        o += buf;
    }
    o += "</g>\n";

    // Bat voltage polyline (green)
    o += "<polyline fill='none' stroke='#4caf50' stroke-width='2' points='";
    for (int i = 0; i < N; ++i) {
        std::snprintf(buf, sizeof(buf), "%.1f,%.1f ", xp(i), yv(display[i].bat_v));
        o += buf;
    }
    o += "'/>\n";

    // Charge current polyline (orange, dashed)
    o += "<polyline fill='none' stroke='#ff9800' stroke-width='1.5' stroke-dasharray='5,3' points='";
    for (int i = 0; i < N; ++i) {
        std::snprintf(buf, sizeof(buf), "%.1f,%.1f ", xp(i), ya(display[i].bat_a));
        o += buf;
    }
    o += "'/>\n";

    // Time axis ticks + edge labels
    o += svg_time_ticks(display.front().timestamp, display.back().timestamp,
                        PL, CW, PT, CH, H - 4);
    {
        char tlbuf[16];
        struct tm tm_b{};
        time_t t0 = std::chrono::system_clock::to_time_t(display.front().timestamp);
        localtime_r(&t0, &tm_b);
        std::strftime(tlbuf, sizeof(tlbuf), "%H:%M", &tm_b);
        std::snprintf(buf, sizeof(buf),
            "<text x='%d' y='%d' fill='#50505e' font-size='9' font-family='monospace'>%s</text>\n",
            PL, H - 4, tlbuf);
        o += buf;
        o += "<text x='"; o += std::to_string(W-PR);
        o += "' y='"; o += std::to_string(H-4);
        o += "' fill='#50505e' font-size='9' font-family='monospace' text-anchor='end'>now</text>\n";
    }

    // Solar voltage polyline (cyan, dotted) — only if any solar data
    if (has_solar) {
        o += "<polyline fill='none' stroke='#00bcd4' stroke-width='1.5' stroke-dasharray='3,4' points='";
        for (int i = 0; i < N; ++i) {
            std::snprintf(buf, sizeof(buf), "%.1f,%.1f ", xp(i), yv(display[i].sol_v));
            o += buf;
        }
        o += "'/>\n";
    }

    o += "<g font-size='11' font-family='monospace'>"
         "<line x1='60' y1='8' x2='74' y2='8' stroke='#4caf50' stroke-width='2'/>"
         "<text x='78' y='12' fill='#4caf50'>Bat V</text>"
         "<line x1='130' y1='8' x2='144' y2='8' stroke='#ff9800' stroke-width='1.5' stroke-dasharray='5,3'/>"
         "<text x='148' y='12' fill='#ff9800'>Charge A</text>";
    if (has_solar)
        o += "<line x1='222' y1='8' x2='236' y2='8' stroke='#00bcd4' stroke-width='1.5' stroke-dasharray='3,4'/>"
             "<text x='240' y='12' fill='#00bcd4'>Solar V</text>";
    o += "</g>\n";

    o += "</svg>\n";
    return o;
}

std::string HttpServer::charger_history_json(const std::vector<PwrGateSnapshot>& snaps) {
    std::string o = "[";
    bool first = true;
    for (const auto& p : snaps) {
        if (!p.valid) continue;
        if (!first) o += ",";
        first = false;
        o += "\n  {\"ts\":" + jstr(iso8601(p.timestamp)) +
             ",\"bat_v\":" + jdbl(p.bat_v) +
             ",\"bat_a\":" + jdbl(p.bat_a) +
             ",\"sol_v\":" + jdbl(p.sol_v) + "}";
    }
    o += "\n]\n";
    return o;
}

std::string HttpServer::flow_json(const BatterySnapshot& bat, const PwrGateSnapshot& chg) {
    std::string o = "{\n";
    o += "  \"battery_voltage\": " + jdbl(bat.valid ? bat.total_voltage_v : 0) + ",\n";
    o += "  \"battery_current\": " + jdbl(bat.valid ? bat.current_a : 0) + ",\n";
    o += "  \"state_of_charge\": " + jdbl(bat.valid ? bat.soc_pct : 0, 1) + ",\n";
    o += "  \"solar_voltage\": " + jdbl(chg.valid ? chg.sol_v : 0) + ",\n";
    o += "  \"charger_voltage\": " + jdbl(chg.valid ? chg.ps_v : 0) + ",\n";
    o += "  \"charger_state\": " + jstr(chg.valid ? chg.state : "") + ",\n";
    double power_w = chg.valid ? (chg.bat_v * chg.bat_a) : (bat.valid ? bat.power_w : 0);
    o += "  \"power_watts\": " + jdbl(std::abs(power_w)) + "\n";
    o += "}\n";
    return o;
}

std::string HttpServer::tx_events_json(const std::vector<TxEvent>& events) {
    std::string o = "[";
    bool first = true;
    for (const auto& e : events) {
        if (!first) o += ",";
        first = false;
        o += "\n  {\"start\":" + jdbl(e.start_time, 0) +
             ",\"duration\":" + jdbl(e.duration) +
             ",\"peak_current\":" + jdbl(e.peak_current) +
             ",\"peak_power\":" + jdbl(e.peak_power) + "}";
    }
    o += "\n]\n";
    return o;
}

std::string HttpServer::charger_json(const PwrGateSnapshot& pg) {
    if (!pg.valid) return "{\"valid\":false}\n";
    std::string o = "{\n";
    o += "  \"valid\": true,\n";
    o += "  \"timestamp\": " + jstr(iso8601(pg.timestamp)) + ",\n";
    o += "  \"state\": "     + jstr(pg.state)    + ",\n";
    o += "  \"ps_v\": "      + jdbl(pg.ps_v)     + ",\n";
    o += "  \"bat_v\": "     + jdbl(pg.bat_v)    + ",\n";
    o += "  \"bat_a\": "     + jdbl(pg.bat_a)    + ",\n";
    o += "  \"sol_v\": "     + jdbl(pg.sol_v)    + ",\n";
    o += "  \"target_v\": "  + jdbl(pg.target_v) + ",\n";
    o += "  \"target_a\": "  + jdbl(pg.target_a) + ",\n";
    o += "  \"stop_a\": "    + jdbl(pg.stop_a)   + ",\n";
    o += "  \"minutes\": "   + std::to_string(pg.minutes) + ",\n";
    o += "  \"pwm\": "       + std::to_string(pg.pwm)     + ",\n";
    o += "  \"temp\": "      + std::to_string(pg.temp)    + ",\n";
    o += "  \"pss\": "       + std::to_string(pg.pss)     + "\n";
    o += "}\n";
    return o;
}

std::string HttpServer::html_dashboard(const BatterySnapshot& s, const std::string& ble_st,
                                       const PwrGateSnapshot& pg,
                                       const std::string& purchase_date,
                                       double hours,
                                       int poll_interval_s) {
    char buf[512];

    const char* cur_cls = s.current_a >  0.01 ? "sv warn" :
                          s.current_a < -0.01 ? "sv ok"   : "sv dim";
    const char* cur_dir = s.current_a >  0.01 ? "discharging" :
                          s.current_a < -0.01 ? "charging"    : "idle";
    const char* rem_dir = s.current_a < -0.01 ? "to full" : "to empty";

    std::string dot_cls = "dot-off";
    if      (ble_st.find("ready")   != std::string::npos) dot_cls = "dot-ok";
    else if (ble_st.find("connect") != std::string::npos ||
             ble_st.find("scan")    != std::string::npos) dot_cls = "dot-warn";
    else if (ble_st.find("error")   != std::string::npos) dot_cls = "dot-err";

    std::string o;
    o.reserve(65536);

    o += R"HTML(<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0d0d11" media="(prefers-color-scheme:dark)">
<meta name="theme-color" content="#f2f3f7" media="(prefers-color-scheme:light)">
<title>limonitor</title>
<style>
/* ── Dark theme (default) ── */
:root{
  --bg:#0d0d11;--card:#16161c;--border:#2e2e3a;
  --text:#e0e0ea;--muted:#9090a8;--sub:#686880;
  --green:#4ade80;--orange:#fb923c;--blue:#60a5fa;--cyan:#22d3ee;--red:#f87171;
  --soc-track:#22222c;--cell-bg:#0f0f16;--alert-bg:#1c0a0a;
  --chart-bg:#0f0f14;--chart-grid:#252530;--chart-tick:#a0a0b8;
  --cv:#4caf50;--ca:#ff9800;--csoc:#4080ff;--csol:#00bcd4
}
/* ── Light theme ── */
html.light{
  --bg:#f2f3f7;--card:#ffffff;--border:#d4d4e0;
  --text:#1a1a2c;--muted:#5a5a72;
  --green:#16a34a;--orange:#c2410c;--blue:#1d4ed8;--cyan:#0891b2;--red:#dc2626;
  --soc-track:#dcdce8;--cell-bg:#f8f8fc;--alert-bg:#fff0f0;
  --chart-bg:#f5f5fa;--chart-grid:#dcdce8;--chart-tick:#4a4a64;
  --cv:#16a34a;--ca:#c2410c;--csoc:#1d4ed8;--csol:#0891b2
}
/* ── Base ── */
*{box-sizing:border-box;margin:0;padding:0}
html{transition:background-color .2s;-webkit-tap-highlight-color:transparent}
body{font-family:'SF Mono',Menlo,Consolas,monospace;background:var(--bg);color:var(--text);font-size:13px;
  transition:background-color .2s,color .15s}
a{color:var(--green);text-decoration:none}a:hover{text-decoration:underline}
.wrap{max-width:1100px;margin:0 auto;padding:1.4rem;
  padding-top:max(1.4rem,env(safe-area-inset-top));
  padding-bottom:max(2.5rem,env(safe-area-inset-bottom))}
/* ── Header ── */
header{display:flex;align-items:center;justify-content:space-between;margin-bottom:1.2rem;flex-wrap:wrap;gap:.6rem}
@media(max-width:640px){header{flex-direction:column;align-items:flex-start;gap:.8rem}}
h1{color:var(--green);font-size:1.7rem;letter-spacing:.2em;font-weight:700}
.hstat{font-size:.72rem;color:var(--muted);display:flex;align-items:center;gap:.8rem;flex-wrap:wrap}
.dot{display:inline-block;width:7px;height:7px;border-radius:50%;vertical-align:middle;margin-right:4px}
.dot-ok{background:var(--green);box-shadow:0 0 7px var(--green)}
.dot-warn{background:var(--orange)}.dot-err{background:var(--red)}.dot-off{background:#444}
/* ── Stat cards ── */
.stats{display:grid;grid-template-columns:repeat(4,1fr);gap:.7rem;margin-bottom:.7rem}
@media(max-width:560px){.stats{grid-template-columns:repeat(2,1fr)}.stat{padding:1rem}}
@media(max-width:640px){body{font-size:14px}.wrap{padding:1rem;padding-top:max(1rem,env(safe-area-inset-top))}.card{padding:1rem}
.stat-lbl{font-size:.7rem}.stat-sub{font-size:.78rem}.sv{font-size:2.2rem}}}
.stat{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:.9rem 1rem;
  transition:background-color .2s,border-color .2s}
.stat-lbl{font-size:.6rem;text-transform:uppercase;letter-spacing:.14em;color:var(--muted)}
.sv{font-size:2rem;font-weight:700;line-height:1.05;margin-top:.2rem}
.sv .u{font-size:.8rem;font-weight:400;color:var(--muted);margin-left:.08em}
.stat-sub{font-size:.68rem;color:var(--muted);margin-top:.28rem;min-height:.9em}
.soc-track{height:3px;background:var(--soc-track);border-radius:2px;margin-top:.45rem}
.soc-fill{height:3px;border-radius:2px;background:var(--green);transition:width .6s ease}
/* ── Cards ── */
.card{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:1.1rem;margin-bottom:.7rem;
  transition:background-color .2s,border-color .2s}
.card-title{font-size:.6rem;text-transform:uppercase;letter-spacing:.14em;color:var(--muted);margin-bottom:.75rem}
/* ── Layout ── */
.col2{display:grid;grid-template-columns:1fr 1fr;gap:.7rem}
@media(max-width:640px){.col2{grid-template-columns:1fr}}
/* ── Data table ── */
.dt{width:100%;border-collapse:collapse;font-size:.82rem}
.dt td{padding:.35rem 0;vertical-align:top;border-bottom:1px solid var(--border)}
.dt tr:last-child td{border-bottom:none}
.dt td:first-child{color:var(--muted);width:44%;padding-right:.5rem}
.dt td:last-child{word-break:break-word}
/* ── Cell grid ── */
.cells{display:grid;grid-template-columns:repeat(auto-fill,minmax(88px,1fr));gap:.4rem;margin-top:.4rem}
.cell{background:var(--cell-bg);border:1px solid var(--border);border-radius:5px;padding:.45rem .55rem;
  transition:background-color .2s,border-color .2s}
@media(max-width:640px){.cells{grid-template-columns:repeat(auto-fill,minmax(72px,1fr))}}
.cell-n{font-size:.58rem;color:var(--muted);text-transform:uppercase;letter-spacing:.1em}
.cell-v{font-size:.92rem;font-weight:700;margin-top:.08rem}
.cell-track{height:2px;background:var(--soc-track);border-radius:1px;margin-top:.38rem}
.cell-fill{height:2px;border-radius:1px}
/* ── Colours ── */
.ok{color:var(--green)}.warn{color:var(--orange)}.err{color:var(--red)}.dim{color:var(--muted)}
/* ── Alert ── */
.alert{background:var(--alert-bg);border:1px solid var(--red);border-radius:8px;
  padding:.7rem 1rem;margin-bottom:.7rem;font-size:.8rem;color:var(--red)}
/* ── Help details ── */
details.card summary{cursor:pointer;color:var(--muted);list-style:none;user-select:none}
details.card summary::-webkit-details-marker{display:none}
details.card summary::before{content:'▸  ';font-size:.8em}
details[open].card summary::before{content:'▾  '}
.ht{width:100%;border-collapse:collapse;font-size:.78rem;margin-top:.6rem}
.ht td{padding:.2rem .35rem}.ht td:first-child{color:var(--muted);width:30%}
.ht .sec td{color:var(--green);padding-top:.75rem;font-weight:600}
/* ── Footer ── */
.footer{font-size:.68rem;color:var(--muted);margin-top:1rem;opacity:.7}
.footer a{color:var(--muted)}.footer a:hover{color:var(--green)}
/* ── Analytics grid ── */
.sec-lbl{font-size:.58rem;text-transform:uppercase;letter-spacing:.14em;color:var(--muted);margin:.6rem 0 .35rem}
.acards{display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:.55rem;margin-bottom:.7rem}
@media(max-width:480px){.acards{grid-template-columns:1fr 1fr}}
@media(max-width:320px){.acards{grid-template-columns:1fr}}
.acard{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:.75rem .9rem;
  transition:background-color .2s,border-color .2s}
.acard .card-title{margin-bottom:.5rem}
.acard .dt td{padding:.2rem 0;font-size:.78rem}
.acard .dt td:first-child{width:48%}
/* ── Buttons ── */
.btn{background:none;border:1px solid var(--border);border-radius:4px;color:var(--muted);
  padding:.2rem .6rem;font-size:.72rem;font-family:inherit;cursor:pointer;
  transition:border-color .15s,color .15s,background .15s}
.btn:hover{border-color:var(--muted);color:var(--text)}
.btn:focus-visible,.trng-btn:focus-visible{outline:2px solid var(--green);outline-offset:2px}
/* ── Time range ── */
.trng{display:flex;gap:.2rem;align-items:center}
.trng-lbl{font-size:.62rem;color:var(--muted);margin-right:.2rem;text-transform:uppercase;letter-spacing:.1em}
.trng-btn{background:none;border:1px solid var(--border);border-radius:4px;color:var(--muted);
  padding:.22rem .65rem;font-size:.72rem;font-family:inherit;cursor:pointer;line-height:1.5;
  transition:border-color .15s,color .15s,background .15s}
@media(max-width:640px){.trng-btn,.btn{min-height:44px;padding:.4rem .8rem;font-size:.8rem}}
.trng-btn:hover{border-color:var(--muted);color:var(--text)}
.trng-btn.active{background:rgba(74,222,128,.15);border-color:var(--green);color:var(--green);font-weight:600}
html.light .trng-btn.active{background:rgba(22,163,74,.1);border-color:var(--green);color:var(--green)}
.chart-svg{width:100%;display:block;background:var(--chart-bg);border-radius:4px}
/* ── Energy Flow Diagram ── */
.flow-wrap{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:1.4rem;margin-bottom:.7rem;box-shadow:0 1px 3px rgba(0,0,0,.06)}
.flow-diagram{width:100%;max-width:720px;margin:0 auto;display:block}
.flow-diagram svg{width:100%;height:auto;display:block}
.flow-node{transition:fill .2s,opacity .2s;stroke:rgba(0,0,0,.08);stroke-width:1}
.flow-node-solar{fill:#f59e0b}
.flow-node-grid{fill:#2563eb}
.flow-node-charger{fill:#4b5563}
.flow-node-battery{fill:#16a34a}
.flow-node-load{fill:#252530}
html.light .flow-node-load{fill:#e2e8f0}
.flow-node-inactive{opacity:.45}
.flow-arrow-chg{stroke:#16a34a}
.flow-arrow-dchg{stroke:#ea580c}
.flow-arrow-idle{stroke:#94a3b8;opacity:.5}
.flow-arrow{stroke-width:3;fill:none;stroke-linecap:round;stroke-linejoin:round;stroke-dasharray:6 6;transition:stroke .2s,opacity .2s}
.flow-arrow-solar{stroke-width:2}
.flow-arrow-label{font-size:9px;font-weight:600;font-family:system-ui,sans-serif}
@keyframes flow{from{stroke-dashoffset:24}to{stroke-dashoffset:0}}
.flow-arrow-anim{animation:flow linear infinite}
@media(max-width:640px){.flow-wrap{padding:1rem}.flow-diagram{max-width:100%}}
)HTML";
    o += ".main{display:flex;flex-direction:column;gap:.7rem}"
         "#bat-chart{height:200px}#chg-chart{height:180px}"
         "@media(max-width:640px){.main{display:flex;flex-direction:column}"
         ".stats{order:-2;margin-bottom:.5rem}.card-bat{order:-1}.card-chg{order:0}.col2{order:1}"
         "#bat-chart{height:280px}#chg-chart{height:260px}.card.card-bat,.card.card-chg{padding-bottom:1.5rem}"
         ".chart-svg{preserve-aspect-ratio:none}}";
    o += R"HTML(
</style></head><body><div class="wrap">
)HTML";

    o += "<header><h1>limonitor</h1><div class=\"hstat\">";
    o += "<span><span class=\"dot " + dot_cls + "\"></span>" + ble_st + "</span>";
    if (s.valid && !s.device_name.empty())
        o += "<span>" + s.device_name + "</span>";
    o += "<span id=\"ts-disp\" style=\"color:var(--muted)\">—</span>";
    // Time range selector (JS-driven, no page reload)
    {
        static const double ph[]   = {0.5, 1.0, 4.0, 24.0};
        static const char*  plbl[] = {"30m", "1h", "4h", "24h"};
        o += "<div class=\"trng\"><span class=\"trng-lbl\">Range</span>";
        for (int pi = 0; pi < 4; ++pi) {
            bool active = std::fabs(hours - ph[pi]) < 0.01;
            char tbuf[128];
            std::snprintf(tbuf, sizeof(tbuf),
                "<button class=\"trng-btn%s\" data-h=\"%.4g\" onclick=\"switchRange(%.4g)\">%s</button>",
                active ? " active" : "", ph[pi], ph[pi], plbl[pi]);
            o += tbuf;
        }
        o += "</div>";
    }
    o += "<button class=\"btn\" onclick=\"loadCharts()\" title=\"Refresh charts\">&#8635;</button>";
    o += "<button id=\"thm-btn\" class=\"btn\" onclick=\"toggleTheme()\" title=\"Toggle theme\">&#9788;</button>";
    o += "</div></header>\n";

    o += "<main class=\"main\"><div class=\"stats\">\n";

    // Voltage
    snprintf(buf, sizeof(buf),
        "<div class=\"stat\"><div class=\"stat-lbl\">Voltage</div>"
        "<div class=\"sv ok\"><span id=\"sv\">%.2f</span><span class=\"u\">V</span></div>"
        "<div class=\"stat-sub\" id=\"sv-sub\">%.3f&nbsp;min &middot; %.3f&nbsp;max</div></div>\n",
        s.valid ? s.total_voltage_v : 0.0,
        s.valid ? s.cell_min_v : 0.0,
        s.valid ? s.cell_max_v : 0.0);
    o += buf;

    // Current
    snprintf(buf, sizeof(buf),
        "<div class=\"stat\"><div class=\"stat-lbl\">Current</div>"
        "<div class=\"%s\" id=\"sa-wrap\"><span id=\"sa\">%.2f</span><span class=\"u\">A</span></div>"
        "<div class=\"stat-sub\" id=\"sa-sub\">%s</div></div>\n",
        cur_cls, std::abs(s.valid ? s.current_a : 0.0), cur_dir);
    o += buf;

    // SoC
    int soc_bar = static_cast<int>(std::max(0.0, std::min(100.0, s.soc_pct)));
    snprintf(buf, sizeof(buf),
        "<div class=\"stat\"><div class=\"stat-lbl\">State of Charge</div>"
        "<div class=\"sv ok\"><span id=\"ssoc\">%.1f</span><span class=\"u\">%%</span></div>"
        "<div class=\"soc-track\"><div class=\"soc-fill\" id=\"soc-bar\" style=\"width:%d%%\"></div></div>"
        "<div class=\"stat-sub\" id=\"ssoc-sub\">%.2f&nbsp;/&nbsp;%.2f&nbsp;Ah</div></div>\n",
        s.soc_pct, soc_bar, s.remaining_ah, s.nominal_ah);
    o += buf;

    // Power
    snprintf(buf, sizeof(buf),
        "<div class=\"stat\"><div class=\"stat-lbl\">Power</div>"
        "<div class=\"sv\"><span id=\"spw\">%.1f</span><span class=\"u\">W</span></div>"
        "<div class=\"stat-sub\" id=\"spw-sub\">%.1f&nbsp;h&nbsp;%s</div></div>\n",
        s.valid ? std::abs(s.power_w) : 0.0,
        s.time_remaining_h, rem_dir);
    o += buf;

    o += "</div>\n"; // .stats

    o += "<div class=\"flow-wrap\"><div class=\"card-title\">Energy Flow</div>"
         "<div id=\"flow-diagram\" class=\"flow-diagram\">"
         "<svg id=\"flow-svg\" viewBox=\"0 0 720 200\" xmlns=\"http://www.w3.org/2000/svg\">"
         "<text x=\"50%\" y=\"50%\" fill=\"var(--muted)\" font-size=\"12\" font-family=\"monospace\""
         " text-anchor=\"middle\" dominant-baseline=\"middle\">Loading…</text>"
         "</svg></div></div>\n";

    o += "<div class=\"card card-bat\"><div class=\"card-title\">Battery History</div>"
         "<svg id=\"bat-chart\" class=\"chart-svg\" viewBox=\"0 0 800 200\">"
         "<text x=\"50%\" y=\"50%\" fill=\"#444\" font-size=\"12\" font-family=\"monospace\""
         " text-anchor=\"middle\" dominant-baseline=\"middle\">Loading\xe2\x80\xa6</text>"
         "</svg></div>\n";

    o += "<div class=\"col2\">\n";

    // Battery details
    o += "<div class=\"card\"><div class=\"card-title\">Battery</div><table class=\"dt\">\n";
    snprintf(buf, sizeof(buf), "%.2f / %.2f Ah", s.remaining_ah, s.nominal_ah);
    o += "<tr><td>Capacity</td><td id=\"bat-cap\">" + std::string(buf) + "</td></tr>\n";
    snprintf(buf, sizeof(buf), "%.1f h %s", s.time_remaining_h, rem_dir);
    o += "<tr><td>Est. remaining</td><td id=\"bat-rem\">" + std::string(buf) + "</td></tr>\n";
    {
        const char* dc = s.cell_delta_v > 0.05 ? "warn" : s.cell_delta_v > 0.02 ? "" : "ok";
        snprintf(buf, sizeof(buf), "<span class=\"%s\">%.0f mV</span>", dc, s.cell_delta_v * 1000.0);
        o += "<tr><td>Cell delta</td><td id=\"bat-delta\">" + std::string(buf) + "</td></tr>\n";
    }
    if (!s.temperatures_c.empty()) {
        std::string temps;
        for (size_t i = 0; i < s.temperatures_c.size(); ++i) {
            if (i) temps += " &middot; ";
            snprintf(buf, sizeof(buf), "T%zu:&nbsp;%.1f&deg;C", i + 1, s.temperatures_c[i]);
            temps += buf;
        }
        o += "<tr><td>Temperature</td><td id=\"bat-temp\">" + temps + "</td></tr>\n";
    }
    o += "<tr><td>Cycles</td><td id=\"bat-cyc\">" + std::to_string(s.cycle_count) + "</td></tr>\n";
    if (s.charge_mosfet || s.discharge_mosfet) {
        const char* cm = s.charge_mosfet    ? "<span class=\"ok\">ON</span>" : "<span class=\"warn\">OFF</span>";
        const char* dm = s.discharge_mosfet ? "<span class=\"ok\">ON</span>" : "<span class=\"warn\">OFF</span>";
        o += "<tr><td>FETs chg/dchg</td><td id=\"bat-fets\">";
        o += cm; o += " / "; o += dm; o += "</td></tr>\n";
    }
    if (!purchase_date.empty())
        o += "<tr><td>Purchased</td><td>" + purchase_date + "</td></tr>\n";
    o += "</table></div>\n";

    // Charger details (always render table so JS can populate it)
    o += "<div class=\"card\"><div class=\"card-title\">Charger &mdash; EpicPowerGate 2</div>";
    o += "<table class=\"dt\">\n";
    auto crow = [&](const char* id, const char* k, const std::string& v) {
        o += "<tr><td>" + std::string(k) + "</td><td id=\"" + id + "\">" + v + "</td></tr>\n";
    };
    if (pg.valid) {
        const char* sc = (pg.state == "Charging" || pg.state == "Float") ? "ok" : "warn";
        snprintf(buf, sizeof(buf), "<span class=\"%s\">%s</span>", sc, pg.state.c_str());
        crow("chg-state", "State", buf);
        snprintf(buf, sizeof(buf), "%.2f V", pg.ps_v);  crow("chg-ps", "Power supply", buf);
        snprintf(buf, sizeof(buf), "%.2f V", pg.sol_v); crow("chg-sol", "Solar", buf);
        snprintf(buf, sizeof(buf), "%.2f V &middot; %.2f A", pg.bat_v, pg.bat_a);
        crow("chg-bat", "Battery", buf);
        snprintf(buf, sizeof(buf), "%.2f V  max %.2f A  stop %.2f A",
                 pg.target_v, pg.target_a, pg.stop_a);
        crow("chg-tgt", "Target", buf);
        snprintf(buf, sizeof(buf), "%d / 1023 (%d%%)", pg.pwm, pg.pwm * 100 / 1023);
        crow("chg-pwm", "PWM", buf);
        snprintf(buf, sizeof(buf), "%d min", pg.minutes); crow("chg-min", "Elapsed", buf);
        snprintf(buf, sizeof(buf), "%d (raw)", pg.temp);  crow("chg-tmp", "Temp", buf);
    } else {
        crow("chg-state", "State",        "—");
        crow("chg-ps",    "Power supply", "—");
        crow("chg-sol",   "Solar",        "—");
        crow("chg-bat",   "Battery",      "—");
        crow("chg-tgt",   "Target",       "—");
        crow("chg-pwm",   "PWM",          "—");
        crow("chg-min",   "Elapsed",      "—");
        crow("chg-tmp",   "Temp",         "—");
    }
    o += "</table></div>\n";

    o += "<div class=\"card\"><div class=\"card-title\">Radio Activity (24h)</div>"
         "<table class=\"dt\">\n"
         "<tr><td>TX events</td><td id=\"tx-count\">—</td></tr>"
         "<tr><td>Total TX time</td><td id=\"tx-duration\">—</td></tr>"
         "<tr><td>Duty cycle</td><td id=\"tx-duty\">—</td></tr>"
         "<tr><td>Energy used</td><td id=\"tx-energy\">—</td></tr>"
         "<tr><td>Peak TX power</td><td id=\"tx-peak-pwr\">—</td></tr>"
         "<tr><td>Avg TX current</td><td id=\"tx-avg-cur\">—</td></tr>"
         "</table></div>\n";

    o += "</div>\n"; // .col2

    o += "<div class=\"card card-chg\"><div class=\"card-title\">Charger History</div>"
         "<svg id=\"chg-chart\" class=\"chart-svg\" viewBox=\"0 0 800 180\">"
         "<text x=\"50%\" y=\"50%\" fill=\"#444\" font-size=\"12\" font-family=\"monospace\""
         " text-anchor=\"middle\" dominant-baseline=\"middle\">Loading\xe2\x80\xa6</text>"
         "</svg></div>\n";

    o += "<div class=\"sec-lbl\">Analytics</div>\n";
    o += "<div class=\"acards\">\n";

    o += "<div class=\"acard\"><div class=\"card-title\">Energy Today</div><table class=\"dt\">\n"
         "<tr><td>Charged</td><td><span id=\"an-e-chg\">—</span> Wh</td></tr>\n"
         "<tr><td>Consumed</td><td><span id=\"an-e-dis\">—</span> Wh</td></tr>\n"
         "<tr><td>Solar</td><td><span id=\"an-e-sol\">—</span> Wh</td></tr>\n"
         "<tr><td>Net change</td><td><span id=\"an-e-net\">—</span> Wh</td></tr>\n"
         "</table></div>\n";

    o += "<div class=\"acard\"><div class=\"card-title\">Battery Replacement</div><table class=\"dt\">\n"
         "<tr><td>Age</td><td><span id=\"an-bat-age\">—</span></td></tr>\n"
         "<tr><td>Est. health</td><td><span id=\"an-bat-health\">—</span></td></tr>\n"
         "<tr><td>Est. remaining</td><td><span id=\"an-bat-repl\">—</span></td></tr>\n"
         "<tr><td id=\"an-bat-warn-row\" style=\"display:none\" colspan=\"2\">"
         "<span class=\"warn\">Replacement recommended soon</span></td></tr>\n"
         "</table></div>\n";

    o += "<div class=\"acard\"><div class=\"card-title\">Battery Health</div><table class=\"dt\">\n"
         "<tr><td>Health</td><td><span id=\"an-cap-health\">—</span></td></tr>\n"
         "<tr><td>Est. capacity</td><td><span id=\"an-cap-usable\">—</span></td></tr>\n"
         "<tr><td>Rated capacity</td><td><span id=\"an-cap-rated\">—</span></td></tr>\n"
         "</table></div>\n";

    o += "<div class=\"acard\"><div class=\"card-title\">Charging Stage</div><table class=\"dt\">\n"
         "<tr><td>Stage</td><td><span id=\"an-chg-stage\">—</span></td></tr>\n"
         "</table></div>\n";

    o += "<div class=\"acard\"><div class=\"card-title\">Cell Balance</div><table class=\"dt\">\n"
         "<tr><td>Delta</td><td><span id=\"an-cell-delta\">—</span> mV</td></tr>\n"
         "<tr><td>Status</td><td><span id=\"an-cell-bal\">—</span></td></tr>\n"
         "</table></div>\n";

    o += "<div class=\"acard\"><div class=\"card-title\">Battery Temperature</div><table class=\"dt\">\n"
         "<tr><td>T1</td><td><span id=\"an-t1\">—</span></td></tr>\n"
         "<tr><td>T2</td><td><span id=\"an-t2\">—</span></td></tr>\n"
         "<tr><td>Status</td><td><span id=\"an-temp-status\">—</span></td></tr>\n"
         "</table></div>\n";

    o += "<div class=\"acard\"><div class=\"card-title\">Charger Efficiency</div><table class=\"dt\">\n"
         "<tr><td>Efficiency</td><td><span id=\"an-eff\">—</span></td></tr>\n"
         "</table></div>\n";

    o += "<div class=\"acard\"><div class=\"card-title\">Solar Input</div><table class=\"dt\">\n"
         "<tr><td>Voltage</td><td><span id=\"an-sol-v\">—</span> V</td></tr>\n"
         "<tr><td>Status</td><td><span id=\"an-sol-status\">—</span></td></tr>\n"
         "<tr><td>Power</td><td><span id=\"an-sol-pwr\">—</span></td></tr>\n"
         "<tr><td>Energy today</td><td><span id=\"an-sol-energy\">—</span></td></tr>\n"
         "</table></div>\n";

    o += "<div class=\"acard\"><div class=\"card-title\">Depth of Discharge</div><table class=\"dt\">\n"
         "<tr><td>DoD today</td><td><span id=\"an-dod\">—</span> %</td></tr>\n"
         "<tr><td>Status</td><td><span id=\"an-dod-status\">—</span></td></tr>\n"
         "</table></div>\n";

    o += "<div class=\"acard\"><div class=\"card-title\">System Load Profile</div><table class=\"dt\">\n"
         "<tr><td>Average</td><td><span id=\"an-avg-load\">—</span> W</td></tr>\n"
         "<tr><td>Peak</td><td><span id=\"an-peak-load\">—</span> W</td></tr>\n"
         "<tr><td>Idle</td><td><span id=\"an-idle-load\">—</span> W</td></tr>\n"
         "</table></div>\n";

    o += "</div>\n"; // .acards

    if (!s.cell_voltages_v.empty()) {
        o += "<div class=\"card\"><div class=\"card-title\">Cell Voltages";
        snprintf(buf, sizeof(buf), " <span style=\"color:#222;font-weight:400\">&Delta;&nbsp;%.0f mV</span>",
                 s.cell_delta_v * 1000.0);
        o += buf;
        o += "</div><div class=\"cells\">\n";
        for (size_t i = 0; i < s.cell_voltages_v.size(); ++i) {
            double v = s.cell_voltages_v[i];
            const char* vc = v < 3.0 ? "err" : v < 3.2 ? "warn" : "ok";
            int fill = static_cast<int>((v - 2.8) / (3.65 - 2.8) * 100.0);
            if (fill < 0) fill = 0; if (fill > 100) fill = 100;
            const char* bc = v < 3.0 ? "var(--red)" : v < 3.2 ? "var(--orange)" : "var(--green)";
            snprintf(buf, sizeof(buf),
                "<div class=\"cell\"><div class=\"cell-n\">C%zu</div>"
                "<div class=\"cell-v %s\">%.3f</div>"
                "<div class=\"cell-track\"><div class=\"cell-fill\""
                " style=\"width:%d%%;background:%s\"></div></div></div>\n",
                i + 1, vc, v, fill, bc);
            o += buf;
        }
        o += "</div></div>\n";
    }

    if (s.protection.any()) {
        o += "<div class=\"alert\"><b>Protection Active:</b>";
        if (s.protection.cell_overvoltage)      o += " Cell&nbsp;OV";
        if (s.protection.cell_undervoltage)     o += " Cell&nbsp;UV";
        if (s.protection.pack_overvoltage)      o += " Pack&nbsp;OV";
        if (s.protection.pack_undervoltage)     o += " Pack&nbsp;UV";
        if (s.protection.charge_overtemp)       o += " Chg&nbsp;OT";
        if (s.protection.discharge_overtemp)    o += " Dchg&nbsp;OT";
        if (s.protection.charge_overcurrent)    o += " Chg&nbsp;OC";
        if (s.protection.discharge_overcurrent) o += " Dchg&nbsp;OC";
        if (s.protection.short_circuit)         o += " Short&nbsp;Circuit";
        o += "</div>\n";
    }

    o += R"(<details class="card"><summary>Help</summary>
<table class="ht">
<tr class="sec"><td colspan="2">Battery (BLE)</td></tr>
<tr><td>Voltage</td><td>Total pack voltage (sum of all cells)</td></tr>
<tr><td>Current</td><td>Positive = discharging &nbsp;·&nbsp; Negative = charging</td></tr>
<tr><td>SoC</td><td>State of Charge &mdash; % of rated capacity remaining</td></tr>
<tr><td>Power</td><td>V &times; A. Negative while charging</td></tr>
<tr><td>Capacity</td><td>Remaining Ah / Nominal (rated) Ah</td></tr>
<tr><td>Est. remaining</td><td>Time to full (charging) or empty (discharging) at current rate</td></tr>
<tr><td>Cell delta</td><td>Spread weakest to strongest cell. &lt;5 mV excellent, &gt;50 mV needs balancing</td></tr>
<tr class="sec"><td colspan="2">Charger (EpicPowerGate 2)</td></tr>
<tr><td>Power supply</td><td>Input voltage measured by charger (grid / alternator)</td></tr>
<tr><td>Solar</td><td>Solar panel voltage (0 V when no solar input)</td></tr>
<tr><td>Battery</td><td>Battery voltage and charge current as measured by charger</td></tr>
<tr><td>Target</td><td>Bulk target V / max charge A / stop (termination) A</td></tr>
<tr><td>PWM</td><td>Charge duty cycle &mdash; 1023 = 100% = full power</td></tr>
<tr><td>Elapsed</td><td>Minutes since current charge session started</td></tr>
<tr><td>Temp</td><td>Charger PCB temperature (raw ADC &mdash; not in &deg;C)</td></tr>
</table></details>
)";

    o += "</main><div class=\"footer\">API: "
         "<a href=\"/api/status\">/api/status</a> &nbsp;"
         "<a href=\"/api/analytics\">/api/analytics</a> &nbsp;"
         "<a href=\"/api/flow\">/api/flow</a> &nbsp;"
         "<a href=\"/api/tx_events\">/api/tx_events</a> &nbsp;"
         "<a href=\"/api/cells\">/api/cells</a> &nbsp;"
         "<a href=\"/api/history\">/api/history</a> &nbsp;"
         "<a href=\"/api/charger\">/api/charger</a> &nbsp;"
         "<a href=\"/metrics\">/metrics</a> (Prometheus)"
         "</div>\n";

    {
        char jshdr[96];
        std::snprintf(jshdr, sizeof(jshdr),
            "<script>\nvar currentH=%.4g,pollIvl=%d,lastUpd=Date.now()\n",
            hours, std::max(1, poll_interval_s));
        o += jshdr;
    }

    o += R"(
function $(id){return document.getElementById(id)}
function fmt(v,d){return(v==null||isNaN(+v))? '\u2014':(+v).toFixed(d)}

function initTheme(){
  var t=localStorage.getItem('lm-theme')
  if(!t) t=window.matchMedia('(prefers-color-scheme:light)').matches?'light':'dark'
  applyTheme(t,false)
}
function applyTheme(t,redraw){
  document.documentElement.classList.toggle('light',t==='light')
  var b=$('thm-btn'); if(b) b.textContent=t==='light'?'\u263d':'\u2600'
  localStorage.setItem('lm-theme',t)
  if(redraw) loadCharts()
}
function toggleTheme(){
  applyTheme(document.documentElement.classList.contains('light')?'dark':'light',true)
}
initTheme()

function upBat(){fetch('/api/status').then(function(r){return r.json()}).then(function(d){
  if(!d.valid)return
  $('sv').textContent=fmt(d.voltage_v,2)
  $('sv-sub').textContent=fmt(d.cell_min_v,3)+' min \xb7 '+fmt(d.cell_max_v,3)+' max'
  var a=d.current_a,aa=Math.abs(a)
  var adir=a>0.01?'discharging':a<-0.01?'charging':'idle'
  $('sa').textContent=fmt(aa,2)
  $('sa-wrap').className='sv '+(a>0.01?'warn':a<-0.01?'ok':'dim')
  $('sa-sub').textContent=adir
  $('ssoc').textContent=fmt(d.soc_pct,1)
  $('soc-bar').style.width=Math.max(0,Math.min(100,d.soc_pct))+'%'
  $('ssoc-sub').textContent=fmt(d.remaining_ah,2)+'\xa0/\xa0'+fmt(d.nominal_ah,2)+'\xa0Ah'
  $('spw').textContent=fmt(Math.abs(d.power_w),1)
  var rd=a<-0.01?'to full':'to empty'
  $('spw-sub').textContent=fmt(d.time_remaining_h,1)+'\xa0h\xa0'+rd
  $('bat-cap').textContent=fmt(d.remaining_ah,2)+' / '+fmt(d.nominal_ah,2)+' Ah'
  $('bat-rem').textContent=fmt(d.time_remaining_h,1)+' h '+rd
  if(d.cells&&d.cells.length){
    var vs=d.cells.map(function(c){return c.voltage_v})
    var mn=Math.min.apply(null,vs),mx=Math.max.apply(null,vs)
    var delta=(mx-mn)*1000
    var dc=delta>50?'warn':delta>20?'':'ok'
    if($('bat-delta'))$('bat-delta').innerHTML='<span class="'+dc+'">'+delta.toFixed(0)+' mV</span>'
  }
  if(d.temperatures&&d.temperatures.length&&$('bat-temp'))
    $('bat-temp').innerHTML=d.temperatures.map(function(t,i){
      return 'T'+(i+1)+':\xa0'+t.temp_c.toFixed(1)+'\xb0C'}).join(' \xb7 ')
  if($('bat-cyc'))$('bat-cyc').textContent=d.cycle_count
  if($('bat-fets'))$('bat-fets').innerHTML=
    (d.charge_mosfet?'<span class="ok">ON</span>':'<span class="warn">OFF</span>')+
    ' / '+(d.discharge_mosfet?'<span class="ok">ON</span>':'<span class="warn">OFF</span>')
  lastUpd=Date.now()
}).catch(function(){})}

function upChg(){fetch('/api/charger').then(function(r){return r.json()}).then(function(d){
  if(!d.valid)return
  var sc=d.state==='Charging'||d.state==='Float'?'ok':'warn'
  if($('chg-state'))$('chg-state').innerHTML='<span class="'+sc+'">'+d.state+'</span>'
  if($('chg-ps'))$('chg-ps').textContent=fmt(d.ps_v,2)+' V'
  if($('chg-sol'))$('chg-sol').textContent=fmt(d.sol_v,2)+' V'
  if($('chg-bat'))$('chg-bat').textContent=fmt(d.bat_v,2)+' V \xb7 '+fmt(d.bat_a,2)+' A'
  if($('chg-tgt'))$('chg-tgt').textContent=
    fmt(d.target_v,2)+' V  max '+fmt(d.target_a,2)+' A  stop '+fmt(d.stop_a,2)+' A'
  if($('chg-pwm'))$('chg-pwm').textContent=d.pwm+' / 1023 ('+Math.round(d.pwm*100/1023)+'%)'
  if($('chg-min'))$('chg-min').textContent=d.minutes+' min'
  if($('chg-tmp'))$('chg-tmp').textContent=d.temp+' (raw)'
}).catch(function(){})}

setInterval(upBat,5000); setInterval(upChg,5000)
upBat(); upChg()

function upFlow(){fetch('/api/flow').then(function(r){return r.json()}).then(function(d){
  lastFlowData=d;renderFlowDiagram(d)
}).catch(function(){})}
var lastFlowData=null
setInterval(upFlow,3000); upFlow()
window.addEventListener('resize',function(){if(lastFlowData)renderFlowDiagram(lastFlowData)})

function upTx(){fetch('/api/tx_events').then(function(r){return r.json()}).then(function(evs){
  var cutoff=Date.now()/1000-86400
  var in24=evs.filter(function(e){return e.start>=cutoff})
  var n=in24.length
  var totalS=in24.reduce(function(a,e){return a+e.duration},0)
  var peakPwr=0,sumCur=0,energyWh=0
  in24.forEach(function(e){
    if(e.peak_power>peakPwr)peakPwr=e.peak_power
    sumCur+=e.peak_current
    energyWh+=e.peak_power*e.duration/3600
  })
  var avgCur=n>0?sumCur/n:0
  var dutyCycle=totalS/864
  $('tx-count').textContent=n>0?n:'—'
  $('tx-duration').textContent=totalS>0?(totalS>=60?Math.floor(totalS/60)+'m '+Math.round(totalS%60)+'s':Math.round(totalS)+'s'):'—'
  $('tx-duty').textContent=dutyCycle>0?dutyCycle.toFixed(2)+' %':'—'
  $('tx-energy').textContent=energyWh>0?energyWh.toFixed(2)+' Wh':'—'
  $('tx-peak-pwr').textContent=peakPwr>0?fmt(peakPwr,0)+' W':'—'
  $('tx-avg-cur').textContent=avgCur>0?fmt(avgCur,1)+' A':'—'
}).catch(function(){})}
setInterval(upTx,5000); upTx()

function upAnalytics(){fetch('/api/analytics').then(function(r){return r.json()}).then(function(d){
  function sv(id,v){var e=$(id);if(e)e.textContent=v}
  function sc(id,cls,v){var e=$(id);if(e){e.textContent=v;e.className=cls}}
  sv('an-e-chg',   fmt(d.energy_charged_today_wh,0))
  sv('an-e-dis',   fmt(d.energy_discharged_today_wh,0))
  sv('an-e-sol',   fmt(d.solar_energy_today_wh,0))
  var net=d.net_energy_today_wh||0
  sc('an-e-net',net>=0?'ok':'warn',(net>=0?'+':'')+net.toFixed(0))
  sv('an-bat-age', d.battery_age_years>=0 ? d.battery_age_years.toFixed(1)+' yr' : '—')
  var hp=d.battery_health_pct
  sc('an-bat-health', hp<0?'dim':hp<80?'err':hp<90?'warn':'ok',
     hp>=0 ? hp.toFixed(1)+'%' : '—')
  var rl=d.years_remaining_low,rh=d.years_remaining_high
  sv('an-bat-repl', rl>=0&&rh>=0 ? rl.toFixed(0)+'\u2013'+rh.toFixed(0)+' yr remaining' : '—')
  var wr=$('an-bat-warn-row')
  if(wr)wr.style.display=d.battery_replace_warn?'':'none'
  var stg=d.charging_stage||'—'
  var stgcls=stg==='Bulk'?'ok':stg==='Absorption'?'ok':stg==='Float'?'dim':'dim'
  sc('an-chg-stage',stgcls,stg)
  var ch=d.capacity_health_pct
  sc('an-cap-health', ch<0?'dim':ch<80?'err':ch<90?'warn':'ok',
     ch>=0 ? ch.toFixed(1)+'%' : '—')
  sv('an-cap-usable', d.usable_capacity_ah>0 ? d.usable_capacity_ah.toFixed(1)+' Ah' : '—')
  sv('an-cap-rated',  d.rated_capacity_ah>0  ? d.rated_capacity_ah.toFixed(1)+' Ah'  : '—')
  var dm=d.cell_delta_mv||0
  sc('an-cell-delta', dm>=50?'err':dm>=25?'warn':'ok', dm.toFixed(1))
  var bs=d.cell_balance_status||'—'
  sc('an-cell-bal', bs==='Excellent'||bs==='Good'?'ok':bs==='Warning'?'warn':bs==='Imbalance'?'err':'dim', bs)
  if(d.temp_valid){
    sv('an-t1', d.temp1_c.toFixed(1)+'\xb0C')
    sv('an-t2', d.temp2_c.toFixed(1)+'\xb0C')
    var ts=d.temp_status||'—'
    sc('an-temp-status', ts==='Normal'?'ok':ts==='Warm'?'warn':'err', ts)
  }
  if(d.efficiency_valid&&d.charger_efficiency>=0){
    var eff=d.charger_efficiency*100
    sc('an-eff', eff<80?'warn':'ok', eff.toFixed(1)+'%')
  }
  sv('an-sol-v', fmt(d.solar_voltage_v,2))
  sc('an-sol-status', d.solar_active?'ok':'dim', d.solar_active?'Active':'Inactive')
  sv('an-sol-pwr',    d.solar_active&&d.solar_power_w>0 ? fmt(d.solar_power_w,1)+' W' : '—')
  sv('an-sol-energy', d.solar_energy_today_wh>0 ? fmt(d.solar_energy_today_wh,0)+' Wh' : '—')
  var dod=d.depth_of_discharge_pct||0
  sc('an-dod', dod>=70?'err':dod>=30?'warn':'ok', dod.toFixed(1))
  var ds=d.dod_status||'—'
  sc('an-dod-status', ds==='Low stress'?'ok':ds==='Normal'?'':'err', ds)
  sv('an-avg-load',  fmt(d.avg_load_watts,1))
  sv('an-peak-load', fmt(d.peak_load_watts,1))
  sv('an-idle-load', fmt(d.idle_load_watts,1))
}).catch(function(){})}
setInterval(upAnalytics,5000); upAnalytics()

function renderFlowDiagram(d){
  var el=document.getElementById('flow-svg');if(!el)return
  var bv=d.battery_voltage||0,ba=d.battery_current||0,sol=d.solar_voltage||0,chv=d.charger_voltage||0
  var pw=d.power_watts||0
  var batPwr=bv*ba
  var sysLoad=bv*Math.abs(ba)
  var charging=ba<-0.1,discharging=ba>0.1,idle=Math.abs(ba)<=0.1
  var solarActive=sol>1
  var animSpeed=Math.max(0.5,Math.min(2,Math.abs(batPwr)/50))
  var dur=(1/animSpeed).toFixed(2)+'s'
  var chgCls='flow-arrow-chg',dchgCls='flow-arrow-dchg',idleCls='flow-arrow-idle'
  var chgPwr=chv>0?(pw||0):0
  var mobile=window.innerWidth<640
  var vb=mobile?'0 0 200 420':'0 0 720 200'
  var nodes,arrows
  var loadAnim=discharging||(sysLoad>1)
  var chgNodeLbl=charging?'Charging':'Charger'
  var chgNodeVal=fmt(chgPwr||0,0)+' W'
  var isDark=!document.documentElement.classList.contains('light')
  var pillBg=isDark?'rgba(0,0,0,0.55)':'rgba(255,255,255,0.82)'
  var chgColor=isDark?'#4ade80':'#16a34a'
  var dchgColor=isDark?'#fb923c':'#ea580c'
  var wFill='#fff',wMuted='rgba(255,255,255,0.7)'
  var flowRef='flowShadow',textEnd='<'+'/text>',fillTxt='var(--text)',fillMuted='var(--muted)'
  if(mobile){
    nodes=[
      {id:'solar',x:100,y:52,w:80,h:44,cls:'flow-node flow-node-solar',lbl:'Solar',val:fmt(sol,1)+' V',inactive:!solarActive,tf:wFill,lf:wMuted},
      {id:'grid',x:100,y:128,w:80,h:44,cls:'flow-node flow-node-grid',lbl:'Grid',val:fmt(chv,2)+' V',inactive:false,tf:wFill,lf:wMuted},
      {id:'charger',x:100,y:204,w:80,h:44,cls:'flow-node flow-node-charger',lbl:chgNodeLbl,val:chgNodeVal,inactive:false,tf:wFill,lf:wMuted},
      {id:'battery',x:100,y:280,w:92,h:58,cls:'flow-node flow-node-battery',lbl:'Battery',val:fmt(bv,2)+' V',val2:(ba>=0?'+':'')+fmt(ba,2)+' A',val2Cls:idle?'':charging?'flow-cur-chg':'flow-cur-dchg',inactive:false,tf:wFill,lf:wMuted},
      {id:'load',x:100,y:368,w:80,h:44,cls:'flow-node flow-node-load',lbl:'Load',val:fmt(sysLoad||0,1)+' W',inactive:false,tf:fillTxt,lf:fillMuted}
    ]
    arrows=[
      {path:'M 100 74 L 100 102',cls:(solarActive?chgCls:idleCls)+' flow-arrow-solar',anim:charging&&solarActive,pwr:solarActive&&charging?fmt(chgPwr,0)+' W':'',mx:118,my:88},
      {path:'M 100 150 L 100 182',cls:chv>1?chgCls:idleCls,anim:charging&&chv>1,pwr:chv>1&&charging?fmt(chgPwr,0)+' W':'',mx:118,my:166},
      {path:'M 100 226 L 100 258',cls:chgCls,anim:charging,pwr:charging?fmt(Math.abs(batPwr||0),0)+' W':'',mx:118,my:242},
      {path:'M 100 302 L 100 346',cls:loadAnim?dchgCls:idleCls,anim:loadAnim,pwr:(sysLoad||0)>0?fmt(sysLoad||0,0)+' W':'',mx:118,my:324}
    ]
  }else{
    nodes=[
      {id:'solar',x:324,y:55,w:76,h:40,cls:'flow-node flow-node-solar',lbl:'Solar',val:fmt(sol,1)+' V',inactive:!solarActive,tf:wFill,lf:wMuted},
      {id:'grid',x:180,y:140,w:76,h:44,cls:'flow-node flow-node-grid',lbl:'Grid',val:fmt(chv,2)+' V',inactive:false,tf:wFill,lf:wMuted},
      {id:'charger',x:324,y:140,w:88,h:44,cls:'flow-node flow-node-charger',lbl:chgNodeLbl,val:chgNodeVal,inactive:false,tf:wFill,lf:wMuted},
      {id:'battery',x:468,y:140,w:92,h:58,cls:'flow-node flow-node-battery',lbl:'Battery',val:fmt(bv,2)+' V',val2:(ba>=0?'+':'')+fmt(ba,2)+' A',val2Cls:idle?'':charging?'flow-cur-chg':'flow-cur-dchg',inactive:false,tf:wFill,lf:wMuted},
      {id:'load',x:612,y:140,w:76,h:44,cls:'flow-node flow-node-load',lbl:'Load',val:fmt(sysLoad||0,1)+' W',inactive:false,tf:fillTxt,lf:fillMuted}
    ]
    arrows=[
      {path:'M 324 75 L 324 118',cls:(solarActive?chgCls:idleCls)+' flow-arrow-solar',anim:charging&&solarActive,pwr:solarActive&&charging?fmt(chgPwr,0)+' W':'',mx:336,my:96},
      {path:'M 218 140 L 280 140',cls:chv>1?chgCls:idleCls,anim:charging&&chv>1,pwr:chv>1&&charging?fmt(chgPwr,0)+' W':'',mx:249,my:133},
      {path:'M 368 140 L 422 140',cls:chgCls,anim:charging,pwr:charging?fmt(Math.abs(batPwr||0),0)+' W':'',mx:395,my:133},
      {path:'M 514 140 L 574 140',cls:loadAnim?dchgCls:idleCls,anim:loadAnim,pwr:sysLoad>0?fmt(sysLoad||0,0)+' W':'',mx:544,my:133}
    ]
  }
  var defs='<defs><filter id="flowShadow"><feDropShadow dx="0" dy="1" stdDeviation="2" flood-opacity=".12"/></filter></defs>'
  var s=defs+'<rect width="'+(mobile?200:720)+'" height="'+(mobile?420:200)+'" fill="transparent"/>'
  arrows.forEach(function(a){
    var ac=a.anim?' flow-arrow-anim" style="animation-duration:'+dur+'"':''
    s+='<path d="'+a.path+'" class="flow-arrow '+a.cls+ac+'"/>'
    if(a.pwr){
      var lc=a.cls.indexOf(chgCls)>=0?chgColor:a.cls.indexOf(dchgCls)>=0?dchgColor:'#94a3b8'
      var tw=a.pwr.length*5.5+10
      s+='<rect x="'+(a.mx-tw/2).toFixed(1)+'" y="'+(a.my-2)+'" width="'+tw.toFixed(1)+'" height="13" rx="4" fill="'+pillBg+'"/>'
      s+='<text x="'+a.mx+'" y="'+(a.my+9)+'" text-anchor="middle" font-size="9" font-weight="600" font-family="system-ui,sans-serif" fill="'+lc+'">'+a.pwr+textEnd
    }
  })
  nodes.forEach(function(n){
    var rx=12,cls=n.cls+(n.inactive?' flow-node-inactive':'')
    s+='<rect x="'+(n.x-n.w/2)+'" y="'+(n.y-n.h/2)+'" width="'+n.w+'" height="'+n.h+'" rx="'+rx+'" class="'+cls+'" filter=\'url(#'+flowRef+')\'/>'
    s+='<text x="'+n.x+'" y="'+(n.y-6)+'" text-anchor="middle" font-size="8" font-family="system-ui,sans-serif" font-weight="500" fill="'+(n.lf||fillMuted)+'">'+n.lbl+textEnd
    s+='<text x="'+n.x+'" y="'+(n.y+10)+'" text-anchor="middle" font-size="11" font-family="system-ui,sans-serif" font-weight="600" fill="'+(n.tf||fillTxt)+'">'+n.val+textEnd
    if(n.val2){
      var curFill=n.val2Cls==='flow-cur-chg'?'var(--green)':n.val2Cls==='flow-cur-dchg'?'var(--orange)':fillMuted
      s+='<text x="'+n.x+'" y="'+(n.y+24)+'" text-anchor="middle" font-size="9" font-family="system-ui,sans-serif" fill="'+curFill+'">'+n.val2+textEnd
    }
  })
  el.outerHTML='<svg id="flow-svg" viewBox="'+vb+'" xmlns="http://www.w3.org/2000/svg">'+s+'</svg>'
}
setInterval(function(){
  var s=Math.round((Date.now()-lastUpd)/1000)
  var e=$('ts-disp'); if(!e)return
  e.textContent=s<3?'live':s+'s ago'
  e.style.color=s>30?'var(--orange)':'var(--muted)'
},1000)

function switchRange(h){
  currentH=h
  history.replaceState(null,'','/?h='+h)
  document.querySelectorAll('.trng-btn').forEach(function(b){
    b.classList.toggle('active',Math.abs(parseFloat(b.dataset.h)-h)<0.01)
  })
  loadCharts()
}

function loadCharts(){
  var cutoff=Date.now()-currentH*3600000
  var batN=Math.round(currentH*3600/pollIvl*1.15)+50
  var chgN=Math.round(currentH*7200)+100
  Promise.all([
    fetch('/api/history?n='+batN).then(function(r){return r.json()}),
    fetch('/api/tx_events?n=200').then(function(r){return r.json()})
  ]).then(function(res){
    var hist=res[0].filter(function(p){return new Date(p.ts).getTime()>=cutoff})
    var txEvs=res[1].filter(function(e){return e.start*1000>=cutoff})
    renderBatChart(hist,txEvs)
  }).catch(function(){})
  fetch('/api/charger/history?n='+chgN)
    .then(function(r){return r.json()})
    .then(function(d){renderChgChart(d.filter(function(p){return new Date(p.ts).getTime()>=cutoff}))})
    .catch(function(){})
}

function isMobile(){return window.innerWidth<640}
function chartDims(el,hBat,hChg){
  if(!isMobile())return null
  var w=el?el.clientWidth:0
  if(!w)w=Math.max(340,window.innerWidth-32)
  return{w:Math.min(500,w),hBat:hBat||280,hChg:hChg||260}
}

function timeTicks(t0,t1,tspan,pl,cw,chartTop,chartH,labelY,fsTick){
  if(tspan<1000) return ''
  var fs=fsTick||10
  var ss=tspan/1000
  var ts=ss<=600?120:ss<=1800?300:ss<=5400?900:ss<=21600?3600:ss<=86400?14400:43200
  var tms=ts*1000,firstTk=Math.ceil(t0/tms)*tms
  var multi=new Date(t0).toDateString()!==new Date(t1).toDateString()
  function pad(n){return n.toString().padStart(2,'0')}
  function fmtT(t,prevDay){
    var d=new Date(t),h=pad(d.getHours()),m=pad(d.getMinutes())
    var ds=(d.getMonth()+1)+'/'+d.getDate()
    if(multi&&h==='00'&&m==='00') return ds
    if(multi&&prevDay!==d.toDateString()) return ds+' '+h+':'+m
    return h+':'+m
  }
  function fmtStart(t,short){
    var d=new Date(t)
    if(short) return multi ? (d.getMonth()+1)+'/'+d.getDate() : pad(d.getHours())+':'+pad(d.getMinutes())
    return multi ? (d.getMonth()+1)+'/'+d.getDate()+' '+pad(d.getHours())+':'+pad(d.getMinutes()) : pad(d.getHours())+':'+pad(d.getMinutes())
  }
  var minGap=Math.max(36,fs*2.8)
  var firstTkX=firstTk<t1?pl+(firstTk-t0)/tspan*cw:pl+cw
  var firstTkClose=firstTkX<pl+minGap*2
  var startLbl=fmtStart(t0,firstTkClose)
  var startW=Math.max(minGap,startLbl.length*fs*0.7)
  var s="<g stroke='var(--chart-grid)' stroke-width='1' stroke-dasharray='2,4'>"
  for(var tk=firstTk;tk<t1;tk+=tms){
    var f=(tk-t0)/tspan; if(f<0.01||f>0.99) continue
    s+="<line x1='"+(pl+f*cw).toFixed(1)+"' y1='"+chartTop+"' x2='"+(pl+f*cw).toFixed(1)+"' y2='"+(chartTop+chartH)+"'/>"
  }
  s+="</g><g font-size='"+fs+"' font-family='monospace' style='fill:var(--chart-tick)'>"
  s+="<text x='"+pl+"' y='"+labelY+"' text-anchor='start'>"+startLbl+"</text>"
  var lastX=pl+startW,prevDay=new Date(t0).toDateString()
  for(var tk=firstTk;tk<t1;tk+=tms){
    var f=(tk-t0)/tspan; if(f<0.01||f>0.99) continue
    var x=pl+f*cw
    if(x<lastX||x>pl+cw-minGap) continue
    var d=new Date(tk)
    var lbl=fmtT(tk,prevDay)
    var gap=minGap
    if(lbl.length>6) gap=Math.max(gap,fs*lbl.length*0.6)
    lastX=x+gap
    prevDay=d.toDateString()
    s+="<text x='"+x.toFixed(1)+"' y='"+labelY+"' text-anchor='middle'>"+lbl+"</text>"
  }
  s+="<text x='"+(pl+cw)+"' y='"+labelY+"' text-anchor='end'>now</text>"
  s+="</g>"
  return s
}

function sma(arr,w){return arr.map(function(_,i){var a=0,c=0;for(var j=Math.max(0,i-w+1);j<=i;j++){a+=arr[j];c++}return a/c})}

function renderBatChart(data,txEvs){
  var el=$('bat-chart'); if(!el) return
  txEvs=txEvs||[]
  if(!data||data.length<2){
    el.innerHTML="<text x='50%' y='50%' font-size='14' font-family='monospace' text-anchor='middle' dominant-baseline='middle' style='fill:var(--muted)'>Collecting data\u2026</text>"
    return
  }
  var md=chartDims(el,280,260)
  var W,H,PL,PR,PT,PB,fs,fsTick,sw,sw2,legY
  if(md){
    W=md.w;H=md.hBat;PL=78;PR=72;PT=32;PB=42
    fs=16;fsTick=14;sw=4;sw2=3;legY=22
  }else{
    W=800;H=200;PL=62;PR=52;PT=16;PB=24
    fs=11;fsTick=10;sw=2;sw2=1.5;legY=13
  }
  var CW=W-PL-PR,CH=H-PT-PB
  var vlo=Infinity,vhi=-Infinity
  data.forEach(function(d){if(d.v<vlo)vlo=d.v;if(d.v>vhi)vhi=d.v})
  var vrng=Math.max(0.1,vhi-vlo);vlo-=vrng*.08;vhi+=vrng*.08;vrng=vhi-vlo
  var ts=data.map(function(d){return new Date(d.ts).getTime()})
  var t0=ts[0],t1=ts[ts.length-1],tspan=Math.max(1,t1-t0)
  function xp(i){return (PL+(ts[i]-t0)/tspan*CW).toFixed(1)}
  function yv(v){return (PT+CH-(v-vlo)/vrng*CH).toFixed(1)}
  function ys(s){return (PT+CH-(s/100)*CH).toFixed(1)}
  var s="<rect width='"+W+"' height='"+H+"' style='fill:var(--chart-bg)' rx='6'/>"
  s+="<g stroke='var(--chart-grid)' stroke-width='1'>"
  for(var i=0;i<=4;i++){var gy=(PT+CH*i/4).toFixed(1);s+="<line x1='"+PL+"' y1='"+gy+"' x2='"+(W-PR)+"' y2='"+gy+"'/>"}
  s+="</g>"
  s+="<g font-size='"+fs+"' font-family='monospace' text-anchor='end' style='fill:var(--cv)'>"
  for(var i=0;i<=4;i++) s+="<text x='"+(PL-6)+"' y='"+(PT+CH*i/4+5).toFixed(1)+"'>"+(vhi-vrng*i/4).toFixed(2)+"V</text>"
  s+="</g>"
  s+="<g font-size='"+fs+"' font-family='monospace' text-anchor='start' style='fill:var(--csoc)'>"
  for(var i=0;i<=4;i++) s+="<text x='"+(W-PR+6)+"' y='"+(PT+CH*i/4+5).toFixed(1)+"'>"+(100-25*i)+"%</text>"
  s+="</g>"
  s+=timeTicks(t0,t1,tspan,PL,CW,PT,CH,H-8,fsTick)
  txEvs.forEach(function(e){
    var x=PL+(e.start*1000-t0)/tspan*CW
    if(x>=PL&&x<=PL+CW){
      s+="<g><line x1='"+x.toFixed(1)+"' y1='"+PT+"' x2='"+x.toFixed(1)+"' y2='"+(PT+CH)+"' stroke='var(--ca)' stroke-width='1' stroke-dasharray='2,3' opacity='0.7'/>"
      s+="<text x='"+x.toFixed(1)+"' y='"+(PT-4)+"' text-anchor='middle' font-size='12' fill='var(--ca)'>\u26a1</text>"
      s+="<title>TX Event\nDuration: "+e.duration.toFixed(1)+" s\nPeak Power: "+e.peak_power.toFixed(0)+" W</title></g>"
    }
  })
  var pts=data.map(function(d,i){return xp(i)+','+yv(d.v)}).join(' ')
  s+="<polyline fill='none' stroke='var(--cv)' stroke-width='"+sw+"' stroke-linecap='round' stroke-linejoin='round' points='"+pts+"'/>"
  pts=data.map(function(d,i){return xp(i)+','+ys(d.soc)}).join(' ')
  s+="<polyline fill='none' stroke='var(--csoc)' stroke-width='"+sw2+"' stroke-dasharray='6,4' stroke-linecap='round' stroke-linejoin='round' points='"+pts+"'/>"
  s+="<g font-size='"+fs+"' font-family='monospace'>"
  s+="<line x1='"+(PL+8)+"' y1='"+legY+"' x2='"+(PL+28)+"' y2='"+legY+"' stroke='var(--cv)' stroke-width='"+sw+"' stroke-linecap='round'/>"
  s+="<text x='"+(PL+34)+"' y='"+(legY+4)+"' style='fill:var(--cv)'>Voltage</text>"
  s+="<line x1='"+(PL+110)+"' y1='"+legY+"' x2='"+(PL+130)+"' y2='"+legY+"' stroke='var(--csoc)' stroke-width='"+sw2+"' stroke-dasharray='6,4' stroke-linecap='round'/>"
  s+="<text x='"+(PL+136)+"' y='"+(legY+4)+"' style='fill:var(--csoc)'>SoC</text>"
  s+="</g>"
  el.setAttribute('viewBox','0 0 '+W+' '+H)
  el.innerHTML=s
}

function renderChgChart(data){
  var el=$('chg-chart'); if(!el) return
  if(!data||data.length<2){
    el.innerHTML="<text x='50%' y='50%' font-size='14' font-family='monospace' text-anchor='middle' dominant-baseline='middle' style='fill:var(--muted)'>Collecting data\u2026</text>"
    return
  }
  var md=chartDims(el,280,260)
  var W,H,PL,PR,PT,PB,fs,fsTick,sw,sw2,legY
  if(md){
    W=md.w;H=md.hChg;PL=78;PR=72;PT=32;PB=42
    fs=16;fsTick=14;sw=4;sw2=3;legY=22
  }else{
    W=800;H=180;PL=62;PR=52;PT=16;PB=24
    fs=11;fsTick=10;sw=2;sw2=1.5;legY=13
  }
  var CW=W-PL-PR,CH=H-PT-PB
  var win=Math.max(3,Math.floor(data.length/60))
  var rawV=data.map(function(d){return d.bat_v})
  var rawA=data.map(function(d){return d.bat_a})
  var smoothV=sma(rawV,win),smoothA=sma(rawA,win)
  var vlo=Infinity,vhi=-Infinity,alo=Infinity,ahi=-Infinity,hasSol=false
  data.forEach(function(d,i){
    var vm=d.sol_v>0.1?Math.max(smoothV[i],d.sol_v):smoothV[i]
    if(smoothV[i]<vlo)vlo=smoothV[i];if(vm>vhi)vhi=vm
    if(smoothA[i]<alo)alo=smoothA[i];if(smoothA[i]>ahi)ahi=smoothA[i]
    if(d.sol_v>0.1)hasSol=true
  })
  var vrng=Math.max(0.1,vhi-vlo);vlo-=vrng*.08;vhi+=vrng*.08;vrng=vhi-vlo
  var arng=Math.max(0.1,ahi-alo);alo-=arng*.08;ahi+=arng*.08;arng=ahi-alo
  var ts=data.map(function(d){return new Date(d.ts).getTime()})
  var t0=ts[0],t1=ts[ts.length-1],tspan=Math.max(1,t1-t0)
  function xp(i){return (PL+(ts[i]-t0)/tspan*CW).toFixed(1)}
  function yv(v){return (PT+CH-(v-vlo)/vrng*CH).toFixed(1)}
  function ya(a){return (PT+CH-(a-alo)/arng*CH).toFixed(1)}
  var s="<rect width='"+W+"' height='"+H+"' style='fill:var(--chart-bg)' rx='6'/>"
  s+="<g stroke='var(--chart-grid)' stroke-width='1'>"
  for(var i=0;i<=4;i++){var gy=(PT+CH*i/4).toFixed(1);s+="<line x1='"+PL+"' y1='"+gy+"' x2='"+(W-PR)+"' y2='"+gy+"'/>"}
  s+="</g>"
  s+="<g font-size='"+fs+"' font-family='monospace' text-anchor='end' style='fill:var(--cv)'>"
  for(var i=0;i<=4;i++) s+="<text x='"+(PL-6)+"' y='"+(PT+CH*i/4+5).toFixed(1)+"'>"+(vhi-vrng*i/4).toFixed(2)+"V</text>"
  s+="</g>"
  s+="<g font-size='"+fs+"' font-family='monospace' text-anchor='start' style='fill:var(--ca)'>"
  for(var i=0;i<=4;i++) s+="<text x='"+(W-PR+6)+"' y='"+(PT+CH*i/4+5).toFixed(1)+"'>"+(ahi-arng*i/4).toFixed(1)+"A</text>"
  s+="</g>"
  s+=timeTicks(t0,t1,tspan,PL,CW,PT,CH,H-8,fsTick)
  var pts=smoothV.map(function(v,i){return xp(i)+','+yv(v)}).join(' ')
  s+="<polyline fill='none' stroke='var(--cv)' stroke-width='"+sw+"' stroke-linecap='round' stroke-linejoin='round' points='"+pts+"'/>"
  pts=smoothA.map(function(a,i){return xp(i)+','+ya(a)}).join(' ')
  s+="<polyline fill='none' stroke='var(--ca)' stroke-width='"+sw2+"' stroke-dasharray='6,4' stroke-linecap='round' stroke-linejoin='round' points='"+pts+"'/>"
  if(hasSol){
    pts=data.map(function(d,i){return xp(i)+','+yv(d.sol_v)}).join(' ')
    s+="<polyline fill='none' stroke='var(--csol)' stroke-width='"+sw2+"' stroke-dasharray='4,5' stroke-linecap='round' stroke-linejoin='round' points='"+pts+"'/>"
    s+="<line x1='"+(PL+180)+"' y1='"+legY+"' x2='"+(PL+200)+"' y2='"+legY+"' stroke='var(--csol)' stroke-width='"+sw2+"' stroke-dasharray='4,5' stroke-linecap='round'/>"
    s+="<text x='"+(PL+206)+"' y='"+(legY+4)+"' font-size='"+fs+"' style='fill:var(--csol)'>Solar</text>"
  }
  s+="<g font-size='"+fs+"' font-family='monospace'>"
  s+="<line x1='"+(PL+8)+"' y1='"+legY+"' x2='"+(PL+28)+"' y2='"+legY+"' stroke='var(--cv)' stroke-width='"+sw+"' stroke-linecap='round'/>"
  s+="<text x='"+(PL+34)+"' y='"+(legY+4)+"' style='fill:var(--cv)'>Bat V</text>"
  s+="<line x1='"+(PL+100)+"' y1='"+legY+"' x2='"+(PL+120)+"' y2='"+legY+"' stroke='var(--ca)' stroke-width='"+sw2+"' stroke-dasharray='6,4' stroke-linecap='round'/>"
  s+="<text x='"+(PL+126)+"' y='"+(legY+4)+"' style='fill:var(--ca)'>Charge A</text>"
  s+="</g>"
  el.setAttribute('viewBox','0 0 '+W+' '+H)
  el.innerHTML=s
}

loadCharts()
setInterval(loadCharts,Math.max(pollIvl*1000,10000))
(function(){var rt;window.addEventListener('resize',function(){clearTimeout(rt);rt=setTimeout(loadCharts,150)})})()
</script></div></body></html>)";

    return o;
}

void HttpServer::send_response(int fd, int code, const char* content_type, const std::string& body) {
    const char* status_msg = "OK";
    if (code == 400) status_msg = "Bad Request";
    else if (code == 404) status_msg = "Not Found";
    else if (code == 405) status_msg = "Method Not Allowed";

    char header[320];
    std::snprintf(header, sizeof(header),
        "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        code, status_msg, content_type, body.size());
    send(fd, header, strlen(header), MSG_NOSIGNAL);
    send(fd, body.data(), body.size(), MSG_NOSIGNAL);
}
