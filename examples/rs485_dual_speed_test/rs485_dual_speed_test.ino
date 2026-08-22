/**
 * @file      rs485_dual_speed_test.ino
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2026  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2026-08-21
 * @note      T-C5-Base Shield dual RS485 Arduino speed test.
 * 
 *          Connect RS4851 and RS4852 to the same half-duplex RS485 bus:
 *          RS4851_A <-> RS4852_A, RS4851_B <-> RS4852_B, and GND <-> GND.
*/

#include <Arduino.h>
#include <inttypes.h>
#include "SSD1306Wire.h"

#if !ARDUINO_USB_CDC_ON_BOOT
#error "This example needs USB CDC enabled because UART0/Serial0 is used for RS4851. Enable ARDUINO_USB_CDC_ON_BOOT=1."
#endif

#ifndef RS485_BAUD
#define RS485_BAUD 4000000
#endif
#define TEST_PAYLOAD_SIZE 224
#define RECEIVE_TIMEOUT_US 30000
#define TURNAROUND_DELAY_US 200
#define REPORT_INTERVAL_MS 1000
#define AUTODETECT_LINK 1
#define AUTODETECT_BAUD 115200
#define OLED_I2C_ADDRESS 0x3C
#define OLED_SDA_PIN 2
#define OLED_SCL_PIN 3
#ifndef RS485_ENABLE_OLED
#define RS485_ENABLE_OLED 1
#endif

// RS4851 pins
#define RS4851_DE_PIN 27
#define RS4851_TX_PIN 5
#define RS4851_RX_PIN 4
// RS4852 pins
#define RS4852_TX_PIN 23
#define RS4852_RX_PIN 24
#define RS4852_DE_PIN 7

struct Rs485Bus {
  const char *name;
  HardwareSerial *port;
  int rxPin;
  int txPin;
  int dePin;
  uint32_t txBytes;
  uint32_t rxBytes;
  uint32_t rawRxBytes;
  uint32_t goodFrames;
  uint32_t badFrames;
  uint32_t timeouts;
};

struct LinkConfig {
  int rs4851RxPin;
  int rs4851TxPin;
  int rs4852RxPin;
  int rs4852TxPin;
  uint8_t txEnableLevel;
  const char *name;
};

struct __attribute__((packed)) TestFrame {
  uint32_t magic;
  uint8_t direction;
  uint8_t reserved;
  uint16_t payloadLength;
  uint32_t sequence;
  uint32_t checksum;
  uint8_t payload[TEST_PAYLOAD_SIZE];
};

static constexpr uint32_t FRAME_MAGIC = 0x35433854UL;
static constexpr size_t FRAME_SIZE = sizeof(TestFrame);
static constexpr uint8_t DIR_1_TO_2 = 0x12;
static constexpr uint8_t DIR_2_TO_1 = 0x21;

static Rs485Bus rs4851 = {"RS4851", &Serial0, RS4851_RX_PIN, RS4851_TX_PIN, RS4851_DE_PIN, 0, 0, 0, 0, 0, 0};
static Rs485Bus rs4852 = {"RS4852", &Serial1, RS4852_RX_PIN, RS4852_TX_PIN, RS4852_DE_PIN, 0, 0, 0, 0, 0, 0};

static constexpr LinkConfig LINK_CONFIGS[] = {
    {RS4851_RX_PIN, RS4851_TX_PIN, RS4852_RX_PIN, RS4852_TX_PIN, HIGH, "normal pins, DE high"},
    {RS4851_RX_PIN, RS4851_TX_PIN, RS4852_RX_PIN, RS4852_TX_PIN, LOW, "normal pins, DE low"},
    {RS4851_TX_PIN, RS4851_RX_PIN, RS4852_RX_PIN, RS4852_TX_PIN, HIGH, "RS4851 swapped, DE high"},
    {RS4851_TX_PIN, RS4851_RX_PIN, RS4852_RX_PIN, RS4852_TX_PIN, LOW, "RS4851 swapped, DE low"},
    {RS4852_TX_PIN, RS4852_RX_PIN, RS4851_RX_PIN, RS4851_TX_PIN, HIGH, "RS4852 swapped, DE high"},
    {RS4852_TX_PIN, RS4852_RX_PIN, RS4851_RX_PIN, RS4851_TX_PIN, LOW, "RS4852 swapped, DE low"},
    {RS4851_TX_PIN, RS4851_RX_PIN, RS4852_TX_PIN, RS4852_RX_PIN, HIGH, "both swapped, DE high"},
    {RS4851_TX_PIN, RS4851_RX_PIN, RS4852_TX_PIN, RS4852_RX_PIN, LOW, "both swapped, DE low"},
};

