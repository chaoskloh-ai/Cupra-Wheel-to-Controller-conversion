#include <Arduino.h>
#include <BleGamepad.h>

#define LIN_RX 16
#define LIN_TX 17
#define LIN_BAUD 19200
#define PIN_SLP_N 32

#define PIN_ACC_WEISS 25
#define PIN_START_GELB 26

// TODO: An tatsächliche Verkabelung anpassen! Analog zu PIN_ACC_WEISS/PIN_START_GELB
// wird die Cupra-Logo-Taste hier als eigener, separater digitaler Eingang angenommen
// (nicht Teil des LIN-Frames), da sie zusammen mit dem Scrollrad (LIN 0x0E, b1=0x06)
// gehalten werden soll und ein einzelnes LIN-Byte pro Insel i.d.R. nur einen Zustand
// gleichzeitig abbilden kann.
#define PIN_CUPRA_BTN 27

// ---- Debounce-Zeiten (ms) ----
#define DEBOUNCE_MS_ACC    25   // digitaler ACC-Pin
#define DEBOUNCE_MS_START  20   // analoger START-Pin
#define DEBOUNCE_MS_CUPRA  25   // digitaler Cupra-Taster
#define DEBOUNCE_MS_LIN    50   // LIN-Bus Rohbytes (ca. 1 Poll-Zyklus a 60ms Toleranz)

// ---- Backlight ----
#define BACKLIGHT_MIN            0x00
#define BACKLIGHT_MAX            0x7F
#define BACKLIGHT_STEP           0x10
#define BACKLIGHT_RESPONSE_BYTE  0xF9   // unverändert aus Originalcode übernommen (Byte 1 von 0x0D)

BleGamepad bleGamepad("Cupra Wheel", "VAG Retrofit", 100);

unsigned long lastTxTime = 0;
unsigned long lastUdsTime = 0;
uint8_t scheduleStep = 0;

uint8_t lastLeftButton = 0;
uint8_t lastLeftDirection = 0;
bool lastLeftWasCombo = false;   // true, wenn die aktuell gehaltene Taste als Backlight-Kombo verbraucht wurde
uint8_t lastRightButton = 0;
uint8_t lastWippenByte = 0;
bool lastAccState = HIGH;
bool lastStartState = false;

// Cupra-Modifier-Taste
bool lastCupraState = false;
bool cupraHeld = false;
bool cupraComboUsed = false;

// Backlight-Zustand
uint8_t backlightBrightness = BACKLIGHT_MAX;
uint8_t backlightLastLevel = BACKLIGHT_MAX;  // zum Wiederherstellen nach "aus"
bool backlightOn = true;

uint32_t simhubButtons = 0;

// ---------------------------------------------------------------------
// Generischer Software-Debounce (zeitbasiert, "muss X ms stabil sein")
// ---------------------------------------------------------------------
struct DebounceU8 {
  uint8_t committed = 0;
  uint8_t candidate = 0;
  unsigned long candidateSince = 0;
  bool primed = false;
};

uint8_t debounceUpdate(DebounceU8 &d, uint8_t raw, unsigned long now, unsigned long stableMs) {
  if (!d.primed) {
    d.committed = raw;
    d.candidate = raw;
    d.candidateSince = now;
    d.primed = true;
    return d.committed;
  }

  if (raw != d.candidate) {
    d.candidate = raw;
    d.candidateSince = now;
  } else if (d.candidate != d.committed && (now - d.candidateSince) >= stableMs) {
    d.committed = d.candidate;
  }

  return d.committed;
}

// Debounce-Instanzen
DebounceU8 dbAcc;
DebounceU8 dbStart;
DebounceU8 dbCupra;
DebounceU8 dbLeftB1;
DebounceU8 dbLeftB3;
DebounceU8 dbLeftB6;
DebounceU8 dbRightB2;

void triggerGamepadButton(uint8_t buttonId, bool pressed) {
  if (bleGamepad.isConnected()) {
    if (pressed) bleGamepad.press(buttonId);
    else bleGamepad.release(buttonId);
    bleGamepad.sendReport();
  }

  Serial.print(buttonId);
  Serial.print(":");
  Serial.println(pressed ? 1 : 0);
}

