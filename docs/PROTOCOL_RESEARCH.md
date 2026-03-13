# Protocol Research — LiTime BLE & Epic PWRgate USB Serial

Reverse-engineered 2026-03-12/13 via live device probing.

---

## 1. LiTime L-12100BNNA70 — BLE Protocol

### Device identity

| Field          | Value                                    |
|----------------|------------------------------------------|
| Model          | L-12100BNNA70 (100 Ah LiFePO4)          |
| BLE chip       | Beken BK343x (BEKEN SAS)                 |
| BLE FW         | 6.1.2                                    |
| BMS app FW     | 6.3.0                                    |
| BLE MAC        | C8:47:80:19:C1:70 (ham-pi BlueZ)         |
| BLE address type | Random static                          |

### GATT service map

| UUID (short) | Full UUID                                    | Properties            | Purpose                |
|--------------|----------------------------------------------|-----------------------|------------------------|
| FFE0         | 0000ffe0-0000-1000-8000-00805f9b34fb         | (service)             | BMS data service       |
| FFE1         | 0000ffe1-0000-1000-8000-00805f9b34fb         | Notify                | Device → host data     |
| FFE2         | 0000ffe2-0000-1000-8000-00805f9b34fb         | Write, WriteNoResp    | Host → device commands |
| FFE3         | 0000ffe3-0000-1000-8000-00805f9b34fb         | Read, Write, Notify   | Beken AT config channel|

Only one BMS command is supported. JBD protocol commands (DD A5 …) return an error response.

### Command / response framing

**Request** (8 bytes, write to FFE2):

```
00 00 04 01 13 55 AA 17
```

- Bytes 0–1: frame start (fixed)
- Byte 2: payload length (0x04)
- Byte 3: command (0x01 = telemetry)
- Byte 4: argument (0x13)
- Bytes 5–6: magic 55 AA
- Byte 7: checksum = sum(bytes[2..6]) & 0xFF = 0x17

**Response** (105 bytes, single BLE notification on FFE1):

Header: `00 00 65 01 93 55 AA 00`

| Offset  | Type      | Field                          | Notes                                              |
|---------|-----------|--------------------------------|----------------------------------------------------|
| 0–7     | —         | Header                         | `00 00 65 01 93 55 AA 00`                          |
| 8–11    | uint32 LE | Instantaneous voltage (mV)     | Fluctuates; prefer offset 12 for monitoring        |
| 12–15   | uint32 LE | Filtered total voltage (mV)    | Use for display/logging                            |
| 16–47   | 16×uint16 | Cell voltages (mV)             | Zero = slot not populated; 4S → cells 0–3          |
| 48–51   | int32 LE  | Current (mA)                   | LiTime sign: + = charging; **negated in parser** → + = discharging |
| 52–53   | int16 LE  | Cell temperature (°C)          | Direct value, no scaling                           |
| 54–55   | int16 LE  | BMS temperature (°C)           | Direct value, no scaling                           |
| 56–75   | —         | Reserved (zeros)               |                                                    |
| 76–79   | uint32 LE | Protection status bitmask      | See table below                                    |
| 80–87   | —         | Reserved (zeros)               |                                                    |
| 88–89   | uint16 LE | Battery state flags            | Bit 0x0004 = charge FET explicitly disabled        |
| 90–91   | uint16 LE | BMS SoC % (0–100)              | Overrides computed rem/nom ratio when > 0          |
| 92–95   | uint32 LE | Unknown constant               | Observed: 100 (likely rated Ah capacity)           |
| 96–99   | uint32 LE | Cycle count                    |                                                    |
| 100–103 | uint32 LE | Unknown constant               | Observed: 72                                       |
| 104     | uint8     | Checksum                       |                                                    |

**Protection status bitmask (offset 76)**:

| Bit        | Meaning                   |
|------------|---------------------------|
| 0x00000004 | Pack overvoltage          |
| 0x00000020 | Pack undervoltage         |
| 0x00000040 | Charge overcurrent        |
| 0x00000080 | Discharge overcurrent     |
| 0x00000100 | Charge overtemp           |
| 0x00000200 | Discharge overtemp        |
| 0x00000400 | Charge undertemp          |
| 0x00000800 | Discharge undertemp       |
| 0x00004000 | Short circuit             |

### FFE3 — Beken AT config channel

FFE3 accepts ASCII AT commands (write `AT+CMD\r\n`, read response from FFE3 notify or read).

| Command              | Response              | Notes                                 |
|----------------------|-----------------------|---------------------------------------|
| `AT+NAME?\r\n`       | `+NAME=L-12100BNNA70` | Read device name                      |
| `AT+BAUD?\r\n`       | `+BAUD=4`             | 4 = 115200 bps (internal UART rate)   |
| `AT+NAME=xxx\r\n`    | `+ER` (but applied)   | **Silently renames device** despite error response |
| `AT+VER?\r\n`        | `+ER`                 | Not supported                         |
| (other AT commands)  | `+ER`                 | Not supported                         |

**Warning**: `AT+NAME=<value>` applies the rename even though it returns `+ER`. Do not probe this field with non-original values.

### BlueZ workflow (Linux / ham-pi)

```bash
# Trust and pair once
bluetoothctl power on
bluetoothctl scan on          # wait for C8:47:80:19:C1:70
bluetoothctl trust C8:47:80:19:C1:70
bluetoothctl pair  C8:47:80:19:C1:70
bluetoothctl connect C8:47:80:19:C1:70

# limonitor runtime
./limonitor -a C8:47:80:19:C1:70
```

BlueZ connects on startup if the device is trusted and in range. The device must not already be connected when using bleak for scanning; disconnect first if needed.

