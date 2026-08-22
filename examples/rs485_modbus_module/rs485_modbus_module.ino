/**
 * @file      rs485_modbus_module.ino
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2026  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2026-08-21
 * @note      T-C5-Base Shield Modbus RTU over RS485 module example.
 *
 *            Default role is a Modbus RTU master. It reads holding registers from a slave
 *            module with function code 0x03.
 *
 *            For high baud rates or long cables, add RS485
 *            termination according to your wiring.
 */

#include <Arduino.h>
#include <inttypes.h>

#ifndef ACTIVE_RS485
#define ACTIVE_RS485 1
#endif
#define MODBUS_BAUD 9600
#define MODBUS_SLAVE_ID 1
#define MODBUS_START_REGISTER 0x0000
#define MODBUS_REGISTER_COUNT 2
#define MODBUS_POLL_INTERVAL_MS 2000
#define MODBUS_RESPONSE_TIMEOUT_MS 1000
#define MODBUS_MAX_REGISTERS 16
#define RS485_TX_ENABLE_LEVEL HIGH
#define RS485_RX_ENABLE_LEVEL LOW

#if MODBUS_REGISTER_COUNT > MODBUS_MAX_REGISTERS
#error "MODBUS_REGISTER_COUNT exceeds MODBUS_MAX_REGISTERS"
#endif

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
static uint32_t lastPollMs = 0;

static HardwareSerial &rs485Port() {
  if (ACTIVE_RS485 == 1) {
    return Serial0;
  }
  return Serial1;
}

static uint16_t modbusCrc16(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;

  for (size_t pos = 0; pos < length; ++pos) {
    crc ^= data[pos];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      if (crc & 0x0001) {
        crc = (crc >> 1) ^ 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }

  return crc;
}

static bool startRs485() {
  rs485Port().setRxBufferSize(512);
  rs485Port().begin(MODBUS_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  pinMode(RS485_DTR_PIN, OUTPUT);
  digitalWrite(RS485_DTR_PIN, RS485_RX_ENABLE_LEVEL);

  Serial.printf("[%s] Modbus RTU master started, RX=GPIO%d TX=GPIO%d DTR=GPIO%d baud=%d\n",
                RS485_NAME, RS485_RX_PIN, RS485_TX_PIN, RS485_DTR_PIN, MODBUS_BAUD);
  return true;
}

static size_t readModbusResponse(uint8_t *buffer, size_t maxLength, uint32_t timeoutMs) {
  size_t length = 0;
  uint32_t startMs = millis();

  while (millis() - startMs < timeoutMs && length < maxLength) {
    while (rs485Port().available() && length < maxLength) {
      buffer[length++] = (uint8_t)rs485Port().read();
    }

    if (length >= 5) {
      if (buffer[1] & 0x80) {
        return length;
      }

      if (buffer[1] == 0x03 && length >= 3) {
        size_t expectedLength = 5 + buffer[2];
        if (length >= expectedLength) {
          return length;
        }
      }
    }

    delay(1);
  }

  return length;
}

static bool readHoldingRegisters(uint8_t slaveId,
                                 uint16_t startRegister,
                                 uint16_t registerCount,
                                 uint16_t *registers,
                                 size_t registerCapacity) {
  if (registerCount == 0 || registerCount > registerCapacity || registerCount > MODBUS_MAX_REGISTERS) {
    return false;
  }

  uint8_t request[8] = {
      slaveId,
      0x03,
      (uint8_t)(startRegister >> 8),
      (uint8_t)(startRegister & 0xFF),
      (uint8_t)(registerCount >> 8),
      (uint8_t)(registerCount & 0xFF),
      0,
      0,
  };

  uint16_t requestCrc = modbusCrc16(request, 6);
  request[6] = (uint8_t)(requestCrc & 0xFF);
  request[7] = (uint8_t)(requestCrc >> 8);

  while (rs485Port().available()) {
    rs485Port().read();
  }

  delayMicroseconds(2000);
  digitalWrite(RS485_DTR_PIN, RS485_TX_ENABLE_LEVEL);
  delayMicroseconds(100);
  for (size_t i = 0; i < sizeof(request); ++i) {
    rs485Port().write(request[i]);
  }
  rs485Port().flush();
  delayMicroseconds(100);
  digitalWrite(RS485_DTR_PIN, RS485_RX_ENABLE_LEVEL);

  uint8_t response[5 + (MODBUS_MAX_REGISTERS * 2)] = {};
  size_t responseLength = readModbusResponse(response, sizeof(response), MODBUS_RESPONSE_TIMEOUT_MS);

  if (responseLength < 5) {
    Serial.printf("Modbus timeout or short response, len=%u\n", (unsigned)responseLength);
    return false;
  }

  uint16_t receivedCrc = response[responseLength - 2] | ((uint16_t)response[responseLength - 1] << 8);
  uint16_t calculatedCrc = modbusCrc16(response, responseLength - 2);
  if (receivedCrc != calculatedCrc) {
    Serial.printf("Modbus CRC error, rx=0x%04X calc=0x%04X len=%u\n",
                  receivedCrc, calculatedCrc, (unsigned)responseLength);
    return false;
  }

  if (response[0] != slaveId) {
    Serial.printf("Modbus slave mismatch, expected=%u got=%u\n", slaveId, response[0]);
    return false;
  }

  if (response[1] & 0x80) {
    Serial.printf("Modbus exception, function=0x%02X code=0x%02X\n", response[1], response[2]);
    return false;
  }

  if (response[1] != 0x03) {
    Serial.printf("Unexpected Modbus function: 0x%02X\n", response[1]);
    return false;
  }

  uint8_t byteCount = response[2];
  if (byteCount != registerCount * 2 || responseLength < (size_t)(5 + byteCount)) {
    Serial.printf("Unexpected Modbus byte count: %u\n", byteCount);
    return false;
  }

  for (uint16_t i = 0; i < registerCount; ++i) {
    registers[i] = ((uint16_t)response[3 + (i * 2)] << 8) | response[4 + (i * 2)];
  }

  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("T-C5-Base RS485 Modbus RTU module example");

  rs485Ready = startRs485();
}

void loop() {
  if (!rs485Ready) {
    delay(1000);
    return;
  }

  uint32_t now = millis();
  if (now - lastPollMs < MODBUS_POLL_INTERVAL_MS) {
    delay(10);
    return;
  }
  lastPollMs = now;

  uint16_t registers[MODBUS_REGISTER_COUNT] = {};
  bool ok = readHoldingRegisters(MODBUS_SLAVE_ID,
                                 MODBUS_START_REGISTER,
                                 MODBUS_REGISTER_COUNT,
                                 registers,
                                 MODBUS_REGISTER_COUNT);

  if (!ok) {
    Serial.println("Modbus read failed");
    return;
  }

  Serial.printf("Modbus slave=%u start=0x%04X count=%u\n",
                MODBUS_SLAVE_ID, MODBUS_START_REGISTER, MODBUS_REGISTER_COUNT);
  for (uint16_t i = 0; i < MODBUS_REGISTER_COUNT; ++i) {
    Serial.printf("  R%u = %u (0x%04X)\n", i, registers[i], registers[i]);
  }
}
