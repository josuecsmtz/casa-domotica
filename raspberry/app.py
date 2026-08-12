
from __future__ import annotations

import json
import logging
import signal
import threading
import time
from collections import deque
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import cv2
import numpy as np
import serial
from flask import Flask, Response, jsonify, render_template, request
from gpiozero import OutputDevice

BASE_DIR = Path(__file__).resolve().parent
CONFIG = json.loads((BASE_DIR / "config.json").read_text(encoding="utf-8"))

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s"
)
log = logging.getLogger("casa-domotica")

app = Flask(__name__)

state_lock = threading.RLock()
serial_lock = threading.Lock()
stop_event = threading.Event()

serial_port: serial.Serial | None = None
relay_devices: dict[int, OutputDevice] = {}

def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()

STATE: dict[str, Any] = {
    "serial": {
        "connected": False,
        "port": CONFIG["serial"]["port"],
        "last_message_at": None,
        "last_error": None
    },
    "ultrasonics": {
        "us1_cm": None,
        "us2_cm": None
    },
    "presence": {
        "detected": False,
        "last_detected_at": None,
        "updated_at": None
    },
    "water": {
        "level_percent": None,
        "flow1_lmin": 0.0,
        "flow2_lmin": 0.0,
        "total1_liters": 0.0,
        "total2_liters": 0.0,
        "pressure": None
    },
    "air": {
        "aqi": None,
        "tvoc_ppb": None,
        "eco2_ppm": None,
        "ens_flag": None,
        "temperature_c": None
    },
    "lock": {
        "open": False,
        "source": "Sin datos",
        "servo_angle": 0,
        "updated_at": None
    },
    "password": {
        "result": "none",
        "updated_at": None
    },
    "relays": {
        str(i): False for i in range(1, 9)
    },
    "server_started_at": now_iso()
}

temperature_history = deque(
    maxlen=int(CONFIG.get("temperature_history_points", 180))
)

# ============================================================
# GPIO / RELEVADORES
# ============================================================

def setup_relays() -> None:
    active_low = bool(CONFIG["relays"].get("active_low", True))
    pins = CONFIG["relays"]["pins_bcm"]

    for relay_number_str, bcm_pin in pins.items():
        relay_number = int(relay_number_str)

        device = OutputDevice(
            int(bcm_pin),
            active_high=not active_low,
            initial_value=False
        )

        relay_devices[relay_number] = device
        log.info(
            "Relay %d (%s) -> BCM GPIO%d",
            relay_number,
            CONFIG["relays"]["names"].get(relay_number_str, ""),
            bcm_pin
        )

def set_relay(relay_number: int, state: bool, tell_esp: bool = True) -> bool:
    device = relay_devices.get(relay_number)

    if device is None:
        return False

    if state:
        device.on()
    else:
        device.off()

    with state_lock:
        STATE["relays"][str(relay_number)] = state

    if tell_esp:
        serial_send(f"RELAY,{relay_number},{1 if state else 0}")

    return True

def relay_all_line() -> str:
    with state_lock:
        values = [
            "1" if STATE["relays"][str(i)] else "0"
            for i in range(1, 9)
        ]

    return "RELAYALL," + ",".join(values)

# ============================================================
# CALCULOS DERIVADOS
# ============================================================

def calculate_water_level(distance_cm: float | None) -> float | None:
    if distance_cm is None or distance_cm < 0:
        return None

    cfg = CONFIG["water_level"]
    full_cm = float(cfg["full_distance_cm"])
    empty_cm = float(cfg["empty_distance_cm"])

    if empty_cm == full_cm:
        return None

    percent = (
        (empty_cm - distance_cm) /
        (empty_cm - full_cm)
    ) * 100.0

    return max(0.0, min(100.0, percent))

def update_presence(distance_cm: float | None) -> None:
    if distance_cm is None or distance_cm < 0:
        return

    now = now_iso()
    detect_cm = float(CONFIG["presence"]["detect_cm"])
    clear_cm = float(CONFIG["presence"]["clear_cm"])

    with state_lock:
        old = bool(STATE["presence"]["detected"])

        if not old and distance_cm <= detect_cm:
            STATE["presence"]["detected"] = True
            STATE["presence"]["last_detected_at"] = now
            STATE["presence"]["updated_at"] = now

        elif old and distance_cm >= clear_cm:
            STATE["presence"]["detected"] = False
            STATE["presence"]["updated_at"] = now

        elif old:
            STATE["presence"]["updated_at"] = now

# ============================================================
# UART
# ============================================================

def serial_send(line: str) -> bool:
    global serial_port

    with serial_lock:
        port = serial_port

        if port is None or not port.is_open:
            return False

        try:
            port.write((line.strip() + "\n").encode("utf-8"))
            port.flush()
            log.debug("TX ESP32: %s", line)
            return True
        except (serial.SerialException, OSError) as error:
            log.warning("Error escribiendo UART: %s", error)

            with state_lock:
                STATE["serial"]["last_error"] = str(error)

            return False

