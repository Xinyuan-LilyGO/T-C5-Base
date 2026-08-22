/**
 * @file      can_dual_basic_echo.ino
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2026  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2026-08-21
 * @note      T-C5-Base dual CAN echo-back test with optional OLED status.
 *
 *  Wiring:
 *    CAN1_H <-> CAN2_H
 *    CAN1_L <-> CAN2_L
 *    GND    <-> GND
 *
 *  Add CAN termination according to your wiring. For a normal two-node CAN bus,
 *  use 120 ohm termination at both bus ends.
 *
 *  CAN1 sends a classic CAN frame to CAN2. CAN2 verifies the frame and sends
 *  the same payload back to CAN1 with an echo ID, so this sketch can verify
 *  both CAN transceivers on one board.
 *
 *  Note: Arduino-ESP32 does not currently expose a native Arduino API for both
 *  ESP32-C5 on-chip TWAI controllers, so this CAN example uses ESP-IDF TWAI
 *  driver APIs inside an Arduino sketch.
 */

#include <Arduino.h>
#include <inttypes.h>
#include <string.h>
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifndef CAN_BITRATE
#define CAN_BITRATE 1000000
#endif

#ifndef AUTO_SEND_INTERVAL_MS
#define AUTO_SEND_INTERVAL_MS 2000
#endif

#define CAN1_TO_CAN2_ID 0x321
#define CAN2_TO_CAN1_ID 0x421
#define CAN_RX_TIMEOUT_MS 150
#define CAN_RX_QUEUE_DEPTH 16
#define CAN_TX_QUEUE_DEPTH 2
#define LED_OK_PULSE_MS 300
#define OLED_I2C_ADDRESS 0x3C
#define OLED_SDA_PIN 2
#define OLED_SCL_PIN 3

#ifndef CAN_ENABLE_OLED
#define CAN_ENABLE_OLED 1
#endif

#if CAN_ENABLE_OLED
#include <Wire.h>
#include "SSD1306Wire.h"
#endif

// CAN1 pins
#define CAN1_TX_PIN 26
#define CAN1_RX_PIN 25
// CAN2 pins
#define CAN2_TX_PIN 0
#define CAN2_RX_PIN 1

#ifdef LED_BUILTIN
#undef LED_BUILTIN
#endif

#define LED_BUILTIN     8

struct CanFrame {
    uint32_t id;
    uint8_t dlc;
    bool ide;
    bool rtr;
    uint8_t data[TWAI_FRAME_MAX_LEN];
};

struct CanBus {
    const char *name;
    int txPin;
    int rxPin;
    twai_node_handle_t node;
    QueueHandle_t rxQueue;
    twai_frame_t txFrame;
    uint8_t txData[TWAI_FRAME_MAX_LEN];
    volatile bool busOffSeen;
    volatile uint32_t isrDrops;
    volatile uint32_t txOk;
    volatile uint32_t txFail;
    volatile uint32_t busErrors;
    bool started;
};

static CanBus can1 = {"CAN1", CAN1_TX_PIN, CAN1_RX_PIN, nullptr, nullptr, {}, {}, false, 0, 0, 0, 0, false};
static CanBus can2 = {"CAN2", CAN2_TX_PIN, CAN2_RX_PIN, nullptr, nullptr, {}, {}, false, 0, 0, 0, 0, false};
static uint32_t frameSeq = 0;
static uint32_t lastAutoSendMs = 0;
static uint32_t echoOkCount = 0;
static uint32_t echoFailCount = 0;
static bool lastForwardOk = false;
static bool lastBackOk = false;
static bool lastEchoOk = false;
static bool hasEchoResult = false;
static bool ledPulseActive = false;
static uint32_t ledOffMs = 0;

#if CAN_ENABLE_OLED
SSD1306Wire oled(OLED_I2C_ADDRESS, OLED_SDA_PIN, OLED_SCL_PIN);
static bool oledReady = false;
#endif

static uint8_t checksumPayload(const uint8_t *data, size_t len)
{
    uint8_t sum = 0x5A;
    for (size_t i = 0; i < len; ++i) {
        sum = (uint8_t)((sum << 1) | (sum >> 7));
        sum ^= data[i];
    }
    return sum;
}

static void fillPayload(uint8_t *data, uint32_t sequence)
{
    data[0] = 0xC5;
    memcpy(&data[1], &sequence, sizeof(sequence));
    data[5] = 0x5C;
    data[6] = (uint8_t)(sequence ^ (sequence >> 8) ^ (sequence >> 16) ^ (sequence >> 24));
    data[7] = checksumPayload(data, 7);
}

