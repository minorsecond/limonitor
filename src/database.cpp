#include "database.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sqlite3.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Open / close
// ---------------------------------------------------------------------------
bool Database::open() {
    // Ensure parent directory exists
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
    if (db_) { sqlite3_close(db_handle(db_)); db_ = nullptr; }
}

bool Database::is_open() const {
    std::lock_guard<std::mutex> lk(mu_);
    return db_ != nullptr;
}

// ---------------------------------------------------------------------------
// Schema migrations
// ---------------------------------------------------------------------------
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
    );
}

// ---------------------------------------------------------------------------
// Inserts
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// History queries (pre-populate DataStore on startup)
// ---------------------------------------------------------------------------
std::vector<BatterySnapshot> Database::load_battery_history(size_t n) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<BatterySnapshot> result;
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

// ---------------------------------------------------------------------------
// Default path
// ---------------------------------------------------------------------------
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
