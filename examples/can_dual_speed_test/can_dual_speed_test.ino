/**
 * @file      can_speed_test.ino
 * @author    Lewis He (lewishe@outlook.com)
 * @license   MIT
 * @copyright Copyright (c) 2026  ShenZhen XinYuan Electronic Technology Co., Ltd
 * @date      2026-08-21
 * @note      T-C5-Base Shield dual CAN speed test.
 * 
 * This sketch uses the two on-chip TWAI controllers on one ESP32-C5 board.
 * Connect CAN1 and CAN2 to the same physical CAN bus so each controller can ACK
 * and receive frames from the other controller.
 *
 * Note: Arduino-ESP32 does not currently expose a native Arduino API for both
 * ESP32-C5 on-chip TWAI controllers, so this CAN example uses the ESP-IDF TWAI
 * driver APIs inside an Arduino sketch.
 * 
 * Add CAN termination according to your wiring. For a normal two-node CAN bus,
 * use 120 ohm termination at both bus ends.
*/

#include <Arduino.h>
#include <inttypes.h>
#include <string.h>
#include "SSD1306Wire.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define ACTIVE_CAN1 1
#define ACTIVE_CAN2 1
#define CAN_BITRATE 1000000
#define CAN1_TO_CAN2_ID 0x321
#define CAN2_TO_CAN1_ID 0x421
#define RX_QUEUE_DEPTH 128
#define TX_QUEUE_DEPTH 2
#define REPORT_INTERVAL_MS 1000
#define LOOP_YIELD_MS 1
#define OLED_I2C_ADDRESS 0x3C
#define OLED_SDA_PIN 2
#define OLED_SCL_PIN 3

#if !ACTIVE_CAN1 || !ACTIVE_CAN2
#error "This dual speed test needs both ACTIVE_CAN1 and ACTIVE_CAN2 enabled."
#endif

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
  uint8_t data[TWAI_FRAME_MAX_LEN];
};

struct CanBus {
  const char *name;
  int txPin;
  int rxPin;
  uint32_t txId;
  uint32_t expectedRxId;
  uint8_t txMarker;
  uint8_t expectedRxMarker;
  twai_node_handle_t node;
  QueueHandle_t rxQueue;
  twai_frame_t txFrame;
  uint8_t txData[TWAI_FRAME_MAX_LEN];
  volatile bool txBusy;
  volatile bool busOffSeen;
  volatile uint32_t isrDrops;
  volatile uint32_t txFrames;
  volatile uint32_t txBytes;
  volatile uint32_t txFailed;
  volatile uint32_t busErrors;
  bool started;
  uint32_t txSequence;
  uint32_t expectedRxSequence;
  uint32_t rxFrames;
  uint32_t rxBytes;
  uint32_t goodFrames;
  uint32_t badFrames;
  uint32_t unexpectedFrames;
  uint32_t rxQueueDrops;
  uint32_t busOffCount;
};

struct CanReport {
  const char *name;
  uint32_t txFrameRate;
  uint32_t txByteRate;
  uint32_t rxFrameRate;
  uint32_t rxByteRate;
  uint32_t goodFrames;
  uint32_t badFrames;
  uint32_t unexpectedFrames;
  uint32_t txFailed;
  uint32_t isrDrops;
  uint32_t rxQueueDrops;
  uint32_t busErrors;
  uint32_t recordBusErrors;
  uint32_t busOffCount;
  twai_error_state_t state;
  uint8_t txErrorCount;
  uint8_t rxErrorCount;
  bool pass;
};

static CanBus can1 = {
    "CAN1", CAN1_TX_PIN, CAN1_RX_PIN, CAN1_TO_CAN2_ID, CAN2_TO_CAN1_ID, 0xA1, 0xB2,
    nullptr, nullptr, {}, {}, false, false, 0, 0, 0, 0, 0, false,
    0, 0, 0, 0, 0, 0, 0, 0, 0};

static CanBus can2 = {
    "CAN2", CAN2_TX_PIN, CAN2_RX_PIN, CAN2_TO_CAN1_ID, CAN1_TO_CAN2_ID, 0xB2, 0xA1,
    nullptr, nullptr, {}, {}, false, false, 0, 0, 0, 0, 0, false,
    0, 0, 0, 0, 0, 0, 0, 0, 0};

static uint32_t lastReportMs = 0;
static SSD1306Wire oled(OLED_I2C_ADDRESS, OLED_SDA_PIN, OLED_SCL_PIN);
static bool oledReady = false;
static bool oledStatusValid = false;
static bool oledLastPass = false;
static bool oledLastPass1 = false;
static bool oledLastPass2 = false;

static uint8_t checksumPayload(const uint8_t *data, size_t len) {
  uint8_t sum = 0x5A;
  for (size_t i = 0; i < len; ++i) {
    sum = (uint8_t)((sum << 1) | (sum >> 7));
    sum ^= data[i];
  }
  return sum;
}

