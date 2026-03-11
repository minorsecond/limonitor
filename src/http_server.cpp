#include "http_server.hpp"
#include "logger.hpp"
#include "analytics/extensions.hpp"
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
    std::string r;
    r.reserve(s.size() + 4);
    r += '"';
    for (char c : s) {
        if (c == '"') r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else r += c;
    }
    r += '"';
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

HttpServer::HttpServer(DataStore& store, Database* db, const std::string& bind_addr, uint16_t port,
                       int poll_interval_s)
    : store_(store), db_(db), bind_addr_(bind_addr), port_(port),
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

    auto lf = req.find('\n');
    if (lf == std::string::npos) { send_response(fd, 400, "text/plain", "Bad Request"); return; }
    std::string line = req.substr(0, lf);
    if (line.back() == '\r') line.pop_back();

    std::istringstream ss(line);
    std::string method, path, proto;
    ss >> method >> path >> proto;

    // Strip query string for routing
    std::string query;
    auto qpos = path.find('?');
    if (qpos != std::string::npos) { query = path.substr(qpos + 1); path = path.substr(0, qpos); }

    if (method == "OPTIONS") {
        std::string hdr = "HTTP/1.0 204 No Content\r\n"
            "Content-Length: 0\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Access-Control-Max-Age: 86400\r\n"
            "Connection: close\r\n\r\n";
        send(fd, hdr.data(), hdr.size(), MSG_NOSIGNAL);
        return;
    }

    if (method == "POST" && path == "/api/settings") {
        if (!db_) {
            send_response(fd, 503, "application/json", "{\"error\":\"Settings not available\"}\n");
            return;
        }
        auto body_start = req.find("\r\n\r\n");
        if (body_start != std::string::npos) body_start += 4;
        else { body_start = req.find("\n\n"); if (body_start != std::string::npos) body_start += 2; }
        std::string body = (body_start != std::string::npos && body_start < req.size())
            ? req.substr(body_start) : "";
        size_t p = 0;
        while (p < body.size()) {
            auto k1 = body.find('"', p); if (k1 == std::string::npos) break;
            auto k2 = body.find('"', k1 + 1); if (k2 == std::string::npos) break;
            auto c = body.find(':', k2); if (c == std::string::npos) break;
            std::string key = body.substr(k1 + 1, k2 - k1 - 1);
            std::string val;
            auto v1 = body.find('"', c);
            if (v1 != std::string::npos && v1 - c <= 3) {
                auto v2 = body.find('"', v1 + 1); if (v2 == std::string::npos) break;
                val = body.substr(v1 + 1, v2 - v1 - 1);
                p = v2 + 1;
            } else {
                auto end = body.find_first_of(",}", c + 1);
                if (end == std::string::npos) break;
                val = body.substr(c + 1, end - c - 1);
                while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r' || val.back() == '\n')) val.pop_back();
                while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(0, 1);
                p = end;
            }
            db_->set_setting(key, val);
        }
        send_response(fd, 200, "application/json", "{\"ok\":true}\n");
        return;
    }

    if (method != "GET") { send_response(fd, 405, "text/plain", "Method Not Allowed"); return; }

    auto snap_opt = store_.latest();
    BatterySnapshot snap;
    if (snap_opt) snap = *snap_opt;
    std::string ble_st = store_.ble_state();

    auto pg_opt = store_.latest_pwrgate();
    PwrGateSnapshot pg;
    if (pg_opt) pg = *pg_opt;

    double h = 1.0;
    {
        auto hi = query.find("h=");
        if (hi != std::string::npos) {
            try { h = std::stod(query.substr(hi + 2)); } catch (...) {}
        }
        h = std::max(0.1, std::min(h, 168.0));
    }
    if (path == "/settings" || path == "/settings.html") {
        send_response(fd, 200, "text/html", html_settings_page(db_));
    } else if (path == "/setup") {
        send_response(fd, 200, "text/html", html_settings_page(db_));
    } else if (path == "/" || path == "/index.html") {
        bool needs_setup = false;
        if (db_) {
            needs_setup = db_->get_setting("device_name").empty() &&
                          db_->get_setting("device_address").empty() &&
                          db_->get_setting("serial_device").empty() &&
                          db_->get_setting("pwrgate_remote").empty();
        }
        if (needs_setup) {
            send_response(fd, 200, "text/html", html_settings_page(db_));
        } else {
            std::string init_settings;
            if (db_) {
                init_settings = "{\"theme\":" + jstr(db_->get_setting("theme")) +
                    ",\"time\":" + jstr(db_->get_setting("time")) +
                    ",\"range\":" + jstr(db_->get_setting("range")) +
                    ",\"show-extended\":" + jstr(db_->get_setting("show-extended")) +
                    ",\"locale\":" + jstr(db_->get_setting("locale")) + "}";
            }
            std::string theme = db_ ? db_->get_setting("theme") : "";
            send_response(fd, 200, "text/html",
                          html_dashboard(snap, ble_st, pg,
                                         store_.purchase_date(), h, poll_interval_s_, init_settings, theme));
        }
    } else if (path == "/solar" || path == "/solar.html") {
        std::string init_settings;
        if (db_) {
            init_settings = "{\"theme\":" + jstr(db_->get_setting("theme")) +
                ",\"time\":" + jstr(db_->get_setting("time")) +
                ",\"range\":" + jstr(db_->get_setting("range")) +
                ",\"show-extended\":" + jstr(db_->get_setting("show-extended")) +
                ",\"locale\":" + jstr(db_->get_setting("locale")) + "}";
        }
        std::string theme = db_ ? db_->get_setting("theme") : "";
        send_response(fd, 200, "text/html", html_solar_page(init_settings, theme));
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
        auto an = store_.analytics();
        if (an.avg_discharge_24h_w < 0.5 && db_ && snap.valid && snap.nominal_ah > 1.0) {
            auto usage = db_->get_usage_profile(7);
            int total_n = 0;
            double total_sum = 0;
            for (const auto& u : usage) {
                total_sum += u.avg_w * u.sample_count;
                total_n += u.sample_count;
            }
            double load_w = (total_n > 0) ? total_sum / total_n : 0;
            if (load_w > 0.5) {
                double v = snap.total_voltage_v > 1 ? snap.total_voltage_v : 51.2;
                int cells = std::max(1, static_cast<int>(std::round(v / 3.3)));
                double pack_v = cells * 3.2;
                double cap_wh = snap.nominal_ah * pack_v;
                double rf = estimate_runtime_h(cap_wh, 100.0, 10.0, load_w);
                double rn = estimate_runtime_h(cap_wh, snap.soc_pct, 10.0, load_w);
                if (rn > 0) {
                    an.avg_discharge_24h_w = load_w;
                    an.runtime_from_full_h = std::min(rf, 1000.0);
                    an.runtime_from_current_h = std::min(rn, 1000.0);
                    an.runtime_full_exceeds_cap = (rf > 1000.0);
                    an.runtime_current_exceeds_cap = (rn > 1000.0);
                    an.runtime_from_historical = true;
                }
            }
        }
        send_response(fd, 200, "application/json",
                      analytics_json(an, &store_));
    } else if (path == "/api/events") {
        size_t count = 50;
        auto ni = query.find("n=");
        if (ni != std::string::npos) { try { count = std::stoul(query.substr(ni + 2)); } catch (...) {} }
        send_response(fd, 200, "application/json",
                      system_events_json(store_.system_events(count)));
    } else if (path == "/api/anomalies") {
        auto* ext = store_.extensions();
        if (ext)
            send_response(fd, 200, "application/json",
                         anomalies_json(ext->anomaly_detector().anomalies_vec()));
        else
            send_response(fd, 200, "application/json", "{\"anomalies\":[]}\n");
    } else if (path == "/api/solar_simulation") {
        auto* ext = store_.extensions();
        if (ext) {
            double max_a = ext->effective_max_charge_a();
            double v = snap.valid && snap.total_voltage_v > 1 ? snap.total_voltage_v : 51.2;
            double nom = snap.valid && snap.nominal_ah > 0 ? snap.nominal_ah : 100.0;
            auto r = ext->solar_simulator().compute(
                snap.valid ? snap.soc_pct : 0, -1, max_a, v, nom);
            send_response(fd, 200, "application/json", solar_simulation_json(r));
        } else {
            LOG_DEBUG("Solar /api/solar_simulation: extensions not available");
            send_response(fd, 200, "application/json",
                         "{\"panel_watts\":0,\"expected_today_wh\":0,\"battery_projection\":\"—\"}\n");
        }
    } else if (path == "/api/battery/resistance") {
        auto* ext = store_.extensions();
        if (ext) {
            double mohm = ext->resistance_estimator().internal_resistance_mohm();
            std::string trend = ext->resistance_estimator().trend();
            auto series = ext->resistance_estimator().time_series(100);
            send_response(fd, 200, "application/json",
                         battery_resistance_json(mohm, trend, series));
        } else {
            send_response(fd, 200, "application/json",
                         "{\"internal_resistance_milliohms\":0,\"trend\":\"stable\"}\n");
        }
    } else if (path == "/api/solar_forecast_week") {
        auto* ext = store_.extensions();
        if (ext) {
            const auto& cfg = ext->solar_simulator().config();
            double max_a = ext->effective_max_charge_a();
            double v = snap.valid && snap.total_voltage_v > 1 ? snap.total_voltage_v : 51.2;
            double nom = snap.valid && snap.nominal_ah > 0 ? snap.nominal_ah : 100.0;
            double soc = snap.valid ? snap.soc_pct : 0;
            bool want_both = query.find("both=1") != std::string::npos;
            bool want_realistic = !want_both && query.find("realistic=1") != std::string::npos;

            std::vector<ChargeAcceptanceBucket> profile;
            bool profile_from_db = false;
            if ((want_realistic || want_both) && db_)
                profile = db_->get_charge_acceptance_profile(14);
            if (want_realistic || want_both) {
                int total_samples = 0;
                for (const auto& b : profile) total_samples += b.sample_count;
                profile_from_db = total_samples >= 10;
            }
            if ((want_realistic || want_both) && !profile_from_db) {
                static constexpr double kDefaultTaper[] =
                    {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.95, 0.80, 0.45, 0.15};
                profile.clear();
                for (int i = 0; i < 10; ++i) {
                    ChargeAcceptanceBucket b;
                    b.soc_lo = i * 10;
                    b.soc_hi = (i + 1) * 10;
                    b.acceptance_ratio = kDefaultTaper[i];
                    b.sample_count = 100;
                    profile.push_back(b);
                }
            }

            if (want_both) {
                auto r_nom = ext->weather_forecast().get_forecast_week(
                    cfg.panel_watts, cfg.system_efficiency, max_a, v, nom, soc, nullptr);
                auto r_real = ext->weather_forecast().get_forecast_week(
                    cfg.panel_watts, cfg.system_efficiency, max_a, v, nom, soc, &profile);
                r_real.realistic_from_history = profile_from_db;
                auto uc = compute_usage_cache();
                auto json_nom = solar_forecast_week_json(r_nom, &uc);
                auto json_real = solar_forecast_week_json(r_real, &uc);
                std::string body;
                body.reserve(json_nom.size() + json_real.size() + 40);
                body += "{\"nominal\":";
                body += json_nom;
                body += ",\"realistic\":";
                body += json_real;
                body += "}\n";
                send_response(fd, 200, "application/json", body);
            } else {
                auto r = ext->weather_forecast().get_forecast_week(
                    cfg.panel_watts, cfg.system_efficiency, max_a, v, nom, soc,
                    want_realistic ? &profile : nullptr);
                r.realistic_from_history = profile_from_db;
                if (!r.valid && !r.error.empty())
                    LOG_DEBUG("Solar /api/solar_forecast_week: %s", r.error.c_str());
                send_response(fd, 200, "application/json", solar_forecast_week_json(r));
            }
        } else {
            LOG_DEBUG("Solar /api/solar_forecast_week: extensions not available");
            send_response(fd, 200, "application/json",
                         "{\"valid\":false,\"error\":\"Extensions not available\",\"daily\":[]}\n");
        }
    } else if (path == "/api/solar_forecast") {
        auto* ext = store_.extensions();
        if (ext) {
            const auto& cfg = ext->solar_simulator().config();
            double max_a = ext->effective_max_charge_a();
            double v = snap.valid && snap.total_voltage_v > 1 ? snap.total_voltage_v : 51.2;
            double nom = snap.valid && snap.nominal_ah > 0 ? snap.nominal_ah : 100.0;
            auto r = ext->weather_forecast().get_forecast(
                cfg.panel_watts, cfg.system_efficiency, max_a, v, nom,
                snap.valid ? snap.soc_pct : 0);
            if (!r.valid && !r.error.empty())
                LOG_DEBUG("Solar /api/solar_forecast: %s", r.error.c_str());
            send_response(fd, 200, "application/json", solar_forecast_json(r));
        } else {
            LOG_DEBUG("Solar /api/solar_forecast: extensions not available");
            send_response(fd, 200, "application/json",
                         "{\"tomorrow_generation_wh\":0,\"cloud_cover\":0,\"expected_battery_state\":\"—\"}\n");
        }
    } else if (path == "/api/weather_refresh") {
        auto* ext = store_.extensions();
        if (ext) {
            ext->weather_forecast().invalidate_cache();
            send_response(fd, 200, "application/json", "{\"ok\":true,\"message\":\"Cache invalidated. Next forecast request will fetch fresh data.\"}\n");
        } else {
            send_response(fd, 503, "application/json", "{\"ok\":false,\"message\":\"Extensions not available\"}\n");
        }
    } else if (path == "/api/solar_performance") {
        if (!db_) {
            send_response(fd, 200, "application/json",
                         "{\"avg_coefficient\":0,\"samples\":0,\"history\":[]}\n");
        } else {
            int days = 30;
            auto dpos = query.find("days=");
            if (dpos != std::string::npos) {
                int d = std::atoi(query.c_str() + dpos + 5);
                if (d > 0 && d <= 365) days = d;
            }
            auto perf = db_->load_solar_performance(days);
            double avg = db_->get_avg_solar_coefficient(days);
            std::string o = "{\"avg_coefficient\":" + jdbl(avg, 4) +
                ",\"samples\":" + std::to_string(perf.size()) + ",\"history\":[";
            for (size_t i = 0; i < perf.size(); ++i) {
                if (i) o += ",";
                o += "{\"ts\":" + std::to_string(perf[i].ts) +
                     ",\"cloud\":" + jdbl(perf[i].cloud_cover, 2) +
                     ",\"actual_w\":" + jdbl(perf[i].actual_w, 1) +
                     ",\"theoretical_w\":" + jdbl(perf[i].theoretical_w, 1) +
                     ",\"coeff\":" + jdbl(perf[i].coefficient, 4) +
                     ",\"sol_v\":" + jdbl(perf[i].sol_v, 1) +
                     ",\"bat_a\":" + jdbl(perf[i].bat_a, 2) +
                     ",\"soc\":" + jdbl(perf[i].soc, 1) + "}";
            }
            o += "]}\n";
            send_response(fd, 200, "application/json", o);
        }
    } else if (path == "/api/settings") {
        if (!db_) {
            send_response(fd, 200, "application/json", "{\"theme\":\"\",\"time\":\"24\",\"range\":\"1\",\"show-extended\":\"0\",\"locale\":\"\"}\n");
        } else {
            std::string t = db_->get_setting("theme");
            std::string tf = db_->get_setting("time");
            std::string r = db_->get_setting("range");
            std::string se = db_->get_setting("show-extended");
            std::string loc = db_->get_setting("locale");
            if (tf.empty()) tf = "24";
            if (r.empty()) r = "1";
            if (se.empty()) se = "0";
            std::string out = "{\"theme\":" + jstr(t) + ",\"time\":" + jstr(tf) +
                ",\"range\":" + jstr(r) + ",\"show-extended\":" + jstr(se) +
                ",\"locale\":" + jstr(loc) + "}\n";
            send_response(fd, 200, "application/json", out);
        }
    } else if (path == "/api/config") {
        if (!db_) {
            send_response(fd, 503, "application/json", "{\"error\":\"Config not available\"}\n");
        } else {
            auto g = [this](const std::string& k, const std::string& d) {
                std::string v = db_->get_setting(k);
                return v.empty() ? d : v;
            };
            std::string out = "{\"device_address\":" + jstr(g("device_address", "")) +
                ",\"device_name\":" + jstr(g("device_name", "")) +
                ",\"adapter_path\":" + jstr(g("adapter_path", "/org/bluez/hci0")) +
                ",\"service_uuid\":" + jstr(g("service_uuid", "0000ffe0-0000-1000-8000-00805f9b34fb")) +
                ",\"notify_uuid\":" + jstr(g("notify_uuid", "0000ffe1-0000-1000-8000-00805f9b34fb")) +
                ",\"write_uuid\":" + jstr(g("write_uuid", "0000ffe2-0000-1000-8000-00805f9b34fb")) +
                ",\"poll_interval\":" + jstr(g("poll_interval", "5")) +
                ",\"http_port\":" + jstr(g("http_port", "8080")) +
                ",\"http_bind\":" + jstr(g("http_bind", "0.0.0.0")) +
                ",\"log_file\":" + jstr(g("log_file", "")) +
                ",\"verbose\":" + jstr(g("verbose", "0")) +
                ",\"serial_device\":" + jstr(g("serial_device", "")) +
                ",\"serial_baud\":" + jstr(g("serial_baud", "115200")) +
                ",\"pwrgate_remote\":" + jstr(g("pwrgate_remote", "")) +
                ",\"db_path\":" + jstr(g("db_path", "")) +
                ",\"db_interval\":" + jstr(g("db_interval", "60")) +
                ",\"daemon\":" + jstr(g("daemon", "0")) +
                ",\"battery_purchased\":" + jstr(g("battery_purchased", "")) +
                ",\"rated_capacity_ah\":" + jstr(g("rated_capacity_ah", "0")) +
                ",\"tx_threshold\":" + jstr(g("tx_threshold", "1.0")) +
                ",\"solar_enabled\":" + jstr(g("solar_enabled", "0")) +
                ",\"solar_panel_watts\":" + jstr(g("solar_panel_watts", "400")) +
                ",\"solar_system_efficiency\":" + jstr(g("solar_system_efficiency", "0.75")) +
                ",\"solar_zip_code\":" + jstr(g("solar_zip_code", "80112")) +
                ",\"weather_api_key\":" + jstr(g("weather_api_key", "")) +
                ",\"weather_zip_code\":" + jstr(g("weather_zip_code", "80112")) + "}\n";
            send_response(fd, 200, "application/json", out);
        }
    } else if (path == "/api/system_health") {
        auto* ext = store_.extensions();
        if (ext) {
            auto an = store_.analytics();
            auto r = ext->health_scorer().compute(an, ext->anomaly_detector().anomalies().size());
            send_response(fd, 200, "application/json", system_health_json(r));
        } else {
            send_response(fd, 200, "application/json",
                         "{\"score\":0,\"battery\":\"—\",\"cells\":\"—\",\"charging\":\"—\"}\n");
        }
    } else if (path == "/metrics") {
        send_response(fd, 200, "text/plain; version=0.0.4",
                      prometheus(snap) + prometheus_charger(pg));
    } else {
        send_response(fd, 404, "text/plain", "Not Found");
    }
}

std::string HttpServer::status_json(const BatterySnapshot& s, const std::string& ble_st) {
    std::string o;
    o.reserve(2048);
    o += "{\n";
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
    std::string o;
    o.reserve(snaps.size() * 96 + 16);
    o += "[";
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
    o.reserve(2048);
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

std::string HttpServer::system_events_json(const std::vector<SystemEvent>& events) {
    std::string o = "[";
    for (size_t i = events.size(); i > 0; --i) {
        const auto& e = events[i - 1];
        if (i < events.size()) o += ",";
        char tbuf[32];
        std::time_t t = std::chrono::system_clock::to_time_t(e.timestamp);
        struct tm tm_b{};
        localtime_r(&t, &tm_b);
        std::strftime(tbuf, sizeof(tbuf), "%m/%d %H:%M", &tm_b);
        o += "\n  {\"time\":" + jstr(tbuf) + ",\"message\":" + jstr(e.message) + "}";
    }
    o += "\n]\n";
    return o;
}

std::string HttpServer::analytics_json(const AnalyticsSnapshot& a,
                                       const DataStore* store) {
    std::string o;
    o.reserve(4096);
    o += "{\n";
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
    o += "  \"idle_load_watts\": "            + jdbl(a.idle_load_watts, 1)           + ",\n";
    o += "  \"system_status\": "              + jstr(a.system_status)               + ",\n";
    o += "  \"battery_stress\": "             + jstr(a.battery_stress)             + ",\n";
    o += "  \"charger_runtime_today\": "      + jdbl(a.charger_runtime_today_sec, 0) + ",\n";
    o += "  \"energy_used_today_wh\": "        + jdbl(a.energy_discharged_today_wh, 1)+ ",\n";
    o += "  \"energy_used_week_wh\": "        + jdbl(a.energy_used_week_wh, 1)      + ",\n";
    o += "  \"voltage_trend\": "              + jstr(a.voltage_trend)               + ",\n";
    o += "  \"soc_trend\": "                  + jstr(a.soc_trend)                  + ",\n";
    o += "  \"soc_rate_pct_per_h\": "        + jdbl(a.soc_rate_pct_per_h, 2)       + ",\n";
    o += "  \"charger_abnormal_warning\": "   + jstr(a.charger_abnormal_warning)    + ",\n";
    o += "  \"psu_limited_warning\": "        + jstr(a.psu_limited_warning)         + ",\n";
    o += "  \"charge_slowdown_warning\": "    + jstr(a.charge_slowdown_warning)     + ",\n";
    o += "  \"charge_rate_recent_pct_per_h\": "+ jdbl(a.charge_rate_recent_pct_per_h, 2)+ ",\n";
    o += "  \"charge_rate_baseline_pct_per_h\":"+ jdbl(a.charge_rate_baseline_pct_per_h, 2)+ ",\n";
    o += "  \"runtime_from_full_h\": "       + jdbl(a.runtime_from_full_h, 1)       + ",\n";
    o += "  \"runtime_from_current_h\": "    + jdbl(a.runtime_from_current_h, 1)    + ",\n";
    o += "  \"runtime_full_exceeds_cap\": "   + jbool(a.runtime_full_exceeds_cap)   + ",\n";
    o += "  \"runtime_current_exceeds_cap\": " + jbool(a.runtime_current_exceeds_cap) + ",\n";
    o += "  \"runtime_from_charger\": "       + jbool(a.runtime_from_charger)       + ",\n";
    o += "  \"runtime_from_historical\": "    + jbool(a.runtime_from_historical)    + ",\n";
    o += "  \"avg_discharge_24h_w\": "       + jdbl(a.avg_discharge_24h_w, 1)        + ",\n";
    o += "  \"solar_readiness\": "            + jstr(a.solar_readiness)            + ",\n";
    o += "  \"charge_rate_w\": "              + jdbl(a.charge_rate_w, 1)            + ",\n";
    o += "  \"charge_rate_pct_per_h\": "      + jdbl(a.charge_rate_pct_per_h, 1)    + ",\n";
    o += "  \"health_alerts\": [";
    for (size_t i = 0; i < a.health_alerts.size(); ++i) {
        if (i) o += ",";
        o += jstr(a.health_alerts[i]);
    }
    o += "]";
    if (store) {
        auto now_s = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            now_s - store->startup_time()).count();
        auto last_bms = store->last_bms_update();
        auto last_chg = store->last_charger_update();
        auto now_tp = std::chrono::system_clock::now();
        long bms_ago = -1, chg_ago = -1;
        if (store->latest().has_value())
            bms_ago = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
                now_tp - last_bms).count());
        if (store->latest_pwrgate().has_value())
            chg_ago = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
                now_tp - last_chg).count());
        o += ",\n  \"uptime_sec\": " + std::to_string(uptime);
        o += ",\n  \"last_bms_update_ago_sec\": " + (bms_ago >= 0 ? std::to_string(bms_ago) : "null");
        o += ",\n  \"last_charger_update_ago_sec\": " + (chg_ago >= 0 ? std::to_string(chg_ago) : "null");
    }
    o += "\n}\n";
    return o;
}

