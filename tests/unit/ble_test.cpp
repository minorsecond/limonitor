#include "../../src/ble_reconnect_logic.hpp"
#include "../test_helpers.hpp"

// ---------------------------------------------------------------------------
// ble_str_icontains
// ---------------------------------------------------------------------------

void test_ble_icontains_basic_match() {
    ASSERT(ble_str_icontains("Hello World", "world"),   "lowercase needle in mixed hay");
    ASSERT(ble_str_icontains("Hello World", "Hello"),   "exact prefix");
    ASSERT(ble_str_icontains("Hello World", "o Wo"),    "interior substring");
    ASSERT(ble_str_icontains("Hello World", "HELLO"),   "all-caps needle");
    ASSERT(ble_str_icontains("HELLO", "hello"),         "all-caps hay, lowercase needle");
}

void test_ble_icontains_no_match() {
    ASSERT(!ble_str_icontains("Hello", "Hello World"),  "needle longer than hay");
    ASSERT(!ble_str_icontains("abc", "xyz"),            "no overlap");
    ASSERT(!ble_str_icontains("", "x"),                 "empty hay");
    ASSERT(!ble_str_icontains("abc", "abcd"),           "needle one char longer");
}

void test_ble_icontains_edge_cases() {
    ASSERT(ble_str_icontains("abc", ""),   "empty needle always true");
    ASSERT(ble_str_icontains("", ""),      "both empty: empty needle → true");
    ASSERT(ble_str_icontains("a", "a"),    "single char exact");
    ASSERT(!ble_str_icontains("a", "b"),   "single char no match");
}

void test_ble_icontains_device_names() {
    // Full name contains the configured target
    ASSERT(ble_str_icontains("L-12100BNNA70-B06374", "L-12100BNNA70"),
           "full name contains prefix target");
    // Advertised name is shorter than stored target — THIS was the root bug
    ASSERT(!ble_str_icontains("L-12100BNNA70", "L-12100BNNA70-B06374"),
           "short ad name does NOT contain longer target (unidirectional)");
    ASSERT(ble_str_icontains("L-12100BNNA70-B06374", "B06374"),
           "suffix fragment matches");
}

// ---------------------------------------------------------------------------
// ble_is_address_match
// ---------------------------------------------------------------------------

void test_ble_address_match_exact() {
    ASSERT(ble_is_address_match("C8:47:80:19:C1:70", "C8:47:80:19:C1:70"), "exact match");
}

void test_ble_address_match_case_insensitive() {
    ASSERT(ble_is_address_match("C8:47:80:19:C1:70", "c8:47:80:19:c1:70"), "lower target");
    ASSERT(ble_is_address_match("c8:47:80:19:c1:70", "C8:47:80:19:C1:70"), "lower addr");
    ASSERT(ble_is_address_match("C8:47:80:19:C1:70", "C8:47:80:19:c1:70"), "mixed case");
}

void test_ble_address_no_match() {
    ASSERT(!ble_is_address_match("C8:47:80:19:C1:70", "C8:47:80:19:C1:71"), "last octet differs");
    ASSERT(!ble_is_address_match("C8:47:80:19:C1:70", "00:00:00:00:00:00"), "all zeros");
}

void test_ble_address_empty() {
    ASSERT(!ble_is_address_match("",                  "C8:47:80:19:C1:70"), "empty addr");
    ASSERT(!ble_is_address_match("C8:47:80:19:C1:70", ""),                  "empty target");
    ASSERT(!ble_is_address_match("",                  ""),                  "both empty");
}

// ---------------------------------------------------------------------------
// ble_is_name_match  (bidirectional — fixes the root-cause bug)
// ---------------------------------------------------------------------------

void test_ble_name_match_exact() {
    ASSERT(ble_is_name_match("L-12100BNNA70", "L-12100BNNA70"), "exact match");
    ASSERT(ble_is_name_match("JBD-BMS",        "JBD-BMS"),       "exact match 2");
}

void test_ble_name_match_target_is_longer() {
    // Stored target is "L-12100BNNA70-B06374" but BLE advertisement only broadcasts
    // "L-12100BNNA70".  The OLD code failed here; new bidirectional logic fixes it.
    ASSERT(ble_is_name_match("L-12100BNNA70", "L-12100BNNA70-B06374"),
           "ad name is prefix of stored target — must match (root-cause regression test)");
}

void test_ble_name_match_name_is_longer() {
    ASSERT(ble_is_name_match("L-12100BNNA70-B06374", "L-12100BNNA70"),
           "stored name longer than target — substring match");
}

void test_ble_name_match_case_insensitive() {
    ASSERT(ble_is_name_match("l-12100bnna70", "L-12100BNNA70"),      "all lower name");
    ASSERT(ble_is_name_match("L-12100BNNA70", "l-12100bnna70"),      "all lower target");
    ASSERT(ble_is_name_match("l-12100bnna70", "L-12100BNNA70-B06374"), "lower prefix vs full target");
}

void test_ble_name_no_match() {
    ASSERT(!ble_is_name_match("JBD-BMS",  "L-12100BNNA70"), "completely different names");
    ASSERT(!ble_is_name_match("ACME-BAT", "L-12100BNNA70"), "different vendor");
}

