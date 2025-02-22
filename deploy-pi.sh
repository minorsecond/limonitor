#!/usr/bin/env bash
# Clean, build, install limonitor, and set up systemd service on Raspberry Pi.
# Run from project root. Uses sudo for install and systemctl.
set -e

REPO="$(cd "$(dirname "$0")" && pwd)"
cd "$REPO"

# Config: edit these for your setup
# Default: run as deploying user so DB path matches (avoids pi vs rwardrup mismatch)
SVC_USER="${LIMONITOR_USER:-${SUDO_USER:-$USER}}"
HTTP_PORT="${LIMONITOR_PORT:-8080}"
CONFIG_PATH="/etc/limonitor/limonitor.conf"

echo "=== clean ==="
./clean.sh

echo "=== build ==="
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)
cd ..

echo "=== install ==="
sudo cmake --install build

echo "=== systemd ==="
if [[ "$(uname -s)" != "Linux" ]]; then
  echo "skipping systemd (not Linux) — run manually: limonitor --daemon --config /path/to/limonitor.conf"
  echo "done."
  exit 0
fi
sudo mkdir -p /etc/limonitor
if [[ ! -f "$CONFIG_PATH" ]]; then
  DB_PATH="/home/$SVC_USER/.local/share/limonitor/limonitor.db"
  sudo tee "$CONFIG_PATH" > /dev/null << EOF
# limonitor config — edit for your setup
daemon=true
http_port=$HTTP_PORT
verbose=false
db_path=$DB_PATH

# BLE: set device_name or device_address to target a battery
# device_name=L-12100BNNA70
# device_address=AA:BB:CC:DD:EE:FF

# No BLE (serial/HTTP-only node):
# no_ble=true

# EpicPowerGate 2 serial (Pi with charger on USB):
# serial_device=/dev/ttyACM0

# Or poll remote limonitor for charger data:
# pwrgate_remote=other-pi:8080
EOF
  echo "created $CONFIG_PATH — edit and add device_name or device_address"
fi

sudo tee /etc/systemd/system/limonitor.service > /dev/null << EOF
[Unit]
Description=limonitor battery monitor
After=network.target bluetooth.target

[Service]
Type=simple
User=$SVC_USER
ExecStart=/usr/local/bin/limonitor --daemon --config $CONFIG_PATH
Restart=on-failure
RestartSec=10

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable limonitor
sudo systemctl start limonitor

echo "done. status: sudo systemctl status limonitor"
echo "logs: sudo journalctl -u limonitor -f"
echo ""
echo "If DB not updating: ensure User=$SVC_USER matches your login and db_path in config"
