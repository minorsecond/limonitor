#include "http_server.hpp"
#include "logger.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
HttpServer::HttpServer(DataStore& store, const std::string& bind_addr, uint16_t port)
    : store_(store), bind_addr_(bind_addr), port_(port) {}

HttpServer::~HttpServer() { stop(); }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Serve loop
// ---------------------------------------------------------------------------
void HttpServer::serve_loop() {
    while (running_) {
        struct sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client = accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client < 0) {
            if (running_) LOG_DEBUG("HTTP: accept error: %s", strerror(errno));
            continue;
        }
        // Set 5s receive timeout
        struct timeval tv{5, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        handle(client);
        close(client);
    }
}

// ---------------------------------------------------------------------------
// Request handler
// ---------------------------------------------------------------------------
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

    if (path == "/" || path == "/index.html") {
        auto hist    = store_.history(288);
        auto pg_hist = store_.pwrgate_history(288);
        send_response(fd, 200, "text/html", html_dashboard(snap, ble_st, hist, pg, pg_hist));
    } else if (path == "/api/charger") {
        send_response(fd, 200, "application/json", charger_json(pg));
    } else if (path == "/api/status") {
        send_response(fd, 200, "application/json", status_json(snap, ble_st));
    } else if (path == "/api/cells") {
        send_response(fd, 200, "application/json", cells_json(snap));
    } else if (path == "/api/history") {
        size_t count = 100;
        auto ni = query.find("n=");
        if (ni != std::string::npos) count = std::stoul(query.substr(ni + 2));
        send_response(fd, 200, "application/json", history_json(store_.history(count)));
    } else if (path == "/metrics") {
        send_response(fd, 200, "text/plain; version=0.0.4",
                      prometheus(snap) + prometheus_charger(pg));
    } else {
        send_response(fd, 404, "text/plain", "Not Found");
    }
}

