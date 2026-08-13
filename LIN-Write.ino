#include <Arduino.h>
#include <BleGamepad.h>

#define LIN_RX 16
#define LIN_TX 17
#define LIN_BAUD 19200
#define PIN_SLP_N 32

#define PIN_ACC_WEISS 25
#define PIN_START_GELB 26

// --- NEU: Entprellzeiten (nur für die direkten GPIO/ADC-Taster BTN24 & BTN25) ---
// Bewusst kurz gehalten, damit keine spürbare Latenz entsteht.
#define DEBOUNCE_MS_ACC   15
#define DEBOUNCE_MS_START 15

BleGamepad bleGamepad("Cupra Wheel", "VAG Retrofit", 100);

unsigned long lastTxTime = 0;
unsigned long lastUdsTime = 0;
uint8_t scheduleStep = 0;

uint8_t lastLeftButton = 0;
uint8_t lastLeftDirection = 0;
uint8_t lastRightButton = 0;
uint8_t lastWippenByte = 0;
bool lastAccState = HIGH;
bool lastStartState = false;

// --- NEU: Zwischenspeicher für die Entprellung ---
bool pendingAccState = HIGH;
unsigned long accChangeTime = 0;

bool pendingStartState = false;
unsigned long startChangeTime = 0;

uint32_t simhubButtons = 0;

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

// --- NEU: prüft die vom Slave empfangene Checksumme gegen die berechnete ---
bool verifyLinChecksum(uint8_t pid, const uint8_t* data, uint8_t len, uint8_t receivedChecksum) {
  return checksumEnhanced(pid, data, len) == receivedChecksum;
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

void interpretLeftIsland(uint8_t b1, uint8_t b3, uint8_t b6) {
  if (b1 != lastLeftButton || b3 != lastLeftDirection) {
    if (lastLeftButton != 0x00) {
      if (lastLeftButton == 0x12) {
        triggerGamepadButton(1, false);
        triggerGamepadButton(2, false);
      } else if (lastLeftButton == 0x06) {
        triggerGamepadButton(6, false);
        triggerGamepadButton(7, false);
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

    if (b1 != 0x00) {
      switch (b1) {
        case 0x12: triggerGamepadButton((b3 == 0x01) ? 1 : 2, true); break;
        case 0x20: triggerGamepadButton(3, true); break;
        case 0x07: triggerGamepadButton(4, true); break;
        case 0x06: triggerGamepadButton((b3 == 0x01) ? 6 : 7, true); break;
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
    // --- NEU: Checksumme des empfangenen Frames prüfen, bevor wir ihn verwerten.
    // Das ist der eigentliche Fix für "keine Mechanik gegen Prellen/Störungen":
    // Ein durch Störung verfälschtes Byte erzeugt sonst kurzzeitig einen falschen
    // Tastencode -> Phantom-Press/-Release, obwohl die Taste real durchgehend
    // gehalten wird. Schlägt die Checksumme fehl, wird der Frame komplett
    // verworfen und der zuletzt bestätigte Zustand bleibt bestehen - ohne dass
    // dafür irgendeine künstliche Verzögerung nötig ist. Reaktionsgeschwindigkeit
    // von Wippen/+-/Cupra-Taste bleibt dadurch unverändert schnell.
    uint8_t dataLen = idx - 3; // abzüglich 0x55, PID, Checksumme
    bool checksumOk = verifyLinChecksum(rawBuffer[1], &rawBuffer[2], dataLen, rawBuffer[idx - 1]);

    if (checksumOk) {
      if (id == 0x0E) {
        interpretLeftIsland(rawBuffer[3], rawBuffer[5], rawBuffer[8]);
      } else if (id == 0x0F) {
        interpretRightIsland(rawBuffer[4]);
      }
    }
  }
}

void sendBacklight() {
  uint8_t pid = calculateLINPID(0x0D);
  uint8_t payload[4] = { 0x7F, 0xF9, 0xFF, 0xFF };
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

  // --- BTN24 (ACC_WEISS) - jetzt entprellt ---
  // Vorher: jede rohe Pegeländerung wurde sofort durchgereicht -> bei Kontaktprellen
  // bzw. Störspannung auf der Leitung feuerte der Button mehrfach hintereinander.
  // Jetzt: ein neuer Pegel muss DEBOUNCE_MS_ACC lang stabil bleiben, bevor er
  // übernommen wird.
  bool rawAcc = digitalRead(PIN_ACC_WEISS);
  if (rawAcc != pendingAccState) {
    pendingAccState = rawAcc;
    accChangeTime = now;
  }
  if ((now - accChangeTime) >= DEBOUNCE_MS_ACC && pendingAccState != lastAccState) {
    lastAccState = pendingAccState;
    triggerGamepadButton(24, (lastAccState == LOW));
  }

  // --- BTN25 (START_GELB) - gleiche Entprellung ---
  int adcGelb = analogRead(PIN_START_GELB);
  bool rawStart = (adcGelb < 3800);
  if (rawStart != pendingStartState) {
    pendingStartState = rawStart;
    startChangeTime = now;
  }
  if ((now - startChangeTime) >= DEBOUNCE_MS_START && pendingStartState != lastStartState) {
    lastStartState = pendingStartState;
    triggerGamepadButton(25, lastStartState);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_ACC_WEISS, INPUT);
  pinMode(PIN_START_GELB, INPUT_PULLUP);

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
