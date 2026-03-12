// macOS CoreBluetooth backend

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#include "ble_manager.hpp"
#include "logger.hpp"
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern int g_poll_interval_s;

@class BLEDelegate;

struct BleManager::Impl {
    // Config
    std::string target;
    std::string service_uuid;
    std::string notify_uuid;
    std::string write_uuid;

    // Callbacks
    BleManager::DataCb      data_cb;
    BleManager::StateCb     state_cb;
    BleManager::DiscoveryCb discovery_cb;

    // State
    std::atomic<BleState> state{BleState::DISCONNECTED};
    mutable std::mutex    mu;
    std::string           device_address;
    std::string           device_name;

    // CoreBluetooth — only touched on ble_queue
    dispatch_queue_t   ble_queue{nullptr};
    dispatch_source_t  poll_timer{nullptr};
    dispatch_source_t  reconnect_timer{nullptr};
    CBCentralManager*  central{nullptr};
    CBPeripheral*      peripheral{nullptr};
    CBCharacteristic*  notify_char{nullptr};
    CBCharacteristic*  write_char{nullptr};
    BLEDelegate*       delegate{nullptr};
    std::chrono::steady_clock::time_point last_data_time;

    // Discovered peripherals (id -> retained CBPeripheral*)
    std::map<std::string, CBPeripheral*> discovered;

    // Poll command (set before start via set_poll_command)
    std::vector<uint8_t> poll_command;

    // Determined at characteristic-discovery time
    CBCharacteristicWriteType write_type{CBCharacteristicWriteWithoutResponse};

    // Write queue
    std::mutex                        wq_mu;
    std::vector<std::vector<uint8_t>> write_queue;

    void set_state(BleState s, const std::string& msg = "") {
        BleState old = state.exchange(s);
        if (s == BleState::READY) last_data_time = std::chrono::steady_clock::now();
        if (old != s || !msg.empty()) {
            LOG_INFO("BLE: %s -> %s%s%s",
                ble_state_str(old), ble_state_str(s),
                msg.empty() ? "" : " — ", msg.c_str());
        }
        if (state_cb) state_cb(s, msg);
    }

    static std::string peripheral_id(CBPeripheral* p) {
        return [p.identifier.UUIDString UTF8String];
    }

    bool matches_target(CBPeripheral* p, NSDictionary* adv) const {
        if (target.empty()) return adv_has_service(adv);
        NSString* t = [NSString stringWithUTF8String:target.c_str()];
        if (p.name) {
            NSRange r = [p.name rangeOfString:t options:NSCaseInsensitiveSearch];
            if (r.location != NSNotFound) return true;
        }
        return [p.identifier.UUIDString caseInsensitiveCompare:t] == NSOrderedSame;
    }

    bool adv_has_service(NSDictionary* adv) const {
        NSArray* uuids = adv[CBAdvertisementDataServiceUUIDsKey];
        if (!uuids) return false;
        CBUUID* target = [CBUUID UUIDWithString:[NSString stringWithUTF8String:service_uuid.c_str()]];
        for (CBUUID* u in uuids) {
            if ([u isEqual:target]) return true;
        }
        return false;
    }

    // Called on ble_queue when a peripheral is discovered
    void on_discovered(CBPeripheral* p, NSDictionary* adv, int rssi) {
        std::string pid = peripheral_id(p);

        // Retain and store (update if already seen)
        auto it = discovered.find(pid);
        if (it == discovered.end()) {
            discovered[pid] = [p retain];
        }

        // Fire discovery callback (visible to TUI device picker)
        bool is_target = matches_target(p, adv);
        if (discovery_cb) {
            DiscoveredDevice dev;
            dev.id   = pid;
            dev.name = p.name ? [p.name UTF8String] : "";
            dev.rssi = rssi;
            dev.has_target_service = adv_has_service(adv);
            discovery_cb(dev);
        }

        if (state.load() == BleState::SCANNING && is_target) {
            LOG_INFO("BLE: target found — connecting");
            do_connect(p);
        }
    }

    void do_connect(CBPeripheral* p) {
        // Cancel and release any DIFFERENT previous peripheral.
        // Do not cancel if it's the same object to avoid a spurious
        // didDisconnectPeripheral callback arriving for our new connection.
        if (peripheral && peripheral != p) {
            [central cancelPeripheralConnection:peripheral];
            [peripheral release];
            peripheral = nil;
        }
        if (!peripheral) {
            peripheral = [p retain];
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            device_address = peripheral_id(peripheral);
            device_name    = peripheral.name ? [peripheral.name UTF8String] : "";
        }
        peripheral.delegate = (id<CBPeripheralDelegate>)delegate;
        [central stopScan];
        LOG_INFO("BLE: connecting to %s (%s)", device_address.c_str(), device_name.c_str());
        set_state(BleState::CONNECTING);
        [central connectPeripheral:peripheral options:nil];
    }

