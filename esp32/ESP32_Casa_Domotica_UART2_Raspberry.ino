/*
  ============================================================
  CASA DOMOTICA - ESP32 WROOM 38 PINES
  UART2 HACIA RASPBERRY + UART0 LIBRE PARA CONSOLA/PROGRAMACION
  ============================================================

  ARQUITECTURA:

  ESP32:
    - Interfaz local: OLED + teclado 4x4
    - 2 ultrasonicos
    - ENS160
    - 2 caudalimetros
    - 1 servomotor
    - Logica local de cerradura
    - UART2 con Raspberry Pi

  RASPBERRY PI:
    - Access Point / frontend / webcams
    - 8 relevadores fisicamente conectados a sus GPIO
    - Recibe solicitudes locales desde ESP32 por UART2
    - Devuelve a ESP32 el estado real de los relevadores

  UART0 queda LIBRE para:
    - Programar la ESP32 por USB
    - Monitor Serial de Arduino IDE
    - Depuracion

  UART2:
    ESP32 TX2 GPIO17 -> RX Raspberry
    ESP32 RX2 GPIO16 <- TX Raspberry
    GND ESP32 <------> GND Raspberry

  IMPORTANTE:
    La Raspberry trabaja a 3.3 V en UART, por lo que puede conectarse
    directamente al UART del ESP32. Nunca metas RS232 real (+/- voltios)
    directamente a estos GPIO.

  LIBRERIAS:
    - Keypad
    - Adafruit GFX Library
    - Adafruit SSD1306
    - SparkFun Indoor Air Quality Sensor - ENS160
    - ESP32Servo

  ============================================================
*/

#include <Wire.h>
#include <Keypad.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SparkFun_ENS160.h>
#include <ESP32Servo.h>

// ============================================================
// SERIAL
// ============================================================

// UART0: consola USB / programacion
static const uint32_t DEBUG_BAUD = 115200;

// UART2: comunicacion dedicada Raspberry <-> ESP32
static const uint32_t RPI_UART_BAUD = 115200;
static const uint8_t PIN_UART2_RX = 16;
static const uint8_t PIN_UART2_TX = 17;

HardwareSerial RaspberrySerial(2);

// ============================================================
// I2C: OLED + ENS160
// ============================================================

static const uint8_t PIN_SDA = 21;
static const uint8_t PIN_SCL = 22;

static const uint8_t OLED_ADDRESS = 0x3C;
static const int SCREEN_WIDTH = 128;
static const int SCREEN_HEIGHT = 64;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
bool oledOK = false;

SparkFun_ENS160 ens160;
bool ensOK = false;

uint8_t ensAQI = 0;
uint16_t ensTVOC = 0;
uint16_t ensECO2 = 0;
uint8_t ensFlags = 3;

// ============================================================
// ULTRASONICOS
// ============================================================

// Ultrasonico 1
static const uint8_t PIN_US1_TRIG = 25;
static const uint8_t PIN_US1_ECHO = 34; // solo entrada

// Ultrasonico 2
static const uint8_t PIN_US2_TRIG = 26;
static const uint8_t PIN_US2_ECHO = 35; // solo entrada

float distancia1Cm = -1.0f;
float distancia2Cm = -1.0f;

uint8_t ultrasonicTurn = 0;

// ============================================================
// CAUDALIMETROS
// ============================================================

// GPIO36 y GPIO39 son solo entrada y NO tienen pull-up interno.
// Coloca resistencia pull-up externa de 10k desde SIGNAL a 3.3V.
static const uint8_t PIN_FLOW1 = 36;
static const uint8_t PIN_FLOW2 = 39;

// Valor inicial orientativo.
// Debes calibrarlo con tus caudalimetros reales.
static const float FLOW1_PULSES_PER_LITER = 5880.0f;
static const float FLOW2_PULSES_PER_LITER = 5880.0f;

volatile uint32_t flowPulses1 = 0;
volatile uint32_t flowPulses2 = 0;

