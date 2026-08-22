/**
 * @file      can_speed_test.ino
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2026  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2026-08-21
 * @note      T-C5-Base dual CAN + dual RS485 echo-back test.
 *  
 *    Wiring:
 *      CAN1_H   <-> CAN2_H
 *      CAN1_L   <-> CAN2_L
 *      RS4851_A <-> RS4852_A
 *      RS4851_B <-> RS4852_B
 *      GND      <-> GND
 *
 *    Test flow:
 *      CAN1 sends a classic CAN frame to CAN2, then CAN2 sends the same payload
 *      back to CAN1 with an echo ID.
 *      RS4851 sends bytes to RS4852, then RS4852 sends the same bytes back.
 *
 *    Note: Arduino-ESP32 does not currently expose a native Arduino API for both
 *    ESP32-C5 on-chip TWAI controllers, so only the CAN part uses ESP-IDF TWAI
 *    driver APIs. The RS485 part uses Arduino HardwareSerial APIs.
 * 
 *    Add CAN termination according to your wiring. For a normal two-node CAN bus,
 *    use 120 ohm termination at both bus ends.
*/

#include <Arduino.h>
#include <inttypes.h>
#include <string.h>
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifndef RS485_BAUD
#define RS485_BAUD 115200
#endif

#ifndef RS4852_DE_PIN
#define RS4852_DE_PIN 7
#endif

#ifndef CAN_BITRATE
#define CAN_BITRATE 1000000
#endif

#ifndef AUTO_TEST_INTERVAL_MS
#define AUTO_TEST_INTERVAL_MS 1000
#endif

#define RS485_TX_ENABLE_LEVEL HIGH
#define RS485_RX_ENABLE_LEVEL LOW
#define RS485_RX_TIMEOUT_MS 100
#define CAN_RX_TIMEOUT_MS 150
#define RS485_MAX_FRAME_SIZE 96
#define CAN_RX_QUEUE_DEPTH 16
#define CAN1_TO_CAN2_ID 0x321
#define CAN2_TO_CAN1_ECHO_ID 0x421

#if !ARDUINO_USB_CDC_ON_BOOT
#error "RS4851 uses UART0/Serial0. Enable ARDUINO_USB_CDC_ON_BOOT=1 so Serial remains USB CDC."
#endif

// CAN1 pins
#define CAN1_TX_PIN 26
#define CAN1_RX_PIN 25
// CAN2 pins
#define CAN2_TX_PIN 0
#define CAN2_RX_PIN 1
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
};

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

static Rs485Bus rs4851 = {"RS4851", &Serial0, 5, 4, 27};
static Rs485Bus rs4852 = {"RS4852", &Serial1, 24, 23, RS4852_DE_PIN};
static CanBus can1 = {"CAN1", 26, 25, nullptr, nullptr, {}, {}, false, 0, 0, 0, 0, false};
static CanBus can2 = {"CAN2", 0, 1, nullptr, nullptr, {}, {}, false, 0, 0, 0, 0, false};
static uint32_t testSeq = 0;
static uint32_t lastAutoTestMs = 0;

static uint8_t checksumPayload(const uint8_t *data, size_t len) {
  uint8_t sum = 0x5A;
  for (size_t i = 0; i < len; ++i) {
    sum = (uint8_t)((sum << 1) | (sum >> 7));
    sum ^= data[i];
  }
  return sum;
}

static void fillCanPayload(uint8_t *data, uint32_t sequence) {
  data[0] = 0xC5;
  memcpy(&data[1], &sequence, sizeof(sequence));
  data[5] = 0x5C;
  data[6] = (uint8_t)(sequence ^ (sequence >> 8) ^ (sequence >> 16) ^ (sequence >> 24));
  data[7] = checksumPayload(data, 7);
}

static bool verifyCanPayload(const uint8_t *data, uint8_t len, uint32_t sequence) {
  if (len != 8) {
    return false;
  }
  uint32_t rxSequence = 0;
  memcpy(&rxSequence, &data[1], sizeof(rxSequence));
  return data[0] == 0xC5 &&
         rxSequence == sequence &&
         data[5] == 0x5C &&
         data[6] == (uint8_t)(sequence ^ (sequence >> 8) ^ (sequence >> 16) ^ (sequence >> 24)) &&
         data[7] == checksumPayload(data, 7);
}

