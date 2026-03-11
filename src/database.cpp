#include "database.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <map>
#include <sqlite3.h>

static inline sqlite3* db_handle(void* p) { return static_cast<sqlite3*>(p); }

Database::Database(std::string path) : path_(std::move(path)) {}
Database::~Database() { close(); }

bool Database::exec(const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db_handle(db_), sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        LOG_ERROR("DB exec: %s", err ? err : "unknown error");
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool Database::open() {
    // mkdir -p parent
    try {
        auto parent = std::filesystem::path(path_).parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent);
    } catch (const std::exception& e) {
        LOG_WARN("DB: cannot create directory for %s: %s", path_.c_str(), e.what());
    }

    sqlite3* handle = nullptr;
    int rc = sqlite3_open_v2(path_.c_str(), &handle,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                             SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR("DB: cannot open %s: %s", path_.c_str(), sqlite3_errmsg(handle));
        sqlite3_close(handle);
        return false;
    }
    db_ = handle;

    if (!migrate()) { close(); return false; }
    LOG_INFO("DB: opened %s", path_.c_str());
    return true;
}

void Database::close() {
    std::lock_guard<std::mutex> lk(mu_);
    if (db_) {
        int rc = sqlite3_wal_checkpoint_v2(db_handle(db_), nullptr,
                                           SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
        if (rc != SQLITE_OK && rc != SQLITE_BUSY)
            LOG_WARN("DB: checkpoint: %s", sqlite3_errmsg(db_handle(db_)));
        sqlite3_close(db_handle(db_));
        db_ = nullptr;
    }
}

void Database::checkpoint() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return;
    int rc = sqlite3_wal_checkpoint_v2(db_handle(db_), nullptr,
                                       SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
    if (rc != SQLITE_OK && rc != SQLITE_BUSY)
        LOG_WARN("DB: checkpoint: %s", sqlite3_errmsg(db_handle(db_)));
}

bool Database::is_open() const {
    std::lock_guard<std::mutex> lk(mu_);
    return db_ != nullptr;
}

bool Database::migrate() {
    // WAL mode: concurrent readers + writer, faster on flash storage
    if (!exec("PRAGMA journal_mode=WAL;")) return false;
    if (!exec("PRAGMA synchronous=NORMAL;")) return false;

    return exec(
        "CREATE TABLE IF NOT EXISTS battery_readings ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts         INTEGER NOT NULL,"   // Unix timestamp (s)
        "  device     TEXT,"
        "  v          REAL,"               // pack voltage V
        "  a          REAL,"               // current A (neg=charging)
        "  soc        REAL,"               // state of charge %
        "  rem_ah     REAL,"               // remaining Ah
        "  nom_ah     REAL,"               // nominal Ah
        "  pwr        REAL,"               // power W
        "  cell_min   REAL,"
        "  cell_max   REAL,"
        "  cell_delta REAL,"
        "  cells      TEXT,"               // comma-sep cell voltages
        "  t1         REAL,"               // temperature sensor 1
        "  t2         REAL"                // temperature sensor 2
        ");"
        "CREATE INDEX IF NOT EXISTS bat_ts ON battery_readings(ts);"

        "CREATE TABLE IF NOT EXISTS charger_readings ("
        "  id     INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts     INTEGER NOT NULL,"
        "  state  TEXT,"
        "  ps_v   REAL,"
        "  bat_v  REAL,"
        "  bat_a  REAL,"
        "  sol_v  REAL,"
        "  tgt_v  REAL,"
        "  tgt_a  REAL,"
        "  stop_a REAL,"
        "  pwm    INTEGER,"
        "  mins   INTEGER,"
        "  temp   INTEGER,"
        "  pss    INTEGER"
        ");"
        "CREATE INDEX IF NOT EXISTS chg_ts ON charger_readings(ts);"

        "CREATE TABLE IF NOT EXISTS system_events ("
        "  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts   INTEGER NOT NULL,"
        "  msg  TEXT NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS ev_ts ON system_events(ts);"

        "CREATE TABLE IF NOT EXISTS ops_events ("
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts           INTEGER NOT NULL,"
        "  type         TEXT NOT NULL,"
        "  subtype      TEXT DEFAULT '',"
        "  message      TEXT NOT NULL,"
        "  notes        TEXT DEFAULT '',"
        "  metadata_json TEXT DEFAULT ''"
        ");"
        "CREATE INDEX IF NOT EXISTS oe_ts ON ops_events(ts);"
        "CREATE INDEX IF NOT EXISTS oe_type ON ops_events(type);"

        "CREATE TABLE IF NOT EXISTS settings ("
        "  key   TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL"
        ");"

        "CREATE TABLE IF NOT EXISTS weather_cache ("
        "  key        TEXT PRIMARY KEY,"  // 'forecast_3h' or 'forecast_daily'
        "  json_data  TEXT NOT NULL,"
        "  fetched_at INTEGER NOT NULL"   // Unix timestamp when fetched from API
        ");"

        "CREATE TABLE IF NOT EXISTS solar_performance ("
        "  id           INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  ts           INTEGER NOT NULL,"
        "  cloud_cover  REAL,"
        "  actual_w     REAL,"
        "  theoretical_w REAL,"
        "  coefficient  REAL,"
        "  sol_v        REAL,"
        "  bat_v        REAL,"
        "  bat_a        REAL,"
        "  soc          REAL"
        ");"
        "CREATE INDEX IF NOT EXISTS sp_ts ON solar_performance(ts);"

        "CREATE TABLE IF NOT EXISTS weather_daily ("
        "  date        TEXT PRIMARY KEY,"
        "  cloud_cover REAL,"
        "  kwh_forecast REAL,"
        "  sun_hours   REAL,"
        "  source      TEXT,"
        "  updated_at  INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS wd_updated ON weather_daily(updated_at);"
    );
}

static std::string settings_file_path(const std::string& db_path) {
    auto p = std::filesystem::path(db_path);
    return (p.parent_path() / "settings.txt").string();
}

static std::string read_setting_from_file(const std::string& path, const std::string& key) {
    std::ifstream f(path);
    if (!f) return {};
    std::string line, prefix = key + "=";
    while (std::getline(f, line)) {
        if (line.size() > prefix.size() && line.compare(0, prefix.size(), prefix) == 0)
            return line.substr(prefix.size());
    }
    return {};
}

static void write_setting_to_file(const std::string& path, const std::string& key,
                                  const std::string& value) {
    std::map<std::string, std::string> m;
    std::ifstream in(path);
    if (in) {
        std::string line;
        while (std::getline(in, line)) {
            auto eq = line.find('=');
            if (eq != std::string::npos)
                m[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    m[key] = value;
    try {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    } catch (...) {}
    std::ofstream out(path);
    if (!out) return;
    for (const auto& kv : m)
        out << kv.first << "=" << kv.second << "\n";
}

void Database::invalidate_settings_cache() const {
    settings_cache_.clear();
}

std::string Database::get_setting(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto now = std::chrono::steady_clock::now();
    auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - settings_cache_time_).count();
    if (age < SETTINGS_CACHE_TTL_MS) {
        auto it = settings_cache_.find(key);
        if (it != settings_cache_.end()) return it->second;
    }
    if (db_) {
        const char* sql = "SELECT value FROM settings WHERE key=?";
        sqlite3_stmt* stmt = nullptr;
        auto* db = db_handle(db_);
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return {};
        sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        std::string result;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            if (const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)))
                result = v;
        }
        sqlite3_finalize(stmt);
        settings_cache_[key] = result;
        settings_cache_time_ = now;
        return result;
    }
    return read_setting_from_file(settings_file_path(path_), key);
}