uint8_t calculateLINPID(uint8_t id) {
  uint8_t p0 = ((id >> 0) & 1) ^ ((id >> 1) & 1) ^ ((id >> 2) & 1) ^ ((id >> 4) & 1);
  uint8_t p1 = ~(((id >> 1) & 1) ^ ((id >> 3) & 1) ^ ((id >> 4) & 1) ^ ((id >> 5) & 1)) & 1;
  return id | (p0 << 6) | (p1 << 7);
}

uint8_t checksumEnhanced(uint8_t pid, const uint8_t* data, uint8_t len) {
  uint16_t sum = pid;
  for (uint8_t i = 0; i < len; i++) {
    sum += data[i];
    if (sum >= 256) sum = (sum & 0xFF) + 1;
  }
  return (uint8_t)(~sum);
}

void generatePhysicalHeader() {
  Serial1.end();
  pinMode(LIN_TX, OUTPUT);
  digitalWrite(LIN_TX, LOW);
  delayMicroseconds(755);
  digitalWrite(LIN_TX, HIGH);
  delayMicroseconds(112);
  Serial1.begin(LIN_BAUD, SERIAL_8N1, LIN_RX, LIN_TX);
}

void sendUdsInitialization() {
  uint8_t pid = calculateLINPID(0x3C);
  uint8_t payload[8] = { 0x01, 0x02, 0x10, 0x03, 0xFF, 0xFF, 0xFF, 0xFF };
  uint8_t checksum = 0xF9;

  generatePhysicalHeader();
  Serial1.write(0x55);
  Serial1.write(pid);
  for (uint8_t i = 0; i < 8; i++) Serial1.write(payload[i]);
  Serial1.write(checksum);
  Serial1.flush();
}

// ---------------------------------------------------------------------
// Backlight-Steuerung
// ---------------------------------------------------------------------
void setBacklight(uint8_t level, bool on) {
  backlightBrightness = level;
  backlightOn = on;
  if (level > BACKLIGHT_MIN) backlightLastLevel = level;
}

void adjustBacklight(int16_t step) {
  int16_t base = backlightOn ? backlightBrightness : BACKLIGHT_MIN;
  int16_t newLevel = base + step;

  if (newLevel > BACKLIGHT_MAX) newLevel = BACKLIGHT_MAX;

  if (newLevel <= BACKLIGHT_MIN) {
    setBacklight(BACKLIGHT_MIN, false);
  } else {
    setBacklight((uint8_t)newLevel, true);
  }
}

void toggleBacklight() {
  if (backlightOn) {
    setBacklight(BACKLIGHT_MIN, false);
  } else {
    setBacklight(backlightLastLevel > BACKLIGHT_MIN ? backlightLastLevel : BACKLIGHT_MAX, true);
  }
}

