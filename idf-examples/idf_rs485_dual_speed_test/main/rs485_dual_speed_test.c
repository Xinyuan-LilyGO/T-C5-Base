#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

#if SOC_UART_HP_NUM < 2
#error "This test needs two high-performance UART controllers."
#endif

#ifndef RS485_BAUD
#define RS485_BAUD 5000000
#endif
#define TEST_PAYLOAD_SIZE 224
#define UART_RX_BUFFER_SIZE 4096
#define UART_TX_BUFFER_SIZE 1024
#define RECEIVE_TIMEOUT_MS 50
#define REPORT_INTERVAL_MS 1000
#define RX_DRAIN_CHUNK 256
#define LOOP_YIELD_MS 0

typedef struct {
    const char *name;
    uart_port_t port;
    int rx_pin;
    int tx_pin;
    int rts_pin;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint32_t good_frames;
    uint32_t bad_frames;
    uint32_t timeouts;
} rs485_bus_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t direction;
    uint8_t reserved;
    uint16_t payload_length;
    uint32_t sequence;
    uint32_t checksum;
    uint8_t payload[TEST_PAYLOAD_SIZE];
} test_frame_t;

static const char *TAG = "dual_rs485";

static const uint32_t FRAME_MAGIC = 0x35433854UL;
static const uint8_t DIR_1_TO_2 = 0x12;
static const uint8_t DIR_2_TO_1 = 0x21;

static rs485_bus_t s_rs4851 = {
    .name = "RS4851",
    .port = UART_NUM_1,
    .rx_pin = 4,
    .tx_pin = 5,
    .rts_pin = 27,
};

static rs485_bus_t s_rs4852 = {
    .name = "RS4852",
    .port = UART_NUM_0,
    .rx_pin = 24,
    .tx_pin = 23,
    .rts_pin = 7,
};

static uint32_t s_sequence12;
static uint32_t s_sequence21;
static int64_t s_last_report_us;

static uint32_t checksum_frame(const test_frame_t *frame)
{
    uint32_t sum = frame->magic;
    sum += frame->direction;
    sum += frame->reserved;
    sum += frame->payload_length;
    sum += frame->sequence;

    for (size_t i = 0; i < frame->payload_length && i < sizeof(frame->payload); ++i) {
        sum = (sum << 5) | (sum >> 27);
        sum += frame->payload[i];
    }

    return sum;
}

static void fill_frame(test_frame_t *frame, uint8_t direction, uint32_t sequence)
{
    memset(frame, 0, sizeof(*frame));
    frame->magic = FRAME_MAGIC;
    frame->direction = direction;
    frame->payload_length = TEST_PAYLOAD_SIZE;
    frame->sequence = sequence;

    for (size_t i = 0; i < sizeof(frame->payload); ++i) {
        frame->payload[i] = (uint8_t)(sequence + direction + i);
    }

    frame->checksum = checksum_frame(frame);
}

static bool verify_frame(const test_frame_t *frame, uint8_t expected_direction, uint32_t expected_sequence)
{
    if (frame->magic != FRAME_MAGIC ||
        frame->direction != expected_direction ||
        frame->payload_length != TEST_PAYLOAD_SIZE ||
        frame->sequence != expected_sequence) {
        return false;
    }

    return frame->checksum == checksum_frame(frame);
}

static void reset_counters(void)
{
    s_rs4851.tx_bytes = 0;
    s_rs4851.rx_bytes = 0;
    s_rs4851.good_frames = 0;
    s_rs4851.bad_frames = 0;
    s_rs4851.timeouts = 0;

    s_rs4852.tx_bytes = 0;
    s_rs4852.rx_bytes = 0;
    s_rs4852.good_frames = 0;
    s_rs4852.bad_frames = 0;
    s_rs4852.timeouts = 0;
}

static void drain_driver_rx(const rs485_bus_t *bus)
{
    uint8_t scratch[RX_DRAIN_CHUNK];
    while (uart_read_bytes(bus->port, scratch, sizeof(scratch), 0) > 0) {
    }
}