    void connect_to(const std::string& id) {
        // Dispatched to ble_queue by the public method.
        // Always prefer a fresh system-managed peripheral reference via
        // retrievePeripheralsWithIdentifiers: — this avoids stale scan-time objects.
        NSString* uuid_str = [NSString stringWithUTF8String:id.c_str()];
        NSUUID* uuid = [[NSUUID alloc] initWithUUIDString:uuid_str];
        NSArray* known = uuid ? [central retrievePeripheralsWithIdentifiers:@[uuid]] : @[];
        [uuid release];
        if (known.count > 0) {
            do_connect(known[0]);
            return;
        }
        // Fall back to scan-time cache
        auto it = discovered.find(id);
        if (it != discovered.end()) {
            do_connect(it->second);
        } else {
            LOG_WARN("BLE: connect_to — device %s not found, rescanning", id.c_str());
            start_scan();
        }
    }

    void on_connected() {
        set_state(BleState::DISCOVERING);
        NSString* svcStr = [NSString stringWithUTF8String:service_uuid.c_str()];
        [peripheral discoverServices:@[[CBUUID UUIDWithString:svcStr]]];
    }

    void on_services_discovered(CBService* svc) {
        NSString* nUUID = [NSString stringWithUTF8String:notify_uuid.c_str()];
        NSString* wUUID = [NSString stringWithUTF8String:write_uuid.c_str()];
        [peripheral discoverCharacteristics:@[[CBUUID UUIDWithString:nUUID],
                                              [CBUUID UUIDWithString:wUUID]]
                                 forService:svc];
    }

    void on_characteristics_discovered(CBService* svc) {
        // Use CBUUID objects for comparison — CoreBluetooth normalises 128-bit
        // BT-SIG UUIDs to their 16-bit short form, so string compare fails.
        CBUUID* nCBUUID = [CBUUID UUIDWithString:[NSString stringWithUTF8String:notify_uuid.c_str()]];
        CBUUID* wCBUUID = [CBUUID UUIDWithString:[NSString stringWithUTF8String:write_uuid.c_str()]];
        for (CBCharacteristic* ch in svc.characteristics) {
            if ([ch.UUID isEqual:nCBUUID]) notify_char = ch;
            else if ([ch.UUID isEqual:wCBUUID]) write_char = ch;
        }
        if (notify_char && write_char) {
            // Prefer write-with-response if the characteristic supports it.
            write_type = (write_char.properties & CBCharacteristicPropertyWrite)
                         ? CBCharacteristicWriteWithResponse
                         : CBCharacteristicWriteWithoutResponse;
            LOG_INFO("BLE: using write %s",
                     write_type == CBCharacteristicWriteWithResponse ? "with-response" : "without-response");
            [peripheral setNotifyValue:YES forCharacteristic:notify_char];
            LOG_INFO("BLE: ready");
            set_state(BleState::READY);
            schedule_poll();
        } else {
            LOG_ERROR("BLE: characteristics not found (notify=%s write=%s)",
                      notify_char ? "ok" : "missing", write_char ? "ok" : "missing");
            schedule_reconnect(15);
        }
    }

    void on_notification(CBCharacteristic* ch) {
        NSData* d = ch.value;
        last_data_time = std::chrono::steady_clock::now();
        if (!d || d.length == 0 || !data_cb) return;
        const uint8_t* p = static_cast<const uint8_t*>(d.bytes);
        data_cb(std::vector<uint8_t>(p, p + d.length));
    }

    void on_disconnected(NSError* err) {
        if (err) {
            LOG_WARN("BLE: disconnected (code %ld): %s",
                     (long)err.code, [err.localizedDescription UTF8String]);
        } else {
            LOG_INFO("BLE: disconnected");
        }
        notify_char = nil; write_char = nil;
        if (peripheral) { [peripheral release]; peripheral = nil; }
        set_state(BleState::DISCONNECTED);
        schedule_reconnect();
    }