portMUX_TYPE flowMux = portMUX_INITIALIZER_UNLOCKED;

float flow1Lmin = 0.0f;
float flow2Lmin = 0.0f;
float total1Liters = 0.0f;
float total2Liters = 0.0f;

// ============================================================
// SERVOMOTOR - CERRADURA
// ============================================================

static const uint8_t PIN_SERVO = 32;

static const int SERVO_CLOSED_ANGLE = 0;
static const int SERVO_OPEN_ANGLE = 90;

static const uint32_t LOCAL_UNLOCK_TIME_MS = 5000;

Servo lockServo;

bool lockOpen = false;
int servoAngle = SERVO_CLOSED_ANGLE;
uint32_t autoCloseDeadline = 0;

// ============================================================
// TECLADO MATRICIAL 4x4 STEREN
// ============================================================

const byte ROWS = 4;
const byte COLS = 4;

char keyMap[ROWS][COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};

/*
  Se reasignan los GPIO16/17 porque ahora pertenecen a UART2.

  Teclado:
    R1 -> GPIO13
    R2 -> GPIO14
    R3 -> GPIO18
    R4 -> GPIO19

    C1 -> GPIO23
    C2 -> GPIO27
    C3 -> GPIO33
    C4 -> GPIO4
*/

byte rowPins[ROWS] = {13, 14, 18, 19};
byte colPins[COLS] = {23, 27, 33, 4};

Keypad keypad = Keypad(
  makeKeymap(keyMap),
  rowPins,
  colPins,
  ROWS,
  COLS
);

// ============================================================
// CONTRASENA
// ============================================================

String accessPassword = "1234";
String passwordBuffer = "";

// ============================================================
// ESTADOS DE LOS 8 RELEVADORES
// ============================================================

// Los 8 relevadores se conectan fisicamente a la Raspberry.
// La ESP32 mantiene una copia de sus estados para la OLED.
bool relayState[8] = {
  false, false, false, false,
  false, false, false, false
};

// ============================================================
// MENU OLED
// ============================================================

const char* menuItems[] = {
  "Resumen",
  "Vent Rec 1",
  "Vent Rec 2",
  "Vent Rec 3",
  "Extractor 1",
  "Extractor 2",
  "Bomba 1",
  "Bomba 2",
  "Rele reserva",
  "Cerradura",
  "Calidad aire",
  "Caudales",
  "Ultrasonicos"
};

static const uint8_t MENU_COUNT =
  sizeof(menuItems) / sizeof(menuItems[0]);

uint8_t menuIndex = 0;

enum UiMode {
  UI_MENU,
  UI_DETAIL,
  UI_PASSWORD
};

UiMode uiMode = UI_MENU;

String toastMessage = "";
uint32_t toastUntil = 0;

// ============================================================
// TEMPORIZADORES
// ============================================================

uint32_t lastUltrasonicMs = 0;
uint32_t lastFlowCalcMs = 0;
uint32_t lastEnsMs = 0;
uint32_t lastTelemetryMs = 0;
uint32_t lastDisplayMs = 0;

static const uint32_t ULTRASONIC_INTERVAL_MS = 120;
static const uint32_t FLOW_CALC_INTERVAL_MS = 1000;
static const uint32_t ENS_INTERVAL_MS = 1000;
static const uint32_t TELEMETRY_INTERVAL_MS = 1000;
static const uint32_t DISPLAY_INTERVAL_MS = 120;

// ============================================================
// UART2 RX
// ============================================================

String rpiRxBuffer = "";

// ============================================================
// INTERRUPCIONES DE CAUDAL
// ============================================================

void IRAM_ATTR onFlow1Pulse() {
  portENTER_CRITICAL_ISR(&flowMux);
  flowPulses1++;
  portEXIT_CRITICAL_ISR(&flowMux);
}

void IRAM_ATTR onFlow2Pulse() {
  portENTER_CRITICAL_ISR(&flowMux);
  flowPulses2++;
  portEXIT_CRITICAL_ISR(&flowMux);
}

