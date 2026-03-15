#include "ble_manager.hpp"
#include "logger.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <gio/gio.h>

extern int g_poll_interval_s;

static std::string gv_str(GVariant* v) {
    if (!v) return "";
    const char* s = g_variant_get_string(v, nullptr);
    return s ? s : "";
}
static bool str_icontains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
        [](char a, char b){ return std::tolower(a) == std::tolower(b); });
    return it != hay.end();
}

struct BleManager::Impl {
    // Config
    std::string target;
    std::string adapter_path;
    std::string service_uuid;
    std::string notify_uuid;
    std::string write_uuid;

    // State
    std::atomic<BleState> state{BleState::DISCONNECTED};
    mutable std::mutex    state_mu;
    std::string           device_address;
    std::string           device_name;
    std::string           device_path;

    // Callbacks
    DataCb      data_cb;
    StateCb     state_cb;
    DiscoveryCb discovery_cb;

    // Discovered devices: MAC address → D-Bus object path
    std::map<std::string, std::string> discovered;  // protected by state_mu

    // Pending connect_to() from external thread
    std::string           pending_connect_addr;  // set by connect_to(), read in GLib thread
    GMutex                pending_mu;

    // GLib objects — only touched on GLib thread
    GMainContext*       gctx{nullptr};
    GMainLoop*          gloop{nullptr};
    GCancellable*       cancel{nullptr};
    GDBusObjectManager* obj_mgr{nullptr};
    GDBusProxy*         device_proxy{nullptr};
    GDBusProxy*         notify_char{nullptr};
    GDBusProxy*         write_char{nullptr};
    gulong              device_sig{0};
    gulong              char_sig{0};
    guint               poll_src{0};
    guint               reconnect_src{0};
    guint               char_find_src{0};
    guint               scan_stuck_src{0};
    int                 char_find_attempt{0};
    int                 hard_reset_count{0};
    int                 connect_fail_count{0};
    static constexpr int SCAN_STUCK_TIMEOUT_S  = 90;
    static constexpr int CONNECT_FAIL_THRESHOLD = 3;
    std::chrono::steady_clock::time_point last_data_time;

    std::thread thread;

    // Poll command bytes (set via BleManager::set_poll_command before start)
    std::vector<uint8_t> poll_command;

    GMutex                          wq_mu;
    GCond                           wq_cond;
    std::vector<std::vector<uint8_t>> write_queue;

    void set_state(BleState s, const std::string& msg = "") {
        BleState old = state.exchange(s);
        if (s == BleState::READY || s == BleState::CONNECTING || s == BleState::SCANNING)
            last_data_time = std::chrono::steady_clock::now();
        if (old != s || !msg.empty()) {
            LOG_INFO("BLE: %s -> %s%s%s",
                ble_state_str(old), ble_state_str(s),
                msg.empty() ? "" : " — ", msg.c_str());
        }
        if (state_cb) state_cb(s, msg);
    }

    bool is_target(GDBusProxy* dev) const {
        if (target.empty()) return dev_has_service(dev);
        g_autoptr(GVariant) av = g_dbus_proxy_get_cached_property(dev, "Address");
        std::string addr = gv_str(av);
        if (!addr.empty() && strcasecmp(addr.c_str(), target.c_str()) == 0) return true;

        g_autoptr(GVariant) nv = g_dbus_proxy_get_cached_property(dev, "Name");
        std::string name = gv_str(nv);
        // Match if name contains target OR target contains name (handles truncated ad names)
        if (!name.empty() && (str_icontains(name, target) || str_icontains(target, name))) return true;

        g_autoptr(GVariant) al = g_dbus_proxy_get_cached_property(dev, "Alias");
        std::string alias = gv_str(al);
        if (!alias.empty() && (str_icontains(alias, target) || str_icontains(target, alias))) return true;

        return false;
    }

    bool dev_has_service(GDBusProxy* dev) const {
        g_autoptr(GVariant) uuids_v = g_dbus_proxy_get_cached_property(dev, "UUIDs");
        if (!uuids_v) return false;
        GVariantIter it; g_variant_iter_init(&it, uuids_v);
        const char* u;
        while (g_variant_iter_next(&it, "&s", &u))
            if (str_icontains(u, service_uuid)) return true;
        return false;
    }