std::string HttpServer::anomalies_json(const std::vector<AnomalyEvent>& anomalies) {
    std::string o = "{\n  \"anomalies\": [\n";
    for (size_t i = 0; i < anomalies.size(); ++i) {
        const auto& a = anomalies[i];
        if (i) o += ",\n";
        o += "    {\n";
        o += "      \"type\": " + jstr(a.type) + ",\n";
        o += "      \"timestamp\": " + jstr(iso8601(a.timestamp)) + ",\n";
        o += "      \"message\": " + jstr(a.message);
        if (a.type == "load_spike") {
            o += ",\n      \"load_w\": " + jdbl(a.load_w, 0);
            o += ",\n      \"baseline_w\": " + jdbl(a.baseline_w, 0);
        } else if (a.type == "slow_charging") {
            o += ",\n      \"charge_current\": " + jdbl(a.charge_current, 2);
            o += ",\n      \"expected_current\": " + jdbl(a.expected_current, 2);
        } else if (a.type == "resistance_increase") {
            o += ",\n      \"r_internal_today\": " + jdbl(a.r_internal_today, 2);
            o += ",\n      \"r_internal_30day_avg\": " + jdbl(a.r_internal_30day_avg, 2);
        }
        o += "\n    }";
    }
    o += "\n  ]\n}\n";
    return o;
}

std::string HttpServer::solar_simulation_json(const SolarSimResult& r) {
    std::string o = "{\n";
    o += "  \"panel_watts\": " + jdbl(r.panel_watts, 0) + ",\n";
    o += "  \"expected_today_wh\": " + jdbl(r.expected_today_wh, 0) + ",\n";
    o += "  \"battery_projection\": " + jstr(r.battery_projection) + "\n";
    o += "}\n";
    return o;
}

std::string HttpServer::battery_resistance_json(double mohm, const std::string& trend,
                                               const std::vector<ResistanceSample>& series) {
    std::string o = "{\n";
    o += "  \"internal_resistance_milliohms\": " + jdbl(mohm, 1) + ",\n";
    o += "  \"trend\": " + jstr(trend) + ",\n";
    o += "  \"time_series\": [";
    for (size_t i = 0; i < series.size(); ++i) {
        if (i) o += ", ";
        o += "{\"timestamp\":" + jdbl(series[i].timestamp, 0) +
             ",\"resistance_mohm\":" + jdbl(series[i].resistance_mohm, 2) + "}";
    }
    o += "]\n}\n";
    return o;
}

std::string HttpServer::solar_forecast_json(const WeatherForecastResult& r) {
    std::string o = "{\n";
    o += "  \"valid\": " + jbool(r.valid) + ",\n";
    o += "  \"tomorrow_generation_wh\": " + jdbl(r.tomorrow_generation_wh, 0) + ",\n";
    o += "  \"cloud_cover\": " + jdbl(r.cloud_cover, 2) + ",\n";
    o += "  \"expected_battery_state\": " + jstr(r.expected_battery_state);
    if (!r.error.empty())
        o += ",\n  \"error\": " + jstr(r.error);
    o += "\n}\n";
    return o;
}

HttpServer::UsageCache HttpServer::compute_usage_cache() const {
    UsageCache c;
    if (db_) c.usage = db_->get_usage_profile(7);

    auto an = store_.analytics();
    c.measured_avg_w = an.avg_discharge_24h_w;

    if (!c.usage.empty()) {
        std::vector<double> avgs, sds;
        std::vector<int> counts;
        for (const auto& u : c.usage) {
            avgs.push_back(u.avg_w);
            sds.push_back(u.stddev_w);
            counts.push_back(u.sample_count);
        }
        scale_usage_profile(avgs, sds, counts, c.measured_avg_w);
        for (size_t i = 0; i < c.usage.size(); ++i) {
            c.usage[i].avg_w = avgs[i];
            c.usage[i].stddev_w = sds[i];
        }
        int total_n = 0;
        double total_sum = 0;
        for (const auto& u : c.usage) { total_sum += u.avg_w * u.sample_count; total_n += u.sample_count; }
        c.fallback_w = total_n > 0 ? total_sum / total_n : 0;
    }

    if (db_) {
        auto perf = db_->load_solar_performance(30);
        c.perf_count = static_cast<int>(perf.size());
        if (!perf.empty()) {
            double sum = 0;
            int valid_n = 0;
            for (const auto& p : perf) {
                if (p.coefficient > 0 && p.coefficient < 2) { sum += p.coefficient; ++valid_n; }
            }
            if (valid_n > 0) c.avg_coeff = sum / valid_n;
        }
    }
    return c;
}

std::string HttpServer::solar_forecast_week_json(const SolarForecastWeekResult& r,
                                                  const UsageCache* ext_cache) const {
    UsageCache local_cache;
    if (!ext_cache) {
        local_cache = compute_usage_cache();
        ext_cache = &local_cache;
    }
    const auto& usage = ext_cache->usage;
    double fallback_w = ext_cache->fallback_w;

    std::string o;
    o.reserve(8192 + r.daily.size() * 512 + r.slots.size() * 128);
    o += "{\n";
    o += "  \"valid\": " + jbool(r.valid) + ",\n";
    o += "  \"realistic\": " + jbool(r.realistic) + ",\n";
    o += "  \"realistic_from_history\": " + jbool(r.realistic_from_history) + ",\n";
    o += "  \"week_total_kwh\": " + jdbl(r.week_total_kwh, 2) + ",\n";
    o += "  \"recovery_wh\": " + jdbl(r.recovery_wh, 0) + ",\n";
    o += "  \"current_soc_pct\": " + jdbl(r.current_soc_pct, 1) + ",\n";
    o += "  \"days_to_full\": " + std::to_string(r.days_to_full) + ",\n";
    o += "  \"best_day\": " + jstr(r.best_day) + ",\n";
    if (!r.error.empty())
        o += "  \"error\": " + jstr(r.error) + ",\n";
    o += "  \"daily\": [";
    for (size_t i = 0; i < r.daily.size(); ++i) {
        const auto& d = r.daily[i];
        if (i) o += ",";
        double usage_kwh = 0, usage_var_wh2 = 0;
        for (int s = 0; s < 8; ++s) {
            double avg = (s < (int)usage.size() && usage[s].sample_count >= 3) ? usage[s].avg_w : fallback_w;
            double sd  = (s < (int)usage.size() && usage[s].sample_count >= 3) ? usage[s].stddev_w : avg * 0.2;
            usage_kwh += avg * 3.0 / 1000.0;
            usage_var_wh2 += sd * sd * 9.0;  // var of 3h slot energy (Wh²)
        }
        double daily_sd_kwh = std::sqrt(usage_var_wh2) / 1000.0;
        double usage_kwh_lo = std::max(0.0, usage_kwh - 1.96 * daily_sd_kwh);
        double usage_kwh_hi = usage_kwh + 1.96 * daily_sd_kwh;
        double surplus = d.kwh - usage_kwh;
        double surplus_lo = d.max_recovery_wh_lo / 1000.0 - usage_kwh_hi;
        double surplus_hi = d.max_recovery_wh_hi / 1000.0 - usage_kwh_lo;
        o += "\n    {\"date\":" + jstr(d.date) +
             ",\"kwh\":" + jdbl(d.kwh, 2) +
             ",\"cloud_cover\":" + jdbl(d.cloud_cover, 2) +
             ",\"optimal_start\":" + jstr(d.optimal_start) +
             ",\"optimal_end\":" + jstr(d.optimal_end) +
             ",\"optimal_reason\":" + jstr(d.optimal_reason) +
             ",\"sun_hours_effective\":" + jdbl(d.sun_hours_effective, 1) +
             ",\"max_recovery_pct\":" + jdbl(d.max_recovery_pct, 1) +
             ",\"max_recovery_pct_lo\":" + jdbl(d.max_recovery_pct_lo, 1) +
             ",\"max_recovery_pct_hi\":" + jdbl(d.max_recovery_pct_hi, 1) +
             ",\"max_recovery_ah\":" + jdbl(d.max_recovery_ah, 2) +
             ",\"max_recovery_ah_lo\":" + jdbl(d.max_recovery_ah_lo, 2) +
             ",\"max_recovery_ah_hi\":" + jdbl(d.max_recovery_ah_hi, 2) +
             ",\"max_recovery_wh\":" + jdbl(d.max_recovery_wh, 0) +
             ",\"max_recovery_wh_lo\":" + jdbl(d.max_recovery_wh_lo, 0) +
             ",\"max_recovery_wh_hi\":" + jdbl(d.max_recovery_wh_hi, 0) +
             ",\"usage_kwh\":" + jdbl(usage_kwh, 3) +
             ",\"usage_kwh_lo\":" + jdbl(usage_kwh_lo, 3) +
             ",\"usage_kwh_hi\":" + jdbl(usage_kwh_hi, 3) +
             ",\"surplus_kwh\":" + jdbl(surplus, 3) +
             ",\"surplus_kwh_lo\":" + jdbl(surplus_lo, 3) +
             ",\"surplus_kwh_hi\":" + jdbl(surplus_hi, 3) +
             ",\"is_extended\":" + std::string(d.is_extended ? "true" : "false") + "}";
    }
    o += "\n  ],\n";
    o += "  \"nominal_ah\": " + jdbl(r.nominal_ah, 1) + ",\n";
    o += "  \"battery_voltage\": " + jdbl(r.battery_voltage, 2) + ",\n";
    o += "  \"usage_profile\": [";
    for (int s = 0; s < 8; ++s) {
        double avg = (s < (int)usage.size() && usage[s].sample_count >= 3) ? usage[s].avg_w : fallback_w;
        double sd  = (s < (int)usage.size() && usage[s].sample_count >= 3) ? usage[s].stddev_w : avg * 0.2;
        int n      = (s < (int)usage.size()) ? usage[s].sample_count : 0;
        if (s) o += ",";
        o += "{\"slot\":" + std::to_string(s) +
             ",\"avg_w\":" + jdbl(avg, 1) +
             ",\"stddev_w\":" + jdbl(sd, 1) +
             ",\"n\":" + std::to_string(n) + "}";
    }
    o += "],\n";
    // Pre-compute per-slot generation and usage in Wh for SoC projection
    std::vector<double> gen_wh_vec, use_wh_vec;
    gen_wh_vec.reserve(r.slots.size());
    use_wh_vec.reserve(r.slots.size());

    struct SlotUsage { double avg; double sd; double kwh; double kwh_lo; double kwh_hi; };
    std::vector<SlotUsage> slot_usage;
    slot_usage.reserve(r.slots.size());

    for (size_t i = 0; i < r.slots.size(); ++i) {
        const auto& s = r.slots[i];
        int slot_idx = -1;
        {
            time_t tt = static_cast<time_t>(s.timestamp);
            struct tm tm_buf{};
            localtime_r(&tt, &tm_buf);
            slot_idx = tm_buf.tm_hour / 3;
        }
        double use_avg = (slot_idx >= 0 && slot_idx < (int)usage.size() && usage[slot_idx].sample_count >= 3)
                         ? usage[slot_idx].avg_w : fallback_w;
        double use_sd  = (slot_idx >= 0 && slot_idx < (int)usage.size() && usage[slot_idx].sample_count >= 3)
                         ? usage[slot_idx].stddev_w : use_avg * 0.2;
        double use_kwh = use_avg * 3.0 / 1000.0;
        double use_kwh_lo = std::max(0.0, (use_avg - 1.96 * use_sd) * 3.0 / 1000.0);
        double use_kwh_hi = (use_avg + 1.96 * use_sd) * 3.0 / 1000.0;
        slot_usage.push_back({use_avg, use_sd, use_kwh, use_kwh_lo, use_kwh_hi});
        gen_wh_vec.push_back(s.kwh * 1000.0);
        use_wh_vec.push_back(use_kwh * 1000.0);
    }

    double cap_wh = r.nominal_ah * r.battery_voltage;
    auto soc_proj = project_battery_soc(r.current_soc_pct, cap_wh, gen_wh_vec, use_wh_vec);

    o += "  \"capacity_wh\": " + jdbl(cap_wh, 1) + ",\n";
    o += "  \"measured_avg_w\": " + jdbl(ext_cache->measured_avg_w, 1) + ",\n";
    o += "  \"slots\": [";
    for (size_t i = 0; i < r.slots.size(); ++i) {
        const auto& s = r.slots[i];
        const auto& su = slot_usage[i];
        if (i) o += ",";
        o += "{\"ts\":" + std::to_string(s.timestamp) +
             ",\"kwh\":" + jdbl(s.kwh, 4) +
             ",\"kwh_lo\":" + jdbl(s.kwh_lo, 4) +
             ",\"kwh_hi\":" + jdbl(s.kwh_hi, 4) +
             ",\"cloud\":" + jdbl(s.cloud_cover, 2) +
             ",\"day\":" + std::string(s.daytime ? "true" : "false") +
             ",\"use_kwh\":" + jdbl(su.kwh, 4) +
             ",\"use_lo\":" + jdbl(su.kwh_lo, 4) +
             ",\"use_hi\":" + jdbl(su.kwh_hi, 4) +
             ",\"soc_pct\":" + jdbl(i < soc_proj.size() ? soc_proj[i] : 0, 1) + "}";
    }
    o += "],\n";
    o += "  \"solar_perf_coeff\": " + jdbl(ext_cache->avg_coeff, 3) + ",\n";
    o += "  \"solar_perf_samples\": " + std::to_string(ext_cache->perf_count) + "\n";
    o += "}\n";
    return o;
}