void Database::set_setting(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(mu_);
    if (db_) {
        const char* sql = "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)";
        sqlite3_stmt* stmt = nullptr;
        auto* db = db_handle(db_);
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
        sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        settings_cache_[key] = value;
        settings_cache_time_ = std::chrono::steady_clock::now();
        return;
    }
    write_setting_to_file(settings_file_path(path_), key, value);
}

void Database::insert_battery(const BatterySnapshot& s) {
    if (!s.valid) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return;

    // Serialize cell voltages as "3.249,3.251,..."
    std::string cells;
    cells.reserve(s.cell_voltages_v.size() * 7);
    for (size_t i = 0; i < s.cell_voltages_v.size(); ++i) {
        if (i) cells += ',';
        char buf[12];
        std::snprintf(buf, sizeof(buf), "%.3f", s.cell_voltages_v[i]);
        cells += buf;
    }

    auto ts = static_cast<sqlite3_int64>(
        std::chrono::system_clock::to_time_t(s.timestamp));
    double t1 = s.temperatures_c.size() > 0 ? s.temperatures_c[0] : 0.0;
    double t2 = s.temperatures_c.size() > 1 ? s.temperatures_c[1] : 0.0;

    const char* sql =
        "INSERT INTO battery_readings"
        " (ts,device,v,a,soc,rem_ah,nom_ah,pwr,cell_min,cell_max,cell_delta,cells,t1,t2)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)";

    sqlite3_stmt* stmt = nullptr;
    auto* db = db_handle(db_);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_WARN("DB: prepare battery: %s", sqlite3_errmsg(db)); return;
    }
    sqlite3_bind_int64(stmt,  1, ts);
    sqlite3_bind_text(stmt,   2, s.device_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, s.total_voltage_v);
    sqlite3_bind_double(stmt, 4, s.current_a);
    sqlite3_bind_double(stmt, 5, s.soc_pct);
    sqlite3_bind_double(stmt, 6, s.remaining_ah);
    sqlite3_bind_double(stmt, 7, s.nominal_ah);
    sqlite3_bind_double(stmt, 8, s.power_w);
    sqlite3_bind_double(stmt, 9, s.cell_min_v);
    sqlite3_bind_double(stmt,10, s.cell_max_v);
    sqlite3_bind_double(stmt,11, s.cell_delta_v);
    sqlite3_bind_text(stmt,  12, cells.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt,13, t1);
    sqlite3_bind_double(stmt,14, t2);

    if (sqlite3_step(stmt) != SQLITE_DONE)
        LOG_WARN("DB: insert battery: %s", sqlite3_errmsg(db));
    sqlite3_finalize(stmt);
}

