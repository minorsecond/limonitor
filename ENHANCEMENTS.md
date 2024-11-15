# limonitor — Enhancement Roadmap

## Quick Wins (low effort, high value)

- [x] **SoC bar fix** — bar renders `8###---` because percentage digits overlap with the bar start. 1-line fix in `tui.cpp`.
- [x] **Charging/Discharging label in web UI** — HTML dashboard shows raw `-5.22 A` with no context. Add a `CHARGING` / `DISCHARGING` / `IDLE` badge like the TUI does.
- [x] **"Est. Time to Full/Empty" label** — currently says `17.5 h remaining` for both states. When charging should say `17.5 h to full`; when discharging `17.5 h to empty`.
- [x] **History sparkline in TUI** — ring buffer already holds history; add an ASCII sparkline of last ~40 voltage samples in the pack section.
- [ ] **Auto-reconnect** — confirm/implement BLE re-scan on disconnect so the tool recovers from drops without manual restart.

## Medium Effort, Good Value

- [x] **Web dashboard history chart** — `/api/history` already exists. Generate a pure SVG voltage+current chart server-side (no external JS library) for a much more useful web UI.
- [ ] **Config file support** — `~/.config/limonitor.conf` or `/etc/limonitor.conf` so common flags (`-n`, `-p`, `-l`, etc.) don't have to be passed every run.
- [ ] **Alert thresholds + notification** — `--alert-low-soc N` and `--alert-high-temp N` flags; log warning + optional `notify-send` (Linux) / `osascript` (macOS) when thresholds are crossed.

## Multi-Unit Support (significant refactor, high value)

Architecture: accept multiple `-n NAME` / `-a ADDR` flags; spin up one `BleManager` + `DataStore` + `litime::Parser` per battery (already cleanly separated — zero changes needed to those classes).

- [ ] **CLI**: allow repeated `-n`/`-a` flags, one per battery
- [ ] **TUI**: split into vertical panels, one per battery (pack summary + cells per panel)
- [ ] **HTTP**: `/api/batteries` returns array; `/api/battery/0`, `/api/battery/1` for individual packs; `/metrics` uses device label tags
- [ ] **Demo mode**: `--demo 2` (or `--demo N`) to simulate N batteries
- [ ] **Aggregate stats**: for parallel packs show summed current/capacity; for series packs show summed voltage

## Lower Priority / Nice to Have

- [ ] **WebSocket push** — replace 5s meta-refresh with server-sent events or WebSocket for real-time web UI without full-page reload
- [ ] **Grafana dashboard template** — ship a `grafana-dashboard.json` that works out of the box with the existing Prometheus `/metrics` endpoint
- [ ] **Persistent device favorites** — remember last connected device UUID/address so `-n` isn't required on subsequent runs
- [ ] **CSV/JSON history export** — `GET /api/export?format=csv&n=N` endpoint to download history data
- [ ] **Automatic GATT discovery** — instead of hardcoded UUID overrides, scan all characteristics and auto-detect LiTime vs JBD vs unknown protocol
