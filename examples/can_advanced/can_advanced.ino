/**
 * @file      can_dual_basic_echo.ino
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2026  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2026-08-21
 * @note      T-C5-Base Shield dual CAN advanced example.
 * 
 *   ESP32-C5 has two TWAI controllers. This sketch starts two on-chip TWAI nodes
 *   and can bridge frames between CAN1 and CAN2 while reporting error state.
 * 
 *   Note: Arduino-ESP32 does not currently expose a native Arduino API for both
 *   ESP32-C5 on-chip TWAI controllers, so this CAN example uses the ESP-IDF TWAI
 *   driver APIs inside an Arduino sketch.
 * 
 *  Add CAN termination according to your wiring. For a normal two-node CAN bus,
 *  use 120 ohm termination at both bus ends.
*/

#include <Arduino.h>
#include <inttypes.h>
#include <string.h>
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define CAN_BITRATE 500000
#define FORWARD_CAN1_TO_CAN2 1
#define FORWARD_CAN2_TO_CAN1 1
#define ENABLE_SOFTWARE_ID_FILTER 0
#define FILTER_STD_ID_MIN 0x100
#define FILTER_STD_ID_MAX 0x1FF
#define RX_QUEUE_DEPTH 32
#define REPORT_INTERVAL_MS 5000

// CAN1 pins
#define CAN1_TX_PIN 26
#define CAN1_RX_PIN 25
// CAN2 pins
#define CAN2_TX_PIN 0
#define CAN2_RX_PIN 1

struct CanFrame {
  uint32_t id;
  uint8_t dlc;
  bool ide;
  bool rtr;
  uint64_t timestamp;
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
  volatile bool txBusy;
  volatile bool busOffSeen;
  volatile uint32_t isrDrops;
  volatile uint32_t txFrames;
  volatile uint32_t txFailed;
  volatile uint32_t busErrors;
  bool started;
  uint32_t rxFrames;
  uint32_t droppedFrames;
  uint32_t busOffCount;
};

static CanBus can1 = {"CAN1", CAN1_TX_PIN, CAN1_RX_PIN, nullptr, nullptr, {}, {}, false, false, 0, 0, 0, 0, false, 0, 0, 0};
static CanBus can2 = {"CAN2", CAN2_TX_PIN, CAN2_RX_PIN, nullptr, nullptr, {}, {}, false, false, 0, 0, 0, 0, false, 0, 0, 0};
static uint32_t lastReportMs = 0;

static bool IRAM_ATTR onRxDone(twai_node_handle_t handle,
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
    ++bus->isrDrops;
    return taskWoken == pdTRUE;
  }

  CanFrame queuedFrame = {};
  queuedFrame.id = rxFrame.header.id;
  queuedFrame.dlc = rxFrame.header.dlc > TWAI_FRAME_MAX_LEN ? TWAI_FRAME_MAX_LEN : rxFrame.header.dlc;
  queuedFrame.ide = rxFrame.header.ide;
  queuedFrame.rtr = rxFrame.header.rtr;
  queuedFrame.timestamp = rxFrame.header.timestamp;
  memcpy(queuedFrame.data, rxData, queuedFrame.dlc);

  if (xQueueSendFromISR(bus->rxQueue, &queuedFrame, &taskWoken) != pdTRUE) {
    ++bus->isrDrops;
  }

  return taskWoken == pdTRUE;
}

static bool IRAM_ATTR onTxDone(twai_node_handle_t handle,
                               const twai_tx_done_event_data_t *eventData,
                               void *userContext) {
  (void)handle;
  CanBus *bus = (CanBus *)userContext;

  if (eventData->is_tx_success) {
    ++bus->txFrames;
  } else {
    ++bus->txFailed;
  }
  bus->txBusy = false;

  return false;
}

static bool IRAM_ATTR onError(twai_node_handle_t handle,
                              const twai_error_event_data_t *eventData,
                              void *userContext) {
  (void)handle;
  CanBus *bus = (CanBus *)userContext;
  if (eventData->err_flags.val) {
    ++bus->busErrors;
  }
  return false;
}

static bool IRAM_ATTR onStateChange(twai_node_handle_t handle,
                                    const twai_state_change_event_data_t *eventData,
                                    void *userContext) {
  (void)handle;
  CanBus *bus = (CanBus *)userContext;
  if (eventData->new_sta == TWAI_ERROR_BUS_OFF) {
    bus->busOffSeen = true;
  }
  return false;
}