void Database::insert_charger(const PwrGateSnapshot& p) {
    if (!p.valid) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return;

    auto ts = static_cast<sqlite3_int64>(
        std::chrono::system_clock::to_time_t(p.timestamp));

    const char* sql =
        "INSERT INTO charger_readings"
        " (ts,state,ps_v,bat_v,bat_a,sol_v,tgt_v,tgt_a,stop_a,pwm,mins,temp,pss)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)";

    sqlite3_stmt* stmt = nullptr;
    auto* db = db_handle(db_);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_WARN("DB: prepare charger: %s", sqlite3_errmsg(db)); return;
    }
    sqlite3_bind_int64(stmt,  1, ts);
    sqlite3_bind_text(stmt,   2, p.state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 3, p.ps_v);
    sqlite3_bind_double(stmt, 4, p.bat_v);
    sqlite3_bind_double(stmt, 5, p.bat_a);
    sqlite3_bind_double(stmt, 6, p.sol_v);
    sqlite3_bind_double(stmt, 7, p.target_v);
    sqlite3_bind_double(stmt, 8, p.target_a);
    sqlite3_bind_double(stmt, 9, p.stop_a);
    sqlite3_bind_int(stmt,   10, p.pwm);
    sqlite3_bind_int(stmt,   11, p.minutes);
    sqlite3_bind_int(stmt,   12, p.temp);
    sqlite3_bind_int(stmt,   13, p.pss);

    if (sqlite3_step(stmt) != SQLITE_DONE)
        LOG_WARN("DB: insert charger: %s", sqlite3_errmsg(db));
    sqlite3_finalize(stmt);
}

void Database::insert_system_event(const SystemEvent& e) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return;

    auto ts = static_cast<sqlite3_int64>(
        std::chrono::system_clock::to_time_t(e.timestamp));

    const char* sql = "INSERT INTO system_events (ts, msg) VALUES (?, ?)";
    sqlite3_stmt* stmt = nullptr;
    auto* db = db_handle(db_);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_WARN("DB: prepare system_event: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_text(stmt, 2, e.message.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE)
        LOG_WARN("DB: insert system_event: %s", sqlite3_errmsg(db));
    sqlite3_finalize(stmt);
}

