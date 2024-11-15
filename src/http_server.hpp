#pragma once
#include "data_store.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

// Simple single-threaded HTTP/1.0 server.
//
// Endpoints:
//   GET /               HTML dashboard
//   GET /api/status     Full battery JSON
//   GET /api/cells      Cell voltages JSON
//   GET /api/history    Last N snapshots JSON  (?n=100)
//   GET /metrics        Prometheus text format
class HttpServer {
public:
    HttpServer(DataStore& store, const std::string& bind_addr, uint16_t port);
    ~HttpServer();

    bool start();
    void stop();

    uint16_t port() const { return port_; }

private:
    DataStore&   store_;
    std::string  bind_addr_;
    uint16_t     port_;
    int          listen_fd_{-1};
    std::atomic<bool> running_{false};
    std::thread  thread_;

    void serve_loop();
    void handle(int client_fd);

    static std::string status_json(const BatterySnapshot& s, const std::string& ble_state);
    static std::string cells_json(const BatterySnapshot& s);
    static std::string history_json(const std::vector<BatterySnapshot>& snaps);
    static std::string prometheus(const BatterySnapshot& s);
    static std::string svg_history_chart(const std::vector<BatterySnapshot>& hist);
    static std::string html_dashboard(const BatterySnapshot& s, const std::string& ble_state,
                                      const std::vector<BatterySnapshot>& hist);

    static void send_response(int fd, int code, const char* content_type, const std::string& body);
};