static uint8_t rs485TxEnableLevel = HIGH;
static uint8_t rs485RxEnableLevel = LOW;
static uint32_t activeBaud = RS485_BAUD;

static uint32_t sequence12 = 0;
static uint32_t sequence21 = 0;
static uint32_t lastReportMs = 0;
static bool portsReady = false;
static SSD1306Wire oled(OLED_I2C_ADDRESS, OLED_SDA_PIN, OLED_SCL_PIN);
static bool oledReady = false;
static bool oledStatusValid = false;
static bool oledLastPass = false;
static bool oledLastPass12 = false;
static bool oledLastPass21 = false;

static uint32_t checksumFrame(const TestFrame &frame) {
  uint32_t sum = frame.magic;
  sum += frame.direction;
  sum += frame.reserved;
  sum += frame.payloadLength;
  sum += frame.sequence;

  for (size_t i = 0; i < frame.payloadLength && i < sizeof(frame.payload); ++i) {
    sum = (sum << 5) | (sum >> 27);
    sum += frame.payload[i];
  }

  return sum;
}

static void fillFrame(TestFrame &frame, uint8_t direction, uint32_t sequence) {
  frame.magic = FRAME_MAGIC;
  frame.direction = direction;
  frame.reserved = 0;
  frame.payloadLength = TEST_PAYLOAD_SIZE;
  frame.sequence = sequence;

  for (size_t i = 0; i < sizeof(frame.payload); ++i) {
    frame.payload[i] = (uint8_t)(sequence + direction + i);
  }

  frame.checksum = checksumFrame(frame);
}

static bool verifyFrame(const TestFrame &frame, uint8_t expectedDirection, uint32_t expectedSequence) {
  if (frame.magic != FRAME_MAGIC ||
      frame.direction != expectedDirection ||
      frame.payloadLength != TEST_PAYLOAD_SIZE ||
      frame.sequence != expectedSequence) {
    return false;
  }

  return frame.checksum == checksumFrame(frame);
}

static void applyLinkConfig(const LinkConfig &config) {
  rs4851.rxPin = config.rs4851RxPin;
  rs4851.txPin = config.rs4851TxPin;
  rs4852.rxPin = config.rs4852RxPin;
  rs4852.txPin = config.rs4852TxPin;
  rs485TxEnableLevel = config.txEnableLevel;
  rs485RxEnableLevel = config.txEnableLevel == HIGH ? LOW : HIGH;
}

static void resetCounters() {
  rs4851.txBytes = 0;
  rs4851.rxBytes = 0;
  rs4851.rawRxBytes = 0;
  rs4851.goodFrames = 0;
  rs4851.badFrames = 0;
  rs4851.timeouts = 0;

  rs4852.txBytes = 0;
  rs4852.rxBytes = 0;
  rs4852.rawRxBytes = 0;
  rs4852.goodFrames = 0;
  rs4852.badFrames = 0;
  rs4852.timeouts = 0;
}

static bool startBus(Rs485Bus &bus) {
  pinMode(bus.dePin, OUTPUT);
  digitalWrite(bus.dePin, rs485RxEnableLevel);

  bus.port->end();
  bus.port->setRxBufferSize(4096);
  bus.port->setTimeout(1);
  bus.port->begin(activeBaud, SERIAL_8N1, bus.rxPin, bus.txPin);

  digitalWrite(bus.dePin, rs485RxEnableLevel);

  while (bus.port->available()) {
    bus.port->read();
  }

  Serial.printf("[%s] started, RX=GPIO%d TX=GPIO%d DE=GPIO%d baud=%" PRIu32 "\n",
                bus.name, bus.rxPin, bus.txPin, bus.dePin, activeBaud);
  return true;
}

static void drainRx(Rs485Bus &bus) {
  while (bus.port->available()) {
    bus.port->read();
  }
}

static size_t readExact(Rs485Bus &bus, uint8_t *buffer, size_t length, uint32_t timeoutUs) {
  size_t received = 0;
  uint32_t startUs = micros();

  while (received < length && (uint32_t)(micros() - startUs) < timeoutUs) {
    int value = bus.port->read();
    if (value < 0) {
      delayMicroseconds(20);
      continue;
    }

    buffer[received++] = (uint8_t)value;
    ++bus.rawRxBytes;
  }

  return received;
}

static size_t writeBuffer(HardwareSerial &port, const uint8_t *buffer, size_t length) {
  size_t written = 0;

  while (written < length) {
    if (port.write(buffer[written]) == 1) {
      ++written;
    } else {
      delayMicroseconds(10);
    }
  }

  return written;
}

