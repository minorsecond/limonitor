#include "../src/system_load.hpp"
#include "../src/database.hpp"
#include "../tests/test_helpers.hpp"
#include <cmath>
#include <string>

// ─── TxLevel ────────────────────────────────────────────────────────────────

void test_tx_level_efficiency_basic() {
    TxLevel tx;
    tx.rf_out_w = 10.0;
    tx.dc_in_w  = 62.6;  // radio-only DC watts (not total system)
    double eff = tx.efficiency_pct();
    // eff = 10/62.6*100 = 15.97%
    ASSERT(approx_eq(eff, 15.97, 0.1), "10W RF efficiency ~16%");
}

void test_tx_level_efficiency_zero_base() {
    TxLevel tx;
    tx.rf_out_w = 5.0;
    tx.dc_in_w  = 50.2;  // radio-only; eff = 5/50.2*100 = 9.96%
    double eff = tx.efficiency_pct();
    ASSERT(approx_eq(eff, 9.96, 0.1), "5W RF efficiency ~10%");
}

void test_tx_level_efficiency_zero_draw() {
    TxLevel tx;
    tx.rf_out_w = 5.0;
    tx.dc_in_w  = 0.0;
    double eff = tx.efficiency_pct();
    ASSERT(approx_eq(eff, 0.0), "Zero draw returns 0% efficiency");
}

void test_tx_level_efficiency_larger_base_than_draw() {
    // Tests near-zero dc_in_w returns 0 (renamed from "larger base than draw")
    TxLevel tx;
    tx.rf_out_w = 5.0;
    tx.dc_in_w  = 0.005;  // below 0.01 threshold
    double eff = tx.efficiency_pct();
    ASSERT(approx_eq(eff, 0.0), "Near-zero draw returns 0%");
}

// ─── SystemLoadConfig totals ─────────────────────────────────────────────────

void test_total_idle_w_empty() {
    SystemLoadConfig cfg;
    ASSERT(approx_eq(cfg.total_idle_w(), 0.0), "Empty config total_idle_w = 0");
}

void test_total_idle_w_single() {
    SystemLoadConfig cfg;
    SystemComponent c;
    c.idle_w = 5.45;
    cfg.components.push_back(c);
    ASSERT(approx_eq(cfg.total_idle_w(), 5.45), "Single component idle_w matches");
}

void test_total_idle_w_sum() {
    SystemLoadConfig cfg;
    for (double w : {0.55, 5.45, 3.80}) {
        SystemComponent c;
        c.idle_w = w;
        cfg.components.push_back(c);
    }
    ASSERT(approx_eq(cfg.total_idle_w(), 9.80), "Sum of idle_w = 9.80");
}

void test_total_idle_a_sum() {
    SystemLoadConfig cfg;
    for (double a : {0.042, 0.413, 0.290}) {
        SystemComponent c;
        c.idle_a = a;
        cfg.components.push_back(c);
    }
    ASSERT(approx_eq(cfg.total_idle_a(), 0.745, 0.001), "Sum of idle_a = 0.745");
}

// ─── effective_load_w ────────────────────────────────────────────────────────

void test_effective_load_no_tx() {
    SystemLoadConfig cfg = SystemLoadConfig::default_config();
    // duty 0%, no TX component → returns total_idle_w
    double load = cfg.effective_load_w(0.0, -1, 0);
    ASSERT(approx_eq(load, cfg.total_idle_w()), "0% TX returns total_idle_w");
}

void test_effective_load_100_pct_tx() {
    SystemLoadConfig cfg = SystemLoadConfig::default_config();
    // comp_idx=2 (radio), level_idx=1 (10W RF, dc_in_w=68.6)
    double load = cfg.effective_load_w(100.0, 2, 1);
    ASSERT(approx_eq(load, 68.6, 0.01), "100% TX = full TX draw");
}

