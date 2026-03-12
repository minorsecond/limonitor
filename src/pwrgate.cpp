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
    std::string word = s.substr(p, e - p);
    
    // Valid states for EpicPowerGate 2
    static const char* VALID_STATES[] = {
        "Charging", "Float", "Bulk", "Absorption", "Idle", "Off"
    };
    for (const char* valid : VALID_STATES) {
        if (word == valid) return word;
    }

    // If the "first word" contains '=', it's likely a key-value pair, not the state.
    if (word.find('=') != std::string::npos) return "Idle";

    // If it's not a known state and doesn't look like a KV pair, maybe it is a state we don't know?
    // But if we saw "TargetV=..." it would have matched the find('=') above.
    return word;
}

static std::string extract_state(const std::string& line) {
    // Try to find one of the known states anywhere in the line.
    // They are unique enough words and usually appear at the start of the line or
    // before the first key-value pair.
    static const char* VALID_STATES[] = {
        "Charging", "Float", "Bulk", "Absorption", "Idle", "Off"
    };

    for (const char* valid : VALID_STATES) {
        if (line.find(valid) != std::string::npos) return valid;
    }

    // Fallback to first_word logic if no known state found
    return first_word(line);
}

bool parse(const std::string& s1, const std::string& s2, PwrGateSnapshot& snap) {
    if (s1.find("PS=")      == std::string::npos) return false;
    if (s2.find("TargetV=") == std::string::npos) return false;

    snap.state   = extract_state(s1);
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

    snap.target_v = extract_d(s2, "TargetV");
    snap.target_a = extract_d(s2, "TargetI");
    snap.stop_a   = extract_d(s2, "Stop");
    snap.temp     = extract_i(s2, "Temp");
    snap.pss      = extract_i(s2, "PSS");

    snap.timestamp = std::chrono::system_clock::now();
    snap.valid = true;
    return true;
}

} // namespace pwrgate