std::string HttpServer::system_health_json(const HealthScoreResult& r) {
    std::string o = "{\n";
    o += "  \"score\": " + std::to_string(r.score) + ",\n";
    o += "  \"battery\": " + jstr(r.battery) + ",\n";
    o += "  \"cells\": " + jstr(r.cells) + ",\n";
    o += "  \"charging\": " + jstr(r.charging) + ",\n";
    o += "  \"temperature\": " + jstr(r.temperature) + ",\n";
    o += "  \"anomalies\": " + jstr(r.anomalies) + "\n";
    o += "}\n";
    return o;
}

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
                                       int poll_interval_s,
                                       const std::string& init_settings,
                                       const std::string& theme) {
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

    std::string html_class = (theme == "light") ? " class=\"light\"" : "";
    o += "<!DOCTYPE html><html lang=\"en\"" + html_class + "><head>\n";
    o += R"HTML(
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0d0d11" media="(prefers-color-scheme:dark)">
<meta name="theme-color" content="#f2f3f7" media="(prefers-color-scheme:light)">
<link rel="prefetch" href="/solar"><link rel="prefetch" href="/settings">
<title>limonitor</title>
<style>
/* ── Dark theme (default) ── */
:root{
  --bg:#0d0d11;--card:#16161c;--border:#2e2e3a;
  --text:#e0e0ea;--muted:#9090a8;--sub:#686880;
  --green:#4ade80;--orange:#fb923c;--blue:#60a5fa;--cyan:#22d3ee;--red:#f87171;--violet:#a78bfa;
  --soc-track:#22222c;--cell-bg:#0f0f16;--alert-bg:#1c0a0a;
  --chart-bg:#0f0f14;--chart-grid:#252530;--chart-tick:#a0a0b8;
  --cv:#4caf50;--ca:#ff9800;--csoc:#4080ff;--csol:#00bcd4
}
/* ── Light theme ── */
html.light{
  --bg:#f2f3f7;--card:#ffffff;--border:#d4d4e0;
  --text:#1a1a2c;--muted:#5a5a72;
  --green:#16a34a;--orange:#c2410c;--blue:#1d4ed8;--cyan:#0891b2;--red:#dc2626;--violet:#7c3aed;
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
.atabs{display:flex;gap:0;margin-bottom:.6rem;border-bottom:1px solid var(--border)}
.atab{padding:.4rem .9rem;font-size:.8rem;color:var(--muted);background:none;border:none;border-bottom:2px solid transparent;cursor:pointer;font-family:inherit}
.atab:hover{color:var(--text)}
.atab.active{color:var(--green);border-bottom-color:var(--green);font-weight:600}
.atab-pane{display:none}
.atab-pane.active{display:block}
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
/* ── Settings modal ── */
.set-modal{position:fixed;inset:0;background:rgba(0,0,0,.5);display:flex;align-items:center;justify-content:center;z-index:1000}
.set-panel{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:1.2rem;min-width:260px}
.set-title{font-size:.9rem;font-weight:600;margin-bottom:.8rem;color:var(--text)}
.set-row{margin-bottom:.6rem;display:flex;align-items:center;gap:.6rem}
.set-row label{min-width:90px;font-size:.8rem;color:var(--muted)}
.set-row select{flex:1;padding:.35rem .5rem;border:1px solid var(--border);border-radius:4px;background:var(--bg);color:var(--text);font-family:inherit;font-size:.8rem}
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
/* ── Tab nav ── */
.tabs{display:flex;gap:0;margin-bottom:1rem;border-bottom:1px solid var(--border)}
.tab{padding:.5rem 1rem;font-size:.8rem;color:var(--muted);background:none;border:none;border-bottom:2px solid transparent;cursor:pointer;font-family:inherit;text-decoration:none}
.tab:hover{color:var(--text)}
.tab.active{color:var(--green);border-bottom-color:var(--green);font-weight:600}
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
</style>
<link rel="prefetch" href="/solar">
</head><body><div class="wrap">
)HTML";

    o += "<nav class=\"tabs\"><a href=\"/\" class=\"tab active\">Dashboard</a><a href=\"/solar\" class=\"tab\">Solar</a><a href=\"/settings\" class=\"tab\">Settings</a></nav>";
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
    o += "<button id=\"set-btn\" class=\"btn\" onclick=\"toggleSettings()\" title=\"Settings\">&#9881;</button>";
    o += "</div></header>\n";
    o += "<div id=\"settings-modal\" class=\"set-modal\" style=\"display:none\">"
         "<div class=\"set-panel\"><div class=\"set-title\">Settings</div>"
         "<div class=\"set-row\"><label>Theme</label><select id=\"set-theme\" onchange=\"applySetting('theme',this.value)\">"
         "<option value=\"dark\">Dark</option><option value=\"light\">Light</option></select></div>"
         "<div class=\"set-row\"><label>Time format</label><select id=\"set-time\" onchange=\"applySetting('time',this.value)\">"
         "<option value=\"24\">24-hour</option><option value=\"12\">12-hour</option></select></div>"
         "<div class=\"set-row\"><label>Chart range</label><select id=\"set-range\" onchange=\"applySetting('range',this.value)\">"
         "<option value=\"0.5\">30 min</option><option value=\"1\">1 hour</option><option value=\"4\">4 hours</option><option value=\"24\">24 hours</option></select></div>"
         "<button class=\"btn\" onclick=\"toggleSettings()\" style=\"margin-top:.5rem\">Close</button></div></div>\n";

    o += "<main class=\"main\">\n";
    o += "<div id=\"sys-status-panel\" class=\"card\" style=\"margin-bottom:.7rem\">"
         "<div class=\"card-title\">System Status</div>"
         "<div id=\"sys-status-lines\" style=\"font-size:.85rem;line-height:1.6\">—</div>"
         "<div id=\"sys-status-alerts\" style=\"margin-top:.5rem\"></div>"
         "</div>\n";

    o += "<div class=\"stats\">\n";

    // Voltage (with trend indicator)
    snprintf(buf, sizeof(buf),
        "<div class=\"stat\"><div class=\"stat-lbl\">Battery Voltage</div>"
        "<div class=\"sv ok\"><span id=\"sv\">%.2f</span><span class=\"u\">V</span>"
        "<span id=\"sv-trend\" style=\"font-size:1rem;margin-left:.2em\"></span></div>"
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
        "<div class=\"sv ok\"><span id=\"ssoc\">%.1f</span><span class=\"u\">%%</span>"
        "<span id=\"ssoc-trend\" style=\"font-size:1rem;margin-left:.2em\"></span></div>"
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
    double rem_wh = s.remaining_ah * (s.total_voltage_v > 1 ? s.total_voltage_v : 51.2);
    double nom_wh = s.nominal_ah * (s.total_voltage_v > 1 ? s.total_voltage_v : 51.2);
    snprintf(buf, sizeof(buf), "%.2f / %.2f Ah (%.0f / %.0f Wh)", s.remaining_ah, s.nominal_ah, rem_wh, nom_wh);
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

    o += "<div class=\"card\"><div class=\"card-title\">System Events</div>"
         "<div id=\"sys-events\" style=\"font-size:.78rem;max-height:140px;overflow-y:auto\">—</div>"
         "</div>\n";

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
    o += "<div class=\"atabs\"><button class=\"atab active\" data-atab=\"battery\">Battery</button>"
         "<button class=\"atab\" data-atab=\"solar\">Solar</button>"
         "<button class=\"atab\" data-atab=\"charging\">Charging</button>"
         "<button class=\"atab\" data-atab=\"system\">System</button></div>\n";

    o += "<div class=\"atab-pane active\" data-atab=\"battery\"><div class=\"acards\">\n";
    // Battery group
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

    o += "<div class=\"acard\"><div class=\"card-title\">Cell Balance</div><table class=\"dt\">\n"
         "<tr><td>Delta</td><td><span id=\"an-cell-delta\">—</span> mV</td></tr>\n"
         "<tr><td>Status</td><td><span id=\"an-cell-bal\">—</span></td></tr>\n"
         "</table></div>\n";

    o += "<div class=\"acard\"><div class=\"card-title\">Battery Temperature</div><table class=\"dt\">\n"
         "<tr><td>T1</td><td><span id=\"an-t1\">—</span></td></tr>\n"
         "<tr><td>T2</td><td><span id=\"an-t2\">—</span></td></tr>\n"
         "<tr><td>Status</td><td><span id=\"an-temp-status\">—</span></td></tr>\n"
         "</table></div>\n";
    o += "<div class=\"acard\"><div class=\"card-title\">Depth of Discharge</div><table class=\"dt\">\n"
         "<tr><td>DoD today</td><td><span id=\"an-dod\">—</span> %</td></tr>\n"
         "<tr><td>Status</td><td><span id=\"an-dod-status\">—</span></td></tr>\n"
         "</table></div>\n";
    o += "<div class=\"acard\"><div class=\"card-title\">Battery Utilization</div><table class=\"dt\">\n"
         "<tr><td>Energy used today</td><td><span id=\"an-util-today\">—</span></td></tr>\n"
         "<tr><td>Energy used this week</td><td><span id=\"an-util-week\">—</span></td></tr>\n"
         "</table></div>\n";
    o += "<div class=\"acard\"><div class=\"card-title\">Battery Stress</div><table class=\"dt\">\n"
         "<tr><td>Stress level</td><td><span id=\"an-stress\">—</span></td></tr>\n"
         "</table></div>\n";
    o += "<div class=\"acard\"><div class=\"card-title\">Runtime Estimates</div><table class=\"dt\">\n"
         "<tr><td>Full → 10%</td><td><span id=\"an-runtime-full\">—</span></td></tr>\n"
         "<tr><td>Current → 10%</td><td><span id=\"an-runtime-now\">—</span></td></tr>\n"
         "<tr><td>24h avg load</td><td><span id=\"an-avg-load-24h\">—</span></td></tr>\n"
         "</table></div>\n";
    o += "<div class=\"acard\"><div class=\"card-title\">Battery Internal Resistance</div><table class=\"dt\">\n"
         "<tr><td>Resistance</td><td><span id=\"resistance-mohm\">—</span> mΩ</td></tr>\n"
         "<tr><td>Trend</td><td><span id=\"resistance-trend\">—</span></td></tr>\n"
         "<tr id=\"resistance-note-row\" style=\"display:none\"><td colspan=\"2\"><span id=\"resistance-note\" class=\"dim\" style=\"font-size:.7rem\">Requires radio TX events</span></td></tr>\n"
         "</table></div>\n";

    o += "</div></div>\n"; // end Battery

    o += "<div class=\"atab-pane\" data-atab=\"solar\"><div class=\"acards\">\n";
    // Solar group
    o += "<div class=\"acard\"><div class=\"card-title\">Solar Input</div><table class=\"dt\">\n"
         "<tr><td>Voltage</td><td><span id=\"an-sol-v\">—</span> V</td></tr>\n"
         "<tr><td>Status</td><td><span id=\"an-sol-status\">—</span></td></tr>\n"
         "<tr><td>Power</td><td><span id=\"an-sol-pwr\">—</span></td></tr>\n"
         "<tr><td>Energy today</td><td><span id=\"an-sol-energy\">—</span></td></tr>\n"
         "</table></div>\n";
    o += "<div class=\"acard\"><div class=\"card-title\">Solar Readiness</div><table class=\"dt\">\n"
         "<tr><td><span id=\"an-solar-ready\">—</span></td></tr>\n"
         "</table></div>\n";
    o += "<div class=\"acard\"><div class=\"card-title\">Solar Simulation</div><table class=\"dt\">\n"
         "<tr><td>Expected today</td><td><span id=\"solar-sim-wh\">—</span></td></tr>\n"
         "<tr><td>Battery projection</td><td><span id=\"solar-sim-bat\">—</span></td></tr>\n"
         "</table></div>\n";
    o += "<div class=\"acard\"><div class=\"card-title\">7-Day Solar</div><table class=\"dt\">\n"
         "<tr><td>Week total</td><td><span id=\"solar-week-kwh\">—</span></td></tr>\n"
         "<tr><td>Days to full</td><td><span id=\"solar-days-full\">—</span></td></tr>\n"
         "<tr><td><a href=\"/solar\" style=\"font-size:.75rem\">Full solar page →</a></td><td></td></tr>\n"
         "</table></div>\n";
    o += "<div class=\"acard\"><div class=\"card-title\">Solar Forecast</div><table class=\"dt\">\n"
         "<tr><td>Tomorrow</td><td><span id=\"solar-forecast-wh\">—</span></td></tr>\n"
         "<tr><td>Cloud cover</td><td><span id=\"solar-forecast-cloud\">—</span></td></tr>\n"
         "<tr><td>Projected battery</td><td><span id=\"solar-forecast-bat\">—</span></td></tr>\n"
         "<tr id=\"solar-forecast-err-row\" style=\"display:none\"><td colspan=\"2\"><span id=\"solar-forecast-err\" class=\"warn\"></span></td></tr>\n"
         "</table></div>\n";

    o += "</div></div>\n"; // end Solar

    o += "<div class=\"atab-pane\" data-atab=\"charging\"><div class=\"acards\">\n";
    // Charging group
    o += "<div class=\"acard\"><div class=\"card-title\">Charging Stage</div><table class=\"dt\">\n"
         "<tr><td>Stage</td><td><span id=\"an-chg-stage\">—</span></td></tr>\n"
         "</table></div>\n";
    o += "<div class=\"acard\"><div class=\"card-title\">Charger Efficiency</div><table class=\"dt\">\n"
         "<tr><td>Efficiency</td><td><span id=\"an-eff\">—</span></td></tr>\n"
         "</table></div>\n";
    o += "<div class=\"acard\"><div class=\"card-title\">Charger Runtime Today</div><table class=\"dt\">\n"
         "<tr><td>Runtime</td><td><span id=\"an-chg-runtime\">—</span></td></tr>\n"
         "</table></div>\n";
    o += "<div class=\"acard\"><div class=\"card-title\">Charge Rate</div><table class=\"dt\">\n"
         "<tr><td>Power</td><td><span id=\"an-chg-rate-w\">—</span></td></tr>\n"
         "<tr><td>Speed</td><td><span id=\"an-chg-rate-pct\">—</span></td></tr>\n"
         "<tr><td>Recent vs earlier</td><td><span id=\"an-chg-rate-compare\">—</span></td></tr>\n"
         "</table></div>\n";
    o += "<div class=\"acard\"><div class=\"card-title\">Charge Status</div><table class=\"dt\">\n"
         "<tr><td><span id=\"an-psu-status\">—</span></td></tr>\n"
         "</table></div>\n";
    o += "</div></div>\n"; // end Charging

    o += "<div class=\"atab-pane\" data-atab=\"system\"><div class=\"acards\">\n";
    // System group
    o += "<div class=\"acard\"><div class=\"card-title\">System Load Profile</div><table class=\"dt\">\n"
         "<tr><td>Average</td><td><span id=\"an-avg-load\">—</span> W</td></tr>\n"
         "<tr><td>Peak</td><td><span id=\"an-peak-load\">—</span> W</td></tr>\n"
         "<tr><td>Idle</td><td><span id=\"an-idle-load\">—</span> W</td></tr>\n"
         "</table></div>\n";

    o += "<div class=\"acard\"><div class=\"card-title\">Monitor Status</div><table class=\"dt\">\n"
         "<tr><td>Uptime</td><td><span id=\"an-uptime\">—</span></td></tr>\n"
         "<tr><td>Last BMS update</td><td><span id=\"an-bms-ago\">—</span></td></tr>\n"
         "<tr><td>Last charger update</td><td><span id=\"an-chg-ago\">—</span></td></tr>\n"
         "</table></div>\n";

    o += "<div class=\"acard\"><div class=\"card-title\">System Health</div><table class=\"dt\">\n"
         "<tr><td>Score</td><td><span id=\"health-score\">—</span> / 100</td></tr>\n"
         "<tr><td>Battery</td><td><span id=\"health-battery\">—</span></td></tr>\n"
         "<tr><td>Cells</td><td><span id=\"health-cells\">—</span></td></tr>\n"
         "<tr><td>Charging</td><td><span id=\"health-charging\">—</span></td></tr>\n"
         "</table></div>\n";

    o += "<div class=\"acard\"><div class=\"card-title\">Anomalies</div>"
         "<div id=\"anomalies-list\" style=\"font-size:.78rem;max-height:140px;overflow-y:auto\">—</div></div>\n";

    o += "</div></div>\n"; // .acards, .atab-pane system

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
<tr><td>Current</td><td>Positive = discharging &nbsp;&middot;&nbsp; Negative = charging</td></tr>
<tr><td>SoC</td><td>State of Charge &mdash; % of rated capacity remaining</td></tr>
<tr><td>Power</td><td>V &times; A &mdash; positive while discharging, negative while charging</td></tr>
<tr><td>Capacity</td><td>Remaining Ah / Nominal (rated) Ah reported by BMS</td></tr>
<tr><td>Est. remaining</td><td>Time to full (charging) or empty (discharging) at current rate</td></tr>
<tr><td>Cell delta</td><td>Spread between weakest and strongest cell. &lt;10 mV excellent, &gt;50 mV needs balancing</td></tr>
<tr><td>Cycles</td><td>Charge cycle count reported by BMS</td></tr>
<tr><td>FETs chg/dchg</td><td>Charge and discharge MOSFET states &mdash; OFF indicates BMS protection triggered</td></tr>
<tr class="sec"><td colspan="2">Charger (EpicPowerGate 2)</td></tr>
<tr><td>Power supply</td><td>Input voltage measured by charger (grid / alternator / shore power)</td></tr>
<tr><td>Solar</td><td>Solar panel open-circuit voltage &mdash; 0 V when no panel connected or no sun</td></tr>
<tr><td>Battery</td><td>Battery voltage and charge current as measured by the charger</td></tr>
<tr><td>Target</td><td>Absorption (bulk) target voltage / max charge current / tail-current stop threshold</td></tr>
<tr><td>PWM</td><td>Charge duty cycle &mdash; 1023 = 100% = full power</td></tr>
<tr><td>Elapsed</td><td>Minutes since the current charge session started</td></tr>
<tr><td>Temp</td><td>Charger PCB temperature sensor (raw ADC value &mdash; not in &deg;C)</td></tr>
<tr class="sec"><td colspan="2">Analytics</td></tr>
<tr><td>Energy Today</td><td>Wh integrated since midnight UTC. Charged = energy put into the battery; Consumed = energy drawn from it; Solar = energy sourced from the solar panel; Net = charged &minus; consumed.</td></tr>
<tr><td>Battery Replacement</td><td>Age computed from the configured purchase date. Estimated health degrades ~2.5&nbsp;%/yr. Replacement window is the range of years until estimated health falls below 80&nbsp;% or age exceeds 8&nbsp;years. A warning appears when either threshold is crossed.</td></tr>
<tr><td>Battery Health</td><td>Once a full discharge cycle (&ge;5&nbsp;Ah swing) is observed, health is shown as usable&nbsp;Ah / rated&nbsp;Ah. Before that, an age-based estimate is shown with an <em>(est)</em> label.</td></tr>
<tr><td>Charging Stage</td><td>Inferred from charger voltage and current trend: <b>Bulk</b> = charging hard below target voltage; <b>Absorption</b> = near target voltage, current falling; <b>Float</b> = maintenance charge; <b>Idle</b> = charger not in a charge state.</td></tr>
<tr><td>Cell Balance</td><td>Max &minus; min cell voltage. &lt;10&nbsp;mV Excellent, 10&ndash;25&nbsp;mV Good, 25&ndash;50&nbsp;mV Warning, &ge;50&nbsp;mV Imbalance (balancing recommended).</td></tr>
<tr><td>Battery Temperature</td><td>T1 and T2 from BMS NTC sensors. &lt;40&nbsp;&deg;C Normal, 40&ndash;50&nbsp;&deg;C Warm, &ge;50&nbsp;&deg;C Warning.</td></tr>
<tr><td>Charger Efficiency</td><td>Ratio of battery voltage to charger input voltage (bat_v / ps_v). Reflects resistive and PWM losses in the charge path.</td></tr>
<tr><td>Charge Status</td><td>PSU limitation: charger in Bulk, PWM high, but voltage plateaus below target for 15+ min. Charge slowdown: observed charge rate (SoC change) has dropped significantly vs. a few minutes earlier — may indicate PSU limit, increased load, or charger throttling.</td></tr>
<tr><td>Solar Input</td><td>Solar panel voltage and estimated power (panel_v &times; charge_current). Energy today is integrated over the day.</td></tr>
<tr><td>Depth of Discharge</td><td>Today&apos;s peak SoC minus today&apos;s trough SoC. &lt;30&nbsp;% Low stress, 30&ndash;70&nbsp;% Normal, &ge;70&nbsp;% High stress (reduces cycle life).</td></tr>
<tr><td>Runtime Estimates</td><td>Time from full charge (100%) or current SoC down to 10% (LiFePO4 cutoff). Prefers battery discharge data; when charging, uses charger power as fallback (est. load when grid drops).</td></tr>
<tr><td>System Load Profile</td><td>Rolling ~1-hour window of discharge-only samples. Average = mean load; Peak = highest seen; Idle = mean of samples below 15&nbsp;W.</td></tr>
<tr class="sec"><td colspan="2">Radio Activity</td></tr>
<tr><td>TX events</td><td>Count of radio transmission events detected in the last 24&nbsp;h based on net battery current spikes above the configured threshold.</td></tr>
<tr><td>Duty cycle</td><td>Total TX time as a percentage of 24&nbsp;h.</td></tr>
<tr><td>Energy used</td><td>Estimated energy consumed by radio transmissions (peak_power &times; duration).</td></tr>
</table></details>
)";

    o += "</main><div class=\"footer\">API: "
         "<a href=\"/api/status\">/api/status</a> &nbsp;"
         "<a href=\"/api/analytics\">/api/analytics</a> &nbsp;"
         "<a href=\"/api/flow\">/api/flow</a> &nbsp;"
         "<a href=\"/api/events\">/api/events</a> &nbsp;"
         "<a href=\"/api/tx_events\">/api/tx_events</a> &nbsp;"
         "<a href=\"/api/anomalies\">/api/anomalies</a> &nbsp;"
         "<a href=\"/api/system_health\">/api/system_health</a> &nbsp;"
         "<a href=\"/api/solar_simulation\">/api/solar_simulation</a> &nbsp;"
         "<a href=\"/api/solar_forecast\">/api/solar_forecast</a> &nbsp;"
         "<a href=\"/api/solar_forecast_week\">/api/solar_forecast_week</a> &nbsp;"
         "<a href=\"/api/solar_performance\">/api/solar_performance</a> &nbsp;"
         "<a href=\"/solar\">/solar</a> &nbsp;"
         "<a href=\"/api/battery/resistance\">/api/battery/resistance</a> &nbsp;"
         "<a href=\"/api/cells\">/api/cells</a> &nbsp;"
         "<a href=\"/api/history\">/api/history</a> &nbsp;"
         "<a href=\"/api/charger\">/api/charger</a> &nbsp;"
         "<a href=\"/metrics\">/metrics</a> (Prometheus)"
         "</div>\n";

    {
        char jshdr[256];
        std::snprintf(jshdr, sizeof(jshdr),
            "<script>\nvar currentH=%.4g,pollIvl=%d,lastUpd=Date.now()\n",
            hours, std::max(1, poll_interval_s));
        o += jshdr;
        if (!init_settings.empty())
            o += "var lmSettings=" + init_settings + ";\n";
        else
            o += "var lmSettings={theme:\"\",time:\"24\",range:\"1\",\"show-extended\":\"0\"};\n";
    }

    o += R"(
function $(id){return document.getElementById(id)}
function fmt(v,d){return(v==null||isNaN(+v))? '\u2014':(+v).toFixed(d)}

var userTZ;try{userTZ=Intl.DateTimeFormat().resolvedOptions().timeZone}catch(e){userTZ=undefined}
function localDateStr(d){return d.getFullYear()+'-'+String(d.getMonth()+1).padStart(2,'0')+'-'+String(d.getDate()).padStart(2,'0')}
function tsToLocal(ts){return new Date(ts*1000)}

function getSetting(k,d){return(lmSettings&&lmSettings[k]!==undefined)?lmSettings[k]:d}
function setSetting(k,v){if(!lmSettings)lmSettings={};lmSettings[k]=v;fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(lmSettings)}).catch(function(){})}
function initSettings(){
  if(!lmSettings)lmSettings={theme:'',time:'24',range:'1','show-extended':'0'}
  var t=getSetting('theme','');if(!t)t=window.matchMedia('(prefers-color-scheme:light)').matches?'light':'dark'
  lmSettings.theme=t;applyTheme(t,false)
  var tf=getSetting('time','24');var se=$('set-time');if(se)se.value=tf
  var r=getSetting('range','');if(r&&currentH){var sh=parseFloat(r);if(!isNaN(sh)&&[0.5,1,4,24].indexOf(sh)>=0)currentH=sh}
  var sr=$('set-range');if(sr)sr.value=String(currentH)
  var st=$('set-theme');if(st)st.value=t
}
function applyTheme(t,redraw){
  document.documentElement.classList.toggle('light',t==='light')
  var b=$('thm-btn');if(b)b.textContent=t==='light'?'\u263d':'\u2600'
  if(!lmSettings)lmSettings={};lmSettings.theme=t;fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(lmSettings)}).catch(function(){})
  var st=$('set-theme');if(st)st.value=t
  if(redraw)loadCharts()
}
function toggleTheme(){applyTheme(document.documentElement.classList.contains('light')?'dark':'light',true)}
function toggleSettings(){
  var m=$('settings-modal');if(!m)return
  m.style.display=m.style.display==='none'?'flex':'none'
  if(m.style.display==='flex'){var st=$('set-theme');if(st)st.value=getSetting('theme','dark');var se=$('set-time');if(se)se.value=getSetting('time','24');var sr=$('set-range');if(sr)sr.value=String(currentH)}
}
function applySetting(k,v){
  if(!lmSettings)lmSettings={};lmSettings[k]=v
  var toSend={theme:lmSettings.theme||'',time:lmSettings.time||'24',range:lmSettings.range||'1','show-extended':lmSettings['show-extended']||'0',locale:lmSettings.locale||''}
  Object.keys(lmSettings).forEach(function(x){toSend[x]=lmSettings[x]})
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(toSend)}).catch(function(){})
  if(k==='theme'){applyTheme(v,true)}
  else if(k==='time'){loadCharts()}
  else if(k==='range'){var h=parseFloat(v);if(!isNaN(h))switchRange(h)}
}
function initTheme(){
  var t=getSetting('theme','');if(!t)t=window.matchMedia('(prefers-color-scheme:light)').matches?'light':'dark'
  applyTheme(t,false)
}
function initAtabs(){
  var btns=document.querySelectorAll('.atab'),panes=document.querySelectorAll('.atab-pane')
  btns.forEach(function(b){
    b.addEventListener('click',function(){
      var t=b.getAttribute('data-atab')
      btns.forEach(function(x){x.classList.toggle('active',x===b)})
      panes.forEach(function(p){p.classList.toggle('active',p.getAttribute('data-atab')===t)})
    })
  })
}
(function(){var n=document.querySelectorAll('nav a[href^="/"]');for(var i=0;i<n.length;i++){n[i].addEventListener('click',function(e){if(e.ctrlKey||e.metaKey||e.shiftKey)return;var h=this.getAttribute('href');if(!h)return;e.preventDefault();location.href=h})}})();
initSettings()
initAtabs()

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
  var v=d.voltage_v||0;if(v<1)v=51.2
  var remWh=d.remaining_ah*v,nomWh=d.nominal_ah*v
  $('ssoc-sub').textContent=fmt(d.remaining_ah,2)+'\xa0/\xa0'+fmt(d.nominal_ah,2)+'\xa0Ah\xa0('+fmt(remWh,0)+'\xa0/\xa0'+fmt(nomWh,0)+'\xa0Wh)'
  $('spw').textContent=fmt(Math.abs(d.power_w),1)
  var rd=a<-0.01?'to full':'to empty'
  $('spw-sub').textContent=fmt(d.time_remaining_h,1)+'\xa0h\xa0'+rd
  $('bat-cap').textContent=fmt(d.remaining_ah,2)+' / '+fmt(d.nominal_ah,2)+' Ah ('+fmt(remWh,0)+' / '+fmt(nomWh,0)+' Wh)'
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
  function sc(id,cls,v){var e=$(id);if(e){e.textContent=v;e.className=cls;e.removeAttribute('title')}}
  function sct(id,cls,v,title){var e=$(id);if(e){e.textContent=v;e.className=cls;e.title=title||''}}
  sv('an-e-chg',   fmt(d.energy_charged_today_wh,0))
  sv('an-e-dis',   fmt(d.energy_discharged_today_wh,0))
  sv('an-e-sol',   fmt(d.solar_energy_today_wh,0))
  var net=d.net_energy_today_wh||0
  sc('an-e-net',net>=0?'ok':'warn',(net>=0?'+':'')+net.toFixed(0))
  sv('an-bat-age', d.battery_age_years>=0 ? d.battery_age_years.toFixed(1)+' yr' : '—')
  var hp=d.battery_health_pct
  var hpTip=hp>=0?(hp<80?hp.toFixed(1)+'% — consider replacement':hp.toFixed(1)+'% (age/capacity est.)'):''
  sct('an-bat-health', hp<0?'dim':hp<80?'err':hp<90?'warn':'ok',
     hp>=0 ? hp.toFixed(1)+'%' : '—', hpTip)
  var rl=d.years_remaining_low,rh=d.years_remaining_high
  sv('an-bat-repl', rl>=0&&rh>=0 ? rl.toFixed(0)+'\u2013'+rh.toFixed(0)+' yr remaining' : '—')
  var wr=$('an-bat-warn-row')
  if(wr)wr.style.display=d.battery_replace_warn?'':'none'
  var stg=d.charging_stage||'—'
  var stgcls=stg==='Bulk'?'ok':stg==='Absorption'?'ok':stg==='Float'?'dim':'dim'
  var stgTip=stg==='Bulk'?'Charging below target voltage; full current.':
    stg==='Absorption'?'Near target voltage; current tapering off.':
    stg==='Float'?'Maintenance charge; battery full.':
    stg==='Idle'?'Charger not in active charge state.':''
  sct('an-chg-stage',stgcls,stg,stgTip)
  // Prefer observed capacity health; fall back to age-based estimate until a
  // full discharge cycle has been seen.
  var ch=d.capacity_health_pct
  var capObserved=ch>=0
  var dispH=capObserved?ch:(d.battery_health_pct>=0?d.battery_health_pct:-1)
  var chTip=dispH>=0?(capObserved?dispH.toFixed(1)+'% (observed)':dispH.toFixed(1)+'% (age est., no full discharge yet)'):''
  sct('an-cap-health', dispH<0?'dim':dispH<80?'err':dispH<90?'warn':'ok',
     dispH>=0 ? dispH.toFixed(1)+'%'+(capObserved?'':' (est)') : '—', chTip)
  // Est. capacity: observed value, or estimate from health × rated
  var estAh=d.usable_capacity_ah>0 ? d.usable_capacity_ah
            : (dispH>=0&&d.rated_capacity_ah>0 ? dispH/100*d.rated_capacity_ah : -1)
  sv('an-cap-usable', estAh>0 ? estAh.toFixed(1)+' Ah'+(capObserved?'':' (est)') : '—')
  sv('an-cap-rated',  d.rated_capacity_ah>0  ? d.rated_capacity_ah.toFixed(1)+' Ah'  : '—')
  var dm=d.cell_delta_mv||0
  var dmTip=dm>=50?'Imbalance: '+dm.toFixed(1)+' mV (≥50)':dm>=25?'Warning: '+dm.toFixed(1)+' mV (25–50)':dm>=10?'Good: '+dm.toFixed(1)+' mV (10–25)':'Excellent: '+dm.toFixed(1)+' mV (<10)'
  sct('an-cell-delta', dm>=50?'err':dm>=25?'warn':'ok', dm.toFixed(1), dmTip)
  var bs=d.cell_balance_status||'—'
  var bsTip=bs==='Excellent'?'Δ '+dm.toFixed(1)+' mV (<10)':
    bs==='Good'?'Δ '+dm.toFixed(1)+' mV (10–25)':
    bs==='Warning'?'Δ '+dm.toFixed(1)+' mV (25–50)':
    bs==='Imbalance'?'Δ '+dm.toFixed(1)+' mV (≥50)':''
  sct('an-cell-bal', bs==='Excellent'||bs==='Good'?'ok':bs==='Warning'?'warn':bs==='Imbalance'?'err':'dim', bs, bsTip)
  if(d.temp_valid){
    sv('an-t1', d.temp1_c.toFixed(1)+'\xb0C')
    sv('an-t2', d.temp2_c.toFixed(1)+'\xb0C')
    var ts=d.temp_status||'—'
    var tsTip=ts==='Normal'?'T1 '+d.temp1_c.toFixed(1)+'°C, T2 '+d.temp2_c.toFixed(1)+'°C':
      ts==='Warm'?'T1 '+d.temp1_c.toFixed(1)+'°C, T2 '+d.temp2_c.toFixed(1)+'°C (40–50)':
      ts==='Warning'?'T1 '+d.temp1_c.toFixed(1)+'°C, T2 '+d.temp2_c.toFixed(1)+'°C (≥50)':''
    sct('an-temp-status', ts==='Normal'?'ok':ts==='Warm'?'warn':'err', ts, tsTip)
  }
  if(d.efficiency_valid&&d.charger_efficiency>=0){
    var eff=d.charger_efficiency*100
    var effTip=eff<80?eff.toFixed(1)+'% — check for losses':eff.toFixed(1)+'% output/input'
    sct('an-eff', eff<80?'warn':'ok', eff.toFixed(1)+'%', effTip)
  }
  sv('an-sol-v', fmt(d.solar_voltage_v,2))
  var solTip=d.solar_active?fmt(d.solar_voltage_v,1)+' V — contributing to charge':'No solar voltage'
  sct('an-sol-status', d.solar_active?'ok':'dim', d.solar_active?'Active':'Inactive', solTip)
  sv('an-sol-pwr',    d.solar_active&&d.solar_power_w>0 ? fmt(d.solar_power_w,1)+' W' : '—')
  sv('an-sol-energy', d.solar_energy_today_wh>0 ? fmt(d.solar_energy_today_wh,0)+' Wh' : '—')
  var dod=d.depth_of_discharge_pct||0
  sc('an-dod', dod>=70?'err':dod>=30?'warn':'ok', dod.toFixed(1))
  var ds=d.dod_status||'—'
  var dsTip=ds==='Low stress'?dod.toFixed(1)+'% SoC swing (<30%)':
    ds==='Normal'?dod.toFixed(1)+'% SoC swing (30–70%)':
    ds==='High stress'?dod.toFixed(1)+'% SoC swing (≥70%)':''
  sct('an-dod-status', ds==='Low stress'?'ok':ds==='Normal'?'':'err', ds, dsTip)
  sv('an-avg-load',  fmt(d.avg_load_watts,1))
  sv('an-peak-load', fmt(d.peak_load_watts,1))
  sv('an-idle-load', fmt(d.idle_load_watts,1))
  var trend=d.voltage_trend||''
  var te=$('sv-trend');if(te){
    te.textContent=trend==='up'?'\u2191':trend==='down'?'\u2193':''
    te.title=trend==='up'?'Voltage rising':trend==='down'?'Voltage falling':trend==='stable'?'Voltage steady':''
  }
  var socTrend=d.soc_trend||''
  var socRate=d.soc_rate_pct_per_h||0
  var ste=$('ssoc-trend');if(ste){
    ste.textContent=socTrend==='up'?'\u2191':socTrend==='down'?'\u2193':''
    ste.title=socTrend==='up'?'Increasing: +'+socRate.toFixed(2)+'%/h over last ~1 min':
      socTrend==='down'?'Decreasing: '+socRate.toFixed(2)+'%/h over last ~1 min':
      socTrend==='stable'?'Steady: '+socRate.toFixed(2)+'%/h (no significant change)':''
  }
  var sl=$('sys-status-lines');if(sl)sl.innerHTML=(d.system_status||'\u2014').replace(/\u00b7 /g,'<br>')
  var sa=$('sys-status-alerts');var pan=$('sys-status-panel');if(sa&&pan){
    var alerts=d.health_alerts||[]
    if(alerts.length){sa.innerHTML='<span class=\"warn\">'+alerts.join(' \u00b7 ')+'</span>';sa.style.display='';pan.style.borderColor='var(--orange)'}
    else{sa.innerHTML='';sa.style.display='none';pan.style.borderColor=''}
  }
  sv('an-util-today', d.energy_used_today_wh!=null?fmt(d.energy_used_today_wh,0)+' Wh':'—')
  sv('an-util-week',  d.energy_used_week_wh!=null?(d.energy_used_week_wh>=1000?(d.energy_used_week_wh/1000).toFixed(1)+' kWh':fmt(d.energy_used_week_wh,0)+' Wh'):'—')
  sv('an-solar-ready',d.solar_readiness||'—')
  var rt=d.charger_runtime_today
  if(rt!=null&&rt>=0){var h=Math.floor(rt/3600),m=Math.floor((rt%3600)/60);sv('an-chg-runtime',h+'h '+m+'m')}
  else sv('an-chg-runtime','—')
  sv('an-chg-rate-w',  d.charge_rate_w>0?('+'+fmt(d.charge_rate_w,0)+' W'):'—')
  sv('an-chg-rate-pct',d.charge_rate_pct_per_h>0?fmt(d.charge_rate_pct_per_h,1)+'% per hour':'—')
  var rec=d.charge_rate_recent_pct_per_h,base=d.charge_rate_baseline_pct_per_h
  var compEl=$('an-chg-rate-compare')
  if(compEl){
    if(rec!=null&&base!=null&&base>0.01){
      compEl.textContent=fmt(rec,2)+' vs '+fmt(base,2)+' %/h'
      compEl.title='Recent (last ~2 min) vs earlier (~4 min ago). Declining may indicate PSU limit or load.'
    }else compEl.textContent='—'
  }
  var psuW=d.psu_limited_warning||''
  var slowW=d.charge_slowdown_warning||''
  var psuEl=$('an-psu-status')
  if(psuEl){
    if(slowW){psuEl.innerHTML='<span class=\"warn\">Charge rate slowing</span>';psuEl.title=slowW}
    else if(psuW){psuEl.innerHTML='<span class=\"warn\">PSU may be limiting</span>';psuEl.title=psuW}
    else{psuEl.innerHTML='<span class=\"ok\">Normal</span>';psuEl.title='Charger reaching target voltage'}
  }
  var st=d.battery_stress||'—'
  var dod=d.depth_of_discharge_pct||0
  var mxT=Math.max(d.temp1_c||0,d.temp2_c||0)
  var tempValid=d.temp_valid
  var stTip=''
  if(st==='high'){
    var reasons=[]
    if(dod>=50)reasons.push('DoD '+dod.toFixed(0)+'% today (≥50% triggers high)')
    if(tempValid&&mxT>=45)reasons.push('temperature '+mxT.toFixed(0)+'°C (≥45°C triggers high)')
    stTip='High: '+reasons.join(', ')
  }else if(st==='moderate'){
    var reasons=[]
    if(dod>=30&&dod<50)reasons.push('DoD '+dod.toFixed(0)+'% today (30–50% range)')
    if(tempValid&&mxT>=40&&mxT<45)reasons.push('temperature '+mxT.toFixed(0)+'°C (40–45°C range)')
    stTip=reasons.length?'Moderate: '+reasons.join(', '):'DoD '+dod.toFixed(0)+'%, temp '+mxT.toFixed(0)+'°C'
  }else if(st==='low'){
    stTip='DoD '+dod.toFixed(0)+'%'+(tempValid?', temp '+mxT.toFixed(0)+'°C':'')
  }
  sct('an-stress',st==='low'?'ok':st==='moderate'?'warn':st==='high'?'err':'dim',st,stTip)
  var rf=d.runtime_from_full_h,rn=d.runtime_from_current_h,avg24=d.avg_discharge_24h_w
  var excF=d.runtime_full_exceeds_cap,excN=d.runtime_current_exceeds_cap
  sv('an-runtime-full',rf>0?(excF?'> 1000 h':fmt(rf,1)+' h'):'—')
  sv('an-runtime-now', rn>0?(excN?'> 1000 h':fmt(rn,1)+' h'):'—')
  sv('an-avg-load-24h',avg24>0?fmt(avg24,1)+' W':'—')
  var a24=$('an-avg-load-24h');if(a24)a24.title=d.runtime_from_historical?'7-day avg load from DB (on grid). Battery-only estimate.':d.runtime_from_charger?'From charger power when charging (est. load when grid drops).':'Load used for runtime. From 24h discharge or 1h profile (BMS+charger).'
  var rtTip=d.runtime_from_historical?'Battery-only estimate from 7-day avg load (on grid). 10% = LiFePO4 cutoff.':d.runtime_from_charger?'From charger power when charging (est. load when grid drops). 10% = LiFePO4 cutoff.':'24h discharge or 1h load profile. When charging, uses charger power as fallback. 10% = LiFePO4 cutoff.'
  var rfe=$('an-runtime-full');if(rfe)rfe.title=rtTip
  var rne=$('an-runtime-now');if(rne)rne.title=rtTip
  if(d.uptime_sec!=null){var u=d.uptime_sec,days=Math.floor(u/86400);sv('an-uptime',days>0?days+' days':Math.floor(u/3600)+'h')}
  else sv('an-uptime','—')
  if(d.last_bms_update_ago_sec!=null){var b=d.last_bms_update_ago_sec;sv('an-bms-ago',b<60?b+' s ago':b<3600?Math.floor(b/60)+' m ago':'—')}
  else sv('an-bms-ago','—')
  if(d.last_charger_update_ago_sec!=null){var c=d.last_charger_update_ago_sec;sv('an-chg-ago',c<60?c+' s ago':c<3600?Math.floor(c/60)+' m ago':'—')}
  else sv('an-chg-ago','—')
}).catch(function(){})}
setInterval(upAnalytics,5000); upAnalytics()

