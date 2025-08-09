#pragma once
#include "data_store.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

// TUI. ncurses or plain stdout.
class TUI {
public:
    explicit TUI(DataStore& store, uint16_t http_port = 8080);
    ~TUI();

    // Called when the user selects a device from the picker.
    // Typically wired to BleManager::connect_to().
    void set_connect_callback(std::function<void(const std::string&)> cb);

    // Called when the user presses 's' to open settings. Typically runs run_tui_settings().
    void set_settings_callback(std::function<void()> cb);

    // Run the UI loop. Blocks until the user presses 'q' or stop() is called.
    void run();
    void stop();

private:
    DataStore&        store_;
    uint16_t          http_port_;
    std::atomic<bool> running_{true};
    std::function<void(const std::string&)> connect_cb_;
    std::function<void()> settings_cb_;
    int picker_sel_{0};

#ifdef HAVE_NCURSES
    void run_ncurses();
    void draw(const BatterySnapshot& snap, const std::string& ble_st,
              const std::vector<BatterySnapshot>& hist);
    void draw_picker(const std::vector<DiscoveredDevice>& devs, const std::string& ble_st);
    void draw_help();
    bool poll_input(const std::vector<DiscoveredDevice>& devs);
    bool show_help_{false};
#endif
    void run_plain();
    static void print_snap(const BatterySnapshot& snap, const std::string& ble_st);
};
