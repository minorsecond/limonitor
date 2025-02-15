#pragma once
#include "data_store.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

// HTTP/1.0 server. GET /, /api/status, /api/cells, /api/history, /metrics
class HttpServer {
public:
    HttpServer(DataStore& store, const std::string& bind_addr, uint16_t port,
               int poll_interval_s = 5);
    ~HttpServer();

    bool start();
    void stop();

    uint16_t port() const { return port_; }

private:
    DataStore&   store_;
    std::string  bind_addr_;
    uint16_t     port_;
    int          listen_fd_{-1};
    int          poll_interval_s_{5};
    std::atomic<bool> running_{false};
    std::thread  thread_;

    void serve_loop();
    void handle(int client_fd);

    static std::string status_json(const BatterySnapshot& s, const std::string& ble_state);
    static std::string cells_json(const BatterySnapshot& s);
    static std::string history_json(const std::vector<BatterySnapshot>& snaps);
    static std::string prometheus(const BatterySnapshot& s);
    static std::string prometheus_charger(const PwrGateSnapshot& pg);
    static std::string svg_history_chart(const std::vector<BatterySnapshot>& hist);
    static std::string svg_charger_chart(const std::vector<PwrGateSnapshot>& hist);
    static std::string charger_json(const PwrGateSnapshot& pg);
    static std::string charger_history_json(const std::vector<PwrGateSnapshot>& snaps);
    static std::string html_dashboard(const BatterySnapshot& s, const std::string& ble_state,
                                      const PwrGateSnapshot& pg,
                                      const std::string& purchase_date,
                                      double hours,
                                      int poll_interval_s);

    static void send_response(int fd, int code, const char* content_type, const std::string& body);
};