static void sendFrame(Rs485Bus &bus, const TestFrame &frame) {
  digitalWrite(bus.dePin, rs485TxEnableLevel);
  delayMicroseconds(TURNAROUND_DELAY_US);
  size_t written = writeBuffer(*bus.port, (const uint8_t *)&frame, FRAME_SIZE);
  bus.port->flush();
  delayMicroseconds(TURNAROUND_DELAY_US);
  digitalWrite(bus.dePin, rs485RxEnableLevel);
  bus.txBytes += (uint32_t)written;
}

static void transferFrame(Rs485Bus &txBus,
                          Rs485Bus &rxBus,
                          uint8_t direction,
                          uint32_t &sequence) {
  TestFrame txFrame = {};
  TestFrame rxFrame = {};
  fillFrame(txFrame, direction, sequence);

  drainRx(rxBus);
  sendFrame(txBus, txFrame);

  size_t readLen = readExact(rxBus, (uint8_t *)&rxFrame, FRAME_SIZE, RECEIVE_TIMEOUT_US);
  if (readLen != FRAME_SIZE) {
    ++txBus.timeouts;
    ++rxBus.timeouts;
    ++rxBus.badFrames;
    ++sequence;
    return;
  }

  rxBus.rxBytes += (uint32_t)readLen;
  if (verifyFrame(rxFrame, direction, sequence)) {
    ++rxBus.goodFrames;
  } else {
    ++rxBus.badFrames;
  }

  ++sequence;
}

static bool runProbeFrame(Rs485Bus &txBus,
                          Rs485Bus &rxBus,
                          uint8_t direction,
                          uint32_t sequence) {
  TestFrame txFrame = {};
  TestFrame rxFrame = {};
  fillFrame(txFrame, direction, sequence);

  drainRx(rxBus);
  sendFrame(txBus, txFrame);
  size_t readLen = readExact(rxBus, (uint8_t *)&rxFrame, FRAME_SIZE, 200000);

  bool ok = readLen == FRAME_SIZE && verifyFrame(rxFrame, direction, sequence);
  Serial.printf("  %s->%s read=%u raw=%" PRIu32 " %s\n",
                txBus.name, rxBus.name, (unsigned)readLen,
                rxBus.rawRxBytes, ok ? "OK" : "FAIL");
  return ok;
}

static bool autodetectLinkConfig() {
  bool found = false;

  Serial.println("Autodetecting RS485 pin order and DE polarity at 115200 baud...");
  activeBaud = AUTODETECT_BAUD;
  for (const LinkConfig &config : LINK_CONFIGS) {
    applyLinkConfig(config);
    resetCounters();

    Serial.printf("[probe] %s: RS4851 RX=%d TX=%d, RS4852 RX=%d TX=%d, TX_EN=%s\n",
                  config.name, rs4851.rxPin, rs4851.txPin, rs4852.rxPin, rs4852.txPin,
                  rs485TxEnableLevel == HIGH ? "HIGH" : "LOW");

    startBus(rs4851);
    startBus(rs4852);

    bool ok12 = runProbeFrame(rs4851, rs4852, DIR_1_TO_2, 0);
    bool ok21 = runProbeFrame(rs4852, rs4851, DIR_2_TO_1, 0);
    if (ok12 && ok21) {
      Serial.printf("[probe] selected: %s\n", config.name);
      found = true;
      break;
    }
  }

  if (!found) {
    applyLinkConfig(LINK_CONFIGS[0]);
    Serial.println("[probe] no working RS485 link found; using default config for continuous diagnostics");
  }

  resetCounters();
  return found;
}

static bool detectOled() {
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.beginTransmission(OLED_I2C_ADDRESS);
  if (Wire.endTransmission() != 0) {
      return false;
  }
  return true;
}

static void drawOledBoot(const char *line2) {
  if (!oledReady) {
    return;
  }

  oled.clear();
  oled.drawString(0, 0, "T-C5-Base");
  oled.drawString(0, 12, "RS485 speed test");
  oled.drawString(0, 24, line2);
  oled.display();
}

static void initOled() {
#if !RS485_ENABLE_OLED
  Serial.println("[OLED] disabled by RS485_ENABLE_OLED=0");
  return;
#endif

  if (!detectOled()) {
    Serial.printf("[OLED] not found at 0x%02X SDA=GPIO%d SCL=GPIO%d\n",
                  OLED_I2C_ADDRESS, OLED_SDA_PIN, OLED_SCL_PIN);
    return;
  }

  oled.init();
  oled.setFont(ArialMT_Plain_10);
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  oled.setContrast(255);
  oledReady = true;
  Serial.printf("[OLED] started, address=0x%02X SDA=GPIO%d SCL=GPIO%d\n",
                OLED_I2C_ADDRESS, OLED_SDA_PIN, OLED_SCL_PIN);
}

