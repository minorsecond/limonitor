#pragma once
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s\n", msg); \
            exit(1); \
        } \
    } while (0)

inline std::string http_request(const std::string &host, uint16_t port, const std::string &method, const std::string &path, const std::string &body) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock);
        return "";
    }

    std::string request = method + " " + path + " HTTP/1.1\r\n";
    request += "Host: " + host + "\r\n";
    request += "Content-Length: " + std::to_string(body.length()) + "\r\n";
    request += "Connection: close\r\n\r\n";
    request += body;

    send(sock, request.c_str(), request.length(), 0);

    char buffer[4096];
    std::string response;
    int bytes;
    while ((bytes = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, bytes);
    }

    close(sock);
    return response;
}

inline int parse_http_status(const std::string &response) {
    if (response.find("HTTP/1.0 ") == 0) {
        return std::stoi(response.substr(9, 3));
    }
    if (response.find("HTTP/1.1 ") == 0) {
        return std::stoi(response.substr(9, 3));
    }
    return -1;
}

inline bool approx_eq(double a, double b, double tol = 0.001) {
    return std::abs(a - b) < tol;
}

inline double centered_regression_slope(const std::vector<double>& xs, const std::vector<double>& ys) {
    size_t n = std::min(xs.size(), ys.size());
    if (n < 2) return 0;
    double sum_x = 0, sum_y = 0;
    for (size_t i = 0; i < n; ++i) { sum_x += xs[i]; sum_y += ys[i]; }
    double x_bar = sum_x / n;
    double y_bar = sum_y / n;
    double sum_dxdy = 0, sum_dx2 = 0;
    for (size_t i = 0; i < n; ++i) {
        double dx = xs[i] - x_bar;
        double dy = ys[i] - y_bar;
        sum_dx2 += dx * dx;
        sum_dxdy += dx * dy;
    }
    if (sum_dx2 < 1e-12) return 0;
    return sum_dxdy / sum_dx2;
}

inline double estimate_runtime_hours(double soc_pct, double capacity_wh, double load_w, double cutoff_pct) {
    if (load_w <= 0.001) return 999.9;
    if (capacity_wh <= 0) return 0.0;
    if (soc_pct <= cutoff_pct) return 0.0;
    double usable_wh = (soc_pct - cutoff_pct) / 100.0 * capacity_wh;
    return usable_wh / load_w;
}

inline std::vector<double> compute_soc_trend(double start_soc_pct, double capacity_wh, const std::vector<double>& charger_w, const std::vector<double>& load_w) {
    std::vector<double> trend;
    double current_soc = std::clamp(start_soc_pct, 0.0, 100.0);
    
    if (charger_w.empty() || load_w.empty()) {
        trend.push_back(current_soc);
        return trend;
    }
    
    size_t n = std::min(charger_w.size(), load_w.size());
    if (capacity_wh <= 0) {
        for (size_t i = 0; i < n; ++i) trend.push_back(current_soc);
        return trend;
    }
    
    for (size_t i = 0; i < n; ++i) {
        double net_w = charger_w[i] - load_w[i];
        double delta_soc = (net_w / capacity_wh) * 100.0;
        current_soc = std::clamp(current_soc + delta_soc, 0.0, 100.0);
        trend.push_back(current_soc);
    }
    return trend;
}

inline std::vector<double> scale_profile_to_measured(const std::vector<double>& profile, double measured_total) {
    double profile_sum = 0;
    for (double v : profile) profile_sum += v;
    std::vector<double> result;
    if (profile_sum < 1e-6) {
        double val = measured_total / std::max((size_t)1, profile.size());
        for (size_t i = 0; i < profile.size(); ++i) result.push_back(val);
        return result;
    }
    double factor = measured_total / profile_sum;
    for (double v : profile) result.push_back(v * factor);
    return result;
}