static bool verifyPayload(const CanFrame &frame, uint32_t expectedId, uint32_t sequence)
{
    if (frame.ide || frame.rtr || frame.id != expectedId || frame.dlc != 8) {
        return false;
    }

    uint32_t rxSequence = 0;
    memcpy(&rxSequence, &frame.data[1], sizeof(rxSequence));
    return frame.data[0] == 0xC5 &&
           rxSequence == sequence &&
           frame.data[5] == 0x5C &&
           frame.data[6] == (uint8_t)(sequence ^ (sequence >> 8) ^ (sequence >> 16) ^ (sequence >> 24)) &&
           frame.data[7] == checksumPayload(frame.data, 7);
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

static String bitrateLabel()
{
    if (CAN_BITRATE >= 1000000 && (CAN_BITRATE % 1000000) == 0) {
        return String(CAN_BITRATE / 1000000) + "Mbps";
    }
    if (CAN_BITRATE >= 1000 && (CAN_BITRATE % 1000) == 0) {
        return String(CAN_BITRATE / 1000) + "Kbps";
    }
    return String(CAN_BITRATE) + "bps";
}

#if CAN_ENABLE_OLED
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
    drawCenteredText(0, "CAN ECHO");
    oled.setFont(ArialMT_Plain_16);
    drawCenteredText(18, status);
    oled.setFont(ArialMT_Plain_10);
    drawCenteredText(42, bitrateLabel());
    oled.display();
}

static void updateOledResult()
{
    if (!oledReady) {
        return;
    }

    oled.clear();
    oled.setFont(ArialMT_Plain_10);
    drawCenteredText(0, "CAN ECHO  " + bitrateLabel());

    oled.setFont(ArialMT_Plain_24);
    drawCenteredText(13, hasEchoResult ? (lastEchoOk ? "PASS" : "FAIL") : "WAIT");

    oled.setFont(ArialMT_Plain_10);
    drawCenteredText(40, String("1>2 ") + (lastForwardOk ? "OK" : "NG") +
                         "    2>1 " + (lastBackOk ? "OK" : "NG"));
    drawCenteredText(52, String("OK:") + String(echoOkCount) +
                         "  NG:" + String(echoFailCount) +
                         "  ERR:" + String(can1.busErrors + can2.busErrors));
    oled.display();
}

static void initOled()
{
#if !CAN_ENABLE_OLED
    Serial.println("[OLED] disabled by CAN_ENABLE_OLED=0");
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
    Serial.println("[OLED] disabled by CAN_ENABLE_OLED=0");
}
#endif

static bool IRAM_ATTR onRxDone(twai_node_handle_t handle,
                               const twai_rx_done_event_data_t *eventData,
                               void *userContext)
{
    (void)eventData;
    CanBus *bus = (CanBus *)userContext;

    uint8_t rxData[TWAI_FRAME_MAX_LEN] = {};
    twai_frame_t rxFrame = {};
    rxFrame.buffer = rxData;
    rxFrame.buffer_len = sizeof(rxData);

    BaseType_t taskWoken = pdFALSE;
    if (twai_node_receive_from_isr(handle, &rxFrame) != ESP_OK) {
        bus->isrDrops = bus->isrDrops + 1;
        return taskWoken == pdTRUE;
    }

    CanFrame queuedFrame = {};
    queuedFrame.id = rxFrame.header.id;
    queuedFrame.dlc = rxFrame.header.dlc > TWAI_FRAME_MAX_LEN ? TWAI_FRAME_MAX_LEN : rxFrame.header.dlc;
    queuedFrame.ide = rxFrame.header.ide;
    queuedFrame.rtr = rxFrame.header.rtr;
    memcpy(queuedFrame.data, rxData, queuedFrame.dlc);

    if (xQueueSendFromISR(bus->rxQueue, &queuedFrame, &taskWoken) != pdTRUE) {
        bus->isrDrops = bus->isrDrops + 1;
    }

    return taskWoken == pdTRUE;
}

static bool IRAM_ATTR onTxDone(twai_node_handle_t handle,
                               const twai_tx_done_event_data_t *eventData,
                               void *userContext)
{
    (void)handle;
    CanBus *bus = (CanBus *)userContext;

    if (eventData->is_tx_success) {
        bus->txOk = bus->txOk + 1;
    } else {
        bus->txFail = bus->txFail + 1;
    }
    return false;
}

static bool IRAM_ATTR onError(twai_node_handle_t handle,
                              const twai_error_event_data_t *eventData,
                              void *userContext)
{
    (void)handle;
    CanBus *bus = (CanBus *)userContext;
    if (eventData->err_flags.val) {
        bus->busErrors = bus->busErrors + 1;
    }
    return false;
}

static bool IRAM_ATTR onStateChange(twai_node_handle_t handle,
                                    const twai_state_change_event_data_t *eventData,
                                    void *userContext)
{
    (void)handle;
    CanBus *bus = (CanBus *)userContext;
    if (eventData->new_sta == TWAI_ERROR_BUS_OFF) {
        bus->busOffSeen = true;
    }
    return false;
}

