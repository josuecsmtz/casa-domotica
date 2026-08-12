#!/usr/bin/env bash
set -euo pipefail

echo "Configurando UART GPIO14/GPIO15 para la ESP32..."

CONFIG_FILE=""
CMDLINE_FILE=""

if [[ -f /boot/firmware/config.txt ]]; then
    CONFIG_FILE="/boot/firmware/config.txt"
elif [[ -f /boot/config.txt ]]; then
    CONFIG_FILE="/boot/config.txt"
else
    echo "No encontre config.txt de Raspberry Pi."
    exit 1
fi

if [[ -f /boot/firmware/cmdline.txt ]]; then
    CMDLINE_FILE="/boot/firmware/cmdline.txt"
elif [[ -f /boot/cmdline.txt ]]; then
    CMDLINE_FILE="/boot/cmdline.txt"
else
    echo "No encontre cmdline.txt de Raspberry Pi."
    exit 1
fi

cp -n "$CONFIG_FILE" "${CONFIG_FILE}.casa-domotica.bak" || true
cp -n "$CMDLINE_FILE" "${CMDLINE_FILE}.casa-domotica.bak" || true

if ! grep -Eq '^[[:space:]]*enable_uart=1[[:space:]]*$' "$CONFIG_FILE"; then
    echo "" >> "$CONFIG_FILE"
    echo "# Casa Domotica - UART ESP32" >> "$CONFIG_FILE"
    echo "enable_uart=1" >> "$CONFIG_FILE"
fi

python3 - "$CMDLINE_FILE" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])

text = path.read_text(
    encoding="utf-8"
).strip()

tokens = text.split()

blocked = (
    "console=serial0,",
    "console=serial1,",
    "console=ttyS0,",
    "console=ttyAMA0,"
)

tokens = [
    token
    for token in tokens
    if not token.startswith(blocked)
]

path.write_text(
    " ".join(tokens) + "\n",
    encoding="utf-8"
)
PY

systemctl disable --now \
    serial-getty@serial0.service \
    serial-getty@ttyS0.service \
    serial-getty@ttyAMA0.service \
    2>/dev/null || true

systemctl mask \
    serial-getty@serial0.service \
    serial-getty@ttyS0.service \
    serial-getty@ttyAMA0.service \
    2>/dev/null || true

echo "UART habilitado."
echo "Después de reiniciar se usará /dev/serial0."
echo
echo "IMPORTANTE:"
echo "GPIO14/GPIO15 dejan de ser consola Linux."
echo "Para administrar la Raspberry usa SSH por la red local."
