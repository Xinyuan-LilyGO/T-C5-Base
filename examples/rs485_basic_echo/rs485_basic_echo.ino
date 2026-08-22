/**
 * @file      rs485_dual_basic_echo.ino
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2026  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2026-08-21
 * @note      T-C5-Base Shield RS485 basic echo example.
 *
 *            Select RS4851 or RS4852 with ACTIVE_RS485. Data from the Arduino serial
 *            monitor is sent to the RS485 bus; data received from RS485 is printed back.
 *
 *            For high baud rates or long cables, add RS485 termination according to your wiring.
 */

#include <Arduino.h>

#ifndef ACTIVE_RS485
#define ACTIVE_RS485 1
#endif
#define RS485_BAUD 115200
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

static HardwareSerial &rs485Port()
{
    if (ACTIVE_RS485 == 1) {
        return Serial0;
    }
    return Serial1;
}

static bool startRs485()
{
    rs485Port().setRxBufferSize(512);
    rs485Port().begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
    pinMode(RS485_DTR_PIN, OUTPUT);
    digitalWrite(RS485_DTR_PIN, RS485_RX_ENABLE_LEVEL);

    Serial.printf("[%s] started, RX=GPIO%d TX=GPIO%d DTR=GPIO%d baud=%d\n",
                  RS485_NAME, RS485_RX_PIN, RS485_TX_PIN, RS485_DTR_PIN, RS485_BAUD);
    return true;
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("T-C5-Base RS485 basic echo");

    rs485Ready = startRs485();
}

void loop()
{
    if (!rs485Ready) {
        delay(1000);
        return;
    }

    while (Serial.available()) {
        digitalWrite(RS485_DTR_PIN, RS485_TX_ENABLE_LEVEL);
        delayMicroseconds(100);
        rs485Port().write((uint8_t)Serial.read());
        rs485Port().flush();
        delayMicroseconds(100);
        digitalWrite(RS485_DTR_PIN, RS485_RX_ENABLE_LEVEL);
    }

    while (rs485Port().available()) {
        Serial.write((uint8_t)rs485Port().read());
    }
}
