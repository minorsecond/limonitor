#include "system_load.hpp"
#include <algorithm>
#include <cstdio>
#include <locale>
#include <sstream>

// ─── Double-to-string (locale-independent, compact) ────────────────────────

static std::string dbl_str(double v) {
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    // 10 significant digits; trailing zeros stripped by %g behaviour
    oss.precision(10);
    oss << v;
    return oss.str();
}

// ─── JSON string escaping ───────────────────────────────────────────────────

static std::string json_esc(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '"')  r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else r += c;
    }
    return r;
}

// ─── Minimal JSON helpers for parsing ──────────────────────────────────────

// Find the position of a value in a (flat) JSON object string for a given key.
// Returns std::string::npos if not found.
static size_t find_key_value(const std::string& obj, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = obj.find(needle);
    if (pos == std::string::npos) return std::string::npos;
    pos += needle.size();
    while (pos < obj.size() && (obj[pos] == ':' || obj[pos] == ' ' || obj[pos] == '\t')) ++pos;
    return pos;
}

// Extract a JSON string value: "key":"value"
static std::string parse_string_field(const std::string& obj, const std::string& key) {
    size_t pos = find_key_value(obj, key);
    if (pos == std::string::npos || pos >= obj.size() || obj[pos] != '"') return "";
    ++pos; // skip opening "
    std::string r;
    while (pos < obj.size() && obj[pos] != '"') {
        if (obj[pos] == '\\' && pos + 1 < obj.size()) {
            ++pos;
            switch (obj[pos]) {
                case '"':  r += '"'; break;
                case '\\': r += '\\'; break;
                case 'n':  r += '\n'; break;
                case 'r':  r += '\r'; break;
                case 't':  r += '\t'; break;
                default:   r += obj[pos]; break;
            }
        } else {
            r += obj[pos];
        }
        ++pos;
    }
    return r;
}

// Extract a JSON number value: "key":1.23
static double parse_double_field(const std::string& obj, const std::string& key,
                                  double def = 0.0) {
    size_t pos = find_key_value(obj, key);
    if (pos == std::string::npos) return def;
    // Parse until end of number
    std::string num;
    size_t i = pos;
    if (i < obj.size() && (obj[i] == '-' || obj[i] == '+')) num += obj[i++];
    while (i < obj.size() && (std::isdigit(static_cast<unsigned char>(obj[i])) ||
                               obj[i] == '.' || obj[i] == 'e' || obj[i] == 'E' ||
                               obj[i] == '+' || obj[i] == '-')) {
        num += obj[i++];
    }
    if (num.empty()) return def;
    try {
        std::istringstream iss(num);
        iss.imbue(std::locale::classic());
        double v;
        iss >> v;
        return v;
    } catch (...) { return def; }
}

// Split a JSON array string (e.g. "[{...},{...}]") into individual object strings.
// Correctly handles nested objects and strings.
static std::vector<std::string> split_json_objects(const std::string& arr) {
    std::vector<std::string> result;
    size_t pos = arr.find('[');
    if (pos == std::string::npos) return result;
    ++pos;

    while (pos < arr.size()) {
        // Skip whitespace and commas
        while (pos < arr.size() && (arr[pos] == ' ' || arr[pos] == ',' ||
               arr[pos] == '\n' || arr[pos] == '\r' || arr[pos] == '\t')) ++pos;
        if (pos >= arr.size() || arr[pos] == ']') break;

        if (arr[pos] == '{') {
            size_t start = pos;
            int depth = 0;
            bool in_str = false;
            while (pos < arr.size()) {
                char c = arr[pos];
                if (in_str) {
                    if (c == '\\') { ++pos; } // skip escaped char
                    else if (c == '"') in_str = false;
                } else {
                    if (c == '"') in_str = true;
                    else if (c == '{') ++depth;
                    else if (c == '}') {
                        --depth;
                        if (depth == 0) {
                            result.push_back(arr.substr(start, pos - start + 1));
                            ++pos;
                            break;
                        }
                    }
                }
                ++pos;
            }
        } else {
            ++pos; // unexpected character, skip
        }
    }
    return result;
}

