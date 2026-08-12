#!/usr/bin/env bash
set -euo pipefail

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

python3 - "$BASE_DIR/config.json" <<'PY'
import json
import subprocess
import sys

cfg = json.load(
    open(sys.argv[1], encoding="utf-8")
)["hotspot"]

name = cfg["connection_name"]
ssid = cfg["ssid"]
password = cfg["password"]
iface = cfg["interface"]
address = cfg["address"]

existing = subprocess.run(
    ["nmcli", "-t", "-f", "NAME", "connection", "show"],
    capture_output=True,
    text=True,
    check=True
).stdout.splitlines()

if name not in existing:
    subprocess.check_call([
        "nmcli", "connection", "add",
        "type", "wifi",
        "ifname", iface,
        "con-name", name,
        "ssid", ssid
    ])

subprocess.check_call([
    "nmcli", "connection", "modify", name,

    "802-11-wireless.mode", "ap",
    "802-11-wireless.band", "bg",

    "wifi-sec.key-mgmt", "wpa-psk",
    "wifi-sec.psk", password,

    "ipv4.method", "shared",
    "ipv4.addresses", address,

    "ipv6.method", "disabled",

    "connection.autoconnect", "yes",
    "connection.autoconnect-priority", "100"
])

subprocess.check_call([
    "nmcli", "connection", "up", name
])

print()
print("Access Point activo")
print(f"SSID: {ssid}")
print(f"IP Raspberry: {address.split('/')[0]}")
print(
    f"Web: http://{address.split('/')[0]}:8080"
)
PY
