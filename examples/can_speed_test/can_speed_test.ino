/**
 * @file      can_speed_test.ino
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2026  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2026-08-21
 * @note      T-C5-Base Shield CAN speed test.
 * 
 *   Select CAN1 or CAN2 with ACTIVE_CAN. In normal bus operation, another CAN node
 *   must acknowledge frames. Set ENABLE_SELF_TEST_NO_ACK to 1 for bench-only tests.
 * 
 *   Note: Arduino-ESP32 does not currently expose a native Arduino API for the
 *   ESP32-C5 on-chip TWAI controller, so this CAN example uses the ESP-IDF TWAI
 *   driver APIs inside an Arduino sketch.
 * 
 *   Add CAN termination according to your wiring. For a normal two-node CAN bus,
 *   use 120 ohm termination at both bus ends.
*/

#include <Arduino.h>
#include <inttypes.h>
#include <string.h>
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define ACTIVE_CAN 1
#define CAN_BITRATE 500000
#define CAN_TEST_ID 0x321
#define ENABLE_TX_TEST 1
#define ENABLE_SELF_TEST_NO_ACK 0
#define RX_QUEUE_DEPTH 64
#define REPORT_INTERVAL_MS 1000

#if ACTIVE_CAN != 1 && ACTIVE_CAN != 2
#error "ACTIVE_CAN must be 1 or 2"
#endif


// CAN1 pins
#define CAN1_TX_PIN 26
#define CAN1_RX_PIN 25
// CAN2 pins
#define CAN2_TX_PIN 0
#define CAN2_RX_PIN 1

static constexpr const char *CAN_NAME = ACTIVE_CAN == 1 ? "CAN1" : "CAN2";
static constexpr int CAN_TX_PIN = ACTIVE_CAN == 1 ? CAN1_TX_PIN : CAN2_TX_PIN;
static constexpr int CAN_RX_PIN = ACTIVE_CAN == 1 ? CAN1_RX_PIN : CAN2_RX_PIN;

struct CanFrame {
  uint32_t id;
  uint8_t dlc;
  bool ide;
  bool rtr;
  uint8_t data[TWAI_FRAME_MAX_LEN];
};

static twai_node_handle_t canNode = nullptr;
static QueueHandle_t rxQueue = nullptr;
static twai_frame_t txFrame = {};
static uint8_t txData[TWAI_FRAME_MAX_LEN] = {};
static volatile bool txBusy = false;
static volatile bool busOffSeen = false;
static volatile uint32_t isrDrops = 0;
static volatile uint32_t txFrames = 0;
static volatile uint32_t txBytes = 0;
static volatile uint32_t txFails = 0;
static volatile uint32_t busErrors = 0;
static uint32_t txSequence = 0;
static uint32_t rxFrames = 0;
static uint32_t rxBytes = 0;
static uint32_t lastReportMs = 0;
static bool canStarted = false;

static bool IRAM_ATTR onRxDone(twai_node_handle_t handle,
                               const twai_rx_done_event_data_t *eventData,
                               void *userContext) {
  (void)eventData;
  (void)userContext;

  uint8_t rxData[TWAI_FRAME_MAX_LEN] = {};
  twai_frame_t rxFrame = {};
  rxFrame.buffer = rxData;
  rxFrame.buffer_len = sizeof(rxData);

  BaseType_t taskWoken = pdFALSE;
  if (twai_node_receive_from_isr(handle, &rxFrame) != ESP_OK) {
    ++isrDrops;
    return taskWoken == pdTRUE;
  }

  CanFrame queuedFrame = {};
  queuedFrame.id = rxFrame.header.id;
  queuedFrame.dlc = rxFrame.header.dlc > TWAI_FRAME_MAX_LEN ? TWAI_FRAME_MAX_LEN : rxFrame.header.dlc;
  queuedFrame.ide = rxFrame.header.ide;
  queuedFrame.rtr = rxFrame.header.rtr;
  memcpy(queuedFrame.data, rxData, queuedFrame.dlc);

  if (xQueueSendFromISR(rxQueue, &queuedFrame, &taskWoken) != pdTRUE) {
    ++isrDrops;
  }

  return taskWoken == pdTRUE;
}

