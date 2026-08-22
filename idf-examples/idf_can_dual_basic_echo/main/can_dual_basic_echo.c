#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#ifndef CAN_BITRATE
#define CAN_BITRATE 1000000
#endif

#define CAN1_TO_CAN2_ID 0x321
#define CAN2_TO_CAN1_ID 0x421
#define CAN_RX_QUEUE_DEPTH 16
#define CAN_TX_QUEUE_DEPTH 2
#define CAN_RX_TIMEOUT_MS 150
#define AUTO_SEND_INTERVAL_MS 2000
#define TASK_YIELD_DELAY_MS 10

#define CAN1_TX_PIN 26
#define CAN1_RX_PIN 25
#define CAN2_TX_PIN 0
#define CAN2_RX_PIN 1

typedef struct {
    uint32_t id;
    uint8_t dlc;
    bool ide;
    bool rtr;
    uint8_t data[TWAI_FRAME_MAX_LEN];
} can_frame_record_t;

typedef struct {
    const char *name;
    int tx_pin;
    int rx_pin;
    twai_node_handle_t node;
    QueueHandle_t rx_queue;
    twai_frame_t tx_frame;
    uint8_t tx_data[TWAI_FRAME_MAX_LEN];
    volatile bool bus_off_seen;
    volatile uint32_t isr_drops;
    volatile uint32_t tx_ok;
    volatile uint32_t tx_fail;
    volatile uint32_t bus_errors;
    bool started;
} can_bus_t;

static const char *TAG = "idf_dual_can";

static can_bus_t s_can1 = {
    .name = "CAN1",
    .tx_pin = CAN1_TX_PIN,
    .rx_pin = CAN1_RX_PIN,
};

static can_bus_t s_can2 = {
    .name = "CAN2",
    .tx_pin = CAN2_TX_PIN,
    .rx_pin = CAN2_RX_PIN,
};

static uint32_t s_sequence;
static uint32_t s_ok_count;
static uint32_t s_fail_count;
static int64_t s_last_test_us;

static uint8_t checksum_payload(const uint8_t *data, size_t len)
{
    uint8_t sum = 0x5A;
    for (size_t i = 0; i < len; ++i) {
        sum = (uint8_t)((sum << 1) | (sum >> 7));
        sum ^= data[i];
    }
    return sum;
}

static void fill_payload(uint8_t *data, uint32_t sequence)
{
    data[0] = 0xC5;
    memcpy(&data[1], &sequence, sizeof(sequence));
    data[5] = 0x5C;
    data[6] = (uint8_t)(sequence ^ (sequence >> 8) ^ (sequence >> 16) ^ (sequence >> 24));
    data[7] = checksum_payload(data, 7);
}

static bool verify_payload(const can_frame_record_t *frame, uint32_t expected_id, uint32_t sequence)
{
    if (frame->ide || frame->rtr || frame->id != expected_id || frame->dlc != 8) {
        return false;
    }

    uint32_t rx_sequence = 0;
    memcpy(&rx_sequence, &frame->data[1], sizeof(rx_sequence));
    return frame->data[0] == 0xC5 &&
           rx_sequence == sequence &&
           frame->data[5] == 0x5C &&
           frame->data[6] == (uint8_t)(sequence ^ (sequence >> 8) ^ (sequence >> 16) ^ (sequence >> 24)) &&
           frame->data[7] == checksum_payload(frame->data, 7);
}

