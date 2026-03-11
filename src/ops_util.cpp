#include "ops_util.hpp"
#include <cstdio>
#include <sstream>

namespace {

static std::string escape_json_str(const std::string& s) {
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

}  // namespace

std::string build_ops_snapshot_json(
    const std::optional<BatterySnapshot>* snap,
    const std::optional<PwrGateSnapshot>* pg) {
    std::ostringstream out;
    bool first = true;
    auto sep = [&]() {
        if (!first) out << ',';
        first = false;
    };

    if (snap && *snap) {
        const BatterySnapshot& s = **snap;
        sep(); out << "\"soc\":" << s.soc_pct;
        sep(); out << "\"voltage\":" << s.total_voltage_v;
        sep(); out << "\"current_a\":" << s.current_a;
        sep(); out << "\"load\":" << std::abs(s.power_w);
        sep(); out << "\"remaining_ah\":" << s.remaining_ah;
        sep(); out << "\"nominal_ah\":" << s.nominal_ah;
        sep(); out << "\"cell_min_v\":" << s.cell_min_v;
        sep(); out << "\"cell_max_v\":" << s.cell_max_v;
        sep(); out << "\"cell_delta_v\":" << s.cell_delta_v;
        sep(); out << "\"cycle_count\":" << s.cycle_count;
        sep(); out << "\"charge_mosfet\":" << (s.charge_mosfet ? "true" : "false");
        sep(); out << "\"discharge_mosfet\":" << (s.discharge_mosfet ? "true" : "false");
        if (!s.temperatures_c.empty()) {
            double tmin = s.temperatures_c[0], tmax = tmin, tsum = tmin;
            for (size_t i = 1; i < s.temperatures_c.size(); ++i) {
                double t = s.temperatures_c[i];
                if (t < tmin) tmin = t;
                if (t > tmax) tmax = t;
                tsum += t;
            }
            sep(); out << "\"temp_min_c\":" << tmin;
            sep(); out << "\"temp_max_c\":" << tmax;
            sep(); out << "\"temp_avg_c\":" << (tsum / s.temperatures_c.size());
        }
    }

    if (pg && *pg) {
        const PwrGateSnapshot& p = **pg;
        sep(); out << "\"charger\":" << escape_json_str(p.state);
        sep(); out << "\"grid_v\":" << p.ps_v;
        sep(); out << "\"solar_v\":" << p.sol_v;
        sep(); out << "\"bat_v\":" << p.bat_v;
        sep(); out << "\"bat_a\":" << p.bat_a;
        sep(); out << "\"target_v\":" << p.target_v;
        sep(); out << "\"target_a\":" << p.target_a;
    }

    if (first)
        return "{}";
    return "{" + out.str() + "}";
}
