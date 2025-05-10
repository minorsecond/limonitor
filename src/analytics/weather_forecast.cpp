#include "weather_forecast.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <sstream>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

WeatherForecast::WeatherForecast(const WeatherConfig& cfg) : cfg_(cfg) {}

void WeatherForecast::set_config(const WeatherConfig& cfg) {
    cfg_ = cfg;
}

#ifdef HAVE_LIBCURL
static size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* user) {
    auto* s = static_cast<std::string*>(user);
    size_t n = size * nmemb;
    s->append(static_cast<const char*>(ptr), n);
    return n;
}
#endif

std::string WeatherForecast::fetch_api() const {
#ifdef HAVE_LIBCURL
    if (cfg_.api_key.empty() || cfg_.zip_code.empty()) return {};

    CURL* curl = curl_easy_init();
    if (!curl) return {};

    char url[256];
    std::snprintf(url, sizeof(url),
        "https://api.openweathermap.org/data/2.5/forecast?zip=%s,US&appid=%s&units=metric",
        cfg_.zip_code.c_str(), cfg_.api_key.c_str());

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return {};
    return body;
#else
    (void)cfg_;
    return {};
#endif
}

double WeatherForecast::parse_cloud_cover(const std::string& json) const {
    if (json.empty()) return 0;
    double sum = 0;
    int n = 0;
    size_t pos = 0;
    while ((pos = json.find("\"clouds\":{\"all\":", pos)) != std::string::npos) {
        pos += 16;
        double v = 0;
        if (std::sscanf(json.c_str() + pos, "%lf", &v) == 1) {
            sum += v / 100.0;
            ++n;
        }
        if (n >= 8) break;
        ++pos;
    }
    return n > 0 ? sum / n : 0;
}

void WeatherForecast::parse_forecast_list(const std::string& json,
                                         std::vector<std::tuple<int64_t, double, bool>>& out) const {
    out.clear();
    size_t pos = 0;
    while ((pos = json.find("\"dt\":", pos)) != std::string::npos) {
        pos += 5;
        int64_t dt = 0;
        if (std::sscanf(json.c_str() + pos, "%lld", (long long*)&dt) != 1) { ++pos; continue; }
        auto clouds_pos = json.find("\"clouds\":{\"all\":", pos);
        double cloud = 0;
        if (clouds_pos != std::string::npos && clouds_pos < pos + 500) {
            std::sscanf(json.c_str() + clouds_pos + 16, "%lf", &cloud);
        }
        auto pod_pos = json.find("\"pod\":\"", pos);
        bool daytime = true;
        if (pod_pos != std::string::npos && pod_pos < pos + 600) {
            daytime = (json.substr(pod_pos + 7, 1) == "d");
        }
        out.emplace_back(dt, cloud / 100.0, daytime);
        if (out.size() >= 40) break;
        ++pos;
    }
}