static bool IRAM_ATTR onTxDone(twai_node_handle_t handle,
                               const twai_tx_done_event_data_t *eventData,
                               void *userContext) {
  (void)handle;
  (void)userContext;

  if (eventData->is_tx_success) {
    ++txFrames;
    txBytes += eventData->done_tx_frame->buffer_len;
  } else {
    ++txFails;
  }
  txBusy = false;

  return false;
}

static bool IRAM_ATTR onError(twai_node_handle_t handle,
                              const twai_error_event_data_t *eventData,
                              void *userContext) {
  (void)handle;
  (void)userContext;

  if (eventData->err_flags.val) {
    ++busErrors;
  }

  return false;
}

static bool IRAM_ATTR onStateChange(twai_node_handle_t handle,
                                    const twai_state_change_event_data_t *eventData,
                                    void *userContext) {
  (void)handle;
  (void)userContext;

  if (eventData->new_sta == TWAI_ERROR_BUS_OFF) {
    busOffSeen = true;
  }

  return false;
}

static bool configureAcceptAllFilters() {
  twai_mask_filter_config_t standardFilter = {};
  standardFilter.id = 0;
  standardFilter.mask = 0;
  standardFilter.is_ext = false;
  standardFilter.no_fd = true;

  if (twai_node_config_mask_filter(canNode, 0, &standardFilter) != ESP_OK) {
    return false;
  }

  twai_mask_filter_config_t extendedFilter = standardFilter;
  extendedFilter.is_ext = true;
  return twai_node_config_mask_filter(canNode, 1, &extendedFilter) == ESP_OK;
}

static bool startCan() {
  rxQueue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(CanFrame));
  if (rxQueue == nullptr) {
    Serial.printf("[%s] failed to create RX queue\n", CAN_NAME);
    return false;
  }

  twai_onchip_node_config_t nodeConfig = {};
  nodeConfig.io_cfg.tx = (gpio_num_t)CAN_TX_PIN;
  nodeConfig.io_cfg.rx = (gpio_num_t)CAN_RX_PIN;
  nodeConfig.io_cfg.quanta_clk_out = GPIO_NUM_NC;
  nodeConfig.io_cfg.bus_off_indicator = GPIO_NUM_NC;
  nodeConfig.bit_timing.bitrate = CAN_BITRATE;
  nodeConfig.fail_retry_cnt = 3;
  nodeConfig.tx_queue_depth = 1;
  nodeConfig.flags.enable_self_test = ENABLE_SELF_TEST_NO_ACK;

  esp_err_t err = twai_new_node_onchip(&nodeConfig, &canNode);
  if (err != ESP_OK) {
    Serial.printf("[%s] node create failed: 0x%X\n", CAN_NAME, err);
    return false;
  }

  if (!configureAcceptAllFilters()) {
    Serial.printf("[%s] filter config failed\n", CAN_NAME);
    return false;
  }

  twai_event_callbacks_t callbacks = {};
  callbacks.on_rx_done = onRxDone;
  callbacks.on_tx_done = onTxDone;
  callbacks.on_error = onError;
  callbacks.on_state_change = onStateChange;

  err = twai_node_register_event_callbacks(canNode, &callbacks, nullptr);
  if (err != ESP_OK) {
    Serial.printf("[%s] callback register failed: 0x%X\n", CAN_NAME, err);
    return false;
  }

  err = twai_node_enable(canNode);
  if (err != ESP_OK) {
    Serial.printf("[%s] node enable failed: 0x%X\n", CAN_NAME, err);
    return false;
  }

  Serial.printf("[%s] speed test started, TX=GPIO%d RX=GPIO%d bitrate=%d bit/s selfTest=%d\n",
                CAN_NAME, CAN_TX_PIN, CAN_RX_PIN, CAN_BITRATE, ENABLE_SELF_TEST_NO_ACK);
  return true;
}