void test_effective_load_10_pct_tx_10w() {
    SystemLoadConfig cfg = SystemLoadConfig::default_config();
    // total_idle = 9.80, 10W RF dc_in_w = 68.6
    // effective = 0.90*9.80 + 0.10*68.6 = 8.82 + 6.86 = 15.68
    double load = cfg.effective_load_w(10.0, 2, 1);
    ASSERT(approx_eq(load, 15.68, 0.01), "10% TX at 10W RF = 15.68W");
}

void test_effective_load_invalid_comp_idx() {
    SystemLoadConfig cfg = SystemLoadConfig::default_config();
    double load = cfg.effective_load_w(50.0, 99, 0);  // invalid index
    ASSERT(approx_eq(load, cfg.total_idle_w()), "Invalid comp_idx returns idle");
}

void test_effective_load_invalid_level_idx() {
    SystemLoadConfig cfg = SystemLoadConfig::default_config();
    double load = cfg.effective_load_w(50.0, 2, 99);  // invalid level
    ASSERT(approx_eq(load, cfg.total_idle_w()), "Invalid level_idx returns idle");
}

// ─── Runtime estimates ───────────────────────────────────────────────────────

void test_runtime_receive_h_basic() {
    SystemLoadConfig cfg = SystemLoadConfig::default_config();
    // 84 Ah * 13.1V nominal = 1100.4 Wh usable (approx)
    // Use exact values: total_idle_w = 9.80
    double usable_wh = 9.80 * 113.0;  // expect ~113h
    double rt = cfg.runtime_receive_h(usable_wh);
    ASSERT(approx_eq(rt, 113.0, 0.01), "Runtime receive ~113h matches input");
}

void test_runtime_receive_h_known_value() {
    SystemLoadConfig cfg = SystemLoadConfig::default_config();
    // 84 Ah * 13.1 V = 1100.4 Wh, total_idle=9.80 → 1100.4/9.80 = 112.3h
    double rt = cfg.runtime_receive_h(1100.4);
    ASSERT(approx_eq(rt, 112.3, 0.5), "Receive runtime for 84Ah pack ~112h");
}

void test_runtime_receive_h_zero_usable() {
    SystemLoadConfig cfg = SystemLoadConfig::default_config();
    ASSERT(approx_eq(cfg.runtime_receive_h(0.0), 0.0), "Zero usable → 0h runtime");
}

void test_runtime_with_tx_10pct() {
    SystemLoadConfig cfg = SystemLoadConfig::default_config();
    // effective_load = 15.68W, 1100.4 Wh → 70.2h
    double rt = cfg.runtime_with_tx_h(1100.4, 10.0, 2, 1);
    ASSERT(approx_eq(rt, 70.2, 0.5), "10% TX at 10W RF runtime ~70h");
}

void test_runtime_with_tx_zero_duty() {
    SystemLoadConfig cfg = SystemLoadConfig::default_config();
    double rt_rx  = cfg.runtime_receive_h(1100.4);
    double rt_0tx = cfg.runtime_with_tx_h(1100.4, 0.0, 2, 1);
    ASSERT(approx_eq(rt_rx, rt_0tx, 0.01), "0% TX duty = receive-only runtime");
}

// ─── JSON round-trip ─────────────────────────────────────────────────────────

void test_json_roundtrip_empty() {
    SystemLoadConfig cfg;
    std::string json = cfg.to_json();
    auto cfg2 = SystemLoadConfig::from_json(json);
    ASSERT(cfg2.empty(), "Empty config round-trips to empty");
}

