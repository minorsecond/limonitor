#!/usr/bin/env bash
# Clean, build, install limonitor, and set up systemd service on Raspberry Pi.
# Run from project root. Uses sudo for install and systemctl.
set -e

REPO="$(cd "$(dirname "$0")" && pwd)"
cd "$REPO"

# Config: edit these for your setup
SVC_USER="${LIMONITOR_USER:-pi}"
HTTP_PORT="${LIMONITOR_PORT:-8080}"
CONFIG_PATH="/etc/limonitor/limonitor.conf"

echo "=== clean ==="
./clean.sh

echo "=== build ==="
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ..

echo "=== install ==="
sudo cmake --install build

echo "=== systemd ==="
sudo mkdir -p /etc/limonitor
if [[ ! -f "$CONFIG_PATH" ]]; then
  sudo tee "$CONFIG_PATH" > /dev/null << EOF
# limonitor config — edit for your setup
daemon=true
http_port=$HTTP_PORT
verbose=false

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
