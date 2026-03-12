#include "http_server.hpp"
#include "logger.hpp"
#include "ops_events.hpp"
#include "ops_util.hpp"
#include "shelly_client.hpp"
#include "analytics/extensions.hpp"
#include "testing/types.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <netdb.h>
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
static std::string jval_s(const std::string& body, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    auto p = body.find(pat);
    if (p == std::string::npos) return "";
    p = body.find(':', p);
    if (p == std::string::npos) return "";
    p++;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) p++;
    if (p >= body.size() || body[p] != '"') return "";
    auto q = body.find('"', p + 1);
    if (q == std::string::npos) return "";
    return body.substr(p + 1, q - p - 1);
}
static int jval_i(const std::string& body, const std::string& key, int def) {
    std::string pat = "\"" + key + "\"";
    auto p = body.find(pat);
    if (p == std::string::npos) return def;
    p = body.find(':', p);
    if (p == std::string::npos) return def;
    p++;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) p++;
    if (p >= body.size()) return def;
    return std::atoi(body.c_str() + p);
}
static double jval_d(const std::string& body, const std::string& key, double def) {
    std::string pat = "\"" + key + "\"";
    auto p = body.find(pat);
    if (p == std::string::npos) return def;
    p = body.find(':', p);
    if (p == std::string::npos) return def;
    p++;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) p++;
    if (p >= body.size()) return def;
    try { return std::stod(body.c_str() + p); } catch (...) { return def; }
}
static int64_t compute_next_schedule_run(const std::string& freq, int hour, int min, int day) {
    std::time_t now = std::time(nullptr);
    struct tm t{};
    localtime_r(&now, &t);
    t.tm_sec = 0; t.tm_min = min; t.tm_hour = hour;
    if (freq == "daily") {
        std::time_t cand = std::mktime(&t);
        return cand <= now ? cand + 86400 : cand;
    }
    if (freq == "weekly") {
        int wday = (day >= 0 && day <= 6) ? day : 0;
        int delta = (wday - t.tm_wday + 7) % 7;
        if (delta == 0) { t.tm_mday += 7; }
        else { t.tm_mday += delta; }
        return std::mktime(&t);
    }
    if (freq == "monthly") {
        t.tm_mday = (day >= 1 && day <= 28) ? day : 1;
        std::time_t cand = std::mktime(&t);
        if (cand <= now) { t.tm_mon += 1; cand = std::mktime(&t); }
        return cand;
    }
    return now + 86400;
}

HttpServer::HttpServer(DataStore& store, Database* db, const std::string& bind_addr, uint16_t port,
                       int poll_interval_s, testing::TestRunner* test_runner)
    : store_(store), db_(db), test_runner_(test_runner), bind_addr_(bind_addr), port_(port),
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
    shelly_poll_thread_ = std::thread(&HttpServer::shelly_poll_loop, this);
    LOG_INFO("HTTP: listening on %s:%d", bind_addr_.c_str(), port_);
    return true;
}

void HttpServer::stop() {
    running_ = false;
    if (listen_fd_ >= 0) { shutdown(listen_fd_, SHUT_RDWR); close(listen_fd_); listen_fd_ = -1; }
    if (thread_.joinable()) thread_.join();
    if (shelly_poll_thread_.joinable()) shelly_poll_thread_.join();  // exits within 1s
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
        // Handle each connection in its own thread so a slow request (e.g. Shelly
        // network I/O or a heavy DB query) doesn't stall every other client.
        std::thread([this, client]() {
            handle(client);
            close(client);
        }).detach();
    }
}