// ============================================================
// UTILIDADES
// ============================================================

void debugPrintln(const String& msg) {
  Serial.println(msg);
}

void showToast(const String& msg, uint32_t durationMs = 1500) {
  toastMessage = msg;
  toastUntil = millis() + durationMs;
}

bool toastActive() {
  return toastMessage.length() > 0 &&
         (int32_t)(toastUntil - millis()) > 0;
}

void sendRaspberry(const String& msg) {
  RaspberrySerial.println(msg);

  // Tambien se ve en la consola USB para depuracion.
  Serial.print("[TX RPI] ");
  Serial.println(msg);
}

String onOff(bool value) {
  return value ? "ON" : "OFF";
}

// ============================================================
// ULTRASONICOS
// ============================================================

float readUltrasonicCm(uint8_t trigPin, uint8_t echoPin) {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(3);

  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  unsigned long duration = pulseIn(echoPin, HIGH, 30000UL);

  if (duration == 0) {
    return -1.0f;
  }

  float distance = (duration * 0.0343f) / 2.0f;

  if (distance < 2.0f || distance > 400.0f) {
    return -1.0f;
  }

  return distance;
}

void updateUltrasonics() {
  uint32_t now = millis();

  if (now - lastUltrasonicMs < ULTRASONIC_INTERVAL_MS) {
    return;
  }

  lastUltrasonicMs = now;

  // Se disparan alternadamente para reducir interferencia.
  if (ultrasonicTurn == 0) {
    distancia1Cm = readUltrasonicCm(
      PIN_US1_TRIG,
      PIN_US1_ECHO
    );

    ultrasonicTurn = 1;
  } else {
    distancia2Cm = readUltrasonicCm(
      PIN_US2_TRIG,
      PIN_US2_ECHO
    );

    ultrasonicTurn = 0;
  }
}

// ============================================================
// CAUDALIMETROS
// ============================================================

void updateFlowMeters() {
  uint32_t now = millis();

  if (now - lastFlowCalcMs < FLOW_CALC_INTERVAL_MS) {
    return;
  }

  uint32_t elapsedMs = now - lastFlowCalcMs;
  lastFlowCalcMs = now;

  uint32_t p1;
  uint32_t p2;

  portENTER_CRITICAL(&flowMux);

  p1 = flowPulses1;
  p2 = flowPulses2;

  flowPulses1 = 0;
  flowPulses2 = 0;

  portEXIT_CRITICAL(&flowMux);

  float elapsedSec = elapsedMs / 1000.0f;

  float liters1 = p1 / FLOW1_PULSES_PER_LITER;
  float liters2 = p2 / FLOW2_PULSES_PER_LITER;

  total1Liters += liters1;
  total2Liters += liters2;

  if (elapsedSec > 0.0f) {
    flow1Lmin = (liters1 / elapsedSec) * 60.0f;
    flow2Lmin = (liters2 / elapsedSec) * 60.0f;
  } else {
    flow1Lmin = 0.0f;
    flow2Lmin = 0.0f;
  }
}

void resetFlowTotals(uint8_t which) {
  if (which == 0 || which == 1) {
    total1Liters = 0.0f;
  }

  if (which == 0 || which == 2) {
    total2Liters = 0.0f;
  }
}

// ============================================================
// ENS160
// ============================================================

void updateENS160() {
  if (!ensOK) {
    return;
  }

  uint32_t now = millis();

  if (now - lastEnsMs < ENS_INTERVAL_MS) {
    return;
  }

  lastEnsMs = now;

  if (ens160.checkDataStatus()) {
    ensAQI = ens160.getAQI();
    ensTVOC = ens160.getTVOC();
    ensECO2 = ens160.getECO2();
    ensFlags = ens160.getFlags();
  }
}

// ============================================================
// CERRADURA / SERVO
// ============================================================

void setServoAngle(int angle) {
  angle = constrain(angle, 0, 180);

  servoAngle = angle;
  lockServo.write(servoAngle);
}

