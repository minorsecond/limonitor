#pragma once
#include "battery_data.hpp"
#include "pwrgate.hpp"
#include <mutex>
#include <string>

// Thread-safe SQLite3 persistence layer.
// Schema:
//   battery_readings — one row per BatterySnapshot insert
//   charger_readings — one row per PwrGateSnapshot insert
//
// sqlite3* is opaque (forward-declared as void*) so callers don't need sqlite3.h.
class Database {
public:
    explicit Database(std::string path);
    ~Database();

    // Open (or create) the database and run schema migrations.
    bool open();
    void close();
    bool is_open() const;

    void insert_battery(const BatterySnapshot& s);
    void insert_charger(const PwrGateSnapshot& p);

    // Load historical rows (oldest first) to pre-populate DataStore on startup.
    std::vector<BatterySnapshot>  load_battery_history(size_t n = 2880) const;
    std::vector<PwrGateSnapshot>  load_charger_history(size_t n = 2880) const;

    const std::string& path() const { return path_; }

    // Platform default:
    //   macOS: ~/Library/Application Support/limonitor/limonitor.db
    //   Linux: $XDG_DATA_HOME/limonitor/limonitor.db  (or ~/.local/share/...)
    static std::string default_path();

private:
    std::string        path_;
    void*              db_{nullptr};   // sqlite3*
    mutable std::mutex mu_;

    bool migrate();
    bool exec(const char* sql);
};
