# limonitor

Battery health and energy analytics dashboard for LiTime / JBD LiFePO4 packs.
Monitors via BLE, integrates with the EpicPowerGate 2 solar/grid charger over
USB serial or remote HTTP, logs to SQLite, and serves a live web dashboard with
a Prometheus metrics endpoint.

---

## Features

### Performance & Stability
- **Hyper-optimized for Raspberry Pi and Apple Silicon (M-series)**: Automatic architecture detection in the build system applies optimal compiler flags for your specific CPU.
- **Low Memory Footprint**: Efficient ring-buffered analytics ensure stable performance even on resource-constrained devices like the RPi Zero 2W.
- **High Reliability**: Uses floating-point stability-focused optimizations for precise battery math.

### Live monitoring
- Pack voltage, current, SoC, power, and time-remaining
- Per-cell voltages with colour-coded imbalance indicator
- BMS protection fault banner (over/undervoltage, over-temperature, overcurrent, short circuit)
- Charger state (Charging / Float / Idle), target voltage, max current, PWM duty
- Solar panel voltage
- Animated Energy Flow diagram (Solar → Charger → Battery → Load)
- SVG history charts for battery voltage/SoC and charger voltage/current/solar

### Analytics
| Card | What it computes |
|---|---|
| **Energy Today** | Wh integrated since midnight — charged, consumed, solar, and net change |
| **Battery Replacement** | Age from purchase date, estimated health (~2.5 %/yr degradation), replacement window |
| **Battery Health** | Usable capacity observed from discharge cycles; falls back to age estimate |
| **Charging Stage** | Bulk / Absorption / Float / Idle inferred from voltage and current trend |
| **Cell Balance** | Max − min cell voltage with Excellent / Good / Warning / Imbalance thresholds |
| **Battery Temperature** | T1 & T2 from BMS NTCs with Normal / Warm / Warning thresholds |
| **Charger Efficiency** | bat_v / input_v ratio across the charge path |
| **Solar Input** | Panel voltage, estimated power, and energy accumulated today |
| **Depth of Discharge** | Today's SoC swing (Low stress / Normal / High stress) |
| **System Load Profile** | Rolling ~1 h avg / peak / idle load watts |

### Radio TX detection
- Detects radio transmissions from net battery current spikes
- Logs start time, duration, peak current, and peak power per event
- 24 h aggregates: event count, total time, duty cycle, energy used, peak power

### Storage & export
- SQLite with WAL mode — battery and charger readings, configurable write throttle
- History pre-loaded into ring buffer on startup
- Prometheus `/metrics` endpoint for Grafana integration

---

## Building

**Dependencies:** CMake ≥ 3.16, SQLite3, ncurses (optional TUI), gio-2.0 (Linux BLE only)

```bash
mkdir build && cd build
cmake ..
make
```

macOS uses CoreBluetooth; Linux uses BlueZ via GIO D-Bus.

---

## Running

```bash
# BLE battery + USB charger on the same host
limonitor -a AA:BB:CC:DD:EE:FF -s /dev/ttyACM0 --purchase-date 2023-01-15

# BLE battery on Mac, charger on a remote Pi
limonitor -a AA:BB:CC:DD:EE:FF --pwrgate-remote ham-pi:8081

# Pi running serial-only (serves charger data for remote poll)
limonitor --no-ble -s /dev/ttyACM0 --daemon -p 8081

# Demo mode (no hardware required)
limonitor --demo
```

The dashboard is served at `http://HOST:8080/` by default.

---

## Configuration

Settings are read from a config file first, then overridden by CLI flags. On startup, any values from the config file are migrated into the SQLite database (only for keys not already set). The app then loads settings from the DB, so the DB is the source of truth after the first run.

**First-run setup:** If no BLE device, serial device, or remote charger is configured, limonitor will prompt for setup:
- **Interactive (TUI):** When not in daemon mode, a terminal wizard asks for device name, serial port, HTTP port, etc.
- **Web UI:** Visit `http://HOST:PORT/` or `http://HOST:PORT/setup` — the setup form appears when no device is configured. After saving, restart limonitor to apply changes.

**API keys:** The weather API key is stored in the DB. Hashing is not suitable because the key must be sent to the external API. Stored with the same permissions as the DB file. For stronger protection, use a system keyring or encrypt with a machine-derived key.