    void start_scan() {
        set_state(BleState::SCANNING);
        // Scan without service filter so we see all BLE devices for the picker.
        // (A service filter would hide devices that don't advertise the UUID.)
        [central scanForPeripheralsWithServices:nil
                                        options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @NO}];
        LOG_INFO("BLE: scanning for devices…");
    }

    void schedule_poll() {
        cancel_timer(poll_timer);
        poll_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, ble_queue);
        dispatch_source_set_timer(poll_timer,
            dispatch_time(DISPATCH_TIME_NOW, 0),
            static_cast<uint64_t>(g_poll_interval_s) * NSEC_PER_SEC,
            NSEC_PER_SEC / 2);
        dispatch_source_set_event_handler(poll_timer, ^{ do_poll(); });
        dispatch_resume(poll_timer);
    }

    void do_poll() {
        if (state.load() != BleState::READY || !write_char) return;

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_data_time).count() > 30) {
            LOG_WARN("BLE: data timeout (30s) — reconnecting");
            on_disconnected(nil);
            return;
        }

        if (!poll_command.empty()) {
            NSData* d = [NSData dataWithBytes:poll_command.data() length:poll_command.size()];
            [peripheral writeValue:d forCharacteristic:write_char type:write_type];
        }
        // Drain user-queued writes
        std::lock_guard<std::mutex> lk(wq_mu);
        for (const auto& pkt : write_queue) {
            NSData* d = [NSData dataWithBytes:pkt.data() length:pkt.size()];
            [peripheral writeValue:d forCharacteristic:write_char type:write_type];
        }
        write_queue.clear();
    }

    void schedule_reconnect(int delay_s = 5) {
        cancel_timer(poll_timer);
        notify_char = nil; write_char = nil;
        cancel_timer(reconnect_timer);
        LOG_INFO("BLE: reconnect in %d s", delay_s);
        reconnect_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, ble_queue);
        dispatch_source_set_timer(reconnect_timer,
            dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(delay_s) * NSEC_PER_SEC),
            DISPATCH_TIME_FOREVER, NSEC_PER_SEC);
        dispatch_source_set_event_handler(reconnect_timer, ^{
            cancel_timer(reconnect_timer);
            start_scan();
        });
        dispatch_resume(reconnect_timer);
    }

    static void cancel_timer(dispatch_source_t& t) {
        if (t) { dispatch_source_cancel(t); dispatch_release(t); t = nullptr; }
    }

    ~Impl() {
        for (auto& [id, p] : discovered) [p release];
        discovered.clear();
    }
};

@interface BLEDelegate : NSObject<CBCentralManagerDelegate, CBPeripheralDelegate>
@property (assign, nonatomic) BleManager::Impl* impl;
@end

@implementation BLEDelegate

- (void)centralManagerDidUpdateState:(CBCentralManager*)cm {
    switch (cm.state) {
        case CBManagerStatePoweredOn:
            LOG_INFO("BLE: Bluetooth adapter powered on");
            _impl->start_scan();
            break;
        case CBManagerStatePoweredOff:
            LOG_WARN("BLE: Bluetooth is powered off");
            _impl->set_state(BleState::ERROR, "Bluetooth powered off");
            break;
        case CBManagerStateUnauthorized:
            LOG_ERROR("BLE: Bluetooth access unauthorized — System Settings > Privacy > Bluetooth");
            _impl->set_state(BleState::ERROR, "Bluetooth unauthorized");
            break;
        case CBManagerStateUnsupported:
            LOG_ERROR("BLE: Bluetooth LE not supported");
            _impl->set_state(BleState::ERROR, "BLE unsupported");
            break;
        default: break;
    }
}

- (void)centralManager:(CBCentralManager*)cm
 didDiscoverPeripheral:(CBPeripheral*)p
     advertisementData:(NSDictionary*)adv
                  RSSI:(NSNumber*)RSSI {
    (void)cm;
    _impl->on_discovered(p, adv, [RSSI intValue]);
}

- (void)centralManager:(CBCentralManager*)cm didConnectPeripheral:(CBPeripheral*)p {
    (void)cm; (void)p;
    _impl->on_connected();
}

- (void)centralManager:(CBCentralManager*)cm
didFailToConnectPeripheral:(CBPeripheral*)p error:(NSError*)err {
    (void)cm;
    if (p != _impl->peripheral) return; // stale callback for a superseded peripheral
    if (err) {
        LOG_WARN("BLE: connection failed (code %ld): %s",
                 (long)err.code, [err.localizedDescription UTF8String]);
    } else {
        LOG_WARN("BLE: connection failed (no error — device may be busy or out of range)");
    }
    _impl->on_disconnected(err);
}

- (void)centralManager:(CBCentralManager*)cm
didDisconnectPeripheral:(CBPeripheral*)p error:(NSError*)err {
    (void)cm;
    if (p != _impl->peripheral) return; // stale callback for a superseded peripheral
    _impl->on_disconnected(err);
}

