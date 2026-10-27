// BleManager against a fake BlueZ on a private D-Bus (Linux only).
//
// The battery has to be found by a scan whenever BlueZ does not remember it
// (a NixOS node keeps no /var/lib/bluetooth across boots). On 2026-10-27 the
// node's limonitor never saw it appear: its object manager was made with the
// async constructor, GLib ran that in a worker thread, and the
// InterfacesAdded/InterfacesRemoved signals went to the process's default
// main context, which nothing runs. It missed every device a scan found and
// kept every device BlueZ removed.
//
// The fake BlueZ runs in its own thread and main context, and nothing here
// runs the default main context, so a manager listening there hears nothing,
// as on the node. Needs dbus-daemon on PATH; skipped without it.

#ifndef __APPLE__

#include "../../src/ble_manager.hpp"
#include "../test_helpers.hpp"

#include <gio/gio.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

namespace {

const char* ADAPTER = "/org/bluez/hci0";
const char* DEVICE = "/org/bluez/hci0/dev_C8_47_80_19_C1_70";
const char* ADDRESS = "C8:47:80:19:C1:70";
const char* SERVICE = "0000ffe0-0000-1000-8000-00805f9b34fb";

const char* XML =
    "<node>"
    " <interface name='org.freedesktop.DBus.ObjectManager'>"
    "  <method name='GetManagedObjects'><arg type='a{oa{sa{sv}}}' direction='out'/></method>"
    "  <signal name='InterfacesAdded'><arg type='o'/><arg type='a{sa{sv}}'/></signal>"
    "  <signal name='InterfacesRemoved'><arg type='o'/><arg type='as'/></signal>"
    " </interface>"
    " <interface name='org.bluez.Adapter1'>"
    "  <method name='StartDiscovery'/>"
    "  <method name='StopDiscovery'/>"
    "  <method name='RemoveDevice'><arg type='o' direction='in'/></method>"
    "  <method name='SetDiscoveryFilter'><arg type='a{sv}' direction='in'/></method>"
    "  <property name='Discovering' type='b' access='read'/>"
    "  <property name='Powered' type='b' access='readwrite'/>"
    "  <property name='Address' type='s' access='read'/>"
    " </interface>"
    " <interface name='org.bluez.Device1'>"
    "  <method name='Connect'/>"
    "  <method name='Disconnect'/>"
    "  <property name='Address' type='s' access='read'/>"
    "  <property name='Name' type='s' access='read'/>"
    "  <property name='Alias' type='s' access='read'/>"
    "  <property name='Connected' type='b' access='read'/>"
    "  <property name='ServicesResolved' type='b' access='read'/>"
    "  <property name='UUIDs' type='as' access='read'/>"
    "  <property name='RSSI' type='n' access='read'/>"
    " </interface>"
    "</node>";

// BlueZ with one adapter and, maybe, the battery.
struct FakeBluez {
    std::string address;               // the private bus
    bool device_at_start = false;      // BlueZ remembers the battery
    bool add_on_scan = true;           // a scan finds it half a second in
    int connect_failures = 0;          // Connect fails this many times first

    std::atomic<int> scans{0}, connect_calls{0}, connects_ok{0}, removes{0};
    std::atomic<bool> ready{false};

    GDBusNodeInfo* info = nullptr;
    GDBusConnection* conn = nullptr;
    GMainContext* ctx = nullptr;
    GMainLoop* loop = nullptr;
    guint device_id = 0;
    bool discovering = false;
    std::thread thread;

    GVariant* device_props() {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", "Address", g_variant_new_string(ADDRESS));
        g_variant_builder_add(&b, "{sv}", "Name", g_variant_new_string("L-12100BNNA70"));
        g_variant_builder_add(&b, "{sv}", "Alias", g_variant_new_string("L-12100BNNA70"));
        g_variant_builder_add(&b, "{sv}", "Connected", g_variant_new_boolean(FALSE));
        g_variant_builder_add(&b, "{sv}", "ServicesResolved", g_variant_new_boolean(FALSE));
        const char* uuids[] = {SERVICE, nullptr};
        g_variant_builder_add(&b, "{sv}", "UUIDs", g_variant_new_strv(uuids, -1));
        g_variant_builder_add(&b, "{sv}", "RSSI", g_variant_new_int16(-38));
        return g_variant_builder_end(&b);
    }

