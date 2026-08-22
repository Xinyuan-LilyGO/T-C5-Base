/**
 * @file      rs485_speed_test.ino
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2026  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2026-08-21
 * @note      T-C5-Base Shield RS485 speed test.
 *
 *            Select RS4851 or RS4852 with ACTIVE_RS485. The sketch can continuously transmit
 *            test blocks and reports TX/RX throughput once per second.
 * 
 *            For high baud rates or long cables, add RS485
 *            termination according to your wiring.
 */

#include <Arduino.h>
#include <inttypes.h>

#ifndef ACTIVE_RS485
#define ACTIVE_RS485 1
#endif
#ifndef RS485_BAUD
#define RS485_BAUD 921600
#endif
#define ENABLE_TX_TEST 1
#define TX_BLOCK_SIZE 256
#define REPORT_INTERVAL_MS 1000
#define RS485_TX_ENABLE_LEVEL HIGH
#define RS485_RX_ENABLE_LEVEL LOW

#if ACTIVE_RS485 != 1 && ACTIVE_RS485 != 2
#error "ACTIVE_RS485 must be 1 or 2"
#endif

#if ACTIVE_RS485 == 1 && !ARDUINO_USB_CDC_ON_BOOT
#error "RS4851 uses UART0/Serial0. Enable ARDUINO_USB_CDC_ON_BOOT=1 so Serial remains USB CDC."
#endif

// RS4851 pins
#define RS4851_DE_PIN 27
#define RS4851_TX_PIN 5
#define RS4851_RX_PIN 4
// RS4852 pins
#define RS4852_TX_PIN 23
#define RS4852_RX_PIN 24
#define RS4852_DE_PIN 7

static constexpr const char *RS485_NAME = ACTIVE_RS485 == 1 ? "RS4851" : "RS4852";
static constexpr int RS485_RX_PIN = ACTIVE_RS485 == 1 ? RS4851_RX_PIN : RS4852_RX_PIN;
static constexpr int RS485_TX_PIN = ACTIVE_RS485 == 1 ? RS4851_TX_PIN : RS4852_TX_PIN;
static constexpr int RS485_DTR_PIN = ACTIVE_RS485 == 1 ? RS4851_DE_PIN : RS4852_DE_PIN;

static bool rs485Ready = false;
static uint8_t txBuffer[TX_BLOCK_SIZE];
static uint8_t rxBuffer[256];
static uint32_t sequence = 0;
static uint32_t txBytes = 0;
static uint32_t txBlocks = 0;
static uint32_t rxBytes = 0;
static uint32_t lastReportMs = 0;

static HardwareSerial &rs485Port() {
  if (ACTIVE_RS485 == 1) {
    return Serial0;
  }
  return Serial1;
}

static bool startRs485() {
  rs485Port().setRxBufferSize(4096);
  rs485Port().begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  pinMode(RS485_DTR_PIN, OUTPUT);
  digitalWrite(RS485_DTR_PIN, RS485_RX_ENABLE_LEVEL);

  Serial.printf("[%s] speed test started, RX=GPIO%d TX=GPIO%d DTR=GPIO%d baud=%d\n",
                RS485_NAME, RS485_RX_PIN, RS485_TX_PIN, RS485_DTR_PIN, RS485_BAUD);
  return true;
}

static void fillTxBuffer() {
  uint32_t value = sequence++;
  for (size_t i = 0; i < sizeof(txBuffer); ++i) {
    txBuffer[i] = (uint8_t)(value + i);
  }
}

static void transmitBlock() {
  fillTxBuffer();
  digitalWrite(RS485_DTR_PIN, RS485_TX_ENABLE_LEVEL);
  delayMicroseconds(100);
  size_t written = 0;
  while (written < sizeof(txBuffer)) {
    if (rs485Port().write(txBuffer[written]) == 1) {
      ++written;
    } else {
      delayMicroseconds(10);
    }
  }
  rs485Port().flush();
  delayMicroseconds(100);
  digitalWrite(RS485_DTR_PIN, RS485_RX_ENABLE_LEVEL);
  txBytes += written;
  ++txBlocks;
}

static void drainRx() {
  int available = rs485Port().available();
  while (available > 0) {
    size_t toRead = MIN((size_t)available, sizeof(rxBuffer));
    size_t readLen = rs485Port().read(rxBuffer, toRead);
    rxBytes += readLen;
    available = rs485Port().available();
  }
}

static void reportSpeed() {
  uint32_t now = millis();
  if (now - lastReportMs < REPORT_INTERVAL_MS) {
    return;
  }

  uint32_t elapsedMs = now - lastReportMs;
  lastReportMs = now;

  uint32_t txRate = (txBytes * 1000UL) / elapsedMs;
  uint32_t rxRate = (rxBytes * 1000UL) / elapsedMs;

  Serial.printf("[%s] TX=%" PRIu32 " B/s blocks=%" PRIu32 " | RX=%" PRIu32 " B/s\n",
                RS485_NAME, txRate, txBlocks, rxRate);

  txBytes = 0;
  txBlocks = 0;
  rxBytes = 0;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("T-C5-Base RS485 speed test");

  rs485Ready = startRs485();
  lastReportMs = millis();
}

void loop() {
  if (!rs485Ready) {
    delay(1000);
    return;
  }

#if ENABLE_TX_TEST
  transmitBlock();
#endif

  drainRx();
  reportSpeed();
  delay(1);
}
