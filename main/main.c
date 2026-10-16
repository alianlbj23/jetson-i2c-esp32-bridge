/*
 * Jetson <-> ESP32-S3 I2C link test.
 *
 * The ESP32 acts as an I2C *slave* that exposes a small register map, so the
 * Jetson (I2C master) can verify its bus with nothing but i2c-tools:
 *
 *   i2cdetect -y -r <bus>              -> ESP32 shows up at 0x28
 *   i2cget    -y <bus> 0x28 0x00       -> 0xa5 (WHO_AM_I)
 *   i2cset    -y <bus> 0x28 0x08 0x5a  -> write scratch register
 *   i2cget    -y <bus> 0x28 0x08       -> 0x5a (read back)
 *   i2ctransfer -y <bus> w1@0x28 0x10 r16 -> "ESP32-S3 I2C OK"
 *
 * Protocol (register style, like most I2C sensors):
 *   Master write : [reg_addr] [data0] [data1] ...   (data optional)
 *   Master read  : [reg_addr] -> repeated start -> read N bytes
 *                  Bytes are served starting at reg_addr, auto-incrementing
 *                  and wrapping at REG_MAP_SIZE.
 *
 * Timing note (why the register logic lives in the ISR callbacks):
 *   When the master addresses us for a read, the ESP32-S3 stretches SCL and
 *   the driver fires on_request. The hardware only holds the stretch for
 *   stretch_protect_num = 1023 I2C clock cycles (~25 us at 40 MHz) and then
 *   releases SCL on its own. Deferring the FIFO fill to a task (plus UART
 *   logging) blows that window, the master clocks out an empty FIFO and reads
 *   0xFF. So the pointer update and the FIFO fill happen directly inside the
 *   ISR callbacks via the LL layer; the worker task only prints logs.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_slave.h"
#include "driver/gpio.h"
#include "hal/i2c_ll.h"
#include "sdkconfig.h"

static const char *TAG = "jetson_i2c";

#define APP_I2C_PORT        0
#define APP_SDA_GPIO    CONFIG_JETSON_I2C_SDA_GPIO
#define APP_SCL_GPIO    CONFIG_JETSON_I2C_SCL_GPIO
#define APP_SLAVE_ADDR  CONFIG_JETSON_I2C_SLAVE_ADDR

/* ---- Register map -------------------------------------------------------- */
#define REG_WHO_AM_I    0x00    /* RO, always 0xA5 */
#define REG_FW_VERSION  0x01    /* RO */
#define REG_RX_COUNT    0x02    /* RO, number of master->slave transactions (u8, wraps) */
#define REG_TX_COUNT    0x03    /* RO, number of slave->master transactions (u8, wraps) */
#define REG_UPTIME_MS   0x04    /* RO, u32 little-endian, 0x04..0x07 */
#define REG_SCRATCH     0x08    /* RW, 8 bytes, 0x08..0x0F */
#define REG_SCRATCH_LEN 8
#define REG_MESSAGE     0x10    /* RO, 16 bytes ASCII, 0x10..0x1F */
#define REG_MESSAGE_LEN 16
#define REG_MAP_SIZE    0x20

#define WHO_AM_I_VALUE  0xA5
#define FW_VERSION      0x02
#define MESSAGE_TEXT    "ESP32-S3 I2C OK"   /* 15 chars + NUL = 16 bytes */

/* Bytes pushed to the TX FIFO per master read request. The S3 FIFO holds 32
 * bytes, so a single fill covers any read of up to 32 bytes. */
#define TX_FILL_LEN     32

/* ---- Event plumbing: ISR callbacks -> logging task ----------------------- */
typedef enum {
    EVT_RX,     /* master wrote data to us (already applied in ISR) */
    EVT_TX,     /* master read from us (FIFO already filled in ISR) */
} evt_type_t;

typedef struct {
    evt_type_t type;
    uint8_t len;                /* EVT_RX: bytes received */
    uint8_t reg_ptr;            /* register pointer in effect for this event */
    uint8_t first_byte;         /* EVT_TX: first byte served */
    uint8_t data[TX_FILL_LEN];  /* EVT_RX: raw bytes received */
} i2c_evt_t;

