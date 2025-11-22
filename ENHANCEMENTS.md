# Enhancements

## Shipped

- [x] Config file (`key = value`, CLI overrides)
- [x] SQLite persistence with WAL mode and write throttle
- [x] Pre-populate history ring buffer from DB on startup
- [x] Rotating file logger
- [x] Daemon mode (`--daemon`, no TUI)
- [x] Remote PwrGate client (`--pwrgate-remote HOST:PORT`)
- [x] JBD BMS protocol support alongside LiTime
- [x] Time-range picker (30m / 1h / 4h / 24h) in dashboard
- [x] Animated Energy Flow diagram (Solar → Charger → Battery → Load)
- [x] Radio TX event detection and 24 h aggregates
- [x] Prometheus `/metrics` endpoint
- [x] Light / dark theme toggle with localStorage persistence
- [x] Demo mode (synthetic data, no hardware required)
- [x] Battery purchase date (`--purchase-date`) shown in dashboard
- [x] Analytics engine — 10 computed metrics updated server-side each poll:
  - Daily energy accounting (charged / consumed / solar / net, midnight reset)
  - Battery replacement estimator (age, health, replacement window)
  - Battery health from observed discharge capacity
  - Charging stage detection (Bulk / Absorption / Float / Idle)
  - Cell balance indicator with thresholds
  - Temperature monitor with thresholds
  - Charger efficiency estimate
  - Solar monitoring (voltage, power, daily energy)
  - Depth of discharge tracking
  - System load profile (avg / peak / idle)
- [x] `/api/analytics` JSON endpoint
- [x] Rated capacity override (`--rated-capacity N`)

## Pending / Ideas

- [ ] WebSocket push instead of polling (lower latency, no missed updates)
- [ ] Alert thresholds — push notifications for low SoC, high temp, cell imbalance
- [ ] Multi-battery support (aggregate view across multiple BMS devices)
- [ ] Auto-reconnect on BLE drop with configurable backoff
- [ ] Remember last connected device across restarts
- [ ] CSV / JSON export of historical data
- [ ] Grafana dashboard template (uses existing `/metrics` endpoint)
- [ ] MPPT solar tracking (requires hardware with current sensing on solar input)
- [ ] Configurable analytics window sizes (load profile, energy counters)