static esp_err_t start_bus(const rs485_bus_t *bus)
{
    if (uart_is_driver_installed(bus->port)) {
        ESP_RETURN_ON_ERROR(uart_driver_delete(bus->port), TAG, "%s uart_driver_delete failed", bus->name);
    }

    uart_config_t config = {
        .baud_rate = RS485_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_LOGI(TAG, "[%s] installing UART%d driver", bus->name, (int)bus->port);
    ESP_RETURN_ON_ERROR(uart_driver_install(bus->port, UART_RX_BUFFER_SIZE, UART_TX_BUFFER_SIZE, 0, NULL, 0),
                        TAG, "%s uart_driver_install failed", bus->name);
    ESP_RETURN_ON_ERROR(uart_param_config(bus->port, &config),
                        TAG, "%s uart_param_config failed", bus->name);
    ESP_RETURN_ON_ERROR(uart_set_pin(bus->port, bus->tx_pin, bus->rx_pin, bus->rts_pin, UART_PIN_NO_CHANGE),
                        TAG, "%s uart_set_pin failed", bus->name);
    ESP_RETURN_ON_ERROR(uart_set_mode(bus->port, UART_MODE_RS485_HALF_DUPLEX),
                        TAG, "%s uart_set_mode failed", bus->name);
    ESP_RETURN_ON_ERROR(uart_set_rx_timeout(bus->port, 3),
                        TAG, "%s uart_set_rx_timeout failed", bus->name);

    drain_driver_rx(bus);
    ESP_LOGI(TAG, "[%s] started, RX=GPIO%d TX=GPIO%d RTS=GPIO%d baud=%d",
             bus->name, bus->rx_pin, bus->tx_pin, bus->rts_pin, RS485_BAUD);
    return ESP_OK;
}

static void transfer_frame(rs485_bus_t *tx_bus,
                           rs485_bus_t *rx_bus,
                           uint8_t direction,
                           uint32_t *sequence)
{
    test_frame_t tx_frame;
    test_frame_t rx_frame;
    fill_frame(&tx_frame, direction, *sequence);
    memset(&rx_frame, 0, sizeof(rx_frame));

    drain_driver_rx(rx_bus);

    const int written = uart_write_bytes(tx_bus->port, &tx_frame, sizeof(tx_frame));
    if (written != (int)sizeof(tx_frame)) {
        ++tx_bus->timeouts;
        ++rx_bus->bad_frames;
        ++(*sequence);
        return;
    }

    if (uart_wait_tx_done(tx_bus->port, pdMS_TO_TICKS(RECEIVE_TIMEOUT_MS)) != ESP_OK) {
        ++tx_bus->timeouts;
        ++rx_bus->timeouts;
        ++(*sequence);
        return;
    }

    const int read_len = uart_read_bytes(rx_bus->port, &rx_frame, sizeof(rx_frame),
                                         pdMS_TO_TICKS(RECEIVE_TIMEOUT_MS));

    tx_bus->tx_bytes += written;
    if (read_len != (int)sizeof(rx_frame)) {
        ++rx_bus->timeouts;
        ++rx_bus->bad_frames;
        ++(*sequence);
        return;
    }

    rx_bus->rx_bytes += read_len;
    if (verify_frame(&rx_frame, direction, *sequence)) {
        ++rx_bus->good_frames;
    } else {
        ++rx_bus->bad_frames;
    }

    ++(*sequence);
}

static void report_speed(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (now_us - s_last_report_us < REPORT_INTERVAL_MS * 1000LL) {
        return;
    }

    const uint32_t elapsed_ms = (uint32_t)((now_us - s_last_report_us) / 1000);
    s_last_report_us = now_us;

    const uint32_t tx12_rate = (s_rs4851.tx_bytes * 1000UL) / elapsed_ms;
    const uint32_t rx12_rate = (s_rs4852.rx_bytes * 1000UL) / elapsed_ms;
    const uint32_t tx21_rate = (s_rs4852.tx_bytes * 1000UL) / elapsed_ms;
    const uint32_t rx21_rate = (s_rs4851.rx_bytes * 1000UL) / elapsed_ms;

    ESP_LOGI(TAG, "[idf RS4851->RS4852] TX=%" PRIu32 " B/s RX=%" PRIu32
             " B/s good=%" PRIu32 " bad=%" PRIu32 " timeout=%" PRIu32,
             tx12_rate, rx12_rate, s_rs4852.good_frames, s_rs4852.bad_frames, s_rs4852.timeouts);
    ESP_LOGI(TAG, "[idf RS4852->RS4851] TX=%" PRIu32 " B/s RX=%" PRIu32
             " B/s good=%" PRIu32 " bad=%" PRIu32 " timeout=%" PRIu32,
             tx21_rate, rx21_rate, s_rs4851.good_frames, s_rs4851.bad_frames, s_rs4851.timeouts);

    reset_counters();
}

void app_main(void)
{
    ESP_LOGI(TAG, "T-C5-Base pure IDF dual RS485 speed test");
    ESP_LOGI(TAG, "frame=%u bytes payload=%u bytes baud=%d", (unsigned)sizeof(test_frame_t),
             (unsigned)TEST_PAYLOAD_SIZE, RS485_BAUD);

    ESP_ERROR_CHECK(start_bus(&s_rs4851));
    ESP_ERROR_CHECK(start_bus(&s_rs4852));

    s_last_report_us = esp_timer_get_time();

    while (true) {
        transfer_frame(&s_rs4851, &s_rs4852, DIR_1_TO_2, &s_sequence12);
        transfer_frame(&s_rs4852, &s_rs4851, DIR_2_TO_1, &s_sequence21);
        report_speed();
        if (LOOP_YIELD_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(LOOP_YIELD_MS));
        }
    }
}