void setLockState(bool open, const String& source) {
  lockOpen = open;

  if (lockOpen) {
    setServoAngle(SERVO_OPEN_ANGLE);
  } else {
    setServoAngle(SERVO_CLOSED_ANGLE);
    autoCloseDeadline = 0;
  }

  String msg = "EVT,LOCK,";

  msg += lockOpen ? "OPEN," : "CLOSED,";
  msg += source;

  sendRaspberry(msg);
}

void openLockLocal() {
  setLockState(true, "LOCAL");

  autoCloseDeadline =
    millis() + LOCAL_UNLOCK_TIME_MS;
}

void updateAutoClose() {
  if (!lockOpen || autoCloseDeadline == 0) {
    return;
  }

  if ((int32_t)(millis() - autoCloseDeadline) >= 0) {
    autoCloseDeadline = 0;

    setLockState(
      false,
      "LOCAL_AUTO"
    );
  }
}

// ============================================================
// RELEVADORES EN RASPBERRY
// ============================================================

void requestRelayToggle(uint8_t relayNumber) {
  if (relayNumber < 1 || relayNumber > 8) {
    return;
  }

  uint8_t index = relayNumber - 1;
  bool requested = !relayState[index];

  // Actualizacion optimista.
  // Raspberry responde ACK,RELAY,n,estado.
  relayState[index] = requested;

  String msg = "CMD,RELAY,";
  msg += String(relayNumber);
  msg += ",";
  msg += requested ? "1" : "0";

  sendRaspberry(msg);

  showToast(
    String("R") +
    relayNumber +
    " -> " +
    onOff(requested)
  );
}

// ============================================================
// PASSWORD
// ============================================================

void startPasswordEntry() {
  passwordBuffer = "";
  uiMode = UI_PASSWORD;
}

void cancelPasswordEntry() {
  passwordBuffer = "";
  uiMode = UI_MENU;
}

void confirmPassword() {
  if (passwordBuffer == accessPassword) {
    sendRaspberry("EVT,PASSWORD,OK");

    showToast(
      "Clave correcta",
      1200
    );

    passwordBuffer = "";
    uiMode = UI_MENU;

    openLockLocal();
  } else {
    sendRaspberry("EVT,PASSWORD,FAIL");

    showToast(
      "Clave incorrecta",
      1600
    );

    passwordBuffer = "";
  }
}

// ============================================================
// TECLADO / MENU
// ============================================================

void executeMenuSelection() {
  // Los indices 1..8 corresponden a R1..R8.
  if (menuIndex >= 1 && menuIndex <= 8) {
    requestRelayToggle(menuIndex);
    return;
  }

  switch (menuIndex) {
    case 0:
    case 10:
    case 11:
    case 12:
      uiMode = UI_DETAIL;
      break;

    case 9:
      // Cerradura
      if (lockOpen) {
        setLockState(false, "LOCAL");
        showToast("Cerradura cerrada");
      } else {
        startPasswordEntry();
      }
      break;
  }
}

void processKey(char key) {
  if (!key) {
    return;
  }

  Serial.print("[TECLA] ");
  Serial.println(key);

  // ========================================================
  // PASSWORD
  // ========================================================
  if (uiMode == UI_PASSWORD) {
    if (key >= '0' && key <= '9') {
      if (passwordBuffer.length() < 10) {
        passwordBuffer += key;
      }

      return;
    }

    if (key == '*') {
      passwordBuffer = "";
      return;
    }

    if (key == 'D') {
      cancelPasswordEntry();
      return;
    }

    if (key == 'C' || key == '#') {
      confirmPassword();
      return;
    }

    return;
  }

  // ========================================================
  // DETALLE
  // ========================================================
  if (uiMode == UI_DETAIL) {
    if (key == 'D' || key == 'C') {
      uiMode = UI_MENU;
    }

    return;
  }

  // ========================================================
  // MENU
  // ========================================================

  // A = arriba / anterior
  if (key == 'A') {
    if (menuIndex == 0) {
      menuIndex = MENU_COUNT - 1;
    } else {
      menuIndex--;
    }

    return;
  }

  // B = abajo / siguiente
  if (key == 'B') {
    menuIndex++;

    if (menuIndex >= MENU_COUNT) {
      menuIndex = 0;
    }

    return;
  }

  // C = confirmar
  if (key == 'C') {
    executeMenuSelection();
    return;
  }

  // D = inicio
  if (key == 'D') {
    menuIndex = 0;
    return;
  }
}