static void updateOledRs485(uint32_t rx12Rate, uint32_t rx21Rate) {
  if (!oledReady) {
    return;
  }

  const bool pass12 = portsReady &&
                      rs4852.goodFrames > 0 &&
                      rs4851.badFrames == 0 &&
                      rs4852.badFrames == 0 &&
                      rs4851.timeouts == 0 &&
                      rs4852.timeouts == 0;
  const bool pass21 = portsReady &&
                      rs4851.goodFrames > 0 &&
                      rs4851.badFrames == 0 &&
                      rs4852.badFrames == 0 &&
                      rs4851.timeouts == 0 &&
                      rs4852.timeouts == 0;
  const bool pass = pass12 && pass21;

  if (oledStatusValid &&
      pass == oledLastPass &&
      pass12 == oledLastPass12 &&
      pass21 == oledLastPass21) {
    return;
  }

  oledStatusValid = true;
  oledLastPass = pass;
  oledLastPass12 = pass12;
  oledLastPass21 = pass21;

  oled.clear();
  oled.drawString(0, 0, String("RS485 ") + String(activeBaud) + "bps");
  oled.drawString(0, 12, pass ? "PASS" : "FAIL");
  oled.drawString(0, 24, String("1>2 ") + (pass12 ? "OK " : "NG ") +
                            String(rx12Rate) + "B/s");
  oled.drawString(0, 36, String("2>1 ") + (pass21 ? "OK " : "NG ") +
                            String(rx21Rate) + "B/s");
  oled.drawString(0, 48, String("Bad ") + String(rs4851.badFrames + rs4852.badFrames) +
                            " Tout " + String(rs4851.timeouts + rs4852.timeouts));
  oled.display();
}

static void updateOledRs485InitFailed() {
  if (!oledReady) {
    return;
  }

  oled.clear();
  oled.drawString(0, 0, "RS485 FAIL");
  oled.drawString(0, 12, "Port/link failed");
  oled.drawString(0, 24, "Serial0 Serial1");
  oled.display();
}

static void reportSpeed() {
  uint32_t now = millis();
  if (now - lastReportMs < REPORT_INTERVAL_MS) {
    return;
  }

  uint32_t elapsedMs = now - lastReportMs;
  lastReportMs = now;

  uint32_t tx12Rate = (rs4851.txBytes * 1000UL) / elapsedMs;
  uint32_t rx12Rate = (rs4852.rxBytes * 1000UL) / elapsedMs;
  uint32_t tx21Rate = (rs4852.txBytes * 1000UL) / elapsedMs;
  uint32_t rx21Rate = (rs4851.rxBytes * 1000UL) / elapsedMs;

  Serial.printf("[RS4851->RS4852] TX=%" PRIu32 " B/s RX=%" PRIu32
                " B/s raw=%" PRIu32 " good=%" PRIu32 " bad=%" PRIu32 " timeout=%" PRIu32 "\n",
                tx12Rate, rx12Rate, rs4852.rawRxBytes, rs4852.goodFrames, rs4852.badFrames, rs4852.timeouts);
  Serial.printf("[RS4852->RS4851] TX=%" PRIu32 " B/s RX=%" PRIu32
                " B/s raw=%" PRIu32 " good=%" PRIu32 " bad=%" PRIu32 " timeout=%" PRIu32 "\n",
                tx21Rate, rx21Rate, rs4851.rawRxBytes, rs4851.goodFrames, rs4851.badFrames, rs4851.timeouts);

  updateOledRs485(rx12Rate, rx21Rate);
  resetCounters();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("T-C5-Base dual RS485 Arduino speed test");

#if AUTODETECT_LINK
  portsReady = autodetectLinkConfig();
  activeBaud = RS485_BAUD;
#endif

  bool ready1 = startBus(rs4851);
  bool ready2 = startBus(rs4852);
#if AUTODETECT_LINK
  portsReady = portsReady && ready1 && ready2;
#else
  portsReady = ready1 && ready2;
#endif
  initOled();
  if (portsReady) {
    drawOledBoot("RS485 ports started");
  } else {
    updateOledRs485InitFailed();
  }
  lastReportMs = millis();
}

void loop() {
  if (!portsReady) {
    delay(1000);
    return;
  }

  transferFrame(rs4851, rs4852, DIR_1_TO_2, sequence12);
  transferFrame(rs4852, rs4851, DIR_2_TO_1, sequence21);
  reportSpeed();
}