    void maybe_connect(GDBusProxy* proxy, const std::string& path) {
        if (is_target(proxy)) {
            BleState expected = BleState::SCANNING;
            if (!state.compare_exchange_strong(expected, BleState::CONNECTING)) {
                // Already connecting or connected.
                return;
            }
            g_autoptr(GDBusProxy) adapter = g_dbus_proxy_new_for_bus_sync(
                G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, "org.bluez",
                adapter_path.c_str(), "org.bluez.Adapter1", nullptr, nullptr);
            if (adapter) {
                LOG_DEBUG("BLE: stopping discovery before connect");
                g_dbus_proxy_call(adapter, "StopDiscovery",
                    nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr, nullptr);
            }
            connect_device(path);
        }
    }

    // Report a device via the discovery callback and add to discovered map.
    // Called from GLib thread.
    void report_device(GDBusProxy* dev, const std::string& path) {
        g_autoptr(GVariant) av = g_dbus_proxy_get_cached_property(dev, "Address");
        g_autoptr(GVariant) nv = g_dbus_proxy_get_cached_property(dev, "Name");
        g_autoptr(GVariant) rv = g_dbus_proxy_get_cached_property(dev, "RSSI");
        std::string addr = gv_str(av);
        std::string name = gv_str(nv);
        int rssi = rv ? static_cast<int>(g_variant_get_int16(rv)) : 0;

        if (addr.empty()) return;

        {
            std::lock_guard<std::mutex> lk(state_mu);
            discovered[addr] = path;
        }

        if (discovery_cb) {
            DiscoveredDevice d;
            d.id                 = addr;
            d.name               = name;
            d.rssi               = rssi;
            d.has_target_service = dev_has_service(dev);
            discovery_cb(d);
        }
    }

    void gio_main() {
        gctx  = g_main_context_new();
        gloop = g_main_loop_new(gctx, FALSE);
        g_main_context_push_thread_default(gctx);

        g_dbus_object_manager_client_new_for_bus(
            G_BUS_TYPE_SYSTEM,
            G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
            "org.bluez", "/", nullptr, nullptr, nullptr, cancel,
            cb_obj_mgr_ready, this);

        g_main_loop_run(gloop);

        if (char_find_src)  { g_source_remove(char_find_src);  char_find_src  = 0; }
        if (poll_src)       { g_source_remove(poll_src);       poll_src       = 0; }
        if (reconnect_src)  { g_source_remove(reconnect_src);  reconnect_src  = 0; }
        if (scan_stuck_src) { g_source_remove(scan_stuck_src); scan_stuck_src = 0; }
        if (notify_char) {
            if (char_sig) { g_signal_handler_disconnect(notify_char, char_sig); char_sig = 0; }
            g_dbus_proxy_call_sync(notify_char, "StopNotify",
                nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, nullptr);
            g_object_unref(notify_char); notify_char = nullptr;
        }
        if (write_char)   { g_object_unref(write_char);   write_char   = nullptr; }
        if (device_proxy) {
            if (device_sig) { g_signal_handler_disconnect(device_proxy, device_sig); device_sig = 0; }
            g_object_unref(device_proxy); device_proxy = nullptr;
        }
        if (obj_mgr) { g_object_unref(obj_mgr); obj_mgr = nullptr; }
        g_main_context_pop_thread_default(gctx);
        g_main_loop_unref(gloop); gloop = nullptr;
        g_main_context_unref(gctx); gctx = nullptr;
    }

    bool try_find_device() {
        GList* objects = g_dbus_object_manager_get_objects(obj_mgr);
        bool found = false;
        for (GList* l = objects; l; l = l->next) {
            GDBusObject* obj = G_DBUS_OBJECT(l->data);
            GDBusInterface* iface = g_dbus_object_get_interface(obj, "org.bluez.Device1");
            if (iface) {
                GDBusProxy* proxy = G_DBUS_PROXY(iface);
                const char* path = g_dbus_object_get_object_path(obj);
                report_device(proxy, path);
                if (!found && is_target(proxy)) {
                    BleState current = state.load();
                    if (current == BleState::DISCONNECTED || current == BleState::ERROR || current == BleState::SCANNING) {
                        if (state.compare_exchange_strong(current, BleState::CONNECTING)) {
                            connect_device(path);
                            found = true;
                        }
                    }
                }
                g_object_unref(iface);
            }
        }
        g_list_free_full(objects, g_object_unref);
        return found;
    }

