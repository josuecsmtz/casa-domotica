#!/usr/bin/env bash
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Ejecuta:"
    echo "sudo ./install.sh"
    exit 1
fi

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET_DIR="/opt/casa-domotica"
REAL_USER="${SUDO_USER:-$(logname 2>/dev/null || echo pytito-4)}"

echo "========================================"
echo " INSTALACION CASA DOMOTICA"
echo "========================================"
echo "Usuario: $REAL_USER"
echo "Destino: $TARGET_DIR"
echo

apt update

apt install -y \
    python3 \
    python3-flask \
    python3-opencv \
    python3-numpy \
    python3-serial \
    python3-gpiozero \
    python3-lgpio \
    network-manager \
    v4l-utils

systemctl enable --now NetworkManager

rm -rf "$TARGET_DIR"
mkdir -p "$TARGET_DIR"

cp -a "$SRC_DIR/." "$TARGET_DIR/"

chmod +x \
    "$TARGET_DIR/setup_hotspot.sh" \
    "$TARGET_DIR/setup_uart.sh"

chown -R "$REAL_USER:$REAL_USER" "$TARGET_DIR"

# Grupos necesarios
for group in dialout video gpio; do
    if getent group "$group" >/dev/null 2>&1; then
        usermod -aG "$group" "$REAL_USER"
    fi
done

# Configura UART de GPIO14/GPIO15
"$TARGET_DIR/setup_uart.sh"

# Instala servicios systemd
sed \
    "s|__USER__|$REAL_USER|g; s|__DIR__|$TARGET_DIR|g" \
    "$TARGET_DIR/systemd/casa-dashboard.service.in" \
    > /etc/systemd/system/casa-dashboard.service

sed \
    "s|__DIR__|$TARGET_DIR|g" \
    "$TARGET_DIR/systemd/pyhome-hotspot.service.in" \
    > /etc/systemd/system/pyhome-hotspot.service

# Desactiva el antiguo servicio que anteriormente usaba el UART.
systemctl disable --now pytito.service 2>/dev/null || true

systemctl daemon-reload

systemctl enable pyhome-hotspot.service
systemctl enable casa-dashboard.service

echo
echo "========================================"
echo " INSTALACION TERMINADA"
echo "========================================"
echo
echo "Ahora reinicia:"
echo
echo "  sudo reboot"
echo
echo "Despues del reinicio:"
echo
echo "  WiFi: Casa-Domotica"
echo "  Clave: CasaDomotica2026"
echo "  Web: http://10.42.0.1:8080"
echo
echo "Para SSH desde una laptop conectada al AP:"
echo
echo "  ssh $REAL_USER@10.42.0.1"
echo
echo "UART ESP32:"
echo "  Raspberry TX GPIO14 / pin 8 -> ESP32 RX2 GPIO16"
echo "  Raspberry RX GPIO15 / pin 10 <- ESP32 TX2 GPIO17"
echo "  GND / pin 6 <----------------> GND ESP32"
echo