std::vector<SystemEvent> Database::load_system_events(size_t n) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<SystemEvent> result;
    result.reserve(std::min(n, size_t{200}));
    if (!db_) return result;

    char sql[80];
    std::snprintf(sql, sizeof(sql),
        "SELECT ts, msg FROM system_events ORDER BY ts DESC LIMIT %zu", n);

    sqlite3_stmt* stmt = nullptr;
    auto* db = db_handle(db_);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SystemEvent e;
        e.timestamp = std::chrono::system_clock::from_time_t(
            static_cast<time_t>(sqlite3_column_int64(stmt, 0)));
        if (const char* m = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)))
            e.message = m;
        result.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    std::reverse(result.begin(), result.end());  // oldest first
    return result;
}

void Database::insert_ops_event(const OpsEvent& e) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return;

    auto ts = static_cast<sqlite3_int64>(
        std::chrono::system_clock::to_time_t(e.timestamp));

    const char* sql = "INSERT INTO ops_events (ts, type, subtype, message, notes, metadata_json) VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt = nullptr;
    auto* db = db_handle(db_);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        LOG_WARN("DB: prepare ops_event: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int64(stmt, 1, ts);
    sqlite3_bind_text(stmt, 2, e.type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, e.subtype.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, e.message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, e.notes.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, e.metadata_json.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) != SQLITE_DONE)
        LOG_WARN("DB: insert ops_event: %s", sqlite3_errmsg(db));
    sqlite3_finalize(stmt);
}

std::vector<OpsEvent> Database::load_ops_events(size_t n, const std::string& type_filter) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<OpsEvent> result;
    result.reserve(std::min(n, size_t{500}));
    if (!db_) return result;

    std::string sql = "SELECT id, ts, type, subtype, message, notes, metadata_json FROM ops_events ORDER BY ts DESC LIMIT " + std::to_string(std::min(n, size_t{500}));
    if (!type_filter.empty())
        sql = "SELECT id, ts, type, subtype, message, notes, metadata_json FROM ops_events WHERE type=? ORDER BY ts DESC LIMIT " + std::to_string(std::min(n, size_t{500}));

    sqlite3_stmt* stmt = nullptr;
    auto* db = db_handle(db_);
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    if (!type_filter.empty())
        sqlite3_bind_text(stmt, 1, type_filter.c_str(), -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OpsEvent e;
        e.id = sqlite3_column_int64(stmt, 0);
        e.timestamp = std::chrono::system_clock::from_time_t(
            static_cast<time_t>(sqlite3_column_int64(stmt, 1)));
        if (const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))) e.type = p;
        if (const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3))) e.subtype = p;
        if (const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4))) e.message = p;
        if (const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5))) e.notes = p;
        if (const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6))) e.metadata_json = p;
        result.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
    std::reverse(result.begin(), result.end());
    return result;
}

bool Database::update_ops_event_subtype(int64_t id, const std::string& subtype) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return false;

    const char* sql = "UPDATE ops_events SET subtype=? WHERE id=?";
    sqlite3_stmt* stmt = nullptr;
    auto* db = db_handle(db_);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, subtype.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, id);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<BatterySnapshot> Database::load_battery_history(size_t n) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<BatterySnapshot> result;
    result.reserve(std::min(n, size_t{2880}));
    if (!db_) return result;

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "SELECT ts,device,v,a,soc,rem_ah,nom_ah,pwr,cell_min,cell_max,cell_delta,cells,t1,t2 "
        "FROM battery_readings ORDER BY ts DESC LIMIT %zu", n);

    sqlite3_stmt* stmt = nullptr;
    auto* db = db_handle(db_);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        BatterySnapshot s;
        s.timestamp = std::chrono::system_clock::from_time_t(
            static_cast<time_t>(sqlite3_column_int64(stmt, 0)));
        if (auto* t = sqlite3_column_text(stmt, 1)) s.device_name = reinterpret_cast<const char*>(t);
        s.total_voltage_v = sqlite3_column_double(stmt, 2);
        s.current_a       = sqlite3_column_double(stmt, 3);
        s.soc_pct         = sqlite3_column_double(stmt, 4);
        s.remaining_ah    = sqlite3_column_double(stmt, 5);
        s.nominal_ah      = sqlite3_column_double(stmt, 6);
        s.power_w         = sqlite3_column_double(stmt, 7);
        s.cell_min_v      = sqlite3_column_double(stmt, 8);
        s.cell_max_v      = sqlite3_column_double(stmt, 9);
        s.cell_delta_v    = sqlite3_column_double(stmt, 10);
        if (auto* c = sqlite3_column_text(stmt, 11)) {
            const char* p = reinterpret_cast<const char*>(c);
            while (*p) {
                char* end;
                double v = std::strtod(p, &end);
                if (end == p) break;
                s.cell_voltages_v.push_back(v);
                p = (*end == ',') ? end + 1 : end;
            }
        }
        double t1 = sqlite3_column_double(stmt, 12);
        double t2 = sqlite3_column_double(stmt, 13);
        if (t1 != 0.0) s.temperatures_c.push_back(t1);
        if (t2 != 0.0) s.temperatures_c.push_back(t2);
        s.valid = true;
        result.push_back(std::move(s));
    }
    sqlite3_finalize(stmt);
    std::reverse(result.begin(), result.end()); // oldest first
    LOG_INFO("DB: loaded %zu battery rows", result.size());
    return result;
}

