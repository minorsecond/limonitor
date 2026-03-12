#include "../src/pwrgate.hpp"
#include "../src/data_store.hpp"
#include "../src/config.hpp"
#include "../tests/test_helpers.hpp"
#include <cstring>

// --- pwrgate::parse state inference ---

void test_pwrgate_parse_known_state_preserved() {
    PwrGateSnapshot snap;
    std::string s1 = "Charging PS=14.01 Sol=0.00 Bat=13.25V, 5.36A Min=20 P=800 adc=600";
    std::string s2 = "TargetV=14.59 TargetI=10.0 Stop=0.15 Temp=74 PSS=0";
    ASSERT(pwrgate::parse(s1, s2, snap), "Parse succeeds");
    ASSERT(snap.state && *snap.state == "Charging", "Known state word 'Charging' preserved");
}

void test_pwrgate_parse_float_state_preserved() {
    PwrGateSnapshot snap;
    std::string s1 = "Float PS=14.01 Sol=0.00 Bat=14.55V, 0.15A Min=180 P=50 adc=512";
    std::string s2 = "TargetV=14.59 TargetI=10.0 Stop=0.15 Temp=72 PSS=0";
    ASSERT(pwrgate::parse(s1, s2, snap), "Parse succeeds");
    ASSERT(snap.state && *snap.state == "Float", "Known state word 'Float' preserved");
}

void test_pwrgate_parse_state_inferred_charging() {
    // New firmware: status line starts with "TargetV=..." instead of a state word
    PwrGateSnapshot snap;
    std::string s1 = "TargetV=14.59V PS=14.01 Sol=0.00 Bat=13.25V, 5.36A Min=20 P=800 adc=600";
    std::string s2 = "TargetV=14.59 TargetI=10.0 Stop=0.15 Temp=74 PSS=0";
    ASSERT(pwrgate::parse(s1, s2, snap), "Parse succeeds with new firmware format");
    ASSERT(snap.state && *snap.state == "Charging", "State inferred as Charging: bat_a>0, bat_v well below target");
    ASSERT(std::abs(snap.bat_a - 5.36f) < 0.01f, "bat_a parsed correctly");
    ASSERT(std::abs(snap.bat_v - 13.25f) < 0.01f, "bat_v parsed correctly");
}

void test_pwrgate_parse_state_inferred_float() {
    PwrGateSnapshot snap;
    // bat_v near target_v (within 0.10V), low current -> Float
    std::string s1 = "TargetV=14.59V PS=14.01 Sol=0.00 Bat=14.55V, 0.20A Min=120 P=50 adc=512";
    std::string s2 = "TargetV=14.59 TargetI=10.0 Stop=0.15 Temp=74 PSS=0";
    ASSERT(pwrgate::parse(s1, s2, snap), "Parse succeeds");
    ASSERT(snap.state && *snap.state == "Float", "State inferred as Float: bat_v near target");
}

void test_pwrgate_parse_state_inferred_idle() {
    PwrGateSnapshot snap;
    // bat_a ~0, PWM=0 -> Idle
    std::string s1 = "TargetV=14.59V PS=14.01 Sol=0.00 Bat=13.39V, 0.00A Min=50 P=0 adc=512";
    std::string s2 = "TargetV=14.59 TargetI=10.0 Stop=0.15 Temp=74 PSS=0";
    ASSERT(pwrgate::parse(s1, s2, snap), "Parse succeeds");
    ASSERT(snap.state && *snap.state == "Idle", "State inferred as Idle: bat_a~0");
}

void test_pwrgate_parse_single_line() {
    PwrGateSnapshot snap;
    // Both PS= and TargetV= in the same line
    std::string s1 = "Charging PS=14.01 Sol=0.00 Bat=13.25V, 5.36A Min=20 P=800 adc=600 TargetV=14.59 TargetI=10.0 Stop=0.15 Temp=74 PSS=0";
    ASSERT(pwrgate::parse(s1, s1, snap), "Parse succeeds with single line");
    ASSERT(snap.state && *snap.state == "Charging", "State 'Charging' parsed");
    ASSERT(std::abs(snap.ps_v - 14.01f) < 0.01f, "ps_v correct");
    ASSERT(std::abs(snap.target_v - 14.59f) < 0.01f, "target_v correct");
}

void test_pwrgate_parse_lenient() {
    PwrGateSnapshot snap;
    // PS= present, but no TargetV=
    std::string s1 = "Charging PS=15.11V Bat=13.78V, 9.96A Sol=0.24V Min=16";
    ASSERT(pwrgate::parse(s1, "", snap), "Parse succeeds without TargetV");
    ASSERT(snap.state && *snap.state == "Charging", "State 'Charging' parsed");
    ASSERT(std::abs(snap.ps_v - 15.11f) < 0.01f, "ps_v correct");
    ASSERT(std::abs(snap.bat_v - 13.78f) < 0.01f, "bat_v correct");
    ASSERT(std::abs(snap.bat_a - 9.96f) < 0.01f, "bat_a correct");
    ASSERT(std::abs(snap.target_v) < 0.01f, "target_v defaults to 0");
}