typedef struct {
    i2c_slave_dev_handle_t handle;
    i2c_dev_t *hw;
    QueueHandle_t queue;
    uint8_t regs[REG_MAP_SIZE];
    uint8_t reg_ptr;
    uint8_t rx_count;
    uint8_t tx_count;
} ctx_t;

static ctx_t s_ctx;

/* ---- Register helpers (ISR-safe: no logging, no blocking) ---------------- */
static inline bool reg_is_writable(uint8_t reg)
{
    return reg >= REG_SCRATCH && reg < REG_SCRATCH + REG_SCRATCH_LEN;
}

static IRAM_ATTR void refresh_dynamic_regs(ctx_t *ctx)
{
    uint32_t uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    ctx->regs[REG_RX_COUNT] = ctx->rx_count;
    ctx->regs[REG_TX_COUNT] = ctx->tx_count;
    ctx->regs[REG_UPTIME_MS + 0] = (uint8_t)(uptime_ms >> 0);
    ctx->regs[REG_UPTIME_MS + 1] = (uint8_t)(uptime_ms >> 8);
    ctx->regs[REG_UPTIME_MS + 2] = (uint8_t)(uptime_ms >> 16);
    ctx->regs[REG_UPTIME_MS + 3] = (uint8_t)(uptime_ms >> 24);
}

/* ---- ISR callbacks (must live in IRAM, keep them tiny) ------------------- */

/* Master wrote [reg_addr] [data...]. Apply it right here so that a read in
 * the same transaction (repeated start) already sees the new pointer. */
static IRAM_ATTR bool on_receive(i2c_slave_dev_handle_t dev,
                                 const i2c_slave_rx_done_event_data_t *evt,
                                 void *arg)
{
    ctx_t *ctx = (ctx_t *)arg;
    i2c_evt_t e = { .type = EVT_RX };
    e.len = evt->length > sizeof(e.data) ? sizeof(e.data) : evt->length;
    memcpy(e.data, evt->buffer, e.len);

    if (e.len > 0) {
        ctx->rx_count++;
        ctx->reg_ptr = e.data[0] % REG_MAP_SIZE;
        uint8_t reg = ctx->reg_ptr;
        for (uint8_t i = 1; i < e.len; i++) {
            if (reg_is_writable(reg)) {
                ctx->regs[reg] = e.data[i];
            }
            reg = (reg + 1) % REG_MAP_SIZE;
        }
        /* Leave the pointer where the master set it so a following read starts there. */
    }
    e.reg_ptr = ctx->reg_ptr;

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(ctx->queue, &e, &woken);
    return woken == pdTRUE;
}

/* Master wants to read. SCL is being stretched by hardware right now, but only
 * for ~25 us, so fill the FIFO immediately and release the stretch. */
static IRAM_ATTR bool on_request(i2c_slave_dev_handle_t dev,
                                 const i2c_slave_request_event_data_t *evt,
                                 void *arg)
{
    ctx_t *ctx = (ctx_t *)arg;
    ctx->tx_count++;
    refresh_dynamic_regs(ctx);

    uint8_t out[TX_FILL_LEN];
    for (int i = 0; i < TX_FILL_LEN; i++) {
        out[i] = ctx->regs[(ctx->reg_ptr + i) % REG_MAP_SIZE];
    }

    /* Drop anything left over from a previous (shorter) read so the master
     * never sees stale bytes, then load fresh data and let SCL go. */
    i2c_ll_txfifo_rst(ctx->hw);
    i2c_ll_write_txfifo(ctx->hw, out, TX_FILL_LEN);
    i2c_ll_slave_clear_stretch(ctx->hw);

    i2c_evt_t e = { .type = EVT_TX, .reg_ptr = ctx->reg_ptr, .first_byte = out[0] };
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(ctx->queue, &e, &woken);
    return woken == pdTRUE;
}