// Extract a JSON array value string "[...]" for a key.
static std::string extract_array_field(const std::string& obj, const std::string& key) {
    size_t pos = find_key_value(obj, key);
    if (pos == std::string::npos || pos >= obj.size() || obj[pos] != '[') return "[]";
    size_t start = pos;
    int depth = 0;
    bool in_str = false;
    while (pos < obj.size()) {
        char c = obj[pos];
        if (in_str) {
            if (c == '\\') ++pos;
            else if (c == '"') in_str = false;
        } else {
            if (c == '"') in_str = true;
            else if (c == '[') ++depth;
            else if (c == ']') {
                --depth;
                if (depth == 0) return obj.substr(start, pos - start + 1);
            }
        }
        ++pos;
    }
    return "[]";
}

// ─── TxLevel ────────────────────────────────────────────────────────────────

double TxLevel::efficiency_pct() const {
    if (dc_in_w < 0.01) return 0.0;
    return (rf_out_w / dc_in_w) * 100.0;
}

// ─── SystemLoadConfig ───────────────────────────────────────────────────────

double SystemLoadConfig::total_idle_w() const {
    double sum = 0;
    for (const auto& c : components) sum += c.idle_w;
    return sum;
}

double SystemLoadConfig::total_idle_a() const {
    double sum = 0;
    for (const auto& c : components) sum += c.idle_a;
    return sum;
}

double SystemLoadConfig::effective_idle_w() const {
    if (idle_w_override >= 0.0) return idle_w_override;
    return total_idle_w();
}

double SystemLoadConfig::effective_load_w(double tx_duty_pct,
                                           int comp_idx,
                                           int level_idx) const {
    double idle = effective_idle_w();
    if (comp_idx < 0 || comp_idx >= static_cast<int>(components.size())) return idle;
    const auto& comp = components[comp_idx];
    if (level_idx < 0 || level_idx >= static_cast<int>(comp.tx_levels.size())) return idle;
    double duty = std::max(0.0, std::min(100.0, tx_duty_pct)) / 100.0;
    // dc_in_w is radio-only; add back non-radio system idle to get total TX draw
    double radio_dc_w = comp.tx_levels[level_idx].dc_in_w;
    double total_tx_w = idle - comp.idle_w + radio_dc_w;
    return idle * (1.0 - duty) + total_tx_w * duty;
}

double SystemLoadConfig::runtime_receive_h(double usable_wh) const {
    double load = effective_idle_w();
    if (load < 0.01 || usable_wh <= 0) return 0.0;
    return usable_wh / load;
}

double SystemLoadConfig::runtime_with_tx_h(double usable_wh,
                                            double tx_duty_pct,
                                            int comp_idx,
                                            int level_idx) const {
    double load = effective_load_w(tx_duty_pct, comp_idx, level_idx);
    if (load < 0.01 || usable_wh <= 0) return 0.0;
    return usable_wh / load;
}

std::string SystemLoadConfig::to_json() const {
    std::string o;
    o.reserve(1024);
    o += "{\"idle_w_override\":" + dbl_str(idle_w_override) + ",\"components\":[";
    for (size_t i = 0; i < components.size(); ++i) {
        const auto& c = components[i];
        if (i > 0) o += ',';
        o += "{\"name\":\"" + json_esc(c.name) + "\"";
        o += ",\"purpose\":\"" + json_esc(c.purpose) + "\"";
        o += ",\"idle_a\":" + dbl_str(c.idle_a);
        o += ",\"idle_w\":" + dbl_str(c.idle_w);
        o += ",\"tx_levels\":[";
        for (size_t j = 0; j < c.tx_levels.size(); ++j) {
            const auto& t = c.tx_levels[j];
            if (j > 0) o += ',';
            o += "{\"label\":\"" + json_esc(t.label) + "\"";
            o += ",\"rf_out_w\":" + dbl_str(t.rf_out_w);
            o += ",\"dc_in_a\":" + dbl_str(t.dc_in_a);
            o += ",\"dc_in_w\":" + dbl_str(t.dc_in_w);
            o += '}';
        }
        o += "]}";
    }
    o += "]}";
    return o;
}