function upHealth(){fetch('/api/system_health').then(function(r){return r.json()}).then(function(d){
  var e=function(id){return document.getElementById(id)}
  if(e('health-score'))e('health-score').textContent=d.score
  if(e('health-battery'))e('health-battery').textContent=d.battery||'—'
  if(e('health-cells'))e('health-cells').textContent=d.cells||'—'
  if(e('health-charging'))e('health-charging').textContent=d.charging||'—'
}).catch(function(){})}
function upSolarWeek(){fetch('/api/solar_forecast_week').then(function(r){return r.json()}).then(function(d){
  var e=function(id){return document.getElementById(id)}
  if(e('solar-week-kwh'))e('solar-week-kwh').textContent=d.valid?d.week_total_kwh.toFixed(1)+' kWh':'—'
  if(e('solar-days-full'))e('solar-days-full').textContent=d.valid&&d.days_to_full>0?d.days_to_full:'—'
}).catch(function(){})}
function upSolarSim(){fetch('/api/solar_simulation').then(function(r){return r.json()}).then(function(d){
  var e=function(id){return document.getElementById(id)}
  if(e('solar-sim-wh'))e('solar-sim-wh').textContent=d.expected_today_wh>0?(d.expected_today_wh/1000).toFixed(2)+' kWh':'—'
  if(e('solar-sim-bat'))e('solar-sim-bat').textContent=d.battery_projection||'—'
}).catch(function(){})}
function upSolarForecast(){fetch('/api/solar_forecast').then(function(r){return r.json()}).then(function(d){
  var e=function(id){return document.getElementById(id)}
  if(e('solar-forecast-wh'))e('solar-forecast-wh').textContent=d.valid?(d.tomorrow_generation_wh/1000).toFixed(1)+' kWh':'—'
  if(e('solar-forecast-cloud'))e('solar-forecast-cloud').textContent=d.valid?Math.round(d.cloud_cover*100)+'%':'—'
  if(e('solar-forecast-bat'))e('solar-forecast-bat').textContent=d.valid?(d.expected_battery_state||'—'):'—'
  var errRow=e('solar-forecast-err-row'),errEl=e('solar-forecast-err')
  if(errRow&&errEl){if(d.error&&!d.valid){errEl.textContent=d.error;errRow.style.display=''}else{errEl.textContent='';errRow.style.display='none'}}
}).catch(function(){})}
function upResistance(){fetch('/api/battery/resistance').then(function(r){return r.json()}).then(function(d){
  var e=function(id){return document.getElementById(id)}
  var hasData=d.internal_resistance_milliohms>0
  if(e('resistance-mohm'))e('resistance-mohm').textContent=hasData?d.internal_resistance_milliohms.toFixed(1):'—'
  if(e('resistance-trend'))e('resistance-trend').textContent=d.trend||'—'
  var noteRow=e('resistance-note-row');if(noteRow)noteRow.style.display=hasData?'none':''
}).catch(function(){})}
function upAnomalies(){fetch('/api/anomalies').then(function(r){return r.json()}).then(function(d){
  var el=document.getElementById('anomalies-list');if(!el)return
  var a=d.anomalies||[]
  if(!a.length){el.textContent='—';return}
  el.innerHTML=a.slice(-5).reverse().map(function(x){
    var msg=x.message||x.type
    return '<div class=\"warn\" style=\"margin-bottom:.3rem\">⚠ '+msg.replace(/\n/g,'<br>')+'</div>'
  }).join('')
}).catch(function(){})}
setInterval(upHealth,10000); setInterval(upSolarWeek,300000); setInterval(upSolarSim,30000); setInterval(upSolarForecast,60000); setInterval(upResistance,10000); setInterval(upAnomalies,10000)
upHealth(); upSolarWeek(); upSolarSim(); upSolarForecast(); upResistance(); upAnomalies()

function upEvents(){fetch('/api/events?n=30').then(function(r){return r.json()}).then(function(evs){
  var el=$('sys-events');if(!el)return
  if(!evs||!evs.length){el.textContent='—';return}
  el.innerHTML=evs.map(function(e){return '<div>'+e.time+' '+e.message+'</div>'}).join('')
}).catch(function(){})}
setInterval(upEvents,5000); upEvents()

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

function fmtTime(h,m){
  var pad=function(n){return n.toString().padStart(2,'0')}
  if(getSetting('time','24')==='12'){var h12=h%12;if(h12===0)h12=12;return h12+':'+pad(m)+(h<12?' AM':' PM')}
  return pad(h)+':'+pad(m)
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
    var d=new Date(t),h=d.getHours(),m=d.getMinutes(),tm=fmtTime(h,m)
    var ds=(d.getMonth()+1)+'/'+d.getDate()
    if(multi&&h===0&&m===0) return ds
    if(multi&&prevDay!==d.toDateString()) return ds+' '+tm
    return tm
  }
  function fmtStart(t,short){
    var d=new Date(t),h=d.getHours(),m=d.getMinutes(),tm=fmtTime(h,m)
    if(short) return multi ? (d.getMonth()+1)+'/'+d.getDate() : tm
    return multi ? (d.getMonth()+1)+'/'+d.getDate()+' '+tm : tm
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
setInterval(loadCharts,Math.max(pollIvl*1000,10000));
(function(){var rt;window.addEventListener('resize',function(){clearTimeout(rt);rt=setTimeout(loadCharts,150)})})()
</script></div></body></html>)";

    return o;
}

