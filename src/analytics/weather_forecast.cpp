#include "weather_forecast.hpp"
#include "../database.hpp"
#include "../logger.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <sstream>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

std::vector<double> project_battery_soc(double start_soc_pct, double capacity_wh,
                                        const std::vector<double>& generation_wh,
                                        const std::vector<double>& usage_wh) {
    size_t n = std::min(generation_wh.size(), usage_wh.size());
    std::vector<double> result;
    result.reserve(n);
    if (capacity_wh <= 0 || n == 0) {
        result.resize(n, std::clamp(start_soc_pct, 0.0, 100.0));
        return result;
    }
    double soc = std::clamp(start_soc_pct, 0.0, 100.0);
    for (size_t i = 0; i < n; ++i) {
        double net_wh = generation_wh[i] - usage_wh[i];
        soc += (net_wh / capacity_wh) * 100.0;
        soc = std::clamp(soc, 0.0, 100.0);
        result.push_back(soc);
    }
    return result;
}

double estimate_runtime_h(double capacity_wh, double soc_pct, double cutoff_pct, double load_w) {
    if (capacity_wh <= 0 || load_w <= 0 || soc_pct <= cutoff_pct) return 0;
    double usable_wh = capacity_wh * (soc_pct - cutoff_pct) / 100.0;
    return usable_wh / load_w;
}

double scale_usage_profile(std::vector<double>& avg_w_per_slot,
                           std::vector<double>& stddev_w_per_slot,
                           const std::vector<int>& sample_counts,
                           double measured_avg_w) {
    if (measured_avg_w <= 0.5) return 1.0;
    size_t n = avg_w_per_slot.size();
    int total_n = 0;
    double total_sum = 0;
    for (size_t i = 0; i < n && i < sample_counts.size(); ++i) {
        total_sum += avg_w_per_slot[i] * sample_counts[i];
        total_n += sample_counts[i];
    }
    double profile_avg = total_n > 0 ? total_sum / total_n : 0;
    if (profile_avg <= 0.5) return 1.0;
    double scale = measured_avg_w / profile_avg;
    for (size_t i = 0; i < n; ++i) avg_w_per_slot[i] *= scale;
    for (size_t i = 0; i < stddev_w_per_slot.size(); ++i) stddev_w_per_slot[i] *= scale;
    return scale;
}

WeatherForecast::WeatherForecast(const WeatherConfig& cfg, Database* db)
    : cfg_(cfg), db_(db)
{
    if (db_) {
        auto json_3h = db_->load_weather_cache("forecast_3h", DB_CACHE_MAX_AGE);
        if (!json_3h.empty()) {
            cached_json_ = std::move(json_3h);
            cache_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(CACHE_SEC / 2);
            LOG_INFO("Weather: loaded 3h forecast from DB cache");
        }
        auto json_daily = db_->load_weather_cache("forecast_daily", DB_CACHE_MAX_AGE);
        if (!json_daily.empty()) {
            cached_daily_json_ = std::move(json_daily);
            cache_daily_time_ = std::chrono::steady_clock::now() - std::chrono::seconds(CACHE_SEC / 2);
            LOG_INFO("Weather: loaded daily forecast from DB cache");
        }
    }
}

void WeatherForecast::set_config(const WeatherConfig& cfg) {
    cfg_ = cfg;
}

void WeatherForecast::invalidate_cache() const {
    cached_json_.clear();
    cached_daily_json_.clear();
    cache_time_ = {};
    cache_daily_time_ = {};
    LOG_INFO("Weather: cache invalidated, next request will fetch fresh data");
}

double WeatherForecast::current_cloud_cover() const {
    if (cached_json_.empty()) return -1;
    std::vector<std::tuple<int64_t, double, bool>> slots;
    parse_forecast_list(cached_json_, slots);
    if (slots.empty()) return -1;

    auto now = std::time(nullptr);
    int64_t best_dt = 0;
    double best_cloud = -1;
    int64_t best_diff = INT64_MAX;
    for (const auto& [dt, cloud, daytime] : slots) {
        int64_t diff = std::abs(dt - now);
        if (diff < best_diff) {
            best_diff = diff;
            best_dt = dt;
            best_cloud = cloud;
        }
    }
    if (best_diff > 7200) return -1;
    (void)best_dt;
    return best_cloud;
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
    body.reserve(32768);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_WARN("Weather API fetch failed: %s", curl_easy_strerror(res));
        return {};
    }
    LOG_DEBUG("Weather API fetch OK (%zu bytes)", body.size());
    return body;
#else
    (void)cfg_;
    return {};
#endif
}

