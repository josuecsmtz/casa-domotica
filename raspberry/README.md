
# Casa Domótica — Raspberry Pi + ESP32 UART2

Esta carpeta contiene TODO lo necesario para la Raspberry Pi.

## Arquitectura

```text
Laptop / celular
      |
      | WiFi
      v
Casa-Domotica
10.42.0.1
      |
      +---------------- Raspberry Pi ----------------+
      |                                               |
      | Flask / Frontend                              |
      | 2 webcams USB                                 |
      | 8 módulos de relevador                        |
      |                                               |
      +-- UART GPIO14/GPIO15 -------------------------+
                          |
                          |
                       ESP32
                  UART2 GPIO16/17
                          |
         +----------------+----------------+
         | OLED, teclado 4x4, ENS160       |
         | 2 ultrasonicos, 2 caudalimetros |
         | servo cerradura                 |
         +---------------------------------+
```

La ESP32 mantiene UART0 libre para programación y Monitor Serial.

---

# 1. Conexión UART

## ESP32 -> Raspberry

| Señal | ESP32 | Raspberry BCM | Pin físico Raspberry |
|---|---:|---:|---:|
| ESP32 TX2 -> Raspberry RX | GPIO17 | GPIO15 RXD | Pin 10 |
| ESP32 RX2 <- Raspberry TX | GPIO16 | GPIO14 TXD | Pin 8 |
| Tierra | GND | GND | Pin 6 |

Baudios: `115200`, formato `8N1`.

Ambas placas trabajan con UART de 3.3 V. No uses niveles RS232 de +/- voltios directamente.

IMPORTANTE: GPIO14/GPIO15 de la Raspberry dejan de utilizarse como consola Linux porque quedan dedicados a la ESP32.

Después de instalar este proyecto, administra la Raspberry por SSH mediante la red local:

```bash
ssh pytito-4@10.42.0.1
```

También puedes usar PuTTY en modo SSH:
- Host: `10.42.0.1`
- Port: `22`
- Connection type: `SSH`

---

# 2. Relevadores conectados a Raspberry

Los relevadores NO consumen GPIO de la ESP32.

| R | Uso | BCM GPIO Raspberry | Pin físico |
|---|---|---:|---:|
| R1 | Ventilador recámara 1 | GPIO5 | 29 |
| R2 | Ventilador recámara 2 | GPIO6 | 31 |
| R3 | Ventilador recámara 3 | GPIO12 | 32 |
| R4 | Extractor 1 | GPIO13 | 33 |
| R5 | Extractor 2 | GPIO16 | 36 |
| R6 | Bomba 1 | GPIO20 | 38 |
| R7 | Bomba 2 | GPIO21 | 40 |
| R8 | Reserva | GPIO26 | 37 |

Por defecto `config.json` supone módulos de relevador activos en LOW.

Si tus módulos son activos en HIGH cambia:

```json
"active_low": false
```

No alimentes motores o bombas directamente desde GPIO. Los GPIO solamente mandan la señal al módulo de relevador o etapa de potencia.

---

# 3. Access Point

La Raspberry crea automáticamente:

```text
SSID: Casa-Domotica
Password: CasaDomotica2026
```

La Raspberry usa:

```text
10.42.0.1
```

y la web:

```text
http://10.42.0.1:8080
```

La IP queda fija en esta configuración.

---

# 4. Instalación una sola vez

Copia esta carpeta completa a la Raspberry.

Por ejemplo, si quedó en:

```text
/home/pytito-4/casa_domotica_raspberry
```

ejecuta:

```bash
cd /home/pytito-4/casa_domotica_raspberry
chmod +x install.sh
sudo ./install.sh
```

Cuando termine:

```bash
sudo reboot
```

El instalador:
- instala Flask;
- instala OpenCV;
- instala PySerial;
- instala GPIO Zero / lgpio;
- instala NetworkManager;
- habilita el UART de GPIO14/GPIO15;
- quita la consola Linux del UART;
- crea el Access Point;
- instala el servicio web;
- desactiva el antiguo `pytito.service`;
- configura arranque automático.

---

# 5. ¿Cómo se lanza después?

Después de la instalación NO tienes que ejecutar Python manualmente.