void updateKeypad() {
  char key = keypad.getKey();

  if (key) {
    processKey(key);
  }
}

// ============================================================
// OLED
// ============================================================

void drawHeader(const char* title) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print(title);

  display.drawLine(
    0, 10,
    127, 10,
    SSD1306_WHITE
  );
}

void drawMenu() {
  drawHeader("CASA DOMOTICA");

  display.setTextSize(1);

  display.setCursor(0, 17);
  display.print(">");
  display.print(menuItems[menuIndex]);

  if (menuIndex >= 1 && menuIndex <= 8) {
    display.setCursor(0, 33);
    display.print("Estado: ");
    display.print(
      onOff(relayState[menuIndex - 1])
    );
  }

  if (menuIndex == 9) {
    display.setCursor(0, 33);
    display.print("Estado: ");
    display.print(
      lockOpen ? "ABIERTA" : "CERRADA"
    );
  }

  display.setCursor(0, 50);
  display.print("A:< B:> C:OK D:Ini");
}

void drawPassword() {
  drawHeader("CERRADURA");

  display.setCursor(0, 18);
  display.print("Ingrese clave:");

  display.setCursor(0, 32);

  for (
    uint8_t i = 0;
    i < passwordBuffer.length();
    i++
  ) {
    display.print("*");
  }

  display.setCursor(0, 50);
  display.print("C:OK *=Borrar D:Salir");
}

void drawSummary() {
  drawHeader("RESUMEN");

  display.setCursor(0, 14);

  display.print("U1:");

  if (distancia1Cm >= 0) {
    display.print(distancia1Cm, 0);
    display.print("cm");
  } else {
    display.print("---");
  }

  display.print(" U2:");

  if (distancia2Cm >= 0) {
    display.print(distancia2Cm, 0);
    display.print("cm");
  } else {
    display.print("---");
  }

  display.setCursor(0, 27);

  display.print("F1:");
  display.print(flow1Lmin, 1);

  display.print(" F2:");
  display.print(flow2Lmin, 1);
  display.print("L/m");

  display.setCursor(0, 40);

  display.print("AQI:");
  display.print(ensAQI);

  display.print(" CO2:");
  display.print(ensECO2);

  display.setCursor(0, 53);

  display.print("Cerr:");
  display.print(
    lockOpen ? "ABIERTA" : "CERRADA"
  );
}

void drawAirQuality() {
  drawHeader("CALIDAD DE AIRE");

  if (!ensOK) {
    display.setCursor(0, 22);
    display.print("ENS160 no detectado");
    return;
  }

  display.setCursor(0, 15);

  display.print("AQI: ");
  display.print(ensAQI);
  display.print(" / 5");

  display.setCursor(0, 28);

  display.print("TVOC: ");
  display.print(ensTVOC);
  display.print(" ppb");

  display.setCursor(0, 41);

  display.print("eCO2: ");
  display.print(ensECO2);
  display.print(" ppm");

  display.setCursor(0, 54);

  display.print("Flag: ");
  display.print(ensFlags);
}

void drawFlows() {
  drawHeader("CAUDALES");

  display.setCursor(0, 15);

  display.print("F1: ");
  display.print(flow1Lmin, 2);
  display.print(" L/min");

  display.setCursor(0, 28);

  display.print("T1: ");
  display.print(total1Liters, 2);
  display.print(" L");

  display.setCursor(0, 41);

  display.print("F2: ");
  display.print(flow2Lmin, 2);
  display.print(" L/min");

  display.setCursor(0, 54);

  display.print("T2: ");
  display.print(total2Liters, 2);
  display.print(" L");
}