std::string WeatherForecast::fetch_daily_api() const {
#ifdef HAVE_LIBCURL
    if (cfg_.api_key.empty() || cfg_.zip_code.empty()) return {};

    CURL* curl = curl_easy_init();
    if (!curl) return {};

    char url[320];
    std::snprintf(url, sizeof(url),
        "https://api.openweathermap.org/data/2.5/forecast/daily?zip=%s,US&cnt=16&appid=%s&units=metric",
        cfg_.zip_code.c_str(), cfg_.api_key.c_str());

    std::string body;
    body.reserve(16384);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_WARN("Weather daily API fetch failed: %s", curl_easy_strerror(res));
        return {};
    }
    LOG_DEBUG("Weather daily API fetch OK (%zu bytes)", body.size());
    return body;
#else
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
    out.reserve(40);
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
            daytime = (pod_pos + 7 < json.size() && json[pod_pos + 7] == 'd');
        }
        out.emplace_back(dt, cloud / 100.0, daytime);
        if (out.size() >= 40) break;
        ++pos;
    }
}

std::vector<WeatherForecast::DailyEntry> WeatherForecast::parse_daily_entries(const std::string& json) {
    std::vector<DailyEntry> entries;
    entries.reserve(16);
    if (json.empty()) return entries;

    size_t pos = 0;
    while ((pos = json.find("\"dt\":", pos)) != std::string::npos) {
        pos += 5;
        DailyEntry e;
        if (std::sscanf(json.c_str() + pos, "%lld", (long long*)&e.dt) != 1) {
            ++pos;
            continue;
        }

        auto next_dt = json.find("\"dt\":", pos);
        size_t entry_end = (next_dt != std::string::npos) ? next_dt : json.size();

        // Cloud cover
        e.cloud_cover = 0.5; // default
        {
            auto cp = json.find("\"clouds\":", pos);
            if (cp != std::string::npos && cp < entry_end) {
                size_t vstart = cp + 9;
                while (vstart < json.size() &&
                       (json[vstart] == ' ' || json[vstart] == '\t'))
                    ++vstart;
                if (vstart < json.size() && json[vstart] == '{') {
                    auto all_pos = json.find("\"all\":", vstart);
                    if (all_pos != std::string::npos && all_pos < vstart + 30) {
                        double cv = 0;
                        if (std::sscanf(json.c_str() + all_pos + 6, "%lf", &cv) == 1)
                            e.cloud_cover = cv / 100.0;
                    }
                } else {
                    double cv = 0;
                    if (std::sscanf(json.c_str() + vstart, "%lf", &cv) == 1)
                        e.cloud_cover = cv / 100.0;
                }
            }
        }

        // Sunrise / sunset
        e.daylight_h = 12.0;
        {
            auto sr = json.find("\"sunrise\":", pos);
            auto ss = json.find("\"sunset\":", pos);
            if (sr != std::string::npos && sr < entry_end &&
                ss != std::string::npos && ss < entry_end) {
                std::sscanf(json.c_str() + sr + 10, "%lld", (long long*)&e.sunrise);
                std::sscanf(json.c_str() + ss + 9, "%lld", (long long*)&e.sunset);
                if (e.sunrise > 0 && e.sunset > e.sunrise) {
                    e.daylight_h = (e.sunset - e.sunrise) / 3600.0;
                    if (e.daylight_h > 18.0) e.daylight_h = 18.0;
                    if (e.daylight_h < 4.0) e.daylight_h = 4.0;
                }
            }
        }

        entries.push_back(e);
        ++pos;
    }
    return entries;
}

static double acceptance_at_soc(const std::vector<ChargeAcceptanceBucket>* profile, double soc_pct) {
    if (!profile || profile->empty()) return 1.0;
    int bucket = static_cast<int>(soc_pct / 10.0);
    if (bucket < 0) bucket = 0;
    if (bucket > 9) bucket = 9;
    const auto& b = (*profile)[bucket];
    if (b.sample_count < 3) {
        // Not enough data for this bucket - try neighbors
        for (int delta = 1; delta <= 4; ++delta) {
            if (bucket - delta >= 0 && (*profile)[bucket - delta].sample_count >= 3)
                return (*profile)[bucket - delta].acceptance_ratio;
            if (bucket + delta <= 9 && (*profile)[bucket + delta].sample_count >= 3)
                return (*profile)[bucket + delta].acceptance_ratio;
        }
        return 1.0; // no data at all
    }
    return b.acceptance_ratio;
}