SolarForecastWeekResult WeatherForecast::get_forecast_week(double panel_watts, double efficiency,
                                                          double max_charge_a, double battery_voltage,
                                                          double nominal_ah, double current_soc_pct) const {
    SolarForecastWeekResult r;

#ifdef HAVE_LIBCURL
    if (cfg_.api_key.empty()) {
        r.error = "Weather API key not configured";
        return r;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - cache_time_).count();
    if (cached_json_.empty() || elapsed > CACHE_SEC) {
        cached_json_ = fetch_api();
        cache_time_ = now;
    }

    if (cached_json_.empty()) {
        r.error = "Failed to fetch weather";
        return r;
    }
    if (cached_json_.find("\"cod\":401") != std::string::npos ||
        cached_json_.find("\"cod\":\"401\"") != std::string::npos) {
        r.error = "Invalid weather API key";
        return r;
    }
    if (cached_json_.find("\"cod\":404") != std::string::npos ||
        cached_json_.find("\"cod\":\"404\"") != std::string::npos) {
        r.error = "Zip code not found";
        return r;
    }

    std::vector<std::tuple<int64_t, double, bool>> slots;
    parse_forecast_list(cached_json_, slots);
    if (slots.empty()) {
        r.error = "No forecast data";
        return r;
    }

    double max_a = max_charge_a > 0 ? max_charge_a : 10.0;
    double v = battery_voltage > 1 ? battery_voltage : 51.2;
    double cap_ah = nominal_ah > 0 ? nominal_ah : 100.0;
    double max_charge_w = max_a * v;
    double energy_to_full = (100.0 - current_soc_pct) / 100.0 * cap_ah * v;

    // Group by day (local date). Each slot is 3 hours. Peak sun ~6h/day.
    // kWh per 3h slot = panel_watts * 3 * efficiency * (1-cloud) / 1000
    std::map<int, std::vector<std::tuple<int64_t, double, bool>>> by_day;
    for (const auto& t : slots) {
        int64_t dt = std::get<0>(t);
        time_t tt = static_cast<time_t>(dt);
        struct tm tm_buf{};
        localtime_r(&tt, &tm_buf);
        int day_key = tm_buf.tm_year * 1000 + tm_buf.tm_yday;
        by_day[day_key].push_back(t);
    }

    double week_total = 0;
    double best_day_kwh = 0;

    for (const auto& kv : by_day) {
        DailySolarForecast d;
        const auto& day_slots = kv.second;

        time_t first_ts = static_cast<time_t>(std::get<0>(day_slots.front()));
        struct tm tm_buf{};
        localtime_r(&first_ts, &tm_buf);
        char date_buf[16];
        std::snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d",
                      tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday);
        d.date = date_buf;

        double day_kwh = 0;
        double cloud_sum = 0;
        int cloud_n = 0;
        int opt_start_idx = -1, opt_end_idx = -1;
        double sun_hours = 0;

        for (size_t i = 0; i < day_slots.size(); ++i) {
            bool daytime = std::get<2>(day_slots[i]);
            double cloud = std::get<1>(day_slots[i]);
            double slot_kwh = panel_watts * 3.0 * efficiency * (1.0 - cloud) / 1000.0;
            if (daytime) {
                slot_kwh = std::min(slot_kwh, max_charge_w * 3.0 / 1000.0);
                day_kwh += slot_kwh;
                cloud_sum += cloud;
                ++cloud_n;
                sun_hours += 3.0 * (1.0 - cloud);
            }
            if (daytime && cloud < 0.5) {
                if (opt_start_idx < 0) opt_start_idx = static_cast<int>(i);
                opt_end_idx = static_cast<int>(i);
            }
        }

        d.kwh = day_kwh;
        d.cloud_cover = cloud_n > 0 ? cloud_sum / cloud_n : 0;
        d.sun_hours_effective = sun_hours;

        // Max recovery: Wh, Ah, % with 95% CI (uncertainty from cloud forecast)
        double day_wh = day_kwh * 1000.0;
        double cap_wh = cap_ah * v;
        double headroom_pct = std::max(0.0, 100.0 - current_soc_pct);
        double cv = 0.10 + 0.10 * d.cloud_cover;  // 10–20% coefficient of variation
        double margin = 1.96 * cv;                 // 95% CI half-width
        double mult_lo = std::max(0.0, 1.0 - margin);
        double mult_hi = 1.0 + margin;

        d.max_recovery_wh = day_wh;
        d.max_recovery_wh_lo = day_wh * mult_lo;
        d.max_recovery_wh_hi = day_wh * mult_hi;

        d.max_recovery_ah = cap_ah > 0 ? day_wh / v : 0;
        d.max_recovery_ah_lo = cap_ah > 0 ? (day_wh * mult_lo) / v : 0;
        d.max_recovery_ah_hi = cap_ah > 0 ? (day_wh * mult_hi) / v : 0;

        d.max_recovery_pct = cap_wh > 0 ? std::min(headroom_pct, (day_wh / cap_wh) * 100.0) : 0;
        d.max_recovery_pct_lo = cap_wh > 0 ? std::max(0.0, std::min(headroom_pct, (day_wh * mult_lo / cap_wh) * 100.0)) : 0;
        d.max_recovery_pct_hi = cap_wh > 0 ? std::min(headroom_pct, (day_wh * mult_hi / cap_wh) * 100.0) : 0;

        if (opt_start_idx >= 0 && opt_end_idx >= 0) {
            time_t start_ts = static_cast<time_t>(std::get<0>(day_slots[opt_start_idx]));
            time_t end_ts = static_cast<time_t>(std::get<0>(day_slots[opt_end_idx])) + 10800;
            struct tm st{};
            localtime_r(&start_ts, &st);
            struct tm et{};
            localtime_r(&end_ts, &et);
            char tbuf[16];
            std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d", st.tm_hour, st.tm_min);
            d.optimal_start = tbuf;
            std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d", et.tm_hour, et.tm_min);
            d.optimal_end = tbuf;
        } else {
            d.optimal_start = "—";
            d.optimal_end = "—";
            if (d.cloud_cover >= 0.9)
                d.optimal_reason = "Overcast";
            else if (d.cloud_cover >= 0.7)
                d.optimal_reason = "Mostly cloudy";
            else if (sun_hours < 0.5)
                d.optimal_reason = "No clear window";
            else
                d.optimal_reason = "Cloudy all day";
        }

        r.daily.push_back(d);
        week_total += day_kwh;
        if (day_kwh > best_day_kwh) {
            best_day_kwh = day_kwh;
            r.best_day = d.date;
        }
    }

    r.week_total_kwh = week_total;
    r.recovery_wh = week_total * 1000.0;
    r.valid = true;

    double cumul = 0;
    for (const auto& d : r.daily) {
        cumul += d.kwh * 1000.0;
        if (cumul >= energy_to_full && r.days_to_full == 0) {
            r.days_to_full = static_cast<int>(&d - &r.daily[0]) + 1;
        }
    }
    if (r.days_to_full == 0 && week_total > 0)
        r.days_to_full = static_cast<int>(r.daily.size());