Cada vez que enciendes la Raspberry:

1. inicia Linux;
2. NetworkManager crea `Casa-Domotica`;
3. la Raspberry queda en `10.42.0.1`;
4. inicia `casa-dashboard.service`;
5. Python abre `/dev/serial0` a 115200;
6. abre las dos webcams;
7. configura los 8 GPIO de relevadores;
8. sirve la web en el puerto 8080.

Solo haces:

1. conectar laptop/celular a `Casa-Domotica`;
2. abrir `http://10.42.0.1:8080`.

---

# 6. Estado y diagnóstico

Desde SSH:

```bash
cd /opt/casa-domotica
./status.sh
```

O individualmente:

```bash
systemctl status pyhome-hotspot.service
systemctl status casa-dashboard.service
```

Logs en vivo:

```bash
journalctl -u casa-dashboard.service -f
```

Comprobar UART:

```bash
ls -l /dev/serial0
readlink -f /dev/serial0
```

Cámaras:

```bash
v4l2-ctl --list-devices
```

---

# 7. Reiniciar servicios

```bash
cd /opt/casa-domotica
./restart.sh
```

O:

```bash
sudo systemctl restart pyhome-hotspot.service
sudo systemctl restart casa-dashboard.service
```

---

# 8. Ejecutar Python manualmente para depuración

Primero detén el servicio:

```bash
sudo systemctl stop casa-dashboard.service
```

Después:

```bash
cd /opt/casa-domotica
python3 app.py
```

Para regresar al modo automático:

```bash
sudo systemctl start casa-dashboard.service
```

---

# 9. Protocolo UART

## ESP32 -> Raspberry

Telemetría:

```text
STATE,US1,34.2,US2,52.7,FLOW1,1.20,FLOW2,0.80,LIT1,2.430,LIT2,1.540,AQI,2,TVOC,88,ECO2,520,ENSFLAG,0,LOCK,0,SERVO,0,R1,1,R2,0,R3,0,R4,1,R5,0,R6,0,R7,0,R8,0
```

Petición local de relevador:

```text
CMD,RELAY,1,1
```

Contraseña:

```text
EVT,PASSWORD,OK
EVT,PASSWORD,FAIL
```

Cerradura:

```text
EVT,LOCK,OPEN,LOCAL
EVT,LOCK,CLOSED,LOCAL_AUTO
```

Sincronización:

```text
REQ,RELAYALL
```

## Raspberry -> ESP32

Estado individual:

```text
RELAY,1,1
```

Confirmación:

```text
ACK,RELAY,1,1
```

Los ocho relevadores:

```text
RELAYALL,1,0,0,0,0,0,0,0
```

Cerradura:

```text
SET,LOCK,OPEN
SET,LOCK,CLOSE
```

Consultar:

```text
PING
GET
```

---

# 10. Nivel de agua y presencia

Con la asignación actual:

- US1 = sensor de presencia.
- US2 = nivel del tanque/tinaco.

En `config.json`:

```json
"presence": {
  "ultrasonic": "US1",
  "detect_cm": 70.0,
  "clear_cm": 80.0
}
```

Para nivel:

```json
"water_level": {
  "ultrasonic": "US2",
  "full_distance_cm": 10.0,
  "empty_distance_cm": 100.0
}
```

Debes medir las distancias reales de tu maqueta:
- distancia cuando el tanque está lleno;
- distancia cuando está vacío.

Luego reemplazas esos dos números.

---

# 11. Presión y temperatura

El hardware que definiste para esta ESP32 todavía no incluye:
- sensor de presión;
- sensor de temperatura.

Por eso el backend no inventa esos valores.

ENS160 entrega AQI, TVOC y eCO2, pero no sustituye un sensor de temperatura dedicado.

Cuando definas esos dos sensores se agregan al mismo protocolo UART.

---

# 12. Cámaras

Inicialmente se usan:

```text
/dev/video0
/dev/video2
```

Si tus webcams aparecen con otros números:

```bash
v4l2-ctl --list-devices
```

Edita:

```text
/opt/casa-domotica/config.json
```

y cambia:

```json
"devices": [0, 2]
```

Después:

```bash
sudo systemctl restart casa-dashboard.service
```