    GVariant* adapter_props() {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&b, "{sv}", "Discovering", g_variant_new_boolean(discovering));
        g_variant_builder_add(&b, "{sv}", "Powered", g_variant_new_boolean(TRUE));
        g_variant_builder_add(&b, "{sv}", "Address", g_variant_new_string("D8:3A:DD:B1:DA:05"));
        return g_variant_builder_end(&b);
    }

    static GVariant* one_iface(const char* name, GVariant* props) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a{sa{sv}}"));
        g_variant_builder_add(&b, "{s@a{sv}}", name, props);
        return g_variant_builder_end(&b);
    }

    static void on_method(GDBusConnection*, const char*, const char* path, const char* iface,
                          const char* method, GVariant*, GDBusMethodInvocation* inv, gpointer ud) {
        auto* f = static_cast<FakeBluez*>(ud);
        if (!strcmp(method, "GetManagedObjects")) {
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
            g_variant_builder_add(&b, "{o@a{sa{sv}}}", ADAPTER, one_iface("org.bluez.Adapter1", f->adapter_props()));
            if (f->device_id)
                g_variant_builder_add(&b, "{o@a{sa{sv}}}", DEVICE, one_iface("org.bluez.Device1", f->device_props()));
            g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{oa{sa{sv}}})", g_variant_builder_end(&b)));
        } else if (!strcmp(method, "StartDiscovery")) {
            f->discovering = true;
            f->scans++;
            if (f->add_on_scan && !f->device_id) {
                GSource* s = g_timeout_source_new(500);
                g_source_set_callback(s, [](gpointer d) -> gboolean {
                    auto* ff = static_cast<FakeBluez*>(d);
                    if (!ff->device_id) ff->add_device();
                    return G_SOURCE_REMOVE;
                }, f, nullptr);
                g_source_attach(s, f->ctx);
                g_source_unref(s);
            }
            g_dbus_method_invocation_return_value(inv, nullptr);
        } else if (!strcmp(method, "StopDiscovery")) {
            f->discovering = false;
            g_dbus_method_invocation_return_value(inv, nullptr);
        } else if (!strcmp(method, "RemoveDevice")) {
            f->removes++;
            if (f->device_id) f->remove_device();
            g_dbus_method_invocation_return_value(inv, nullptr);
        } else if (!strcmp(method, "Connect") && !strcmp(iface, "org.bluez.Device1") && !strcmp(path, DEVICE)) {
            f->connect_calls++;
            if (f->connect_failures > 0) {
                f->connect_failures--;
                g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.Failed", "le-connection-abort-by-local");
            } else {
                f->connects_ok++;
                g_dbus_method_invocation_return_value(inv, nullptr);
            }
        } else {
            g_dbus_method_invocation_return_value(inv, nullptr);
        }
    }

    static GVariant* on_get(GDBusConnection*, const char*, const char* path, const char* iface,
                            const char* prop, GError**, gpointer ud) {
        auto* f = static_cast<FakeBluez*>(ud);
        GVariant* all = !strcmp(path, ADAPTER) ? f->adapter_props() : f->device_props();
        GVariant* v = g_variant_lookup_value(all, prop, nullptr);
        g_variant_unref(g_variant_ref_sink(all));
        (void)iface;
        return v;
    }

    static gboolean on_set(GDBusConnection*, const char*, const char*, const char*, const char*,
                           GVariant*, GError**, gpointer) {
        return TRUE;    // Powered off and on: accepted, nothing changes
    }

    void register_obj(const char* path, const char* iface, guint* id) {
        static const GDBusInterfaceVTable vt = {on_method, on_get, on_set, {nullptr}};
        GError* err = nullptr;
        *id = g_dbus_connection_register_object(conn, path, g_dbus_node_info_lookup_interface(info, iface),
                                                &vt, this, nullptr, &err);
        if (err) { fprintf(stderr, "fake bluez: %s\n", err->message); g_error_free(err); }
    }

    void add_device() {
        register_obj(DEVICE, "org.bluez.Device1", &device_id);
        g_dbus_connection_emit_signal(conn, nullptr, "/", "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                                      g_variant_new("(o@a{sa{sv}})", DEVICE, one_iface("org.bluez.Device1", device_props())),
                                      nullptr);
    }

    void remove_device() {
        g_dbus_connection_unregister_object(conn, device_id);
        device_id = 0;
        const char* gone[] = {"org.bluez.Device1", nullptr};
        g_dbus_connection_emit_signal(conn, nullptr, "/", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
                                      g_variant_new("(o@as)", DEVICE, g_variant_new_strv(gone, -1)), nullptr);
    }

    void run() {
        ctx = g_main_context_new();
        g_main_context_push_thread_default(ctx);
        GError* err = nullptr;
        info = g_dbus_node_info_new_for_xml(XML, &err);
        conn = g_dbus_connection_new_for_address_sync(
            address.c_str(),
            GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
            nullptr, nullptr, &err);
        if (err) { fprintf(stderr, "fake bluez: %s\n", err->message); g_error_free(err); return; }
        GVariant* r = g_dbus_connection_call_sync(conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                  "org.freedesktop.DBus", "RequestName",
                                                  g_variant_new("(su)", "org.bluez", 4u), nullptr,
                                                  G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
        if (r) g_variant_unref(r);
        guint root = 0, adapter = 0;
        register_obj("/", "org.freedesktop.DBus.ObjectManager", &root);
        register_obj(ADAPTER, "org.bluez.Adapter1", &adapter);
        if (device_at_start) register_obj(DEVICE, "org.bluez.Device1", &device_id);
        loop = g_main_loop_new(ctx, FALSE);
        ready = true;
        g_main_loop_run(loop);
        g_dbus_connection_close_sync(conn, nullptr, nullptr);
        g_object_unref(conn);
        g_main_loop_unref(loop);
        g_dbus_node_info_unref(info);
        g_main_context_pop_thread_default(ctx);
        g_main_context_unref(ctx);
    }

    void start() {
        thread = std::thread(&FakeBluez::run, this);
        while (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    void stop() {
        g_main_context_invoke(ctx, [](gpointer l) -> gboolean { g_main_loop_quit(static_cast<GMainLoop*>(l)); return G_SOURCE_REMOVE; }, loop);
        thread.join();
    }
};

bool wait_for(const std::function<bool()>& done, int seconds) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return done();
}

// One private bus for the process: GLib keeps its system bus connection.
GTestDBus* g_bus = nullptr;

bool bus_up() {
    if (g_bus) return true;
    gchar* daemon = g_find_program_in_path("dbus-daemon");
    if (!daemon) {
        printf("SKIP: BLE against a fake BlueZ (no dbus-daemon on PATH)\n");
        return false;
    }
    g_free(daemon);
    g_bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(g_bus);
    setenv("DBUS_SYSTEM_BUS_ADDRESS", g_test_dbus_get_bus_address(g_bus), 1);
    return true;
}

BleManager* manager() {
    return new BleManager(ADDRESS, ADAPTER, SERVICE,
                          "0000ffe1-0000-1000-8000-00805f9b34fb", "0000ffe2-0000-1000-8000-00805f9b34fb");
}

}  // namespace