static void sendTestFrame() {
  if (!canStarted || txBusy) {
    return;
  }

  uint32_t sequence = txSequence++;
  uint32_t timestamp = micros();

  txFrame = {};
  txFrame.header.id = CAN_TEST_ID;
  txFrame.header.dlc = 8;
  txFrame.buffer = txData;
  txFrame.buffer_len = 8;
  memcpy(&txData[0], &sequence, sizeof(sequence));
  memcpy(&txData[4], &timestamp, sizeof(timestamp));

  txBusy = true;
  esp_err_t err = twai_node_transmit(canNode, &txFrame, 0);
  if (err != ESP_OK) {
    txBusy = false;
    ++txFails;
  }
}

static void drainRx() {
  CanFrame frame = {};
  while (xQueueReceive(rxQueue, &frame, 0) == pdTRUE) {
    ++rxFrames;
    if (!frame.rtr) {
      rxBytes += frame.dlc;
    }
  }
}

static void recoverIfNeeded() {
  if (!busOffSeen) {
    return;
  }

  twai_node_status_t status = {};
  twai_node_get_info(canNode, &status, nullptr);
  if (status.state == TWAI_ERROR_BUS_OFF) {
    Serial.printf("[%s] bus off, start recovery\n", CAN_NAME);
    twai_node_recover(canNode);
    canStarted = false;
  }
  busOffSeen = false;
}

static void refreshRecovered() {
  if (canStarted) {
    return;
  }

  twai_node_status_t status = {};
  twai_node_get_info(canNode, &status, nullptr);
  if (status.state == TWAI_ERROR_ACTIVE) {
    canStarted = true;
    txBusy = false;
    Serial.printf("[%s] recovered\n", CAN_NAME);
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

static void reportSpeed() {
  uint32_t now = millis();
  if (now - lastReportMs < REPORT_INTERVAL_MS) {
    return;
  }

  uint32_t elapsedMs = now - lastReportMs;
  lastReportMs = now;

  twai_node_status_t status = {};
  twai_node_record_t record = {};
  twai_node_get_info(canNode, &status, &record);

  uint32_t txFrameRate = (txFrames * 1000UL) / elapsedMs;
  uint32_t txByteRate = (txBytes * 1000UL) / elapsedMs;
  uint32_t rxFrameRate = (rxFrames * 1000UL) / elapsedMs;
  uint32_t rxByteRate = (rxBytes * 1000UL) / elapsedMs;

  Serial.printf("[%s] TX=%" PRIu32 " frame/s %" PRIu32 " B/s fail=%" PRIu32
                " | RX=%" PRIu32 " frame/s %" PRIu32 " B/s isrDrop=%" PRIu32
                " | state=%s txErr=%u rxErr=%u txFree=%" PRIu32
                " busErr=%" PRIu32 "/%" PRIu32 "\n",
                CAN_NAME,
                txFrameRate,
                txByteRate,
                (uint32_t)txFails,
                rxFrameRate,
                rxByteRate,
                (uint32_t)isrDrops,
                stateName(status.state),
                status.tx_error_count,
                status.rx_error_count,
                status.tx_queue_remaining,
                (uint32_t)busErrors,
                record.bus_err_num);

  txFrames = 0;
  txBytes = 0;
  txFails = 0;
  rxFrames = 0;
  rxBytes = 0;
  isrDrops = 0;
  busErrors = 0;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("T-C5-Base CAN speed test");

  canStarted = startCan();
  lastReportMs = millis();
}

void loop() {
  if (canNode == nullptr) {
    delay(1000);
    return;
  }

  recoverIfNeeded();
  refreshRecovered();

#if ENABLE_TX_TEST
  sendTestFrame();
#endif

  drainRx();
  reportSpeed();
  delay(1);
}