void interpretLeftIsland(uint8_t b1, uint8_t b3, uint8_t b6) {
  if (b1 != lastLeftButton || b3 != lastLeftDirection) {
    if (lastLeftButton != 0x00) {
      if (lastLeftButton == 0x12) {
        triggerGamepadButton(1, false);
        triggerGamepadButton(2, false);
      } else if (lastLeftButton == 0x06) {
        // Nur normale Gamepad-Buttons freigeben, wenn diese Betätigung NICHT
        // als Backlight-Kombo verbraucht wurde.
        if (!lastLeftWasCombo) {
          triggerGamepadButton(6, false);
          triggerGamepadButton(7, false);
        }
      } else {
        switch (lastLeftButton) {
          case 0x20: triggerGamepadButton(3, false); break;
          case 0x07: triggerGamepadButton(4, false); break;
          case 0x03: triggerGamepadButton(8, false); break;
          case 0x02: triggerGamepadButton(9, false); break;
          case 0x15: triggerGamepadButton(10, false); break;
          case 0x16: triggerGamepadButton(11, false); break;
          case 0x19: triggerGamepadButton(12, false); break;
          case 0x23: triggerGamepadButton(13, false); break;
          case 0x25: triggerGamepadButton(14, false); break;
          case 0x70: triggerGamepadButton(15, false); break;
          case 0x74: triggerGamepadButton(16, false); break;
        }
      }
    }

    lastLeftButton = b1;
    lastLeftDirection = b3;
    lastLeftWasCombo = false;

    if (b1 != 0x00) {
      switch (b1) {
        case 0x12: triggerGamepadButton((b3 == 0x01) ? 1 : 2, true); break;
        case 0x20: triggerGamepadButton(3, true); break;
        case 0x07: triggerGamepadButton(4, true); break;
        case 0x06:
          if (cupraHeld) {
            // Kombo: Cupra-Taste + Scrollrad -> Beleuchtung dimmen statt Gamepad-Button
            cupraComboUsed = true;
            lastLeftWasCombo = true;
            adjustBacklight((b3 == 0x01) ? BACKLIGHT_STEP : -BACKLIGHT_STEP);
          } else {
            triggerGamepadButton((b3 == 0x01) ? 6 : 7, true);
          }
          break;
        case 0x03: triggerGamepadButton(8, true); break;
        case 0x02: triggerGamepadButton(9, true); break;
        case 0x15: triggerGamepadButton(10, true); break;
        case 0x16: triggerGamepadButton(11, true); break;
        case 0x19: triggerGamepadButton(12, true); break;
        case 0x23: triggerGamepadButton(13, true); break;
        case 0x25: triggerGamepadButton(14, true); break;
        case 0x70: triggerGamepadButton(15, true); break;
        case 0x74: triggerGamepadButton(16, true); break;
      }
    }
  }

  if (b6 != lastWippenByte) {
    lastWippenByte = b6;
    triggerGamepadButton(17, false);
    triggerGamepadButton(18, false);

    switch (b6) {
      case 0x01: triggerGamepadButton(17, true); break;
      case 0x02: triggerGamepadButton(18, true); break;
      case 0x03:
        triggerGamepadButton(17, true);
        triggerGamepadButton(18, true);
        break;
    }
  }
}

void interpretRightIsland(uint8_t b2) {
  if (b2 == lastRightButton) return;

  switch (lastRightButton) {
    case 0x81: triggerGamepadButton(19, false); break;
    case 0x82: triggerGamepadButton(20, false); break;
    case 0x84: triggerGamepadButton(21, false); break;
    case 0x88: triggerGamepadButton(22, false); break;
    case 0xB0: triggerGamepadButton(23, false); break;
  }

  lastRightButton = b2;

  if (b2 != 0x80) {
    switch (b2) {
      case 0x81: triggerGamepadButton(19, true); break;
      case 0x82: triggerGamepadButton(20, true); break;
      case 0x84: triggerGamepadButton(21, true); break;
      case 0x88: triggerGamepadButton(22, true); break;
      case 0xB0: triggerGamepadButton(23, true); break;
    }
  }
}

void pollSlave(uint8_t id) {
  uint8_t pid = calculateLINPID(id);
  generatePhysicalHeader();

  Serial1.write(0x55);
  Serial1.write(pid);
  Serial1.flush();

  uint8_t rawBuffer[15];
  uint8_t idx = 0;

  unsigned long startTime = micros();
  while ((micros() - startTime < 6000) && (idx < 11)) {
    if (Serial1.available()) {
      rawBuffer[idx++] = Serial1.read();
    }
  }

  if (idx >= 10) {
    unsigned long now = millis();

    if (id == 0x0E) {
      uint8_t b1 = debounceUpdate(dbLeftB1, rawBuffer[3], now, DEBOUNCE_MS_LIN);
      uint8_t b3 = debounceUpdate(dbLeftB3, rawBuffer[5], now, DEBOUNCE_MS_LIN);
      uint8_t b6 = debounceUpdate(dbLeftB6, rawBuffer[8], now, DEBOUNCE_MS_LIN);
      interpretLeftIsland(b1, b3, b6);
    } else if (id == 0x0F) {
      uint8_t b2 = debounceUpdate(dbRightB2, rawBuffer[4], now, DEBOUNCE_MS_LIN);
      interpretRightIsland(b2);
    }
  }
}