SolarForecastWeekResult WeatherForecast::get_forecast_week(double panel_watts, double efficiency,
                                                          double max_charge_a, double battery_voltage,
                                                          double nominal_ah, double current_soc_pct,
                                                          const std::vector<ChargeAcceptanceBucket>* charge_profile) const {
    SolarForecastWeekResult r;
    bool realistic = charge_profile && !charge_profile->empty();
    r.realistic = realistic;

#ifdef HAVE_LIBCURL
    if (cfg_.api_key.empty()) {
        LOG_WARN("Solar forecast: Weather API key not configured");
        r.error = "Weather API key not configured";
        return r;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - cache_time_).count();
    if (cached_json_.empty() || elapsed > CACHE_SEC) {
        std::string fresh = fetch_api();
        if (!fresh.empty()) {
            cached_json_ = fresh;
            cache_time_ = now;
            if (db_) db_->save_weather_cache("forecast_3h", fresh);
        }
    }

    if (cached_json_.empty()) {
        LOG_WARN("Solar forecast: Failed to fetch weather (check network/API)");
        r.error = "Failed to fetch weather";
        return r;
    }
    if (cached_json_.find("\"cod\":401") != std::string::npos ||
        cached_json_.find("\"cod\":\"401\"") != std::string::npos) {
        LOG_WARN("Solar forecast: Invalid weather API key");
        r.error = "Invalid weather API key";
        return r;
    }
    if (cached_json_.find("\"cod\":404") != std::string::npos ||
        cached_json_.find("\"cod\":\"404\"") != std::string::npos) {
        LOG_WARN("Solar forecast: Zip code not found: %s", cfg_.zip_code.c_str());
        r.error = "Zip code not found";
        return r;
    }

    std::vector<std::tuple<int64_t, double, bool>> slots;
    parse_forecast_list(cached_json_, slots);
    if (slots.empty()) {
        LOG_WARN("Solar forecast: No forecast data in API response");
        r.error = "No forecast data";
        return r;
    }

    double max_a = max_charge_a > 0 ? max_charge_a : 10.0;
    double v = battery_voltage > 1 ? battery_voltage : 51.2;
    double cap_ah = nominal_ah > 0 ? nominal_ah : 100.0;
    double max_charge_w = max_a * v;
    double energy_to_full = (100.0 - current_soc_pct) / 100.0 * cap_ah * v;

    r.nominal_ah = cap_ah;
    r.battery_voltage = v;
    r.current_soc_pct = current_soc_pct;

    double cap_wh_total = cap_ah * v;
    double projected_soc = current_soc_pct;

    r.slots.reserve(slots.size());
    r.daily.reserve(16);

    std::map<int, std::vector<std::tuple<int64_t, double, bool>>> by_day;
    for (const auto& t : slots) {
        int64_t dt = std::get<0>(t);
        double cloud = std::get<1>(t);
        bool daytime = std::get<2>(t);

        SolarForecastSlot s;
        s.timestamp = dt;
        s.cloud_cover = cloud;
        s.daytime = daytime;
        if (daytime) {
            double slot_kwh = panel_watts * 3.0 * efficiency * (1.0 - cloud) / 1000.0;
            slot_kwh = std::min(slot_kwh, max_charge_w * 3.0 / 1000.0);

            if (realistic) {
                double ar = acceptance_at_soc(charge_profile, projected_soc);
                slot_kwh *= ar;
            }

            double cv = 0.10 + 0.10 * cloud;
            double margin = 1.96 * cv;
            s.kwh = slot_kwh;
            s.kwh_lo = slot_kwh * std::max(0.0, 1.0 - margin);
            s.kwh_hi = slot_kwh * (1.0 + margin);

            // Advance projected SoC for next slot
            if (realistic && cap_wh_total > 0) {
                double wh_added = slot_kwh * 1000.0;
                projected_soc += (wh_added / cap_wh_total) * 100.0;
                if (projected_soc > 100.0) projected_soc = 100.0;
            }
        }
        r.slots.push_back(s);

        time_t tt = static_cast<time_t>(dt);
        struct tm tm_buf{};
        localtime_r(&tt, &tm_buf);
        int day_key = tm_buf.tm_year * 1000 + tm_buf.tm_yday;
        by_day[day_key].push_back(t);
    }

    double week_total_nominal = 0;
    double week_total_accepted = 0;
    double best_day_kwh = 0;
    double daily_projected_soc = current_soc_pct;

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

        double day_kwh_nominal = 0;
        double day_kwh_accepted = 0;
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
                day_kwh_nominal += slot_kwh;
                double accepted = slot_kwh;
                if (realistic) {
                    double ar = acceptance_at_soc(charge_profile, daily_projected_soc);
                    accepted *= ar;
                }
                day_kwh_accepted += accepted;
                cloud_sum += cloud;
                ++cloud_n;
                sun_hours += 3.0 * (1.0 - cloud);
                if (realistic && cap_wh_total > 0) {
                    daily_projected_soc += (accepted * 1000.0 / cap_wh_total) * 100.0;
                    if (daily_projected_soc > 100.0) daily_projected_soc = 100.0;
                }
            }
            if (daytime && cloud < 0.5) {
                if (opt_start_idx < 0) opt_start_idx = static_cast<int>(i);
                opt_end_idx = static_cast<int>(i);
            }
        }

        d.kwh = day_kwh_nominal;
        d.cloud_cover = cloud_n > 0 ? cloud_sum / cloud_n : 0;
        d.sun_hours_effective = sun_hours;

        double day_wh = (realistic ? day_kwh_accepted : day_kwh_nominal) * 1000.0;
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
        week_total_nominal += day_kwh_nominal;
        week_total_accepted += day_kwh_accepted;
        if (day_kwh_nominal > best_day_kwh) {
            best_day_kwh = day_kwh_nominal;
            r.best_day = d.date;
        }
    }

    r.week_total_kwh = week_total_nominal;
    r.recovery_wh = (realistic ? week_total_accepted : week_total_nominal) * 1000.0;
    r.valid = true;

    auto now_ext = std::chrono::steady_clock::now();
    auto elapsed_ext = std::chrono::duration_cast<std::chrono::seconds>(now_ext - cache_daily_time_).count();
    if (elapsed_ext > CACHE_SEC) {
        std::string fresh_daily = fetch_daily_api();
        if (!fresh_daily.empty() && fresh_daily.find("\"cod\":401") == std::string::npos &&
            fresh_daily.find("\"cod\":404") == std::string::npos) {
            cached_daily_json_ = fresh_daily;
            cache_daily_time_ = now_ext;
            if (db_) db_->save_weather_cache("forecast_daily", fresh_daily);
        }
    }
    if (!cached_daily_json_.empty()) {
        std::set<std::string> existing_dates;
        for (const auto& d : r.daily) existing_dates.insert(d.date);

        auto daily_entries = parse_daily_entries(cached_daily_json_);
        LOG_DEBUG("Extended forecast: parsed %zu entries from daily API JSON (%zu bytes)",
                  daily_entries.size(), cached_daily_json_.size());
        for (const auto& de : daily_entries) {
            time_t tt = static_cast<time_t>(de.dt);
            struct tm tm_buf{};
            localtime_r(&tt, &tm_buf);
            char date_buf[16];
            std::snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d",
                          tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday);
            if (existing_dates.count(date_buf)) {
                LOG_DEBUG("  %s: skipped (from 3h forecast), api_cloud=%.0f%%",
                          date_buf, de.cloud_cover * 100.0);
                continue;
            }
            existing_dates.insert(date_buf);

            double cloud = de.cloud_cover;
            double daylight_h = de.daylight_h;
            LOG_DEBUG("  %s: cloud=%.0f%% daylight=%.1fh (extended)", date_buf, cloud * 100.0, daylight_h);

            DailySolarForecast ext;
            ext.date = date_buf;
            ext.cloud_cover = cloud;
            double sun_h = daylight_h * (1.0 - cloud);
            ext.sun_hours_effective = sun_h;
            double day_kwh_nom = panel_watts * daylight_h * efficiency * (1.0 - cloud) / 1000.0;
            day_kwh_nom = std::min(day_kwh_nom, max_charge_w * daylight_h / 1000.0);
            double day_kwh_acc = day_kwh_nom;
            if (realistic) {
                double total_ar = 0;
                int n_sub = 3;
                for (int ss = 0; ss < n_sub; ++ss) {
                    double sub_soc = daily_projected_soc + (ss * day_kwh_nom * 1000.0 / (n_sub * cap_wh_total)) * 100.0;
                    if (sub_soc > 100) sub_soc = 100;
                    total_ar += acceptance_at_soc(charge_profile, sub_soc);
                }
                day_kwh_acc = day_kwh_nom * total_ar / n_sub;
                if (cap_wh_total > 0) {
                    daily_projected_soc += (day_kwh_acc * 1000.0 / cap_wh_total) * 100.0;
                    if (daily_projected_soc > 100) daily_projected_soc = 100;
                }
            }
            ext.kwh = day_kwh_nom;
            if (cloud < 0.7 && de.sunrise > 0 && de.sunset > de.sunrise) {
                // Estimate optimal window from sunrise/sunset for partly-cloudy extended days
                time_t sr_t = static_cast<time_t>(de.sunrise);
                time_t ss_t = static_cast<time_t>(de.sunset);
                struct tm sr_tm{}, ss_tm{};
                localtime_r(&sr_t, &sr_tm);
                localtime_r(&ss_t, &ss_tm);
                char tbuf[16];
                std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d", sr_tm.tm_hour, sr_tm.tm_min);
                ext.optimal_start = tbuf;
                std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d", ss_tm.tm_hour, ss_tm.tm_min);
                ext.optimal_end = tbuf;
                ext.optimal_reason = "";
            } else {
                ext.optimal_start = "—";
                ext.optimal_end = "—";
                ext.optimal_reason = cloud >= 0.9 ? "Overcast" : "Mostly cloudy";
            }
            ext.is_extended = true;
            double day_wh = (realistic ? day_kwh_acc : day_kwh_nom) * 1000.0;
            double headroom_pct = std::max(0.0, 100.0 - current_soc_pct);
            double cv = 0.10 + 0.10 * ext.cloud_cover;
            double margin = 1.96 * cv;
            double mult_lo = std::max(0.0, 1.0 - margin), mult_hi = 1.0 + margin;
            ext.max_recovery_wh = day_wh;
            ext.max_recovery_wh_lo = day_wh * mult_lo;
            ext.max_recovery_wh_hi = day_wh * mult_hi;
            ext.max_recovery_ah = cap_ah > 0 ? day_wh / v : 0;
            ext.max_recovery_ah_lo = cap_ah > 0 ? (day_wh * mult_lo) / v : 0;
            ext.max_recovery_ah_hi = cap_ah > 0 ? (day_wh * mult_hi) / v : 0;
            double ext_cap_wh = cap_ah * v;
            ext.max_recovery_pct = ext_cap_wh > 0 ? std::min(headroom_pct, (day_wh / ext_cap_wh) * 100.0) : 0;
            ext.max_recovery_pct_lo = ext_cap_wh > 0 ? std::max(0.0, std::min(headroom_pct, (day_wh * mult_lo / ext_cap_wh) * 100.0)) : 0;
            ext.max_recovery_pct_hi = ext_cap_wh > 0 ? std::min(headroom_pct, (day_wh * mult_hi / ext_cap_wh) * 100.0) : 0;
            r.daily.push_back(ext);
            week_total_nominal += day_kwh_nom;
            week_total_accepted += day_kwh_acc;
            r.week_total_kwh = week_total_nominal;
            r.recovery_wh = (realistic ? week_total_accepted : week_total_nominal) * 1000.0;
            if (day_kwh_nom > best_day_kwh) {
                best_day_kwh = day_kwh_nom;
                r.best_day = ext.date;
            }
        }
    }

    double cumul = 0;
    for (const auto& d : r.daily) {
        cumul += d.max_recovery_wh;
        if (cumul >= energy_to_full && r.days_to_full == 0) {
            r.days_to_full = static_cast<int>(&d - &r.daily[0]) + 1;
        }
    }
    if (r.days_to_full == 0 && week_total_nominal > 0)
        r.days_to_full = static_cast<int>(r.daily.size());

    if (db_) {
        for (const auto& d : r.daily) {
            Database::WeatherDailyRow row;
            row.date = d.date;
            row.cloud_cover = d.cloud_cover;
            row.kwh_forecast = d.kwh;
            row.sun_hours = d.sun_hours_effective;
            row.source = d.is_extended ? "daily" : "3h";
            db_->upsert_weather_daily(row);
        }
    }

#else
    (void)panel_watts;
    (void)efficiency;
    (void)max_charge_a;
    (void)battery_voltage;
    (void)nominal_ah;
    (void)current_soc_pct;
    (void)charge_profile;
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
        std::string fresh = fetch_api();
        if (!fresh.empty()) {
            cached_json_ = fresh;
            cache_time_ = now;
            if (db_) db_->save_weather_cache("forecast_3h", fresh);
        }
    }

    if (cached_json_.empty()) {
        r.error = "Failed to fetch weather (check network/libcurl)";
        return r;
    }

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

    double max_a = max_charge_a > 0 ? max_charge_a : 10.0;
    double v = battery_voltage > 1 ? battery_voltage : 51.2;
    double cap_ah = nominal_ah > 0 ? nominal_ah : 100.0;
    double max_charge_w = max_a * v;
    double charger_cap_wh = max_charge_w * psh_tomorrow;
    double effective_wh = std::min(wh, charger_cap_wh);

    r.valid = true;
    r.tomorrow_generation_wh = effective_wh;
    r.cloud_cover = cloud;

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