def parse_state_line(parts: list[str]) -> None:
    # Formato:
    # STATE,US1,xx,US2,xx,FLOW1,xx,FLOW2,xx,LIT1,xx,LIT2,xx,
    # AQI,x,TVOC,x,ECO2,x,ENSFLAG,x,LOCK,x,SERVO,x,R1,x...R8,x

    if len(parts) < 3:
        return

    values: dict[str, str] = {}

    index = 1
    while index + 1 < len(parts):
        values[parts[index].upper()] = parts[index + 1]
        index += 2

    def get_float(key: str) -> float | None:
        raw = values.get(key)

        if raw is None:
            return None

        try:
            return float(raw)
        except ValueError:
            return None

    def get_int(key: str) -> int | None:
        raw = values.get(key)

        if raw is None:
            return None

        try:
            return int(float(raw))
        except ValueError:
            return None

    us1 = get_float("US1")
    us2 = get_float("US2")

    with state_lock:
        if us1 is not None:
            STATE["ultrasonics"]["us1_cm"] = us1

        if us2 is not None:
            STATE["ultrasonics"]["us2_cm"] = us2

        flow1 = get_float("FLOW1")
        flow2 = get_float("FLOW2")
        lit1 = get_float("LIT1")
        lit2 = get_float("LIT2")

        if flow1 is not None:
            STATE["water"]["flow1_lmin"] = flow1

        if flow2 is not None:
            STATE["water"]["flow2_lmin"] = flow2

        if lit1 is not None:
            STATE["water"]["total1_liters"] = lit1

        if lit2 is not None:
            STATE["water"]["total2_liters"] = lit2

        aqi = get_int("AQI")
        tvoc = get_int("TVOC")
        eco2 = get_int("ECO2")
        flag = get_int("ENSFLAG")

        if aqi is not None:
            STATE["air"]["aqi"] = aqi

        if tvoc is not None:
            STATE["air"]["tvoc_ppb"] = tvoc

        if eco2 is not None:
            STATE["air"]["eco2_ppm"] = eco2

        if flag is not None:
            STATE["air"]["ens_flag"] = flag

        lock_value = get_int("LOCK")
        servo_angle = get_int("SERVO")

        if lock_value is not None:
            STATE["lock"]["open"] = bool(lock_value)

        if servo_angle is not None:
            STATE["lock"]["servo_angle"] = servo_angle

        for relay_number in range(1, 9):
            key = f"R{relay_number}"
            value = get_int(key)

            if value is not None:
                STATE["relays"][str(relay_number)] = bool(value)

    # Inferencias fuera del lock
    presence_sensor = CONFIG["presence"]["ultrasonic"].upper()

    if presence_sensor == "US1":
        update_presence(us1)
    else:
        update_presence(us2)

    water_sensor = CONFIG["water_level"]["ultrasonic"].upper()
    level_distance = us2 if water_sensor == "US2" else us1
    level_percent = calculate_water_level(level_distance)

    with state_lock:
        STATE["water"]["level_percent"] = level_percent

def parse_serial_line(line: str) -> None:
    line = line.strip()

    if not line:
        return

    log.info("RX ESP32: %s", line)

    parts = [piece.strip() for piece in line.split(",")]
    command = parts[0].upper()

    with state_lock:
        STATE["serial"]["last_message_at"] = now_iso()

    if command == "STATE":
        parse_state_line(parts)
        return

    if command == "BOOT":
        serial_send(relay_all_line())
        return

    if command == "REQ" and len(parts) >= 2:
        if parts[1].upper() == "RELAYALL":
            serial_send(relay_all_line())
        return

    if command == "CMD" and len(parts) >= 4:
        if parts[1].upper() == "RELAY":
            try:
                relay_number = int(parts[2])
                state = bool(int(parts[3]))
            except ValueError:
                return

            if set_relay(relay_number, state, tell_esp=False):
                serial_send(
                    f"ACK,RELAY,{relay_number},{1 if state else 0}"
                )
        return

    if command == "EVT" and len(parts) >= 3:
        event = parts[1].upper()

        if event == "PASSWORD":
            result = parts[2].upper()

            with state_lock:
                STATE["password"] = {
                    "result": (
                        "correct"
                        if result == "OK"
                        else "incorrect"
                    ),
                    "updated_at": now_iso()
                }

            return

        if event == "LOCK":
            state_text = parts[2].upper()
            source = (
                parts[3]
                if len(parts) >= 4
                else "Local"
            )

            with state_lock:
                STATE["lock"]["open"] = (
                    state_text == "OPEN"
                )
                STATE["lock"]["source"] = source
                STATE["lock"]["updated_at"] = now_iso()

            return

    if command == "ACK":
        return

    if command == "PONG":
        return

    if command == "ERR":
        log.warning("ESP32 reporto error: %s", line)
        return

