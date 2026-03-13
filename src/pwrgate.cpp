#include "pwrgate.hpp"
#include <cstdio>

namespace pwrgate {

static double extract_d(const std::string& line, const std::string& key) {
    std::string pat = key + "=";
    size_t p = line.find(pat);
    if (p == std::string::npos) return 0.0;
    p += pat.size();
    while (p < line.size() && line[p] == ' ') ++p;
    double v = 0.0;
    std::sscanf(line.c_str() + p, "%lf", &v);
    return v;
}

static int extract_i(const std::string& line, const std::string& key) {
    return static_cast<int>(extract_d(line, key));
}

// Extract the leading state token from a telemetry line.
// The firmware emits multi-word states ("Chrg Off", "PS Off") so we must check
// for two-word prefixes before falling back to single-word matching.
static std::string extract_state(const std::string& s) {
    size_t p = 0;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    // Two-word states must be checked first
    if (s.compare(p, 8, "Chrg Off") == 0) return "Chrg Off";
    if (s.compare(p, 6, "PS Off")   == 0) return "PS Off";
    // Single-word states
    size_t e = p;
    while (e < s.size() && s[e] != ' ' && s[e] != '\t') ++e;
    return s.substr(p, e - p);
}

bool parse(const std::string& s1, const std::string& s2, PwrGateSnapshot& snap) {
    if (s1.find("PS=") == std::string::npos) return false;

    // Parse all numeric fields
    snap.ps_v    = static_cast<float>(extract_d(s1, "PS"));
    snap.sol_v   = static_cast<float>(extract_d(s1, "Sol"));
    snap.minutes = extract_i(s1, "Min");
    snap.pwm     = extract_i(s1, "P");
    snap.adc     = extract_i(s1, "adc");

    // "Bat=13.08V,  5.36A" — voltage then current after the comma
    size_t bp = s1.find("Bat=");
    if (bp != std::string::npos) {
        double bv = 0, ba = 0;
        std::sscanf(s1.c_str() + bp + 4, "%lf", &bv);
        snap.bat_v = static_cast<float>(bv);
        size_t cp = s1.find(',', bp);
        if (cp != std::string::npos) {
            std::sscanf(s1.c_str() + cp + 1, " %lf", &ba);
            snap.bat_a = static_cast<float>(ba);
        }
    }

    // Extended telemetry line ('g' command): may arrive as s2 or inline in s1
    const std::string& t_src = (s1.find("TargetV=") != std::string::npos) ? s1 : s2;
    snap.target_v = static_cast<float>(extract_d(t_src, "TargetV"));
    snap.target_a = static_cast<float>(extract_d(t_src, "TargetI"));
    snap.stop_a   = static_cast<float>(extract_d(t_src, "Stop"));
    snap.temp     = extract_i(t_src, "Temp");
    snap.pss      = extract_i(t_src, "PSS");

    // State: firmware prefixes every line with a state token.
    // Confirmed states (FW 1.34, reverse-engineered 2026-03-12/13):
    //   "Chrg Off"  PS present, charger idle
    //   "Charging"  CC-CV charge active
    //   "Charged"   Taper complete
    //   "PS Off"    PS input absent (ps_v == 0.00)
    // "Chrg Off" and "PS Off" are two-word tokens — first_word() alone is insufficient.
    // Fall back to measurement inference only when no state token is present.
    static auto s_chrg_off = std::make_shared<std::string>("Chrg Off");
    static auto s_charging = std::make_shared<std::string>("Charging");
    static auto s_charged  = std::make_shared<std::string>("Charged");
    static auto s_ps_off   = std::make_shared<std::string>("PS Off");

    std::string st = extract_state(s1);
    if (st.find('=') == std::string::npos && !st.empty()) {
        if      (st == "Chrg Off") snap.state = s_chrg_off;
        else if (st == "Charging") snap.state = s_charging;
        else if (st == "Charged")  snap.state = s_charged;
        else if (st == "PS Off")   snap.state = s_ps_off;
        else                       snap.state = std::make_shared<std::string>(st);
    } else {
        // Infer from measurements when no state token is present
        if (snap.bat_a > 0.05f)
            snap.state = s_charging;
        else if (snap.ps_v < 0.5f)
            snap.state = s_ps_off;
        else
            snap.state = s_chrg_off;
    }

    snap.timestamp = std::chrono::system_clock::now();
    snap.valid = true;
    return true;
}

} // namespace pwrgate
