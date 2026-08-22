/**
 * @file      rs485_dual_basic_echo.ino
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2026  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2026-08-21
 * @note       T-C5-Base dual RS485 echo-back GPIO test.
 *
 *  Wiring:
 *    RS4851_A <-> RS4852_A
 *    RS4851_B <-> RS4852_B
 *    GND      <-> GND
 *
 *  For high baud rates or long cables, add RS485
 *  termination according to your wiring.
 *
 *  RS4851 sends a frame to RS4852. RS4852 prints the received frame and sends
 *  the same bytes back to RS4851, so this sketch can verify the two RS485
 *  transceivers.
 */

#include <Arduino.h>
#include <string.h>

#ifndef RS485_BAUD
#define RS485_BAUD 115200
#endif

#ifndef RS4852_DE_PIN
#define RS4852_DE_PIN 7
#endif

#ifndef AUTO_SEND_INTERVAL_MS
#define AUTO_SEND_INTERVAL_MS 2000
#endif

#define RS485_TX_ENABLE_LEVEL HIGH
#define RS485_RX_ENABLE_LEVEL LOW
#define RX_TIMEOUT_MS 100
#define MAX_FRAME_SIZE 96
#define LED_OK_PULSE_MS 300
#define OLED_I2C_ADDRESS 0x3C
#define OLED_SDA_PIN 2
#define OLED_SCL_PIN 3

#ifndef RS485_ENABLE_OLED
#define RS485_ENABLE_OLED 1
#endif

#if RS485_ENABLE_OLED
#include <Wire.h>
#include "SSD1306Wire.h"
#endif

#if !ARDUINO_USB_CDC_ON_BOOT
#error "RS4851 uses UART0/Serial0. Enable ARDUINO_USB_CDC_ON_BOOT=1 so Serial remains USB CDC."
#endif

// RS4851 pins
#define RS4851_DE_PIN 27
#define RS4851_TX_PIN 5
#define RS4851_RX_PIN 4
// RS4852 pins
#define RS4852_TX_PIN 23
#define RS4852_RX_PIN 24

#ifdef LED_BUILTIN
#undef LED_BUILTIN
#endif

#define LED_BUILTIN     8

struct Rs485Bus {
    const char *name;
    HardwareSerial *port;
    int rxPin;
    int txPin;
    int dePin;
};

static Rs485Bus rs4851 = {"RS4851", &Serial0, RS4851_RX_PIN, RS4851_TX_PIN, RS4851_DE_PIN};
static Rs485Bus rs4852 = {"RS4852", &Serial1, RS4852_RX_PIN, RS4852_TX_PIN, RS4852_DE_PIN};
static uint32_t frameSeq = 0;
static uint32_t lastAutoSendMs = 0;
static uint32_t echoOkCount = 0;
static uint32_t echoFailCount = 0;
static uint32_t lastForwardBytes = 0;
static uint32_t lastBackBytes = 0;
static bool lastForwardOk = false;
static bool lastBackOk = false;
static bool lastEchoOk = false;
static bool hasEchoResult = false;
static bool ledPulseActive = false;
static uint32_t ledOffMs = 0;

#if RS485_ENABLE_OLED
SSD1306Wire  oled(OLED_I2C_ADDRESS, OLED_SDA_PIN, OLED_SCL_PIN);
static bool oledReady = false;
#endif

static void setRxMode(const Rs485Bus &bus)
{
    digitalWrite(bus.dePin, RS485_RX_ENABLE_LEVEL);
}

static void setTxMode(const Rs485Bus &bus)
{
    digitalWrite(bus.dePin, RS485_TX_ENABLE_LEVEL);
}

static void clearRx(const Rs485Bus &bus)
{
    while (bus.port->available()) {
        bus.port->read();
    }
}

static void startBus(const Rs485Bus &bus)
{
    pinMode(bus.dePin, OUTPUT);
    setRxMode(bus);

    bus.port->setRxBufferSize(512);
    bus.port->begin(RS485_BAUD, SERIAL_8N1, bus.rxPin, bus.txPin);
    bus.port->setTimeout(1);
    clearRx(bus);

    Serial.printf("[%s] started, RX=GPIO%d TX=GPIO%d DE=GPIO%d baud=%d\n",
                  bus.name, bus.rxPin, bus.txPin, bus.dePin, RS485_BAUD);
}