std::vector<PwrGateSnapshot> Database::load_charger_history(size_t n) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<PwrGateSnapshot> result;
    result.reserve(std::min(n, size_t{2880}));
    if (!db_) return result;

    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "SELECT ts,state,ps_v,bat_v,bat_a,sol_v,tgt_v,tgt_a,stop_a,pwm,mins,temp,pss "
        "FROM charger_readings ORDER BY ts DESC LIMIT %zu", n);

    sqlite3_stmt* stmt = nullptr;
    auto* db = db_handle(db_);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PwrGateSnapshot p;
        p.timestamp = std::chrono::system_clock::from_time_t(
            static_cast<time_t>(sqlite3_column_int64(stmt, 0)));
        if (auto* t = sqlite3_column_text(stmt, 1)) p.state = reinterpret_cast<const char*>(t);
        p.ps_v     = sqlite3_column_double(stmt, 2);
        p.bat_v    = sqlite3_column_double(stmt, 3);
        p.bat_a    = sqlite3_column_double(stmt, 4);
        p.sol_v    = sqlite3_column_double(stmt, 5);
        p.target_v = sqlite3_column_double(stmt, 6);
        p.target_a = sqlite3_column_double(stmt, 7);
        p.stop_a   = sqlite3_column_double(stmt, 8);
        p.pwm      = sqlite3_column_int(stmt,  9);
        p.minutes  = sqlite3_column_int(stmt, 10);
        p.temp     = sqlite3_column_int(stmt, 11);
        p.pss      = sqlite3_column_int(stmt, 12);
        p.valid    = true;
        result.push_back(std::move(p));
    }
    sqlite3_finalize(stmt);
    std::reverse(result.begin(), result.end()); // oldest first
    LOG_INFO("DB: loaded %zu charger rows", result.size());
    return result;
}