static bool IRAM_ATTR on_rx_done(twai_node_handle_t handle,
                                 const twai_rx_done_event_data_t *event_data,
                                 void *user_context)
{
    (void)event_data;
    can_bus_t *bus = (can_bus_t *)user_context;

    uint8_t rx_data[TWAI_FRAME_MAX_LEN] = {};
    twai_frame_t rx_frame = {};
    rx_frame.buffer = rx_data;
    rx_frame.buffer_len = sizeof(rx_data);

    BaseType_t task_woken = pdFALSE;
    if (twai_node_receive_from_isr(handle, &rx_frame) != ESP_OK) {
        bus->isr_drops = bus->isr_drops + 1;
        return task_woken == pdTRUE;
    }

    can_frame_record_t queued_frame = {};
    queued_frame.id = rx_frame.header.id;
    queued_frame.dlc = rx_frame.header.dlc > TWAI_FRAME_MAX_LEN ? TWAI_FRAME_MAX_LEN : rx_frame.header.dlc;
    queued_frame.ide = rx_frame.header.ide;
    queued_frame.rtr = rx_frame.header.rtr;
    memcpy(queued_frame.data, rx_data, queued_frame.dlc);

    if (xQueueSendFromISR(bus->rx_queue, &queued_frame, &task_woken) != pdTRUE) {
        bus->isr_drops = bus->isr_drops + 1;
    }

    return task_woken == pdTRUE;
}

static bool IRAM_ATTR on_tx_done(twai_node_handle_t handle,
                                 const twai_tx_done_event_data_t *event_data,
                                 void *user_context)
{
    (void)handle;
    can_bus_t *bus = (can_bus_t *)user_context;

    if (event_data->is_tx_success) {
        bus->tx_ok = bus->tx_ok + 1;
    } else {
        bus->tx_fail = bus->tx_fail + 1;
    }
    return false;
}

static bool IRAM_ATTR on_error(twai_node_handle_t handle,
                               const twai_error_event_data_t *event_data,
                               void *user_context)
{
    (void)handle;
    can_bus_t *bus = (can_bus_t *)user_context;

    if (event_data->err_flags.val) {
        bus->bus_errors = bus->bus_errors + 1;
    }
    return false;
}

static bool IRAM_ATTR on_state_change(twai_node_handle_t handle,
                                      const twai_state_change_event_data_t *event_data,
                                      void *user_context)
{
    (void)handle;
    can_bus_t *bus = (can_bus_t *)user_context;

    if (event_data->new_sta == TWAI_ERROR_BUS_OFF) {
        bus->bus_off_seen = true;
    }
    return false;
}

static esp_err_t configure_accept_all_filters(twai_node_handle_t node)
{
    twai_mask_filter_config_t standard_filter = {};
    standard_filter.id = 0;
    standard_filter.mask = 0;
    standard_filter.is_ext = false;
    standard_filter.no_fd = true;

    ESP_RETURN_ON_ERROR(twai_node_config_mask_filter(node, 0, &standard_filter),
                        TAG, "standard filter config failed");

    twai_mask_filter_config_t extended_filter = standard_filter;
    extended_filter.is_ext = true;
    return twai_node_config_mask_filter(node, 1, &extended_filter);
}

static esp_err_t start_can_bus(can_bus_t *bus)
{
    bus->rx_queue = xQueueCreate(CAN_RX_QUEUE_DEPTH, sizeof(can_frame_record_t));
    ESP_RETURN_ON_FALSE(bus->rx_queue != NULL, ESP_ERR_NO_MEM, TAG, "%s RX queue create failed", bus->name);

    twai_onchip_node_config_t node_config = {};
    node_config.io_cfg.tx = (gpio_num_t)bus->tx_pin;
    node_config.io_cfg.rx = (gpio_num_t)bus->rx_pin;
    node_config.io_cfg.quanta_clk_out = GPIO_NUM_NC;
    node_config.io_cfg.bus_off_indicator = GPIO_NUM_NC;
    node_config.bit_timing.bitrate = CAN_BITRATE;
    node_config.fail_retry_cnt = 3;
    node_config.tx_queue_depth = CAN_TX_QUEUE_DEPTH;

    ESP_RETURN_ON_ERROR(twai_new_node_onchip(&node_config, &bus->node),
                        TAG, "%s node create failed", bus->name);
    ESP_RETURN_ON_ERROR(configure_accept_all_filters(bus->node),
                        TAG, "%s filter config failed", bus->name);

    twai_event_callbacks_t callbacks = {};
    callbacks.on_rx_done = on_rx_done;
    callbacks.on_tx_done = on_tx_done;
    callbacks.on_error = on_error;
    callbacks.on_state_change = on_state_change;

    ESP_RETURN_ON_ERROR(twai_node_register_event_callbacks(bus->node, &callbacks, bus),
                        TAG, "%s callback register failed", bus->name);
    ESP_RETURN_ON_ERROR(twai_node_enable(bus->node),
                        TAG, "%s node enable failed", bus->name);

    bus->started = true;
    ESP_LOGI(TAG, "[%s] started, TX=GPIO%d RX=GPIO%d bitrate=%d bit/s",
             bus->name, bus->tx_pin, bus->rx_pin, CAN_BITRATE);
    return ESP_OK;
}