#else
    (void)panel_watts;
    (void)efficiency;
    (void)max_charge_a;
    (void)battery_voltage;
    (void)nominal_ah;
    (void)current_soc_pct;
    r.error = "Weather API not available (build without libcurl)";
#endif
    return r;
}

WeatherForecastResult WeatherForecast::get_forecast(double panel_watts, double efficiency,
                                                    double max_charge_a, double battery_voltage,
                                                    double nominal_ah, double current_soc_pct) const {
    WeatherForecastResult r;

#ifdef HAVE_LIBCURL
    if (cfg_.api_key.empty()) {
        r.error = "Weather API key not configured";
        return r;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - cache_time_).count();
    if (cached_json_.empty() || elapsed > CACHE_SEC) {
        cached_json_ = fetch_api();
        cache_time_ = now;
    }

    if (cached_json_.empty()) {
        r.error = "Failed to fetch weather (check network/libcurl)";
        return r;
    }

    // Check for API errors (401 invalid key, 404 city not found, etc.)
    if (cached_json_.find("\"cod\":401") != std::string::npos ||
        cached_json_.find("\"cod\":\"401\"") != std::string::npos) {
        r.error = "Invalid weather API key";
        return r;
    }
    if (cached_json_.find("\"cod\":404") != std::string::npos ||
        cached_json_.find("\"cod\":\"404\"") != std::string::npos) {
        r.error = "Zip code not found (try weather_zip_code)";
        return r;
    }
    if (cached_json_.find("\"cod\":") != std::string::npos &&
        cached_json_.find("\"cod\":\"200\"") == std::string::npos &&
        cached_json_.find("\"cod\":200") == std::string::npos) {
        r.error = "Weather API error";
        return r;
    }

    double cloud = parse_cloud_cover(cached_json_);

    std::time_t t = std::time(nullptr);
    struct tm tm_buf{};
    localtime_r(&t, &tm_buf);
    int month = tm_buf.tm_mon + 1;

    static const double psh[] = {4.5, 5.0, 5.8, 6.2, 6.5, 7.0, 6.8, 6.5, 6.0, 5.2, 4.5, 4.2};
    double psh_tomorrow = (month >= 1 && month <= 12) ? psh[month - 1] : 5.5;

    double wh = panel_watts * psh_tomorrow * efficiency * (1.0 - cloud);

    // Cap by charger capacity when available
    double max_a = max_charge_a > 0 ? max_charge_a : 10.0;
    double v = battery_voltage > 1 ? battery_voltage : 51.2;
    double cap_ah = nominal_ah > 0 ? nominal_ah : 100.0;
    double max_charge_w = max_a * v;
    double charger_cap_wh = max_charge_w * psh_tomorrow;
    double effective_wh = std::min(wh, charger_cap_wh);

    r.valid = true;
    r.tomorrow_generation_wh = effective_wh;
    r.cloud_cover = cloud;

    // Projection based on energy needed to reach 100%
    double energy_to_full = (100.0 - current_soc_pct) / 100.0 * cap_ah * v;
    if (effective_wh >= energy_to_full && current_soc_pct > 20)
        r.expected_battery_state = "100%";
    else if (effective_wh >= energy_to_full * 0.8)
        r.expected_battery_state = "90–100%";
    else if (effective_wh >= energy_to_full * 0.5 || effective_wh > 1000)
        r.expected_battery_state = "70–90%";
    else if (effective_wh > 500)
        r.expected_battery_state = "50–70%";
    else
        r.expected_battery_state = "<50%";
#else
    (void)panel_watts;
    (void)efficiency;
    (void)max_charge_a;
    (void)battery_voltage;
    (void)nominal_ah;
    (void)current_soc_pct;
    r.error = "Weather API not available (build without libcurl)";
#endif
    return r;
}