def serial_worker() -> None:
    global serial_port

    cfg = CONFIG["serial"]
    serial_device = str(cfg["port"])
    baudrate = int(cfg["baudrate"])
    retry = float(cfg.get("reconnect_seconds", 2))

    while not stop_event.is_set():
        try:
            log.info(
                "Abriendo %s a %d baudios",
                serial_device,
                baudrate
            )

            port = serial.Serial(
                port=serial_device,
                baudrate=baudrate,
                timeout=0.5,
                write_timeout=1
            )

            serial_port = port

            with state_lock:
                STATE["serial"]["connected"] = True
                STATE["serial"]["last_error"] = None

            time.sleep(0.5)

            # Sincroniza al conectar
            serial_send("PING")
            serial_send(relay_all_line())
            serial_send("GET")

            while (
                not stop_event.is_set()
                and port.is_open
            ):
                raw = port.readline()

                if not raw:
                    continue

                line = raw.decode(
                    "utf-8",
                    errors="replace"
                ).strip()

                if line:
                    parse_serial_line(line)

        except (serial.SerialException, OSError) as error:
            log.warning("UART no disponible: %s", error)

            with state_lock:
                STATE["serial"]["connected"] = False
                STATE["serial"]["last_error"] = str(error)

        finally:
            port = serial_port

            if port is not None:
                try:
                    port.close()
                except Exception:
                    pass

            serial_port = None

            with state_lock:
                STATE["serial"]["connected"] = False

        stop_event.wait(retry)

# ============================================================
# CAMARAS
# ============================================================

class CameraStream:
    def __init__(
        self,
        device: int,
        name: str,
        width: int,
        height: int,
        fps: int,
        quality: int
    ):
        self.device = device
        self.name = name
        self.width = width
        self.height = height
        self.fps = max(1, fps)
        self.quality = quality

        self.connected = False
        self.frame_lock = threading.Lock()
        self.latest_jpeg: bytes | None = None

        threading.Thread(
            target=self._run,
            daemon=True
        ).start()

    def placeholder(self, message: str) -> bytes:
        frame = np.zeros(
            (self.height, self.width, 3),
            dtype=np.uint8
        )

        cv2.putText(
            frame,
            self.name,
            (25, 55),
            cv2.FONT_HERSHEY_SIMPLEX,
            1.0,
            (255, 255, 255),
            2
        )

        cv2.putText(
            frame,
            message,
            (25, 100),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.62,
            (255, 255, 255),
            2
        )

        ok, encoded = cv2.imencode(
            ".jpg",
            frame,
            [cv2.IMWRITE_JPEG_QUALITY, self.quality]
        )

        return encoded.tobytes() if ok else b""

    def _run(self):
        delay = 1.0 / self.fps

        while not stop_event.is_set():
            cap = cv2.VideoCapture(
                self.device,
                cv2.CAP_V4L2
            )

            cap.set(
                cv2.CAP_PROP_FRAME_WIDTH,
                self.width
            )
            cap.set(
                cv2.CAP_PROP_FRAME_HEIGHT,
                self.height
            )
            cap.set(
                cv2.CAP_PROP_FPS,
                self.fps
            )

            if not cap.isOpened():
                self.connected = False

                with self.frame_lock:
                    self.latest_jpeg = self.placeholder(
                        f"/dev/video{self.device} no disponible"
                    )

                cap.release()
                stop_event.wait(2)
                continue

            self.connected = True
            log.info(
                "%s activa en /dev/video%d",
                self.name,
                self.device
            )

            while (
                not stop_event.is_set()
                and cap.isOpened()
            ):
                start = time.monotonic()

                ok, frame = cap.read()

                if not ok:
                    break

                cv2.putText(
                    frame,
                    datetime.now().strftime(
                        "%Y-%m-%d %H:%M:%S"
                    ),
                    (15, frame.shape[0] - 18),
                    cv2.FONT_HERSHEY_SIMPLEX,
                    0.55,
                    (255, 255, 255),
                    2,
                    cv2.LINE_AA
                )

                ok, encoded = cv2.imencode(
                    ".jpg",
                    frame,
                    [
                        cv2.IMWRITE_JPEG_QUALITY,
                        self.quality
                    ]
                )

                if ok:
                    with self.frame_lock:
                        self.latest_jpeg = (
                            encoded.tobytes()
                        )

                remaining = (
                    delay -
                    (time.monotonic() - start)
                )

                if remaining > 0:
                    stop_event.wait(remaining)

            cap.release()
            self.connected = False
            stop_event.wait(1)

    def get_jpeg(self) -> bytes:
        with self.frame_lock:
            if self.latest_jpeg is not None:
                return self.latest_jpeg

        return self.placeholder(
            "Esperando imagen"
        )

