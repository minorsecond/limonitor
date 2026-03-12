#include <cstdio>
#include <string>
#include <thread>
#include <chrono>

// Required by BLE manager
int g_poll_interval_s = 5;

// Forward declarations of test functions
void test_database();
void test_database_event_logging();
void test_database_concurrency();
void test_database_test_runs();
void test_database_test_telemetry();

void test_http_options_cors();
void test_http_post_settings();
void test_http_solar_apis_with_extensions();
void test_solar_page_fetch_contract();
void test_solar_api_response_structure();
void test_http_solar_page_theme();
void test_http_tests_api();
void test_http_tests_start_stop_with_battery();
void test_http_tests_telemetry_and_notes();
void test_charger_history_api_empty();
void test_charger_history_api_with_data();
void test_charger_history_persistence_via_store();

void test_weather_forecast_empty_api_key();
void test_parse_daily_entries_cloud_cover();
void test_parse_daily_entries_sunrise_sunset();
void test_parse_daily_entries_nested_clouds();
void test_forecast_week_extended_cloud_values();
void test_parse_daily_entries_empty();
void test_soc_basic_solar_charge();
void test_soc_basic_drain();
void test_soc_clamped_at_100();
void test_soc_clamped_at_0();
void test_soc_net_zero();
void test_soc_day_night_cycle();
void test_soc_empty_inputs();
void test_soc_zero_capacity();
void test_soc_negative_capacity();
void test_soc_start_above_100();
void test_soc_start_below_0();
void test_soc_mismatched_lengths();
void test_soc_realistic_scenario();
void test_runtime_basic();
void test_runtime_full_to_cutoff();
void test_runtime_zero_load();
void test_runtime_soc_below_cutoff();
void test_runtime_soc_at_cutoff();
void test_runtime_zero_capacity();
void test_runtime_consistency_with_dashboard();
void test_runtime_prod_db();
void test_scale_basic();
void test_scale_no_measured();
void test_scale_zero_profile();
void test_scale_preserves_ratio();
void test_scale_uneven_counts();
void test_soc_with_scaled_usage();
void test_soc_full_drain_recovery();
void test_regression_centered();
void test_regression_flat();
void test_runtime_nominal_voltage();
void test_daily_ci_variance_addition();

void test_sys_load_available();
void test_sys_mem_total_nonzero();
void test_disk_free_valid();
void test_cpu_freq_graceful();
void test_ssd_metrics_graceful();
void test_self_monitor_rss_nonzero();
void test_self_monitor_db_populated();
void test_analytics_api_self_monitor();
void test_dashboard_visibility();
void test_shelly_api_visibility();

void test_parse_test_type();
void test_test_type_str();
void test_test_result_str();
void test_make_telemetry_sample();
void test_test_runner_start_stop();
void test_test_runner_refuses_low_soc();
void test_test_runner_refuses_maintenance_mode();
void test_test_runner_safety_limits();
void test_test_schedules_crud();
void test_system_event_deduplication();

int main() {
    printf("=== limonitor unit tests ===\n");

    test_database();
    printf("PASS: Database basic\n");
    test_database_event_logging();
    printf("PASS: Database event logging\n");
    test_database_concurrency();
    printf("PASS: Database concurrency\n");
    test_database_test_runs();
    printf("PASS: Database test runs\n");
    test_database_test_telemetry();
    printf("PASS: Database test telemetry\n");

    test_http_options_cors();
    test_http_post_settings();
    test_http_solar_apis_with_extensions();
    test_solar_page_fetch_contract();
    test_solar_api_response_structure();
    test_http_solar_page_theme();
    test_http_tests_api();
    test_http_tests_start_stop_with_battery();
    test_http_tests_telemetry_and_notes();
    test_charger_history_api_empty();
    test_charger_history_api_with_data();
    test_charger_history_persistence_via_store();
    test_analytics_api_self_monitor();
    test_dashboard_visibility();
    test_shelly_api_visibility();
    printf("PASS: HTTP APIs\n");

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    test_weather_forecast_empty_api_key();
    test_parse_daily_entries_cloud_cover();
    test_parse_daily_entries_sunrise_sunset();
    test_parse_daily_entries_nested_clouds();
    test_forecast_week_extended_cloud_values();
    test_parse_daily_entries_empty();
    printf("PASS: Weather parsing\n");

    test_soc_basic_solar_charge();
    test_soc_basic_drain();
    test_soc_clamped_at_100();
    test_soc_clamped_at_0();
    test_soc_net_zero();
    test_soc_day_night_cycle();
    test_soc_empty_inputs();
    test_soc_zero_capacity();
    test_soc_negative_capacity();
    test_soc_start_above_100();
    test_soc_start_below_0();
    test_soc_mismatched_lengths();
    test_soc_realistic_scenario();
    printf("PASS: SOC engine\n");

    test_runtime_basic();
    test_runtime_full_to_cutoff();
    test_runtime_zero_load();
    test_runtime_soc_below_cutoff();
    test_runtime_soc_at_cutoff();
    test_runtime_zero_capacity();
    test_runtime_consistency_with_dashboard();
    test_runtime_prod_db();
    printf("PASS: Runtime engine\n");

    test_scale_basic();
    test_scale_no_measured();
    test_scale_zero_profile();
    test_scale_preserves_ratio();
    test_scale_uneven_counts();
    test_soc_with_scaled_usage();
    test_soc_full_drain_recovery();
    printf("PASS: Scaling engine\n");

    test_regression_centered();
    test_regression_flat();
    printf("PASS: Regression engine\n");

    test_runtime_nominal_voltage();
    test_daily_ci_variance_addition();
    test_sys_load_available();
    test_sys_mem_total_nonzero();
    test_disk_free_valid();
    test_cpu_freq_graceful();
    test_ssd_metrics_graceful();
    test_self_monitor_rss_nonzero();
    test_self_monitor_db_populated();
    printf("PASS: Misc analytics\n");

    test_parse_test_type();
    test_test_type_str();
    test_test_result_str();
    test_make_telemetry_sample();
    test_test_runner_start_stop();
    test_test_runner_refuses_low_soc();
    test_test_runner_refuses_maintenance_mode();
    test_test_runner_safety_limits();
    test_test_schedules_crud();
    test_system_event_deduplication();
    printf("PASS: Test runner\n");

    printf("\nALL UNIT TESTS PASSED\n");
    return 0;
}