static bool IRAM_ATTR onCanRxDone(twai_node_handle_t handle,
                                  const twai_rx_done_event_data_t *eventData,
                                  void *userContext) {
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

static bool IRAM_ATTR onCanTxDone(twai_node_handle_t handle,
                                  const twai_tx_done_event_data_t *eventData,
                                  void *userContext) {
  (void)handle;
  CanBus *bus = (CanBus *)userContext;
  if (eventData->is_tx_success) {
    bus->txOk = bus->txOk + 1;
  } else {
    bus->txFail = bus->txFail + 1;
  }
  return false;
}

static bool IRAM_ATTR onCanError(twai_node_handle_t handle,
                                 const twai_error_event_data_t *eventData,
                                 void *userContext) {
  (void)handle;
  CanBus *bus = (CanBus *)userContext;
  if (eventData->err_flags.val) {
    bus->busErrors = bus->busErrors + 1;
  }
  return false;
}

static bool IRAM_ATTR onCanStateChange(twai_node_handle_t handle,
                                       const twai_state_change_event_data_t *eventData,
                                       void *userContext) {
  (void)handle;
  CanBus *bus = (CanBus *)userContext;
  if (eventData->new_sta == TWAI_ERROR_BUS_OFF) {
    bus->busOffSeen = true;
  }
  return false;
}

static bool configureCanAcceptAllFilters(twai_node_handle_t node) {
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

static bool startCanBus(CanBus &bus) {
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
  nodeConfig.tx_queue_depth = 2;

  esp_err_t err = twai_new_node_onchip(&nodeConfig, &bus.node);
  if (err != ESP_OK) {
    Serial.printf("[%s] node create failed: 0x%X\n", bus.name, err);
    return false;
  }

  if (!configureCanAcceptAllFilters(bus.node)) {
    Serial.printf("[%s] accept-all filter config failed\n", bus.name);
    return false;
  }

  twai_event_callbacks_t callbacks = {};
  callbacks.on_rx_done = onCanRxDone;
  callbacks.on_tx_done = onCanTxDone;
  callbacks.on_error = onCanError;
  callbacks.on_state_change = onCanStateChange;

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

static void setRs485RxMode(const Rs485Bus &bus) {
  digitalWrite(bus.dePin, RS485_RX_ENABLE_LEVEL);
}

static void setRs485TxMode(const Rs485Bus &bus) {
  digitalWrite(bus.dePin, RS485_TX_ENABLE_LEVEL);
}

static void clearRs485Rx(const Rs485Bus &bus) {
  while (bus.port->available()) {
    bus.port->read();
  }
}

static void startRs485Bus(const Rs485Bus &bus) {
  pinMode(bus.dePin, OUTPUT);
  setRs485RxMode(bus);

  bus.port->setRxBufferSize(512);
  bus.port->begin(RS485_BAUD, SERIAL_8N1, bus.rxPin, bus.txPin);
  bus.port->setTimeout(1);
  clearRs485Rx(bus);

  Serial.printf("[%s] started, RX=GPIO%d TX=GPIO%d DE=GPIO%d baud=%d\n",
                bus.name, bus.rxPin, bus.txPin, bus.dePin, RS485_BAUD);
}

static size_t writeRs485(const Rs485Bus &bus, const uint8_t *data, size_t len) {
  setRs485TxMode(bus);
  delayMicroseconds(20);
  const size_t written = bus.port->write(data, len);
  bus.port->flush();
  delayMicroseconds(20);
  setRs485RxMode(bus);
  return written;
}

static size_t readRs485For(const Rs485Bus &bus, uint8_t *buffer, size_t maxLen,
                           uint32_t timeoutMs) {
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

static void clearCanQueue(const CanBus &bus) {
  CanFrame ignored = {};
  while (xQueueReceive(bus.rxQueue, &ignored, 0) == pdTRUE) {
  }
}

static bool transmitCan(CanBus &bus, uint32_t id, const uint8_t *data, uint8_t len) {
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
                         uint32_t timeoutMs) {
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

static void printCanData(const CanFrame &frame) {
  Serial.printf("ID=0x%03" PRIX32 " DLC=%u DATA=", frame.id, (unsigned)frame.dlc);
  for (uint8_t i = 0; i < frame.dlc; ++i) {
    Serial.printf("%02X", frame.data[i]);
    if (i + 1 < frame.dlc) {
      Serial.print(' ');
    }
  }
}

static void printBytes(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    const uint8_t c = data[i];
    if (c >= 32 && c <= 126) {
      Serial.write(c);
    } else {
      Serial.printf("\\x%02X", c);
    }
  }
}

static bool runCanEchoBack(uint32_t sequence) {
  uint8_t payload[8] = {};
  fillCanPayload(payload, sequence);
  CanFrame rxOnCan2 = {};
  CanFrame rxOnCan1 = {};

  clearCanQueue(can1);
  clearCanQueue(can2);

  if (!transmitCan(can1, CAN1_TO_CAN2_ID, payload, sizeof(payload))) {
    Serial.println("[CAN echo] FAIL: CAN1 transmit failed");
    return false;
  }

  if (!waitCanFrame(can2, CAN1_TO_CAN2_ID, rxOnCan2, CAN_RX_TIMEOUT_MS)) {
    Serial.println("[CAN echo] FAIL: CAN2 received no request frame");
    return false;
  }

  Serial.print("[CAN1 -> CAN2] ");
  printCanData(rxOnCan2);
  Serial.println();

  if (!verifyCanPayload(rxOnCan2.data, rxOnCan2.dlc, sequence)) {
    Serial.println("[CAN echo] FAIL: CAN2 request payload mismatch");
    return false;
  }

  if (!transmitCan(can2, CAN2_TO_CAN1_ECHO_ID, rxOnCan2.data, rxOnCan2.dlc)) {
    Serial.println("[CAN echo] FAIL: CAN2 echo transmit failed");
    return false;
  }

  if (!waitCanFrame(can1, CAN2_TO_CAN1_ECHO_ID, rxOnCan1, CAN_RX_TIMEOUT_MS)) {
    Serial.println("[CAN echo] FAIL: CAN1 received no echo frame");
    return false;
  }

  Serial.print("[CAN2 -> CAN1] ");
  printCanData(rxOnCan1);
  Serial.println();

  const bool ok = verifyCanPayload(rxOnCan1.data, rxOnCan1.dlc, sequence);
  Serial.println(ok ? "[CAN echo] OK" : "[CAN echo] FAIL: CAN1 echo payload mismatch");
  return ok;
}

static bool runRs485EchoBack(uint32_t sequence) {
  char payload[32] = {};
  const int len = snprintf(payload, sizeof(payload), "RS485 PING %lu", (unsigned long)sequence);
  uint8_t rxOnRs4852[RS485_MAX_FRAME_SIZE] = {};
  uint8_t rxOnRs4851[RS485_MAX_FRAME_SIZE] = {};

  clearRs485Rx(rs4851);
  clearRs485Rx(rs4852);

  const size_t writtenForward = writeRs485(rs4851, (const uint8_t *)payload, (size_t)len);
  const size_t receivedForward =
      readRs485For(rs4852, rxOnRs4852, sizeof(rxOnRs4852), RS485_RX_TIMEOUT_MS);

  Serial.printf("[RS4851 -> RS4852] written=%u received=%u data=\"",
                (unsigned)writtenForward, (unsigned)receivedForward);
  printBytes(rxOnRs4852, receivedForward);
  Serial.println("\"");

  if (receivedForward == 0) {
    Serial.println("[RS485 echo] FAIL: RS4852 received no data");
    return false;
  }

  const size_t writtenBack = writeRs485(rs4852, rxOnRs4852, receivedForward);
  const size_t receivedBack =
      readRs485For(rs4851, rxOnRs4851, sizeof(rxOnRs4851), RS485_RX_TIMEOUT_MS);

  Serial.printf("[RS4852 -> RS4851] written=%u received=%u data=\"",
                (unsigned)writtenBack, (unsigned)receivedBack);
  printBytes(rxOnRs4851, receivedBack);
  Serial.println("\"");

  const bool ok = receivedBack == (size_t)len &&
                  memcmp(payload, rxOnRs4851, (size_t)len) == 0;
  Serial.println(ok ? "[RS485 echo] OK" : "[RS485 echo] FAIL: echoed data mismatch");
  return ok;
}

static void recoverCanIfNeeded(CanBus &bus) {
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

static void refreshCanRecovered(CanBus &bus) {
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

static void runCombinedEchoBack() {
  const uint32_t sequence = testSeq++;
  Serial.printf("\n[Test %" PRIu32 "]\n", sequence);

  const bool canOk = can1.started && can2.started && runCanEchoBack(sequence);
  const bool rs485Ok = runRs485EchoBack(sequence);

  Serial.printf("[summary] CAN=%s RS485=%s txOk=%" PRIu32 "/%" PRIu32
                " txFail=%" PRIu32 "/%" PRIu32 " canErr=%" PRIu32 "/%" PRIu32
                " isrDrop=%" PRIu32 "/%" PRIu32 "\n",
                canOk ? "OK" : "FAIL",
                rs485Ok ? "OK" : "FAIL",
                can1.txOk, can2.txOk,
                can1.txFail, can2.txFail,
                can1.busErrors, can2.busErrors,
                can1.isrDrops, can2.isrDrops);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("T-C5-Base dual CAN + dual RS485 echo-back test");
  Serial.printf("CAN1 TX=GPIO%d RX=GPIO%d, CAN2 TX=GPIO%d RX=GPIO%d, bitrate=%d bit/s\n",
                can1.txPin, can1.rxPin, can2.txPin, can2.rxPin, CAN_BITRATE);
  Serial.printf("RS4851 RX=GPIO%d TX=GPIO%d DE=GPIO%d, RS4852 RX=GPIO%d TX=GPIO%d DE=GPIO%d, baud=%d\n",
                rs4851.rxPin, rs4851.txPin, rs4851.dePin,
                rs4852.rxPin, rs4852.txPin, rs4852.dePin,
                RS485_BAUD);

  startRs485Bus(rs4851);
  startRs485Bus(rs4852);
  startCanBus(can1);
  startCanBus(can2);
  lastAutoTestMs = millis();
}

void loop() {
  recoverCanIfNeeded(can1);
  recoverCanIfNeeded(can2);
  refreshCanRecovered(can1);
  refreshCanRecovered(can2);

  const uint32_t now = millis();
  if (now - lastAutoTestMs >= AUTO_TEST_INTERVAL_MS) {
    lastAutoTestMs = now;
    runCombinedEchoBack();
  }

  delay(1);
}
