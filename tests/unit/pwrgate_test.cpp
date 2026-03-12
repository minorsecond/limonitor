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
    ASSERT(snap.state == "Charging", "Known state word 'Charging' preserved");
}

void test_pwrgate_parse_float_state_preserved() {
    PwrGateSnapshot snap;
    std::string s1 = "Float PS=14.01 Sol=0.00 Bat=14.55V, 0.15A Min=180 P=50 adc=512";
    std::string s2 = "TargetV=14.59 TargetI=10.0 Stop=0.15 Temp=72 PSS=0";
    ASSERT(pwrgate::parse(s1, s2, snap), "Parse succeeds");
    ASSERT(snap.state == "Float", "Known state word 'Float' preserved");
}

void test_pwrgate_parse_state_inferred_charging() {
    // New firmware: status line starts with "TargetV=..." instead of a state word
    PwrGateSnapshot snap;
    std::string s1 = "TargetV=14.59V PS=14.01 Sol=0.00 Bat=13.25V, 5.36A Min=20 P=800 adc=600";
    std::string s2 = "TargetV=14.59 TargetI=10.0 Stop=0.15 Temp=74 PSS=0";
    ASSERT(pwrgate::parse(s1, s2, snap), "Parse succeeds with new firmware format");
    ASSERT(snap.state == "Charging", "State inferred as Charging: bat_a>0, bat_v well below target");
    ASSERT(std::abs(snap.bat_a - 5.36) < 0.01, "bat_a parsed correctly");
    ASSERT(std::abs(snap.bat_v - 13.25) < 0.01, "bat_v parsed correctly");
}

void test_pwrgate_parse_state_inferred_float() {
    PwrGateSnapshot snap;
    // bat_v near target_v (within 0.10V), low current -> Float
    std::string s1 = "TargetV=14.59V PS=14.01 Sol=0.00 Bat=14.55V, 0.20A Min=120 P=50 adc=512";
    std::string s2 = "TargetV=14.59 TargetI=10.0 Stop=0.15 Temp=74 PSS=0";
    ASSERT(pwrgate::parse(s1, s2, snap), "Parse succeeds");
    ASSERT(snap.state == "Float", "State inferred as Float: bat_v near target");
}

void test_pwrgate_parse_state_inferred_idle() {
    PwrGateSnapshot snap;
    // bat_a ~0, PWM=0 -> Idle
    std::string s1 = "TargetV=14.59V PS=14.01 Sol=0.00 Bat=13.39V, 0.00A Min=50 P=0 adc=512";
    std::string s2 = "TargetV=14.59 TargetI=10.0 Stop=0.15 Temp=74 PSS=0";
    ASSERT(pwrgate::parse(s1, s2, snap), "Parse succeeds");
    ASSERT(snap.state == "Idle", "State inferred as Idle: bat_a~0");
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
    ASSERT(std::abs(snap.ps_v     - 14.01) < 0.01, "ps_v correct");
    ASSERT(std::abs(snap.sol_v    -  1.23) < 0.01, "sol_v correct");
    ASSERT(std::abs(snap.bat_v    - 13.39) < 0.01, "bat_v correct");
    ASSERT(std::abs(snap.bat_a    -  5.00) < 0.01, "bat_a correct");
    ASSERT(snap.minutes == 45,                      "minutes correct");
    ASSERT(snap.pwm     == 750,                     "pwm correct");
    ASSERT(std::abs(snap.target_v - 14.60) < 0.01, "target_v correct");
    ASSERT(std::abs(snap.target_a -  9.50) < 0.01, "target_a correct");
    ASSERT(std::abs(snap.stop_a   -  0.20) < 0.01, "stop_a correct");
    ASSERT(snap.temp == 68,                         "temp correct");
    ASSERT(snap.pss  ==  1,                         "pss correct");
    ASSERT(snap.valid,                              "valid set");
}

// --- Analytics charging stage (driven by inferred state) ---

void test_charging_stage_bulk_from_inferred_state() {
    DataStore store;
    store.init_extensions(Config{});

    PwrGateSnapshot pg;
    pg.bat_v    = 13.25;   // well below target (14.59 - 13.25 = 1.34V gap)
    pg.bat_a    = 5.36;    // active charging current
    pg.target_v = 14.59;
    pg.state    = "Charging";
    pg.valid    = true;
    store.update_pwrgate(pg);

    ASSERT(store.analytics().charging_stage == "Bulk",
           "Bulk stage: bat_v << target_v with active current");
}

void test_charging_stage_absorption_from_voltage() {
    DataStore store;
    store.init_extensions(Config{});

    PwrGateSnapshot pg;
    pg.bat_v    = 14.48;   // near target (< 0.15V gap)
    pg.bat_a    = 2.1;     // current still flowing
    pg.target_v = 14.59;
    pg.state    = "Charging";
    pg.valid    = true;
    store.update_pwrgate(pg);

    ASSERT(store.analytics().charging_stage == "Absorption",
           "Absorption stage: bat_v near target");
}

void test_charging_stage_float_from_voltage() {
    DataStore store;
    store.init_extensions(Config{});

    PwrGateSnapshot pg;
    pg.bat_v    = 14.55;   // within 0.05V of target
    pg.bat_a    = 0.20;    // low tail current
    pg.target_v = 14.59;
    pg.state    = "Charging";
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
    pg.bat_v    = 13.50;
    pg.bat_a    = 3.00;
    pg.target_v = 14.59;
    pg.state    = "Charging";   // This is now what the inferred state provides
    pg.valid    = true;
    store.update_pwrgate(pg);

    ASSERT(store.analytics().charging_stage != "Idle",
           "Charging stage is not Idle when bat_a > 0");
}