/* ---- Logging task (everything slow happens here, after the fact) --------- */
static void log_rx(const i2c_evt_t *e)
{
    if (e->len == 0) {
        return;
    }
    if (e->len == 1) {
        ESP_LOGI(TAG, "RX: set reg pointer -> 0x%02X", e->reg_ptr);
        return;
    }
    ESP_LOGI(TAG, "RX: write %u byte(s) starting at reg 0x%02X", e->len - 1, e->reg_ptr);
    uint8_t reg = e->reg_ptr;
    for (uint8_t i = 1; i < e->len; i++) {
        if (reg_is_writable(reg)) {
            ESP_LOGI(TAG, "    reg[0x%02X] = 0x%02X", reg, e->data[i]);
        } else {
            ESP_LOGW(TAG, "    reg[0x%02X] is read-only, ignored 0x%02X", reg, e->data[i]);
        }
        reg = (reg + 1) % REG_MAP_SIZE;
    }
}

static void i2c_log_task(void *arg)
{
    ctx_t *ctx = (ctx_t *)arg;
    i2c_evt_t e;
    while (true) {
        if (xQueueReceive(ctx->queue, &e, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (e.type) {
        case EVT_RX:
            log_rx(&e);
            break;
        case EVT_TX:
            ESP_LOGI(TAG, "TX: served read from reg 0x%02X (first byte 0x%02X, %d bytes in FIFO)",
                     e.reg_ptr, e.first_byte, TX_FILL_LEN);
            break;
        }
    }
}

static void heartbeat_task(void *arg)
{
    ctx_t *ctx = (ctx_t *)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "alive | addr 0x%02X | rx=%u tx=%u | scratch[0]=0x%02X | SDA=%d SCL=%d",
                 APP_SLAVE_ADDR, ctx->rx_count, ctx->tx_count, ctx->regs[REG_SCRATCH],
                 gpio_get_level(APP_SDA_GPIO), gpio_get_level(APP_SCL_GPIO));
    }
}

void app_main(void)
{
    ctx_t *ctx = &s_ctx;
    memset(ctx, 0, sizeof(*ctx));

    ctx->regs[REG_WHO_AM_I] = WHO_AM_I_VALUE;
    ctx->regs[REG_FW_VERSION] = FW_VERSION;
    memcpy(&ctx->regs[REG_MESSAGE], MESSAGE_TEXT, sizeof(MESSAGE_TEXT));
    refresh_dynamic_regs(ctx);

    ctx->queue = xQueueCreate(16, sizeof(i2c_evt_t));
    if (ctx->queue == NULL) {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }

    i2c_slave_config_t cfg = {
        .i2c_port = APP_I2C_PORT,
        .sda_io_num = APP_SDA_GPIO,
        .scl_io_num = APP_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .send_buf_depth = 256,
        .receive_buf_depth = 256,
        .slave_addr = APP_SLAVE_ADDR,
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
        .intr_priority = 0,
        /* Jetson 40-pin header pins 3/5 already have on-board pull-ups;
         * keep the (weak) internal ones on anyway so the bus is never floating. */
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_slave_device(&cfg, &ctx->handle));
    ctx->hw = I2C_LL_GET_HW(APP_I2C_PORT);

    i2c_slave_event_callbacks_t cbs = {
        .on_receive = on_receive,
        .on_request = on_request,
    };
    ESP_ERROR_CHECK(i2c_slave_register_event_callbacks(ctx->handle, &cbs, ctx));

    xTaskCreate(i2c_log_task, "i2c_log", 4096, ctx, 10, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 3072, ctx, 2, NULL);

    ESP_LOGI(TAG, "I2C slave ready: addr=0x%02X SDA=GPIO%d SCL=GPIO%d",
             APP_SLAVE_ADDR, APP_SDA_GPIO, APP_SCL_GPIO);
    ESP_LOGI(TAG, "On the Jetson run:  sudo i2cdetect -y -r <bus>   (expect 0x%02X)", APP_SLAVE_ADDR);
}
