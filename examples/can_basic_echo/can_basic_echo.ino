/**
 * @file      can_dual_basic_echo.ino
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2026  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2026-08-21
 * @note      T-C5-Base Shield CAN basic echo example.
 *           
 *         
 *           Select CAN1 or CAN2 with ACTIVE_CAN. The sketch receives classic CAN frames
 *           and transmits the same frame back on the selected bus.
 *         
 *           Note: Arduino-ESP32 does not currently expose a native Arduino API for the
 *           ESP32-C5 on-chip TWAI controller, so this CAN example uses the ESP-IDF TWAI
 *           driver APIs inside an Arduino sketch.
 * 
 *           Add CAN termination according to your wiring. For a normal two-node CAN bus,
 *           use 120 ohm termination at both bus ends.
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
#define RX_QUEUE_DEPTH 16

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
static volatile uint32_t txOk = 0;
static volatile uint32_t txFail = 0;
static volatile uint32_t busErrors = 0;
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
    ++txOk;
  } else {
    ++txFail;
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

  Serial.printf("[%s] started, TX=GPIO%d RX=GPIO%d bitrate=%d bit/s\n",
                CAN_NAME, CAN_TX_PIN, CAN_RX_PIN, CAN_BITRATE);
  return true;
}

static void printFrame(const CanFrame &frame) {
  Serial.printf("[%s] RX %s ID=0x%08" PRIX32 " DLC=%u",
                CAN_NAME,
                frame.ide ? "EXT" : "STD",
                frame.id,
                (unsigned)frame.dlc);

  if (frame.rtr) {
    Serial.println(" RTR");
    return;
  }

  Serial.print(" DATA=");
  for (uint8_t i = 0; i < frame.dlc; ++i) {
    Serial.printf("%02X", frame.data[i]);
    if (i + 1 < frame.dlc) {
      Serial.print(' ');
    }
  }
  Serial.println();
}

static void echoFrame(const CanFrame &frame) {
  if (txBusy) {
    Serial.printf("[%s] echo skipped: TX busy\n", CAN_NAME);
    return;
  }

  txFrame = {};
  txFrame.header.id = frame.id;
  txFrame.header.dlc = frame.dlc;
  txFrame.header.ide = frame.ide;
  txFrame.header.rtr = frame.rtr;
  txFrame.buffer = txData;
  txFrame.buffer_len = frame.rtr ? 0 : frame.dlc;
  memcpy(txData, frame.data, frame.dlc);

  txBusy = true;
  esp_err_t err = twai_node_transmit(canNode, &txFrame, 20);
  if (err != ESP_OK) {
    txBusy = false;
    ++txFail;
    Serial.printf("[%s] echo failed: 0x%X\n", CAN_NAME, err);
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

static void pollCan() {
  recoverIfNeeded();
  refreshRecovered();

  CanFrame frame = {};
  while (xQueueReceive(rxQueue, &frame, 0) == pdTRUE) {
    printFrame(frame);
    echoFrame(frame);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("T-C5-Base CAN basic echo");

  canStarted = startCan();
}

void loop() {
  if (canNode == nullptr) {
    delay(1000);
    return;
  }

  pollCan();
  delay(1);
}
