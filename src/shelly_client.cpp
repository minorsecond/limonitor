#include "shelly_client.hpp"
#include "analytics.hpp"
#include "data_store.hpp"
#include "database.hpp"
#include "logger.hpp"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace shelly {

// Returns response body, or "" on failure
static std::string do_get(const std::string& host, int port, const char* path, int timeout_s = 5) {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    char portstr[8];
    std::snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host.c_str(), portstr, &hints, &res) != 0) return "";
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return ""; }
    struct timeval tv{timeout_s, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res); close(sock); return "";
    }
    freeaddrinfo(res);
    char reqbuf[256];
    std::snprintf(reqbuf, sizeof(reqbuf),
        "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host.c_str());
    send(sock, reqbuf, std::strlen(reqbuf), MSG_NOSIGNAL);
    std::string body;
    char rbuf[1024];
    ssize_t n;
    while ((n = recv(sock, rbuf, sizeof(rbuf) - 1, 0)) > 0) {
        rbuf[n] = '\0';
        body += rbuf;
    }
    close(sock);
    // Strip HTTP headers
    auto pos = body.find("\r\n\r\n");
    if (pos != std::string::npos) body = body.substr(pos + 4);
    return body;
}

// Simple JSON value extractor: finds "key":value (number or bool) in a flat JSON string
static double json_double(const std::string& body, const char* key, double def = 0) {
    std::string pat = std::string("\"") + key + "\":";
    auto pos = body.find(pat);
    if (pos == std::string::npos) return def;
    pos += pat.size();
    while (pos < body.size() && body[pos] == ' ') ++pos;
    try { return std::stod(body.substr(pos)); } catch (...) { return def; }
}

static bool json_bool(const std::string& body, const char* key, bool def = false) {
    std::string pat = std::string("\"") + key + "\":";
    auto pos = body.find(pat);
    if (pos == std::string::npos) return def;
    pos += pat.size();
    while (pos < body.size() && body[pos] == ' ') ++pos;
    if (body.substr(pos, 4) == "true") return true;
    if (body.substr(pos, 5) == "false") return false;
    return def;
}

Status get_status(Database* db) {
    Status st;
    if (!db) return st;
    std::string host = db->get_setting("shelly_host");
    if (host.empty()) return st;

    int port = 80;
    auto colon = host.find(':');
    if (colon != std::string::npos) {
        try { port = std::stoi(host.substr(colon + 1)); } catch (...) {}
        host = host.substr(0, colon);
    }

    // Try Gen2 (Plus/Pro) first: rpc endpoint — 2s timeout for live monitoring
    std::string body = do_get(host, port, "/rpc/Switch.GetStatus?id=0", 2);
    if (body.find("\"apower\"") != std::string::npos) {
        // Gen2
        st.ok = true;
        st.relay_on = json_bool(body, "output");
        st.power_w  = json_double(body, "apower");
        st.current_a = json_double(body, "current");
        st.voltage_v = json_double(body, "voltage");
        // "aenergy":{"total":kwh}
        auto ep = body.find("\"aenergy\"");
        if (ep != std::string::npos) {
            auto sub = body.substr(ep);
            st.total_kwh = json_double(sub, "total") / 1000.0; // Wh → kWh
        }
        return st;
    }

    // Gen1 fallback: /relay/0 + /meter/0
    std::string relay_body = do_get(host, port, "/relay/0", 2);
    std::string meter_body = do_get(host, port, "/meter/0", 2);
    if (relay_body.empty() && meter_body.empty()) return st;
    st.ok = true;
    st.relay_on = json_bool(relay_body, "ison");
    st.power_w  = json_double(meter_body, "power");
    st.total_kwh = json_double(meter_body, "total") / 60000.0; // Watt-minutes → kWh
    return st;
}