void HttpServer::handle(int fd) {
    char buf[8192] = {};
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    std::string req(buf, static_cast<size_t>(n));

    // For POST, read full body if Content-Length exceeds what we have
    if (req.size() >= 4 && (req.compare(0, 4, "POST") == 0 || req.compare(0, 4, "post") == 0)) {
        size_t cl_pos = req.find("Content-Length:");
        if (cl_pos != std::string::npos) {
            cl_pos = req.find_first_not_of(" \t", cl_pos + 15);
            if (cl_pos != std::string::npos) {
                size_t cl_end = req.find_first_of("\r\n", cl_pos);
                if (cl_end != std::string::npos) {
                    try {
                        size_t content_len = std::stoul(req.substr(cl_pos, cl_end - cl_pos));
                        size_t body_start = req.find("\r\n\r\n");
                        if (body_start != std::string::npos) body_start += 4;
                        else { body_start = req.find("\n\n"); if (body_start != std::string::npos) body_start += 2; }
                        size_t body_so_far = (body_start != std::string::npos && body_start < req.size())
                            ? (req.size() - body_start) : 0;
                        while (body_so_far < content_len && req.size() < 65536) {
                            char extra[4096];
                            ssize_t nr = recv(fd, extra, sizeof(extra), 0);
                            if (nr <= 0) break;
                            req.append(extra, static_cast<size_t>(nr));
                            body_so_far += static_cast<size_t>(nr);
                        }
                    } catch (...) {}
                }
            }
        }
    }

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

    if (method == "POST" && path == "/api/shelly/relay") {
        std::string body;
        { auto p = req.find("\r\n\r\n"); if (p != std::string::npos) body = req.substr(p + 4);
          else { p = req.find("\n\n"); if (p != std::string::npos) body = req.substr(p + 2); } }
        std::string reason = jval_s(body, "reason");
        while (!reason.empty() && (reason.back() == ' ' || reason.back() == '\t' || reason.back() == '\r' || reason.back() == '\n')) reason.pop_back();
        while (!reason.empty() && (reason.front() == ' ' || reason.front() == '\t')) reason.erase(0, 1);
        if (reason.empty()) {
            send_response(fd, 400, "application/json", "{\"ok\":false,\"error\":\"reason required\"}\n");
            return;
        }
        std::string turn = "off";
        if (body.find("\"turn\":\"on\"") != std::string::npos || body.find("\"turn\": \"on\"") != std::string::npos)
            turn = "on";
        bool is_test = (body.find("\"test\":true") != std::string::npos || body.find("\"test\": true") != std::string::npos);
        std::string host = db_ ? db_->get_setting("shelly_host") : "";
        if (host.empty()) {
            send_response(fd, 400, "application/json", "{\"ok\":false,\"error\":\"Shelly not configured\"}\n");
            return;
        }
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
        if (getaddrinfo(host.c_str(), portstr, &hints, &res) != 0) {
            send_response(fd, 502, "application/json", "{\"ok\":false,\"error\":\"Cannot resolve Shelly host\"}\n");
            return;
        }
        int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock < 0) { freeaddrinfo(res);
            send_response(fd, 502, "application/json", "{\"ok\":false,\"error\":\"Socket failed\"}\n"); return; }
        struct timeval tv{5, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
            freeaddrinfo(res); close(sock);
            send_response(fd, 502, "application/json", "{\"ok\":false,\"error\":\"Cannot connect to Shelly\"}\n");
            return;
        }
        freeaddrinfo(res);
        char reqbuf[256];
        std::snprintf(reqbuf, sizeof(reqbuf),
            "GET /relay/0?turn=%s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
            turn.c_str(), host.c_str());
        ssize_t sent = send(sock, reqbuf, strlen(reqbuf), MSG_NOSIGNAL);
        char rbuf[512];
        recv(sock, rbuf, sizeof(rbuf), 0);
        close(sock);
        if (sent > 0) {
            if (db_) {
                if (turn == "off" && is_test) {
                    db_->set_setting("shelly_test_active", "1");
                    db_->set_setting("shelly_test_start_ts", std::to_string(static_cast<long>(std::time(nullptr))));
                } else if (turn == "on") {
                    db_->set_setting("shelly_test_active", "0");
                    db_->set_setting("shelly_test_start_ts", "");
                }
                auto snap_opt = store_.latest();
                auto pg_opt = store_.latest_pwrgate();
                OpsEvent ev;
                ev.timestamp = std::chrono::system_clock::now();
                ev.type = (turn == "off") ? (is_test ? "grid_test_start" : "maintenance_start") : (is_test ? "grid_test_end" : "maintenance_end");
                ev.subtype = is_test ? "test" : "maintenance";
                ev.message = (turn == "off") ? ("Grid turned off" + (reason.empty() ? "" : ": " + reason)) : ("Grid turned on" + (reason.empty() ? "" : ": " + reason));
                ev.notes = reason;
                ev.metadata_json = build_ops_snapshot_json(&snap_opt, &pg_opt);
                db_->insert_ops_event(ev);
            }
            send_response(fd, 200, "application/json", "{\"ok\":true,\"turn\":\"" + turn + "\"}\n");
        } else {
            send_response(fd, 502, "application/json", "{\"ok\":false,\"error\":\"Shelly request failed\"}\n");
        }
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

    if (method == "POST" && path == "/api/maintenance/start") {
        if (!db_) { send_response(fd, 503, "application/json", "{\"ok\":false,\"error\":\"Database unavailable\"}\n"); return; }
        auto body_start = req.find("\r\n\r\n");
        if (body_start != std::string::npos) body_start += 4;
        else { body_start = req.find("\n\n"); if (body_start != std::string::npos) body_start += 2; }
        std::string body = (body_start != std::string::npos && body_start < req.size()) ? req.substr(body_start) : "";
        std::string reason = jval_s(body, "reason");
        while (!reason.empty() && (reason.back() == ' ' || reason.back() == '\t' || reason.back() == '\r' || reason.back() == '\n')) reason.pop_back();
        while (!reason.empty() && (reason.front() == ' ' || reason.front() == '\t')) reason.erase(0, 1);
        if (reason.empty()) {
            send_response(fd, 400, "application/json", "{\"ok\":false,\"error\":\"reason required\"}\n");
            return;
        }
        std::string expected = jval_s(body, "expected_duration");
        std::string notes = jval_s(body, "notes");
        auto now = std::chrono::system_clock::now();
        db_->set_setting("maintenance_mode", "1");
        db_->set_setting("maintenance_start_time", std::to_string(std::chrono::system_clock::to_time_t(now)));
        db_->set_setting("maintenance_end_time", "");
        auto snap_opt = store_.latest();
        auto pg_opt = store_.latest_pwrgate();
        OpsEvent ev;
        ev.timestamp = now;
        ev.type = "maintenance_start";
        ev.subtype = "maintenance";
        ev.message = "Maintenance started" + (reason.empty() ? "" : ": " + reason);
        ev.notes = notes;
        ev.metadata_json = build_ops_snapshot_json(&snap_opt, &pg_opt);
        if (!expected.empty()) {
            if (ev.metadata_json == "{}")
                ev.metadata_json = "{\"expected_duration\":" + jstr(expected) + "}";
            else {
                ev.metadata_json.pop_back();
                ev.metadata_json += ",\"expected_duration\":" + jstr(expected) + "}";
            }
        }
        db_->insert_ops_event(ev);
        send_response(fd, 200, "application/json", "{\"ok\":true}\n");
        return;
    }

    if (method == "POST" && path == "/api/maintenance/end") {
        if (!db_) { send_response(fd, 503, "application/json", "{\"ok\":false,\"error\":\"Database unavailable\"}\n"); return; }
        auto now = std::chrono::system_clock::now();
        db_->set_setting("maintenance_mode", "0");
        db_->set_setting("maintenance_end_time", std::to_string(std::chrono::system_clock::to_time_t(now)));
        auto snap_opt = store_.latest();
        auto pg_opt = store_.latest_pwrgate();
        OpsEvent ev;
        ev.timestamp = now;
        ev.type = "maintenance_end";
        ev.subtype = "maintenance";
        ev.message = "Maintenance ended";
        ev.metadata_json = build_ops_snapshot_json(&snap_opt, &pg_opt);
        db_->insert_ops_event(ev);
        send_response(fd, 200, "application/json", "{\"ok\":true}\n");
        return;
    }

    if (method == "POST" && path == "/api/note") {
        if (!db_) { send_response(fd, 503, "application/json", "{\"ok\":false,\"error\":\"Database unavailable\"}\n"); return; }
        auto body_start = req.find("\r\n\r\n");
        if (body_start != std::string::npos) body_start += 4;
        else { body_start = req.find("\n\n"); if (body_start != std::string::npos) body_start += 2; }
        std::string body = (body_start != std::string::npos && body_start < req.size()) ? req.substr(body_start) : "";
        std::string message = jval_s(body, "message");
        if (message.empty()) { send_response(fd, 400, "application/json", "{\"ok\":false,\"error\":\"message required\"}\n"); return; }
        auto snap_opt = store_.latest();
        auto pg_opt = store_.latest_pwrgate();
        OpsEvent ev;
        ev.timestamp = std::chrono::system_clock::now();
        ev.type = "note";
        ev.message = message;
        ev.metadata_json = build_ops_snapshot_json(&snap_opt, &pg_opt);
        db_->insert_ops_event(ev);
        send_response(fd, 200, "application/json", "{\"ok\":true}\n");
        return;
    }

    if (method == "POST" && path == "/api/event/reclassify") {
        if (!db_) { send_response(fd, 503, "application/json", "{\"ok\":false,\"error\":\"Database unavailable\"}\n"); return; }
        auto body_start = req.find("\r\n\r\n");
        if (body_start != std::string::npos) body_start += 4;
        else { body_start = req.find("\n\n"); if (body_start != std::string::npos) body_start += 2; }
        std::string body = (body_start != std::string::npos && body_start < req.size()) ? req.substr(body_start) : "";
        std::string id_s = jval_s(body, "id");
        std::string subtype = jval_s(body, "subtype");
        if (id_s.empty() || subtype.empty()) { send_response(fd, 400, "application/json", "{\"ok\":false,\"error\":\"id and subtype required\"}\n"); return; }
        int64_t id = 0;
        try { id = std::stoll(id_s); } catch (...) { send_response(fd, 400, "application/json", "{\"ok\":false,\"error\":\"invalid id\"}\n"); return; }
        if (db_->update_ops_event_subtype(id, subtype))
            send_response(fd, 200, "application/json", "{\"ok\":true}\n");
        else
            send_response(fd, 404, "application/json", "{\"ok\":false,\"error\":\"event not found\"}\n");
        return;
    }

    if (method == "POST" && path == "/api/tests/start") {
        if (!test_runner_) { send_response(fd, 503, "application/json", "{\"ok\":false,\"error\":\"Test runner unavailable\"}\n"); return; }
        if (test_runner_->is_running()) { send_response(fd, 409, "application/json", "{\"ok\":false,\"error\":\"Test already running\"}\n"); return; }
        auto body_start = req.find("\r\n\r\n");
        if (body_start != std::string::npos) body_start += 4;
        else { body_start = req.find("\n\n"); if (body_start != std::string::npos) body_start += 2; }
        std::string body = (body_start != std::string::npos && body_start < req.size()) ? req.substr(body_start) : "";
        std::string type_s = jval_s(body, "test_type");
        if (type_s.empty()) type_s = "capacity";
        auto type = testing::parse_test_type(type_s.c_str());
        int64_t id = test_runner_->start_test(type);
        if (id > 0) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "{\"ok\":true,\"test_id\":%lld}\n", (long long)id);
            send_response(fd, 200, "application/json", buf);
        } else {
            send_response(fd, 400, "application/json", "{\"ok\":false,\"error\":\"Cannot start test (check SOC, maintenance mode)\"}\n");
        }
        return;
    }

    if (method == "POST" && path == "/api/tests/stop") {
        if (!test_runner_) { send_response(fd, 503, "application/json", "{\"ok\":false,\"error\":\"Test runner unavailable\"}\n"); return; }
        test_runner_->stop_test();
        send_response(fd, 200, "application/json", "{\"ok\":true}\n");
        return;
    }

    if (method == "POST" && path.compare(0, 11, "/api/tests/") == 0 && path.find("/notes") != std::string::npos) {
        size_t notes_pos = path.find("/notes");
        size_t id_start = 11;
        size_t id_end = path.find('/', id_start);
        if (id_end != std::string::npos && id_end <= notes_pos) {
            std::string id_part = path.substr(id_start, id_end - id_start);
            int64_t id = 0;
            try { id = std::stoll(id_part); } catch (...) {}
            if (id > 0 && db_) {
                auto body_start = req.find("\r\n\r\n");
                if (body_start != std::string::npos) body_start += 4;
                else { body_start = req.find("\n\n"); if (body_start != std::string::npos) body_start += 2; }
                std::string body = (body_start != std::string::npos && body_start < req.size()) ? req.substr(body_start) : "";
                std::string notes = jval_s(body, "notes");
                if (db_->update_test_run_notes(id, notes))
                    send_response(fd, 200, "application/json", "{\"ok\":true}\n");
                else
                    send_response(fd, 404, "application/json", "{\"ok\":false,\"error\":\"test not found\"}\n");
            } else {
                send_response(fd, 400, "application/json", "{\"ok\":false,\"error\":\"invalid id\"}\n");
            }
            return;
        }
    }

    if (method == "POST" && path == "/api/tests/safety_limits") {
        std::string body;
        { auto p = req.find("\r\n\r\n"); if (p != std::string::npos) body = req.substr(p + 4);
          else { p = req.find("\n\n"); if (p != std::string::npos) body = req.substr(p + 2); } }
        if (!test_runner_ || !db_) {
            send_response(fd, 503, "application/json", "{\"ok\":false,\"error\":\"unavailable\"}\n");
            return;
        }
        testing::SafetyLimits lim = test_runner_->safety_limits();
        lim.soc_floor_pct   = jval_d(body, "soc_floor_pct",   lim.soc_floor_pct);
        lim.voltage_floor_v = jval_d(body, "voltage_floor_v", lim.voltage_floor_v);
        lim.max_duration_sec = jval_i(body, "max_duration_sec", lim.max_duration_sec);
        if (body.find("\"abort_on_overtemp\":true") != std::string::npos)
            lim.abort_on_overtemp = true;
        else if (body.find("\"abort_on_overtemp\":false") != std::string::npos)
            lim.abort_on_overtemp = false;
        test_runner_->set_safety_limits(lim);
        db_->set_setting("test_soc_floor_pct", std::to_string(lim.soc_floor_pct));
        db_->set_setting("test_voltage_floor_v", std::to_string(lim.voltage_floor_v));
        db_->set_setting("test_max_duration_sec", std::to_string(lim.max_duration_sec));
        db_->set_setting("test_abort_on_overtemp", lim.abort_on_overtemp ? "1" : "0");
        send_response(fd, 200, "application/json", "{\"ok\":true}\n");
        return;
    }

    if (method == "POST" && path == "/api/tests/schedules") {
        if (!db_) { send_response(fd, 503, "application/json", "{\"ok\":false,\"error\":\"Database unavailable\"}\n"); return; }
        auto body_start = req.find("\r\n\r\n");
        if (body_start != std::string::npos) body_start += 4;
        else { body_start = req.find("\n\n"); if (body_start != std::string::npos) body_start += 2; }
        std::string body = (body_start != std::string::npos && body_start < req.size()) ? req.substr(body_start) : "";
        std::string test_type = jval_s(body, "test_type");
        std::string freq = jval_s(body, "frequency");
        if (freq.empty()) freq = "monthly";
        int hour = jval_i(body, "run_hour", 2);
        int min = jval_i(body, "run_minute", 0);
        int dom = jval_i(body, "day_of_month", 1);
        if (test_type.empty()) { send_response(fd, 400, "application/json", "{\"ok\":false,\"error\":\"test_type required\"}\n"); return; }
        int64_t next = compute_next_schedule_run(freq, hour, min, dom);
        Database::TestScheduleRow row;
        row.test_type = test_type;
        row.frequency = freq;
        row.run_hour = hour;
        row.run_minute = min;
        row.day_of_month = dom;
        row.next_run_ts = next;
        row.enabled = true;
        int64_t id = db_->insert_test_schedule(row);
        if (id > 0)
            send_response(fd, 200, "application/json", "{\"ok\":true,\"id\":" + std::to_string(id) + "}\n");
        else
            send_response(fd, 500, "application/json", "{\"ok\":false,\"error\":\"insert failed\"}\n");
        return;
    }

    if (method == "DELETE" && path.compare(0, 20, "/api/tests/schedules/") == 0 && path.size() > 20) {
        std::string id_part = path.substr(20);
        int64_t id = 0;
        try { id = std::stoll(id_part); } catch (...) {}
        if (id > 0 && db_ && db_->delete_test_schedule(id))
            send_response(fd, 200, "application/json", "{\"ok\":true}\n");
        else
            send_response(fd, 404, "application/json", "{\"ok\":false,\"error\":\"not found\"}\n");
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
                    ",\"locale\":" + jstr(db_->get_setting("locale")) +
                    ",\"shelly_host\":" + jstr(db_->get_setting("shelly_host")) +
                    ",\"shelly_enabled\":" + jstr(db_->get_setting("shelly_enabled")) +
                    ",\"shelly_test_active\":" + jstr(db_->get_setting("shelly_test_active")) + "}";
            }
            std::string theme = db_ ? db_->get_setting("theme") : "";
            auto an = store_.analytics();
            send_response(fd, 200, "text/html",
                          html_dashboard(snap, ble_st, pg, an, store_,
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
    } else if (path == "/ops_log" || path == "/ops_log.html") {
        std::string theme = db_ ? db_->get_setting("theme") : "";
        if (theme.empty()) theme = "dark";
        send_response(fd, 200, "text/html", html_ops_log_page(db_, theme));
    } else if (path == "/testing" || path == "/testing.html") {
        std::string theme = db_ ? db_->get_setting("theme") : "";
        if (theme.empty()) theme = "dark";
        send_response(fd, 200, "text/html", html_testing_page(db_, test_runner_, theme));
    } else if (path == "/api/ops_log") {
        size_t n = 100;
        auto ni = query.find("n=");
        if (ni != std::string::npos) { try { n = std::stoul(query.substr(ni + 2)); } catch (...) {} }
        std::string filter;
        auto fi = query.find("type=");
        if (fi != std::string::npos) {
            auto end = query.find('&', fi);
            std::string raw = (end != std::string::npos) ? query.substr(fi + 5, end - fi - 5) : query.substr(fi + 5);
            if (raw.find(',') == std::string::npos) filter = raw;
        }
        auto events = db_ ? db_->load_ops_events(std::min(n, size_t{500}), filter) : std::vector<OpsEvent>{};
        send_response(fd, 200, "application/json", ops_log_json(events));
    } else if (path == "/api/maintenance_status") {
        send_response(fd, 200, "application/json", maintenance_status_json(db_));
    } else if (path == "/api/grid_events") {
        bool unclassified_only = query.find("unclassified=1") != std::string::npos;
        auto evs = db_ ? (unclassified_only ? db_->load_unclassified_grid_events()
                                             : db_->load_grid_events())
                       : std::vector<Database::GridEventRow>{};
        std::string out = "[";
        bool gfirst = true;
        for (const auto& e : evs) {
            if (!gfirst) out += ",";
            gfirst = false;
            out += "{\"id\":"           + std::to_string(e.id)
                 + ",\"start_ts\":"     + std::to_string(e.start_ts)
                 + ",\"end_ts\":"       + std::to_string(e.end_ts)
                 + ",\"duration_s\":"   + std::to_string(e.duration_s)
                 + ",\"soc_start\":"    + jdbl(e.soc_start, 1)
                 + ",\"soc_end\":"      + jdbl(e.soc_end, 1)
                 + ",\"classification\":" + jstr(e.classification)
                 + ",\"user_notes\":"   + jstr(e.user_notes)
                 + "}";
        }
        out += "]\n";
        send_response(fd, 200, "application/json", out);
    } else if (method == "POST" && path.size() > 17 &&
               path.substr(0, 17) == "/api/grid_events/" &&
               path.find("/classify") != std::string::npos) {
        // POST /api/grid_events/{id}/classify
        int64_t event_id = 0;
        try { event_id = std::stoll(path.substr(17)); } catch (...) {}
        std::string body;
        { auto p = req.find("\r\n\r\n"); if (p != std::string::npos) body = req.substr(p + 4);
          else { p = req.find("\n\n"); if (p != std::string::npos) body = req.substr(p + 2); } }
        auto jfield = [&](const std::string& key) -> std::string {
            std::string pat = "\"" + key + "\":\"";
            auto p = body.find(pat);
            if (p == std::string::npos) return "";
            p += pat.size();
            auto q = body.find('"', p);
            return q == std::string::npos ? "" : body.substr(p, q - p);
        };
        std::string cls   = jfield("classification");
        std::string notes = jfield("notes");
        // Validate classification value
        if (event_id > 0 && db_ &&
            (cls == "outage" || cls == "maintenance" || cls == "false_alarm")) {
            db_->classify_grid_event(event_id, cls, notes);
            send_response(fd, 200, "application/json", "{\"ok\":true}\n");
        } else {
            send_response(fd, 400, "application/json", "{\"ok\":false,\"error\":\"invalid\"}\n");
        }
    } else if (path == "/api/shelly/status") {
        bool active = db_ && db_->get_setting("shelly_test_active") == "1";
        std::string start = db_ ? db_->get_setting("shelly_test_start_ts") : "";
        auto shst = fetch_shelly_status();
        std::string out = "{\"test_active\":" + std::string(active ? "true" : "false");
        if (!start.empty()) out += ",\"start_ts\":" + start;
        out += ",\"relay_on\":" + std::string(shst.relay_on ? "true" : "false");
        out += ",\"power_w\":" + jdbl(shst.power_w);
        if (shst.current_a > 0) out += ",\"current_a\":" + jdbl(shst.current_a);
        if (shst.voltage_v > 0) out += ",\"voltage_v\":" + jdbl(shst.voltage_v);
        if (shst.total_kwh > 0) out += ",\"total_kwh\":" + jdbl(shst.total_kwh, 3);
        out += ",\"ok\":" + std::string(shst.ok ? "true" : "false");
        out += "}\n";
        send_response(fd, 200, "application/json", out);
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
        send_response(fd, 200, "application/json", flow_json(snap, pg, fetch_shelly_status(), store_, store_.analytics()));
    } else if (path == "/api/tx_events") {
        size_t count = 100;
        auto ni = query.find("n=");
        if (ni != std::string::npos) { try { count = std::stoul(query.substr(ni + 2)); } catch (...) {} }
        send_response(fd, 200, "application/json", tx_events_json(store_.tx_events(count)));
    } else if (path == "/api/analytics") {
        store_.update_self_monitor();
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
            // With few discharge samples (on grid), profile avg can be biased high by spikes.
            // Apply conservative factor when sample count is low.
            if (total_n > 0 && total_n < 20)
                load_w *= 0.85;
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
    } else if (path == "/api/battery/capacity") {
        if (!db_) {
            send_response(fd, 200, "application/json", "{\"history\":[],\"latest_health\":-1}\n");
        } else {
            auto hist = db_->load_battery_capacity_history(365);
            std::string out = "{\"history\":[";
            for (size_t i = 0; i < hist.size(); ++i) {
                if (i) out += ",";
                char buf[64];
                std::snprintf(buf, sizeof(buf), "[%lld,%.2f]", (long long)hist[i].first, hist[i].second);
                out += buf;
            }
            double latest = hist.empty() ? -1 : hist.back().second;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "],\"latest_health\":%.1f}\n", latest);
            out += buf;
            send_response(fd, 200, "application/json", out);
        }
    } else if (path == "/api/battery/health") {
        auto* ext = store_.extensions();
        if (ext) {
            auto an = store_.analytics();
            auto r = ext->health_scorer().compute(an, ext->anomaly_detector().anomalies().size());
            send_response(fd, 200, "application/json", system_health_json(r));
        } else {
            send_response(fd, 200, "application/json",
                         "{\"score\":0,\"battery\":\"—\",\"cells\":\"—\",\"charging\":\"—\"}\n");
        }
    } else if (path == "/api/tests/safety_limits") {
        if (!test_runner_) {
            send_response(fd, 200, "application/json",
                "{\"soc_floor_pct\":10,\"voltage_floor_v\":11,\"max_duration_sec\":7200,\"abort_on_overtemp\":true}\n");
        } else {
            const auto& lim = test_runner_->safety_limits();
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                "{\"soc_floor_pct\":%.1f,\"voltage_floor_v\":%.1f,\"max_duration_sec\":%d,\"abort_on_overtemp\":%s}\n",
                lim.soc_floor_pct, lim.voltage_floor_v, lim.max_duration_sec,
                lim.abort_on_overtemp ? "true" : "false");
            send_response(fd, 200, "application/json", buf);
        }
    } else if (path == "/api/tests/active") {
        if (!test_runner_ || !test_runner_->is_running()) {
            send_response(fd, 200, "application/json", "{\"running\":false}\n");
        } else {
            auto st = test_runner_->active_stats();
            double v = st.battery_voltage;
            double a = st.battery_current;
            double soc = st.battery_soc;
            double load = st.load_power;
            double v_start = st.voltage_at_start;
            double v_min = st.min_voltage_seen;
            double sag = (v_start > 0 && v_min > 0 && v_start > v_min) ? (v_start - v_min) : 0;
            char buf[384];
            std::snprintf(buf, sizeof(buf),
                "{\"running\":true,\"test_id\":%lld,\"test_type\":\"%s\",\"start_time\":%lld,\"duration_seconds\":%d,"
                "\"battery_voltage\":%.2f,\"battery_current\":%.2f,\"battery_soc\":%.1f,\"load_power\":%.1f,"
                "\"energy_delivered_wh\":%.1f,\"voltage_sag\":%.2f}\n",
                (long long)st.test_id, st.test_type.c_str(), (long long)st.start_time, st.duration_seconds,
                v, a, soc, load, st.energy_delivered_wh, sag);
            send_response(fd, 200, "application/json", buf);
        }
    } else if (path == "/api/battery/health" || path == "/api/battery/diagnostics") {
        double capacity_pct = -1;
        double resistance_mohm = 0;
        double avg_sag = -1;
        double charge_eff = -1;
        int health_score = 0;
        if (db_) {
            auto cap_hist = db_->load_battery_capacity_history(365);
            if (!cap_hist.empty()) capacity_pct = cap_hist.back().second;
            auto sag_hist = db_->load_voltage_sag_history(90);
            if (!sag_hist.empty()) {
                double sum = 0;
                for (const auto& p : sag_hist) sum += p.second;
                avg_sag = sum / sag_hist.size();
            }
        }
        if (store_.extensions()) {
            resistance_mohm = store_.extensions()->resistance_estimator().internal_resistance_mohm();
        }
        auto an = store_.analytics();
        if (an.efficiency_valid && an.charger_efficiency >= 0)
            charge_eff = an.charger_efficiency * 100;
        auto* ext = store_.extensions();
        if (ext) {
            auto r = ext->health_scorer().compute(an, ext->anomaly_detector().anomalies().size());
            health_score = r.score;
        }
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "{\"estimated_capacity_pct\":%.1f,\"internal_resistance_mohm\":%.1f,\"avg_voltage_sag_v\":%.2f,"
            "\"charge_efficiency_pct\":%.1f,\"health_score\":%d}\n",
            capacity_pct, resistance_mohm, avg_sag, charge_eff, health_score);
        send_response(fd, 200, "application/json", buf);
    } else if (path == "/api/tests/schedules") {
        if (!db_) {
            send_response(fd, 200, "application/json", "{\"schedules\":[]}\n");
        } else {
            auto scheds = db_->load_test_schedules();
            std::string out = "{\"schedules\":[";
            for (size_t i = 0; i < scheds.size(); ++i) {
                if (i) out += ",";
                const auto& s = scheds[i];
                out += "{\"id\":" + std::to_string(s.id) +
                    ",\"test_type\":" + jstr(s.test_type) +
                    ",\"frequency\":" + jstr(s.frequency) +
                    ",\"run_hour\":" + std::to_string(s.run_hour) +
                    ",\"run_minute\":" + std::to_string(s.run_minute) +
                    ",\"day_of_month\":" + std::to_string(s.day_of_month) +
                    ",\"next_run_ts\":" + std::to_string(s.next_run_ts) +
                    ",\"enabled\":" + std::string(s.enabled ? "true" : "false") + "}";
            }
            out += "]}\n";
            send_response(fd, 200, "application/json", out);
        }
    } else if (path == "/api/tests") {
        if (!db_) {
            send_response(fd, 200, "application/json", "{\"tests\":[],\"running\":false}\n");
        } else {
            auto runs = db_->load_test_runs(100);
            std::string out = "{\"tests\":[";
            for (size_t i = 0; i < runs.size(); ++i) {
                if (i) out += ",";
                out += "{\"id\":" + std::to_string(runs[i].id) +
                    ",\"test_type\":" + jstr(runs[i].test_type) +
                    ",\"start_time\":" + std::to_string(runs[i].start_time) +
                    ",\"end_time\":" + std::to_string(runs[i].end_time) +
                    ",\"duration_seconds\":" + std::to_string(runs[i].duration_seconds) +
                    ",\"result\":" + jstr(runs[i].result) +
                    ",\"initial_soc\":" + jdbl(runs[i].initial_soc) +
                    ",\"user_notes\":" + jstr(runs[i].user_notes) + "}";
            }
            out += "],\"running\":" + std::string(test_runner_ && test_runner_->is_running() ? "true" : "false");
            if (test_runner_ && test_runner_->is_running()) {
                out += ",\"current_test_id\":" + std::to_string(test_runner_->current_test_id());
            }
            out += "}\n";
            send_response(fd, 200, "application/json", out);
        }
    } else if (path.compare(0, 11, "/api/tests/") == 0 && path.size() > 11) {
        size_t id_end = path.find('/', 11);
        std::string id_part = (id_end != std::string::npos) ? path.substr(11, id_end - 11) : path.substr(11);
        std::string suffix = (id_end != std::string::npos && id_end + 1 < path.size()) ? path.substr(id_end + 1) : "";
        int64_t id = 0;
        try { id = std::stoll(id_part); } catch (...) {}
        if (id <= 0 || !db_) {
            send_response(fd, 404, "application/json", "{\"error\":\"not found\"}\n");
        } else if (suffix == "telemetry") {
            auto samples = db_->load_test_telemetry(id, 5000);
            std::string out = "{\"test_id\":" + std::to_string(id) + ",\"samples\":[";
            for (size_t i = 0; i < samples.size(); ++i) {
                if (i) out += ",";
                char buf[256];
                std::snprintf(buf, sizeof(buf),
                    "{\"ts\":%lld,\"v\":%.2f,\"a\":%.2f,\"soc\":%.1f,\"load\":%.1f,\"chg_a\":%.2f,\"chg\":\"%s\"}",
                    (long long)samples[i].timestamp, samples[i].battery_voltage, samples[i].battery_current,
                    samples[i].battery_soc, samples[i].load_power, samples[i].charger_current, samples[i].charger_state.c_str());
                out += buf;
            }
            out += "]}\n";
            send_response(fd, 200, "application/json", out);
        } else {
            auto run = db_->load_test_run(id);
            if (!run) {
                send_response(fd, 404, "application/json", "{\"error\":\"not found\"}\n");
            } else {
                std::string out = "{\"id\":" + std::to_string(run->id) +
                    ",\"test_type\":" + jstr(run->test_type) +
                    ",\"start_time\":" + std::to_string(run->start_time) +
                    ",\"end_time\":" + std::to_string(run->end_time) +
                    ",\"duration_seconds\":" + std::to_string(run->duration_seconds) +
                    ",\"result\":" + jstr(run->result) +
                    ",\"initial_soc\":" + jdbl(run->initial_soc) +
                    ",\"initial_voltage\":" + jdbl(run->initial_voltage) +
                    ",\"metadata_json\":" + jstr(run->metadata_json) +
                    ",\"user_notes\":" + jstr(run->user_notes) + "}\n";
                send_response(fd, 200, "application/json", out);
            }
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
                ",\"shelly_host\":" + jstr(g("shelly_host", "")) +
                ",\"shelly_enabled\":" + jstr(g("shelly_enabled", "0")) +
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

std::string HttpServer::ops_log_json(const std::vector<OpsEvent>& events) {
    std::string o = "{\"events\":[";
    for (size_t i = 0; i < events.size(); i++) {
        if (i) o += ",";
        const auto& e = events[i];
        char tbuf[32];
        std::time_t t = std::chrono::system_clock::to_time_t(e.timestamp);
        struct tm tm_b{};
        localtime_r(&t, &tm_b);
        std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", &tm_b);
        o += "\n  {\"id\":" + std::to_string(e.id) + ",\"timestamp\":\"" + tbuf + "\",\"type\":" + jstr(e.type) +
             ",\"subtype\":" + jstr(e.subtype) + ",\"message\":" + jstr(e.message) + ",\"notes\":" + jstr(e.notes) +
             ",\"metadata\":" + (e.metadata_json.empty() ? "{}" : e.metadata_json) + "}";
    }
    o += "\n]}\n";
    return o;
}

std::string HttpServer::maintenance_status_json(Database* db) {
    if (!db) return "{\"maintenance_mode\":false}\n";
    bool active = db->get_setting("maintenance_mode") == "1";
    std::string start = db->get_setting("maintenance_start_time");
    std::string end = db->get_setting("maintenance_end_time");
    std::string o = "{\"maintenance_mode\":" + std::string(active ? "true" : "false");
    if (!start.empty()) o += ",\"maintenance_start_time\":" + jstr(start);
    if (!end.empty()) o += ",\"maintenance_end_time\":" + jstr(end);
    o += "}\n";
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
    o += ",\n  \"process_rss_kb\": " + std::to_string(a.process_rss_kb);
    o += ",\n  \"process_vsz_kb\": " + std::to_string(a.process_vsz_kb);
    o += ",\n  \"process_cpu_pct\": " + jdbl(a.process_cpu_pct, 1);
    o += ",\n  \"db_size_bytes\": " + std::to_string(a.db_size_bytes);
    o += ",\n  \"db_table_sizes\": {";
    for (size_t i = 0; i < a.db_table_sizes.size(); ++i) {
        if (i) o += ",";
        o += jstr(a.db_table_sizes[i].first) + ":" + std::to_string(a.db_table_sizes[i].second);
    }
    o += "}";
    o += ",\n  \"sys_load_1m\": "          + jdbl(a.sys_load_1m, 2);
    o += ",\n  \"sys_load_5m\": "          + jdbl(a.sys_load_5m, 2);
    o += ",\n  \"sys_load_15m\": "         + jdbl(a.sys_load_15m, 2);
    o += ",\n  \"sys_mem_total_kb\": "     + std::to_string(a.sys_mem_total_kb);
    o += ",\n  \"sys_mem_available_kb\": " + std::to_string(a.sys_mem_available_kb);
    o += ",\n  \"disk_free_bytes\": "      + std::to_string(a.disk_free_bytes);
    o += ",\n  \"disk_total_bytes\": "     + std::to_string(a.disk_total_bytes);
    o += ",\n  \"cpu_freq_mhz\": "         + std::to_string(a.cpu_freq_mhz);
    o += ",\n  \"ssd_wear_pct\": "         + std::to_string(a.ssd_wear_pct);
    o += ",\n  \"ssd_power_on_hours\": "   + std::to_string(a.ssd_power_on_hours);
    o += ",\n  \"ssd_data_written_gb\": "  + std::to_string(a.ssd_data_written_gb);
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
        if (vmin < vlo) vlo = vmin;
        if (vmax > vhi) vhi = vmax;
        if (p.bat_a < alo) alo = p.bat_a;
        if (p.bat_a > ahi) ahi = p.bat_a;
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

// Effective charger state: override "Charging"/"Float" to "Idle" when no current/PWM
static std::string effective_charger_state(const PwrGateSnapshot& pg) {
    if (!pg.valid) return "";
    if ((pg.state == "Charging" || pg.state == "Float") &&
        std::abs(pg.bat_a) < 0.05 && pg.pwm < 10)
        return "Idle";
    return pg.state;
}

std::string HttpServer::grid_event_banner_js() {
    // Raw JS injected into every page. Uses \uXXXX escapes so JS handles Unicode.
    // checkGridEvents() fetches unclassified events and shows a fixed red banner.
    // clsGrid() POSTs the classification and re-checks for more events.
    return R"JS(
function checkGridEvents(){
fetch('/api/grid_events?unclassified=1').then(function(r){return r.json()}).then(function(evs){
var b=document.getElementById('grid-evt-banner');if(b)b.remove();
if(!evs||!evs.length)return;
var ev=evs[0];
var dur=ev.duration_s>0?(ev.duration_s>=3600?(ev.duration_s/3600).toFixed(1)+' hr':Math.ceil(ev.duration_s/60)+' min'):'<1 min';
var ts=new Date(ev.start_ts*1000).toLocaleString();
var si=ev.soc_start>0?' \u00b7 Battery: '+ev.soc_start.toFixed(0)+'%\u2192'+(ev.soc_end>0?ev.soc_end.toFixed(0)+'%':'?'):'';
var wrap=document.createElement('div');
wrap.id='grid-evt-banner';
wrap.style.cssText='position:fixed;top:0;left:0;right:0;z-index:9999;background:#7f1d1d;color:#fff;padding:10px 16px;font-size:13px;font-family:system-ui,sans-serif;box-shadow:0 2px 12px rgba(0,0,0,.5);display:flex;align-items:center;gap:10px;flex-wrap:wrap;';
var msg=document.createElement('span');
msg.style.cssText='flex:1;min-width:160px';
msg.innerHTML='<b>\u26a1 Grid event \u2014 classification required:</b> '+dur+' at '+ts+si;
wrap.appendChild(msg);
var notes=document.createElement('input');
notes.id='grid-evt-notes';notes.placeholder='Notes (optional)';
notes.style.cssText='padding:4px 8px;border-radius:4px;border:none;font-size:12px;min-width:110px;background:rgba(255,255,255,0.15);color:#fff;outline:none';
wrap.appendChild(notes);
[['Outage','outage','#fff','#7f1d1d','700'],
 ['Maintenance','maintenance','rgba(255,255,255,0.2)','#fff','400'],
 ['False Alarm','false_alarm','rgba(255,255,255,0.1)','rgba(255,255,255,0.7)','400']
].forEach(function(x){
  var btn=document.createElement('button');
  btn.textContent=x[0];
  btn.style.cssText='background:'+x[2]+';color:'+x[3]+';border:1px solid rgba(255,255,255,0.35);padding:5px 12px;border-radius:5px;cursor:pointer;font-size:12px;font-weight:'+x[4]+';font-family:system-ui,sans-serif';
  btn.onclick=function(){clsGrid(ev.id,x[1])};
  wrap.appendChild(btn);
});
document.body.insertBefore(wrap,document.body.firstChild);
}).catch(function(){});}
window.clsGrid=function(id,cls){
var n=document.getElementById('grid-evt-notes');
fetch('/api/grid_events/'+id+'/classify',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({classification:cls,notes:n?n.value:''})}).then(function(r){if(r.ok)checkGridEvents()}).catch(function(){});
};
checkGridEvents();
)JS";
}

shelly::Status HttpServer::fetch_shelly_status() {
    std::lock_guard<std::mutex> lk(shelly_cache_mu_);
    return shelly_cache_;
}

void HttpServer::shelly_poll_loop() {
    int wait_ticks = 0;  // each tick = 1s
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!running_) break;
        if (--wait_ticks > 0) continue;

        shelly::Status st;
        try {
            st = shelly::get_status(db_);
        } catch (...) {
            st.ok = false;
        }
        {
            std::lock_guard<std::mutex> lk(shelly_cache_mu_);
            shelly_cache_ = st;
        }
        wait_ticks = st.ok ? 30 : 10;

        // Grid outage detection — only when Shelly is configured
        bool host_configured = db_ && !db_->get_setting("shelly_host").empty();
        if (!host_configured) continue;

        bool test_active  = db_->get_setting("shelly_test_active") == "1";
        bool maintenance  = db_->get_setting("maintenance_mode")   == "1";

        if (!st.ok && !test_active && !maintenance) {
            ++shelly_fail_count_;
            // Require 2 consecutive failures (~20s) + battery discharging before logging
            if (shelly_fail_count_ == 2 && active_grid_event_id_ == 0) {
                auto bat = store_.latest();
                bool discharging = bat && bat->valid && bat->current_a > 0.1;
                if (discharging) {
                    double soc = bat->soc_pct;
                    int64_t now_ts = static_cast<int64_t>(std::time(nullptr));
                    active_grid_event_id_ = db_->insert_grid_event(now_ts, soc);
                    grid_event_start_ts_  = now_ts;
                    LOG_WARN("Grid: connectivity lost while battery discharging — possible outage (event id=%lld)",
                             (long long)active_grid_event_id_);
                }
            }
        } else if (st.ok) {
            if (active_grid_event_id_ > 0) {
                // Outage ended — close the event so the classification banner appears
                auto bat = store_.latest();
                double soc_end = bat && bat->valid ? bat->soc_pct : 0;
                int64_t now_ts  = static_cast<int64_t>(std::time(nullptr));
                int duration_s  = static_cast<int>(now_ts - grid_event_start_ts_);
                db_->close_grid_event(active_grid_event_id_, now_ts, duration_s, soc_end);
                LOG_INFO("Grid: connectivity restored after %ds (event id=%lld) — awaiting classification",
                         duration_s, (long long)active_grid_event_id_);
                active_grid_event_id_ = 0;
                grid_event_start_ts_  = 0;
            }
            shelly_fail_count_ = 0;
        }
    }
}