void test_pwrgate_parse_rejects_missing_ps() {
    PwrGateSnapshot snap;
    std::string s1 = "Charging Sol=0.00 Bat=13.25V, 5.36A Min=20 P=800 adc=600"; // no PS=
    std::string s2 = "TargetV=14.59 TargetI=10.0 Stop=0.15 Temp=74 PSS=0";
    ASSERT(!pwrgate::parse(s1, s2, snap), "Parse rejects line without PS=");
}

void test_pwrgate_parse_values_correct() {
    PwrGateSnapshot snap;
    std::string s1 = "Charging PS=14.01 Sol=1.23 Bat=13.39V, 5.00A Min=45 P=750 adc=600";
    std::string s2 = "TargetV=14.60 TargetI=9.50 Stop=0.20 Temp=68 PSS=1";
    ASSERT(pwrgate::parse(s1, s2, snap), "Parse succeeds");
    ASSERT(std::abs(snap.ps_v     - 14.01f) < 0.01f, "ps_v correct");
    ASSERT(std::abs(snap.sol_v    -  1.23f) < 0.01f, "sol_v correct");
    ASSERT(std::abs(snap.bat_v    - 13.39f) < 0.01f, "bat_v correct");
    ASSERT(std::abs(snap.bat_a    -  5.00f) < 0.01f, "bat_a correct");
    ASSERT(snap.minutes == 45,                      "minutes correct");
    ASSERT(snap.pwm     == 750,                     "pwm correct");
    ASSERT(std::abs(snap.target_v - 14.60f) < 0.01f, "target_v correct");
    ASSERT(std::abs(snap.target_a -  9.50f) < 0.01f, "target_a correct");
    ASSERT(std::abs(snap.stop_a   -  0.20f) < 0.01f, "stop_a correct");
    ASSERT(snap.temp == 68,                         "temp correct");
    ASSERT(snap.pss  ==  1,                         "pss correct");
    ASSERT(snap.valid,                              "valid set");
}

// --- Analytics charging stage (driven by inferred state) ---

void test_charging_stage_bulk_from_inferred_state() {
    DataStore store;
    store.init_extensions(Config{});

    PwrGateSnapshot pg;
    pg.bat_v    = 13.25f;   // well below target (14.59 - 13.25 = 1.34V gap)
    pg.bat_a    = 5.36f;    // active charging current
    pg.target_v = 14.59f;
    pg.state    = std::make_shared<std::string>("Charging");
    pg.valid    = true;
    store.update_pwrgate(pg);

    ASSERT(store.analytics().charging_stage == "Bulk",
           "Bulk stage: bat_v << target_v with active current");
}

void test_charging_stage_absorption_from_voltage() {
    DataStore store;
    store.init_extensions(Config{});

    PwrGateSnapshot pg;
    pg.bat_v    = 14.48f;   // near target (< 0.15V gap)
    pg.bat_a    = 2.1f;     // current still flowing
    pg.target_v = 14.59f;
    pg.state    = std::make_shared<std::string>("Charging");
    pg.valid    = true;
    store.update_pwrgate(pg);

    ASSERT(store.analytics().charging_stage == "Absorption",
           "Absorption stage: bat_v near target");
}

void test_charging_stage_float_from_voltage() {
    DataStore store;
    store.init_extensions(Config{});

    PwrGateSnapshot pg;
    pg.bat_v    = 14.55f;   // within 0.05V of target
    pg.bat_a    = 0.20f;    // low tail current
    pg.target_v = 14.59f;
    pg.state    = std::make_shared<std::string>("Charging");
    pg.valid    = true;
    store.update_pwrgate(pg);

    ASSERT(store.analytics().charging_stage == "Float",
           "Float stage: bat_v within 0.05V of target with low current");
}

void test_charging_stage_not_idle_when_current_flows() {
    // Even if state string is unrecognised, current > 0 must not yield "Idle"
    DataStore store;
    store.init_extensions(Config{});

    PwrGateSnapshot pg;
    pg.bat_v    = 13.50f;
    pg.bat_a    = 3.00f;
    pg.target_v = 14.59f;
    pg.state    = std::make_shared<std::string>("Charging");   // This is now what the inferred state provides
    pg.valid    = true;
    store.update_pwrgate(pg);

    ASSERT(store.analytics().charging_stage != "Idle",
           "Charging stage is not Idle when bat_a > 0");
}