static size_t writeBus(const Rs485Bus &bus, const uint8_t *data, size_t len)
{
    setTxMode(bus);
    delayMicroseconds(20);
    const size_t written = bus.port->write(data, len);
    bus.port->flush();
    delayMicroseconds(20);
    setRxMode(bus);
    return written;
}

static size_t readBytesFor(const Rs485Bus &bus, uint8_t *buffer, size_t maxLen,
                           uint32_t timeoutMs)
{
    const uint32_t deadline = millis() + timeoutMs;
    size_t count = 0;

    while ((int32_t)(millis() - deadline) < 0 && count < maxLen) {
        while (bus.port->available() && count < maxLen) {
            buffer[count++] = (uint8_t)bus.port->read();
        }
        if (count > 0 && !bus.port->available()) {
            delay(2);
            if (!bus.port->available()) {
                break;
            }
        }
        delay(1);
    }

    return count;
}

static void printBytes(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        const uint8_t c = data[i];
        if (c >= 32 && c <= 126) {
            Serial.write(c);
        } else {
            Serial.printf("\\x%02X", c);
        }
    }
}

static void initLed()
{
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
}

static void pulseOkLed()
{
    digitalWrite(LED_BUILTIN, HIGH);
    ledPulseActive = true;
    ledOffMs = millis() + LED_OK_PULSE_MS;
}

static void updateOkLed()
{
    if (ledPulseActive && (int32_t)(millis() - ledOffMs) >= 0) {
        digitalWrite(LED_BUILTIN, LOW);
        ledPulseActive = false;
    }
}

static String baudLabel()
{
    if (RS485_BAUD >= 1000000 && (RS485_BAUD % 1000000) == 0) {
        return String(RS485_BAUD / 1000000) + "Mbps";
    }
    if (RS485_BAUD >= 1000 && (RS485_BAUD % 1000) == 0) {
        return String(RS485_BAUD / 1000) + "Kbps";
    }
    return String(RS485_BAUD) + "bps";
}

#if RS485_ENABLE_OLED
static void drawCenteredText(int16_t y, const String &text)
{
    if (!oledReady) {
        return;
    }

    oled.setTextAlignment(TEXT_ALIGN_CENTER);
    oled.drawString(64, y, text);
    oled.setTextAlignment(TEXT_ALIGN_LEFT);
}

static void drawOledBoot(const char *status)
{
    if (!oledReady) {
        return;
    }

    oled.clear();
    oled.setFont(ArialMT_Plain_10);
    drawCenteredText(0, "RS485 ECHO");
    oled.setFont(ArialMT_Plain_16);
    drawCenteredText(18, status);
    oled.setFont(ArialMT_Plain_10);
    drawCenteredText(42, baudLabel() + "  DE2=GPIO" + String(RS4852_DE_PIN));
    oled.display();
}

static void updateOledResult()
{
    if (!oledReady) {
        return;
    }

    oled.clear();
    oled.setFont(ArialMT_Plain_10);
    drawCenteredText(0, "RS485 ECHO  " + baudLabel());

    oled.setFont(ArialMT_Plain_24);
    drawCenteredText(13, hasEchoResult ? (lastEchoOk ? "PASS" : "FAIL") : "WAIT");

    oled.setFont(ArialMT_Plain_10);
    drawCenteredText(40, String("1>2 ") + (lastForwardOk ? "OK" : "NG") +
                         "    2>1 " + (lastBackOk ? "OK" : "NG"));
    drawCenteredText(52, String("OK:") + String(echoOkCount) +
                         "  NG:" + String(echoFailCount) +
                         "  DE2:" + String(RS4852_DE_PIN));
    oled.display();
}

static void initOled()
{
#if !RS485_ENABLE_OLED
    Serial.println("[OLED] disabled by RS485_ENABLE_OLED=0");
    return;
#endif

    Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
    Wire.beginTransmission(OLED_I2C_ADDRESS);
    if (Wire.endTransmission() != 0) {
        Serial.printf("[OLED] not found at 0x%02X SDA=GPIO%d SCL=GPIO%d\n",
                      OLED_I2C_ADDRESS, OLED_SDA_PIN, OLED_SCL_PIN);
        return;
    }

    oled.init();
    oled.flipScreenVertically();
    oled.setFont(ArialMT_Plain_10);
    oled.setTextAlignment(TEXT_ALIGN_LEFT);
    oled.setContrast(255);
    oledReady = true;
    Serial.printf("[OLED] started, address=0x%02X SDA=GPIO%d SCL=GPIO%d\n",
                  OLED_I2C_ADDRESS, OLED_SDA_PIN, OLED_SCL_PIN);
}
#else
static void drawOledBoot(const char *status)
{
    (void)status;
}

