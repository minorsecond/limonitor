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

static std::string first_word(const std::string& s) {
    size_t p = 0;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    size_t e = p;
    while (e < s.size() && s[e] != ' ' && s[e] != '\t') ++e;
    return s.substr(p, e - p);
}

bool parse(const std::string& s1, const std::string& s2, PwrGateSnapshot& snap) {
    if (s1.find("PS=")      == std::string::npos) return false;
    // s2 must have TargetV=, unless s1 already has it (single-line mode)
    if (s2.find("TargetV=") == std::string::npos &&
        s1.find("TargetV=") == std::string::npos) return false;

    // Parse all numeric fields before inferring state
    snap.ps_v    = extract_d(s1, "PS");
    snap.sol_v   = extract_d(s1, "Sol");
    snap.minutes = extract_i(s1, "Min");
    snap.pwm     = extract_i(s1, "P");
    snap.adc     = extract_i(s1, "adc");

    // "Bat=13.08V,  5.36A" — voltage then current after the comma
    size_t bp = s1.find("Bat=");
    if (bp != std::string::npos) {
        std::sscanf(s1.c_str() + bp + 4, "%lf", &snap.bat_v);
        size_t cp = s1.find(',', bp);
        if (cp != std::string::npos)
            std::sscanf(s1.c_str() + cp + 1, " %lf", &snap.bat_a);
    }

    // Field source: if s1 has TargetV, it's a single-line update.
    // Otherwise use s2 for target fields.
    const std::string& t_src = (s1.find("TargetV=") != std::string::npos) ? s1 : s2;

    snap.target_v = extract_d(t_src, "TargetV");
    snap.target_a = extract_d(t_src, "TargetI");
    snap.stop_a   = extract_d(t_src, "Stop");
    snap.temp     = extract_i(t_src, "Temp");
    snap.pss      = extract_i(t_src, "PSS");

    // State: use first word if it looks like a known keyword (no '='), otherwise
    // infer from measurements. Older firmware prefixed lines with "Charging"/"Float"/
    // "Idle"; newer firmware omits the keyword and starts with a field like "TargetV=".
    std::string fw = first_word(s1);
    if (fw.find('=') == std::string::npos && !fw.empty()) {
        snap.state = fw;
    } else {
        // Infer: Float when bat_v is within 0.10 V of target; Charging when current
        // is flowing; Idle otherwise.
        if (snap.bat_a > 0.05) {
            snap.state = (snap.bat_v >= snap.target_v - 0.10) ? "Float" : "Charging";
        } else {
            snap.state = "Idle";
        }
    }

    snap.timestamp = std::chrono::system_clock::now();
    snap.valid = true;
    return true;
}

} // namespace pwrgate