std::string HttpServer::html_solar_page(const std::string& init_settings,
                                        const std::string& theme) {
    std::string o;
    o.reserve(16000);
    std::string html_class = (theme == "light") ? " class=\"light\"" : "";
    o += "<!DOCTYPE html><html lang=\"en\"" + html_class + "><head>\n";
    o += R"HTML(
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Solar Forecast — limonitor</title>
<style>
:root{--bg:#0d0d11;--card:#16161c;--border:#2e2e3a;--text:#e0e0ea;--muted:#9090a8;--green:#4ade80;--orange:#fb923c;--cyan:#22d3ee}
html.light{--bg:#f2f3f7;--card:#fff;--border:#d4d4e0;--text:#1a1a2c;--muted:#5a5a72;--green:#16a34a;--orange:#c2410c;--cyan:#0891b2}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'SF Mono',Menlo,Consolas,monospace;background:var(--bg);color:var(--text);font-size:13px;padding:1.4rem}
a{color:var(--green);text-decoration:none}a:hover{text-decoration:underline}
h1{font-size:1.5rem;color:var(--green);margin-bottom:.5rem}
.sub{font-size:.72rem;color:var(--muted);margin-bottom:1rem}
.card{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:1rem;margin-bottom:.7rem}
.card-title{font-size:.6rem;text-transform:uppercase;letter-spacing:.14em;color:var(--muted);margin-bottom:.6rem}
.stats{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:.6rem;margin-bottom:1rem}
.stat{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:.8rem}
.stat-best .stat-val{min-width:0;overflow:hidden;text-overflow:ellipsis;font-size:1rem}
.stat-lbl{font-size:.6rem;color:var(--muted);text-transform:uppercase}
.stat-val{font-size:1.4rem;font-weight:700;color:var(--green);margin-top:.2rem}
.stat[title]{cursor:help}
th[title]{cursor:help;text-decoration:underline dotted var(--muted);text-underline-offset:3px}
td[title]{cursor:help}
table{width:100%;border-collapse:collapse;font-size:.85rem}
th,td{padding:.4rem .5rem;text-align:left;border-bottom:1px solid var(--border)}
th{color:var(--muted);font-weight:500}
.ok{color:var(--green)}.warn{color:var(--orange)}.dim{color:var(--muted)}
.footer{font-size:.68rem;color:var(--muted);margin-top:1rem}
.tabs{display:flex;gap:0;margin-bottom:1rem;border-bottom:1px solid var(--border)}
.tab{padding:.5rem 1rem;font-size:.8rem;color:var(--muted);background:none;border:none;border-bottom:2px solid transparent;cursor:pointer;font-family:inherit;text-decoration:none}
.tab:hover{color:var(--text)}
.tab.active{color:var(--green);border-bottom-color:var(--green);font-weight:600}
.theme-wrap{display:flex;align-items:center;gap:.5rem;margin-bottom:1rem}
.btn{background:var(--card);border:1px solid var(--border);color:var(--text);padding:.4rem .7rem;border-radius:6px;cursor:pointer;font-family:inherit;font-size:.85rem}
.btn:hover{background:var(--border)}
.set-modal{position:fixed;inset:0;background:rgba(0,0,0,.5);display:flex;align-items:center;justify-content:center;z-index:1000}
.set-panel{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:1.2rem;min-width:260px}
.set-title{font-size:.9rem;font-weight:600;margin-bottom:.8rem;color:var(--text)}
.set-row{margin-bottom:.6rem;display:flex;align-items:center;gap:.6rem}
.set-row label{min-width:90px;font-size:.8rem;color:var(--muted)}
.set-row select{flex:1;padding:.35rem .5rem;border:1px solid var(--border);border-radius:4px;background:var(--bg);color:var(--text);font-family:inherit;font-size:.8rem}
.chart-tabs{display:flex;gap:0;border-bottom:1px solid var(--border);margin-bottom:.5rem}
.chart-tab{padding:.3rem .7rem;font-size:.72rem;color:var(--muted);background:none;border:none;border-bottom:2px solid transparent;cursor:pointer;font-family:inherit}
.chart-tab:hover{color:var(--text)}
.chart-tab.active{color:var(--green);border-bottom-color:var(--green);font-weight:600}
.chart-svg{width:100%;display:block;background:var(--bg);border-radius:4px;overflow:visible}
.recovery-ci{font-size:.75rem;color:var(--muted)}
td .rc-main{font-weight:600}
.chart-tooltip{position:absolute;pointer-events:none;background:var(--card);border:1px solid var(--border);border-radius:6px;padding:.4rem .6rem;font-size:.72rem;line-height:1.4;z-index:100;white-space:nowrap;box-shadow:0 4px 12px rgba(0,0,0,.3)}
.chart-tooltip .tt-date{font-weight:600;color:var(--text);margin-bottom:2px}
.chart-tooltip .tt-row{color:var(--muted)}
.chart-tooltip .tt-val{color:var(--green);font-weight:600}
.chart-wrap{position:relative}
.chart-legend{display:flex;flex-wrap:wrap;gap:.3rem .9rem;padding:.5rem .2rem 0;font-size:.72rem;font-family:inherit}
.chart-legend .lg{display:inline-flex;align-items:center;gap:.35rem;cursor:help;position:relative;color:var(--muted)}
.chart-legend .lg:hover{color:var(--text)}
.chart-legend .lg .swatch{width:18px;height:2px;flex-shrink:0}
.chart-legend .lg .swatch-band{width:14px;height:8px;border-radius:2px;flex-shrink:0}
.chart-legend .lg .dot{width:6px;height:6px;border-radius:50%;flex-shrink:0;margin-left:-10px}
.chart-legend .lg .tip{display:none;position:absolute;bottom:calc(100% + 6px);left:0;background:var(--card);border:1px solid var(--border);border-radius:6px;padding:.4rem .6rem;font-size:.68rem;line-height:1.35;white-space:normal;width:260px;z-index:200;box-shadow:0 4px 12px rgba(0,0,0,.3);color:var(--text);pointer-events:none}
.chart-legend .lg:hover .tip{display:block}
</style>
<link rel="prefetch" href="/"><link rel="prefetch" href="/settings">
</head><body>
)HTML";
    o += "<nav class=\"tabs\"><a href=\"/\" class=\"tab\">Dashboard</a><a href=\"/solar\" class=\"tab active\">Solar</a><a href=\"/settings\" class=\"tab\">Settings</a>";
    o += "<button id=\"thm-btn\" class=\"btn\" onclick=\"toggleTheme()\" title=\"Toggle theme\" style=\"margin-left:auto\">&#9788;</button>";
    o += "<button id=\"set-btn\" class=\"btn\" onclick=\"toggleSettings()\" title=\"Settings\">&#9881;</button></nav>";
    o += "<div id=\"settings-modal\" class=\"set-modal\" style=\"display:none\">"
         "<div class=\"set-panel\"><div class=\"set-title\">Settings</div>"
         "<div class=\"set-row\"><label>Theme</label><select id=\"set-theme\" onchange=\"applySetting('theme',this.value)\">"
         "<option value=\"dark\">Dark</option><option value=\"light\">Light</option></select></div>"
         "<div class=\"set-row\"><label>Time format</label><select id=\"set-time\" onchange=\"applySetting('time',this.value)\">"
         "<option value=\"24\">24-hour</option><option value=\"12\">12-hour</option></select></div>"
         "<div class=\"set-row\"><label>Locale</label><select id=\"set-locale\" onchange=\"applySetting('locale',this.value)\">"
         "<option value=\"auto\">Auto (browser)</option><option value=\"en-US\">English (US)</option><option value=\"en-GB\">English (UK)</option><option value=\"de-DE\">Deutsch</option><option value=\"fr-FR\">Français</option><option value=\"es-ES\">Español</option></select></div>"
         "<button class=\"btn\" onclick=\"toggleSettings()\" style=\"margin-top:.5rem\">Close</button></div></div>";
    o += "<h1>Solar Forecast</h1>";
    o += "<p class=\"sub\">";
    o += "<a href=\"/api/solar_forecast_week\">/api/solar_forecast_week</a> &nbsp;"
         "<a href=\"/api/solar_simulation\">/api/solar_simulation</a> &nbsp;"
         "<a href=\"/api/solar_forecast\">/api/solar_forecast</a> &nbsp;"
         "<a href=\"/api/solar_performance\">/api/solar_performance</a></p>";

    o += "<div class=\"stats\" id=\"solar-stats\">";
    o += "<div class=\"stat\" title=\"Total nominal panel output for the 7-day forecast period\"><div class=\"stat-lbl\">Week total</div><div class=\"stat-val\" id=\"week-kwh\">—</div><div class=\"stat-lbl\">kilowatt-hours</div></div>";
    o += "<div class=\"stat\" title=\"Estimated days to reach full charge from solar alone\"><div class=\"stat-lbl\">Days to full</div><div class=\"stat-val\" id=\"days-full\">—</div><div class=\"stat-lbl\">solar only</div></div>";
    o += "<div class=\"stat stat-best\" title=\"Day with the highest expected solar yield\"><div class=\"stat-lbl\">Best day</div><div class=\"stat-val\" id=\"best-day\">—</div><div class=\"stat-lbl\">highest yield</div></div>";
    o += "<div class=\"stat\" title=\"Recent panel performance vs theoretical maximum, based on measured data\"><div class=\"stat-lbl\">Panel efficiency</div><div class=\"stat-val\" id=\"solar-coeff\">—</div><div class=\"stat-lbl\" id=\"solar-coeff-detail\">coefficient</div></div>";
    o += "</div>";
    o += "<div style=\"margin:.5rem 0 .2rem;display:flex;align-items:center;gap:.6rem\">"
         "<label style=\"font-size:.82rem;color:var(--text);cursor:pointer;display:flex;align-items:center;gap:.35rem\" title=\"Model real-world charge acceptance. Uses historical charge controller data when available, otherwise a default LiFePO4 taper curve. Affects recovery estimates and chart projections.\">"
         "<input type=\"checkbox\" id=\"show-realistic\" onchange=\"toggleRealistic()\"> Realistic mode</label>"
         "<span id=\"realistic-badge\" style=\"font-size:.68rem;color:var(--muted);display:none\"></span></div>";

    o += "<div class=\"card\"><div class=\"card-title\" title=\"Per-day breakdown of expected solar generation, battery recovery, and optimal charging windows\">Daily Forecast</div>";
    o += "<div style=\"margin-bottom:.5rem\"><label style=\"font-size:.8rem;color:var(--muted)\" title=\"Extend forecast to 16 days using the long-range weather API (lower accuracy)\"><input type=\"checkbox\" id=\"show-extended\" onchange=\"applySetting('show-extended',this.checked?'1':'0');renderDaily();renderActiveChart()\"> Show extended (6–16 days, from 16-day API if available)</label></div>";
    o += "<div style=\"overflow-x:auto\"><table><thead><tr>"
         "<th></th>"
         "<th>Date</th>"
         "<th title=\"Nominal panel output (kWh) at rated capacity, adjusted for weather\">Energy</th>"
         "<th title=\"Average cloud cover from weather forecast\">Cloud</th>"
         "<th title=\"Effective sun hours, weighted by cloud cover\">Sun hrs</th>"
         "<th title=\"Energy the battery can accept. In Realistic mode, accounts for charge taper at high SoC\">Recovery (95% CI)</th>"
         "<th title=\"Consecutive daytime hours with the clearest skies\">Optimal window</th>"
         "</tr></thead>";
    o += "<tbody id=\"daily-table\"><tr><td colspan=\"7\" class=\"dim\">Loading…</td></tr></tbody></table></div></div>";

    o += "<div class=\"card\"><div class=\"card-title\">Solar Energy Forecast</div>";
    o += "<div style=\"display:flex;align-items:center;gap:.5rem;flex-wrap:wrap\">"
         "<div class=\"chart-tabs\" id=\"solar-view-tabs\" style=\"margin-bottom:0\">"
         "<button class=\"chart-tab active\" data-view=\"solar\" onclick=\"switchChartView('solar')\" title=\"Projected solar generation and cumulative energy\">Solar Forecast</button>"
         "<button class=\"chart-tab\" data-view=\"balance\" onclick=\"switchChartView('balance')\" title=\"Daily generation vs estimated consumption\">Daily Balance</button></div>"
         "<span style=\"width:1px;height:16px;background:var(--border);margin:0 .1rem\"></span>"
         "<div class=\"chart-tabs\" id=\"solar-unit-tabs\" style=\"margin-bottom:0\">"
         "<button class=\"chart-tab active\" data-unit=\"kwh\" onclick=\"switchChartUnit('kwh')\" title=\"Kilowatt-hours\">kWh</button>"
         "<button class=\"chart-tab\" data-unit=\"ah\" onclick=\"switchChartUnit('ah')\" title=\"Amp-hours at nominal voltage\">Ah</button>"
         "<button class=\"chart-tab\" data-unit=\"pct\" onclick=\"switchChartUnit('pct')\" title=\"Equivalent battery state-of-charge\">Battery %</button></div>"
         "<span style=\"flex:1\"></span>"
         "<label style=\"font-size:.72rem;color:var(--muted);cursor:pointer\" title=\"Show running total of energy over the forecast period\"><input type=\"checkbox\" id=\"show-cumul\" onchange=\"renderActiveChart()\"> Cumulative</label>"
         "<label style=\"font-size:.72rem;color:var(--muted);cursor:pointer\" title=\"Overlay estimated daily consumption from historical usage data\"><input type=\"checkbox\" id=\"show-usage\" onchange=\"renderActiveChart()\" checked> Est. usage</label>"
         "<label style=\"font-size:.72rem;color:var(--muted);cursor:pointer\" title=\"Project battery state of charge over time based on solar generation minus estimated usage\"><input type=\"checkbox\" id=\"show-soc\" onchange=\"renderActiveChart()\" checked> Projected SoC</label></div>"
         "<div class=\"chart-wrap\" id=\"solar-chart-wrap\">"
         "<svg id=\"solar-chart\" class=\"chart-svg\" viewBox=\"0 0 900 340\">"
         "<text x=\"50%\" y=\"50%\" fill=\"var(--muted)\" font-size=\"12\" font-family=\"monospace\""
         " text-anchor=\"middle\" dominant-baseline=\"middle\">Loading…</text>"
         "</svg><div id=\"solar-tt\" class=\"chart-tooltip\" style=\"display:none\"></div></div>"
         "<div id=\"chart-legend\" class=\"chart-legend\"></div></div>\n";

    o += "<div class=\"card\"><div class=\"card-title\">Today & Tomorrow</div>";
    o += "<table><tr><td title=\"Expected solar generation today from simulation model\">Expected today</td><td id=\"today-wh\">—</td></tr>";
    o += "<tr><td title=\"Predicted generation for tomorrow from weather forecast\">Tomorrow</td><td id=\"tomorrow-wh\">—</td></tr>";
    o += "<tr><td title=\"Projected battery state at end of day based on current usage and solar\">Battery projection</td><td id=\"bat-proj\">—</td></tr></table></div>";

    o += "<div class=\"footer\">Data from OpenWeather 5-day forecast. Cached 30 min. Optimal window = clearest daytime hours. "
         "<button onclick=\"refreshWeather()\" style=\"background:var(--green);color:#fff;border:none;padding:.25rem .7rem;border-radius:4px;cursor:pointer;font-size:.75rem;font-weight:600;vertical-align:middle\">&#8635; Refresh weather</button>"
         " <span id=\"refresh-msg\" style=\"font-size:.75rem\"></span></div>";

    o += "<script>\n";
    if (!init_settings.empty())
        o += "var lmSettings=" + init_settings + ";\n";
    else
        o += "var lmSettings={theme:\"\",time:\"24\",\"show-extended\":\"0\",\"show-realistic\":\"0\",locale:\"\"};\n";
    o += "function $(id){return document.getElementById(id)}\n"
         "function fmt(v,d){return(v==null||isNaN(+v))?'—':(+v).toFixed(d)}\n"
         "function getSetting(k,d){return(lmSettings&&lmSettings[k]!==undefined)?lmSettings[k]:d}\n"
         "function setSetting(k,v){if(!lmSettings)lmSettings={};lmSettings[k]=v;fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(lmSettings)}).catch(function(){})}\n"
         "function getLocale(){var loc=getSetting('locale','');return loc===''||loc==='auto'?(navigator.language||'en-US'):loc}\n"
         "var userTZ;try{userTZ=Intl.DateTimeFormat().resolvedOptions().timeZone}catch(e){userTZ=undefined}\n"
         "function localDateStr(d){return d.getFullYear()+'-'+String(d.getMonth()+1).padStart(2,'0')+'-'+String(d.getDate()).padStart(2,'0')}\n"
         "function tsToLocal(ts){return new Date(ts*1000)}\n"
         "function fmtDate(iso){try{var d=new Date(iso+'T12:00:00');return d.toLocaleDateString(getLocale(),{weekday:'short',month:'short',day:'numeric',timeZone:userTZ})}catch(e){return iso}}\n"
         "var lastDailyData=null;\n"
         "var lastSlots=null;\n"
         "var cachedNominal=null;\n"
         "var cachedRealistic=null;\n"
         "var solarChartUnit='kwh';\n"
         "var solarChartView='solar';\n"
         "function dayEmoji(cloud){var c=(cloud||0)*100;return c<=25?'☀️':c<=50?'⛅':c<=75?'☁️':'🌧️'}\n"
         "function fmtCI(v,lo,hi,d){return fmt(v,d)+' ('+fmt(lo,d)+'\\u2013'+fmt(hi,d)+')'}\n"
         "function renderDaily(){if(!lastDailyData)return;var showExt=!!($('show-extended')&&$('show-extended').checked);var list=lastDailyData.daily.filter(function(x){return !x.is_extended||showExt});var tbody=$('daily-table');if(!tbody)return;tbody.innerHTML=list.map(function(x){\n"
         "var opt=x.optimal_start==='\\u2014'?(x.optimal_reason||'No clear window'):fmtOptTime(x.optimal_start)+'\\u2013'+fmtOptTime(x.optimal_end);\n"
         "var rcTip='Recovery: '+fmt(x.max_recovery_ah,1)+' Ah ('+fmt(x.max_recovery_ah_lo,1)+'\\u2013'+fmt(x.max_recovery_ah_hi,1)+')\\n= '+fmt(x.max_recovery_wh,0)+' Wh ('+fmt(x.max_recovery_wh_lo,0)+'\\u2013'+fmt(x.max_recovery_wh_hi,0)+')\\n= '+fmt(x.max_recovery_pct,1)+'% SoC ('+fmt(x.max_recovery_pct_lo,1)+'\\u2013'+fmt(x.max_recovery_pct_hi,1)+'%)';\n"
         "var rcHtml='<span class=\"rc-main ok\">'+fmt(x.max_recovery_ah,1)+' Ah</span><br><span class=\"recovery-ci\">'+fmt(x.max_recovery_ah_lo,1)+'\\u2013'+fmt(x.max_recovery_ah_hi,1)+' Ah</span>';\n"
         "return '<tr'+(x.is_extended?' class=dim':'')+'><td>'+dayEmoji(x.cloud_cover)+'</td><td>'+fmtDate(x.date)+'</td><td class=ok>'+fmt(x.kwh,2)+' kWh</td><td>'+Math.round((x.cloud_cover||0)*100)+'%</td><td>'+fmt(x.sun_hours_effective,1)+'</td><td title=\"'+rcTip+'\">'+rcHtml+'</td><td>'+opt+'</td></tr>';\n"
         "}).join('');}\n"
         "function switchChartUnit(u){solarChartUnit=u;document.querySelectorAll('#solar-unit-tabs .chart-tab').forEach(function(b){b.classList.toggle('active',b.dataset.unit===u)});renderActiveChart();}\n"
         "function switchChartView(v){solarChartView=v;document.querySelectorAll('#solar-view-tabs .chart-tab').forEach(function(b){b.classList.toggle('active',b.dataset.view===v)});renderActiveChart();}\n"
         "function renderActiveChart(){if(solarChartView==='balance')renderBalanceChart();else renderSolarChart();}\n"
         "function fmtTime(h,m){var pad=function(n){return n.toString().padStart(2,'0')};if(getSetting('time','24')==='12'){var h12=h%12;if(h12===0)h12=12;return h12+(m>0?':'+pad(m):'')+(h<12?' AM':' PM');}return pad(h)+':'+pad(m);}\n"
         "function niceAxis(maxV){if(maxV<=0)return{max:1,step:0.25,dec:2};var mag=Math.pow(10,Math.floor(Math.log10(maxV)));var r=maxV/mag;var step;if(r<=1.5)step=0.25*mag;else if(r<=3)step=0.5*mag;else if(r<=7)step=mag;else step=2*mag;var nmax=Math.ceil(maxV/step)*step;var dec=step>=10?0:step>=1?0:step>=0.1?1:2;return{max:nmax,step:step,dec:dec};}\n"
         "function synthExtSlots(daily,apiSlots,usageProfile){\n"
         "var extDays=daily.filter(function(d){return d.is_extended;});\n"
         "if(!extDays.length)return [];\n"
         "var existDates={};apiSlots.forEach(function(s){var dd=new Date(s.ts*1000);existDates[dd.getFullYear()+'-'+String(dd.getMonth()+1).padStart(2,'0')+'-'+String(dd.getDate()).padStart(2,'0')]=true;});\n"
         "var fallbackW=0;\n"
         "if(usageProfile){var tw=0,tn=0;usageProfile.forEach(function(u){if(u&&u.n>0){tw+=u.avg_w*u.n;tn+=u.n;}});if(tn>0)fallbackW=tw/tn;}\n"
         "var synth=[];\n"
         "extDays.forEach(function(day){\n"
         "if(existDates[day.date])return;\n"
         "var base=new Date(day.date+'T12:00:00');var midnight=new Date(base.getFullYear(),base.getMonth(),base.getDate());var bt=Math.floor(midnight.getTime()/1000);\n"
         "for(var h=0;h<24;h+=3){\n"
         "var ts=bt+h*3600;var isDaytime=(h>=6&&h<18);\n"
         "var slotKwh=0,slotLo=0,slotHi=0;\n"
         "if(isDaytime){slotKwh=day.kwh/4;var cv=0.10+0.10*day.cloud_cover;var m=1.96*cv;slotLo=slotKwh*Math.max(0,1-m);slotHi=slotKwh*(1+m);}\n"
         "var useKwh=0,useLo=0,useHi=0;\n"
         "if(usageProfile){var si=Math.floor(h/3);var u=usageProfile[si];if(u&&u.n>=3){useKwh=u.avg_w*3/1000;var sd=u.stddev_w;useLo=Math.max(0,(u.avg_w-1.96*sd)*3/1000);useHi=(u.avg_w+1.96*sd)*3/1000;}else{useKwh=fallbackW*3/1000;useLo=Math.max(0,fallbackW*0.8*3/1000);useHi=fallbackW*1.2*3/1000;}}\n"
         "synth.push({ts:ts,kwh:slotKwh,kwh_lo:slotLo,kwh_hi:slotHi,cloud:day.cloud_cover,day:isDaytime,use_kwh:useKwh,use_lo:useLo,use_hi:useHi,ext:true});\n"
         "}});\n"
         "return synth;}\n"
         "function renderSolarChart(){\n"
         "var el=$('solar-chart');var tt=$('solar-tt');\n"
         "if(!el||!lastSlots||!lastSlots.length){if(el)el.innerHTML=\"<text x='50%' y='50%' font-size='11' font-family='monospace' text-anchor='middle' dominant-baseline='middle' fill='var(--muted)'>No slot data</text>\";return;}\n"
         "var ah=lastDailyData?lastDailyData.nominal_ah:100;\n"
         "var bv=lastDailyData?lastDailyData.battery_voltage:13;\n"
         "var capWh=ah*bv;\n"
         "var showCumul=$('show-cumul')&&$('show-cumul').checked;\n"
         "var showSoc=$('show-soc')&&$('show-soc').checked;\n"
         "var showExt=!!($('show-extended')&&$('show-extended').checked);\n"
         "var isLight=document.documentElement.classList.contains('light');\n"
         "var needRightAxis=showCumul||showSoc;\n"
         "var W=900,H=340,PL=64,PR=needRightAxis?64:20,PT=16,PB=52;\n"
         "var CW=W-PL-PR,CH=H-PT-PB;\n"
         "var baseSlots=lastSlots.map(function(s){var o={};for(var k in s)o[k]=s[k];o.ext=false;return o;});\n"
         "var slots=baseSlots;\n"
         "var extBoundaryTs=0;\n"
         "if(showExt&&lastDailyData&&lastDailyData.daily){\n"
         "extBoundaryTs=baseSlots[baseSlots.length-1].ts+10800;\n"
         "var extSlots=synthExtSlots(lastDailyData.daily,lastSlots,lastDailyData.usage_profile);\n"
         "if(extSlots.length)slots=baseSlots.concat(extSlots);}\n"
         "var t0=slots[0].ts*1000,t1=(slots[slots.length-1].ts+10800)*1000;\n"
         "var tspan=Math.max(1,t1-t0);\n"
         "function conv(kwh){\n"
         "if(solarChartUnit==='ah')return kwh*1000/bv;\n"
         "if(solarChartUnit==='pct')return capWh>0?(kwh*1000/capWh)*100:0;\n"
         "return kwh;}\n"
         "var unitLbl=solarChartUnit==='ah'?'Ah':solarChartUnit==='pct'?'% of battery':'kWh';\n"
         "var unitShort=solarChartUnit==='ah'?'Ah':solarChartUnit==='pct'?'%':'kWh';\n"
         "var cumulLbl=solarChartUnit==='ah'?'Cumul. Ah':solarChartUnit==='pct'?'Cumul. %':'Cumul. kWh';\n"
         "var ymax=0;var r99=2.576/1.96;\n"
         "slots.forEach(function(s){var hi99=s.kwh+(s.kwh_hi-s.kwh)*r99;var v=conv(hi99);if(v>ymax)ymax=v;var u=conv(s.use_hi||0);if(u>ymax)ymax=u;});\n"
         "if(ymax<0.01)ymax=1;\n"
         "var ya=niceAxis(ymax);\n"
         "var cumArr=[];var cumSum=0;\n"
         "slots.forEach(function(s){cumSum+=conv(s.kwh);cumArr.push(cumSum);});\n"
         "var cumMax=cumArr.length?cumArr[cumArr.length-1]*1.1:1;\n"
         "var ca=niceAxis(cumMax);\n"
         "var socArr=[];\n"
         "if(showSoc&&capWh>0){\n"
         "var lastSoc=lastDailyData.current_soc_pct||0;\n"
         "slots.forEach(function(s){\n"
         "if(typeof s.soc_pct==='number'){lastSoc=s.soc_pct;socArr.push(lastSoc);}\n"
         "else{var netWh=(s.kwh-(s.use_kwh||0))*1000;lastSoc+=(netWh/capWh)*100;lastSoc=Math.max(0,Math.min(100,lastSoc));socArr.push(lastSoc);}\n"
         "});}\n"
         "function ypSoc(pct){return PT+CH-(pct/100)*CH;}\n"
         "function xp(ts){return PL+((ts*1000-t0)/tspan)*CW;}\n"
         "function xpM(ts){return xp(ts+5400);}\n"
         "function yp(v){return PT+CH-Math.min(1,v/ya.max)*CH;}\n"
         "function yp2(v){return PT+CH-Math.min(1,v/ca.max)*CH;}\n"
         "var s='<defs>';\n"
         "s+=\"<linearGradient id='ci95g' x1='0' y1='0' x2='0' y2='1'><stop offset='0%' stop-color='var(--green)' stop-opacity='.12'/><stop offset='100%' stop-color='var(--green)' stop-opacity='.04'/></linearGradient>\";\n"
         "s+=\"<linearGradient id='ci99g' x1='0' y1='0' x2='0' y2='1'><stop offset='0%' stop-color='var(--green)' stop-opacity='.07'/><stop offset='100%' stop-color='var(--green)' stop-opacity='.02'/></linearGradient>\";\n"
         "s+=\"<linearGradient id='cumg' x1='0' y1='0' x2='0' y2='1'><stop offset='0%' stop-color='var(--cyan)' stop-opacity='.15'/><stop offset='100%' stop-color='var(--cyan)' stop-opacity='.02'/></linearGradient>\";\n"
         "s+='</defs>';\n"
         "s+=\"<rect width='\"+W+\"' height='\"+H+\"' fill='var(--bg)' rx='6'/>\";\n"
         "var nightFill=isLight?'rgba(100,110,140,0.06)':'rgba(140,160,220,0.04)';\n"
         "var sunFill=isLight?'rgba(255,220,50,0.04)':'rgba(255,220,50,0.02)';\n"
         "var inNight=false,nightStart=0;\n"
         "slots.forEach(function(sl){\n"
         "if(!sl.day&&!inNight){inNight=true;nightStart=sl.ts;}\n"
         "if(sl.day&&inNight){inNight=false;var x1=xp(nightStart),x2=xp(sl.ts);x1=Math.max(x1,PL);x2=Math.min(x2,PL+CW);\n"
         "s+=\"<rect x='\"+x1.toFixed(1)+\"' y='\"+PT+\"' width='\"+(x2-x1).toFixed(1)+\"' height='\"+CH+\"' fill='\"+nightFill+\"'/>\";\n"
         "var mx=(x1+x2)/2;if(x2-x1>30)s+=\"<text x='\"+mx.toFixed(1)+\"' y='\"+(PT+14)+\"' text-anchor='middle' font-size='8' fill='\"+(isLight?'rgba(0,0,0,.15)':'rgba(255,255,255,.08)')+\"' font-family='monospace'>NIGHT</text>\";}\n"
         "});\n"
         "if(inNight){var x1=Math.max(xp(nightStart),PL),x2=PL+CW;s+=\"<rect x='\"+x1.toFixed(1)+\"' y='\"+PT+\"' width='\"+(x2-x1).toFixed(1)+\"' height='\"+CH+\"' fill='\"+nightFill+\"'/>\";}\n"
         "s+=\"<g stroke='\"+(isLight?'rgba(0,0,0,.08)':'rgba(255,255,255,.06)')+\"' stroke-width='0.5'>\";\n"
         "for(var yv=ya.step;yv<ya.max;yv+=ya.step){var gy=yp(yv);s+=\"<line x1='\"+PL+\"' y1='\"+gy.toFixed(1)+\"' x2='\"+(PL+CW)+\"' y2='\"+gy.toFixed(1)+\"'/>\";}\n"
         "s+=\"</g>\";\n"
         "var prevDay='',dayStarts=[];\n"
         "slots.forEach(function(sl){\n"
         "var d=tsToLocal(sl.ts);var ds=localDateStr(d);\n"
         "if(ds!==prevDay){dayStarts.push({ts:sl.ts,d:d});prevDay=ds;}});\n"
         "s+=\"<g stroke='\"+(isLight?'rgba(0,0,0,.12)':'rgba(255,255,255,.08)')+\"' stroke-width='0.5' stroke-dasharray='4,3'>\";\n"
         "dayStarts.forEach(function(ds,i){if(i===0)return;var x=xp(ds.ts);if(x>PL+5&&x<PL+CW-5)s+=\"<line x1='\"+x.toFixed(1)+\"' y1='\"+PT+\"' x2='\"+x.toFixed(1)+\"' y2='\"+(PT+CH)+\"'/>\";});\n"
         "s+=\"</g>\";\n"
         "if(showExt&&extBoundaryTs>0){\n"
         "var bx=xp(extBoundaryTs);if(bx>PL&&bx<PL+CW){\n"
         "s+=\"<rect x='\"+bx.toFixed(1)+\"' y='\"+PT+\"' width='\"+(PL+CW-bx).toFixed(1)+\"' height='\"+CH+\"' fill='\"+(isLight?'rgba(0,0,0,0.025)':'rgba(255,255,255,0.015)')+\"'/>\";\n"
         "s+=\"<line x1='\"+bx.toFixed(1)+\"' y1='\"+PT+\"' x2='\"+bx.toFixed(1)+\"' y2='\"+(PT+CH)+\"' stroke='var(--muted)' stroke-width='1' stroke-dasharray='6,4'/>\";\n"
         "s+=\"<text x='\"+(bx+6).toFixed(1)+\"' y='\"+(PT+12)+\"' font-size='7.5' font-family='monospace' fill='var(--muted)' opacity='.7'>EXTENDED (est.)</text>\";}}\n"
         "var ci95f='',ci95b='',ci99f='',ci99b='';\n"
         "var ratio99=2.576/1.96;\n"
         "slots.forEach(function(sl){var x=xpM(sl.ts);\n"
         "ci95f+=x.toFixed(1)+','+yp(conv(sl.kwh_hi)).toFixed(1)+' ';\n"
         "var w99lo=sl.kwh-(sl.kwh-sl.kwh_lo)*ratio99;\n"
         "var w99hi=sl.kwh+(sl.kwh_hi-sl.kwh)*ratio99;\n"
         "ci99f+=x.toFixed(1)+','+yp(conv(Math.max(0,w99hi))).toFixed(1)+' ';\n"
         "});\n"
         "for(var i=slots.length-1;i>=0;i--){var sl=slots[i];var x=xpM(sl.ts);\n"
         "ci95b+=x.toFixed(1)+','+yp(conv(sl.kwh_lo)).toFixed(1)+' ';\n"
         "var w99lo=sl.kwh-(sl.kwh-sl.kwh_lo)*ratio99;\n"
         "ci99b+=x.toFixed(1)+','+yp(conv(Math.max(0,w99lo))).toFixed(1)+' ';\n"
         "}\n"
         "s+=\"<polygon points='\"+ci99f+ci99b+\"' fill='url(#ci99g)'/>\";\n"
         "s+=\"<polygon points='\"+ci95f+ci95b+\"' fill='url(#ci95g)'/>\";\n"
         "if(showCumul){\n"
         "var cumPts=PL.toFixed(1)+','+(PT+CH).toFixed(1)+' ';\n"
         "slots.forEach(function(sl,i){cumPts+=xpM(sl.ts).toFixed(1)+','+yp2(cumArr[i]).toFixed(1)+' ';});\n"
         "cumPts+=(PL+CW).toFixed(1)+','+(PT+CH).toFixed(1);\n"
         "s+=\"<polygon points='\"+cumPts+\"' fill='url(#cumg)'/>\";\n"
         "var cumLine='';slots.forEach(function(sl,i){cumLine+=xpM(sl.ts).toFixed(1)+','+yp2(cumArr[i]).toFixed(1)+' ';});\n"
         "s+=\"<polyline fill='none' stroke='var(--cyan)' stroke-width='1.5' stroke-dasharray='6,3' stroke-linecap='round' points='\"+cumLine+\"'/>\";}\n"
         "if(showSoc&&socArr.length){\n"
         "s+=\"<line x1='\"+PL+\"' y1='\"+ypSoc(20).toFixed(1)+\"' x2='\"+(PL+CW)+\"' y2='\"+ypSoc(20).toFixed(1)+\"' stroke='var(--red)' stroke-width='0.5' stroke-dasharray='3,4' opacity='.3'/>\";\n"
         "s+=\"<line x1='\"+PL+\"' y1='\"+ypSoc(80).toFixed(1)+\"' x2='\"+(PL+CW)+\"' y2='\"+ypSoc(80).toFixed(1)+\"' stroke='var(--green)' stroke-width='0.5' stroke-dasharray='3,4' opacity='.3'/>\";\n"
         "var socFill=PL.toFixed(1)+','+(PT+CH).toFixed(1)+' ';\n"
         "slots.forEach(function(sl,i){socFill+=xpM(sl.ts).toFixed(1)+','+ypSoc(socArr[i]).toFixed(1)+' ';});\n"
         "socFill+=(PL+CW).toFixed(1)+','+(PT+CH).toFixed(1);\n"
         "s+=\"<polygon points='\"+socFill+\"' fill='var(--violet)' opacity='.12'/>\";\n"
         "var socLine='';slots.forEach(function(sl,i){socLine+=xpM(sl.ts).toFixed(1)+','+ypSoc(socArr[i]).toFixed(1)+' ';});\n"
         "s+=\"<polyline fill='none' stroke='var(--violet)' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round' points='\"+socLine+\"'/>\";\n"
         "var startX=xpM(slots[0].ts),startY=ypSoc(socArr[0]);\n"
         "s+=\"<circle cx='\"+startX.toFixed(1)+\"' cy='\"+startY.toFixed(1)+\"' r='5' fill='var(--violet)' stroke='var(--bg)' stroke-width='2'/>\";\n"
         "s+=\"<text x='\"+(startX+8).toFixed(1)+\"' y='\"+(startY-5).toFixed(1)+\"' font-size='9' font-family='monospace' fill='var(--violet)' font-weight='700'>\"+Math.round(socArr[0])+'%</text>';}\n"
         "var linePts='';slots.forEach(function(sl){linePts+=xpM(sl.ts).toFixed(1)+','+yp(conv(sl.kwh)).toFixed(1)+' ';});\n"
         "s+=\"<polyline fill='none' stroke='var(--green)' stroke-width='2' stroke-linecap='round' stroke-linejoin='round' points='\"+linePts+\"'/>\";\n"
         "slots.forEach(function(sl,i){\n"
         "if(sl.kwh>0){var cx=xpM(sl.ts),cy=yp(conv(sl.kwh));\n"
         "if(sl.ext)s+=\"<circle cx='\"+cx.toFixed(1)+\"' cy='\"+cy.toFixed(1)+\"' r='3' fill='var(--bg)' stroke='var(--green)' stroke-width='1' opacity='.5'/>\";\n"
         "else s+=\"<circle cx='\"+cx.toFixed(1)+\"' cy='\"+cy.toFixed(1)+\"' r='3' fill='var(--green)' stroke='var(--bg)' stroke-width='1.5'/>\";}\n"
         "});\n"
         "var showUse=$('show-usage')&&$('show-usage').checked;\n"
         "if(showUse){\n"
         "var useCi='',useCiB='';\n"
         "slots.forEach(function(sl){var x=xpM(sl.ts);useCi+=x.toFixed(1)+','+yp(conv(sl.use_hi||0)).toFixed(1)+' ';});\n"
         "for(var i=slots.length-1;i>=0;i--){var sl=slots[i];useCiB+=xpM(sl.ts).toFixed(1)+','+yp(conv(sl.use_lo||0)).toFixed(1)+' ';}\n"
         "s+=\"<polygon points='\"+useCi+useCiB+\"' fill='var(--orange)' opacity='.10'/>\";\n"
         "var useLine='';slots.forEach(function(sl){useLine+=xpM(sl.ts).toFixed(1)+','+yp(conv(sl.use_kwh||0)).toFixed(1)+' ';});\n"
         "s+=\"<polyline fill='none' stroke='var(--orange)' stroke-width='1.5' stroke-dasharray='6,4' stroke-linecap='round' points='\"+useLine+\"'/>\";\n"
         "slots.forEach(function(sl){\n"
         "if((sl.use_kwh||0)>0){var cx=xpM(sl.ts),cy=yp(conv(sl.use_kwh));\n"
         "s+=\"<circle cx='\"+cx.toFixed(1)+\"' cy='\"+cy.toFixed(1)+\"' r='2' fill='var(--orange)' stroke='var(--bg)' stroke-width='1'/>\";}\n"
         "});}\n"
         "s+=\"<line x1='\"+PL+\"' y1='\"+PT+\"' x2='\"+PL+\"' y2='\"+(PT+CH)+\"' stroke='var(--muted)' stroke-width='1'/>\";\n"
         "s+=\"<line x1='\"+PL+\"' y1='\"+(PT+CH)+\"' x2='\"+(PL+CW)+\"' y2='\"+(PT+CH)+\"' stroke='var(--muted)' stroke-width='1'/>\";\n"
         "if(needRightAxis){var rc=showSoc?'var(--violet)':'var(--cyan)';s+=\"<line x1='\"+(PL+CW)+\"' y1='\"+PT+\"' x2='\"+(PL+CW)+\"' y2='\"+(PT+CH)+\"' stroke='\"+rc+\"' stroke-width='0.5' opacity='.4'/>\";}\n"
         "s+=\"<g font-size='9' font-family='monospace' fill='var(--muted)'>\";\n"
         "for(var yv=0;yv<=ya.max;yv+=ya.step){var gy=yp(yv);\n"
         "s+=\"<line x1='\"+(PL-3)+\"' y1='\"+gy.toFixed(1)+\"' x2='\"+PL+\"' y2='\"+gy.toFixed(1)+\"' stroke='var(--muted)' stroke-width='0.5'/>\";\n"
         "s+=\"<text x='\"+(PL-5)+\"' y='\"+(gy+3).toFixed(1)+\"' text-anchor='end'>\"+fmt(yv,ya.dec)+\"</text>\";}\n"
         "s+=\"</g>\";\n"
         "if(showSoc){s+=\"<g font-size='10' font-family='monospace' fill='var(--violet)' font-weight='600'>\";\n"
         "for(var pv=0;pv<=100;pv+=20){var gy=ypSoc(pv);\n"
         "s+=\"<line x1='\"+(PL+CW)+\"' y1='\"+gy.toFixed(1)+\"' x2='\"+(PL+CW+4)+\"' y2='\"+gy.toFixed(1)+\"' stroke='var(--violet)' stroke-width='1'/>\";\n"
         "s+=\"<text x='\"+(PL+CW+6)+\"' y='\"+(gy+3.5).toFixed(1)+\"' text-anchor='start'>\"+pv+\"%</text>\";}\n"
         "s+=\"</g>\";}\n"
         "else if(showCumul){s+=\"<g font-size='9' font-family='monospace' fill='var(--cyan)'>\";\n"
         "for(var yv=0;yv<=ca.max;yv+=ca.step){var gy=yp2(yv);\n"
         "s+=\"<line x1='\"+(PL+CW)+\"' y1='\"+gy.toFixed(1)+\"' x2='\"+(PL+CW+3)+\"' y2='\"+gy.toFixed(1)+\"' stroke='var(--cyan)' stroke-width='0.5'/>\";\n"
         "s+=\"<text x='\"+(PL+CW+5)+\"' y='\"+(gy+3).toFixed(1)+\"' text-anchor='start'>\"+fmt(yv,ca.dec)+\"</text>\";}\n"
         "s+=\"</g>\";}\n"
         "var nDays=dayStarts.length;\n"
         "var hStep=nDays<=5?6:nDays<=8?12:24;\n"
         "var showHourTicks=nDays<=8;\n"
         "var dateFmt=nDays<=7?{weekday:'short',month:'short',day:'numeric'}:nDays<=14?{month:'short',day:'numeric'}:{month:'numeric',day:'numeric'};\n"
         "var dateLabelEvery=nDays<=10?1:nDays<=16?2:3;\n"
         "s+=\"<g font-size='8.5' font-family='monospace' fill='var(--muted)'>\";\n"
         "if(showHourTicks){var lastTx=-999;\n"
         "slots.forEach(function(sl){\n"
         "var d=tsToLocal(sl.ts);var h=d.getHours();\n"
         "if(h%hStep!==0)return;\n"
         "var x=xp(sl.ts);if(x<PL+8||x>PL+CW-8)return;\n"
         "if(x-lastTx<28)return;lastTx=x;\n"
         "s+=\"<line x1='\"+x.toFixed(1)+\"' y1='\"+(PT+CH)+\"' x2='\"+x.toFixed(1)+\"' y2='\"+(PT+CH+4)+\"' stroke='var(--muted)' stroke-width='0.5'/>\";\n"
         "s+=\"<text x='\"+x.toFixed(1)+\"' y='\"+(PT+CH+14)+\"' text-anchor='middle'>\"+fmtTime(h,0)+\"</text>\";});}\n"
         "var lastDx=-999;\n"
         "dayStarts.forEach(function(ds,i){\n"
         "if(i%dateLabelEvery!==0&&i!==0)return;\n"
         "var d=ds.d;var dayEnd=i+1<dayStarts.length?dayStarts[i+1].ts:slots[slots.length-1].ts+10800;\n"
         "var mid=ds.ts+(dayEnd-ds.ts)/2;var x=xp(mid);\n"
         "if(x<PL+10||x>PL+CW-10)return;\n"
         "if(x-lastDx<45)return;lastDx=x;\n"
         "var dfOpts=Object.assign({},dateFmt);if(userTZ)dfOpts.timeZone=userTZ;\n"
         "s+=\"<text x='\"+x.toFixed(1)+\"' y='\"+(H-4)+\"' text-anchor='middle' font-size='\"+(nDays>10?'7.5':'9')+\"' font-weight='600'>\"+d.toLocaleDateString(getLocale(),dfOpts)+\"</text>\";});\n"
         "s+=\"</g>\";\n"
         "s+=\"<text x='\"+(PL/2)+\"' y='\"+(PT+CH/2)+\"' text-anchor='middle' font-size='9' font-family='monospace' fill='var(--muted)' transform='rotate(-90 \"+(PL/2)+\" \"+(PT+CH/2)+\")'>\"+unitLbl+\" per 3h slot</text>\";\n"
         "if(showSoc)s+=\"<text x='\"+(W-PR/2)+\"' y='\"+(PT+CH/2)+\"' text-anchor='middle' font-size='9' font-family='monospace' fill='var(--violet)' transform='rotate(90 \"+(W-PR/2)+\" \"+(PT+CH/2)+\")'>Battery SoC %</text>\";\n"
         "else if(showCumul)s+=\"<text x='\"+(W-PR/2)+\"' y='\"+(PT+CH/2)+\"' text-anchor='middle' font-size='9' font-family='monospace' fill='var(--cyan)' transform='rotate(90 \"+(W-PR/2)+\" \"+(PT+CH/2)+\")'>\"+cumulLbl+\"</text>\";\n"
         "var isReal=lastDailyData&&lastDailyData.realistic;\n"
         "var isHist=isReal&&lastDailyData.realistic_from_history;\n"
         "var lg=$('chart-legend');if(lg){var lh='';\n"
         "lh+='<span class=lg><span class=swatch style=\"background:var(--green)\"></span><span class=dot style=\"background:var(--green)\"></span>Solar generation'+(isReal?(isHist?' (realistic, measured)':' (realistic, default taper)'):' (nominal)')+'<span class=tip>'+(isReal?(isHist?'Solar energy adjusted for charge controller absorption/float taper derived from the last 14 days of historical charge data. At higher SoC, the charger reduces current, so less solar energy is actually accepted by the battery.':'Solar energy adjusted using a standard CC-CV taper model. At higher SoC the charger reduces current, so less energy is accepted. Connect a charge controller for measured taper data.'):'Expected solar energy per 3-hour slot based on panel capacity, system efficiency, and cloud cover from OpenWeather forecast. Assumes constant max charge current (nominal).')+'</span></span>';\n"
         "lh+='<span class=lg><span class=\"swatch-band\" style=\"background:linear-gradient(var(--green),transparent);opacity:.2\"></span>95% CI<span class=tip>Inner band: 95% confidence interval (\\u00b11.96\\u03c3). Based on cloud forecast uncertainty (CV\\u00a0=\\u00a010\\u201320%). The actual yield should fall within this band ~95% of the time.</span></span>';\n"
         "lh+='<span class=lg><span class=\"swatch-band\" style=\"background:linear-gradient(var(--green),transparent);opacity:.08\"></span>99% CI<span class=tip>Outer band: 99% confidence interval (\\u00b12.576\\u03c3). Very unlikely the actual yield falls outside this range\\u2014only ~1% of the time if the cloud model is well-calibrated.</span></span>';\n"
         "if(showUse){var mw=lastDailyData.measured_avg_w;lh+='<span class=lg><span class=swatch style=\"background:var(--orange);border-top:1.5px dashed var(--orange)\"></span><span class=dot style=\"background:var(--orange)\"></span>Est. usage'+(mw>0?' ('+fmt(mw,1)+'W avg)':' (7d)')+'<span class=tip>'+(mw>0?'Power consumption estimated from the measured 24h average load ('+fmt(mw,1)+' W). Time-of-day profile scaled to match actual integrated discharge. Shaded band shows 95% CI.':'Average power consumption per 3-hour time-of-day slot from the last 7 days of battery discharge data. Shaded band shows the 95% CI from observed variability.')+'</span></span>';}\n"
         "if(showCumul)lh+='<span class=lg><span class=swatch style=\"border-top:1.5px dashed var(--cyan)\"></span>Cumulative<span class=tip>Running total of solar energy generated over the forecast period. Useful for projecting total harvest and comparing against battery capacity or load.</span></span>';\n"
         "if(showSoc)lh+='<span class=lg><span class=swatch style=\"background:var(--violet)\"></span>Projected SoC<span class=tip>Projected battery state of charge over time. Starts at current SoC ('+Math.round(lastDailyData.current_soc_pct||0)+'%) and adds solar generation while subtracting estimated usage each slot. Dashed lines mark 20% (low) and 80% (absorption taper) thresholds.</span></span>';\n"
         "lg.innerHTML=lh;}\n"
         "s+=\"<rect id='solar-overlay' x='\"+PL+\"' y='\"+PT+\"' width='\"+CW+\"' height='\"+CH+\"' fill='transparent' style='cursor:crosshair'/>\";\n"
         "el.setAttribute('viewBox','0 0 '+W+' '+H);el.innerHTML=s;\n"
         "var overlay=document.getElementById('solar-overlay');\n"
         "if(overlay&&tt){\n"
         "var wrap=$('solar-chart-wrap');\n"
         "overlay.addEventListener('mousemove',function(e){\n"
         "var rect=el.getBoundingClientRect();\n"
         "var scaleX=W/rect.width;\n"
         "var svgX=(e.clientX-rect.left)*scaleX;\n"
         "var frac=(svgX-PL)/CW;frac=Math.max(0,Math.min(1,frac));\n"
         "var hoverTs=t0/1000+frac*(tspan/1000);\n"
         "var best=-1,bestD=1e15;\n"
         "slots.forEach(function(sl,i){var d=Math.abs(sl.ts+5400-hoverTs);if(d<bestD){bestD=d;best=i;}});\n"
         "if(best<0){tt.style.display='none';return;}\n"
         "var sl=slots[best];\n"
         "var d=tsToLocal(sl.ts);\n"
         "var w99lo=Math.max(0,sl.kwh-(sl.kwh-sl.kwh_lo)*ratio99);\n"
         "var w99hi=sl.kwh+(sl.kwh_hi-sl.kwh)*ratio99;\n"
         "var ttDateOpts={weekday:'short',month:'short',day:'numeric'};if(userTZ)ttDateOpts.timeZone=userTZ;\n"
         "var htm='<div class=tt-date>'+d.toLocaleDateString(getLocale(),ttDateOpts)+' '+fmtTime(d.getHours(),d.getMinutes())+' \\u2013 '+fmtTime((d.getHours()+3)%24,d.getMinutes())+'</div>';\n"
         "htm+='<div class=tt-row>'+(sl.day?'\\u2600\\ufe0f Daytime':'\\u263d Night')+(sl.ext?' (extended est.)':'')+'</div>';\n"
         "htm+='<div class=tt-row>Estimate: <span class=tt-val>'+fmt(conv(sl.kwh),2)+' '+unitShort+'</span></div>';\n"
         "htm+='<div class=tt-row>95% CI: '+fmt(conv(sl.kwh_lo),2)+' \\u2013 '+fmt(conv(sl.kwh_hi),2)+' '+unitShort+'</div>';\n"
         "htm+='<div class=tt-row>99% CI: '+fmt(conv(w99lo),2)+' \\u2013 '+fmt(conv(w99hi),2)+' '+unitShort+'</div>';\n"
         "htm+='<div class=tt-row>Cloud: '+Math.round(sl.cloud*100)+'%</div>';\n"
         "if(showUse&&(sl.use_kwh||0)>0)htm+='<div class=tt-row style=\"color:var(--orange)\">Usage: '+fmt(conv(sl.use_kwh),2)+' '+unitShort+' ('+fmt(conv(sl.use_lo),2)+'\\u2013'+fmt(conv(sl.use_hi),2)+')</div>';\n"
         "if(showCumul)htm+='<div class=tt-row style=\"color:var(--cyan)\">Cumul: '+fmt(cumArr[best],2)+' '+unitShort+'</div>';\n"
         "if(showSoc&&socArr.length>best)htm+='<div class=tt-row style=\"color:var(--violet)\">SoC: '+Math.round(socArr[best])+'%</div>';\n"
         "tt.innerHTML=htm;\n"
         "var px=e.clientX-wrap.getBoundingClientRect().left;\n"
         "var py=e.clientY-wrap.getBoundingClientRect().top;\n"
         "tt.style.left=(px+12)+'px';tt.style.top=(py-10)+'px';tt.style.display='block';\n"
         "if(px+tt.offsetWidth+12>wrap.offsetWidth)tt.style.left=(px-tt.offsetWidth-8)+'px';\n"
         "});\n"
         "overlay.addEventListener('mouseleave',function(){tt.style.display='none';});\n"
         "}}\n"
         "function renderBalanceChart(){\n"
         "var el=$('solar-chart');var tt=$('solar-tt');\n"
         "if(!el||!lastDailyData||!lastDailyData.daily||!lastDailyData.daily.length){if(el)el.innerHTML=\"<text x='50%' y='50%' font-size='11' font-family='monospace' text-anchor='middle' dominant-baseline='middle' fill='var(--muted)'>No data</text>\";return;}\n"
         "var showExt=!!($('show-extended')&&$('show-extended').checked);\n"
         "var days=lastDailyData.daily.filter(function(x){return !x.is_extended||showExt;});\n"
         "if(!days.length)return;\n"
         "var showCumul=$('show-cumul')&&$('show-cumul').checked;\n"
         "var isLight=document.documentElement.classList.contains('light');\n"
         "var bv=lastDailyData.battery_voltage||12.8;\n"
         "var capWh=(lastDailyData.nominal_ah||100)*bv;\n"
         "function bc(kwh){if(solarChartUnit==='ah')return kwh*1000/bv;if(solarChartUnit==='pct')return capWh>0?(kwh*1000/capWh)*100:0;return kwh;}\n"
         "var bUnit=solarChartUnit==='ah'?'Ah':solarChartUnit==='pct'?'%':'kWh';\n"
         "var bCumLbl=solarChartUnit==='ah'?'Cumul. Ah':solarChartUnit==='pct'?'Cumul. %':'Cumul. kWh';\n"
         "var W=900,H=340,PL=64,PR=showCumul?64:20,PT=16,PB=56;\n"
         "var CW=W-PL-PR,CH=H-PT-PB;\n"
         "var ymin=0,ymax=0;\n"
         "days.forEach(function(d){var v=bc(d.surplus_kwh||0);var lo=bc(d.surplus_kwh_lo||0);var hi=bc(d.surplus_kwh_hi||0);if(hi>ymax)ymax=hi;if(lo<ymin)ymin=lo;if(v>ymax)ymax=v;if(v<ymin)ymin=v;});\n"
         "var pad=Math.max(Math.abs(ymax),Math.abs(ymin))*0.15;if(pad<0.1)pad=0.1;\n"
         "ymax+=pad;ymin-=pad;\n"
         "var ya=niceAxis(Math.max(Math.abs(ymin),ymax));\n"
         "ya.max=ya.max;ya.min=-ya.max;\n"
         "var cumArr=[];var cumSum=0;\n"
         "days.forEach(function(d){cumSum+=bc(d.surplus_kwh||0);cumArr.push(cumSum);});\n"
         "var cumAbs=0;cumArr.forEach(function(v){if(Math.abs(v)>cumAbs)cumAbs=Math.abs(v);});\n"
         "var ca=niceAxis(cumAbs||1);\n"
         "function yp(v){return PT+CH/2-((v/ya.max)*(CH/2));}\n"
         "function yp2(v){return PT+CH/2-((v/ca.max)*(CH/2));}\n"
         "var bw=Math.min(CW/days.length*0.6,60);\n"
         "var gap=(CW-bw*days.length)/(days.length+1);\n"
         "function bx(i){return PL+gap+(gap+bw)*i;}\n"
         "var s='<defs>';\n"
         "s+=\"<linearGradient id='surpG' x1='0' y1='0' x2='0' y2='1'><stop offset='0%' stop-color='var(--green)' stop-opacity='.8'/><stop offset='100%' stop-color='var(--green)' stop-opacity='.4'/></linearGradient>\";\n"
         "s+=\"<linearGradient id='defG' x1='0' y1='0' x2='0' y2='1'><stop offset='0%' stop-color='var(--orange)' stop-opacity='.4'/><stop offset='100%' stop-color='var(--orange)' stop-opacity='.8'/></linearGradient>\";\n"
         "s+='</defs>';\n"
         "s+=\"<rect width='\"+W+\"' height='\"+H+\"' fill='var(--bg)' rx='6'/>\";\n"
         "s+=\"<g stroke='\"+(isLight?'rgba(0,0,0,.06)':'rgba(255,255,255,.05)')+\"' stroke-width='0.5'>\";\n"
         "for(var yv=ya.step;yv<=ya.max;yv+=ya.step){s+=\"<line x1='\"+PL+\"' y1='\"+yp(yv).toFixed(1)+\"' x2='\"+(PL+CW)+\"' y2='\"+yp(yv).toFixed(1)+\"'/>\";s+=\"<line x1='\"+PL+\"' y1='\"+yp(-yv).toFixed(1)+\"' x2='\"+(PL+CW)+\"' y2='\"+yp(-yv).toFixed(1)+\"'/>\";}\n"
         "s+=\"</g>\";\n"
         "var zero_y=yp(0);\n"
         "s+=\"<line x1='\"+PL+\"' y1='\"+zero_y.toFixed(1)+\"' x2='\"+(PL+CW)+\"' y2='\"+zero_y.toFixed(1)+\"' stroke='var(--muted)' stroke-width='1' stroke-dasharray='4,2'/>\";\n"
         "s+=\"<text x='\"+(PL+CW+4)+\"' y='\"+(zero_y+3).toFixed(1)+\"' font-size='8' font-family='monospace' fill='var(--muted)'>0</text>\";\n"
         "days.forEach(function(d,i){\n"
         "var x=bx(i),val=bc(d.surplus_kwh||0),lo=bc(d.surplus_kwh_lo||0),hi=bc(d.surplus_kwh_hi||0);\n"
         "var y0=zero_y,yVal=yp(val),yLo=yp(lo),yHi=yp(hi);\n"
         "s+=\"<rect x='\"+x.toFixed(1)+\"' y='\"+Math.min(yHi,yp(0)).toFixed(1)+\"' width='\"+bw.toFixed(1)+\"' height='\"+Math.abs(yHi-yp(0)).toFixed(1)+\"' fill='\"+(hi>=0?'var(--green)':'var(--orange)')+\"' opacity='.06' rx='2'/>\";\n"
         "s+=\"<rect x='\"+x.toFixed(1)+\"' y='\"+Math.min(yp(0),yLo).toFixed(1)+\"' width='\"+bw.toFixed(1)+\"' height='\"+Math.abs(yLo-yp(0)).toFixed(1)+\"' fill='\"+(lo>=0?'var(--green)':'var(--orange)')+\"' opacity='.06' rx='2'/>\";\n"
         "var barTop=Math.min(y0,yVal),barH=Math.abs(yVal-y0);\n"
         "var fill=val>=0?'url(#surpG)':'url(#defG)';\n"
         "s+=\"<rect x='\"+x.toFixed(1)+\"' y='\"+barTop.toFixed(1)+\"' width='\"+bw.toFixed(1)+\"' height='\"+Math.max(1,barH).toFixed(1)+\"' fill='\"+fill+\"' rx='2'/>\";\n"
         "var cx=(x+bw/2).toFixed(1);\n"
         "s+=\"<line x1='\"+cx+\"' y1='\"+yHi.toFixed(1)+\"' x2='\"+cx+\"' y2='\"+yLo.toFixed(1)+\"' stroke='\"+(val>=0?'var(--green)':'var(--orange)')+\"' stroke-width='1.5'/>\";\n"
         "s+=\"<line x1='\"+(x+bw*0.25).toFixed(1)+\"' y1='\"+yHi.toFixed(1)+\"' x2='\"+(x+bw*0.75).toFixed(1)+\"' y2='\"+yHi.toFixed(1)+\"' stroke='\"+(val>=0?'var(--green)':'var(--orange)')+\"' stroke-width='1.5'/>\";\n"
         "s+=\"<line x1='\"+(x+bw*0.25).toFixed(1)+\"' y1='\"+yLo.toFixed(1)+\"' x2='\"+(x+bw*0.75).toFixed(1)+\"' y2='\"+yLo.toFixed(1)+\"' stroke='\"+(val>=0?'var(--green)':'var(--orange)')+\"' stroke-width='1.5'/>\";\n"
         "s+=\"<text x='\"+cx+\"' y='\"+(Math.min(yVal,y0)-5).toFixed(1)+\"' text-anchor='middle' font-size='8.5' font-weight='600' font-family='monospace' fill='\"+(val>=0?'var(--green)':'var(--orange)')+\"'>\"+fmt(val,solarChartUnit==='pct'?1:2)+'</text>';\n"
         "var dt=new Date(d.date+'T12:00:00');\n"
         "var wdOpts={weekday:'short'};var mdOpts={month:'short',day:'numeric'};if(userTZ){wdOpts.timeZone=userTZ;mdOpts.timeZone=userTZ;}\n"
         "s+=\"<text x='\"+cx+\"' y='\"+(PT+CH+14)+\"' text-anchor='middle' font-size='8.5' font-family='monospace' fill='var(--muted)'>\"+dt.toLocaleDateString(getLocale(),wdOpts)+\"</text>\";\n"
         "s+=\"<text x='\"+cx+\"' y='\"+(PT+CH+24)+\"' text-anchor='middle' font-size='7.5' font-family='monospace' fill='var(--muted)'>\"+dt.toLocaleDateString(getLocale(),mdOpts)+\"</text>\";\n"
         "});\n"
         "if(showCumul&&days.length>1){\n"
         "var cumLine='';days.forEach(function(d,i){cumLine+=(bx(i)+bw/2).toFixed(1)+','+yp2(cumArr[i]).toFixed(1)+' ';});\n"
         "s+=\"<polyline fill='none' stroke='var(--cyan)' stroke-width='1.5' stroke-dasharray='6,3' stroke-linecap='round' points='\"+cumLine+\"'/>\";\n"
         "days.forEach(function(d,i){var cx=bx(i)+bw/2,cy=yp2(cumArr[i]);s+=\"<circle cx='\"+cx.toFixed(1)+\"' cy='\"+cy.toFixed(1)+\"' r='2.5' fill='var(--cyan)' stroke='var(--bg)' stroke-width='1'/>\";});}\n"
         "s+=\"<line x1='\"+PL+\"' y1='\"+PT+\"' x2='\"+PL+\"' y2='\"+(PT+CH)+\"' stroke='var(--muted)' stroke-width='1'/>\";\n"
         "s+=\"<line x1='\"+PL+\"' y1='\"+(PT+CH)+\"' x2='\"+(PL+CW)+\"' y2='\"+(PT+CH)+\"' stroke='var(--muted)' stroke-width='1'/>\";\n"
         "s+=\"<g font-size='9' font-family='monospace' fill='var(--muted)'>\";\n"
         "for(var yv=-ya.max;yv<=ya.max;yv+=ya.step){if(Math.abs(yv)<ya.step*0.01)continue;var gy=yp(yv);\n"
         "s+=\"<line x1='\"+(PL-3)+\"' y1='\"+gy.toFixed(1)+\"' x2='\"+PL+\"' y2='\"+gy.toFixed(1)+\"' stroke='var(--muted)' stroke-width='0.5'/>\";\n"
         "s+=\"<text x='\"+(PL-5)+\"' y='\"+(gy+3).toFixed(1)+\"' text-anchor='end'>\"+fmt(yv,ya.dec)+\"</text>\";}\n"
         "s+=\"</g>\";\n"
         "if(showCumul){s+=\"<line x1='\"+(PL+CW)+\"' y1='\"+PT+\"' x2='\"+(PL+CW)+\"' y2='\"+(PT+CH)+\"' stroke='var(--cyan)' stroke-width='0.5' opacity='.4'/>\";\n"
         "s+=\"<g font-size='9' font-family='monospace' fill='var(--cyan)'>\";\n"
         "for(var yv=-ca.max;yv<=ca.max;yv+=ca.step){if(Math.abs(yv)<ca.step*0.01)continue;var gy=yp2(yv);\n"
         "s+=\"<line x1='\"+(PL+CW)+\"' y1='\"+gy.toFixed(1)+\"' x2='\"+(PL+CW+3)+\"' y2='\"+gy.toFixed(1)+\"' stroke='var(--cyan)' stroke-width='0.5'/>\";\n"
         "s+=\"<text x='\"+(PL+CW+5)+\"' y='\"+(gy+3).toFixed(1)+\"' text-anchor='start'>\"+fmt(yv,ca.dec)+\"</text>\";}\n"
         "s+=\"</g>\";}\n"
         "s+=\"<text x='\"+(PL/2)+\"' y='\"+(PT+CH/2)+\"' text-anchor='middle' font-size='9' font-family='monospace' fill='var(--muted)' transform='rotate(-90 \"+(PL/2)+\" \"+(PT+CH/2)+\")'>\"+('Surplus / Deficit ('+bUnit+')')+\"</text>\";\n"
         "if(showCumul)s+=\"<text x='\"+(W-PR/2)+\"' y='\"+(PT+CH/2)+\"' text-anchor='middle' font-size='9' font-family='monospace' fill='var(--cyan)' transform='rotate(90 \"+(W-PR/2)+\" \"+(PT+CH/2)+\")'>\"+bCumLbl+\"</text>\";\n"
         "var lg=$('chart-legend');if(lg){var lh='';\n"
         "lh+='<span class=lg><span class=\"swatch-band\" style=\"background:linear-gradient(var(--green),transparent);opacity:.6\"></span>Surplus<span class=tip>Daily net energy surplus: solar generation exceeds estimated consumption. Whisker lines show the 95% CI combining both forecast uncertainty and usage variability. Bar height is the point estimate.</span></span>';\n"
         "lh+='<span class=lg><span class=\"swatch-band\" style=\"background:linear-gradient(transparent,var(--orange));opacity:.6\"></span>Deficit<span class=tip>Daily net energy deficit: estimated consumption exceeds solar generation. The battery must cover this shortfall. Whiskers show 95% CI.</span></span>';\n"
         "if(showCumul)lh+='<span class=lg><span class=swatch style=\"border-top:1.5px dashed var(--cyan)\"></span>Cumul. net<span class=tip>Running total of daily surplus/deficit over the forecast period. A falling line means cumulative energy loss; rising means net gain. Useful for seeing if the battery is trending toward full or empty.</span></span>';\n"
         "lg.innerHTML=lh;}\n"
         "s+=\"<rect id='balance-overlay' x='\"+PL+\"' y='\"+PT+\"' width='\"+CW+\"' height='\"+CH+\"' fill='transparent' style='cursor:crosshair'/>\";\n"
         "el.setAttribute('viewBox','0 0 '+W+' '+H);el.innerHTML=s;\n"
         "var overlay=document.getElementById('balance-overlay');\n"
         "if(overlay&&tt){\n"
         "var wrap=$('solar-chart-wrap');\n"
         "overlay.addEventListener('mousemove',function(e){\n"
         "var rect=el.getBoundingClientRect();\n"
         "var scaleX=W/rect.width;\n"
         "var svgX=(e.clientX-rect.left)*scaleX;\n"
         "var best=-1;\n"
         "days.forEach(function(d,i){var x=bx(i);if(svgX>=x&&svgX<=x+bw)best=i;});\n"
         "if(best<0){tt.style.display='none';return;}\n"
         "var d=days[best];\n"
         "var dec=solarChartUnit==='pct'?1:2;\n"
         "var htm='<div class=tt-date>'+fmtDate(d.date)+'</div>';\n"
         "htm+='<div class=tt-row>Solar: <span class=tt-val>'+fmt(bc(d.kwh),dec)+' '+bUnit+'</span></div>';\n"
         "htm+='<div class=tt-row style=\"color:var(--orange)\">Usage: '+fmt(bc(d.usage_kwh),dec)+' '+bUnit+' ('+fmt(bc(d.usage_kwh_lo),dec)+'\\u2013'+fmt(bc(d.usage_kwh_hi),dec)+')</div>';\n"
         "var color=(d.surplus_kwh||0)>=0?'var(--green)':'var(--orange)';\n"
         "htm+='<div class=tt-row style=\"color:'+color+'\">Net: '+fmt(bc(d.surplus_kwh),dec)+' '+bUnit+'</div>';\n"
         "htm+='<div class=tt-row>95% CI: '+fmt(bc(d.surplus_kwh_lo),dec)+' to '+fmt(bc(d.surplus_kwh_hi),dec)+' '+bUnit+'</div>';\n"
         "if(showCumul)htm+='<div class=tt-row style=\"color:var(--cyan)\">Cumul: '+fmt(cumArr[best],dec)+' '+bUnit+'</div>';\n"
         "tt.innerHTML=htm;\n"
         "var px=e.clientX-wrap.getBoundingClientRect().left;\n"
         "var py=e.clientY-wrap.getBoundingClientRect().top;\n"
         "tt.style.left=(px+12)+'px';tt.style.top=(py-10)+'px';tt.style.display='block';\n"
         "if(px+tt.offsetWidth+12>wrap.offsetWidth)tt.style.left=(px-tt.offsetWidth-8)+'px';\n"
         "});\n"
         "overlay.addEventListener('mouseleave',function(){tt.style.display='none';});\n"
         "}}\n"
         "function applyForecast(d){if(!d||!d.valid){$('week-kwh').textContent='—';$('days-full').textContent='—';$('best-day').textContent=d?(d.error||'—'):'—';var tb=$('daily-table');if(tb)tb.innerHTML='<tr><td colspan=7 class=warn>'+(d?d.error:'No data')+'</td></tr>';return}\n"
         "$('week-kwh').textContent=fmt(d.week_total_kwh,2);$('days-full').textContent=d.days_to_full>0?d.days_to_full:'—';$('best-day').textContent=d.best_day?fmtDate(d.best_day):'—';\n"
         "if(d.solar_perf_coeff>0){var pct=Math.round(d.solar_perf_coeff*100);$('solar-coeff').textContent=pct+'%';$('solar-coeff-detail').textContent=d.solar_perf_samples+' samples (30d)'}else{$('solar-coeff').textContent='—';$('solar-coeff-detail').textContent='collecting data'}\n"
         "var badge=$('realistic-badge');if(badge){if(d.realistic){badge.style.display='';badge.textContent=d.realistic_from_history?'using measured charge data':'using default LiFePO4 taper'}else{badge.style.display='none'}}\n"
         "if(d.daily&&d.daily.length){lastDailyData=d;if(d.slots)lastSlots=d.slots;var cb=$('show-extended');if(cb)cb.checked=getSetting('show-extended','0')==='1';renderDaily();renderActiveChart()}else{var tb=$('daily-table');if(tb)tb.innerHTML='<tr><td colspan=7 class=dim>No data</td></tr>'}}\n"
         "function toggleRealistic(){var cb=$('show-realistic');if(cb)applySetting('show-realistic',cb.checked?'1':'0');var want=cb&&cb.checked;var cached=want?cachedRealistic:cachedNominal;if(cached){applyForecast(cached);return}loadAll();}\n"
         "function refreshWeather(){var m=$('refresh-msg');if(m)m.textContent='Refreshing…';fetch('/api/weather_refresh').then(function(r){return r.json()}).then(function(d){if(m)m.textContent=d.ok?'Done! Reloading…':'Error: '+d.message;if(d.ok)setTimeout(loadAll,500)}).catch(function(e){if(m)m.textContent='Error: '+e.message});}\n"
         "function loadAll(){var t=15000;var base=window.location.origin||(window.location.protocol+'//'+window.location.host);function to(url){return Promise.race([fetch(url),new Promise(function(_,rej){setTimeout(function(){rej(new Error('timeout'))},t)})]).then(function(r){return r.ok?r.json():Promise.reject(new Error(r.status))})}\n"
         "var rlCb=$('show-realistic');var wantReal=rlCb&&rlCb.checked;\n"
         "Promise.all([to(base+'/api/solar_forecast_week?both=1'),to(base+'/api/solar_simulation'),to(base+'/api/solar_forecast')]).then(function(arr){\n"
         "var both=arr[0];cachedNominal=both.nominal;cachedRealistic=both.realistic;var sim=arr[1],fore=arr[2];\n"
         "applyForecast(wantReal?cachedRealistic:cachedNominal);\n"
         "$('today-wh').textContent=sim.expected_today_wh>0?fmt(sim.expected_today_wh/1000,2)+' kilowatt-hours':'—';$('tomorrow-wh').textContent=fore.valid?fmt(fore.tomorrow_generation_wh/1000,1)+' kilowatt-hours':'—';$('bat-proj').textContent=sim.battery_projection||fore.expected_battery_state||'—';\n"
         "}).catch(function(err){try{console.error('Solar loadAll failed:',err);}catch(e){}var t=$('daily-table');if(t)t.innerHTML='<tr><td colspan=7 class=warn>Error loading data. Check solar_enabled and weather_api_key in Settings.</td></tr>';$('week-kwh').textContent='—';$('days-full').textContent='—';$('best-day').textContent='—';$('today-wh').textContent='—';$('tomorrow-wh').textContent='—';$('bat-proj').textContent='—'})}\n"
         "function fmtOptTime(s){if(!s||s==='\u2014')return s;var m=s.match(/^(\\d{1,2}):(\\d{2})$/);if(!m)return s;var h=parseInt(m[1],10),mn=parseInt(m[2],10);var loc=getLocale();if(getSetting('time','24')==='12'){var h12=h%12;if(h12===0)h12=12;return h12+':'+(mn<10?'0':'')+mn+(h<12?' AM':' PM')}try{var d=new Date(2000,0,1,h,mn);return d.toLocaleTimeString(loc,{hour:'2-digit',minute:'2-digit',hour12:getSetting('time','24')==='12'})}catch(e){return h+':'+(mn<10?'0':'')+mn}}\n"
         "function initSettings(){if(!lmSettings)lmSettings={theme:'',time:'24','show-extended':'0','show-realistic':'0',locale:''};var t=getSetting('theme','');if(!t)t=window.matchMedia('(prefers-color-scheme:light)').matches?'light':'dark';lmSettings.theme=t;document.documentElement.classList.toggle('light',t==='light');var b=$('thm-btn');if(b)b.textContent=t==='light'?'\u263d':'\u2600';var st=$('set-theme');if(st)st.value=t;var se=$('set-time');if(se)se.value=getSetting('time','24');var sl=$('set-locale');if(sl)sl.value=getSetting('locale','auto');var sr=$('show-realistic');if(sr)sr.checked=getSetting('show-realistic','0')==='1'}\n"
         "function applyTheme(t){document.documentElement.classList.toggle('light',t==='light');var b=$('thm-btn');if(b)b.textContent=t==='light'?'\u263d':'\u2600';if(!lmSettings)lmSettings={};lmSettings.theme=t;fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(lmSettings)}).catch(function(){});var st=$('set-theme');if(st)st.value=t}\n"
         "function toggleTheme(){applyTheme(document.documentElement.classList.contains('light')?'dark':'light')}\n"
         "function toggleSettings(){var m=$('settings-modal');if(!m)return;m.style.display=m.style.display==='none'?'flex':'none';if(m.style.display==='flex'){var st=$('set-theme');if(st)st.value=getSetting('theme','dark');var se=$('set-time');if(se)se.value=getSetting('time','24');var sl=$('set-locale');if(sl)sl.value=getSetting('locale','auto')}}\n"
         "function applySetting(k,v){if(!lmSettings)lmSettings={};lmSettings[k]=v;fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(lmSettings)}).catch(function(){});if(k==='theme')applyTheme(v);else if(k==='time'||k==='locale')loadAll()}\n"
         "(function(){var n=document.querySelectorAll('nav a[href^=\"/\"]');for(var i=0;i<n.length;i++){n[i].addEventListener('click',function(e){if(e.ctrlKey||e.metaKey||e.shiftKey)return;var h=this.getAttribute('href');if(!h)return;e.preventDefault();location.href=h})}})();\n"
         "initSettings();loadAll();setInterval(loadAll,300000)\n"
         "</script></body></html>";
    return o;
}

std::string HttpServer::html_settings_page(Database* db) {
    auto g = [db](const std::string& k, const std::string& d) {
        if (!db) return d;
        std::string v = db->get_setting(k);
        return v.empty() ? d : v;
    };
    auto esc = [](const std::string& s) {
        std::string r;
        for (char c : s) {
            if (c == '"') r += "&quot;";
            else if (c == '&') r += "&amp;";
            else if (c == '<') r += "&lt;";
            else r += c;
        }
        return r;
    };
    std::string o;
    o.reserve(8000);
    std::string theme = g("theme", "");
    if (theme.empty()) theme = "dark";
    o += R"HTML(<!DOCTYPE html><html lang="en" class=")HTML";
    o += (theme == "light") ? "light" : "";
    o += R"HTML("><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Settings — limonitor</title>
<link rel="prefetch" href="/"><link rel="prefetch" href="/solar">
<style>
:root{--bg:#0d0d11;--card:#16161c;--border:#2e2e3a;--text:#e0e0ea;--muted:#9090a8;--green:#4ade80;--input-bg:#1a1a22}
html.light{--bg:#f2f3f7;--card:#fff;--border:#d4d4e0;--text:#1a1a2c;--muted:#5a5a72;--green:#16a34a;--input-bg:#fafafa}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--text);font-size:14px;line-height:1.5;padding:1.5rem;max-width:560px;margin:0 auto;transition:background .2s,color .2s}
nav{display:flex;align-items:center;gap:.5rem;margin-bottom:1.5rem;padding-bottom:1rem;border-bottom:1px solid var(--border)}
nav a{color:var(--muted);text-decoration:none;font-size:.9rem;padding:.35rem .6rem;border-radius:6px;transition:color .15s,background .15s}
nav a:hover{color:var(--text);background:var(--card)}
nav a.active{color:var(--green);font-weight:600}
nav .spacer{flex:1}
nav .thm{background:var(--card);border:1px solid var(--border);color:var(--text);padding:.35rem .6rem;border-radius:6px;cursor:pointer;font-size:.85rem}
h1{font-size:1.35rem;font-weight:600;color:var(--green);margin-bottom:.25rem}
.sub{font-size:.8rem;color:var(--muted);margin-bottom:1.25rem}
.form-wrap{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:1.25rem;margin-bottom:1rem;box-shadow:0 1px 3px rgba(0,0,0,.08)}
.section-title{font-size:.65rem;text-transform:uppercase;letter-spacing:.12em;color:var(--muted);margin:1rem 0 .5rem;font-weight:600}
.section-title:first-child{margin-top:0}
.row{margin-bottom:.75rem}
.row:last-of-type{margin-bottom:0}
.row label{display:block;font-size:.8rem;color:var(--muted);margin-bottom:.3rem;font-weight:500}
.row input,.row select{width:100%;padding:.5rem .6rem;border:1px solid var(--border);border-radius:6px;background:var(--input-bg);color:var(--text);font-family:inherit;font-size:.9rem;transition:border-color .15s}
.row input:focus,.row select:focus{outline:none;border-color:var(--green)}
.row input[type=checkbox]{width:auto;margin-right:.5rem;vertical-align:middle}
.btn{background:var(--green);color:#fff;border:none;padding:.55rem 1.25rem;border-radius:6px;cursor:pointer;font-family:inherit;font-size:.9rem;font-weight:600;margin-top:.5rem}
.btn:hover{opacity:.92;transform:translateY(-1px)}
.msg{font-size:.8rem;color:var(--muted);margin-top:.6rem}
.err{color:#dc2626}
</style></head><body>
<nav><a href="/">Dashboard</a><a href="/solar">Solar</a><a href="/settings" class="active">Settings</a><span class="spacer"></span><button class="thm" id="thm-btn" onclick="toggleTheme()" title="Toggle theme">)HTML";
    o += (theme == "light") ? "&#9788;" : "&#263d;";
    o += R"HTML(</button></nav>
<h1>Settings</h1>
<p class="sub">Application configuration. Restart limonitor after saving to apply hardware changes.</p>
<form id="cfg-form" class="form-wrap">
<div class="section-title">BLE</div>
<div class="row"><label>Device name</label><input type="text" name="device_name" placeholder="e.g. L-12100BNNA70" value=")HTML";
    o += esc(g("device_name", ""));
    o += "\"></div><div class=\"row\"><label>Device address (MAC)</label><input type=\"text\" name=\"device_address\" placeholder=\"AA:BB:CC:DD:EE:FF\" value=\"";
    o += esc(g("device_address", ""));
    o += "\"></div><div class=\"row\"><label>Adapter path</label><input type=\"text\" name=\"adapter_path\" value=\"";
    o += esc(g("adapter_path", "/org/bluez/hci0"));
    o += "\"></div><div class=\"section-title\">HTTP</div><div class=\"row\"><label>Port</label><input type=\"number\" name=\"http_port\" min=\"1\" max=\"65535\" value=\"";
    o += esc(g("http_port", "8080"));
    o += "\"></div><div class=\"row\"><label>Bind address</label><input type=\"text\" name=\"http_bind\" value=\"";
    o += esc(g("http_bind", "0.0.0.0"));
    o += "\"></div><div class=\"row\"><label>Log file</label><input type=\"text\" name=\"log_file\" placeholder=\"(stderr only)\" value=\"";
    o += esc(g("log_file", ""));
    o += "\"></div><div class=\"row\"><label>Verbose (DEBUG logs)</label><input type=\"checkbox\" name=\"verbose\" ";
    o += (g("verbose", "0") == "1" || g("verbose", "0") == "true") ? "checked" : "";
    o += "></div><div class=\"section-title\">EpicPowerGate / Serial</div><div class=\"row\"><label>Serial device</label><input type=\"text\" name=\"serial_device\" placeholder=\"/dev/ttyACM0\" value=\"";
    o += esc(g("serial_device", ""));
    o += "\"></div><div class=\"row\"><label>Serial baud</label><input type=\"number\" name=\"serial_baud\" value=\"";
    o += esc(g("serial_baud", "115200"));
    o += "\"></div><div class=\"row\"><label>Remote PwrGate (host:port)</label><input type=\"text\" name=\"pwrgate_remote\" placeholder=\"ham-pi:8081\" value=\"";
    o += esc(g("pwrgate_remote", ""));
    o += "\"></div><div class=\"section-title\">Database & polling</div><div class=\"row\"><label>Poll interval (seconds)</label><input type=\"number\" name=\"poll_interval\" min=\"1\" value=\"";
    o += esc(g("poll_interval", "5"));
    o += "\"></div><div class=\"row\"><label>DB path</label><input type=\"text\" name=\"db_path\" placeholder=\"default\" value=\"";
    o += esc(g("db_path", ""));
    o += "\"></div><div class=\"row\"><label>DB write interval (seconds)</label><input type=\"number\" name=\"db_interval\" min=\"0\" value=\"";
    o += esc(g("db_interval", "60"));
    o += "\"></div><div class=\"section-title\">Battery</div><div class=\"row\"><label>Purchase date (YYYY-MM-DD)</label><input type=\"text\" name=\"battery_purchased\" placeholder=\"2024-03-15\" value=\"";
    o += esc(g("battery_purchased", ""));
    o += "\"></div><div class=\"row\"><label>Rated capacity (Ah, 0=auto)</label><input type=\"number\" name=\"rated_capacity_ah\" min=\"0\" step=\"0.1\" value=\"";
    o += esc(g("rated_capacity_ah", "0"));
    o += "\"></div><div class=\"row\"><label>TX threshold (amps)</label><input type=\"number\" name=\"tx_threshold\" min=\"0\" step=\"0.1\" value=\"";
    o += esc(g("tx_threshold", "1.0"));
    o += "\"></div><div class=\"section-title\">Solar & weather</div><div class=\"row\"><label>Solar enabled</label><input type=\"checkbox\" name=\"solar_enabled\" ";
    o += (g("solar_enabled", "0") == "1" || g("solar_enabled", "0") == "true") ? "checked" : "";
    o += "></div><div class=\"row\"><label>Solar panel watts</label><input type=\"number\" name=\"solar_panel_watts\" min=\"0\" value=\"";
    o += esc(g("solar_panel_watts", "400"));
    o += "\"></div><div class=\"row\"><label>Solar system efficiency (0–1)</label><input type=\"number\" name=\"solar_system_efficiency\" min=\"0\" max=\"1\" step=\"0.01\" value=\"";
    o += esc(g("solar_system_efficiency", "0.75"));
    o += "\"></div><div class=\"row\"><label>Solar ZIP code</label><input type=\"text\" name=\"solar_zip_code\" value=\"";
    o += esc(g("solar_zip_code", "80112"));
    o += "\"></div><div class=\"row\"><label>Weather API key</label><input type=\"text\" name=\"weather_api_key\" placeholder=\"OpenWeather\" value=\"";
    o += esc(g("weather_api_key", ""));
    o += "\"></div><div class=\"row\"><label>Weather ZIP code</label><input type=\"text\" name=\"weather_zip_code\" value=\"";
    o += esc(g("weather_zip_code", "80112"));
    o += "\"></div><div class=\"row\"><label>Refresh weather cache</label>"
         "<button type=\"button\" class=\"btn\" style=\"padding:.35rem .8rem;font-size:.8rem\" onclick=\"refreshWx()\">&#8635; Fetch fresh data</button>"
         " <span id=\"wx-msg\" style=\"font-size:.8rem;color:var(--muted)\"></span></div>"
         "<div class=\"row\"><label>Daemon mode</label><input type=\"checkbox\" name=\"daemon\" ";
    o += (g("daemon", "0") == "1" || g("daemon", "0") == "true") ? "checked" : "";
    o += "></div><button type=\"submit\" class=\"btn\">Save</button><div id=\"msg\" class=\"msg\"></div></form><script>\n"
         "(function(){var n=document.querySelectorAll('nav a[href^=\"/\"]');for(var i=0;i<n.length;i++){n[i].addEventListener('click',function(e){if(e.ctrlKey||e.metaKey||e.shiftKey)return;var h=this.getAttribute('href');if(!h)return;e.preventDefault();location.href=h})}})();\n"
         "document.getElementById('cfg-form').onsubmit=function(e){e.preventDefault();var f=e.target;var o={settings_initialized:'1'};\n"
         "['device_name','device_address','adapter_path','http_port','http_bind','log_file','verbose','serial_device','serial_baud','pwrgate_remote','poll_interval','db_path','db_interval','battery_purchased','rated_capacity_ah','tx_threshold','solar_enabled','solar_panel_watts','solar_system_efficiency','solar_zip_code','weather_api_key','weather_zip_code','daemon'].forEach(function(k){\n"
         "var el=f.elements[k];if(el)o[k]=el.type==='checkbox'?(el.checked?'1':'0'):el.value\n"
         "});\nfetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)})\n"
         ".then(function(r){if(r.ok){document.getElementById('msg').textContent='Saved. Restart limonitor to apply.';document.getElementById('msg').className='msg'}else{throw new Error()}})\n"
         ".catch(function(){document.getElementById('msg').textContent='Save failed.';document.getElementById('msg').className='msg err'})\n"
         "}\n"
         "function toggleTheme(){var isLight=document.documentElement.classList.toggle('light');var t=isLight?'light':'dark';document.getElementById('thm-btn').textContent=isLight?'\u263d':'\u263c';fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({theme:t})}).catch(function(){})}\n"
         "function refreshWx(){var m=document.getElementById('wx-msg');if(m)m.textContent='Refreshing…';fetch('/api/weather_refresh').then(function(r){return r.json()}).then(function(d){if(m)m.textContent=d.ok?'Done! Cache cleared. Visit Solar page to see fresh data.':'Error: '+d.message;if(m)m.style.color=d.ok?'var(--green)':'#dc2626'}).catch(function(e){if(m){m.textContent='Error: '+e.message;m.style.color='#dc2626'}})}\n"
         "</script></body></html>";
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