void test_ble_name_empty() {
    ASSERT(!ble_is_name_match("",              "L-12100BNNA70"), "empty name");
    ASSERT(!ble_is_name_match("L-12100BNNA70", ""),              "empty target");
    ASSERT(!ble_is_name_match("",              ""),              "both empty");
}

// ---------------------------------------------------------------------------
// BleReconnectState — constants
// ---------------------------------------------------------------------------

void test_ble_reconnect_constants() {
    ASSERT(BleReconnectState::SCAN_STUCK_TIMEOUT_S   == 90, "scan stuck timeout is 90s");
    ASSERT(BleReconnectState::CONNECT_FAIL_THRESHOLD ==  3, "connect fail threshold is 3");
    ASSERT(BleReconnectState::POWER_CYCLE_THRESHOLD  ==  4, "power cycle threshold is 4");
}

// ---------------------------------------------------------------------------
// BleReconnectState — connect failure counter
// ---------------------------------------------------------------------------

void test_ble_reconnect_connect_fail_below_threshold() {
    BleReconnectState s;
    ASSERT(!s.on_connect_failure(), "1st failure: no reset");
    ASSERT(s.connect_fail_count == 1, "count is 1 after first failure");
    ASSERT(!s.on_connect_failure(), "2nd failure: no reset");
    ASSERT(s.connect_fail_count == 2, "count is 2 after second failure");
}

void test_ble_reconnect_connect_fail_at_threshold() {
    BleReconnectState s;
    s.on_connect_failure();
    s.on_connect_failure();
    bool should_reset = s.on_connect_failure();
    ASSERT(should_reset, "3rd failure triggers hard reset");
    ASSERT(s.connect_fail_count == 3, "count at threshold before hard reset");
}

void test_ble_reconnect_connect_fail_above_threshold() {
    BleReconnectState s;
    // Simulate not calling hard_reset after threshold — count keeps incrementing
    for (int i = 0; i < 5; ++i) s.on_connect_failure();
    ASSERT(s.connect_fail_count == 5, "count accumulates beyond threshold");
}

void test_ble_reconnect_success_resets_fail_count() {
    BleReconnectState s;
    s.on_connect_failure();
    s.on_connect_failure();
    s.on_connect_success();
    ASSERT(s.connect_fail_count == 0, "success clears fail count");
    ASSERT(s.hard_reset_count   == 0, "success clears hard reset count");
    // After reset, threshold is fresh
    ASSERT(!s.on_connect_failure(), "after success, first failure again is below threshold");
}

// ---------------------------------------------------------------------------
// BleReconnectState — hard reset escalation
// ---------------------------------------------------------------------------

void test_ble_reconnect_hard_reset_increments() {
    BleReconnectState s;
    ASSERT(!s.begin_hard_reset(), "1st hard reset: no power cycle");
    ASSERT(s.hard_reset_count == 1, "count is 1 after 1st hard reset");
    ASSERT(s.connect_fail_count == 0, "begin_hard_reset clears connect_fail_count");
}

void test_ble_reconnect_hard_reset_below_power_cycle() {
    BleReconnectState s;
    ASSERT(!s.begin_hard_reset(), "reset 1: no power cycle");
    ASSERT(!s.begin_hard_reset(), "reset 2: no power cycle");
    ASSERT(!s.begin_hard_reset(), "reset 3: no power cycle");
}

void test_ble_reconnect_hard_reset_at_power_cycle_threshold() {
    BleReconnectState s;
    s.begin_hard_reset(); // 1
    s.begin_hard_reset(); // 2
    s.begin_hard_reset(); // 3
    bool needs_cycle = s.begin_hard_reset(); // 4
    ASSERT(needs_cycle,             "4th hard reset triggers power cycle");
    ASSERT(s.hard_reset_count == 4, "count is 4 at threshold");
}

void test_ble_reconnect_power_cycle_resets_count() {
    BleReconnectState s;
    s.begin_hard_reset(); s.begin_hard_reset(); s.begin_hard_reset(); s.begin_hard_reset();
    s.on_power_cycled();
    ASSERT(s.hard_reset_count == 0, "power cycle resets hard reset count");
    // Threshold restarts from zero
    ASSERT(!s.begin_hard_reset(), "after power cycle, 1st hard reset is below threshold");
    ASSERT(s.hard_reset_count == 1, "count is 1 after first reset post-cycle");
}

void test_ble_reconnect_begin_hard_reset_clears_connect_fails() {
    BleReconnectState s;
    s.on_connect_failure();
    s.on_connect_failure();
    s.on_connect_failure(); // now at threshold
    s.begin_hard_reset();
    ASSERT(s.connect_fail_count == 0, "begin_hard_reset clears connect fail count");
}

void test_ble_reconnect_full_lifecycle() {
    BleReconnectState s;
    // 3 connect failures → hard reset
    s.on_connect_failure(); s.on_connect_failure();
    bool should_reset = s.on_connect_failure();
    ASSERT(should_reset, "threshold reached");
    // Hard reset (1st — no power cycle)
    bool cycle = s.begin_hard_reset();
    ASSERT(!cycle, "1st hard reset: no power cycle");
    // Reconnect succeeds
    s.on_connect_success();
    ASSERT(s.connect_fail_count == 0 && s.hard_reset_count == 0,
           "success clears all state");
    // Start fresh — 3 more failures
    s.on_connect_failure(); s.on_connect_failure();
    ASSERT(s.on_connect_failure(), "fresh threshold reached after success");
}