    void start_discovery() {
        BleState curr = state.load();
        if (curr != BleState::DISCONNECTED && curr != BleState::ERROR && curr != BleState::SCANNING) {
            LOG_DEBUG("BLE: skip start_discovery in state %s", ble_state_str(curr));
            return;
        }
        set_state(BleState::SCANNING, target);
        GError* err = nullptr;
        g_autoptr(GDBusProxy) adapter = g_dbus_proxy_new_for_bus_sync(
            G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr,
            "org.bluez", adapter_path.c_str(), "org.bluez.Adapter1", cancel, &err);
        if (err) {
            LOG_ERROR("BLE: Adapter1: %s", err->message);
            g_error_free(err);
            schedule_reconnect(10);
            return;
        }

        // Check if already discovering
        g_autoptr(GVariant) d = g_dbus_proxy_get_cached_property(adapter, "Discovering");
        if (d && g_variant_get_boolean(d)) {
            LOG_INFO("BLE: discovery already active");
            start_scan_stuck_timer();
            return;
        }

        g_autoptr(GVariant) r = g_dbus_proxy_call_sync(adapter, "StartDiscovery",
            nullptr, G_DBUS_CALL_FLAGS_NONE, 15000, cancel, &err);
        if (err) {
            bool in_progress = strstr(err->message, "InProgress") != nullptr;
            if (!in_progress) {
                LOG_WARN("BLE: StartDiscovery: %s — trying StopDiscovery", err->message);
                g_dbus_proxy_call_sync(adapter, "StopDiscovery", nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
                set_state(BleState::DISCONNECTED);
                schedule_reconnect(10);
            }
            g_error_free(err);
        } else {
            LOG_INFO("BLE: scanning for devices…");
            start_scan_stuck_timer();
        }
    }

    void connect_device(const std::string& path) {
        device_path = path;
        set_state(BleState::CONNECTING);
        GError* err = nullptr;
        device_proxy = g_dbus_proxy_new_for_bus_sync(
            G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr,
            "org.bluez", path.c_str(), "org.bluez.Device1", cancel, &err);
        if (err) {
            LOG_ERROR("BLE: Device1: %s", err->message);
            g_error_free(err); schedule_reconnect(); return;
        }
        {
            g_autoptr(GVariant) av = g_dbus_proxy_get_cached_property(device_proxy, "Address");
            g_autoptr(GVariant) nv = g_dbus_proxy_get_cached_property(device_proxy, "Name");
            std::lock_guard<std::mutex> lk(state_mu);
            if (av) device_address = gv_str(av);
            if (nv) device_name    = gv_str(nv);
        }
        LOG_INFO("BLE: connecting to %s (%s)", device_address.c_str(), device_name.c_str());

        device_sig = g_signal_connect(device_proxy, "g-properties-changed",
            G_CALLBACK(cb_device_props), this);

        g_autoptr(GVariant) sr = g_dbus_proxy_get_cached_property(device_proxy, "ServicesResolved");
        if (sr && g_variant_get_boolean(sr)) { on_services_resolved(); return; }

        g_dbus_proxy_call(device_proxy, "Connect",
            nullptr, G_DBUS_CALL_FLAGS_NONE, 30000, cancel,
            +[](GObject* src, GAsyncResult* res, gpointer ud) {
                GError* e = nullptr;
                g_autoptr(GVariant) r = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &e);
                auto* self = static_cast<Impl*>(ud);
                if (e) {
                    if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                        LOG_WARN("BLE: Connect failed: %s", e->message);
                        self->set_state(BleState::DISCONNECTED);
                        if (++self->connect_fail_count >= CONNECT_FAIL_THRESHOLD) {
                            LOG_WARN("BLE: %d consecutive connect failures — hard reset", self->connect_fail_count);
                            self->connect_fail_count = 0;
                            self->hard_reset_device();
                        } else {
                            self->schedule_reconnect();
                        }
                    }
                    g_error_free(e);
                } else {
                    self->set_state(BleState::DISCOVERING);
                }
            }, this);
    }

    void on_services_resolved() {
        set_state(BleState::DISCOVERING);
        LOG_INFO("BLE: services resolved, starting characteristics discovery");
        char_find_attempt = 0;
        schedule_char_find();
    }

    void schedule_char_find(int delay_s = 0) {
        if (char_find_src) { g_source_remove(char_find_src); char_find_src = 0; }
        GSource* src = delay_s > 0
            ? g_timeout_source_new_seconds(delay_s)
            : g_idle_source_new();
        g_source_set_callback(src, cb_try_find_chars, this, nullptr);
        char_find_src = g_source_attach(src, gctx);
        g_source_unref(src);
    }

    void do_find_chars() {
        if (find_characteristics()) {
            LOG_INFO("BLE: characteristics found, starting notifications");
            g_dbus_proxy_call(notify_char, "StartNotify",
                nullptr, G_DBUS_CALL_FLAGS_NONE, 10000, cancel,
                +[](GObject* src, GAsyncResult* res, gpointer ud) {
                    GError* e = nullptr;
                    g_autoptr(GVariant) r = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &e);
                    auto* self = static_cast<Impl*>(ud);
                    if (e) {
                        if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                            LOG_ERROR("BLE: StartNotify: %s", e->message);
                            self->schedule_reconnect();
                        }
                        g_error_free(e);
                    } else {
                        LOG_INFO("BLE: ready");
                        self->connect_fail_count = 0;
                        self->set_state(BleState::READY);
                        self->schedule_poll();
                    }
                }, this);
            return;
        }
        if (char_find_attempt < 4) {
            LOG_INFO("BLE: characteristics not found yet, retrying in 1s... (attempt %d/5)", char_find_attempt + 1);
            char_find_attempt++;
            schedule_char_find(1);
        } else {
            LOG_ERROR("BLE: characteristics not found");
            schedule_reconnect(15);
        }
    }

    bool find_characteristics() {
        std::string notify_path, write_path;
        int char_count = 0;
        GList* objects = g_dbus_object_manager_get_objects(obj_mgr);
        for (GList* l = objects; l; l = l->next) {
            GDBusObject* obj = G_DBUS_OBJECT(l->data);
            const char* path = g_dbus_object_get_object_path(obj);
            if (device_path.empty() || strncmp(path, device_path.c_str(), device_path.size()) != 0)
                continue;

            GDBusInterface* iface = g_dbus_object_get_interface(obj, "org.bluez.GattCharacteristic1");
            if (iface) {
                char_count++;
                GDBusProxy* proxy = G_DBUS_PROXY(iface);
                g_autoptr(GVariant) uv = g_dbus_proxy_get_cached_property(proxy, "UUID");
                std::string uuid = gv_str(uv);
                LOG_DEBUG("BLE: found char at %s: UUID=%s", path, uuid.c_str());
                if (uuid == notify_uuid && notify_path.empty())
                    notify_path = path;
                else if (uuid == write_uuid && write_path.empty())
                    write_path = path;
                g_object_unref(iface);
            }
            if (!notify_path.empty() && !write_path.empty()) break;
        }
        g_list_free_full(objects, g_object_unref);

        if (notify_path.empty() || write_path.empty()) {
            LOG_WARN("BLE: find_characteristics failed (found %d chars, notify=%s, write=%s)",
                     char_count, notify_path.empty() ? "NO" : "YES", write_path.empty() ? "NO" : "YES");
            return false;
        }

        LOG_INFO("BLE: notify char: %s", notify_path.c_str());
        LOG_INFO("BLE: write  char: %s", write_path.c_str());

        // Create dedicated proxies (not from object manager cache) so that
        // g-properties-changed is delivered reliably for incoming notifications.
        GError* err = nullptr;
        notify_char = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
            G_DBUS_PROXY_FLAGS_NONE, nullptr, "org.bluez",
            notify_path.c_str(), "org.bluez.GattCharacteristic1", cancel, &err);
        if (err) {
            LOG_ERROR("BLE: notify proxy: %s", err->message);
            g_error_free(err); return false;
        }
        char_sig = g_signal_connect(notify_char, "g-properties-changed",
            G_CALLBACK(cb_char_props), this);

        write_char = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM,
            G_DBUS_PROXY_FLAGS_NONE, nullptr, "org.bluez",
            write_path.c_str(), "org.bluez.GattCharacteristic1", cancel, &err);
        if (err) {
            LOG_ERROR("BLE: write proxy: %s", err->message);
            g_error_free(err);
            g_object_unref(notify_char); notify_char = nullptr; return false;
        }
        return true;
    }

    void schedule_poll(int delay_s = 0) {
        if (poll_src) { g_source_remove(poll_src); poll_src = 0; }
        GSource* src = g_timeout_source_new_seconds(delay_s);
        g_source_set_callback(src, cb_poll, this, nullptr);
        poll_src = g_source_attach(src, gctx);
        g_source_unref(src);
    }
    void do_poll() {
        BleState s = state.load();
        if (s != BleState::READY && s != BleState::CONNECTING && s != BleState::DISCOVERING) {
            schedule_poll(g_poll_interval_s);
            return;
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_data_time).count() > 30) {
            LOG_WARN("BLE: data timeout (30s, state %s) — reconnecting", ble_state_str(s));
            schedule_reconnect();
            return;
        }

        if (s != BleState::READY || !write_char || poll_command.empty()) {
            schedule_poll(g_poll_interval_s);
            return;
        }

        LOG_DEBUG("BLE: polling (%zu bytes)", poll_command.size());
        GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("ay"));
        for (uint8_t byte : poll_command) g_variant_builder_add(&b, "y", byte);
        GVariantBuilder opts; g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&opts, "{sv}", "type", g_variant_new_string("request"));
        GVariant* params = g_variant_new("(@ay@a{sv})", g_variant_builder_end(&b),
                                         g_variant_builder_end(&opts));
        g_dbus_proxy_call(write_char, "WriteValue", params,
            G_DBUS_CALL_FLAGS_NONE, 5000, cancel,
            +[](GObject*, GAsyncResult* r, gpointer ud) {
                GError* e = nullptr;
                g_autoptr(GVariant) ret = g_dbus_proxy_call_finish(G_DBUS_PROXY(ud), r, &e);
                if (e) {
                    if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                        LOG_WARN("BLE: WriteValue: %s", e->message);
                    g_error_free(e);
                } else {
                    LOG_DEBUG("BLE: WriteValue OK");
                }
            }, write_char);
        schedule_poll(g_poll_interval_s);
    }

    void drain_write_queue() {
        g_mutex_lock(&wq_mu);
        auto queue = std::move(write_queue);
        g_mutex_unlock(&wq_mu);
        for (const auto& pkt : queue) {
            GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE("ay"));
            for (uint8_t byte : pkt) g_variant_builder_add(&b, "y", byte);
            GVariantBuilder opts; g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&opts, "{sv}", "type", g_variant_new_string("request"));
            GVariant* params = g_variant_new("(@ay@a{sv})", g_variant_builder_end(&b),
                                             g_variant_builder_end(&opts));
            g_dbus_proxy_call(write_char, "WriteValue", params,
                G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr, nullptr);
        }
    }

    void start_scan_stuck_timer() {
        if (scan_stuck_src) { g_source_remove(scan_stuck_src); scan_stuck_src = 0; }
        GSource* src = g_timeout_source_new_seconds(SCAN_STUCK_TIMEOUT_S);
        g_source_set_callback(src, cb_scan_stuck, this, nullptr);
        scan_stuck_src = g_source_attach(src, gctx);
        g_source_unref(src);
    }

    // Hard reset: disconnect the device from BlueZ and remove it from BlueZ's
    // device cache so the next scan rediscovers it fresh.  This clears stale
    // connection state that can get the BLE stack stuck in SCANNING indefinitely.
    void hard_reset_device() {
        LOG_WARN("BLE: hard reset #%d — removing stale device from BlueZ cache", ++hard_reset_count);

        // Locate the target device in the object manager
        std::string dev_path;
        if (obj_mgr) {
            GList* objects = g_dbus_object_manager_get_objects(obj_mgr);
            for (GList* l = objects; l && dev_path.empty(); l = l->next) {
                GDBusObject*    obj   = G_DBUS_OBJECT(l->data);
                GDBusInterface* iface = g_dbus_object_get_interface(obj, "org.bluez.Device1");
                if (iface) {
                    GDBusProxy* proxy = G_DBUS_PROXY(iface);
                    if (is_target(proxy))
                        dev_path = g_dbus_object_get_object_path(obj);
                    g_object_unref(iface);
                }
            }
            g_list_free_full(objects, g_object_unref);
        }

        if (dev_path.empty()) {
            LOG_WARN("BLE: hard reset — device not in BlueZ cache; stopping stuck scan and restarting");
            // Stop the current (stuck) discovery so start_discovery() gets a fresh scan.
            // Without this, BlueZ reports Discovering=true indefinitely and we skip out early.
            GError* err2 = nullptr;
            g_autoptr(GDBusProxy) adp = g_dbus_proxy_new_for_bus_sync(
                G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr,
                "org.bluez", adapter_path.c_str(), "org.bluez.Adapter1", nullptr, &err2);
            if (err2) { g_error_free(err2); err2 = nullptr; }
            if (adp) {
                g_dbus_proxy_call_sync(adp, "StopDiscovery",
                    nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &err2);
                if (err2) { g_error_free(err2); err2 = nullptr; }
                LOG_INFO("BLE: hard reset — StopDiscovery done, will restart scan in 3s");
            }
            // If we've been failing for a long time, also power-cycle the adapter
            if (hard_reset_count >= 4) {
                LOG_WARN("BLE: %d hard resets — power-cycling BT adapter", hard_reset_count);
                g_autoptr(GDBusProxy) props = g_dbus_proxy_new_for_bus_sync(
                    G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr,
                    "org.bluez", adapter_path.c_str(), "org.freedesktop.DBus.Properties", nullptr, nullptr);
                if (props) {
                    g_dbus_proxy_call_sync(props, "Set",
                        g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered", g_variant_new_boolean(FALSE)),
                        G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
                    g_usleep(2000000); // 2s — blocks GLib loop intentionally during adapter reset
                    g_dbus_proxy_call_sync(props, "Set",
                        g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered", g_variant_new_boolean(TRUE)),
                        G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
                    LOG_INFO("BLE: adapter power-cycled");
                    hard_reset_count = 0; // restart the escalation counter
                }
            }
            schedule_reconnect(3);
            return;
        }

        // Disconnect first (best-effort; ignore errors)
        GError* err = nullptr;
        g_autoptr(GDBusProxy) dev_proxy = g_dbus_proxy_new_for_bus_sync(
            G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr,
            "org.bluez", dev_path.c_str(), "org.bluez.Device1", nullptr, &err);
        if (err) { g_error_free(err); err = nullptr; }
        if (dev_proxy) {
            g_autoptr(GVariant) conn = g_dbus_proxy_get_cached_property(dev_proxy, "Connected");
            if (conn && g_variant_get_boolean(conn)) {
                LOG_INFO("BLE: hard reset — disconnecting device");
                g_dbus_proxy_call_sync(dev_proxy, "Disconnect",
                    nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
            }
        }

        // Remove device from adapter cache so the next scan is fresh
        g_autoptr(GDBusProxy) adapter = g_dbus_proxy_new_for_bus_sync(
            G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr,
            "org.bluez", adapter_path.c_str(), "org.bluez.Adapter1", nullptr, &err);
        if (err) { g_error_free(err); err = nullptr; }
        if (adapter) {
            LOG_INFO("BLE: hard reset — calling RemoveDevice(%s)", dev_path.c_str());
            g_autoptr(GVariant) r = g_dbus_proxy_call_sync(adapter, "RemoveDevice",
                g_variant_new("(o)", dev_path.c_str()),
                G_DBUS_CALL_FLAGS_NONE, 10000, nullptr, &err);
            if (err) {
                LOG_WARN("BLE: RemoveDevice failed: %s — will retry via normal reconnect", err->message);
                g_error_free(err);
            } else {
                LOG_INFO("BLE: hard reset — device removed, scanning fresh");
            }
        }

        schedule_reconnect(3);
    }

    void schedule_reconnect(int delay_s = 5) {
        if (char_find_src)  { g_source_remove(char_find_src);  char_find_src  = 0; }
        if (reconnect_src)  { g_source_remove(reconnect_src);  reconnect_src  = 0; }
        if (poll_src)       { g_source_remove(poll_src);       poll_src       = 0; }
        if (scan_stuck_src) { g_source_remove(scan_stuck_src); scan_stuck_src = 0; }
        if (notify_char) {
            if (char_sig) { g_signal_handler_disconnect(notify_char, char_sig); char_sig = 0; }
            g_object_unref(notify_char); notify_char = nullptr;
        }
        if (write_char)   { g_object_unref(write_char);   write_char   = nullptr; }
        if (device_proxy) {
            if (device_sig) { g_signal_handler_disconnect(device_proxy, device_sig); device_sig = 0; }
            g_object_unref(device_proxy); device_proxy = nullptr;
        }
        device_path.clear();
        { std::lock_guard<std::mutex> lk(state_mu); device_address.clear(); device_name.clear(); }
        // Always land in DISCONNECTED so try_find_device() and start_discovery() can re-enter
        // (they guard on DISCONNECTED/ERROR/SCANNING; DISCOVERING/CONNECTING would deadlock them)
        set_state(BleState::DISCONNECTED);
        LOG_INFO("BLE: reconnect in %d s", delay_s);
        GSource* src = g_timeout_source_new_seconds(delay_s);
        g_source_set_callback(src, cb_reconnect, this, nullptr);
        reconnect_src = g_source_attach(src, gctx);
        g_source_unref(src);
    }

    static void cb_obj_mgr_ready(GObject*, GAsyncResult* res, gpointer ud) {
        auto* self = static_cast<Impl*>(ud);
        GError* err = nullptr;
        self->obj_mgr = g_dbus_object_manager_client_new_for_bus_finish(res, &err);
        if (err) {
            LOG_ERROR("BLE: ObjectManager: %s", err->message);
            g_error_free(err);
            self->set_state(BleState::ERROR, "D-Bus/BlueZ unavailable");
            return;
        }
        g_signal_connect(self->obj_mgr, "object-added", G_CALLBACK(cb_obj_added), self);
        g_signal_connect(self->obj_mgr, "interface-proxy-properties-changed", G_CALLBACK(cb_interface_props_changed), self);
        if (!self->try_find_device()) self->start_discovery();
    }

    // Called whenever a new BlueZ object appears (new BLE device discovered in scan)
    static void cb_obj_added(GDBusObjectManager*, GDBusObject* obj, gpointer ud) {
        auto* self = static_cast<Impl*>(ud);
        if (self->state.load() != BleState::SCANNING) return;

        GDBusInterface* iface = g_dbus_object_get_interface(obj, "org.bluez.Device1");
        if (!iface) return;
        GDBusProxy* proxy = G_DBUS_PROXY(iface);
        const char* path = g_dbus_object_get_object_path(obj);

        // Always report to picker
        self->report_device(proxy, path);
        self->maybe_connect(proxy, path);
        g_object_unref(iface);
    }

    static void cb_interface_props_changed(GDBusObjectManagerClient*, GDBusObjectProxy* obj,
                                           GDBusProxy* proxy, GVariant*, GStrv, gpointer ud) {
        auto* self = static_cast<Impl*>(ud);
        if (self->state.load() != BleState::SCANNING) return;

        const char* iface = g_dbus_proxy_get_interface_name(proxy);
        if (iface && strcmp(iface, "org.bluez.Device1") == 0) {
            const char* path = g_dbus_object_get_object_path(G_DBUS_OBJECT(obj));
            self->maybe_connect(proxy, path);
        }
    }

    static void cb_device_props(GDBusProxy*, GVariant* changed, GStrv, gpointer ud) {
        auto* self = static_cast<Impl*>(ud);
        g_autoptr(GVariant) sr = g_variant_lookup_value(changed, "ServicesResolved", G_VARIANT_TYPE_BOOLEAN);
        if (sr && g_variant_get_boolean(sr)) { self->on_services_resolved(); return; }
        g_autoptr(GVariant) conn = g_variant_lookup_value(changed, "Connected", G_VARIANT_TYPE_BOOLEAN);
        if (conn && !g_variant_get_boolean(conn)) {
            LOG_WARN("BLE: disconnected");
            self->set_state(BleState::DISCONNECTED);
            self->schedule_reconnect();
        }
    }

    static void cb_char_props(GDBusProxy*, GVariant* changed, GStrv, gpointer ud) {
        auto* self = static_cast<Impl*>(ud);
        g_autoptr(GVariant) val = g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE("ay"));
        if (!val) return;
        gsize n = 0;
        const uint8_t* d = static_cast<const uint8_t*>(g_variant_get_fixed_array(val, &n, 1));
        LOG_INFO("BLE: notification received, %zu bytes", n);
        self->last_data_time = std::chrono::steady_clock::now();
        if (d && n > 0 && self->data_cb)
            self->data_cb(std::vector<uint8_t>(d, d + n));
    }

    static gboolean cb_scan_stuck(gpointer ud) {
        auto* self = static_cast<Impl*>(ud);
        self->scan_stuck_src = 0;
        if (self->state.load() == BleState::SCANNING) {
            LOG_WARN("BLE: stuck in SCANNING for >%ds — initiating hard reset", SCAN_STUCK_TIMEOUT_S);
            self->hard_reset_device();
        }
        return G_SOURCE_REMOVE;
    }
    static gboolean cb_try_find_chars(gpointer ud) {
        auto* self = static_cast<Impl*>(ud); self->char_find_src = 0; self->do_find_chars(); return G_SOURCE_REMOVE;
    }
    static gboolean cb_poll(gpointer ud) {
        auto* self = static_cast<Impl*>(ud); self->poll_src = 0; self->do_poll(); return G_SOURCE_REMOVE;
    }
    static gboolean cb_reconnect(gpointer ud) {
        auto* self = static_cast<Impl*>(ud); self->reconnect_src = 0;
        if (!self->try_find_device()) self->start_discovery();
        return G_SOURCE_REMOVE;
    }
    static gboolean cb_drain(gpointer ud) {
        static_cast<Impl*>(ud)->drain_write_queue(); return G_SOURCE_REMOVE;
    }

    // Dispatched to GLib thread by connect_to()
    static gboolean cb_do_connect_to(gpointer ud) {
        auto* self = static_cast<Impl*>(ud);
        g_mutex_lock(&self->pending_mu);
        std::string addr = self->pending_connect_addr;
        g_mutex_unlock(&self->pending_mu);

        std::string path;
        {
            std::lock_guard<std::mutex> lk(self->state_mu);
            auto it = self->discovered.find(addr);
            if (it != self->discovered.end()) path = it->second;
        }
        if (!path.empty()) {
            // Stop discovery
            g_autoptr(GDBusProxy) adapter = g_dbus_proxy_new_for_bus_sync(
                G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, nullptr, "org.bluez",
                self->adapter_path.c_str(), "org.bluez.Adapter1", nullptr, nullptr);
            if (adapter)
                g_dbus_proxy_call_sync(adapter, "StopDiscovery",
                    nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
            self->connect_device(path);
        } else {
            LOG_WARN("BLE: connect_to — device %s not in discovered list", addr.c_str());
        }
        return G_SOURCE_REMOVE;
    }
};

BleManager::BleManager(const std::string& target, const std::string& adapter_path,
                       const std::string& svc, const std::string& ntf, const std::string& wr)
    : impl_(std::make_unique<Impl>())
{
    impl_->target       = target;
    impl_->adapter_path = adapter_path;
    impl_->service_uuid = svc;
    impl_->notify_uuid  = ntf;
    impl_->write_uuid   = wr;
    g_mutex_init(&impl_->wq_mu);
    g_cond_init(&impl_->wq_cond);
    g_mutex_init(&impl_->pending_mu);
}

BleManager::~BleManager() {
    stop();
    g_mutex_clear(&impl_->wq_mu);
    g_cond_clear(&impl_->wq_cond);
    g_mutex_clear(&impl_->pending_mu);
}

bool BleManager::start() {
    if (impl_->thread.joinable()) return false;
    impl_->cancel = g_cancellable_new();
    impl_->thread = std::thread(&Impl::gio_main, impl_.get());
    return true;
}

void BleManager::stop() {
    if (impl_->cancel)  { g_cancellable_cancel(impl_->cancel); }
    if (impl_->gloop)   { g_main_loop_quit(impl_->gloop); }
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->cancel)  { g_object_unref(impl_->cancel); impl_->cancel = nullptr; }
}

bool BleManager::send(const std::vector<uint8_t>& data) {
    if (impl_->state.load() != BleState::READY) return false;
    g_mutex_lock(&impl_->wq_mu);
    impl_->write_queue.push_back(data);
    g_mutex_unlock(&impl_->wq_mu);
    if (impl_->gctx) {
        GSource* src = g_idle_source_new();
        g_source_set_callback(src, Impl::cb_drain, impl_.get(), nullptr);
        g_source_attach(src, impl_->gctx);
        g_source_unref(src);
    }
    return true;
}

void BleManager::connect_to(const std::string& device_id) {
    g_mutex_lock(&impl_->pending_mu);
    impl_->pending_connect_addr = device_id;
    g_mutex_unlock(&impl_->pending_mu);

    if (impl_->gctx) {
        GSource* src = g_idle_source_new();
        g_source_set_callback(src, Impl::cb_do_connect_to, impl_.get(), nullptr);
        g_source_attach(src, impl_->gctx);
        g_source_unref(src);
    }
}

void BleManager::set_poll_command(std::vector<uint8_t> cmd) { impl_->poll_command = std::move(cmd); }
void BleManager::set_data_callback(DataCb cb)           { impl_->data_cb      = std::move(cb); }
void BleManager::set_state_callback(StateCb cb)          { impl_->state_cb     = std::move(cb); }
void BleManager::set_discovery_callback(DiscoveryCb cb)  { impl_->discovery_cb = std::move(cb); }
BleState    BleManager::state()          const { return impl_->state.load(); }
std::string BleManager::device_address() const { std::lock_guard<std::mutex> lk(impl_->state_mu); return impl_->device_address; }
std::string BleManager::device_name()    const { std::lock_guard<std::mutex> lk(impl_->state_mu); return impl_->device_name; }