static bool configureAcceptAllFilters(twai_node_handle_t node)
{
    twai_mask_filter_config_t standardFilter = {};
    standardFilter.id = 0;
    standardFilter.mask = 0;
    standardFilter.is_ext = false;
    standardFilter.no_fd = true;

    if (twai_node_config_mask_filter(node, 0, &standardFilter) != ESP_OK) {
        return false;
    }

    twai_mask_filter_config_t extendedFilter = standardFilter;
    extendedFilter.is_ext = true;
    return twai_node_config_mask_filter(node, 1, &extendedFilter) == ESP_OK;
}

static bool startCanBus(CanBus &bus)
{
    bus.rxQueue = xQueueCreate(CAN_RX_QUEUE_DEPTH, sizeof(CanFrame));
    if (bus.rxQueue == nullptr) {
        Serial.printf("[%s] failed to create RX queue\n", bus.name);
        return false;
    }

    twai_onchip_node_config_t nodeConfig = {};
    nodeConfig.io_cfg.tx = (gpio_num_t)bus.txPin;
    nodeConfig.io_cfg.rx = (gpio_num_t)bus.rxPin;
    nodeConfig.io_cfg.quanta_clk_out = GPIO_NUM_NC;
    nodeConfig.io_cfg.bus_off_indicator = GPIO_NUM_NC;
    nodeConfig.bit_timing.bitrate = CAN_BITRATE;
    nodeConfig.fail_retry_cnt = 3;
    nodeConfig.tx_queue_depth = CAN_TX_QUEUE_DEPTH;

    esp_err_t err = twai_new_node_onchip(&nodeConfig, &bus.node);
    if (err != ESP_OK) {
        Serial.printf("[%s] node create failed: 0x%X\n", bus.name, err);
        return false;
    }

    if (!configureAcceptAllFilters(bus.node)) {
        Serial.printf("[%s] accept-all filter config failed\n", bus.name);
        return false;
    }

    twai_event_callbacks_t callbacks = {};
    callbacks.on_rx_done = onRxDone;
    callbacks.on_tx_done = onTxDone;
    callbacks.on_error = onError;
    callbacks.on_state_change = onStateChange;

    err = twai_node_register_event_callbacks(bus.node, &callbacks, &bus);
    if (err != ESP_OK) {
        Serial.printf("[%s] callback register failed: 0x%X\n", bus.name, err);
        return false;
    }

    err = twai_node_enable(bus.node);
    if (err != ESP_OK) {
        Serial.printf("[%s] node enable failed: 0x%X\n", bus.name, err);
        return false;
    }

    bus.started = true;
    Serial.printf("[%s] started, TX=GPIO%d RX=GPIO%d bitrate=%d bit/s\n",
                  bus.name, bus.txPin, bus.rxPin, CAN_BITRATE);
    return true;
}

static void clearCanQueue(const CanBus &bus)
{
    if (bus.rxQueue == nullptr) {
        return;
    }

    CanFrame ignored = {};
    while (xQueueReceive(bus.rxQueue, &ignored, 0) == pdTRUE) {
    }
}

static bool transmitCan(CanBus &bus, uint32_t id, const uint8_t *data, uint8_t len)
{
    bus.txFrame = {};
    bus.txFrame.header.id = id;
    bus.txFrame.header.dlc = len;
    bus.txFrame.header.ide = false;
    bus.txFrame.header.rtr = false;
    bus.txFrame.buffer = bus.txData;
    bus.txFrame.buffer_len = len;
    memcpy(bus.txData, data, len);

    const esp_err_t err = twai_node_transmit(bus.node, &bus.txFrame, pdMS_TO_TICKS(50));
    if (err != ESP_OK) {
        bus.txFail = bus.txFail + 1;
        Serial.printf("[%s] transmit failed: 0x%X\n", bus.name, err);
        return false;
    }
    return true;
}

static bool waitCanFrame(const CanBus &bus, uint32_t expectedId, CanFrame &frame,
                         uint32_t timeoutMs)
{
    const uint32_t deadline = millis() + timeoutMs;
    while ((int32_t)(millis() - deadline) < 0) {
        while (xQueueReceive(bus.rxQueue, &frame, 0) == pdTRUE) {
            if (!frame.ide && !frame.rtr && frame.id == expectedId) {
                return true;
            }
        }
        delay(1);
    }
    return false;
}

static void printCanFrame(const char *label, const CanFrame &frame)
{
    Serial.printf("[%s] ID=0x%03" PRIX32 " DLC=%u DATA=",
                  label, frame.id, (unsigned)frame.dlc);
    for (uint8_t i = 0; i < frame.dlc; ++i) {
        Serial.printf("%02X", frame.data[i]);
        if (i + 1 < frame.dlc) {
            Serial.print(' ');
        }
    }
    Serial.println();
}