void sendBacklight() {
  uint8_t pid = calculateLINPID(0x0D);
  uint8_t payload[4] = {
    backlightOn ? backlightBrightness : BACKLIGHT_MIN,
    BACKLIGHT_RESPONSE_BYTE,
    0xFF,
    0xFF
  };
  uint8_t checksum = checksumEnhanced(pid, payload, 4);

  generatePhysicalHeader();
  Serial1.write(0x55);
  Serial1.write(pid);
  for (uint8_t i = 0; i < 4; i++) Serial1.write(payload[i]);
  Serial1.write(checksum);
  Serial1.flush();
  delay(2);
}

void checkAnalogButtons() {
  unsigned long now = millis();

  // --- BTN24 (ACC_WEISS) ---
  bool rawAcc = digitalRead(PIN_ACC_WEISS);
  bool currentAcc = debounceUpdate(dbAcc, rawAcc ? 1 : 0, now, DEBOUNCE_MS_ACC) != 0;
  if (currentAcc != lastAccState) {
    lastAccState = currentAcc;
    triggerGamepadButton(24, (currentAcc == LOW));
  }

  // --- BTN25 (START_GELB) ---
  int adcGelb = analogRead(PIN_START_GELB);
  bool rawStart = (adcGelb < 3800);
  bool currentStart = debounceUpdate(dbStart, rawStart ? 1 : 0, now, DEBOUNCE_MS_START) != 0;
  if (currentStart != lastStartState) {
    lastStartState = currentStart;
    triggerGamepadButton(25, currentStart);
  }
}

// ---------------------------------------------------------------------
// Cupra-Taste: Modifier für die Backlight-Kombo + Tap = Ein/Aus
// ---------------------------------------------------------------------
void checkCupraButton() {
  unsigned long now = millis();

  // TODO: Pegel prüfen/anpassen (hier: Taster gegen GND, INPUT_PULLUP -> LOW = gedrückt)
  bool raw = (digitalRead(PIN_CUPRA_BTN) == LOW);
  bool debounced = debounceUpdate(dbCupra, raw ? 1 : 0, now, DEBOUNCE_MS_CUPRA) != 0;

  if (debounced && !lastCupraState) {
    // Flanke: Taste gerade gedrückt
    cupraHeld = true;
    cupraComboUsed = false;
  } else if (!debounced && lastCupraState) {
    // Flanke: Taste losgelassen
    cupraHeld = false;
    if (!cupraComboUsed) {
      // Reiner Tap ohne Scroll-Kombo -> Licht an/aus umschalten
      toggleBacklight();
    }
  }

  lastCupraState = debounced;
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_ACC_WEISS, INPUT);
  pinMode(PIN_START_GELB, INPUT_PULLUP);
  pinMode(PIN_CUPRA_BTN, INPUT_PULLUP);

  pinMode(PIN_SLP_N, OUTPUT);
  digitalWrite(PIN_SLP_N, HIGH);
  delay(5);

  BleGamepadConfiguration bleGamepadConfig;
  bleGamepadConfig.setAutoReport(false);
  bleGamepadConfig.setButtonCount(32);

  bleGamepad.begin(&bleGamepadConfig);
}

void loop() {
  unsigned long currentMillis = millis();

  if (currentMillis - lastUdsTime >= 1000) {
    lastUdsTime = currentMillis;
    sendUdsInitialization();
  }

  if (currentMillis - lastTxTime >= 20) {
    lastTxTime = currentMillis;

    switch (scheduleStep) {
      case 0: sendBacklight(); break;
      case 1: pollSlave(0x0E); break;
      case 2: pollSlave(0x0F); break;
    }
    scheduleStep = (scheduleStep + 1) % 3;
  }

  checkAnalogButtons();
  checkCupraButton();

  // INTERNER SIMHUB-STANDARD:
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    if (cmd == '2') {
      Serial.write(0x03);
      Serial.write((uint8_t)(simhubButtons & 0xFF));
      Serial.write((uint8_t)((simhubButtons >> 8) & 0xFF));
      Serial.write((uint8_t)((simhubButtons >> 16) & 0xFF));
      Serial.write((uint8_t)((simhubButtons >> 24) & 0xFF));
    }
  }
}