---

## 2. Epic PWRgate R2 — USB CDC Serial Protocol

### Device identity

| Field        | Value                                          |
|--------------|------------------------------------------------|
| Model        | Epic PWRgate PG40S (West Mountain Radio)       |
| FW version   | 1.34 (observed)                                |
| Interface    | USB CDC ACM (/dev/ttyACM0, 115200 8N1)         |
| Poll rate    | ~1 Hz continuous telemetry                     |
| USB power    | Stays alive on battery — telemetry continues even when PS is absent |

### Telemetry line format

The firmware emits two types of lines at ~1 Hz:

**Status line** (always present):
```
<State> PS=13.78  Sol=0.00  Bat=13.08V,  5.36A  Min=42  P=742  adc=254
```

**Extended line** (`g` command response, may be inline or on a second line):
```
TargetV=14.40  TargetI=40.00  Stop=2.00  Temp=74  PSS=1
```

### State tokens

The state token is the leading word(s) on every status line. Two-word states must be matched before falling back to single-word extraction.

| State token | Meaning                                                  |
|-------------|----------------------------------------------------------|
| `Chrg Off`  | PS present; charger intentionally idle (bat full, timer) |
| `Charging`  | CC-CV charge cycle active (PSS=1, PWM ~600–970)          |
| `Charged`   | Taper complete; current dropped to ≤ stop_a (PSS=0)     |
| `PS Off`    | PS input absent (PS=0.00V); telemetry continues on battery |

### Field reference

| Field      | Format   | Meaning                                                         |
|------------|----------|-----------------------------------------------------------------|
| `PS=`      | float V  | Power-supply input voltage; 0.00 when PS absent                |
| `Sol=`     | float V  | Solar panel input voltage; 0.00 if no panel connected          |
| `Bat=`     | float V  | Battery voltage (charger-measured)                             |
| `,` (after Bat=) | float A | Charge current; positive = charging into battery          |
| `Min=`     | int      | State timer: **Chrg Off** = wall-clock MM (minutes of hour); **Charging/Charged** = elapsed minutes; **PS Off** = elapsed minutes since PS lost |
| `P=`       | int      | PWM duty counter; 0 = off; ~600–970 = actively charging        |
| `adc=`     | int      | Current-sense ADC (~47 counts/A); 0 when not charging; absent/0 when PS absent |
| `TargetV=` | float V  | Absorption (target) voltage                                    |
| `TargetI=` | float A  | Maximum charge current                                         |
| `Stop=`    | float A  | Tail-current threshold; charge terminates when bat_a ≤ stop_a |
| `Temp=`    | int °F   | Temperature sensor (~74°F at room temp)                        |
| `PSS=`     | int      | Charge-cycle active flag: 1 = CC-CV running; 0 = idle/charged; **absent** when PS Off |

### Charger state machine

```
         PS removed                      PS restored
         (PS=0.00V)                      (PS>0.5V)
              │                               │
   ┌──────────▼──────────┐       ┌────────────┴──────────┐
   │       PS Off        │       │        Chrg Off        │
   │  (PS=0.00, PSS=—)   │       │  (PSS=0, PWM=0)        │
   └─────────────────────┘       └────────────┬──────────┘
                                              │ charge triggered
                                              ▼
                                  ┌──────────────────────┐
                                  │       Charging        │
                                  │  (PSS=1, PWM 600-970) │
                                  └──────────┬───────────┘
                                             │ bat_a ≤ stop_a
                                             ▼
                                  ┌──────────────────────┐
                                  │       Charged         │
                                  │  (PSS=0, PWM~0)       │
                                  └──────────────────────┘
```

### Parser implementation notes

- `extract_state()` must check two-word prefixes (`"Chrg Off"`, `"PS Off"`) before single-word extraction — `first_word()` or `split(' ')[0]` is insufficient.
- When no state token is present (line starts with a key=value field), infer from measurements:
  - `bat_a > 0.05A` → Charging
  - `ps_v < 0.5V` → PS Off
  - otherwise → Chrg Off
- The `g` command extended telemetry may arrive inline in s1 or as a separate line s2; check both.

### Example telemetry

**Charging**:
```
Charging PS=13.78  Sol=0.00  Bat=13.08V,  5.36A  Min=42  P=742  adc=254
TargetV=14.40  TargetI=40.00  Stop=2.00  Temp=74  PSS=1
```

**Charged**:
```
Charged PS=13.80  Sol=0.00  Bat=14.38V,  1.89A  Min=3  P=600  adc=89
TargetV=14.40  TargetI=40.00  Stop=2.00  Temp=74  PSS=0
```

**Chrg Off**:
```
Chrg Off PS=13.79  Sol=0.00  Bat=13.45V,  0.00A  Min=23  P=0  adc=0
TargetV=14.40  TargetI=40.00  Stop=2.00  Temp=74  PSS=0
```

**PS Off**:
```
PS Off PS=0.00  Sol=0.00  Bat=13.20V,  0.00A  Min=5  P=0
TargetV=14.40  TargetI=40.00  Stop=2.00  Temp=74
```
(Note: `adc=` and `PSS=` absent in PS Off state)

---

## Source files

| File                         | Purpose                                    |
|------------------------------|--------------------------------------------|
| `src/litime_protocol.hpp`    | BLE request/response declarations          |
| `src/litime_protocol.cpp`    | Parser: 105-byte response → BatterySnapshot|
| `src/pwrgate.hpp`            | PwrGateSnapshot struct + field docs        |
| `src/pwrgate.cpp`            | extract_state(), parse() implementation    |
| `src/battery_data.hpp`       | BatterySnapshot struct, ProtectionStatus   |