std::vector<UsageSlotProfile> Database::get_usage_profile(int days_back) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<UsageSlotProfile> result(8);
    for (int i = 0; i < 8; ++i) result[i].slot = i;
    if (!db_) return result;

    auto* db = db_handle(db_);
    auto cutoff = std::time(nullptr) - days_back * 86400;
    char sql[512];
    std::snprintf(sql, sizeof(sql),
        "SELECT (CAST(strftime('%%H', ts, 'unixepoch', 'localtime') AS INTEGER) / 3) AS slot, "
        "AVG(pwr), "
        "CASE WHEN COUNT(*) > 1 THEN "
        "  SQRT(MAX(0, SUM(pwr*pwr)/(COUNT(*)-1) "
        "    - (SUM(pwr)*SUM(pwr))/(COUNT(*)*(COUNT(*)-1)))) "
        "ELSE 0 END, "
        "COUNT(*) "
        "FROM battery_readings "
        "WHERE ts > %lld AND a > 0.05 "
        "GROUP BY slot ORDER BY slot",
        (long long)cutoff);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int slot = sqlite3_column_int(stmt, 0);
        if (slot >= 0 && slot < 8) {
            result[slot].avg_w = sqlite3_column_double(stmt, 1);
            result[slot].stddev_w = sqlite3_column_double(stmt, 2);
            result[slot].sample_count = sqlite3_column_int(stmt, 3);
        }
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<ChargeAcceptanceBucket> Database::get_charge_acceptance_profile(int days_back) const {
    std::lock_guard<std::mutex> lk(mu_);
    // 10 buckets of 10% each: 0-10, 10-20, ..., 90-100
    std::vector<ChargeAcceptanceBucket> result(10);
    for (int i = 0; i < 10; ++i) { result[i].soc_lo = i * 10; result[i].soc_hi = (i + 1) * 10; }
    if (!db_) return result;

    auto* db = db_handle(db_);
    auto cutoff = std::time(nullptr) - days_back * 86400;

    // Time-correlate charger_readings with battery_readings:
    // for each charger row where bat_a > 0.1 and tgt_a > 0.5 (actively charging),
    // find the closest battery_readings.soc within ±60s.
    const char* sql =
        "SELECT CAST(b.soc / 10 AS INTEGER) AS bucket, "
        "       AVG(c.bat_a), AVG(c.tgt_a), "
        "       AVG(c.bat_a / c.tgt_a), "
        "       CASE WHEN COUNT(*) > 1 THEN "
        "         SQRT(MAX(0, SUM((c.bat_a/c.tgt_a)*(c.bat_a/c.tgt_a))/(COUNT(*)-1) "
        "              - (SUM(c.bat_a/c.tgt_a)*SUM(c.bat_a/c.tgt_a))/(COUNT(*)*(COUNT(*)-1)))) "
        "       ELSE 0 END, "
        "       COUNT(*) "
        "FROM charger_readings c "
        "JOIN battery_readings b ON b.ts BETWEEN c.ts - 60 AND c.ts + 60 "
        "WHERE c.ts > ?1 AND c.bat_a > 0.1 AND c.tgt_a > 0.5 "
        "  AND b.soc >= 0 AND b.soc <= 100 "
        "GROUP BY bucket ORDER BY bucket";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_int64(stmt, 1, (int64_t)cutoff);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int bucket = sqlite3_column_int(stmt, 0);
        if (bucket < 0) bucket = 0;
        if (bucket > 9) bucket = 9;
        result[bucket].avg_current_a    = sqlite3_column_double(stmt, 1);
        result[bucket].avg_target_a     = sqlite3_column_double(stmt, 2);
        result[bucket].acceptance_ratio = sqlite3_column_double(stmt, 3);
        result[bucket].stddev_ratio     = sqlite3_column_double(stmt, 4);
        result[bucket].sample_count     = sqlite3_column_int(stmt, 5);
    }
    sqlite3_finalize(stmt);

    for (auto& b : result) {
        if (b.acceptance_ratio > 1.0) b.acceptance_ratio = 1.0;
        if (b.acceptance_ratio < 0.0) b.acceptance_ratio = 0.0;
    }

    // Fill empty buckets from nearest neighbor with data
    for (int i = 0; i < 10; ++i) {
        if (result[i].sample_count >= 3) continue;
        for (int d = 1; d <= 9; ++d) {
            if (i - d >= 0 && result[i - d].sample_count >= 3) {
                result[i].acceptance_ratio = result[i - d].acceptance_ratio;
                break;
            }
            if (i + d < 10 && result[i + d].sample_count >= 3) {
                result[i].acceptance_ratio = result[i + d].acceptance_ratio;
                break;
            }
        }
    }

    return result;
}

void Database::save_weather_cache(const std::string& key, const std::string& json) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_ || json.empty()) return;
    auto* db = db_handle(db_);
    const char* sql = "INSERT OR REPLACE INTO weather_cache (key, json_data, fetched_at) VALUES (?1, ?2, ?3)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    auto now = std::time(nullptr);
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, now);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::string Database::load_weather_cache(const std::string& key, int64_t max_age_sec) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return {};
    auto* db = db_handle(db_);
    auto cutoff = std::time(nullptr) - max_age_sec;
    const char* sql = "SELECT json_data FROM weather_cache WHERE key = ?1 AND fetched_at > ?2";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return {};
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, cutoff);
    std::string result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (auto* t = sqlite3_column_text(stmt, 0))
            result = reinterpret_cast<const char*>(t);
    }
    sqlite3_finalize(stmt);
    return result;
}