static void updateOledResult()
{
}

static void initOled()
{
    Serial.println("[OLED] disabled by RS485_ENABLE_OLED=0");
}
#endif

static bool echoBack(const uint8_t *payload, size_t len)
{
    uint8_t rxOnRs4852[MAX_FRAME_SIZE] = {};
    uint8_t rxOnRs4851[MAX_FRAME_SIZE] = {};

    clearRx(rs4851);
    clearRx(rs4852);

    const size_t writtenForward = writeBus(rs4851, payload, len);
    const size_t receivedForward = readBytesFor(rs4852, rxOnRs4852, sizeof(rxOnRs4852), RX_TIMEOUT_MS);

    Serial.printf("[RS4851 -> RS4852] written=%u received=%u data=\"",
                  (unsigned)writtenForward, (unsigned)receivedForward);
    printBytes(rxOnRs4852, receivedForward);
    Serial.println("\"");
    lastForwardBytes = (uint32_t)receivedForward;
    lastForwardOk = receivedForward == len && memcmp(payload, rxOnRs4852, len) == 0;

    if (receivedForward == 0) {
        Serial.println("[echo] FAIL: RS4852 received no data");
        lastBackBytes = 0;
        lastBackOk = false;
        lastEchoOk = false;
        hasEchoResult = true;
        ++echoFailCount;
        updateOledResult();
        return false;
    }

    const size_t writtenBack = writeBus(rs4852, rxOnRs4852, receivedForward);
    const size_t receivedBack = readBytesFor(rs4851, rxOnRs4851, sizeof(rxOnRs4851), RX_TIMEOUT_MS);

    Serial.printf("[RS4852 -> RS4851] written=%u received=%u data=\"",
                  (unsigned)writtenBack, (unsigned)receivedBack);
    printBytes(rxOnRs4851, receivedBack);
    Serial.println("\"");
    lastBackBytes = (uint32_t)receivedBack;
    lastBackOk = receivedBack == len && memcmp(payload, rxOnRs4851, len) == 0;

    const bool ok = lastForwardOk && lastBackOk;
    Serial.println(ok ? "[echo] OK" : "[echo] FAIL: echoed data mismatch");
    lastEchoOk = ok;
    hasEchoResult = true;
    if (ok) {
        ++echoOkCount;
        pulseOkLed();
    } else {
        ++echoFailCount;
    }
    updateOledResult();
    return ok;
}

static void processUsbInput()
{
    static uint8_t line[MAX_FRAME_SIZE] = {};
    static size_t lineLen = 0;

    while (Serial.available()) {
        const int value = Serial.read();
        if (value < 0) {
            return;
        }

        const uint8_t c = (uint8_t)value;
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            if (lineLen > 0) {
                echoBack(line, lineLen);
                lineLen = 0;
            }
            continue;
        }
        if (lineLen < sizeof(line)) {
            line[lineLen++] = c;
        }
    }
}

static void processAutoSend()
{
    const uint32_t now = millis();
    if (now - lastAutoSendMs < AUTO_SEND_INTERVAL_MS) {
        return;
    }
    lastAutoSendMs = now;

    char payload[32] = {};
    const int len = snprintf(payload, sizeof(payload), "PING %lu", (unsigned long)frameSeq++);
    echoBack((const uint8_t *)payload, (size_t)len);
}

void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("T-C5-Base dual RS485 echo-back GPIO test");
    Serial.printf("RS4852_DE_PIN=GPIO%d\n", RS4852_DE_PIN);

    initLed();
    initOled();
    drawOledBoot("STARTING");
    startBus(rs4851);
    startBus(rs4852);
    drawOledBoot("WAITING");

    Serial.println("Type a line in USB Serial Monitor, or wait for auto PING frames.");
}

void loop()
{
    processUsbInput();
    processAutoSend();
    updateOkLed();
}