void test_json_roundtrip_single_component() {
    SystemLoadConfig cfg;
    SystemComponent c;
    c.name    = "Test Unit";
    c.purpose = "Testing round-trip";
    c.idle_a  = 1.234;
    c.idle_w  = 12.34;
    cfg.components.push_back(c);

    auto cfg2 = SystemLoadConfig::from_json(cfg.to_json());
    ASSERT(cfg2.components.size() == 1, "Single component count preserved");
    ASSERT(cfg2.components[0].name == "Test Unit", "Name preserved");
    ASSERT(cfg2.components[0].purpose == "Testing round-trip", "Purpose preserved");
    ASSERT(approx_eq(cfg2.components[0].idle_a, 1.234, 1e-6), "idle_a preserved");
    ASSERT(approx_eq(cfg2.components[0].idle_w, 12.34, 1e-6), "idle_w preserved");
}

void test_json_roundtrip_with_tx_levels() {
    SystemLoadConfig cfg;
    SystemComponent c;
    c.name   = "Radio";
    c.idle_w = 3.8;
    TxLevel tx;
    tx.label    = "10W RF";
    tx.rf_out_w = 10.0;
    tx.dc_in_a  = 5.2;
    tx.dc_in_w  = 68.6;
    c.tx_levels.push_back(tx);
    cfg.components.push_back(c);

    auto cfg2 = SystemLoadConfig::from_json(cfg.to_json());
    ASSERT(cfg2.components.size() == 1, "Component count preserved with TX");
    ASSERT(cfg2.components[0].tx_levels.size() == 1, "TX level count preserved");
    ASSERT(cfg2.components[0].tx_levels[0].label == "10W RF", "TX label preserved");
    ASSERT(approx_eq(cfg2.components[0].tx_levels[0].rf_out_w, 10.0, 1e-6), "rf_out preserved");
    ASSERT(approx_eq(cfg2.components[0].tx_levels[0].dc_in_a, 5.2, 1e-6), "dc_in_a preserved");
    ASSERT(approx_eq(cfg2.components[0].tx_levels[0].dc_in_w, 68.6, 1e-6), "dc_in_w preserved");
}

void test_json_roundtrip_special_chars_in_name() {
    SystemLoadConfig cfg;
    SystemComponent c;
    c.name    = "Test \"quoted\" & more";
    c.purpose = "Line1\nLine2";
    c.idle_w  = 1.0;
    cfg.components.push_back(c);

    auto cfg2 = SystemLoadConfig::from_json(cfg.to_json());
    ASSERT(cfg2.components.size() == 1, "Component with special chars preserved count");
    ASSERT(cfg2.components[0].name == "Test \"quoted\" & more", "Special chars in name round-trip");
    ASSERT(cfg2.components[0].purpose == "Line1\nLine2", "Newline in purpose round-trip");
}

void test_json_from_json_empty_string() {
    auto cfg = SystemLoadConfig::from_json("");
    ASSERT(cfg.empty(), "from_json empty string returns empty config");
}

void test_json_from_json_malformed() {
    auto cfg = SystemLoadConfig::from_json("{\"not_components\":42}");
    ASSERT(cfg.empty(), "from_json with wrong key returns empty config");
}

// ─── Default config values ───────────────────────────────────────────────────

void test_default_config_component_count() {
    auto cfg = SystemLoadConfig::default_config();
    ASSERT(cfg.components.size() == 3, "Default config has 3 components");
}

void test_default_config_total_idle_w() {
    auto cfg = SystemLoadConfig::default_config();
    // 0.55 + 5.45 + 3.80 = 9.80
    ASSERT(approx_eq(cfg.total_idle_w(), 9.80, 0.01), "Default total_idle_w = 9.80");
}

void test_default_config_total_idle_a() {
    auto cfg = SystemLoadConfig::default_config();
    // 0.042 + 0.413 + 0.290 = 0.745
    ASSERT(approx_eq(cfg.total_idle_a(), 0.745, 0.001), "Default total_idle_a = 0.745");
}