static bool echoBack(uint32_t sequence)
{
    uint8_t payload[8] = {};
    CanFrame rxOnCan2 = {};
    CanFrame rxOnCan1 = {};
    fillPayload(payload, sequence);

    if (!can1.started || can1.rxQueue == nullptr ||
        !can2.started || can2.rxQueue == nullptr) {
        Serial.println("[echo] FAIL: CAN node not started");
        lastForwardOk = false;
        lastBackOk = false;
        lastEchoOk = false;
        hasEchoResult = true;
        ++echoFailCount;
        updateOledResult();
        return false;
    }

    clearCanQueue(can1);
    clearCanQueue(can2);

    if (!transmitCan(can1, CAN1_TO_CAN2_ID, payload, sizeof(payload))) {
        lastForwardOk = false;
        lastBackOk = false;
        lastEchoOk = false;
        hasEchoResult = true;
        ++echoFailCount;
        updateOledResult();
        return false;
    }

    lastForwardOk = waitCanFrame(can2, CAN1_TO_CAN2_ID, rxOnCan2, CAN_RX_TIMEOUT_MS) &&
                    verifyPayload(rxOnCan2, CAN1_TO_CAN2_ID, sequence);
    if (lastForwardOk) {
        printCanFrame("CAN1 -> CAN2", rxOnCan2);
    } else {
        Serial.println("[CAN1 -> CAN2] FAIL");
    }

    if (!lastForwardOk ||
        !transmitCan(can2, CAN2_TO_CAN1_ID, rxOnCan2.data, rxOnCan2.dlc)) {
        lastBackOk = false;
        lastEchoOk = false;
        hasEchoResult = true;
        ++echoFailCount;
        Serial.println("[echo] FAIL");
        updateOledResult();
        return false;
    }

    lastBackOk = waitCanFrame(can1, CAN2_TO_CAN1_ID, rxOnCan1, CAN_RX_TIMEOUT_MS) &&
                 verifyPayload(rxOnCan1, CAN2_TO_CAN1_ID, sequence);
    if (lastBackOk) {
        printCanFrame("CAN2 -> CAN1", rxOnCan1);
    } else {
        Serial.println("[CAN2 -> CAN1] FAIL");
    }

    lastEchoOk = lastForwardOk && lastBackOk;
    hasEchoResult = true;
    if (lastEchoOk) {
        ++echoOkCount;
        pulseOkLed();
    } else {
        ++echoFailCount;
    }

    Serial.println(lastEchoOk ? "[echo] OK" : "[echo] FAIL");
    updateOledResult();
    return lastEchoOk;
}

static void recoverIfNeeded(CanBus &bus)
{
    if (!bus.busOffSeen || bus.node == nullptr) {
        return;
    }

    twai_node_status_t status = {};
    twai_node_get_info(bus.node, &status, nullptr);
    if (status.state == TWAI_ERROR_BUS_OFF) {
        Serial.printf("[%s] bus off, start recovery\n", bus.name);
        twai_node_recover(bus.node);
        bus.started = false;
    }
    bus.busOffSeen = false;
}

static void refreshRecovered(CanBus &bus)
{
    if (bus.started || bus.node == nullptr) {
        return;
    }

    twai_node_status_t status = {};
    twai_node_get_info(bus.node, &status, nullptr);
    if (status.state == TWAI_ERROR_ACTIVE) {
        bus.started = true;
        Serial.printf("[%s] recovered\n", bus.name);
    }
}

static void processAutoSend()
{
    const uint32_t now = millis();
    if (now - lastAutoSendMs < AUTO_SEND_INTERVAL_MS) {
        return;
    }
    lastAutoSendMs = now;

    Serial.printf("\n[Test %" PRIu32 "]\n", frameSeq);
    echoBack(frameSeq++);
}

void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println();
    Serial.println("T-C5-Base dual CAN echo-back test");
    Serial.printf("CAN1 TX=GPIO%d RX=GPIO%d, CAN2 TX=GPIO%d RX=GPIO%d, bitrate=%d bit/s\n",
                  can1.txPin, can1.rxPin, can2.txPin, can2.rxPin, CAN_BITRATE);

    initLed();
    initOled();
    drawOledBoot("STARTING");
    startCanBus(can1);
    startCanBus(can2);
    drawOledBoot("WAITING");
}

void loop()
{
    recoverIfNeeded(can1);
    recoverIfNeeded(can2);
    refreshRecovered(can1);
    refreshRecovered(can2);
    processAutoSend();
    updateOkLed();
    delay(1);
}