// BlueZ does not know the battery yet: the scan finds it, and limonitor
// connects. On the node it never did.
void test_ble_connects_to_a_device_a_scan_finds() {
    if (!bus_up()) return;
    FakeBluez f;
    f.address = g_test_dbus_get_bus_address(g_bus);
    f.start();
    BleManager* b = manager();
    b->start();
    ASSERT(wait_for([&] { return f.scans > 0; }, 10), "limonitor starts a scan when BlueZ does not know the battery");
    ASSERT(wait_for([&] { return f.connect_calls > 0; }, 10),
           "limonitor connects to the battery the scan found (InterfacesAdded heard)");
    b->stop();
    delete b;
    f.stop();
}

// BlueZ knows the battery, the connection fails three times, limonitor
// removes it from BlueZ and scans again: it must forget the removed device
// and connect when the scan finds the battery again. On the node it kept
// calling Connect on the removed object ("connecting to  ()").
void test_ble_forgets_a_removed_device_and_finds_it_again() {
    if (!bus_up()) return;
    FakeBluez f;
    f.address = g_test_dbus_get_bus_address(g_bus);
    f.device_at_start = true;
    f.connect_failures = 3;
    f.start();
    BleManager* b = manager();
    b->start();
    ASSERT(wait_for([&] { return f.removes > 0; }, 30), "after three failed connects limonitor removes the device");
    ASSERT(wait_for([&] { return f.connects_ok > 0; }, 20),
           "limonitor scans again and connects to the battery once found (InterfacesRemoved and Added heard)");
    b->stop();
    delete b;
    f.stop();
}

#endif