static bool do_request(Database* db, const char* turn) {
    if (!db) return false;
    std::string host = db->get_setting("shelly_host");
    if (host.empty()) return false;

    int port = 80;
    auto colon = host.find(':');
    if (colon != std::string::npos) {
        try { port = std::stoi(host.substr(colon + 1)); } catch (...) {}
        host = host.substr(0, colon);
    }

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    char portstr[8];
    std::snprintf(portstr, sizeof(portstr), "%d", port);

    if (getaddrinfo(host.c_str(), portstr, &hints, &res) != 0) return false;

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return false; }

    struct timeval tv{5, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        close(sock);
        return false;
    }
    freeaddrinfo(res);

    char reqbuf[256];
    std::snprintf(reqbuf, sizeof(reqbuf),
        "GET /relay/0?turn=%s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
        turn, host.c_str());
    ssize_t sent = send(sock, reqbuf, std::strlen(reqbuf), MSG_NOSIGNAL);
    char rbuf[256];
    recv(sock, rbuf, sizeof(rbuf), 0);
    close(sock);

    return sent > 0;
}

bool turn_on(Database* db) {
    bool ok = do_request(db, "on");
    if (ok) LOG_INFO("Shelly: grid turned ON");
    return ok;
}

bool turn_off(Database* db) {
    bool ok = do_request(db, "off");
    if (ok) LOG_INFO("Shelly: grid turned OFF");
    return ok;
}

void check_test_conditions(Database* db, const DataStore& store) {
    if (!db || !db->is_open()) return;

    std::string active = db->get_setting("shelly_test_active");
    if (active != "1") return;

    std::string host = db->get_setting("shelly_host");
    std::string enabled = db->get_setting("shelly_enabled");
    if (host.empty() || (enabled != "1" && enabled != "true")) return;

    auto an = store.analytics();
    auto bat_opt = store.latest();
    double soc = bat_opt && bat_opt->valid ? bat_opt->soc_pct : 0;

    // Low SoC safety: turn grid on if battery drops below threshold
    std::string low_soc_s = db->get_setting("shelly_battery_test_low_soc");
    double low_soc = 20.0;
    if (!low_soc_s.empty()) { try { low_soc = std::stod(low_soc_s); } catch (...) {} }
    if (soc > 0 && soc < low_soc) {
        LOG_WARN("Shelly: battery test auto-ended (SoC %.1f%% < %.1f%%)", soc, low_soc);
        if (turn_on(db)) {
            db->set_setting("shelly_test_active", "0");
            db->set_setting("shelly_test_start_ts", "");
        }
        return;
    }

    // Max hours safety: turn grid on after max hours
    std::string start_ts_s = db->get_setting("shelly_test_start_ts");
    std::string max_h_s = db->get_setting("shelly_battery_test_max_hours");
    double max_hours = 24.0;
    if (!max_h_s.empty()) { try { max_hours = std::stod(max_h_s); } catch (...) {} }
    if (!start_ts_s.empty()) {
        try {
            long start_ts = std::stol(start_ts_s);
            long now_ts = static_cast<long>(std::time(nullptr));
            double elapsed_h = (now_ts - start_ts) / 3600.0;
            if (elapsed_h >= max_hours) {
                LOG_WARN("Shelly: battery test auto-ended (%.1f h >= %.1f h max)", elapsed_h, max_hours);
                if (turn_on(db)) {
                    db->set_setting("shelly_test_active", "0");
                    db->set_setting("shelly_test_start_ts", "");
                }
                return;
            }
        } catch (...) {}
    }

    // Auto-end when sufficient data: avg_discharge_24h_w > 0.5 means we have 6+ hours of discharge
    std::string auto_end_s = db->get_setting("shelly_battery_test_auto");
    if (auto_end_s == "1" || auto_end_s == "true") {
        if (an.avg_discharge_24h_w > 0.5) {
            LOG_INFO("Shelly: battery test auto-ended (sufficient discharge data: %.1f W avg)", an.avg_discharge_24h_w);
            if (turn_on(db)) {
                db->set_setting("shelly_test_active", "0");
                db->set_setting("shelly_test_start_ts", "");
            }
        }
    }
}

} // namespace shelly