static void clear_can_queue(const can_bus_t *bus)
{
    if (bus->rx_queue == NULL) {
        return;
    }

    can_frame_record_t ignored = {};
    for (uint32_t i = 0; i < CAN_RX_QUEUE_DEPTH; ++i) {
        if (xQueueReceive(bus->rx_queue, &ignored, 0) != pdTRUE) {
            break;
        }
    }
}

static esp_err_t transmit_can(can_bus_t *bus, uint32_t id, const uint8_t *data, uint8_t len)
{
    bus->tx_frame = (twai_frame_t){};
    bus->tx_frame.header.id = id;
    bus->tx_frame.header.dlc = len;
    bus->tx_frame.header.ide = false;
    bus->tx_frame.header.rtr = false;
    bus->tx_frame.buffer = bus->tx_data;
    bus->tx_frame.buffer_len = len;
    memcpy(bus->tx_data, data, len);

    esp_err_t err = twai_node_transmit(bus->node, &bus->tx_frame, pdMS_TO_TICKS(50));
    if (err != ESP_OK) {
        bus->tx_fail = bus->tx_fail + 1;
    }
    return err;
}

static bool wait_can_frame(const can_bus_t *bus, uint32_t expected_id,
                           can_frame_record_t *frame, uint32_t timeout_ms)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while ((int32_t)(xTaskGetTickCount() - deadline) < 0) {
        while (xQueueReceive(bus->rx_queue, frame, 0) == pdTRUE) {
            if (!frame->ide && !frame->rtr && frame->id == expected_id) {
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(TASK_YIELD_DELAY_MS));
    }
    return false;
}

static void print_can_frame(const char *label, const can_frame_record_t *frame)
{
    char data_text[3 * TWAI_FRAME_MAX_LEN + 1] = {};
    size_t offset = 0;
    for (uint8_t i = 0; i < frame->dlc && offset < sizeof(data_text); ++i) {
        offset += (size_t)snprintf(&data_text[offset], sizeof(data_text) - offset,
                                   "%02X%s", frame->data[i], i + 1 < frame->dlc ? " " : "");
    }

    ESP_LOGI(TAG, "[%s] ID=0x%03" PRIX32 " DLC=%u DATA=%s",
             label, frame->id, frame->dlc, data_text);
}