void test_default_config_radio_tx_levels() {
    auto cfg = SystemLoadConfig::default_config();
    // Radio is the last component (idx 2)
    ASSERT(cfg.components[2].tx_levels.size() == 3, "Radio has 3 TX levels");
    ASSERT(approx_eq(cfg.components[2].tx_levels[1].rf_out_w, 10.0, 0.01), "10W RF output");
    // dc_in_w is radio-only: 68.6W total - 6.0W non-radio idle = 62.6W
    ASSERT(approx_eq(cfg.components[2].tx_levels[1].dc_in_w, 62.6, 0.01), "10W RF dc_in_w (radio-only)");
}

void test_default_config_not_empty() {
    auto cfg = SystemLoadConfig::default_config();
    ASSERT(!cfg.empty(), "Default config is not empty");
}

void test_default_config_full_roundtrip() {
    auto cfg = SystemLoadConfig::default_config();
    auto cfg2 = SystemLoadConfig::from_json(cfg.to_json());
    ASSERT(cfg2.components.size() == cfg.components.size(), "Default round-trip preserves count");
    ASSERT(approx_eq(cfg2.total_idle_w(), cfg.total_idle_w(), 1e-6), "Default round-trip preserves total_idle_w");
    ASSERT(cfg2.components[2].tx_levels.size() == cfg.components[2].tx_levels.size(),
           "Default round-trip preserves TX level count");
}

// ─── idle_w_override / effective_idle_w ──────────────────────────────────────

void test_effective_idle_w_default_is_minus1() {
    SystemLoadConfig cfg;
    ASSERT(approx_eq(cfg.idle_w_override, -1.0), "Default idle_w_override is -1");
}

void test_effective_idle_w_no_override_returns_total() {
    SystemLoadConfig cfg = SystemLoadConfig::default_config();
    // No override set → effective_idle_w() == total_idle_w()
    ASSERT(approx_eq(cfg.effective_idle_w(), cfg.total_idle_w()),
           "effective_idle_w with no override equals total_idle_w");
}

void test_effective_idle_w_override_used() {
    SystemLoadConfig cfg = SystemLoadConfig::default_config();
    cfg.idle_w_override = 15.0;
    ASSERT(approx_eq(cfg.effective_idle_w(), 15.0),
           "effective_idle_w returns override value when set");
}

void test_effective_idle_w_override_zero_allowed() {
    SystemLoadConfig cfg = SystemLoadConfig::default_config();
    cfg.idle_w_override = 0.0;
    ASSERT(approx_eq(cfg.effective_idle_w(), 0.0),
           "effective_idle_w returns 0 when override is 0");
}

void test_runtime_receive_h_uses_override() {
    SystemLoadConfig cfg = SystemLoadConfig::default_config();
    // Default total_idle_w = 9.80; override to 5.0 → runtime doubles
    double rt_default = cfg.runtime_receive_h(980.0); // 980 / 9.80 = 100h
    cfg.idle_w_override = 5.0;
    double rt_override = cfg.runtime_receive_h(980.0); // 980 / 5.0 = 196h
    ASSERT(approx_eq(rt_default, 100.0, 0.01), "Baseline receive runtime = 100h");
    ASSERT(approx_eq(rt_override, 196.0, 0.01), "Override halves load → runtime doubles");
}

void test_effective_load_w_uses_override() {
    SystemLoadConfig cfg = SystemLoadConfig::default_config();
    // Idle override changes the idle portion of the TX duty calculation.
    // comp_idx=2 (radio), level_idx=0 (5W RF, dc_in_w=44.2)
    // total_idle = 9.80; radio idle = 3.80; non-radio = 6.00
    // With 0% TX: effective_load = effective_idle_w = 20.0 (override)
    cfg.idle_w_override = 20.0;
    double load_0tx = cfg.effective_load_w(0.0, 2, 0);
    ASSERT(approx_eq(load_0tx, 20.0, 0.01),
           "0% TX with override returns override value");

    // With 100% TX: total_tx_w = override - comp.idle_w + tx.dc_in_w
    //             = 20.0 - 3.80 + 44.2 = 60.4
    double load_100tx = cfg.effective_load_w(100.0, 2, 0);
    ASSERT(approx_eq(load_100tx, 60.4, 0.01),
           "100% TX with override adjusts formula correctly");
}