SystemLoadConfig SystemLoadConfig::from_json(const std::string& json) {
    SystemLoadConfig cfg;
    cfg.idle_w_override = parse_double_field(json, "idle_w_override", -1.0);
    std::string arr_str = extract_array_field(json, "components");
    auto comp_objs = split_json_objects(arr_str);
    cfg.components.reserve(comp_objs.size());
    for (const auto& co : comp_objs) {
        SystemComponent comp;
        comp.name    = parse_string_field(co, "name");
        comp.purpose = parse_string_field(co, "purpose");
        comp.idle_a  = parse_double_field(co, "idle_a");
        comp.idle_w  = parse_double_field(co, "idle_w");

        std::string tx_arr = extract_array_field(co, "tx_levels");
        auto tx_objs = split_json_objects(tx_arr);
        comp.tx_levels.reserve(tx_objs.size());
        for (const auto& to : tx_objs) {
            TxLevel tx;
            tx.label     = parse_string_field(to, "label");
            tx.rf_out_w  = parse_double_field(to, "rf_out_w");
            tx.dc_in_a   = parse_double_field(to, "dc_in_a");
            tx.dc_in_w   = parse_double_field(to, "dc_in_w");
            comp.tx_levels.push_back(std::move(tx));
        }
        cfg.components.push_back(std::move(comp));
    }
    return cfg;
}

SystemLoadConfig SystemLoadConfig::default_config() {
    SystemLoadConfig cfg;

    // Measured grid-off, battery-to-PwrGate DC line:
    //   PwrGate standby:           0.042 A /  0.55 W
    //   Pi on (no radio):          0.455 A /  6.00 W  → Pi delta: 0.413 A / 5.45 W
    //   Pi + radio receive:        0.745 A /  9.80 W  → Radio delta: 0.290 A / 3.80 W
    //   Non-radio system idle:     0.455 A /  6.00 W  (PwrGate + Pi)
    //   TX  5W RF radio-only:      3.345 A / 44.20 W  (total - non-radio: 3.8 - 0.455, 50.2 - 6.0)
    //   TX 10W RF radio-only:      4.745 A / 62.60 W  (5.2 - 0.455, 68.6 - 6.0)
    //   TX 55W RF radio-only:      9.545 A /126.00 W  (estimated: 10.0 - 0.455, 132.0 - 6.0)

    SystemComponent pwrgate;
    pwrgate.name    = "PwrGate EPG2";
    pwrgate.purpose = "AC-to-DC PSU and battery charger";
    pwrgate.idle_a  = 0.042;
    pwrgate.idle_w  = 0.55;
    cfg.components.push_back(std::move(pwrgate));

    SystemComponent pi;
    pi.name    = "Raspberry Pi";
    pi.purpose = "System controller and BLE monitor";
    pi.idle_a  = 0.413;
    pi.idle_w  = 5.45;
    cfg.components.push_back(std::move(pi));

    SystemComponent radio;
    radio.name    = "IC-2100H Radio";
    radio.purpose = "Ham radio transceiver (2m FM)";
    radio.idle_a  = 0.290;   // receive mode
    radio.idle_w  = 3.80;    // receive mode

    TxLevel tx5;
    tx5.label    = "5W RF";
    tx5.rf_out_w = 5.0;
    tx5.dc_in_a  = 3.345;   // radio-only amps during TX (measured 3.8A - 0.455A non-radio)
    tx5.dc_in_w  = 44.2;    // radio-only watts during TX (measured 50.2W - 6.0W non-radio)

    TxLevel tx10;
    tx10.label    = "10W RF";
    tx10.rf_out_w = 10.0;
    tx10.dc_in_a  = 4.745;  // radio-only (5.2 - 0.455)
    tx10.dc_in_w  = 62.6;   // radio-only (68.6 - 6.0)

    TxLevel tx55;
    tx55.label    = "55W RF";
    tx55.rf_out_w = 55.0;
    tx55.dc_in_a  = 9.545;  // estimated radio-only (10.0 - 0.455)
    tx55.dc_in_w  = 126.0;  // estimated radio-only (132.0 - 6.0)

    radio.tx_levels = {tx5, tx10, tx55};
    cfg.components.push_back(std::move(radio));

    return cfg;
}