void drawUltrasonics() {
  drawHeader("ULTRASONICOS");

  display.setCursor(0, 19);
  display.print("US1: ");

  if (distancia1Cm >= 0) {
    display.print(distancia1Cm, 1);
    display.print(" cm");
  } else {
    display.print("Sin lectura");
  }

  display.setCursor(0, 37);
  display.print("US2: ");

  if (distancia2Cm >= 0) {
    display.print(distancia2Cm, 1);
    display.print(" cm");
  } else {
    display.print("Sin lectura");
  }

  display.setCursor(0, 54);
  display.print("D/C: regresar");
}

void drawDetail() {
  switch (menuIndex) {
    case 0:
      drawSummary();
      break;

    case 10:
      drawAirQuality();
      break;

    case 11:
      drawFlows();
      break;

    case 12:
      drawUltrasonics();
      break;

    default:
      uiMode = UI_MENU;
      drawMenu();
      break;
  }
}

void updateOLED() {
  if (!oledOK) {
    return;
  }

  uint32_t now = millis();

  if (now - lastDisplayMs < DISPLAY_INTERVAL_MS) {
    return;
  }

  lastDisplayMs = now;

  display.clearDisplay();

  if (toastActive()) {
    drawHeader("AVISO");

    display.setCursor(0, 25);
    display.setTextSize(1);
    display.print(toastMessage);

    display.display();
    return;
  }

  if (toastMessage.length() > 0) {
    toastMessage = "";
  }

  if (uiMode == UI_PASSWORD) {
    drawPassword();
  } else if (uiMode == UI_DETAIL) {
    drawDetail();
  } else {
    drawMenu();
  }

  display.display();
}

// ============================================================
// TELEMETRIA HACIA RASPBERRY
// ============================================================

void sendTelemetry() {
  uint32_t now = millis();

  if (now - lastTelemetryMs < TELEMETRY_INTERVAL_MS) {
    return;
  }

  lastTelemetryMs = now;

  String msg;
  msg.reserve(280);

  msg = "STATE";

  msg += ",US1,";
  msg += String(distancia1Cm, 1);

  msg += ",US2,";
  msg += String(distancia2Cm, 1);

  msg += ",FLOW1,";
  msg += String(flow1Lmin, 2);

  msg += ",FLOW2,";
  msg += String(flow2Lmin, 2);

  msg += ",LIT1,";
  msg += String(total1Liters, 3);

  msg += ",LIT2,";
  msg += String(total2Liters, 3);

  msg += ",AQI,";
  msg += String(ensAQI);

  msg += ",TVOC,";
  msg += String(ensTVOC);

  msg += ",ECO2,";
  msg += String(ensECO2);

  msg += ",ENSFLAG,";
  msg += String(ensFlags);

  msg += ",LOCK,";
  msg += lockOpen ? "1" : "0";

  msg += ",SERVO,";
  msg += String(servoAngle);

  for (uint8_t i = 0; i < 8; i++) {
    msg += ",R";
    msg += String(i + 1);
    msg += ",";
    msg += relayState[i] ? "1" : "0";
  }

  sendRaspberry(msg);
}

// ============================================================
// UART2 DESDE RASPBERRY
// ============================================================

void setRelayMirror(
  uint8_t relayNumber,
  bool state
) {
  if (
    relayNumber < 1 ||
    relayNumber > 8
  ) {
    return;
  }

  relayState[relayNumber - 1] = state;
}