camera_cfg = CONFIG["cameras"]
camera_devices = camera_cfg["devices"]

CAMERAS = [
    CameraStream(
        int(camera_devices[0]),
        "Camara 1",
        int(camera_cfg["width"]),
        int(camera_cfg["height"]),
        int(camera_cfg["fps"]),
        int(camera_cfg["jpeg_quality"])
    ),
    CameraStream(
        int(camera_devices[1]),
        "Camara 2",
        int(camera_cfg["width"]),
        int(camera_cfg["height"]),
        int(camera_cfg["fps"]),
        int(camera_cfg["jpeg_quality"])
    )
]

def camera_generator(camera: CameraStream):
    while not stop_event.is_set():
        jpeg = camera.get_jpeg()

        yield (
            b"--frame\r\n"
            b"Content-Type: image/jpeg\r\n"
            b"Cache-Control: no-cache\r\n\r\n"
            + jpeg
            + b"\r\n"
        )

        time.sleep(0.08)

# ============================================================
# API
# ============================================================

@app.get("/")
def index():
    return render_template(
        "index.html",
        relay_names=CONFIG["relays"]["names"]
    )

@app.get("/api/state")
def api_state():
    with state_lock:
        data = json.loads(
            json.dumps(STATE)
        )

        data["relay_names"] = (
            CONFIG["relays"]["names"]
        )

        data["temperature_history"] = (
            list(temperature_history)
        )

        data["cameras"] = [
            {
                "index": i,
                "device": camera_devices[i],
                "connected": CAMERAS[i].connected
            }
            for i in range(2)
        ]

    return jsonify(data)

@app.post("/api/relay/<int:relay_number>")
def api_relay(relay_number: int):
    if relay_number not in range(1, 9):
        return jsonify({
            "ok": False,
            "error": "Relay invalido"
        }), 404

    body = request.get_json(silent=True) or {}

    if "state" not in body:
        return jsonify({
            "ok": False,
            "error": "Falta state"
        }), 400

    state = bool(body["state"])

    if not set_relay(
        relay_number,
        state,
        tell_esp=True
    ):
        return jsonify({
            "ok": False,
            "error": "No se pudo activar GPIO"
        }), 500

    return jsonify({
        "ok": True,
        "relay": relay_number,
        "state": state
    })

@app.post("/api/lock")
def api_lock():
    body = request.get_json(silent=True) or {}
    state = str(
        body.get("state", "")
    ).lower()

    if state not in {"open", "closed"}:
        return jsonify({
            "ok": False,
            "error": "Estado invalido"
        }), 400

    command = (
        "SET,LOCK,OPEN"
        if state == "open"
        else "SET,LOCK,CLOSE"
    )

    sent = serial_send(command)

    return jsonify({
        "ok": sent,
        "state": state
    }), (200 if sent else 503)

@app.post("/api/flow/reset/<which>")
def api_reset_flow(which: str):
    which = which.upper()

    if which not in {"1", "2", "ALL"}:
        return jsonify({
            "ok": False,
            "error": "Parametro invalido"
        }), 400

    sent = serial_send(
        f"RESETFLOW,{which}"
    )

    return jsonify({
        "ok": sent
    }), (200 if sent else 503)

@app.get("/video/<int:index>")
def video(index: int):
    if not 0 <= index < len(CAMERAS):
        return "Camara no encontrada", 404

    return Response(
        camera_generator(CAMERAS[index]),
        mimetype=(
            "multipart/x-mixed-replace; "
            "boundary=frame"
        )
    )

@app.get("/health")
def health():
    return jsonify({
        "ok": True,
        "time": now_iso(),
        "serial_connected": (
            STATE["serial"]["connected"]
        )
    })

# ============================================================
# CIERRE
# ============================================================

def shutdown_handler(_signum, _frame):
    stop_event.set()

    for device in relay_devices.values():
        try:
            device.off()
            device.close()
        except Exception:
            pass

signal.signal(
    signal.SIGTERM,
    shutdown_handler
)
signal.signal(
    signal.SIGINT,
    shutdown_handler
)

# ============================================================
# MAIN
# ============================================================

if __name__ == "__main__":
    setup_relays()

    threading.Thread(
        target=serial_worker,
        daemon=True
    ).start()

    app.run(
        host=str(
            CONFIG["web"].get(
                "host",
                "0.0.0.0"
            )
        ),
        port=int(
            CONFIG["web"].get(
                "port",
                8080
            )
        ),
        threaded=True,
        debug=False,
        use_reloader=False
    )