std::string HttpServer::flow_json(const BatterySnapshot& bat, const PwrGateSnapshot& chg,
                                  const shelly::Status& shelly, const DataStore& /*store*/,
                                  const AnalyticsSnapshot& an) {
    std::string o = "{\n";
    o += "  \"battery_voltage\": " + jdbl(bat.valid ? bat.total_voltage_v : 0) + ",\n";
    o += "  \"battery_current\": " + jdbl(bat.valid ? bat.current_a : 0) + ",\n";
    o += "  \"state_of_charge\": " + jdbl(bat.valid ? bat.soc_pct : 0, 1) + ",\n";
    o += "  \"solar_voltage\": " + jdbl(chg.valid ? chg.sol_v : 0) + ",\n";
    o += "  \"charger_voltage\": " + jdbl(chg.valid ? chg.ps_v : 0) + ",\n";
    o += "  \"charger_state\": " + jstr(effective_charger_state(chg)) + ",\n";
    double bat_pwr = bat.valid ? bat.power_w : 0;
    double chg_pwr = chg.valid ? (chg.bat_v * chg.bat_a) : 0;
    
    // System load estimation logic:
    // 1) Direct DC load (Charger Output + Battery Discharge)
    double load_w = chg_pwr + bat_pwr;
    
    // 2) If Shelly grid power is available, we can estimate DC load from AC side
    //    assuming ~85% PSU efficiency and subtracting battery charge power.
    if (load_w < 0.1 && shelly.ok && shelly.power_w > 5.0) {
        double dc_from_ac = std::max(0.0, shelly.power_w * 0.85);
        // If battery is charging, subtract that power from the DC total to find the system load.
        // If battery current is negative (charging), bat_pwr is negative.
        double net_load = dc_from_ac + (bat_pwr < 0 ? bat_pwr : 0);
        if (net_load > 0.1) load_w = net_load;
    } else if (load_w < 0.1) {
        // Fallback to historical average from analytics if available
        if (an.avg_load_watts > 0.1) load_w = an.avg_load_watts;
    }
    
    load_w = std::max(0.0, load_w);

    o += "  \"power_watts\": " + jdbl(load_w) + ",\n";
    o += "  \"battery_power_w\": " + jdbl(bat_pwr) + ",\n";
    o += "  \"charger_power_w\": " + jdbl(chg_pwr) + ",\n";
    // Shelly grid stats (AC side — true wall power, upstream of PSU)
    o += "  \"grid_enabled\": " + std::string(shelly.ok ? "true" : "false") + ",\n";
    o += "  \"grid_relay_on\": " + std::string(shelly.relay_on ? "true" : "false") + ",\n";
    o += "  \"grid_power_w\": " + jdbl(shelly.power_w) + ",\n";
    o += "  \"grid_current_a\": " + jdbl(shelly.current_a) + ",\n";
    o += "  \"grid_voltage_v\": " + jdbl(shelly.voltage_v) + "\n";
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
    std::string eff_state = effective_charger_state(pg);
    std::string o = "{\n";
    o += "  \"valid\": true,\n";
    o += "  \"timestamp\": " + jstr(iso8601(pg.timestamp)) + ",\n";
    o += "  \"state\": "     + jstr(eff_state)   + ",\n";
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
                                       const AnalyticsSnapshot& a,
                                       const DataStore& store,
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
/* ── Performance Toggles ── */
.p-toggles{display:flex;gap:4px;background:var(--bg);padding:3px;border-radius:6px;margin-bottom:1rem;border:1px solid var(--border);width:fit-content}
.p-toggle{padding:.3rem .8rem;font-size:.7rem;border-radius:4px;cursor:pointer;color:var(--muted);transition:all .15s;font-weight:600}
.p-toggle:hover{color:var(--text)}
.p-toggle.active{background:var(--card);color:var(--green);box-shadow:0 1px 3px rgba(0,0,0,.15)}
.p-bar-wrap{height:6px;background:var(--bg);border-radius:3px;overflow:hidden;margin-top:4px;border:1px solid var(--border)}
.p-bar-fill{height:100%;background:var(--green);transition:width .4s ease-out}
.p-bar-fill.warn{background:var(--orange)}
.p-bar-fill.err{background:var(--red)}
.p-grid{display:grid;grid-template-columns:1fr;gap:.8rem}
.p-sec-title{font-size:.65rem;text-transform:uppercase;letter-spacing:.1em;color:var(--muted);margin-bottom:.4rem;font-weight:700}
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
/* ── Grid Control (Shelly) ── */
.grid-control-wrap{margin-top:1rem;padding-top:1rem;border-top:1px solid var(--border);background:linear-gradient(135deg,rgba(251,146,60,.08) 0%,rgba(34,197,94,.06) 100%);border-radius:8px;padding:1rem;margin-bottom:0}
html.light .grid-control-wrap{background:linear-gradient(135deg,rgba(234,88,12,.06) 0%,rgba(22,163,74,.05) 100%)}
.grid-control-wrap .card-title{font-size:.9rem;font-weight:600;color:var(--text);margin-bottom:.4rem}
.grid-control-desc{font-size:.8rem;line-height:1.45;color:var(--text);opacity:.92;margin-bottom:.9rem}
.grid-control-btns{display:flex;gap:.5rem;flex-wrap:wrap}
.grid-btn{font-family:inherit;font-size:.85rem;font-weight:600;padding:.5rem 1rem;border-radius:6px;cursor:pointer;border:none;transition:opacity .15s,transform .15s}
.grid-btn:hover{opacity:.92}
.grid-btn:active{transform:scale(.98)}
.grid-btn:focus-visible{outline:2px solid var(--green);outline-offset:2px}
.grid-btn-off{background:var(--orange);color:#fff}
html.light .grid-btn-off{background:#ea580c;color:#fff}
.grid-btn-on{background:var(--green);color:#fff}
html.light .grid-btn-on{background:#16a34a;color:#fff}
.grid-btn-test{background:var(--border);color:var(--text);font-size:.8rem}
html.light .grid-btn-test{background:var(--border);color:var(--text)}
#shelly-msg{margin-top:.5rem;font-size:.8rem;min-height:1.2em}
@media(max-width:640px){.grid-control-btns{flex-direction:column}.grid-btn{min-height:44px;padding:.6rem 1rem;font-size:.9rem}}
)HTML";
    o += ".main{display:flex;flex-direction:column;gap:.7rem}"
         "#bat-chart{height:200px}#chg-chart{height:180px}"
         "@media(max-width:640px){.main{display:flex;flex-direction:column}"
         ".stats{order:-2;margin-bottom:.5rem}.card-bat{order:-1}.card-chg{order:0}.col2{order:1}"
         "#bat-chart{height:280px}#chg-chart{height:260px}.card.card-bat,.card.card-chg{padding-bottom:1.5rem}"
         ".chart-svg{preserve-aspect-ratio:none}}";
    o += R"HTML(
/* ── Test banner ── */
.test-banner{display:none;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:.5rem;padding:.6rem 1rem;margin-bottom:1rem;background:linear-gradient(135deg,#c2410c 0%,#ea580c 100%);color:#fff;border-radius:8px;font-size:.85rem}
.test-banner.show{display:flex}
.test-banner .tb-btn{background:#fff!important;color:#c2410c!important;border:none;padding:.35rem .8rem;font-weight:600;cursor:pointer;border-radius:4px}
.test-banner .tb-btn:hover{opacity:.92}
/* ── Power reason modal ── */
.pwr-modal{position:fixed;inset:0;background:rgba(0,0,0,.6);backdrop-filter:blur(4px);display:flex;align-items:center;justify-content:center;z-index:1100;animation:pwr-fadeIn .2s ease}
@keyframes pwr-fadeIn{from{opacity:0}to{opacity:1}}
.pwr-modal-panel{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:1.5rem;min-width:320px;max-width:90vw;box-shadow:0 12px 40px rgba(0,0,0,.35);animation:pwr-slideUp .25s ease}
@keyframes pwr-slideUp{from{opacity:0;transform:translateY(-12px)}to{opacity:1;transform:translateY(0)}}
.pwr-modal-title{font-size:1rem;font-weight:600;color:var(--text);margin-bottom:.4rem}
.pwr-modal-desc{font-size:.85rem;color:var(--muted);line-height:1.4;margin-bottom:1rem}
.pwr-reason-input{width:100%;padding:.6rem .75rem;border:1px solid var(--border);border-radius:8px;background:var(--bg);color:var(--text);font-size:.9rem;font-family:inherit;resize:vertical;min-height:80px;margin-bottom:.5rem;transition:border-color .15s}
.pwr-reason-input:focus{outline:none;border-color:var(--green);box-shadow:0 0 0 2px rgba(74,222,128,.2)}
.pwr-reason-input::placeholder{color:var(--muted);opacity:.8}
.pwr-modal-err{font-size:.8rem;color:var(--red);margin-bottom:.5rem;display:none}
.pwr-modal-btns{display:flex;gap:.5rem;justify-content:flex-end;margin-top:1rem}
.pwr-modal-btn{font-family:inherit;font-size:.85rem;font-weight:600;padding:.5rem 1rem;border-radius:6px;cursor:pointer;border:none;transition:opacity .15s}
.pwr-modal-cancel{background:var(--border);color:var(--muted)}
.pwr-modal-cancel:hover{background:var(--border);color:var(--text);opacity:.9}
.pwr-modal-submit{background:var(--green);color:#fff}
html.light .pwr-modal-submit{background:#16a34a}
.pwr-modal-submit:hover:not(:disabled){opacity:.92}
.pwr-modal-submit:disabled{opacity:.5;cursor:not-allowed}
</style>
<link rel="prefetch" href="/solar">
</head><body><div class="wrap">
)HTML";

    o += "<nav class=\"tabs\"><a href=\"/\" class=\"tab active\">Dashboard</a><a href=\"/solar\" class=\"tab\">Solar</a><a href=\"/settings\" class=\"tab\">Settings</a><a href=\"/ops_log\" class=\"tab\">Ops Log</a><a href=\"/testing\" class=\"tab\">Testing</a></nav>";
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
    o += "<div id=\"test-banner\" class=\"test-banner\">"
         "<span><strong>Battery test active</strong> &middot; SoC: <span id=\"tb-soc\">—</span>% &middot; Load: <span id=\"tb-load\">—</span> W &middot; Runtime: <span id=\"tb-rt\">—</span> h</span>"
         "<button class=\"tb-btn\" onclick=\"endTestFromBanner()\">End test</button></div>\n";
    o += "<div id=\"pwr-reason-modal\" class=\"pwr-modal\" style=\"display:none\">"
         "<div class=\"pwr-modal-panel\" onclick=\"event.stopPropagation()\">"
         "<div class=\"pwr-modal-title\" id=\"pwr-modal-title\">Reason required</div>"
         "<p class=\"pwr-modal-desc\" id=\"pwr-modal-desc\">Please provide a reason for this action.</p>"
         "<textarea id=\"pwr-reason-input\" class=\"pwr-reason-input\" placeholder=\"e.g. Starting battery capacity test\" rows=\"3\"></textarea>"
         "<div class=\"pwr-modal-err\" id=\"pwr-modal-err\">Please enter a reason.</div>"
         "<div class=\"pwr-modal-btns\">"
         "<button type=\"button\" class=\"pwr-modal-btn pwr-modal-cancel\" id=\"pwr-modal-cancel\">Cancel</button>"
         "<button type=\"button\" class=\"pwr-modal-btn pwr-modal-submit\" id=\"pwr-modal-submit\" disabled>Submit</button>"
         "</div></div></div>\n";

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

    snprintf(buf, sizeof(buf),
        "<div class=\"stat\" title=\"Battery current. Positive = discharging (from battery), Negative = charging (into battery).\"><div class=\"stat-lbl\">Current</div>"
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
    double load_w = 0;
    bool load_estimated = false;
    if (s.valid) {
        double chg_pwr = pg.valid ? (pg.bat_v * pg.bat_a) : 0;
        load_w = chg_pwr + s.power_w;
        
    // Improve load visibility when running on grid with full battery.
    // If direct DC measurements show 0 load but we have historical data or grid power, use it.
    if (load_w < 0.1) {
        // We can't easily fetch shelly status here because it's a static method, 
        // but we can check the historical average from analytics since we have 'a'.
        if (a.avg_load_watts > 0.1) {
            // Fallback to historical average if we know the system is likely 'on'
            load_w = a.avg_load_watts;
            load_estimated = true;
        }
    }
        load_w = std::max(0.0, load_w);
    }
    
    std::string tooltip = "Estimated total system load (Charger Output + Battery Flow).";
    if (load_estimated) tooltip += " (Estimated from historical or grid data)";

    snprintf(buf, sizeof(buf),
        "<div class=\"stat\" id=\"spw-panel\" title=\"%s\">"
        "<div class=\"stat-lbl\">System Load</div>"
        "<div class=\"sv ok\"><span id=\"spw\">%.1f</span><span class=\"u\">W</span></div>"
        "<div class=\"stat-sub\" id=\"spw-sub\">Battery: %.1f&nbsp;W&nbsp;%s</div></div>\n",
        tooltip.c_str(), load_w, std::abs(s.valid ? s.power_w : 0.0), (s.valid && s.power_w < -0.1) ? "IN" : "OUT");
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
        std::string eff = effective_charger_state(pg);
        const char* sc = (eff == "Charging" || eff == "Float") ? "ok" : "warn";
        snprintf(buf, sizeof(buf), "<span class=\"%s\">%s</span>", sc, eff.c_str());
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
    o += "</table>";
    o += "<div id=\"grid-control-wrap\" class=\"grid-control-wrap\" style=\"display:none\">"
         "<div class=\"card-title\">Grid Control (Shelly)</div>"
         "<p class=\"grid-control-desc\">Turn off grid power supply to run on battery only. Use for learning discharge data.</p>"
         "<div class=\"grid-control-btns\">"
         "<button type=\"button\" class=\"grid-btn grid-btn-off\" id=\"shelly-off-btn\">Turn off grid</button>"
         "<button type=\"button\" class=\"grid-btn grid-btn-on\" id=\"shelly-on-btn\">Turn on grid</button>"
         "<button type=\"button\" class=\"grid-btn grid-btn-test\" id=\"shelly-test-start-btn\">Start battery test</button>"
         "<button type=\"button\" class=\"grid-btn grid-btn-test\" id=\"shelly-test-end-btn\" style=\"display:none\">End battery test</button>"
         "</div>"
         "<div id=\"shelly-msg\" class=\"msg\"></div>"
         "</div></div>\n";

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
    o += "<div class=\"acard\" style=\"grid-column: 1 / -1\"><div class=\"card-title\">Self-Analysis Performance</div>"
         "<div class=\"p-toggles\">"
         "<div class=\"p-toggle active\" data-pview=\"both\">Both Together</div>"
         "<div class=\"p-toggle\" data-pview=\"backend\">C++ Backend</div>"
         "<div class=\"p-toggle\" data-pview=\"frontend\">Web UI Frontend</div>"
         "</div>"
         "<div class=\"p-grid\">"
         "<div id=\"p-sec-backend\">"
         "<div class=\"p-sec-title\">C++ Server Backend</div>"
         "<table class=\"dt\">"
         "<tr><td>CPU Usage <span id=\"p-be-cpu-val\" style=\"float:right;font-weight:600\">—</span>"
         "<div class=\"p-bar-wrap\"><div id=\"p-be-cpu-bar\" class=\"p-bar-fill\" style=\"width:0\"></div></div></td></tr>"
         "<tr><td>Memory (RSS) <span id=\"p-be-mem-val\" style=\"float:right;font-weight:600\">—</span>"
         "<div class=\"p-bar-wrap\"><div id=\"p-be-mem-bar\" class=\"p-bar-fill\" style=\"width:0\"></div></div></td></tr>"
         "<tr><td>Virtual Size (VSZ)</td><td id=\"p-be-vsz\" style=\"text-align:right\">—</td></tr>"
         "<tr><td>Uptime</td><td id=\"p-be-uptime\" style=\"text-align:right\">—</td></tr>"
         "</table></div>"
         "<div id=\"p-sec-frontend\">"
         "<div class=\"p-sec-title\">Web UI Frontend</div>"
         "<table class=\"dt\">"
         "<tr><td>JS Heap <span id=\"p-fe-mem-val\" style=\"float:right;font-weight:600\">—</span>"
         "<div class=\"p-bar-wrap\"><div id=\"p-fe-mem-bar\" class=\"p-bar-fill\" style=\"width:0\"></div></div></td></tr>"
         "<tr><td>Update Latency <span id=\"p-fe-lat-val\" style=\"float:right;font-weight:600\">—</span>"
         "<div class=\"p-bar-wrap\"><div id=\"p-fe-lat-bar\" class=\"p-bar-fill\" style=\"width:0\"></div></div></td></tr>"
         "<tr><td>DOM Nodes</td><td id=\"p-fe-nodes\" style=\"text-align:right\">—</td></tr>"
         "<tr><td>JS Environment</td><td id=\"p-fe-env\" style=\"text-align:right\">—</td></tr>"
         "</table></div>"
         "</div></div>\n";

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
            if (fill < 0) fill = 0;
            if (fill > 100) fill = 100;
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
function showPowerReasonModal(opts,onConfirm){var title=opts.title||'Reason required';var desc=opts.desc||'Please provide a reason for this action.';var confirmText=opts.confirmText||'Submit';var modal=$('pwr-reason-modal');var inp=$('pwr-reason-input');var err=$('pwr-modal-err');var submitBtn=$('pwr-modal-submit');if(!modal||!inp)return;$('pwr-modal-title').textContent=title;var descEl=$('pwr-modal-desc');descEl.textContent=desc;descEl.style.display=desc?'':'none';submitBtn.textContent=confirmText;inp.value='';err.style.display='none';submitBtn.disabled=true;modal.style.display='flex';inp.focus();function close(){modal.style.display='none';}$('pwr-modal-cancel').onclick=close;submitBtn.onclick=function(){var r=inp.value.trim();if(!r){err.style.display='';err.textContent='Please enter a reason.';return;}close();onConfirm(r);};inp.oninput=function(){submitBtn.disabled=!inp.value.trim();err.style.display='none';};inp.onkeydown=function(e){if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();if(inp.value.trim())submitBtn.click();}};modal.onclick=function(e){if(e.target===modal)close();};}
function endTestFromBanner(){showPowerReasonModal({title:'End battery test',desc:'This will turn grid back on. A reason is required.',confirmText:'End test'},function(r){fetch('/api/shelly/relay',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({turn:'on',reason:r})}).then(function(x){return x.json()}).then(function(d){if(d.ok){var b=$('test-banner');if(b)b.classList.remove('show');if(typeof lmSettings!=='undefined')lmSettings.shelly_test_active='0';var te=$('shelly-test-end-btn');var ts=$('shelly-test-start-btn');if(te)te.style.display='none';if(ts)ts.style.display='inline-block'}else if(d.error){var m=$('shelly-msg');if(m){m.textContent='Error: '+d.error;m.className='msg err'}}}).catch(function(){})})}
function upTestBanner(){var b=$('test-banner');if(!b||!b.classList.contains('show'))return;fetch('/api/shelly/status').then(function(r){return r.json()}).then(function(s){if(!s.test_active){b.classList.remove('show');if(typeof lmSettings!=='undefined')lmSettings.shelly_test_active='0';return}Promise.all([fetch('/api/status').then(function(r){return r.json()}),fetch('/api/analytics').then(function(r){return r.json()})]).then(function(arr){var st=arr[0],an=arr[1];var soc=$('tb-soc'),load=$('tb-load'),rt=$('tb-rt');if(soc)soc.textContent=st.valid?fmt(st.soc_pct,1):'—';if(load)load.textContent=an.avg_discharge_24h_w>0?fmt(an.avg_discharge_24h_w,1):'—';if(rt)rt.textContent=an.runtime_from_current_h>0?fmt(an.runtime_from_current_h,1):'—'})})}
initSettings()
initAtabs()
fetch('/api/shelly/status').then(function(r){return r.json()}).then(function(s){if(s.test_active){var b=$('test-banner');if(b){b.classList.add('show');upTestBanner();setInterval(upTestBanner,5000)}}}).catch(function(){})
function initGridControl(){var gw=$('grid-control-wrap');if(!gw)return;var sh=getSetting('shelly_host',''),se=getSetting('shelly_enabled','0');if(!sh||(se!=='1'&&se!=='true'))return;gw.style.display='block';var offBtn=$('shelly-off-btn'),onBtn=$('shelly-on-btn'),testStart=$('shelly-test-start-btn'),testEnd=$('shelly-test-end-btn'),shellyMsg=$('shelly-msg');var testActive=getSetting('shelly_test_active','0')==='1';if(testActive&&testStart)testStart.style.display='none';if(testActive&&testEnd)testEnd.style.display='inline-block';function shellyReq(turn,isTest,reason){if(shellyMsg)shellyMsg.textContent='';if(shellyMsg)shellyMsg.className='msg';var body={turn:turn,reason:reason};if(isTest)body.test=true;fetch('/api/shelly/relay',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)}).then(function(r){return r.json()}).then(function(d){if(d.ok){if(shellyMsg)shellyMsg.textContent='Done. Grid turned '+turn+'.';if(shellyMsg)shellyMsg.className='msg';if(turn==='off'){if(testStart)testStart.style.display='none';if(testEnd)testEnd.style.display='inline-block'}else{if(testStart)testStart.style.display='inline-block';if(testEnd)testEnd.style.display='none'}}else{if(shellyMsg){shellyMsg.textContent='Error: '+(d.error||'unknown');shellyMsg.className='msg err'}}}).catch(function(){if(shellyMsg){shellyMsg.textContent='Request failed.';shellyMsg.className='msg err'}})}if(offBtn)offBtn.onclick=function(){showPowerReasonModal({title:'Turn off grid',desc:'System will run on battery only. A reason is required.'},function(r){shellyReq('off',false,r)})};if(onBtn)onBtn.onclick=function(){showPowerReasonModal({title:'Turn on grid',desc:'Restore grid power supply. A reason is required.'},function(r){shellyReq('on',false,r)})};if(testStart)testStart.onclick=function(){showPowerReasonModal({title:'Start battery test',desc:'This will turn off grid to collect discharge data. Run for a few hours, then click End test. A reason is required.'},function(r){shellyReq('off',true,r)})};if(testEnd){testEnd.onclick=function(){showPowerReasonModal({title:'End battery test',desc:'This will turn grid back on. A reason is required.'},function(r){shellyReq('on',false,r)})};testEnd.style.display='none'}}
initGridControl()

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
  
  // Power & Load calculation
  window.lastBatV = d.voltage_v;
  window.lastBatPwr = d.power_w;
  window.lastBatPwrIn = (d.power_w < -0.1);
  var rd=a<-0.01?'to full':'to empty'
  updatePowerStat();

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
  window.lastChgPwr = (d.bat_v * d.bat_a);
  updatePowerStat();
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

window.lastBatV = 13.3;
window.lastBatPwr = 0;
window.lastBatPwrIn = false;
window.lastChgPwr = 0;

function updatePowerStat() {
  var loadW = (window.lastChgPwr || 0) + (window.lastBatPwr || 0);
  
  // Client-side estimation matching server logic
  var isEstimated = false;
  if (window.lastGridPwr !== undefined) {
    if (loadW < 0.1) {
        var gridW = window.lastGridPwr || 0;
        if (gridW > 5.0) {
            loadW = Math.max(0, gridW * 0.85 + (window.lastBatPwrIn ? window.lastBatPwr : 0));
            isEstimated = true;
        } else if (window.avgLoadWatts > 0.1) {
            loadW = window.avgLoadWatts;
            isEstimated = true;
        }
    }
  }
  loadW = Math.max(0, loadW);

  var pwrEl = $('spw');
  if (pwrEl) pwrEl.textContent = fmt(loadW, 1);
  var subEl = $('spw-sub');
  if (subEl) {
    var pwr = Math.abs(window.lastBatPwr || 0);
    var dir = window.lastBatPwrIn ? 'IN' : 'OUT';
    subEl.textContent = 'Battery: ' + fmt(pwr, 1) + ' W ' + dir;
  }
  var panEl = $('spw-panel');
  if (panEl) {
    var chgP = window.lastChgPwr || 0;
    var batP = window.lastBatPwr || 0;
    var tooltip = "Estimated total system load (Charger Output + Battery Flow).\n" +
                  "Formula: Charger (" + fmt(chgP, 1) + "W) + Battery (" + fmt(batP, 1) + "W) = " + fmt(chgP+batP, 1) + "W\n";
    if (isEstimated) {
      tooltip += "Current value is ESTIMATED (Real-time sensors reporting 0 while on grid or based on history).\n";
    }
    if (window.lastBatPwrIn) {
      tooltip += "Battery is currently CHARGING (" + fmt(Math.abs(batP), 1) + "W into battery).";
    } else if (batP > 0.1) {
      tooltip += "Battery is currently DISCHARGING (" + fmt(batP, 1) + "W from battery).";
    } else {
      tooltip += "Battery is IDLE.";
    }
    panEl.title = tooltip;
  }
  var saEl = $('sa-wrap');
  if (saEl) {
    var pwr = window.lastBatPwr || 0;
    var v = window.lastBatV || 13.3;
    var a = pwr / v;
    var tooltip = "Battery current. Positive = discharging (from battery), Negative = charging (into battery).\n" +
                  "Current: " + fmt(a, 2) + " A\n";
    if (a < -0.01) tooltip += "Charging battery at " + fmt(Math.abs(a), 2) + " A";
    else if (a > 0.01) tooltip += "Discharging battery at " + fmt(a, 2) + " A";
    else tooltip += "Battery idle";
    saEl.parentElement.title = tooltip;
  }
}

setInterval(upBat,5000); setInterval(upChg,5000)
upBat(); upChg()

function upFlow(){fetch('/api/flow').then(function(r){return r.json()}).then(function(d){
  lastFlowData=d;renderFlowDiagram(d)
  if(d.battery_current !== undefined) {
    window.lastBatV = d.battery_voltage;
    window.lastBatPwr = d.battery_power_w;
    window.lastBatPwrIn = d.battery_current < -0.1;
    window.lastChgPwr = d.charger_power_w;
    window.lastGridPwr = d.grid_power_w;
    updatePowerStat();
  }
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
  window.avgLoadWatts = d.avg_load_watts || 0;
  updatePowerStat();
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
  sv('sys-mem', (d.process_rss_kb/1024).toFixed(1) + ' / ' + (d.process_vsz_kb/1024).toFixed(1) + ' MB')
  sv('sys-cpu', (d.process_cpu_pct||0).toFixed(1) + '%')
  sv('sys-db-size', (d.db_size_bytes/1024/1024).toFixed(2) + ' MB')
  var dbt=$('sys-db-tables');if(dbt&&d.db_table_sizes){
    var h=''; Object.keys(d.db_table_sizes).forEach(function(k){
      if(h)h+='<br/>'; h+='<b>'+k+':</b> '+d.db_table_sizes[k]
    }); dbt.innerHTML=h
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

  // Performance Update
  (function(){
    var now=performance.now();
    var lat=window.lastAnUpdate ? (now - window.lastAnUpdate) : 0;
    window.lastAnUpdate = now;
    
    var rss=d.process_rss_kb||0, vsz=d.process_vsz_kb||0, cpu=d.process_cpu_pct||0;
    sv('p-be-cpu-val', cpu.toFixed(1) + '%');
    var cBar=$('p-be-cpu-bar'); if(cBar){cBar.style.width=Math.min(cpu,100)+'%'; cBar.className='p-bar-fill '+(cpu>80?'err':cpu>50?'warn':'')}
    sv('p-be-mem-val', (rss/1024).toFixed(1) + ' MB');
    var mBar=$('p-be-mem-bar'); if(mBar){var mp=Math.min((rss/1024)/512*100,100); mBar.style.width=mp+'%'; mBar.className='p-bar-fill '+(mp>85?'err':mp>70?'warn':'')}
    sv('p-be-vsz', (vsz/1024).toFixed(1) + ' MB');
    if(d.uptime_sec!=null){var u=d.uptime_sec,days=Math.floor(u/86400),hours=Math.floor((u%86400)/3600),mins=Math.floor((u%3600)/60);sv('p-be-uptime',days+'d '+hours+'h '+mins+'m')}

    // Frontend
    var nodes=document.getElementsByTagName('*').length;
    sv('p-fe-nodes', nodes);
    sv('p-fe-lat-val', Math.round(lat)+'ms');
    var lBar=$('p-fe-lat-bar'); if(lBar){var lp=Math.min(lat/500*100,100); lBar.style.width=lp+'%'; lBar.className='p-bar-fill '+(lat>200?'err':lat>100?'warn':'')}
    
    if(window.performance && performance.memory) {
      var mem=performance.memory.usedJSHeapSize / 1024 / 1024;
      var limit=performance.memory.jsHeapSizeLimit / 1024 / 1024;
      sv('p-fe-mem-val', mem.toFixed(1) + ' MB');
      var fBar=$('p-fe-mem-bar'); if(fBar){var fp=Math.min(mem/limit*100,100); fBar.style.width=fp+'%'; fBar.className='p-bar-fill '+(fp>80?'err':fp>50?'warn':'')}
    } else {
      sv('p-fe-mem-val', 'N/A');
    }
    sv('p-fe-env', (window.chrome?'Chrome/Blink':(window.sidebar?'Firefox/Gecko':(window.safari?'Safari/WebKit':'Other'))));
  })();

  var a24=$('an-avg-load-24h');if(a24)a24.title=d.runtime_from_historical?'7-day avg load from DB (on grid). Battery-only estimate.':d.runtime_from_charger?'From charger power when charging (est. load when grid drops).':'Load used for runtime. From 24h discharge or 1h profile (BMS+charger).'
  var rtTip=d.runtime_from_historical?'Battery-only estimate from 7-day avg load (on grid). 10% = LiFePO4 cutoff.':d.runtime_from_charger?'From charger power when charging (est. load when grid drops). 10% = LiFePO4 cutoff.':'24h discharge or 1h load profile. When charging, uses charger power as fallback. 10% = LiFePO4 cutoff.'
  var rfe=$('an-runtime-full');if(rfe)rfe.title=rtTip
  var rne=$('an-runtime-now');if(rne)rne.title=rtTip
  if(d.uptime_sec!=null){var u=d.uptime_sec,days=Math.floor(u/86400);sv('an-uptime',days>0?days+' days':Math.floor(u/3600)+'h')}
  else sv('an-uptime','—')
  
  // Performance toggles
  if(!window.perfInit){
    document.querySelectorAll('.p-toggle').forEach(function(btn){
      btn.onclick=function(){
        document.querySelectorAll('.p-toggle').forEach(function(b){b.classList.remove('active')});
        this.classList.add('active');
        var v=this.dataset.pview, grid=document.querySelector('.p-grid');
        var be=$('p-sec-backend'), fe=$('p-sec-frontend');
        if(v==='both'){ be.style.display=''; fe.style.display=''; grid.style.gridTemplateColumns='1fr 1fr' }
        else if(v==='backend'){ be.style.display=''; fe.style.display='none'; grid.style.gridTemplateColumns='1fr' }
        else if(v==='frontend'){ be.style.display='none'; fe.style.display=''; grid.style.gridTemplateColumns='1fr' }
      }
    });
    // Initial state based on screen width
    if(window.innerWidth < 600) {
        var bt=document.querySelector('.p-toggle[data-pview="both"]');
        if(bt) bt.click(); // Mobile default is still both, but stacked
    }
    window.perfInit=true;
  }

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
  var grouped=[],last=null;
  evs.forEach(function(e){
    if(last&&last.message===e.message){last.count=(last.count||1)+1}
    else{last=e;grouped.push(e)}
  });
  el.innerHTML=grouped.map(function(e){
    var c=e.count>1?' <span class=\"dim\">(\u00d7'+e.count+')</span>':'';
    return '<div>'+e.time+' '+e.message+c+'</div>'
  }).join('')
}).catch(function(){})}
setInterval(upEvents,5000); upEvents()

function renderFlowDiagram(d){
  var el=document.getElementById('flow-svg');if(!el)return
  var bv=d.battery_voltage||0,ba=d.battery_current||0,sol=d.solar_voltage||0,chv=d.charger_voltage||0
  var pw=d.power_watts||0
  var gridEnabled=!!d.grid_enabled,gridPwr=d.grid_power_w||0,gridOn=d.grid_relay_on!==false
  var gridActive=gridEnabled&&gridOn&&gridPwr>3
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
      {id:'grid',x:100,y:128,w:80,h:44,cls:'flow-node flow-node-grid',lbl:'Grid',val:gridEnabled?fmt(gridPwr,0)+' W':fmt(chv,2)+' V',inactive:gridEnabled&&!gridOn,tf:wFill,lf:wMuted},
      {id:'charger',x:100,y:204,w:80,h:44,cls:'flow-node flow-node-charger',lbl:chgNodeLbl,val:chgNodeVal,inactive:false,tf:wFill,lf:wMuted},
      {id:'battery',x:100,y:280,w:92,h:58,cls:'flow-node flow-node-battery',lbl:'Battery',val:fmt(bv,2)+' V',val2:(ba>=0?'+':'')+fmt(ba,2)+' A',val2Cls:idle?'':charging?'flow-cur-chg':'flow-cur-dchg',inactive:false,tf:wFill,lf:wMuted},
      {id:'load',x:100,y:368,w:80,h:44,cls:'flow-node flow-node-load',lbl:'Load',val:fmt(sysLoad||0,1)+' W',inactive:false,tf:fillTxt,lf:fillMuted}
    ]
    arrows=[
      {path:'M 100 74 L 100 102',cls:(solarActive?chgCls:idleCls)+' flow-arrow-solar',anim:charging&&solarActive,pwr:solarActive&&charging?fmt(chgPwr,0)+' W':'',mx:118,my:88},
      {path:'M 100 150 L 100 182',cls:(gridActive||((!gridEnabled)&&chv>1))?chgCls:idleCls,anim:gridActive||((!gridEnabled)&&charging&&chv>1),pwr:gridEnabled?fmt(gridPwr,0)+' W':(chv>1&&charging?fmt(chgPwr,0)+' W':''),mx:118,my:166},
      {path:'M 100 226 L 100 258',cls:chgCls,anim:charging,pwr:charging?fmt(Math.abs(batPwr||0),0)+' W':'',mx:118,my:242},
      {path:'M 100 302 L 100 346',cls:loadAnim?dchgCls:idleCls,anim:loadAnim,pwr:(sysLoad||0)>0?fmt(sysLoad||0,0)+' W':'',mx:118,my:324}
    ]
  }else{
    nodes=[
      {id:'solar',x:324,y:55,w:76,h:40,cls:'flow-node flow-node-solar',lbl:'Solar',val:fmt(sol,1)+' V',inactive:!solarActive,tf:wFill,lf:wMuted},
      {id:'grid',x:180,y:140,w:76,h:44,cls:'flow-node flow-node-grid',lbl:'Grid',val:gridEnabled?fmt(gridPwr,0)+' W':fmt(chv,2)+' V',inactive:gridEnabled&&!gridOn,tf:wFill,lf:wMuted},
      {id:'charger',x:324,y:140,w:88,h:44,cls:'flow-node flow-node-charger',lbl:chgNodeLbl,val:chgNodeVal,inactive:false,tf:wFill,lf:wMuted},
      {id:'battery',x:468,y:140,w:92,h:58,cls:'flow-node flow-node-battery',lbl:'Battery',val:fmt(bv,2)+' V',val2:(ba>=0?'+':'')+fmt(ba,2)+' A',val2Cls:idle?'':charging?'flow-cur-chg':'flow-cur-dchg',inactive:false,tf:wFill,lf:wMuted},
      {id:'load',x:612,y:140,w:76,h:44,cls:'flow-node flow-node-load',lbl:'Load',val:fmt(sysLoad||0,1)+' W',inactive:false,tf:fillTxt,lf:fillMuted}
    ]
    arrows=[
      {path:'M 324 75 L 324 118',cls:(solarActive?chgCls:idleCls)+' flow-arrow-solar',anim:charging&&solarActive,pwr:solarActive&&charging?fmt(chgPwr,0)+' W':'',mx:336,my:96},
      {path:'M 218 140 L 280 140',cls:(gridActive||((!gridEnabled)&&chv>1))?chgCls:idleCls,anim:gridActive||((!gridEnabled)&&charging&&chv>1),pwr:gridEnabled?fmt(gridPwr,0)+' W':(chv>1&&charging?fmt(chgPwr,0)+' W':''),mx:249,my:133},
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
    .then(function(d){
      var filtered=d.filter(function(p){return new Date(p.ts).getTime()>=cutoff});
      renderChgChart(filtered.length>=2?filtered:d);
    })
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
    var ds=(d.getMonth()+1)+'/'+d.getDate()
    if(short) return ds
    return ds+' '+tm
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
)" + grid_event_banner_js() + R"(</script></div></body></html>)";

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
.test-banner{display:none;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:.5rem;padding:.6rem 1rem;margin-bottom:1rem;background:linear-gradient(135deg,#c2410c 0%,#ea580c 100%);color:#fff;border-radius:8px;font-size:.85rem}
.test-banner.show{display:flex}
.test-banner .tb-btn{background:#fff!important;color:#c2410c!important;border:none;padding:.35rem .8rem;font-weight:600;cursor:pointer;border-radius:4px}
.test-banner .tb-btn:hover{opacity:.92}
.pwr-modal{position:fixed;inset:0;background:rgba(0,0,0,.6);backdrop-filter:blur(4px);display:flex;align-items:center;justify-content:center;z-index:1100}
.pwr-modal-panel{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:1.5rem;min-width:320px;max-width:90vw;box-shadow:0 12px 40px rgba(0,0,0,.35)}
.pwr-modal-title{font-size:1rem;font-weight:600;color:var(--text);margin-bottom:.4rem}
.pwr-modal-desc{font-size:.85rem;color:var(--muted);line-height:1.4;margin-bottom:1rem}
.pwr-reason-input{width:100%;padding:.6rem .75rem;border:1px solid var(--border);border-radius:8px;background:var(--bg);color:var(--text);font-size:.9rem;font-family:inherit;resize:vertical;min-height:80px;margin-bottom:.5rem}
.pwr-reason-input:focus{outline:none;border-color:var(--green)}
.pwr-modal-err{font-size:.8rem;color:var(--red);margin-bottom:.5rem;display:none}
.pwr-modal-btns{display:flex;gap:.5rem;justify-content:flex-end;margin-top:1rem}
.pwr-modal-btn{font-family:inherit;font-size:.85rem;font-weight:600;padding:.5rem 1rem;border-radius:6px;cursor:pointer;border:none}
.pwr-modal-cancel{background:var(--border);color:var(--muted)}
.pwr-modal-submit{background:var(--green);color:#fff}
.pwr-modal-submit:disabled{opacity:.5;cursor:not-allowed}
</style>
<link rel="prefetch" href="/"><link rel="prefetch" href="/settings">
</head><body>
)HTML";
    o += "<nav class=\"tabs\"><a href=\"/\" class=\"tab\">Dashboard</a><a href=\"/solar\" class=\"tab active\">Solar</a><a href=\"/settings\" class=\"tab\">Settings</a><a href=\"/ops_log\" class=\"tab\">Ops Log</a><a href=\"/testing\" class=\"tab\">Testing</a>";
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
    o += "<div id=\"test-banner\" class=\"test-banner\">"
         "<span><strong>Battery test active</strong> &middot; SoC: <span id=\"tb-soc\">—</span>% &middot; Load: <span id=\"tb-load\">—</span> W &middot; Runtime: <span id=\"tb-rt\">—</span> h</span>"
         "<button class=\"tb-btn\" onclick=\"endTestFromBanner()\">End test</button></div>";
    o += "<div id=\"pwr-reason-modal\" class=\"pwr-modal\" style=\"display:none\">"
         "<div class=\"pwr-modal-panel\" onclick=\"event.stopPropagation()\">"
         "<div class=\"pwr-modal-title\" id=\"pwr-modal-title\">Reason required</div>"
         "<p class=\"pwr-modal-desc\" id=\"pwr-modal-desc\">Please provide a reason for this action.</p>"
         "<textarea id=\"pwr-reason-input\" class=\"pwr-reason-input\" placeholder=\"e.g. Ending battery test\" rows=\"3\"></textarea>"
         "<div class=\"pwr-modal-err\" id=\"pwr-modal-err\">Please enter a reason.</div>"
         "<div class=\"pwr-modal-btns\">"
         "<button type=\"button\" class=\"pwr-modal-btn pwr-modal-cancel\" id=\"pwr-modal-cancel\">Cancel</button>"
         "<button type=\"button\" class=\"pwr-modal-btn pwr-modal-submit\" id=\"pwr-modal-submit\" disabled>Submit</button>"
         "</div></div></div>";
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
         "function showPowerReasonModal(opts,onConfirm){var title=opts.title||'Reason required';var desc=opts.desc||'Please provide a reason for this action.';var confirmText=opts.confirmText||'Submit';var modal=$('pwr-reason-modal');var inp=$('pwr-reason-input');var err=$('pwr-modal-err');var submitBtn=$('pwr-modal-submit');if(!modal||!inp)return;$('pwr-modal-title').textContent=title;var descEl=$('pwr-modal-desc');descEl.textContent=desc;descEl.style.display=desc?'':'none';submitBtn.textContent=confirmText;inp.value='';err.style.display='none';submitBtn.disabled=true;modal.style.display='flex';inp.focus();function close(){modal.style.display='none';}$('pwr-modal-cancel').onclick=close;submitBtn.onclick=function(){var r=inp.value.trim();if(!r){err.style.display='';err.textContent='Please enter a reason.';return;}close();onConfirm(r);};inp.oninput=function(){submitBtn.disabled=!inp.value.trim();err.style.display='none';};inp.onkeydown=function(e){if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();if(inp.value.trim())submitBtn.click();}};modal.onclick=function(e){if(e.target===modal)close();};}\n"
         "function endTestFromBanner(){showPowerReasonModal({title:'End battery test',desc:'This will turn grid back on. A reason is required.',confirmText:'End test'},function(r){fetch('/api/shelly/relay',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({turn:'on',reason:r})}).then(function(x){return x.json()}).then(function(d){if(d.ok){var b=$('test-banner');if(b)b.classList.remove('show');if(typeof lmSettings!=='undefined')lmSettings.shelly_test_active='0'}}).catch(function(){})})}\n"
         "function upTestBanner(){var b=$('test-banner');if(!b||!b.classList.contains('show'))return;fetch('/api/shelly/status').then(function(r){return r.json()}).then(function(s){if(!s.test_active){b.classList.remove('show');if(typeof lmSettings!=='undefined')lmSettings.shelly_test_active='0';return}Promise.all([fetch('/api/status').then(function(r){return r.json()}),fetch('/api/analytics').then(function(r){return r.json()})]).then(function(arr){var st=arr[0],an=arr[1];var soc=$('tb-soc'),load=$('tb-load'),rt=$('tb-rt');if(soc)soc.textContent=st.valid?fmt(st.soc_pct,1):'—';if(load)load.textContent=an.avg_discharge_24h_w>0?fmt(an.avg_discharge_24h_w,1):'—';if(rt)rt.textContent=an.runtime_from_current_h>0?fmt(an.runtime_from_current_h,1):'—'})})}\n"
         "(function(){var n=document.querySelectorAll('nav a[href^=\"/\"]');for(var i=0;i<n.length;i++){n[i].addEventListener('click',function(e){if(e.ctrlKey||e.metaKey||e.shiftKey)return;var h=this.getAttribute('href');if(!h)return;e.preventDefault();location.href=h})}})();\n"
         "initSettings();loadAll();setInterval(loadAll,300000)\n"
         "fetch('/api/shelly/status').then(function(r){return r.json()}).then(function(s){if(s.test_active){var b=$('test-banner');if(b){b.classList.add('show');upTestBanner();setInterval(upTestBanner,5000)}}}).catch(function(){})\n"
         + grid_event_banner_js() +
         "</script></body></html>";
    return o;
}

std::string HttpServer::html_ops_log_page(Database* db, const std::string& theme) {
    (void)db;
    std::string o;
    o.reserve(6000);
    o += R"HTML(<!DOCTYPE html><html lang="en" class=")HTML";
    o += (theme == "light") ? "light" : "";
    o += R"HTML("><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Ops Log — limonitor</title>
<link rel="prefetch" href="/"><link rel="prefetch" href="/solar"><link rel="prefetch" href="/settings">
<style>
:root{--bg:#0d0d11;--card:#16161c;--border:#2e2e3a;--text:#e0e0ea;--muted:#9090a8;--green:#4ade80;--orange:#fb923c;--red:#f87171;--blue:#60a5fa}
html.light{--bg:#f2f3f7;--card:#fff;--border:#d4d4e0;--text:#1a1a2c;--muted:#5a5a72;--green:#16a34a;--orange:#ea580c;--red:#dc2626;--blue:#2563eb}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--text);font-size:14px;line-height:1.5;padding:1rem;max-width:640px;margin:0 auto}
nav{display:flex;align-items:center;gap:.5rem;margin-bottom:1rem;padding-bottom:.75rem;border-bottom:1px solid var(--border)}
nav a{color:var(--muted);text-decoration:none;font-size:.9rem;padding:.35rem .6rem;border-radius:6px}
nav a:hover{color:var(--text)}nav a.active{color:var(--green);font-weight:600}
h1{font-size:1.25rem;color:var(--green);margin-bottom:.75rem}
.ops-card{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:1rem;margin-bottom:.75rem}
.ops-card h2{font-size:.85rem;color:var(--muted);margin-bottom:.5rem;text-transform:uppercase;letter-spacing:.08em}
.ops-btn{font-family:inherit;font-size:.85rem;font-weight:600;padding:.5rem 1rem;border-radius:6px;cursor:pointer;border:none;margin-right:.5rem;margin-bottom:.5rem}
.ops-btn-start{background:var(--blue);color:#fff}.ops-btn-end{background:var(--orange);color:#fff}
.ops-btn:hover{opacity:.92}
.ops-input{width:100%;padding:.5rem .6rem;border:1px solid var(--border);border-radius:6px;background:var(--bg);color:var(--text);font-size:.9rem;margin-bottom:.5rem}
.ops-submit{background:var(--green);color:#fff;border:none;padding:.5rem 1rem;border-radius:6px;cursor:pointer;font-weight:600;font-size:.85rem}
.ops-timeline{list-style:none}
.ops-timeline li{padding:.4rem 0;border-bottom:1px solid var(--border);font-size:.85rem;display:flex;gap:.5rem;align-items:flex-start}
.ops-timeline li:last-child{border-bottom:none}
.ops-time{color:var(--muted);font-family:monospace;font-size:.75rem;min-width:4.5rem;flex-shrink:0}
.ops-msg{flex:1}
.ops-type-note{border-left:3px solid var(--green);padding-left:.5rem}
.ops-type-maintenance_start,.ops-type-maintenance_end,.ops-type-grid_test_start,.ops-type-grid_test_end{border-left:3px solid var(--blue);padding-left:.5rem}
.ops-type-grid_outage{border-left:3px solid var(--red);padding-left:.5rem}
.ops-type-system_event{border-left:3px solid var(--orange);padding-left:.5rem}
.ops-filter{display:flex;gap:.35rem;flex-wrap:wrap;margin-bottom:.5rem}
.ops-filter button{background:var(--card);border:1px solid var(--border);color:var(--muted);padding:.25rem .5rem;font-size:.75rem;border-radius:4px;cursor:pointer}
.ops-filter button:hover,.ops-filter button.active{border-color:var(--green);color:var(--green)}
.maint-active{background:rgba(96,165,250,.15)!important;border:1px solid var(--blue);padding:.75rem;border-radius:6px;margin-bottom:.5rem}
.pwr-modal{position:fixed;inset:0;background:rgba(0,0,0,.6);backdrop-filter:blur(4px);display:flex;align-items:center;justify-content:center;z-index:1100}
.pwr-modal-panel{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:1.5rem;min-width:320px;max-width:90vw;box-shadow:0 12px 40px rgba(0,0,0,.35)}
.pwr-modal-title{font-size:1rem;font-weight:600;color:var(--text);margin-bottom:.4rem}
.pwr-modal-desc{font-size:.85rem;color:var(--muted);line-height:1.4;margin-bottom:1rem}
.pwr-reason-input{width:100%;padding:.6rem .75rem;border:1px solid var(--border);border-radius:8px;background:var(--bg);color:var(--text);font-size:.9rem;font-family:inherit;resize:vertical;min-height:80px;margin-bottom:.5rem}
.pwr-reason-input:focus{outline:none;border-color:var(--green)}
.pwr-modal-err{font-size:.8rem;color:var(--red);margin-bottom:.5rem;display:none}
.pwr-modal-btns{display:flex;gap:.5rem;justify-content:flex-end;margin-top:1rem}
.pwr-modal-btn{font-family:inherit;font-size:.85rem;font-weight:600;padding:.5rem 1rem;border-radius:6px;cursor:pointer;border:none}
.pwr-modal-cancel{background:var(--border);color:var(--muted)}
.pwr-modal-submit{background:var(--green);color:#fff}
.pwr-modal-submit:disabled{opacity:.5;cursor:not-allowed}
</style></head><body>
<nav><a href="/">Dashboard</a><a href="/solar">Solar</a><a href="/settings">Settings</a><a href="/ops_log" class="active">Ops Log</a><a href="/testing">Testing</a></nav>
<div id="pwr-reason-modal" class="pwr-modal" style="display:none"><div class="pwr-modal-panel" onclick="event.stopPropagation()"><div class="pwr-modal-title" id="pwr-modal-title">Reason required</div><p class="pwr-modal-desc" id="pwr-modal-desc">Please provide a reason for this action.</p><textarea id="pwr-reason-input" class="pwr-reason-input" placeholder="e.g. Replacing battery cells" rows="3"></textarea><div class="pwr-modal-err" id="pwr-modal-err">Please enter a reason.</div><div class="pwr-modal-btns"><button type="button" class="pwr-modal-btn pwr-modal-cancel" id="pwr-modal-cancel">Cancel</button><button type="button" class="pwr-modal-btn pwr-modal-submit" id="pwr-modal-submit" disabled>Submit</button></div></div></div>
<h1>Ops Log</h1>
<div class="ops-card" id="maint-card">
<h2>Maintenance</h2>
<div id="maint-status"></div>
<button type="button" class="ops-btn ops-btn-start" id="maint-start-btn" style="display:none">Start Maintenance</button>
<button type="button" class="ops-btn ops-btn-end" id="maint-end-btn" style="display:none">End Maintenance</button>
</div>
<div class="ops-card">
<h2>Add Note</h2>
<input type="text" class="ops-input" id="note-input" placeholder="e.g. Adjusted charge current to 10A">
<button type="button" class="ops-submit" id="note-submit">Add</button>
</div>
<div class="ops-card" id="diag-card">
<h2>System Diagnostics</h2>
<table style="width:100%;border-collapse:collapse;font-size:.85rem">
<tr><td style="color:var(--muted);padding:.2rem 0">Server CPU</td><td id="diag-cpu" style="text-align:right">—</td></tr>
<tr><td style="color:var(--muted);padding:.2rem 0">Server RAM</td><td id="diag-mem" style="text-align:right">—</td></tr>
<tr><td style="color:var(--muted);padding:.2rem 0">System RAM</td><td id="diag-sys-mem" style="text-align:right">—</td></tr>
<tr><td style="color:var(--muted);padding:.2rem 0">Load Average</td><td id="diag-load" style="text-align:right">—</td></tr>
<tr><td style="color:var(--muted);padding:.2rem 0">CPU Frequency</td><td id="diag-cpufreq" style="text-align:right">—</td></tr>
<tr><td style="color:var(--muted);padding:.2rem 0">UI Latency</td><td id="diag-lat" style="text-align:right">—</td></tr>
<tr><td style="color:var(--muted);padding:.2rem 0">Uptime</td><td id="diag-uptime" style="text-align:right">—</td></tr>
<tr><td style="color:var(--muted);padding:.2rem 0">Disk Free</td><td id="diag-disk" style="text-align:right">—</td></tr>
<tr><td style="color:var(--muted);padding:.2rem 0">Database File</td><td id="diag-db-size" style="text-align:right">—</td></tr>
<tr><td style="color:var(--muted);padding:.2rem 0;vertical-align:top">Table Row Counts</td><td id="diag-db-tables" style="text-align:right;font-size:.78rem;line-height:1.5">—</td></tr>
<tr><td style="color:var(--muted);padding:.2rem 0">SSD Wear</td><td id="diag-ssd-wear" style="text-align:right">—</td></tr>
<tr><td style="color:var(--muted);padding:.2rem 0">SSD Power-On</td><td id="diag-ssd-hours" style="text-align:right">—</td></tr>
<tr><td style="color:var(--muted);padding:.2rem 0">SSD Written</td><td id="diag-ssd-written" style="text-align:right">—</td></tr>
</table>
</div>
<div class="ops-card">
<h2>Recent Events</h2>
<div class="ops-filter">
<button type="button" class="ops-filter-btn active" data-filter="">All</button>
<button type="button" class="ops-filter-btn" data-filter="note">Notes</button>
<button type="button" class="ops-filter-btn" data-filter="maintenance_start,maintenance_end,grid_test_start,grid_test_end">Maintenance</button>
<button type="button" class="ops-filter-btn" data-filter="grid_outage">Outages</button>
</div>
<ul class="ops-timeline" id="ops-timeline"></ul>
</div>
<script>
function $(id){return document.getElementById(id)}
function showPowerReasonModal(opts,onConfirm){var title=opts.title||'Reason required';var desc=opts.desc||'Please provide a reason for this action.';var confirmText=opts.confirmText||'Submit';var modal=$('pwr-reason-modal');var inp=$('pwr-reason-input');var err=$('pwr-modal-err');var submitBtn=$('pwr-modal-submit');if(!modal||!inp)return;$('pwr-modal-title').textContent=title;var descEl=$('pwr-modal-desc');descEl.textContent=desc;descEl.style.display=desc?'':'none';submitBtn.textContent=confirmText;inp.value='';err.style.display='none';submitBtn.disabled=true;modal.style.display='flex';inp.focus();function close(){modal.style.display='none';}$('pwr-modal-cancel').onclick=close;submitBtn.onclick=function(){var r=inp.value.trim();if(!r){err.style.display='';err.textContent='Please enter a reason.';return;}close();onConfirm(r);};inp.oninput=function(){submitBtn.disabled=!inp.value.trim();err.style.display='none';};inp.onkeydown=function(e){if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();if(inp.value.trim())submitBtn.click();}};modal.onclick=function(e){if(e.target===modal)close();};}
function loadMaintStatus(){fetch('/api/maintenance_status').then(function(r){return r.json()}).then(function(d){
var st=$('maint-status'),start=$('maint-start-btn'),end=$('maint-end-btn');
if(d.maintenance_mode){st.innerHTML='<span class="maint-active">Maintenance window active</span>';if(start)start.style.display='none';if(end)end.style.display='inline-block'}
else{st.textContent='No maintenance in progress';if(start)start.style.display='inline-block';if(end)end.style.display='none'}
})}
function loadOpsLog(filter){var url='/api/ops_log?n=100';var singleType=filter&&filter.indexOf(',')<0;if(singleType)url+='&type='+encodeURIComponent(filter);fetch(url).then(function(r){return r.json()}).then(function(d){
var ul=$('ops-timeline');ul.innerHTML='';
var evs=d.events||[];
if(filter&&filter.indexOf(',')>=0){var types=filter.split(',');evs=evs.filter(function(e){return types.indexOf(e.type)>=0})}
if(!evs.length){ul.innerHTML='<li class="ops-msg" style="color:var(--muted)">No events yet</li>';return}
evs.forEach(function(e){
var li=document.createElement('li');
var time=e.timestamp?e.timestamp.replace('T',' ').substring(0,16):'';
li.innerHTML='<span class="ops-time">'+time+'</span><span class="ops-msg ops-type-'+e.type+'">'+e.message+(e.notes?' <span style="color:var(--muted)">('+e.notes+')</span>':'')+'</span>';
ul.appendChild(li);
});
})}
$('maint-start-btn').onclick=function(){showPowerReasonModal({title:'Start maintenance',desc:'A reason is required for maintenance mode.'},function(r){fetch('/api/maintenance/start',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({reason:r})}).then(function(x){return x.json()}).then(function(d){if(d.ok){loadMaintStatus();loadOpsLog()}}).catch(function(){})})}
$('maint-end-btn').onclick=function(){fetch('/api/maintenance/end',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({})}).then(function(x){return x.json()}).then(function(d){if(d.ok){loadMaintStatus();loadOpsLog()}})}
$('note-submit').onclick=function(){var inp=$('note-input');var msg=inp.value.trim();if(!msg)return;fetch('/api/note',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({message:msg})}).then(function(x){return x.json()}).then(function(d){if(d.ok){inp.value='';loadOpsLog()}})}
$('note-input').onkeydown=function(e){if(e.key==='Enter')$('note-submit').click()}
document.querySelectorAll('.ops-filter-btn').forEach(function(b){b.onclick=function(){document.querySelectorAll('.ops-filter-btn').forEach(function(x){x.classList.remove('active')});b.classList.add('active');loadOpsLog(b.dataset.filter)}})
function loadDiagnostics(){
  var t0=performance.now();
  fetch('/api/analytics').then(function(r){return r.json()}).then(function(d){
    var lat=Math.round(performance.now()-t0);
    var rss=d.process_rss_kb||0,cpu=d.process_cpu_pct||0;
    function sv(id,v){var e=$(id);if(e)e.textContent=v}
    sv('diag-cpu',cpu.toFixed(1)+'%');
    sv('diag-mem',(rss/1024).toFixed(1)+' MB');
    sv('diag-lat',lat+'ms');
    if(d.uptime_sec!=null){var u=d.uptime_sec,dy=Math.floor(u/86400),hr=Math.floor((u%86400)/3600),mn=Math.floor((u%3600)/60);sv('diag-uptime',dy+'d '+hr+'h '+mn+'m')}
    if(d.db_size_bytes>0)sv('diag-db-size',(d.db_size_bytes/1024/1024).toFixed(2)+' MB');
    if(d.db_table_sizes){var tbls=Object.entries(d.db_table_sizes).map(function(kv){return kv[0]+': '+kv[1]}).join('\n');var e=$('diag-db-tables');if(e)e.style.whiteSpace='pre';sv('diag-db-tables',tbls||'—')}
  }).catch(function(){})
}
setInterval(loadDiagnostics,10000);loadDiagnostics();
loadMaintStatus();loadOpsLog();
)HTML" + grid_event_banner_js() + R"HTML(</script></body></html>)HTML";
    return o;
}

std::string HttpServer::html_testing_page(Database* db, testing::TestRunner* runner,
                                          const std::string& theme) {
    (void)db;
    (void)runner;
    std::string o;
    o.reserve(5000);
    o += R"HTML(<!DOCTYPE html><html lang="en" class=")HTML";
    o += (theme == "light") ? "light" : "";
    o += R"HTML("><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>System Testing — limonitor</title>
<link rel="prefetch" href="/"><link rel="prefetch" href="/solar"><link rel="prefetch" href="/ops_log">
<style>
:root{--bg:#0d0d11;--card:#16161c;--border:#2e2e3a;--text:#e0e0ea;--muted:#9090a8;--green:#4ade80;--orange:#fb923c;--red:#f87171;--blue:#60a5fa}
html.light{--bg:#f2f3f7;--card:#fff;--border:#d4d4e0;--text:#1a1a2c;--muted:#5a5a72;--green:#16a34a;--orange:#ea580c;--red:#dc2626;--blue:#2563eb}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--text);font-size:14px;line-height:1.5;padding:1rem;max-width:720px;margin:0 auto}
nav{display:flex;align-items:center;gap:.5rem;margin-bottom:1rem;padding-bottom:.75rem;border-bottom:1px solid var(--border)}
nav a{color:var(--muted);text-decoration:none;font-size:.9rem;padding:.35rem .6rem;border-radius:6px}
nav a:hover{color:var(--text)}nav a.active{color:var(--green);font-weight:600}
h1{font-size:1.25rem;color:var(--green);margin-bottom:.75rem}
.test-card{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:1rem;margin-bottom:.75rem}
.test-card h2{font-size:.85rem;color:var(--muted);margin-bottom:.5rem;text-transform:uppercase;letter-spacing:.08em}
.test-btn{font-family:inherit;font-size:.85rem;font-weight:600;padding:.5rem 1rem;border-radius:6px;cursor:pointer;border:none;margin-right:.5rem;margin-bottom:.5rem}
.test-btn-start{background:var(--green);color:#fff}.test-btn-stop{background:var(--red);color:#fff}
.test-btn:disabled{opacity:.5;cursor:not-allowed}
.test-table{width:100%;border-collapse:collapse;font-size:.85rem}
.test-table th,.test-table td{padding:.4rem .5rem;text-align:left;border-bottom:1px solid var(--border)}
.test-table th{color:var(--muted);font-weight:600;font-size:.75rem;text-transform:uppercase}
.test-table tr:hover{background:var(--card)}
.test-table a{color:var(--blue);text-decoration:none}
.test-table a:hover{text-decoration:underline}
.running{color:var(--green)}.passed{color:var(--green)}.failed{color:var(--red)}.aborted{color:var(--orange)}
.test-row{cursor:pointer}
.test-row:hover{background:var(--card)}
.test-item{display:flex;justify-content:space-between;align-items:center;padding:.6rem 0;border-bottom:1px solid var(--border);gap:.5rem}
.test-item:last-child{border-bottom:none}
.test-item-desc{font-size:.8rem;color:var(--muted);margin-top:.2rem}
.test-item-meta{font-size:.72rem;color:var(--muted);margin-top:.25rem}
.test-item-btn{flex-shrink:0}
.active-monitor{background:linear-gradient(135deg,rgba(74,222,128,.08) 0%,rgba(96,165,250,.06) 100%);border:1px solid var(--green)}
html.light .active-monitor{background:linear-gradient(135deg,rgba(22,163,74,.06) 0%,rgba(37,99,235,.05) 100%)}
.diag-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:.5rem;margin-bottom:.5rem}
.diag-item{background:var(--bg);border-radius:6px;padding:.5rem;font-size:.8rem}
.diag-item .val{font-weight:700;font-size:1rem;color:var(--green)}
.active-inline{margin-bottom:.5rem}
.sched-item{display:flex;justify-content:space-between;align-items:center;padding:.4rem 0;font-size:.85rem}
.sched-placeholder{color:var(--muted);font-size:.85rem;font-style:italic}
.detail-chart{height:140px;background:var(--bg);border-radius:6px;margin-bottom:.5rem}
.notes-prompt{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:1rem;margin-top:.5rem}
.notes-prompt textarea{width:100%;padding:.5rem;border:1px solid var(--border);border-radius:6px;background:var(--bg);color:var(--text);font-size:.85rem;min-height:60px;margin-bottom:.5rem}
.export-btns{display:flex;gap:.5rem;margin-top:.5rem}
</style></head><body>
<nav><a href="/">Dashboard</a><a href="/solar">Solar</a><a href="/settings">Settings</a><a href="/ops_log">Ops Log</a><a href="/testing" class="active">Testing</a></nav>
<h1>System Testing</h1>
<div class="test-card">
<h2>Run Test</h2>
<div id="test-list"></div>
<button type="button" class="test-btn test-btn-stop" id="stop-test" disabled>Stop Test</button>
<span id="test-status" style="color:var(--muted);margin-left:.5rem"></span>
</div>
<div class="test-card active-monitor" id="active-monitor" style="display:none">
<h2>Active Test</h2>
<div id="active-content"></div>
<div id="active-charts" style="margin-top:.75rem"></div>
</div>
<div class="test-card">
<h2>Battery Health</h2>
<div class="diag-grid" id="battery-health"></div>
</div>
<div class="test-card">
<h2>Scheduled Tests</h2>
<p style="font-size:.85rem;color:var(--muted);margin:.25rem 0 .75rem">Automatically run a test at a recurring time. Tests will only start if the battery SoC is above the safety floor and no test is already running.</p>
<div id="sched-list" class="sched-placeholder">No scheduled tests.</div>
<div style="margin-top:.75rem;display:flex;flex-wrap:wrap;gap:.75rem;align-items:flex-end">
<label style="display:flex;flex-direction:column;gap:.25rem;font-size:.8rem;color:var(--muted)">Test type
<select id="sched-type" onchange="schedFreqChange()" style="font-family:inherit;font-size:.85rem;padding:.4rem .5rem;height:2.1rem;border:1px solid var(--border);border-radius:6px;background:var(--bg);color:var(--text)"><option value="ups_failover">UPS Failover</option><option value="load_spike">Load Spike</option><option value="capacity">Capacity</option><option value="charger_recovery">Charger Recovery</option><option value="simulated_outage">Simulated Outage</option></select>
</label>
<label style="display:flex;flex-direction:column;gap:.25rem;font-size:.8rem;color:var(--muted)">Frequency
<select id="sched-freq" onchange="schedFreqChange()" style="font-family:inherit;font-size:.85rem;padding:.4rem .5rem;height:2.1rem;border:1px solid var(--border);border-radius:6px;background:var(--bg);color:var(--text)"><option value="daily">Daily</option><option value="weekly">Weekly</option><option value="monthly" selected>Monthly</option></select>
</label>
<label style="display:flex;flex-direction:column;gap:.25rem;font-size:.8rem;color:var(--muted)">Hour (0–23)
<input type="number" id="sched-hour" min="0" max="23" value="2" style="width:4rem;font-family:inherit;font-size:.85rem;padding:.4rem .4rem;height:2.1rem;box-sizing:border-box;border:1px solid var(--border);border-radius:6px;background:var(--bg);color:var(--text)">
</label>
<label style="display:flex;flex-direction:column;gap:.25rem;font-size:.8rem;color:var(--muted)">Minute
<input type="number" id="sched-min" min="0" max="59" value="0" style="width:4rem;font-family:inherit;font-size:.85rem;padding:.4rem .4rem;height:2.1rem;box-sizing:border-box;border:1px solid var(--border);border-radius:6px;background:var(--bg);color:var(--text)">
</label>
<label id="sched-dom-label" style="display:flex;flex-direction:column;gap:.25rem;font-size:.8rem;color:var(--muted)">Day of month
<input type="number" id="sched-dom" min="1" max="28" value="1" style="width:4rem;font-family:inherit;font-size:.85rem;padding:.4rem .4rem;height:2.1rem;box-sizing:border-box;border:1px solid var(--border);border-radius:6px;background:var(--bg);color:var(--text)">
</label>
<button type="button" class="test-btn test-btn-start" onclick="addTestSchedule()" style="margin:0;height:2.1rem;padding:0 1rem">Add schedule</button>
</div>
</div>
<div class="test-card">
<h2>Test Safety Limits</h2>
<div id="safety-limits-view"></div>
<div id="safety-limits-form" style="display:none">
<div class="diag-grid" style="margin-bottom:.5rem">
<div class="diag-item"><div style="font-size:.75rem;color:var(--muted);margin-bottom:.3rem">Min SOC %</div><input type="number" id="sl-soc" min="5" max="50" step="1" style="width:100%;font-family:inherit;font-size:.9rem;padding:.3rem .4rem;border:1px solid var(--border);border-radius:4px;background:var(--bg);color:var(--text)"></div>
<div class="diag-item"><div style="font-size:.75rem;color:var(--muted);margin-bottom:.3rem">Min Voltage V</div><input type="number" id="sl-v" min="10" max="60" step="0.1" style="width:100%;font-family:inherit;font-size:.9rem;padding:.3rem .4rem;border:1px solid var(--border);border-radius:4px;background:var(--bg);color:var(--text)"></div>
<div class="diag-item"><div style="font-size:.75rem;color:var(--muted);margin-bottom:.3rem">Max Duration (min)</div><input type="number" id="sl-dur" min="5" max="600" step="5" style="width:100%;font-family:inherit;font-size:.9rem;padding:.3rem .4rem;border:1px solid var(--border);border-radius:4px;background:var(--bg);color:var(--text)"></div>
<div class="diag-item" style="display:flex;align-items:center;gap:.5rem"><input type="checkbox" id="sl-ot"><label for="sl-ot" style="font-size:.85rem">Abort on overtemp</label></div>
</div>
<button type="button" class="test-btn test-btn-start" onclick="saveSafetyLimits()" style="margin-right:.5rem">Save</button>
<button type="button" class="test-btn" onclick="showSafetyView()" style="background:var(--border);color:var(--text);margin:0">Cancel</button>
<span id="sl-msg" style="font-size:.8rem;color:var(--muted);margin-left:.5rem"></span>
</div>
</div>
<div class="test-card">
<h2>Test History</h2>
<table class="test-table"><thead><tr><th>Date</th><th>Type</th><th>Result</th><th>Duration</th><th>Notes</th></tr></thead>
<tbody id="test-tbody"></tbody></table>
</div>
<div class="test-card" id="detail-card" style="display:none">
<h2>Test Details <span id="detail-id"></span></h2>
<div id="detail-metrics"></div>
<div id="detail-charts"></div>
<div class="export-btns"><button type="button" class="test-btn test-btn-start" id="export-csv">Export CSV</button><button type="button" class="test-btn test-btn-start" id="export-json">Export JSON</button></div>
<div style="margin-top:1rem"><label style="font-size:.8rem;color:var(--muted)">Notes</label><textarea id="detail-notes" class="notes-prompt" style="width:100%;padding:.5rem;border:1px solid var(--border);border-radius:6px;background:var(--bg);color:var(--text);min-height:50px;margin-top:.3rem"></textarea><button type="button" class="test-btn test-btn-start" id="save-notes" style="margin-top:.5rem">Save Notes</button></div>
</div>
<div class="test-card notes-prompt" id="notes-prompt-card" style="display:none">
<h2>Test Complete</h2>
<p style="font-size:.85rem;margin-bottom:.5rem">Add notes about this test? (optional)</p>
<textarea id="finish-notes" placeholder="e.g. PSU unplugged manually, Radio transmitting during test"></textarea>
<button type="button" class="test-btn test-btn-start" id="finish-notes-ok">Save & Close</button>
</div>
<script>
function $(id){return document.getElementById(id)}
var TEST_TYPES=[{id:'capacity',name:'Capacity Test',desc:'Estimate battery capacity by controlled discharge',dur:'2–3 h',impact:'Moderate discharge'},{id:'ups_failover',name:'UPS Failover Test',desc:'Verify grid to battery transition',dur:'~5m',impact:'Brief discharge'},{id:'load_spike',name:'Load Spike Test',desc:'Measure voltage sag during high load',dur:'~12s',impact:'Minimal'},{id:'charger_recovery',name:'Charger Recovery Test',desc:'Verify charging ramp after outage',dur:'~15m',impact:'Charge cycle'},{id:'simulated_outage',name:'Simulated Outage',desc:'Estimate runtime without disconnecting power',dur:'~5m',impact:'Simulation only'}]
var lastStoppedTestId=null
function fmtDate(ts){if(!ts)return'—';var d=new Date(ts*1000);return d.toLocaleDateString()+' '+d.toLocaleTimeString('en-US',{hour:'2-digit',minute:'2-digit'})}
function fmtDateShort(ts){if(!ts)return'—';var d=new Date(ts*1000);return d.toLocaleDateString()}
function fmtDur(s){if(!s)return'—';if(s<60)return s+'s';if(s<3600)return Math.floor(s/60)+'m';return Math.floor(s/3600)+'h '+(Math.floor((s%3600)/60))+'m'}
function fmtType(t){var x=TEST_TYPES.find(function(p){return p.id===t});return x?x.name:t}
function renderTestList(running,activeData){var wrap=$('test-list');wrap.innerHTML='';if(running&&activeData){var start=new Date(activeData.start_time*1000);var elapsed=activeData.duration_seconds||0;var m=Math.floor(elapsed/60),s=elapsed%60;var elapsedStr=(m>0?m+'m ':'')+s+'s';wrap.innerHTML='<div class="active-inline"><strong>'+fmtType(activeData.test_type)+'</strong><div class="test-item-meta">Started: '+start.toLocaleTimeString('en-US',{hour:'2-digit',minute:'2-digit'})+' &middot; Elapsed: '+elapsedStr+'</div><div class="diag-grid" style="margin-top:.5rem"><div class="diag-item">Battery: <span class="val">'+(activeData.battery_voltage||0).toFixed(2)+' V</span></div><div class="diag-item">Current: <span class="val">'+(activeData.battery_current||0).toFixed(1)+' A</span></div><div class="diag-item">Load: <span class="val">'+(activeData.load_power||0).toFixed(0)+' W</span></div><div class="diag-item">SOC: <span class="val">'+(activeData.battery_soc||0).toFixed(0)+'%</span></div><div class="diag-item">Energy: <span class="val">'+(activeData.energy_delivered_wh||0).toFixed(0)+' Wh</span></div><div class="diag-item">Sag: <span class="val">'+(activeData.voltage_sag||0).toFixed(2)+' V</span></div></div></div>';return}TEST_TYPES.forEach(function(t){var div=document.createElement('div');div.className='test-item';div.innerHTML='<div><strong>'+t.name+'</strong><div class="test-item-desc">'+t.desc+'</div><div class="test-item-meta">Duration: '+t.dur+' &middot; Battery impact: '+t.impact+'</div></div><button type="button" class="test-btn test-btn-start test-item-btn" data-type="'+t.id+'">Run</button>';div.querySelector('button').onclick=function(){if(running)return;fetch('/api/tests/start',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({test_type:t.id})}).then(function(r){return r.json()}).then(function(d){if(d.ok){loadTests();loadActive()}})};wrap.appendChild(div)})}
function loadBatteryHealth(){fetch('/api/battery/diagnostics').then(function(r){return r.json()}).then(function(d){var wrap=$('battery-health');var capVal=d.estimated_capacity_pct>=0?d.estimated_capacity_pct.toFixed(0)+'%':'unknown';var resVal=d.internal_resistance_mohm>0?(d.internal_resistance_mohm/1000).toFixed(3)+' Ω':'estimating';var sagVal=d.avg_voltage_sag_v>=0?d.avg_voltage_sag_v.toFixed(2)+' V':'unknown';var effVal=d.charge_efficiency_pct>=0?d.charge_efficiency_pct.toFixed(0)+'%':'unknown';var healthVal=(d.health_score||0)>0?(d.health_score||0)+'/100':'pending tests';wrap.innerHTML='<div class="diag-item"><span class="val">'+capVal+'</span><div>Estimated Capacity</div></div><div class="diag-item"><span class="val">'+resVal+'</span><div>Internal Resistance</div></div><div class="diag-item"><span class="val">'+sagVal+'</span><div>Average Voltage Sag</div></div><div class="diag-item"><span class="val">'+effVal+'</span><div>Charge Efficiency</div></div><div class="diag-item"><span class="val">'+healthVal+'</span><div>Health Score</div></div>'})}
var _safetyLimits={}
function showSafetyView(){$('safety-limits-view').style.display='';$('safety-limits-form').style.display='none'}
function showSafetyEdit(){$('safety-limits-view').style.display='none';$('safety-limits-form').style.display='';if(_safetyLimits.soc_floor_pct!=null){$('sl-soc').value=_safetyLimits.soc_floor_pct;$('sl-v').value=_safetyLimits.voltage_floor_v;$('sl-dur').value=Math.round(_safetyLimits.max_duration_sec/60);$('sl-ot').checked=!!_safetyLimits.abort_on_overtemp}}
function saveSafetyLimits(){var lim={soc_floor_pct:parseFloat($('sl-soc').value),voltage_floor_v:parseFloat($('sl-v').value),max_duration_sec:parseInt($('sl-dur').value,10)*60,abort_on_overtemp:$('sl-ot').checked};fetch('/api/tests/safety_limits',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(lim)}).then(function(r){return r.json()}).then(function(d){if(d.ok){_safetyLimits=lim;loadSafetyLimits();showSafetyView()}else{$('sl-msg').textContent='Save failed';$('sl-msg').style.color='#dc2626'}})}
function loadSafetyLimits(){fetch('/api/tests/safety_limits').then(function(r){return r.json()}).then(function(d){_safetyLimits=d;var ot=d.abort_on_overtemp?'enabled':'disabled';$('safety-limits-view').innerHTML='<div class="diag-grid" style="margin:0"><div class="diag-item">Minimum SOC: <span class="val">'+d.soc_floor_pct+'%</span></div><div class="diag-item">Minimum Voltage: <span class="val">'+d.voltage_floor_v+' V</span></div><div class="diag-item">Max Duration: <span class="val">'+fmtDur(d.max_duration_sec)+'</span></div><div class="diag-item">Overtemp abort: <span class="val">'+ot+'</span></div></div><button type="button" class="test-btn test-btn-start" onclick="showSafetyEdit()" style="margin-top:.5rem;font-size:.75rem;padding:.3rem .7rem">Edit</button>'})}
function loadActive(){fetch('/api/tests/active').then(function(r){return r.json()}).then(function(d){var mon=$('active-monitor');if(!d.running){mon.style.display='none';renderRunTestState(false,null);return}mon.style.display='block';var start=new Date(d.start_time*1000);var elapsed=d.duration_seconds||0;var m=Math.floor(elapsed/60),s=elapsed%60;var elapsedStr=(m>0?m+'m ':'')+s+'s';$('active-content').innerHTML='<div><strong>'+fmtType(d.test_type)+'</strong><div class="test-item-meta">Started: '+start.toLocaleTimeString('en-US',{hour:'2-digit',minute:'2-digit'})+' &middot; Elapsed: '+elapsedStr+'</div></div><div class="diag-grid"><div class="diag-item">Battery Voltage: <span class="val">'+d.battery_voltage.toFixed(2)+' V</span></div><div class="diag-item">Battery Current: <span class="val">'+d.battery_current.toFixed(1)+' A</span></div><div class="diag-item">Load Power: <span class="val">'+d.load_power.toFixed(0)+' W</span></div><div class="diag-item">SOC: <span class="val">'+d.battery_soc.toFixed(0)+'%</span></div><div class="diag-item">Energy Delivered: <span class="val">'+d.energy_delivered_wh.toFixed(0)+' Wh</span></div><div class="diag-item">Voltage Sag: <span class="val">'+d.voltage_sag.toFixed(2)+' V</span></div></div>';renderRunTestState(true,d);fetch('/api/tests/'+d.test_id+'/telemetry').then(function(x){return x.json()}).then(function(tel){var s=tel.samples||[];if(s.length<2){$('active-charts').innerHTML='';return}var w=400,h=80,pad=25;var svg=function(key,label,color){var min=999,max=-999;s.forEach(function(x){var v=x[key];if(v!=null&&!isNaN(v)){min=Math.min(min,v);max=Math.max(max,v)}});if(max<min)max=min+1;var xf=function(i){return pad+(w-pad*2)*(i/(s.length-1||1))};var yf=function(v){return h-pad-(h-pad*2)*(v-min)/(max-min)};var path='M';s.forEach(function(x,i){var v=x[key]!=null?x[key]:min;path+=(i?'L':'')+xf(i)+','+yf(v)});return'<div class="detail-chart">'+label+'</div><svg width="100%" height="80" viewBox="0 0 400 80"><path d="'+path+'" fill="none" stroke="'+color+'" stroke-width="1"/></svg>'};$('active-charts').innerHTML=svg('v','Voltage (V)','var(--green)')+svg('a','Current (A)','var(--blue)')+svg('load','Load (W)','var(--orange)')})})}
function renderRunTestState(running,activeData){renderTestList(running,activeData);var stopBtn=$('stop-test');if(stopBtn)stopBtn.disabled=!running}
function loadTests(){fetch('/api/tests').then(function(r){return r.json()}).then(function(d){
var tbody=$('test-tbody');tbody.innerHTML='';
var tests=d.tests||[];
if(!tests.length){tbody.innerHTML='<tr><td colspan=5 style="color:var(--muted);padding:1rem;text-align:center">No tests yet.<br><span style="font-size:.8rem">Run a test to begin collecting battery diagnostics.</span></td></tr>';renderRunTestState(d.running,null);if(d.running)window._lastTestId=d.current_test_id;else window._lastTestId=null;return}
tests.forEach(function(t){
var tr=document.createElement('tr');tr.className='test-row';
var resCls='';if(t.result==='running')resCls='running';else if(t.result==='passed')resCls='passed';else if(t.result==='failed')resCls='failed';else if(t.result==='aborted')resCls='aborted';
var dur=t.duration_seconds?fmtDur(t.duration_seconds):(t.result==='running'?'…':'—');
tr.innerHTML='<td>'+fmtDateShort(t.start_time)+'</td><td>'+fmtType(t.test_type)+'</td><td class="'+resCls+'">'+(t.result||'—').toUpperCase()+'</td><td>'+dur+'</td><td>'+(t.user_notes||'—')+'</td>';
tr.dataset.id=t.id;tr.onclick=function(){loadDetail(parseInt(tr.dataset.id,10))};
tbody.appendChild(tr);
});
var st=$('test-status');if(st)st.textContent=d.running?'Test running':'';
if(d.running){window._lastTestId=d.current_test_id;fetch('/api/tests/active').then(function(r){return r.json()}).then(function(ad){if(ad.running)renderRunTestState(true,ad)})}else{window._lastTestId=null;renderRunTestState(false,null)}
})}
function loadDetail(id){$('detail-card').style.display='block';$('detail-id').textContent='#'+id;window._detailId=id;fetch('/api/tests/'+id).then(function(r){return r.json()}).then(function(t){var meta={};try{meta=(t.metadata_json&&t.metadata_json.length)?JSON.parse(t.metadata_json):{}}catch(e){}var html='<div class="diag-grid">';if(meta.energy_delivered_wh)html+='<div class="diag-item">Energy Delivered: <span class="val">'+meta.energy_delivered_wh.toFixed(0)+' Wh</span></div>';if(meta.health_percent>=0)html+='<div class="diag-item">Health: <span class="val">'+meta.health_percent.toFixed(1)+'%</span></div>';html+='<div class="diag-item">Duration: <span class="val">'+fmtDur(t.duration_seconds)+'</span></div><div class="diag-item">Result: <span class="val">'+t.result+'</span></div></div>';$('detail-metrics').innerHTML=html;$('detail-notes').value=t.user_notes||''});fetch('/api/tests/'+id+'/telemetry').then(function(r){return r.json()}).then(function(d){var s=d.samples||[];if(!s.length){$('detail-charts').innerHTML='<div style="color:var(--muted);padding:1rem">No telemetry</div>';return}var minV=999,maxLoad=0,energyWh=0,vStart=0,vMin=999,chgRecovSec=-1,lastChgA=0;for(var i=0;i<s.length;i++){var x=s[i];if(x.v!=null&&x.v>0){minV=Math.min(minV,x.v);if(i===0)vStart=x.v}if(x.load!=null&&x.load>0)maxLoad=Math.max(maxLoad,x.load);if(i>0&&(x.load||0)>0){var dt=(x.ts-s[i-1].ts)||2;energyWh+=(x.load||0)*(dt/3600)}}for(var i=0;i<s.length;i++){var x=s[i];if(x.chg_a!=null&&x.chg_a>0.5){if(lastChgA<0.1&&i>0)chgRecovSec=Math.round((x.ts-s[0].ts));lastChgA=x.chg_a}else lastChgA=0}vMin=minV;var sag=(vStart>0&&vMin>0&&vStart>vMin)?(vStart-vMin).toFixed(2):'0';var computed='<div class="diag-grid" style="margin-bottom:.75rem"><div class="diag-item">Min Voltage: <span class="val">'+(minV<999?minV.toFixed(2):'—')+' V</span></div><div class="diag-item">Max Load: <span class="val">'+(maxLoad>0?maxLoad.toFixed(0):'—')+' W</span></div><div class="diag-item">Voltage Sag: <span class="val">'+sag+' V</span></div><div class="diag-item">Charger Recovery: <span class="val">'+(chgRecovSec>=0?chgRecovSec+'s':'—')+'</span></div></div>';var w=400,h=120,pad=30;var charts=[];var series=[{key:'v',label:'Voltage (V)',color:'var(--green)'},{key:'a',label:'Current (A)',color:'var(--blue)'},{key:'load',label:'Load (W)',color:'var(--orange)'},{key:'chg_a',label:'Charger (A)',color:'var(--muted)'}];series.forEach(function(ser){var min=999,max=-999;s.forEach(function(x){var v=x[ser.key];if(v!=null&&!isNaN(v)){min=Math.min(min,v);max=Math.max(max,v)}});if(max<min)max=min+1;var xf=function(i){return pad+(w-pad*2)*(i/(s.length-1||1))};var yf=function(v){return h-pad-(h-pad*2)*(v-min)/(max-min)};var path='M';s.forEach(function(x,i){var v=x[ser.key]!=null?x[ser.key]:min;path+=(i?'L':'')+xf(i)+','+yf(v)});charts.push('<div class="detail-chart"><div class="test-item-meta">'+ser.label+'</div><svg width="100%" height="120" viewBox="0 0 400 120"><path d="'+path+'" fill="none" stroke="'+ser.color+'" stroke-width="1"/></svg></div>')});$('detail-charts').innerHTML=computed+charts.join('')})}
function exportTest(id,fmt){fetch('/api/tests/'+id+'/telemetry').then(function(r){return r.json()}).then(function(d){var s=d.samples||[];if(fmt==='csv'){var h='timestamp,voltage_v,current_a,soc_pct,load_w,charger_a\n';var rows=s.map(function(x){return x.ts+','+(x.v||'')+','+(x.a||'')+','+(x.soc||'')+','+(x.load||'')+','+(x.chg_a||'')});var blob=new Blob([h+rows.join('\n')],{type:'text/csv'});var a=document.createElement('a');a.href=URL.createObjectURL(blob);a.download='test-'+id+'.csv';a.click()}else{var blob=new Blob([JSON.stringify(d)],{type:'application/json'});var a=document.createElement('a');a.href=URL.createObjectURL(blob);a.download='test-'+id+'.json';a.click()}})}
$('stop-test').onclick=function(){var tid=window._lastTestId;fetch('/api/tests/stop',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({})}).then(function(r){return r.json()}).then(function(d){if(d.ok){loadTests();loadActive();if(tid){lastStoppedTestId=tid;$('notes-prompt-card').style.display='block';$('finish-notes').value=''}}})}
$('export-csv').onclick=function(){if(window._detailId)exportTest(window._detailId,'csv')}
$('export-json').onclick=function(){if(window._detailId)exportTest(window._detailId,'json')}
$('save-notes').onclick=function(){if(!window._detailId)return;var n=$('detail-notes').value;fetch('/api/tests/'+window._detailId+'/notes',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({notes:n})}).then(function(r){return r.json()}).then(function(d){if(d.ok)loadTests()})}
$('finish-notes-ok').onclick=function(){if(lastStoppedTestId&&$('finish-notes').value.trim()){fetch('/api/tests/'+lastStoppedTestId+'/notes',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({notes:$('finish-notes').value.trim()})}).then(function(r){return r.json()})};$('notes-prompt-card').style.display='none';lastStoppedTestId=null}
function schedFreqChange(){var f=$('sched-freq').value;var dl=$('sched-dom-label');if(dl)dl.style.display=f==='monthly'?'':'none'}
function addTestSchedule(){var t=$('sched-type').value,f=$('sched-freq').value,h=parseInt($('sched-hour').value,10)||2,m=parseInt($('sched-min').value,10)||0,d=parseInt($('sched-dom').value,10)||1;fetch('/api/tests/schedules',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({test_type:t,frequency:f,run_hour:h,run_minute:m,day_of_month:d})}).then(function(r){return r.json()}).then(function(x){if(x.ok)loadSchedules()})}
function delTestSchedule(id){fetch('/api/tests/schedules/'+id,{method:'DELETE'}).then(function(r){if(r.ok)loadSchedules()})}
function loadSchedules(){fetch('/api/tests/schedules').then(function(r){return r.json()}).then(function(d){var wrap=$('sched-list');var s=d.schedules||[];if(!s.length){wrap.className='sched-placeholder';wrap.innerHTML='No scheduled tests.';return}wrap.className='';var html='';s.forEach(function(x){var next=new Date(x.next_run_ts*1000);var freq=x.frequency||'monthly';var freqStr=freq.charAt(0).toUpperCase()+freq.slice(1);var en=x.enabled?'':'<span style="color:var(--muted)"> (disabled)</span>';html+='<div class="sched-item"><span>'+fmtType(x.test_type)+en+'</span><span style="color:var(--muted);font-size:.8rem">'+freqStr+' &middot; Next: '+next.toLocaleDateString()+' '+next.toLocaleTimeString('en-US',{hour:'2-digit',minute:'2-digit'})+'</span><button type="button" onclick="delTestSchedule('+x.id+')" style="font-family:inherit;font-size:.75rem;background:none;border:none;color:var(--muted);cursor:pointer;padding:.1rem .4rem" title="Remove">&#x2715;</button></div>'});wrap.innerHTML=html})}
loadTests();loadBatteryHealth();loadSafetyLimits();loadActive();loadSchedules();schedFreqChange();
setInterval(function(){loadTests();loadActive()},3000);
setInterval(loadBatteryHealth,60000);
)HTML" + grid_event_banner_js() + R"HTML(</script></body></html>)HTML";
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
    auto chk = [&g](const std::string& key) {
        auto v = g(key, "0");
        return std::string((v == "1" || v == "true") ? " checked" : "");
    };
    std::string o;
    o.reserve(10000);
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
body{font-family:system-ui,-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:var(--bg);color:var(--text);font-size:14px;line-height:1.5;padding:1.5rem;max-width:560px;margin:0 auto}
nav{display:flex;align-items:center;gap:.5rem;margin-bottom:1.5rem;padding-bottom:1rem;border-bottom:1px solid var(--border)}
nav a{color:var(--muted);text-decoration:none;font-size:.9rem;padding:.35rem .6rem;border-radius:6px}
nav a:hover{color:var(--text);background:var(--card)}nav a.active{color:var(--green);font-weight:600}
nav .spacer{flex:1}nav .thm{background:var(--card);border:1px solid var(--border);color:var(--text);padding:.35rem .6rem;border-radius:6px;cursor:pointer;font-size:.85rem}
h1{font-size:1.35rem;font-weight:600;color:var(--green);margin-bottom:.25rem}
.sub{font-size:.8rem;color:var(--muted);margin-bottom:1.25rem}
.card{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:1.25rem;margin-bottom:1rem;box-shadow:0 1px 3px rgba(0,0,0,.08)}
.card-title{font-size:.7rem;text-transform:uppercase;letter-spacing:.1em;color:var(--muted);font-weight:700;margin-bottom:.75rem;padding-bottom:.4rem;border-bottom:1px solid var(--border)}
.row{margin-bottom:.65rem}.row:last-child{margin-bottom:0}
.row label{display:block;font-size:.8rem;color:var(--muted);margin-bottom:.3rem;font-weight:500}
.row input[type=text],.row input[type=number],.row select{width:100%;padding:.5rem .6rem;border:1px solid var(--border);border-radius:6px;background:var(--input-bg);color:var(--text);font-family:inherit;font-size:.9rem}
.row input:focus,.row select:focus{outline:none;border-color:var(--green)}
.row input[type=checkbox]{width:auto;margin-right:.4rem;vertical-align:middle;cursor:pointer}
.hint{font-size:.75rem;color:var(--muted);margin-top:.2rem}
.btn{background:var(--green);color:#fff;border:none;padding:.55rem 1.25rem;border-radius:6px;cursor:pointer;font-family:inherit;font-size:.9rem;font-weight:600}
.btn-sm{padding:.35rem .8rem;font-size:.8rem}
.msg{font-size:.8rem;color:var(--muted);margin-top:.6rem}.err{color:#dc2626}
.test-banner{display:none;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:.5rem;padding:.6rem 1rem;margin-bottom:1rem;background:linear-gradient(135deg,#c2410c 0%,#ea580c 100%);color:#fff;border-radius:8px;font-size:.85rem}
.test-banner.show{display:flex}.test-banner .tb-btn{background:#fff!important;color:#c2410c!important;border:none;padding:.35rem .8rem;font-weight:600;cursor:pointer;border-radius:4px}
.pwr-modal{position:fixed;inset:0;background:rgba(0,0,0,.6);backdrop-filter:blur(4px);display:flex;align-items:center;justify-content:center;z-index:1100}
.pwr-modal-panel{background:var(--card);border:1px solid var(--border);border-radius:12px;padding:1.5rem;min-width:320px;max-width:90vw;box-shadow:0 12px 40px rgba(0,0,0,.35)}
.pwr-modal-title{font-size:1rem;font-weight:600;margin-bottom:.4rem}.pwr-modal-desc{font-size:.85rem;color:var(--muted);line-height:1.4;margin-bottom:1rem}
.pwr-reason-input{width:100%;padding:.6rem .75rem;border:1px solid var(--border);border-radius:8px;background:var(--input-bg);color:var(--text);font-size:.9rem;font-family:inherit;resize:vertical;min-height:80px;margin-bottom:.5rem}
.pwr-reason-input:focus{outline:none;border-color:var(--green)}.pwr-modal-err{font-size:.8rem;color:#dc2626;margin-bottom:.5rem;display:none}
.pwr-modal-btns{display:flex;gap:.5rem;justify-content:flex-end;margin-top:1rem}
.pwr-modal-btn{font-family:inherit;font-size:.85rem;font-weight:600;padding:.5rem 1rem;border-radius:6px;cursor:pointer;border:none}
.pwr-modal-cancel{background:var(--border);color:var(--muted)}.pwr-modal-submit{background:var(--green);color:#fff}.pwr-modal-submit:disabled{opacity:.5;cursor:not-allowed}
</style></head><body>
<nav><a href="/">Dashboard</a><a href="/solar">Solar</a><a href="/settings" class="active">Settings</a><a href="/ops_log">Ops Log</a><a href="/testing">Testing</a><span class="spacer"></span><button class="thm" id="thm-btn" onclick="toggleTheme()" title="Toggle theme">)HTML";
    o += (theme == "light") ? "&#9788;" : "&#263d;";
    o += R"HTML(</button></nav>
<div id="test-banner" class="test-banner"><span><strong>Battery test active</strong> &middot; SoC: <span id="tb-soc">—</span>% &middot; Load: <span id="tb-load">—</span> W &middot; Runtime: <span id="tb-rt">—</span> h</span><button class="tb-btn" onclick="endTestFromBanner()">End test</button></div>
<div id="pwr-reason-modal" class="pwr-modal" style="display:none"><div class="pwr-modal-panel" onclick="event.stopPropagation()"><div class="pwr-modal-title" id="pwr-modal-title">Reason required</div><p class="pwr-modal-desc" id="pwr-modal-desc"></p><textarea id="pwr-reason-input" class="pwr-reason-input" placeholder="e.g. Ending battery test" rows="3"></textarea><div class="pwr-modal-err" id="pwr-modal-err">Please enter a reason.</div><div class="pwr-modal-btns"><button type="button" class="pwr-modal-btn pwr-modal-cancel" id="pwr-modal-cancel">Cancel</button><button type="button" class="pwr-modal-btn pwr-modal-submit" id="pwr-modal-submit" disabled>Submit</button></div></div></div>
<h1>Settings</h1>
<p class="sub">Application configuration. Hardware changes (BLE, serial) require a restart to take effect.</p>
<form id="cfg-form">
<div class="card"><div class="card-title">Bluetooth (BLE)</div>
<div class="row"><label>Device name fragment</label><input type="text" name="device_name" placeholder="e.g. L-12100BNNA70" value=")HTML";
    o += esc(g("device_name", ""));
    o += R"HTML("><p class="hint">Connects to the first device whose name contains this string.</p></div>
<div class="row"><label>Device address (MAC)</label><input type="text" name="device_address" placeholder="AA:BB:CC:DD:EE:FF" value=")HTML";
    o += esc(g("device_address", ""));
    o += R"HTML("></div>
<div class="row"><label>Adapter path</label><input type="text" name="adapter_path" value=")HTML";
    o += esc(g("adapter_path", "/org/bluez/hci0"));
    o += R"HTML("></div></div>

<div class="card"><div class="card-title">HTTP Server</div>
<div class="row"><label>Port</label><input type="number" name="http_port" min="1" max="65535" value=")HTML";
    o += esc(g("http_port", "8080"));
    o += R"HTML("></div>
<div class="row"><label>Bind address</label><input type="text" name="http_bind" value=")HTML";
    o += esc(g("http_bind", "0.0.0.0"));
    o += R"HTML("></div>
<div class="row"><label>Log file</label><input type="text" name="log_file" placeholder="(stderr only)" value=")HTML";
    o += esc(g("log_file", ""));
    o += R"HTML("></div>
<div class="row"><label><input type="checkbox" name="verbose")HTML";
    o += chk("verbose");
    o += R"HTML(> Verbose logging (DEBUG level)</label></div></div>

<div class="card"><div class="card-title">Grid Control (Shelly)</div>
<div class="row"><label>Shelly host</label><input type="text" name="shelly_host" placeholder="192.168.3.10" value=")HTML";
    o += esc(g("shelly_host", ""));
    o += R"HTML("><p class="hint">IP address or hostname of the Shelly smart plug. Gen1 and Gen2 supported.</p></div>
<div class="row"><label><input type="checkbox" name="shelly_enabled")HTML";
    o += chk("shelly_enabled");
    o += R"HTML(> Enable grid control (relay on/off)</label></div>
<div class="row"><label><input type="checkbox" name="shelly_battery_test_auto")HTML";
    o += chk("shelly_battery_test_auto");
    o += R"HTML(> Auto-end test when discharge data is sufficient</label>
<p class="hint">Restores grid after ~6 hours of discharge history is collected.</p></div>
<div class="row"><label>Max test duration (hours)</label><input type="number" name="shelly_battery_test_max_hours" min="1" max="168" value=")HTML";
    o += esc(g("shelly_battery_test_max_hours", "24"));
    o += R"HTML("><p class="hint">Grid is force-restored after this many hours regardless of state.</p></div>
<div class="row"><label>Low SoC safety cutoff (%)</label><input type="number" name="shelly_battery_test_low_soc" min="5" max="50" step="1" value=")HTML";
    o += esc(g("shelly_battery_test_low_soc", "20"));
    o += R"HTML("><p class="hint">Grid is force-restored if battery SoC drops below this threshold.</p></div>
<div class="row"><label>Maintenance window timeout (minutes, 0 = no timeout)</label><input type="number" name="maintenance_auto_timeout_minutes" min="0" max="10080" value=")HTML";
    o += esc(g("maintenance_auto_timeout_minutes", "60"));
    o += R"HTML("><p class="hint">Suppresses outage detection. Auto-ends after this many minutes.</p></div></div>

<div class="card"><div class="card-title">Battery</div>
<div class="row"><label>Purchase date</label><input type="text" name="battery_purchased" placeholder="YYYY-MM-DD" value=")HTML";
    o += esc(g("battery_purchased", ""));
    o += R"HTML("></div>
<div class="row"><label>Rated capacity (Ah, 0 = auto-detect)</label><input type="number" name="rated_capacity_ah" min="0" step="0.1" value=")HTML";
    o += esc(g("rated_capacity_ah", "0"));
    o += R"HTML("></div>
<div class="row"><label>TX event threshold (amps)</label><input type="number" name="tx_threshold" min="0" step="0.1" value=")HTML";
    o += esc(g("tx_threshold", "1.0"));
    o += R"HTML("><p class="hint">Minimum current change to log a charge/discharge transition event.</p></div></div>

<div class="card"><div class="card-title">Solar &amp; Weather</div>
<div class="row"><label><input type="checkbox" name="solar_enabled")HTML";
    o += chk("solar_enabled");
    o += R"HTML(> Enable solar features</label></div>
<div class="row"><label>Panel rated watts</label><input type="number" name="solar_panel_watts" min="0" value=")HTML";
    o += esc(g("solar_panel_watts", "400"));
    o += R"HTML("></div>
<div class="row"><label>System efficiency (0–1)</label><input type="number" name="solar_system_efficiency" min="0" max="1" step="0.01" value=")HTML";
    o += esc(g("solar_system_efficiency", "0.75"));
    o += R"HTML("></div>
<div class="row"><label>Solar ZIP code</label><input type="text" name="solar_zip_code" value=")HTML";
    o += esc(g("solar_zip_code", "80112"));
    o += R"HTML("></div>
<div class="row"><label>OpenWeather API key</label><input type="text" name="weather_api_key" placeholder="Get one at openweathermap.org" value=")HTML";
    o += esc(g("weather_api_key", ""));
    o += R"HTML("></div>
<div class="row"><label>Weather ZIP code</label><input type="text" name="weather_zip_code" value=")HTML";
    o += esc(g("weather_zip_code", "80112"));
    o += R"HTML("></div>
<div class="row"><button type="button" class="btn btn-sm" onclick="refreshWx()">&#8635; Refresh weather cache</button> <span id="wx-msg" class="hint" style="display:inline"></span></div></div>

<div class="card"><div class="card-title">Serial / PwrGate</div>
<div class="row"><label>Serial device</label><input type="text" name="serial_device" placeholder="/dev/ttyACM0" value=")HTML";
    o += esc(g("serial_device", ""));
    o += R"HTML("></div>
<div class="row"><label>Baud rate</label><input type="number" name="serial_baud" value=")HTML";
    o += esc(g("serial_baud", "115200"));
    o += R"HTML("></div>
<div class="row"><label>Remote PwrGate (host:port)</label><input type="text" name="pwrgate_remote" placeholder="ham-pi:8081" value=")HTML";
    o += esc(g("pwrgate_remote", ""));
    o += R"HTML("></div></div>

<div class="card"><div class="card-title">System</div>
<div class="row"><label>Poll interval (seconds)</label><input type="number" name="poll_interval" min="1" value=")HTML";
    o += esc(g("poll_interval", "5"));
    o += R"HTML("></div>
<div class="row"><label>Database path</label><input type="text" name="db_path" placeholder="default" value=")HTML";
    o += esc(g("db_path", ""));
    o += R"HTML("></div>
<div class="row"><label>DB write interval (seconds)</label><input type="number" name="db_interval" min="0" value=")HTML";
    o += esc(g("db_interval", "60"));
    o += R"HTML("></div>
<div class="row"><label><input type="checkbox" name="daemon")HTML";
    o += chk("daemon");
    o += R"HTML(> Run as daemon (background service)</label></div></div>
<button type="submit" class="btn">Save settings</button><div id="msg" class="msg"></div>
</form>
<script>
(function(){var n=document.querySelectorAll('nav a[href^="/"]');for(var i=0;i<n.length;i++){n[i].addEventListener('click',function(e){if(e.ctrlKey||e.metaKey||e.shiftKey)return;var h=this.getAttribute('href');if(!h)return;e.preventDefault();location.href=h})}})();
document.getElementById('cfg-form').onsubmit=function(e){e.preventDefault();var f=e.target;var o={settings_initialized:'1'};
['device_name','device_address','adapter_path','http_port','http_bind','log_file','verbose','shelly_host','shelly_enabled','shelly_battery_test_auto','shelly_battery_test_max_hours','shelly_battery_test_low_soc','maintenance_auto_timeout_minutes','serial_device','serial_baud','pwrgate_remote','poll_interval','db_path','db_interval','battery_purchased','rated_capacity_ah','tx_threshold','solar_enabled','solar_panel_watts','solar_system_efficiency','solar_zip_code','weather_api_key','weather_zip_code','daemon'].forEach(function(k){var el=f.elements[k];if(el)o[k]=el.type==='checkbox'?(el.checked?'1':'0'):el.value});
fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)})
.then(function(r){if(r.ok){document.getElementById('msg').textContent='Saved. Restart limonitor to apply hardware changes.';document.getElementById('msg').className='msg'}else{throw new Error()}})
.catch(function(){document.getElementById('msg').textContent='Save failed.';document.getElementById('msg').className='msg err'})}
function toggleTheme(){var isLight=document.documentElement.classList.toggle('light');var t=isLight?'light':'dark';document.getElementById('thm-btn').textContent=isLight?'\u263d':'\u263c';fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({theme:t})}).catch(function(){})}
function refreshWx(){var m=document.getElementById('wx-msg');if(m)m.textContent='Refreshing\u2026';fetch('/api/weather_refresh').then(function(r){return r.json()}).then(function(d){if(m)m.textContent=d.ok?'Done! Visit the Solar page to see fresh data.':'Error: '+d.message;if(m)m.style.color=d.ok?'var(--green)':'#dc2626'}).catch(function(e){if(m){m.textContent='Error: '+e.message;m.style.color='#dc2626'}})}
function $(id){return document.getElementById(id)}
function fmt(n,p){return n==null||isNaN(n)?'\u2014':n.toFixed(p)}
function showPowerReasonModal(opts,onConfirm){var title=opts.title||'Reason required';var desc=opts.desc||'';var confirmText=opts.confirmText||'Submit';var modal=$('pwr-reason-modal');var inp=$('pwr-reason-input');var err=$('pwr-modal-err');var submitBtn=$('pwr-modal-submit');if(!modal||!inp)return;$('pwr-modal-title').textContent=title;var descEl=$('pwr-modal-desc');descEl.textContent=desc;descEl.style.display=desc?'':'none';submitBtn.textContent=confirmText;inp.value='';err.style.display='none';submitBtn.disabled=true;modal.style.display='flex';inp.focus();function close(){modal.style.display='none';}$('pwr-modal-cancel').onclick=close;submitBtn.onclick=function(){var r=inp.value.trim();if(!r){err.style.display='';err.textContent='Please enter a reason.';return;}close();onConfirm(r);};inp.oninput=function(){submitBtn.disabled=!inp.value.trim();err.style.display='none';};inp.onkeydown=function(e){if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();if(inp.value.trim())submitBtn.click();}};modal.onclick=function(e){if(e.target===modal)close();};}
function endTestFromBanner(){showPowerReasonModal({title:'End battery test',desc:'This will turn grid back on. A reason is required.',confirmText:'End test'},function(r){fetch('/api/shelly/relay',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({turn:'on',reason:r})}).then(function(x){return x.json()}).then(function(d){if(d.ok){var b=$('test-banner');if(b)b.classList.remove('show')}}).catch(function(){})})}
function upTestBanner(){var b=$('test-banner');if(!b||!b.classList.contains('show'))return;fetch('/api/shelly/status').then(function(r){return r.json()}).then(function(s){if(!s.test_active){b.classList.remove('show');return}Promise.all([fetch('/api/status').then(function(r){return r.json()}),fetch('/api/analytics').then(function(r){return r.json()})]).then(function(arr){var st=arr[0],an=arr[1];var soc=$('tb-soc'),load=$('tb-load'),rt=$('tb-rt');if(soc)soc.textContent=st.valid?fmt(st.soc_pct,1):'\u2014';if(load)load.textContent=an.avg_discharge_24h_w>0?fmt(an.avg_discharge_24h_w,1):'\u2014';if(rt)rt.textContent=an.runtime_from_current_h>0?fmt(an.runtime_from_current_h,1):'\u2014'})})}
fetch('/api/shelly/status').then(function(r){return r.json()}).then(function(s){if(s.test_active){var b=$('test-banner');if(b){b.classList.add('show');upTestBanner();setInterval(upTestBanner,5000)}}}).catch(function(){})
)HTML";
    o += grid_event_banner_js();
    o += R"HTML(</script></body></html>)HTML";
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