void parseRaspberryLine(String line) {
  line.trim();

  if (line.length() == 0) {
    return;
  }

  Serial.print("[RX RPI] ");
  Serial.println(line);

  // PING
  if (line == "PING") {
    sendRaspberry("PONG");
    return;
  }

  // GET
  if (line == "GET") {
    lastTelemetryMs = 0;
    sendTelemetry();
    return;
  }

  // ACK,RELAY,n,state
  // RELAY,n,state
  if (
    line.startsWith("ACK,RELAY,") ||
    line.startsWith("RELAY,")
  ) {
    int startIndex;

    if (line.startsWith("ACK,RELAY,")) {
      startIndex =
        String("ACK,RELAY,").length();
    } else {
      startIndex =
        String("RELAY,").length();
    }

    int comma =
      line.indexOf(',', startIndex);

    if (comma > 0) {
      uint8_t relayNumber =
        line.substring(
          startIndex,
          comma
        ).toInt();

      bool state =
        line.substring(
          comma + 1
        ).toInt() != 0;

      setRelayMirror(
        relayNumber,
        state
      );
    }

    return;
  }

  // RELAYALL,s1,s2,s3,s4,s5,s6,s7,s8
  if (line.startsWith("RELAYALL,")) {
    String payload =
      line.substring(9);

    int from = 0;

    for (uint8_t i = 0; i < 8; i++) {
      int comma =
        payload.indexOf(',', from);

      String token;

      if (comma < 0) {
        token =
          payload.substring(from);
      } else {
        token =
          payload.substring(from, comma);
      }

      relayState[i] =
        token.toInt() != 0;

      if (comma < 0) {
        break;
      }

      from = comma + 1;
    }

    return;
  }

  // Cerradura desde Raspberry/web
  if (line == "SET,LOCK,OPEN") {
    autoCloseDeadline = 0;

    setLockState(
      true,
      "WIFI"
    );

    showToast(
      "Abierta desde RPi"
    );

    return;
  }

  if (
    line == "SET,LOCK,CLOSE" ||
    line == "SET,LOCK,CLOSED"
  ) {
    setLockState(
      false,
      "WIFI"
    );

    showToast(
      "Cerrada desde RPi"
    );

    return;
  }

  // Servo manual
  if (line.startsWith("SET,SERVO,")) {
    int angle =
      line.substring(10).toInt();

    autoCloseDeadline = 0;

    setServoAngle(angle);

    lockOpen =
      (angle == SERVO_OPEN_ANGLE);

    sendRaspberry(
      String("ACK,SERVO,") +
      String(servoAngle)
    );

    return;
  }

  // Reset de caudales
  if (line == "RESETFLOW,1") {
    resetFlowTotals(1);
    sendRaspberry("ACK,RESETFLOW,1");
    return;
  }

  if (line == "RESETFLOW,2") {
    resetFlowTotals(2);
    sendRaspberry("ACK,RESETFLOW,2");
    return;
  }

  if (line == "RESETFLOW,ALL") {
    resetFlowTotals(0);
    sendRaspberry("ACK,RESETFLOW,ALL");
    return;
  }
}