static bool configureAcceptAllFilters(twai_node_handle_t node) {
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

static bool configureRangeFilter(twai_node_handle_t node) {
  twai_range_filter_config_t rangeFilter = {};
  rangeFilter.range_low = FILTER_STD_ID_MIN;
  rangeFilter.range_high = FILTER_STD_ID_MAX;
  rangeFilter.is_ext = false;
  rangeFilter.no_fd = true;
  return twai_node_config_range_filter(node, 0, &rangeFilter) == ESP_OK;
}

static bool startBus(CanBus &bus) {
  bus.rxQueue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(CanFrame));
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
  nodeConfig.tx_queue_depth = 1;

  esp_err_t err = twai_new_node_onchip(&nodeConfig, &bus.node);
  if (err != ESP_OK) {
    Serial.printf("[%s] node create failed: 0x%X\n", bus.name, err);
    return false;
  }

#if ENABLE_SOFTWARE_ID_FILTER
  if (!configureRangeFilter(bus.node)) {
    Serial.printf("[%s] range filter config failed\n", bus.name);
    return false;
  }
#else
  if (!configureAcceptAllFilters(bus.node)) {
    Serial.printf("[%s] accept-all filter config failed\n", bus.name);
    return false;
  }
#endif

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

static bool frameAllowed(const CanFrame &frame) {
#if ENABLE_SOFTWARE_ID_FILTER
  if (frame.ide) {
    return false;
  }
  return frame.id >= FILTER_STD_ID_MIN && frame.id <= FILTER_STD_ID_MAX;
#else
  (void)frame;
  return true;
#endif
}

static esp_err_t transmitFrame(CanBus &bus, const CanFrame &frame) {
  if (!bus.started || bus.txBusy) {
    ++bus.droppedFrames;
    return ESP_ERR_INVALID_STATE;
  }

  if (!frameAllowed(frame)) {
    ++bus.droppedFrames;
    return ESP_ERR_NOT_SUPPORTED;
  }

  bus.txFrame = {};
  bus.txFrame.header.id = frame.id;
  bus.txFrame.header.dlc = frame.dlc;
  bus.txFrame.header.ide = frame.ide;
  bus.txFrame.header.rtr = frame.rtr;
  bus.txFrame.buffer = bus.txData;
  bus.txFrame.buffer_len = frame.rtr ? 0 : frame.dlc;
  memcpy(bus.txData, frame.data, frame.dlc);

  bus.txBusy = true;
  esp_err_t err = twai_node_transmit(bus.node, &bus.txFrame, 10);
  if (err != ESP_OK) {
    bus.txBusy = false;
    ++bus.txFailed;
  }
  return err;
}

static void recoverIfNeeded(CanBus &bus) {
  if (!bus.busOffSeen) {
    return;
  }

  twai_node_status_t status = {};
  twai_node_get_info(bus.node, &status, nullptr);
  if (status.state != TWAI_ERROR_BUS_OFF) {
    bus.busOffSeen = false;
    return;
  }

  ++bus.busOffCount;
  bus.started = false;
  Serial.printf("[%s] bus off, start recovery\n", bus.name);
  if (twai_node_recover(bus.node) == ESP_OK) {
    bus.busOffSeen = false;
  }
}

static void refreshRecovered(CanBus &bus) {
  if (bus.started) {
    return;
  }

  twai_node_status_t status = {};
  twai_node_get_info(bus.node, &status, nullptr);
  if (status.state == TWAI_ERROR_ACTIVE) {
    bus.started = true;
    bus.txBusy = false;
    Serial.printf("[%s] recovered\n", bus.name);
  }
}

static void receiveAndForward(CanBus &from, CanBus &to, bool forwardEnabled) {
  CanFrame frame = {};
  while (xQueueReceive(from.rxQueue, &frame, 0) == pdTRUE) {
    ++from.rxFrames;
    if (forwardEnabled) {
      transmitFrame(to, frame);
    }
  }
}

static const char *stateName(twai_error_state_t state) {
  switch (state) {
    case TWAI_ERROR_ACTIVE:
      return "active";
    case TWAI_ERROR_WARNING:
      return "warning";
    case TWAI_ERROR_PASSIVE:
      return "passive";
    case TWAI_ERROR_BUS_OFF:
      return "bus_off";
    default:
      return "unknown";
  }
}

static void printBusReport(CanBus &bus) {
  twai_node_status_t status = {};
  twai_node_record_t record = {};
  twai_node_get_info(bus.node, &status, &record);

  Serial.printf("[%s] state=%s rx=%" PRIu32 " tx=%" PRIu32
                " drop=%" PRIu32 " isrDrop=%" PRIu32 " txFail=%" PRIu32
                " busErr=%" PRIu32 " recordBusErr=%" PRIu32 " busOff=%" PRIu32
                " txErr=%u rxErr=%u txFree=%" PRIu32 "\n",
                bus.name,
                stateName(status.state),
                bus.rxFrames,
                (uint32_t)bus.txFrames,
                bus.droppedFrames,
                (uint32_t)bus.isrDrops,
                (uint32_t)bus.txFailed,
                (uint32_t)bus.busErrors,
                record.bus_err_num,
                bus.busOffCount,
                status.tx_error_count,
                status.rx_error_count,
                status.tx_queue_remaining);
}

static void report() {
  uint32_t now = millis();
  if (now - lastReportMs < REPORT_INTERVAL_MS) {
    return;
  }
  lastReportMs = now;

  printBusReport(can1);
  printBusReport(can2);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("T-C5-Base dual CAN advanced bridge");

  startBus(can1);
  startBus(can2);
  lastReportMs = millis();
}

void loop() {
  recoverIfNeeded(can1);
  recoverIfNeeded(can2);
  refreshRecovered(can1);
  refreshRecovered(can2);

  receiveAndForward(can1, can2, FORWARD_CAN1_TO_CAN2);
  receiveAndForward(can2, can1, FORWARD_CAN2_TO_CAN1);

  report();
  delay(1);
}