**Config file paths:**
- macOS: `~/Library/Application Support/limonitor/limonitor.conf`
- Linux: `~/.config/limonitor/limonitor.conf`
- Override: `--config FILE`

**Format:** `key = value` — blank lines and `#` comments are ignored.

| Key / Flag | Default | Description |
|---|---|---|
| `device_address` / `-a ADDR` | — | BLE MAC address (takes precedence over name) |
| `device_name` / `-n NAME` | — | BLE device name substring match |
| `adapter_path` / `-i PATH` | `/org/bluez/hci0` | BlueZ adapter (Linux) |
| `http_port` / `-p PORT` | `8080` | HTTP listen port |
| `http_bind` / `-b ADDR` | `0.0.0.0` | HTTP bind address |
| `poll_interval` / `-I SECS` | `5` | BLE poll interval |
| `serial_device` / `-s DEVICE` | — | EpicPowerGate 2 USB serial port |
| `serial_baud` / `--baud N` | `115200` | Serial baud rate |
| `pwrgate_remote` / `--pwrgate-remote HOST:PORT` | — | Poll remote limonitor for charger data |
| `db_path` / `--db PATH` | platform default | SQLite database path |
| `db_interval` / `--db-interval N` | `60` | DB write throttle (seconds; 0 = every update) |
| `battery_purchased` / `--purchase-date DATE` | — | Battery purchase date e.g. `2023-01-15` |
| `rated_capacity_ah` / `--rated-capacity N` | auto | Rated battery capacity in Ah (auto-detected from BMS if not set) |
| `tx_threshold` / `--tx-threshold A` | `1.0` | TX detection threshold in amps |
| `log_file` / `-l FILE` | — | Log to file (rotating, 10 MB) |
| `verbose` / `-v` | `false` | Show DEBUG log messages |
| `daemon` / `--daemon` | `false` | Headless mode (no TUI) |
| `demo` / `--demo` | `false` | Synthetic data mode (no hardware needed) |
| `no_ble` / `--no-ble` | `false` | Skip BLE (serial/HTTP-only node) |

---

## API endpoints

| Endpoint | Description |
|---|---|
| `GET /` | HTML dashboard |
| `GET /api/status` | Full battery snapshot JSON |
| `GET /api/analytics` | All 10 analytics fields JSON |
| `GET /api/cells` | Cell voltages JSON |
| `GET /api/history?n=N` | Battery history ring buffer JSON |
| `GET /api/charger` | Latest charger snapshot JSON |
| `GET /api/charger/history?n=N` | Charger history JSON |
| `GET /api/flow` | Energy flow diagram data JSON |
| `GET /api/tx_events?n=N` | Radio TX event log JSON |
| `GET /metrics` | Prometheus text metrics |

### `/api/analytics` response fields

```json
{
  "energy_charged_today_wh":    410.0,
  "energy_discharged_today_wh": 365.0,
  "solar_energy_today_wh":      120.0,
  "net_energy_today_wh":         45.0,
  "battery_age_years":            2.3,
  "battery_health_pct":          94.0,
  "years_remaining_low":            3,
  "years_remaining_high":           5,
  "battery_replace_warn":       false,
  "charging_stage":           "Bulk",
  "usable_capacity_ah":          96.0,
  "rated_capacity_ah":          100.0,
  "capacity_health_pct":         96.0,
  "cell_delta_mv":                1.0,
  "cell_balance_status":   "Excellent",
  "temp1_c":                     20.0,
  "temp2_c":                     21.0,
  "temp_valid":                  true,
  "temp_status":             "Normal",
  "charger_efficiency":         0.966,
  "efficiency_valid":            true,
  "solar_voltage_v":             18.4,
  "solar_power_w":              120.0,
  "solar_active":                true,
  "depth_of_discharge_pct":      32.0,
  "dod_status":              "Normal",
  "avg_load_watts":              11.0,
  "peak_load_watts":             62.0,
  "idle_load_watts":              8.0
}
```

---

## Supported hardware

| Hardware | Interface | Notes |
|---|---|---|
| LiTime LiFePO4 (L1200 series) | BLE (FFE0/FFE1/FFE2) | Native protocol |
| JBD / Daly BMS | BLE (FF00/FF01/FF02) | JBD binary protocol |
| EpicPowerGate 2 | USB serial (115200 baud) | Solar/grid PWM charger |

---

## Development

Pre-commit hooks run clang-format and a build check:

```bash
pip install -r requirements-dev.txt
pre-commit install
```
