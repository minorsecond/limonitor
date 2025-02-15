#include "pwrgate_client.hpp"
#include "logger.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

PwrGateClient::PwrGateClient(std::string host, uint16_t port, int poll_interval_s)
    : host_(std::move(host)), port_(port), poll_interval_s_(poll_interval_s) {}

PwrGateClient::~PwrGateClient() { stop(); }

bool PwrGateClient::start() {
    running_ = true;
    thread_ = std::thread(&PwrGateClient::poll_loop, this);
    LOG_INFO("PwrGateClient: polling http://%s:%d/api/charger every %ds",
             host_.c_str(), port_, poll_interval_s_);
    return true;
}

void PwrGateClient::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

std::string PwrGateClient::http_get(const std::string& path) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[8];
    std::snprintf(portstr, sizeof(portstr), "%u", port_);

    if (getaddrinfo(host_.c_str(), portstr, &hints, &res) != 0) {
        LOG_DEBUG("PwrGateClient: cannot resolve %s", host_.c_str());
        return {};
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return {}; }

    // 5-second timeouts for connect, send, recv
    struct timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        LOG_DEBUG("PwrGateClient: cannot connect to %s:%d: %s",
                  host_.c_str(), port_, strerror(errno));
        freeaddrinfo(res); close(fd);
        return {};
    }
    freeaddrinfo(res);

    std::string req = "GET " + path + " HTTP/1.0\r\n"
                      "Host: " + host_ + "\r\n"
                      "Connection: close\r\n\r\n";
    send(fd, req.data(), req.size(), MSG_NOSIGNAL);

    std::string resp;
    resp.reserve(2048);
    char buf[1024];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
        resp.append(buf, static_cast<size_t>(n));
    close(fd);

    // Strip HTTP headers (everything before the blank line)
    auto pos = resp.find("\r\n\r\n");
    if (pos == std::string::npos) return {};
    return resp.substr(pos + 4);
}

static double jval_d(const std::string& body, const std::string& key) {
    // Matches both `"key": 1.23` and `"key":1.23`
    for (auto sep : {"\": ", "\":"}) {
        std::string pat = "\"" + key + sep;
        auto p = body.find(pat);
        if (p == std::string::npos) continue;
        p += pat.size();
        double v = 0.0;
        std::sscanf(body.c_str() + p, "%lf", &v);
        return v;
    }
    return 0.0;
}
static int jval_i(const std::string& b, const std::string& k) {
    return static_cast<int>(jval_d(b, k));
}
static std::string jval_s(const std::string& body, const std::string& key) {
    for (auto sep : {"\": \"", "\":\""}) {
        std::string pat = "\"" + key + sep;
        auto p = body.find(pat);
        if (p == std::string::npos) continue;
        p += pat.size();
        auto e = body.find('"', p);
        if (e == std::string::npos) continue;
        return body.substr(p, e - p);
    }
    return {};
}

bool PwrGateClient::parse_json(const std::string& body, PwrGateSnapshot& snap) {
    if (body.empty()) return false;
    // Must have "valid": true
    if (body.find("\"valid\": true") == std::string::npos &&
        body.find("\"valid\":true")  == std::string::npos)
        return false;

    snap.state    = jval_s(body, "state");
    snap.ps_v     = jval_d(body, "ps_v");
    snap.bat_v    = jval_d(body, "bat_v");
    snap.bat_a    = jval_d(body, "bat_a");
    snap.sol_v    = jval_d(body, "sol_v");
    snap.target_v = jval_d(body, "target_v");
    snap.target_a = jval_d(body, "target_a");
    snap.stop_a   = jval_d(body, "stop_a");
    snap.minutes  = jval_i(body, "minutes");
    snap.pwm      = jval_i(body, "pwm");
    snap.temp     = jval_i(body, "temp");
    snap.pss      = jval_i(body, "pss");
    snap.timestamp = std::chrono::system_clock::now();
    snap.valid = true;
    return true;
}

void PwrGateClient::poll_loop() {
    while (running_) {
        std::string body = http_get("/api/charger");
        if (!body.empty()) {
            PwrGateSnapshot snap;
            if (parse_json(body, snap)) {
                if (cb_) cb_(snap);
                LOG_DEBUG("PwrGateClient: %s  PS=%.2fV  Bat=%.2fV %.2fA  Sol=%.2fV",
                          snap.state.c_str(), snap.ps_v, snap.bat_v, snap.bat_a, snap.sol_v);
            } else if (body.find("\"valid\": false") != std::string::npos ||
                       body.find("\"valid\":false")  != std::string::npos) {
                LOG_DEBUG("PwrGateClient: %s:%d has no charger data yet",
                          host_.c_str(), port_);
            } else {
                LOG_WARN("PwrGateClient: unexpected response from %s:%d: %.80s",
                         host_.c_str(), port_, body.c_str());
            }
        }

        // Sleep in small increments so stop() wakes up promptly
        for (int i = 0; i < poll_interval_s_ * 10 && running_; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
