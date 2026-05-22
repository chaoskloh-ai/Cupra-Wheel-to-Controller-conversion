#include <Arduino.h>
#include <BleGamepad.h>

#define LIN_RX 16
#define LIN_TX 17
#define LIN_BAUD 19200
#define PIN_SLP_N 32

#define PIN_ACC_WEISS 25
#define PIN_START_GELB 26

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
    if (id == 0x0E) {
      interpretLeftIsland(rawBuffer[3], rawBuffer[5], rawBuffer[8]);
    } else if (id == 0x0F) {
      interpretRightIsland(rawBuffer[4]);
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
  bool currentAcc = digitalRead(PIN_ACC_WEISS);
  if (currentAcc != lastAccState) {
    lastAccState = currentAcc;
    triggerGamepadButton(24, (currentAcc == LOW));
  }

  int adcGelb = analogRead(PIN_START_GELB);
  bool currentStart = (adcGelb < 3800);
  if (currentStart != lastStartState) {
    lastStartState = currentStart;
    triggerGamepadButton(25, currentStart);
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