void test_json_roundtrip_idle_w_override_negative() {
    SystemLoadConfig cfg = SystemLoadConfig::default_config();
    // Default -1 should survive round-trip
    auto cfg2 = SystemLoadConfig::from_json(cfg.to_json());
    ASSERT(approx_eq(cfg2.idle_w_override, -1.0),
           "Default idle_w_override=-1 preserved in JSON round-trip");
}

void test_json_roundtrip_idle_w_override_positive() {
    SystemLoadConfig cfg = SystemLoadConfig::default_config();
    cfg.idle_w_override = 12.5;
    auto cfg2 = SystemLoadConfig::from_json(cfg.to_json());
    ASSERT(approx_eq(cfg2.idle_w_override, 12.5, 1e-6),
           "Positive idle_w_override preserved in JSON round-trip");
}

void test_json_from_old_format_defaults_override_to_minus1() {
    // Old JSON without idle_w_override field should default to -1
    std::string old_json = R"({"components":[{"name":"Pi","purpose":"ctrl","idle_a":0.4,"idle_w":5.0,"tx_levels":[]}]})";
    auto cfg = SystemLoadConfig::from_json(old_json);
    ASSERT(cfg.components.size() == 1, "Old format parsed correctly");
    ASSERT(approx_eq(cfg.idle_w_override, -1.0),
           "Old JSON without idle_w_override defaults to -1 (backwards compat)");
}

// ─── DB storage / retrieval ──────────────────────────────────────────────────

void test_db_system_load_default_when_empty() {
    std::string db_path = "/tmp/sysload_test_" + std::to_string(getpid()) + ".db";
    remove(db_path.c_str());
    Database db(db_path);
    ASSERT(db.open(), "DB opens for system load test");

    auto cfg = db.get_system_load_config();
    // Should return default config when nothing stored
    ASSERT(!cfg.empty(), "Default config returned when DB has no config");
    ASSERT(cfg.components.size() == 3, "Default from DB has 3 components");

    remove(db_path.c_str());
}

void test_db_system_load_store_and_retrieve() {
    std::string db_path = "/tmp/sysload_db_" + std::to_string(getpid()) + ".db";
    remove(db_path.c_str());
    Database db(db_path);
    db.open();

    SystemLoadConfig cfg;
    SystemComponent c;
    c.name   = "Widget";
    c.purpose = "Test widget";
    c.idle_a = 0.123;
    c.idle_w = 1.23;
    cfg.components.push_back(c);

    db.set_system_load_config(cfg);

    auto cfg2 = db.get_system_load_config();
    ASSERT(cfg2.components.size() == 1, "Stored and retrieved component count matches");
    ASSERT(cfg2.components[0].name == "Widget", "Component name matches after DB round-trip");
    ASSERT(approx_eq(cfg2.components[0].idle_w, 1.23, 1e-6), "idle_w matches after DB round-trip");

    remove(db_path.c_str());
}

void test_db_system_load_overwrite() {
    std::string db_path = "/tmp/sysload_ow_" + std::to_string(getpid()) + ".db";
    remove(db_path.c_str());
    Database db(db_path);
    db.open();

    auto def = SystemLoadConfig::default_config();
    db.set_system_load_config(def);

    // Overwrite with a smaller config
    SystemLoadConfig small;
    SystemComponent c;
    c.name   = "Solo";
    c.idle_w = 5.0;
    small.components.push_back(c);
    db.set_system_load_config(small);

    auto got = db.get_system_load_config();
    ASSERT(got.components.size() == 1, "Overwritten config has 1 component");
    ASSERT(got.components[0].name == "Solo", "Overwritten component name is correct");

    remove(db_path.c_str());
}