void updateRaspberryUART() {
  while (RaspberrySerial.available()) {
    char c = RaspberrySerial.read();

    if (c == '\n' || c == '\r') {
      if (rpiRxBuffer.length() > 0) {
        parseRaspberryLine(
          rpiRxBuffer
        );

        rpiRxBuffer = "";
      }
    } else {
      rpiRxBuffer += c;

      if (rpiRxBuffer.length() > 300) {
        rpiRxBuffer = "";
      }
    }
  }
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  // --------------------------------------------------------
  // UART0 -> USB / consola
  // --------------------------------------------------------
  Serial.begin(DEBUG_BAUD);
  delay(300);

  Serial.println();
  Serial.println("==================================");
  Serial.println(" CASA DOMOTICA - ESP32");
  Serial.println(" UART0 = consola USB");
  Serial.println(" UART2 = Raspberry");
  Serial.println("==================================");

  // --------------------------------------------------------
  // UART2 -> Raspberry
  // --------------------------------------------------------
  RaspberrySerial.begin(
    RPI_UART_BAUD,
    SERIAL_8N1,
    PIN_UART2_RX,
    PIN_UART2_TX
  );

  Serial.print("UART2 RX = GPIO");
  Serial.println(PIN_UART2_RX);

  Serial.print("UART2 TX = GPIO");
  Serial.println(PIN_UART2_TX);

  // --------------------------------------------------------
  // I2C
  // --------------------------------------------------------
  Wire.begin(
    PIN_SDA,
    PIN_SCL
  );

  Wire.setClock(100000);

  // --------------------------------------------------------
  // OLED
  // --------------------------------------------------------
  oledOK =
    display.begin(
      SSD1306_SWITCHCAPVCC,
      OLED_ADDRESS
    );

  if (oledOK) {
    display.clearDisplay();

    display.setTextColor(
      SSD1306_WHITE
    );

    display.setTextSize(1);

    display.setCursor(0, 0);
    display.println("CASA DOMOTICA");
    display.println("Iniciando...");

    display.display();

    Serial.println("[OLED] OK");
  } else {
    Serial.println("[OLED] ERROR");
  }

  // --------------------------------------------------------
  // ENS160
  // --------------------------------------------------------
  ensOK = ens160.begin();

  if (ensOK) {
    ens160.setOperatingMode(
      SFE_ENS160_RESET
    );

    delay(100);

    ens160.setOperatingMode(
      SFE_ENS160_STANDARD
    );

    ensFlags = ens160.getFlags();

    Serial.println("[ENS160] OK");
  } else {
    Serial.println("[ENS160] NO DETECTADO");

    sendRaspberry(
      "ERR,ENS160,NOT_FOUND"
    );
  }

  // --------------------------------------------------------
  // ULTRASONICOS
  // --------------------------------------------------------
  pinMode(
    PIN_US1_TRIG,
    OUTPUT
  );

  pinMode(
    PIN_US1_ECHO,
    INPUT
  );

  pinMode(
    PIN_US2_TRIG,
    OUTPUT
  );

  pinMode(
    PIN_US2_ECHO,
    INPUT
  );

  digitalWrite(
    PIN_US1_TRIG,
    LOW
  );

  digitalWrite(
    PIN_US2_TRIG,
    LOW
  );

  // --------------------------------------------------------
  // CAUDALIMETROS
  // --------------------------------------------------------
  pinMode(
    PIN_FLOW1,
    INPUT
  );

  pinMode(
    PIN_FLOW2,
    INPUT
  );

  attachInterrupt(
    digitalPinToInterrupt(PIN_FLOW1),
    onFlow1Pulse,
    RISING
  );

  attachInterrupt(
    digitalPinToInterrupt(PIN_FLOW2),
    onFlow2Pulse,
    RISING
  );

  // --------------------------------------------------------
  // SERVO
  // --------------------------------------------------------
  lockServo.setPeriodHertz(50);

  lockServo.attach(
    PIN_SERVO,
    500,
    2400
  );

  setServoAngle(
    SERVO_CLOSED_ANGLE
  );

  // --------------------------------------------------------
  // TEMPORIZADORES
  // --------------------------------------------------------
  uint32_t now = millis();

  lastUltrasonicMs = now;
  lastFlowCalcMs = now;
  lastEnsMs = now;
  lastTelemetryMs = now;
  lastDisplayMs = now;

  // --------------------------------------------------------
  // AVISO A RASPBERRY
  // --------------------------------------------------------
  sendRaspberry(
    "BOOT,ESP32_CASA,2"
  );

  sendRaspberry(
    "REQ,RELAYALL"
  );

  showToast(
    "Sistema listo",
    1200
  );

  Serial.println("[SISTEMA] LISTO");
}

// ============================================================
// LOOP
// ============================================================

void loop() {
  // UART2 Raspberry
  updateRaspberryUART();

  // Interfaz local
  updateKeypad();

  // Sensores
  updateUltrasonics();
  updateFlowMeters();
  updateENS160();

  // Cerradura
  updateAutoClose();

  // Telemetria
  sendTelemetry();

  // Pantalla
  updateOLED();
}