- (void)peripheral:(CBPeripheral*)p didDiscoverServices:(NSError*)err {
    if (err) {
        LOG_WARN("BLE: discoverServices failed: %s", [err.localizedDescription UTF8String]);
        _impl->schedule_reconnect(15);
        return;
    }
    if (p.services.count == 0) {
        LOG_WARN("BLE: target service not found on device — wrong UUID? "
                 "(expected %s)", _impl->service_uuid.c_str());
        _impl->schedule_reconnect(15);
        return;
    }
    for (CBService* svc in p.services) _impl->on_services_discovered(svc);
}

- (void)peripheral:(CBPeripheral*)p
didDiscoverCharacteristicsForService:(CBService*)svc error:(NSError*)err {
    (void)p;
    if (err) { LOG_WARN("BLE: discoverChars: %s", [err.localizedDescription UTF8String]); return; }
    _impl->on_characteristics_discovered(svc);
}

- (void)peripheral:(CBPeripheral*)p
didUpdateValueForCharacteristic:(CBCharacteristic*)ch error:(NSError*)err {
    (void)p;
    if (err) { LOG_WARN("BLE: value: %s", [err.localizedDescription UTF8String]); return; }
    _impl->on_notification(ch);
}

- (void)peripheral:(CBPeripheral*)p
didUpdateNotificationStateForCharacteristic:(CBCharacteristic*)ch error:(NSError*)err {
    (void)p; (void)ch;
    if (err) LOG_WARN("BLE: setNotify: %s", [err.localizedDescription UTF8String]);
}

- (void)peripheral:(CBPeripheral*)p
didWriteValueForCharacteristic:(CBCharacteristic*)ch error:(NSError*)err {
    (void)p; (void)ch;
    if (err) LOG_WARN("BLE: write: %s", [err.localizedDescription UTF8String]);
}

@end

BleManager::BleManager(const std::string& target, const std::string& /*adapter_path*/,
                       const std::string& svc, const std::string& ntf, const std::string& wr)
    : impl_(std::make_unique<Impl>())
{
    impl_->target       = target;
    impl_->service_uuid = svc;
    impl_->notify_uuid  = ntf;
    impl_->write_uuid   = wr;
}
BleManager::~BleManager() { stop(); }

bool BleManager::start() {
    impl_->ble_queue = dispatch_queue_create("com.limonitor.ble", DISPATCH_QUEUE_SERIAL);
    impl_->delegate  = [[BLEDelegate alloc] init];
    impl_->delegate.impl = impl_.get();
    impl_->central = [[CBCentralManager alloc]
        initWithDelegate:impl_->delegate
                   queue:impl_->ble_queue
                 options:@{CBCentralManagerOptionShowPowerAlertKey: @YES}];
    return true;
}

void BleManager::stop() {
    if (!impl_->ble_queue) return;
    dispatch_sync(impl_->ble_queue, ^{
        Impl::cancel_timer(impl_->poll_timer);
        Impl::cancel_timer(impl_->reconnect_timer);
        if (impl_->peripheral) {
            [impl_->central cancelPeripheralConnection:impl_->peripheral];
            [impl_->peripheral release]; impl_->peripheral = nil;
        }
        [impl_->central stopScan];
        [impl_->central release];  impl_->central  = nil;
        [impl_->delegate release]; impl_->delegate = nil;
    });
    dispatch_release(impl_->ble_queue);
    impl_->ble_queue = nullptr;
}

bool BleManager::send(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lk(impl_->wq_mu);
    impl_->write_queue.push_back(data);
    return true;
}

void BleManager::connect_to(const std::string& device_id) {
    if (!impl_->ble_queue) return;
    std::string id_copy = device_id;
    dispatch_async(impl_->ble_queue, ^{
        impl_->connect_to(id_copy);
    });
}

void BleManager::set_poll_command(std::vector<uint8_t> cmd) { impl_->poll_command = std::move(cmd); }
void BleManager::set_data_callback(DataCb cb)           { impl_->data_cb      = std::move(cb); }
void BleManager::set_state_callback(StateCb cb)          { impl_->state_cb     = std::move(cb); }
void BleManager::set_discovery_callback(DiscoveryCb cb)  { impl_->discovery_cb = std::move(cb); }
BleState    BleManager::state()          const { return impl_->state.load(); }
std::string BleManager::device_address() const { std::lock_guard<std::mutex> lk(impl_->mu); return impl_->device_address; }
std::string BleManager::device_name()    const { std::lock_guard<std::mutex> lk(impl_->mu); return impl_->device_name; }