static void fillPayload(CanBus &bus) {
  const uint32_t sequence = bus.txSequence++;
  bus.txData[0] = bus.txMarker;
  memcpy(&bus.txData[1], &sequence, sizeof(sequence));
  bus.txData[5] = (uint8_t)~bus.txMarker;
  bus.txData[6] = (uint8_t)(sequence ^ (sequence >> 8) ^ (sequence >> 16) ^ (sequence >> 24));
  bus.txData[7] = checksumPayload(bus.txData, 7);
}

static bool verifyPayload(CanBus &bus, const CanFrame &frame) {
  if (frame.ide || frame.rtr || frame.id != bus.expectedRxId || frame.dlc != 8) {
    ++bus.unexpectedFrames;
    return false;
  }

  uint32_t sequence = 0;
  memcpy(&sequence, &frame.data[1], sizeof(sequence));

  const bool payloadOk =
      frame.data[0] == bus.expectedRxMarker &&
      frame.data[5] == (uint8_t)~bus.expectedRxMarker &&
      frame.data[6] == (uint8_t)(sequence ^ (sequence >> 8) ^ (sequence >> 16) ^ (sequence >> 24)) &&
      frame.data[7] == checksumPayload(frame.data, 7);

  if (!payloadOk) {
    return false;
  }

  if (sequence != bus.expectedRxSequence) {
    bus.expectedRxSequence = sequence + 1;
    return false;
  }

  ++bus.expectedRxSequence;
  return true;
}

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
    bus->rxQueueDrops = bus->rxQueueDrops + 1;
  }

  return taskWoken == pdTRUE;
}