void Database::upsert_weather_daily(const WeatherDailyRow& row) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_ || row.date.empty()) return;
    auto* db = db_handle(db_);
    const char* sql =
        "INSERT OR REPLACE INTO weather_daily (date, cloud_cover, kwh_forecast, sun_hours, source, updated_at)"
        " VALUES (?1, ?2, ?3, ?4, ?5, ?6)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, row.date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, row.cloud_cover);
    sqlite3_bind_double(stmt, 3, row.kwh_forecast);
    sqlite3_bind_double(stmt, 4, row.sun_hours);
    sqlite3_bind_text(stmt, 5, row.source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, row.updated_at > 0 ? row.updated_at : std::time(nullptr));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<Database::WeatherDailyRow> Database::load_weather_daily(int days_back) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<WeatherDailyRow> result;
    if (!db_) return result;
    auto* db = db_handle(db_);
    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "SELECT date, cloud_cover, kwh_forecast, sun_hours, source, updated_at "
        "FROM weather_daily WHERE updated_at > %lld ORDER BY date",
        (long long)(std::time(nullptr) - days_back * 86400));
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WeatherDailyRow r;
        if (auto* t = sqlite3_column_text(stmt, 0)) r.date = reinterpret_cast<const char*>(t);
        r.cloud_cover = sqlite3_column_double(stmt, 1);
        r.kwh_forecast = sqlite3_column_double(stmt, 2);
        r.sun_hours = sqlite3_column_double(stmt, 3);
        if (auto* t = sqlite3_column_text(stmt, 4)) r.source = reinterpret_cast<const char*>(t);
        r.updated_at = sqlite3_column_int64(stmt, 5);
        result.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return result;
}

void Database::insert_solar_performance(const SolarPerfRow& row) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return;
    auto* db = db_handle(db_);
    const char* sql =
        "INSERT INTO solar_performance (ts, cloud_cover, actual_w, theoretical_w, coefficient, sol_v, bat_v, bat_a, soc)"
        " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, row.ts > 0 ? row.ts : std::time(nullptr));
    sqlite3_bind_double(stmt, 2, row.cloud_cover);
    sqlite3_bind_double(stmt, 3, row.actual_w);
    sqlite3_bind_double(stmt, 4, row.theoretical_w);
    sqlite3_bind_double(stmt, 5, row.coefficient);
    sqlite3_bind_double(stmt, 6, row.sol_v);
    sqlite3_bind_double(stmt, 7, row.bat_v);
    sqlite3_bind_double(stmt, 8, row.bat_a);
    sqlite3_bind_double(stmt, 9, row.soc);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<Database::SolarPerfRow> Database::load_solar_performance(int days_back) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<SolarPerfRow> result;
    if (!db_) return result;
    auto* db = db_handle(db_);
    auto cutoff = std::time(nullptr) - days_back * 86400;
    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "SELECT ts, cloud_cover, actual_w, theoretical_w, coefficient, sol_v, bat_v, bat_a, soc "
        "FROM solar_performance WHERE ts > %lld ORDER BY ts", (long long)cutoff);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return result;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SolarPerfRow r;
        r.ts = sqlite3_column_int64(stmt, 0);
        r.cloud_cover = sqlite3_column_double(stmt, 1);
        r.actual_w = sqlite3_column_double(stmt, 2);
        r.theoretical_w = sqlite3_column_double(stmt, 3);
        r.coefficient = sqlite3_column_double(stmt, 4);
        r.sol_v = sqlite3_column_double(stmt, 5);
        r.bat_v = sqlite3_column_double(stmt, 6);
        r.bat_a = sqlite3_column_double(stmt, 7);
        r.soc = sqlite3_column_double(stmt, 8);
        result.push_back(r);
    }
    sqlite3_finalize(stmt);
    return result;
}

double Database::get_avg_solar_coefficient(int days_back) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (!db_) return 0;
    auto* db = db_handle(db_);
    auto cutoff = std::time(nullptr) - days_back * 86400;
    char sql[256];
    std::snprintf(sql, sizeof(sql),
        "SELECT AVG(coefficient), COUNT(*) FROM solar_performance "
        "WHERE ts > %lld AND coefficient > 0 AND coefficient < 2", (long long)cutoff);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return 0;
    double avg = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int n = sqlite3_column_int(stmt, 1);
        if (n > 0) avg = sqlite3_column_double(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return avg;
}

std::string Database::default_path() {
#ifdef __APPLE__
    const char* home = std::getenv("HOME");
    if (home)
        return std::string(home) + "/Library/Application Support/limonitor/limonitor.db";
#else
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg)
        return std::string(xdg) + "/limonitor/limonitor.db";
    const char* home = std::getenv("HOME");
    if (home)
        return std::string(home) + "/.local/share/limonitor/limonitor.db";
#endif
    return "./limonitor.db";
}
