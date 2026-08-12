#!/usr/bin/env bash
echo "===== Access Point ====="
systemctl --no-pager --full status pyhome-hotspot.service || true

echo
echo "===== Dashboard ====="
systemctl --no-pager --full status casa-dashboard.service || true

echo
echo "===== UART ====="
ls -l /dev/serial0 2>/dev/null || true
readlink -f /dev/serial0 2>/dev/null || true

echo
echo "===== Red ====="
nmcli connection show --active || true

echo
echo "===== Camaras ====="
v4l2-ctl --list-devices 2>/dev/null || true