// ---------------------------------------------------------------------------
// Response serializers
// ---------------------------------------------------------------------------
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

    // Time axis labels
    {
        double span_min = (N - 1) * 5.0 / 60.0; // assuming 5s poll
        std::snprintf(buf, sizeof(buf),
            "<text x='%d' y='%d' fill='#555' font-size='10' font-family='monospace'>%.0f min ago</text>\n",
            PL, H - 4, span_min);
        o += buf;
        o += "<text x='"; o += std::to_string(W - PR);
        o += "' y='"; o += std::to_string(H - 4);
        o += "' fill='#555' font-size='10' font-family='monospace' text-anchor='end'>now</text>\n";
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

    const int W = 800, H = 180;
    const int PL = 52, PR = 48, PT = 14, PB = 28;
    const int CW = W - PL - PR, CH = H - PT - PB;

    double vlo = 1e9, vhi = -1e9, alo = 1e9, ahi = -1e9;
    bool has_solar = false;
    for (auto& p : hist) {
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

    int N = static_cast<int>(hist.size());
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
        std::snprintf(buf, sizeof(buf), "%.1f,%.1f ", xp(i), yv(hist[i].bat_v));
        o += buf;
    }
    o += "'/>\n";

    // Charge current polyline (orange, dashed)
    o += "<polyline fill='none' stroke='#ff9800' stroke-width='1.5' stroke-dasharray='5,3' points='";
    for (int i = 0; i < N; ++i) {
        std::snprintf(buf, sizeof(buf), "%.1f,%.1f ", xp(i), ya(hist[i].bat_a));
        o += buf;
    }
    o += "'/>\n";

    {
        double span_min = (N - 1) / 60.0; // ~1 sample/sec from serial
        std::snprintf(buf, sizeof(buf),
            "<text x='%d' y='%d' fill='#555' font-size='10' font-family='monospace'>%.0f min ago</text>\n",
            PL, H-4, span_min);
        o += buf;
        o += "<text x='"; o += std::to_string(W-PR);
        o += "' y='"; o += std::to_string(H-4);
        o += "' fill='#555' font-size='10' font-family='monospace' text-anchor='end'>now</text>\n";
    }

    // Solar voltage polyline (cyan, dotted) — only if any solar data
    if (has_solar) {
        o += "<polyline fill='none' stroke='#00bcd4' stroke-width='1.5' stroke-dasharray='3,4' points='";
        for (int i = 0; i < N; ++i) {
            std::snprintf(buf, sizeof(buf), "%.1f,%.1f ", xp(i), yv(hist[i].sol_v));
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
                                       const std::vector<BatterySnapshot>& hist,
                                       const PwrGateSnapshot& pg,
                                       const std::vector<PwrGateSnapshot>& pg_hist) {
    std::string ts = s.valid ? iso8601(s.timestamp) : "—";
    std::string o = R"(<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<meta http-equiv="refresh" content="5">
<title>limonitor — Battery Dashboard</title>
<style>
  body{font-family:monospace;background:#0d0d0d;color:#ccc;margin:2em}
  h1{color:#4caf50}
  .card{background:#1a1a1a;border:1px solid #333;padding:1em;margin:1em 0;border-radius:4px}
  .ok{color:#4caf50}.warn{color:#ff9800}.err{color:#f44336}
  table{border-collapse:collapse;width:100%}
  td,th{padding:4px 8px;text-align:left}
  th{color:#888}
  .bar{display:inline-block;background:#4caf50;height:14px;vertical-align:middle}
</style></head><body>
<h1>limonitor</h1>
<div class="card">
  <b>BLE:</b> )";
    o += ble_st + "&nbsp; <b>Device:</b> " + (s.device_name.empty() ? "—" : s.device_name);
    o += " &nbsp;<b>Updated:</b> " + ts;
    o += R"(</div>
<div class="card">)" + svg_history_chart(hist) + R"(</div>
<div class="card">
<table><tr><th>Metric</th><th>Value</th></tr>)";

    auto row = [&](const char* k, const std::string& v) {
        o += "<tr><td>" + std::string(k) + "</td><td>" + v + "</td></tr>\n";
    };
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%.2f V", s.total_voltage_v); row("Voltage", buf);
    {
        const char* dir     = s.current_a >  0.01 ? "DISCHARGING" :
                              s.current_a < -0.01 ? "CHARGING"    : "IDLE";
        const char* dir_cls = s.current_a >  0.01 ? "warn" :
                              s.current_a < -0.01 ? "ok"   : "";
        std::snprintf(buf, sizeof(buf), "%.2f A &nbsp;<span class=\"%s\"><b>%s</b></span>",
                      s.current_a, dir_cls, dir);
        row("Current", buf);
    }
    std::snprintf(buf, sizeof(buf), "%.1f %%", s.soc_pct); row("SoC", buf);
    std::snprintf(buf, sizeof(buf), "%.2f / %.2f Ah", s.remaining_ah, s.nominal_ah); row("Capacity", buf);
    std::snprintf(buf, sizeof(buf), "%.1f W", s.power_w); row("Power", buf);
    {
        const char* time_lbl = s.current_a < -0.01 ? "to full" : "to empty";
        std::snprintf(buf, sizeof(buf), "%.1f h %s", s.time_remaining_h, time_lbl);
        row("Est. Remaining", buf);
    }
    std::snprintf(buf, sizeof(buf), "%u", s.cycle_count); row("Cycles", buf);

    o += "</table></div>";

    // Cell voltages
    if (!s.cell_voltages_v.empty()) {
        o += "<div class=\"card\"><b>Cell Voltages</b><table><tr><th>#</th><th>V</th><th></th></tr>\n";
        for (size_t i = 0; i < s.cell_voltages_v.size(); ++i) {
            double v = s.cell_voltages_v[i];
            int bar = static_cast<int>((v - 2.8) / (3.65 - 2.8) * 200);
            if (bar < 0) bar = 0; if (bar > 200) bar = 200;
            std::snprintf(buf, sizeof(buf),
                "<tr><td>%zu</td><td>%.3f</td><td><span class=\"bar\" style=\"width:%dpx\"></span></td></tr>\n",
                i + 1, v, bar);
            o += buf;
        }
        o += "</table></div>";
    }

    // Temperatures
    if (!s.temperatures_c.empty()) {
        o += "<div class=\"card\"><b>Temperatures</b><br>";
        for (size_t i = 0; i < s.temperatures_c.size(); ++i) {
            std::snprintf(buf, sizeof(buf), "T%zu: %.1f°C &nbsp;", i + 1, s.temperatures_c[i]);
            o += buf;
        }
        o += "</div>";
    }

    // Protection
    if (s.protection.any()) {
        o += "<div class=\"card err\"><b>PROTECTION ACTIVE:</b> ";
        if (s.protection.cell_overvoltage)     o += "CellOV ";
        if (s.protection.cell_undervoltage)    o += "CellUV ";
        if (s.protection.pack_overvoltage)     o += "PackOV ";
        if (s.protection.pack_undervoltage)    o += "PackUV ";
        if (s.protection.charge_overtemp)      o += "ChgOT ";
        if (s.protection.discharge_overtemp)   o += "DchgOT ";
        if (s.protection.charge_overcurrent)   o += "ChgOC ";
        if (s.protection.discharge_overcurrent)o += "DchgOC ";
        if (s.protection.short_circuit)        o += "ShortCircuit ";
        o += "</div>";
    }

    // Charger (PwrGate) section
    if (pg.valid) {
        o += "<div class=\"card\"><b>Charger — EpicPowerGate 2</b>";
        o += "<table><tr><th>Metric</th><th>Value</th></tr>\n";
        auto crow = [&](const char* k, const std::string& v) {
            o += "<tr><td>" + std::string(k) + "</td><td>" + v + "</td></tr>\n";
        };
        const char* st_cls = (pg.state == "Charging") ? "ok" :
                             (pg.state == "Float")     ? "ok" : "warn";
        std::snprintf(buf, sizeof(buf), "<span class='%s'><b>%s</b></span>", st_cls, pg.state.c_str());
        crow("State", buf);
        std::snprintf(buf, sizeof(buf), "%.2f V", pg.ps_v);   crow("Power Supply", buf);
        std::snprintf(buf, sizeof(buf), "%.2f V", pg.sol_v);  crow("Solar", buf);
        std::snprintf(buf, sizeof(buf), "%.2f V  /  %.2f A", pg.bat_v, pg.bat_a); crow("Battery (charger)", buf);
        std::snprintf(buf, sizeof(buf), "%.2f V  max %.2f A  stop %.2f A",
                      pg.target_v, pg.target_a, pg.stop_a);   crow("Target", buf);
        std::snprintf(buf, sizeof(buf), "%d / 1023  (%d%%)", pg.pwm, pg.pwm * 100 / 1023); crow("PWM", buf);
        std::snprintf(buf, sizeof(buf), "%d min", pg.minutes); crow("Elapsed", buf);
        std::snprintf(buf, sizeof(buf), "%d (raw)", pg.temp);  crow("Temp", buf);
        o += "</table>";
        if (!pg_hist.empty())
            o += svg_charger_chart(pg_hist);
        o += "</div>";
    }

    o += R"(<details class="card" style="font-size:0.85em;cursor:pointer">
<summary style="color:#888;outline:none"><b>Help — field reference</b></summary>
<table style="margin-top:0.5em">
<tr><th colspan="2" style="color:#4caf50;padding-top:0.5em">BATTERY (BLE)</th></tr>
<tr><td>Voltage</td><td>Total pack voltage (sum of all cells)</td></tr>
<tr><td>Current</td><td>Amps in/out. Negative = charging, positive = discharging</td></tr>
<tr><td>Power</td><td>Voltage × Current (negative while charging)</td></tr>
<tr><td>SoC</td><td>State of Charge — % of rated capacity remaining</td></tr>
<tr><td>Capacity</td><td>Remaining Ah / Nominal (rated) Ah</td></tr>
<tr><td>Est. Remaining</td><td>Time to full (charging) or empty (discharging)</td></tr>
<tr><td>Cycles</td><td>Full charge cycles counted by the BMS</td></tr>
<tr><th colspan="2" style="color:#4caf50;padding-top:0.5em">CELLS</th></tr>
<tr><td>C1..N</td><td>Individual cell voltages. LiFePO4 healthy range: ~3.0–3.65 V</td></tr>
<tr><td>delta</td><td>Spread between min and max cell. &lt;5 mV = excellent, &gt;50 mV = rebalance</td></tr>
<tr><th colspan="2" style="color:#4caf50;padding-top:0.5em">CHARGER (EpicPowerGate 2)</th></tr>
<tr><td>State</td><td>Charging / Float / Idle / Standby</td></tr>
<tr><td>Power Supply</td><td>Input supply voltage measured by charger</td></tr>
<tr><td>Solar</td><td>Solar panel voltage (0 when no solar input)</td></tr>
<tr><td>Battery</td><td>Battery voltage and current as measured by charger</td></tr>
<tr><td>Target</td><td>Bulk charge target voltage / max current / stop (termination) current</td></tr>
<tr><td>PWM</td><td>Charge controller duty cycle (1023 = 100% = full power)</td></tr>
<tr><td>Elapsed</td><td>Minutes since current charge session started</td></tr>
<tr><td>Temp</td><td>Charger controller temperature (raw ADC — not °C)</td></tr>
</table>
</details>
<div class="card" style="font-size:0.8em;color:#555">
API: <a href="/api/status">/api/status</a> &nbsp;
<a href="/api/cells">/api/cells</a> &nbsp;
<a href="/api/history">/api/history</a> &nbsp;
<a href="/api/charger">/api/charger</a> &nbsp;
<a href="/metrics">/metrics</a> (Prometheus)
</div></body></html>)";
    return o;
}

// ---------------------------------------------------------------------------
// Send HTTP response
// ---------------------------------------------------------------------------
void HttpServer::send_response(int fd, int code, const char* content_type, const std::string& body) {
    const char* status_msg = "OK";
    if (code == 400) status_msg = "Bad Request";
    else if (code == 404) status_msg = "Not Found";
    else if (code == 405) status_msg = "Method Not Allowed";

    char header[256];
    std::snprintf(header, sizeof(header),
        "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
        code, status_msg, content_type, body.size());
    send(fd, header, strlen(header), MSG_NOSIGNAL);
    send(fd, body.data(), body.size(), MSG_NOSIGNAL);
}