static bool echo_back(uint32_t sequence)
{
    if (!s_can1.started || s_can1.rx_queue == NULL ||
        !s_can2.started || s_can2.rx_queue == NULL) {
        ESP_LOGE(TAG, "[echo] FAIL: CAN node not started");
        ++s_fail_count;
        return false;
    }

    uint8_t payload[8] = {};
    can_frame_record_t rx_on_can2 = {};
    can_frame_record_t rx_on_can1 = {};
    fill_payload(payload, sequence);

    clear_can_queue(&s_can1);
    clear_can_queue(&s_can2);

    if (transmit_can(&s_can1, CAN1_TO_CAN2_ID, payload, sizeof(payload)) != ESP_OK) {
        ESP_LOGE(TAG, "[echo] FAIL: CAN1 transmit failed");
        ++s_fail_count;
        return false;
    }

    const bool forward_ok = wait_can_frame(&s_can2, CAN1_TO_CAN2_ID, &rx_on_can2, CAN_RX_TIMEOUT_MS) &&
                            verify_payload(&rx_on_can2, CAN1_TO_CAN2_ID, sequence);
    if (forward_ok) {
        print_can_frame("CAN1 -> CAN2", &rx_on_can2);
    } else {
        ESP_LOGE(TAG, "[CAN1 -> CAN2] FAIL");
    }

    if (!forward_ok ||
        transmit_can(&s_can2, CAN2_TO_CAN1_ID, rx_on_can2.data, rx_on_can2.dlc) != ESP_OK) {
        ESP_LOGE(TAG, "[echo] FAIL");
        ++s_fail_count;
        return false;
    }

    const bool back_ok = wait_can_frame(&s_can1, CAN2_TO_CAN1_ID, &rx_on_can1, CAN_RX_TIMEOUT_MS) &&
                         verify_payload(&rx_on_can1, CAN2_TO_CAN1_ID, sequence);
    if (back_ok) {
        print_can_frame("CAN2 -> CAN1", &rx_on_can1);
    } else {
        ESP_LOGE(TAG, "[CAN2 -> CAN1] FAIL");
    }

    const bool ok = forward_ok && back_ok;
    if (ok) {
        ++s_ok_count;
    } else {
        ++s_fail_count;
    }

    ESP_LOGI(TAG, "[echo] %s ok=%" PRIu32 " fail=%" PRIu32
             " txOk=%" PRIu32 "/%" PRIu32 " txFail=%" PRIu32 "/%" PRIu32
             " busErr=%" PRIu32 "/%" PRIu32 " isrDrop=%" PRIu32 "/%" PRIu32,
             ok ? "OK" : "FAIL",
             s_ok_count, s_fail_count,
             s_can1.tx_ok, s_can2.tx_ok,
             s_can1.tx_fail, s_can2.tx_fail,
             s_can1.bus_errors, s_can2.bus_errors,
             s_can1.isr_drops, s_can2.isr_drops);
    return ok;
}

static void recover_if_needed(can_bus_t *bus)
{
    if (!bus->bus_off_seen || bus->node == NULL) {
        return;
    }

    twai_node_status_t status = {};
    twai_node_get_info(bus->node, &status, NULL);
    if (status.state == TWAI_ERROR_BUS_OFF) {
        ESP_LOGW(TAG, "[%s] bus off, start recovery", bus->name);
        twai_node_recover(bus->node);
        bus->started = false;
    }
    bus->bus_off_seen = false;
}

static void refresh_recovered(can_bus_t *bus)
{
    if (bus->started || bus->node == NULL) {
        return;
    }

    twai_node_status_t status = {};
    twai_node_get_info(bus->node, &status, NULL);
    if (status.state == TWAI_ERROR_ACTIVE) {
        bus->started = true;
        ESP_LOGI(TAG, "[%s] recovered", bus->name);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "T-C5-Base ESP-IDF dual CAN echo-back test");
    ESP_LOGI(TAG, "CAN1 TX=GPIO%d RX=GPIO%d, CAN2 TX=GPIO%d RX=GPIO%d, bitrate=%d bit/s",
             s_can1.tx_pin, s_can1.rx_pin, s_can2.tx_pin, s_can2.rx_pin, CAN_BITRATE);

    ESP_ERROR_CHECK(start_can_bus(&s_can1));
    ESP_ERROR_CHECK(start_can_bus(&s_can2));
    s_last_test_us = esp_timer_get_time();

    while (true) {
        recover_if_needed(&s_can1);
        recover_if_needed(&s_can2);
        refresh_recovered(&s_can1);
        refresh_recovered(&s_can2);

        const int64_t now_us = esp_timer_get_time();
        if (now_us - s_last_test_us >= AUTO_SEND_INTERVAL_MS * 1000LL) {
            s_last_test_us = now_us;
            ESP_LOGI(TAG, "[Test %" PRIu32 "]", s_sequence);
            echo_back(s_sequence++);
        }

        vTaskDelay(pdMS_TO_TICKS(TASK_YIELD_DELAY_MS));
    }
}