static bool IRAM_ATTR onTxDone(twai_node_handle_t handle,
                               const twai_tx_done_event_data_t *eventData,
                               void *userContext) {
  (void)handle;
  CanBus *bus = (CanBus *)userContext;

  if (eventData->is_tx_success) {
    bus->txFrames = bus->txFrames + 1;
    bus->txBytes += eventData->done_tx_frame->buffer_len;
  } else {
    bus->txFailed = bus->txFailed + 1;
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
    bus->busErrors = bus->busErrors + 1;
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
  nodeConfig.tx_queue_depth = TX_QUEUE_DEPTH;

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
  Serial.printf("[%s] started, TX=GPIO%d RX=GPIO%d txId=0x%03" PRIX32
                " rxId=0x%03" PRIX32 " bitrate=%d bit/s\n",
                bus.name, bus.txPin, bus.rxPin, bus.txId, bus.expectedRxId, CAN_BITRATE);
  return true;
}

static void transmitNext(CanBus &bus) {
  if (!bus.started || bus.txBusy) {
    return;
  }

  fillPayload(bus);

  bus.txFrame = {};
  bus.txFrame.header.id = bus.txId;
  bus.txFrame.header.dlc = 8;
  bus.txFrame.header.ide = false;
  bus.txFrame.header.rtr = false;
  bus.txFrame.buffer = bus.txData;
  bus.txFrame.buffer_len = 8;

  bus.txBusy = true;
  esp_err_t err = twai_node_transmit(bus.node, &bus.txFrame, 0);
  if (err != ESP_OK) {
    bus.txBusy = false;
    bus.txFailed = bus.txFailed + 1;
  }
}

static void drainRx(CanBus &bus) {
  CanFrame frame = {};
  while (xQueueReceive(bus.rxQueue, &frame, 0) == pdTRUE) {
    ++bus.rxFrames;
    if (!frame.rtr) {
      bus.rxBytes += frame.dlc;
    }

    if (verifyPayload(bus, frame)) {
      ++bus.goodFrames;
    } else {
      ++bus.badFrames;
    }
  }
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
  oled.drawString(0, 12, "CAN speed test");
  oled.drawString(0, 24, line2);
  oled.display();
}

static void initOled() {
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

static void updateOledCan(const CanReport &report1, const CanReport &report2) {
  if (!oledReady) {
    return;
  }

  const bool pass = report1.pass && report2.pass;
  if (oledStatusValid &&
      pass == oledLastPass &&
      report1.pass == oledLastPass1 &&
      report2.pass == oledLastPass2) {
    return;
  }

  oledStatusValid = true;
  oledLastPass = pass;
  oledLastPass1 = report1.pass;
  oledLastPass2 = report2.pass;

  oled.clear();
  oled.drawString(0, 0, String("CAN ") + CAN_BITRATE + "bps");
  oled.drawString(0, 12, String(pass ? "PASS" : "FAIL") +
                            " C1 " + report1.goodFrames + "/" + report1.rxFrameRate + "fps");
  oled.drawString(0, 24, String("C2 ") + (report2.pass ? "PASS " : "FAIL ") +
                            report2.goodFrames + "/" + report2.rxFrameRate + "fps");
  oled.drawString(0, 36, String("Err ") + (report1.busErrors + report2.busErrors) +
                            " Off " + (report1.busOffCount + report2.busOffCount));
  oled.drawString(0, 48, String("Bad ") + (report1.badFrames + report2.badFrames) +
                            " Drop " + (report1.isrDrops + report1.rxQueueDrops +
                                         report2.isrDrops + report2.rxQueueDrops));
  oled.display();
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
  bus.txBusy = false;
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

static CanReport printBusReport(CanBus &bus, uint32_t elapsedMs) {
  twai_node_status_t status = {};
  twai_node_record_t record = {};
  twai_node_get_info(bus.node, &status, &record);

  CanReport report = {};
  report.name = bus.name;
  report.txFrameRate = (bus.txFrames * 1000UL) / elapsedMs;
  report.txByteRate = (bus.txBytes * 1000UL) / elapsedMs;
  report.rxFrameRate = (bus.rxFrames * 1000UL) / elapsedMs;
  report.rxByteRate = (bus.rxBytes * 1000UL) / elapsedMs;
  report.goodFrames = bus.goodFrames;
  report.badFrames = bus.badFrames;
  report.unexpectedFrames = bus.unexpectedFrames;
  report.txFailed = bus.txFailed;
  report.isrDrops = bus.isrDrops;
  report.rxQueueDrops = bus.rxQueueDrops;
  report.busErrors = bus.busErrors;
  report.recordBusErrors = record.bus_err_num;
  report.busOffCount = bus.busOffCount;
  report.state = status.state;
  report.txErrorCount = status.tx_error_count;
  report.rxErrorCount = status.rx_error_count;
  report.pass = bus.started &&
                report.goodFrames > 0 &&
                report.badFrames == 0 &&
                report.unexpectedFrames == 0 &&
                report.txFailed == 0 &&
                report.isrDrops == 0 &&
                report.rxQueueDrops == 0 &&
                report.busErrors == 0 &&
                report.busOffCount == 0 &&
                report.state == TWAI_ERROR_ACTIVE &&
                report.txErrorCount == 0 &&
                report.rxErrorCount == 0;

  Serial.printf("[%s] TX=%" PRIu32 " frame/s %" PRIu32 " B/s fail=%" PRIu32
                " | RX=%" PRIu32 " frame/s %" PRIu32 " B/s good=%" PRIu32
                " bad=%" PRIu32 " unexpected=%" PRIu32
                " | isrDrop=%" PRIu32 " qDrop=%" PRIu32
                " | state=%s txErr=%u rxErr=%u txFree=%" PRIu32
                " busErr=%" PRIu32 "/%" PRIu32 " busOff=%" PRIu32 "\n",
                bus.name,
                report.txFrameRate,
                report.txByteRate,
                report.txFailed,
                report.rxFrameRate,
                report.rxByteRate,
                report.goodFrames,
                report.badFrames,
                report.unexpectedFrames,
                report.isrDrops,
                report.rxQueueDrops,
                stateName(status.state),
                status.tx_error_count,
                status.rx_error_count,
                status.tx_queue_remaining,
                report.busErrors,
                report.recordBusErrors,
                report.busOffCount);

  bus.txFrames = 0;
  bus.txBytes = 0;
  bus.txFailed = 0;
  bus.rxFrames = 0;
  bus.rxBytes = 0;
  bus.goodFrames = 0;
  bus.badFrames = 0;
  bus.unexpectedFrames = 0;
  bus.isrDrops = 0;
  bus.rxQueueDrops = 0;
  bus.busErrors = 0;

  return report;
}

static void reportSpeed() {
  const uint32_t now = millis();
  if (now - lastReportMs < REPORT_INTERVAL_MS) {
    return;
  }

  const uint32_t elapsedMs = now - lastReportMs;
  lastReportMs = now;

  CanReport report1 = printBusReport(can1, elapsedMs);
  CanReport report2 = printBusReport(can2, elapsedMs);
  updateOledCan(report1, report2);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("T-C5-Base dual CAN speed test");
  Serial.printf("CAN1 TX=GPIO%d RX=GPIO%d, CAN2 TX=GPIO%d RX=GPIO%d, bitrate=%d bit/s\n",
                can1.txPin, can1.rxPin, can2.txPin, can2.rxPin, CAN_BITRATE);

  initOled();
  startBus(can1);
  startBus(can2);
  drawOledBoot("CAN bus started");
  lastReportMs = millis();
}

void loop() {
  recoverIfNeeded(can1);
  recoverIfNeeded(can2);
  refreshRecovered(can1);
  refreshRecovered(can2);

  drainRx(can1);
  drainRx(can2);
  static bool sendCan1Next = true;
  if (sendCan1Next) {
    transmitNext(can1);
  } else {
    transmitNext(can2);
  }
  sendCan1Next = !sendCan1Next;

  reportSpeed();

  if (LOOP_YIELD_MS > 0) {
    delay(LOOP_YIELD_MS);
  } else {
    yield();
  }
}
