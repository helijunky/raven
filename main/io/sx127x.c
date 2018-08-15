#include <math.h>
#include <stdint.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_heap_caps.h>
#include <esp_system.h>

#include <driver/spi_master.h>

#include <hal/gpio.h>
#include <hal/log.h>

#include "util/fec.h"
#include "util/macros.h"
#include "util/time.h"

#include "sx127x.h"

// constants
#define SX127X_FXOSC 32000000            // 32Mhz
#define SX127X_FSK_FREQ_STEP 61.03515625 // 61khz

// Common registers
#define REG_FIFO 0x00
#define REG_OP_MODE 0x01
#define REG_FRF_MSB 0x06
#define REG_FRF_MID 0x07
#define REG_FRF_LSB 0x08
#define REG_PA_CONFIG 0x09
#define REG_PA_RAMP 0x0A
#define REG_LNA 0x0c
#define REG_DIO_MAPPING_1 0x40
#define REG_DIO_MAPPING_2 0x41
#define REG_VERSION 0x42
#define REG_PA_DAC 0x4d

// FSK Registers
#define REG_FSK_BITRATE_MSB 0x02
#define REG_FSK_BITRATE_LSB 0x03
#define REG_FSK_FDEV_MSB 0x04
#define REG_FSK_FDEV_LSB 0x05
#define REG_FSK_RX_CONFIG 0x0d
#define REG_FSK_RSSI_THRES 0x10
#define REG_FSK_RSSI_VALUE 0x11
#define REG_FSK_RX_BW 0x12
#define REG_FSK_RX_AFC_BW 0x13
#define REG_FSK_FEI_MSB 0x1d
#define REG_FSK_FEI_LSB 0x1e
#define REG_FSK_PREAMBLE_DETECT 0x1f
#define REG_FSK_PREAMBLE_MSB 0x25
#define REG_FSK_PREAMBLE_LSB 0x26
#define REG_FSK_SYNC_CONFIG 0x27
#define REG_FSK_SYNC_VALUE_1 0x28
#define REG_FSK_SYNC_VALUE_2 0x29
#define REG_FSK_SYNC_VALUE_3 0x2a
#define REG_FSK_SYNC_VALUE_4 0x2b
#define REG_FSK_PACKET_CONFIG_1 0x30
#define REG_FSK_PACKET_CONFIG_2 0x31
#define REG_FSK_PAYLOAD_LENGTH 0x32
#define REG_FSK_FIFO_THRESH 0x35
#define REG_FSK_IRQ_FLAGS_1 0x3e
#define REG_FSK_IRQ_FLAGS_2 0x3f

// LoRa registers
#define REG_LORA_FIFO_ADDR_PTR 0x0d
#define REG_LORA_FIFO_TX_BASE_ADDR 0x0e
#define REG_LORA_FIFO_RX_BASE_ADDR 0x0f
#define REG_LORA_FIFO_RX_CURRENT_ADDR 0x10
#define REG_LORA_IRQ_FLAGS 0x12
#define REG_LORA_RX_NB_BYTES 0x13
#define REG_LORA_PKT_SNR_VALUE 0x19
#define REG_LORA_PKT_RSSI_VALUE 0x1a
#define REG_LORA_MODEM_CONFIG_1 0x1d
#define REG_LORA_MODEM_CONFIG_2 0x1e
#define REG_LORA_PREAMBLE_MSB 0x20
#define REG_LORA_PREAMBLE_LSB 0x21
#define REG_LORA_PAYLOAD_LENGTH 0x22
#define REG_LORA_MODEM_CONFIG_3 0x26
#define REG_LORA_PPM_CORRECTION 0x27
#define REG_LORA_FEI_MSB 0x28
#define REG_LORA_FEI_MID 0x29
#define REG_LORA_FEI_LSB 0x2A
#define REG_LORA_RSSI_WIDEBAND 0x2c
#define REG_LORA_DETECTION_OPTIMIZE 0x31
#define REG_LORA_DETECTION_BW500_OPTIMIZE_1 0x36
#define REG_LORA_DETECTION_THRESHOLD 0x37
#define REG_LORA_SYNC_WORD 0x39
#define REG_LORA_DETECTION_BW500_OPTIMIZE_2 0x3A

// modes
#define MODE_LORA 0x80
#define MODE_SLEEP 0x00
#define MODE_STDBY 0x01
#define MODE_TX 0x03
#define MODE_RX_CONTINUOUS 0x05
// #define MODE_RX_SINGLE 0x06 // Unused, only valid in LoRa mode

// PA config
#define PA_BOOST 0x80

// IRQ masks
#define IRQ_FSK_MODE_READY (1 << 7)    // in REG_FSK_IRQ_FLAGS_1
#define IRQ_FSK_RX_READY (1 << 6)      // in REG_FSK_IRQ_FLAGS_1
#define IRQ_FSK_TX_READY (1 << 5)      // in REG_FSK_IRQ_FLAGS_1
#define IRQ_FSK_PACKET_SENT (1 << 3)   // in REG_FSK_IRQ_FLAGS_2
#define IRQ_FSK_PAYLOAD_READY (1 << 2) // in REG_FSK_IRQ_FLAGS_2

#define IRQ_TX_DONE_MASK 0x08
#define IRQ_PAYLOAD_CRC_ERROR_MASK 0x20
#define IRQ_RX_DONE_MASK 0x40

// Page 46, table 18 indicates the DIO0 values,
// page 92 indicates that DIO0 is in the most
// significant 2 bits
#define DIO0_BIT_OFFSET 6
#define DIO0_LORA_RX_DONE (0 << DIO0_BIT_OFFSET)
#define DIO0_LORA_TX_DONE (1 << DIO0_BIT_OFFSET)
#define DIO0_LORA_NONE (3 << DIO0_BIT_OFFSET)

// Page 69, Table 30 (packet mode, we don't use continous mode)
#define DIO0_FSK_PAYLOAD_READY (0 << DIO0_BIT_OFFSET)
#define DIO0_FSK_PACKET_SENT DIO0_FSK_PAYLOAD_READY
#define DIO0_FSK_NONE (2 << DIO0_BIT_OFFSET)

#define TX_FIFO_ADDR 0x80
#define RX_FIFO_ADDR 0

#define SX127X_EXPECTED_VERSION 18

enum
{
    DIO0_TRIGGER_RX_DONE = 1,
    DIO0_TRIGGER_TX_DONE,
};

enum
{
    SX127X_RX_SENSITIVITY_BW500_WORKAROUND_NONE = 0,
    SX127X_RX_SENSITIVITY_BW500_WORKAROUND_HIGH_BAND = 1,
    SX127X_RX_SENSITIVITY_BW500_WORKAROUND_LOW_BAND = 2,
};

static const char *TAG = "SX127X";

static float sx127x_get_lora_signal_bw_khz(sx127x_t *sx127x, sx127x_lora_signal_bw_e sbw);
static void sx127x_apply_bw500_sensitivity_workaround(sx127x_t *sx127x);

static esp_err_t spi_device_transmit_sync(spi_device_handle_t handle, spi_transaction_t *trans_desc)
{
    // Just a wrapper for now
    return spi_device_transmit(handle, trans_desc);
}

static uint8_t sx127x_read_reg(sx127x_t *sx127x, uint8_t addr)
{
    spi_transaction_t t;
    t.cmd = 0;
    t.addr = addr;
    t.length = 8; // Send 8 arbitrary bits to get one byte back in full duplex
    t.rxlength = 0;
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    ESP_ERROR_CHECK(spi_device_transmit_sync(sx127x->state.spi, &t));
    return t.rx_data[0];
}

static inline void sx127x_write_prepare_spi_transaction(spi_transaction_t *t, uint8_t addr, uint8_t value)
{
    t->cmd = 1;
    t->addr = addr;
    t->length = 8; // Value byte is 8 bits
    t->rxlength = 0;
    t->flags = SPI_TRANS_USE_TXDATA;
    t->tx_data[0] = value;
    t->rx_buffer = NULL;
}

static void sx127x_write_reg(sx127x_t *sx127x, uint8_t addr, uint8_t value)
{
    spi_transaction_t t;
    sx127x_write_prepare_spi_transaction(&t, addr, value);
    ESP_ERROR_CHECK(spi_device_transmit_sync(sx127x->state.spi, &t));
}

static void sx127x_set_mode(sx127x_t *sx127x, uint8_t mode)
{
    if (sx127x->state.mode != mode)
    {
        sx127x_write_reg(sx127x, REG_OP_MODE, mode);
        sx127x->state.mode = mode;
    }
}

static void sx127x_prepare_write(sx127x_t *sx127x)
{
    switch (sx127x->state.op_mode)
    {
    case SX127X_OP_MODE_FSK:
        sx127x_sleep(sx127x);
        break;
    case SX127X_OP_MODE_LORA:
    {
        // Registers can only be written to when the device is
        // in sleep or idle mode. If it's not, we set it to idle
        uint8_t mode = sx127x->state.mode & ~MODE_LORA;
        if (mode != MODE_SLEEP && mode != MODE_STDBY)
        {
            sx127x_idle(sx127x);
        }
        break;
    }
    }
}

static int link_quality(int min_dbm, int max_dbm, int dbm)
{
    return (100 * (max_dbm - min_dbm) * (max_dbm - min_dbm) - (max_dbm - dbm) * (25 * (max_dbm - min_dbm) + 75 * (max_dbm - dbm))) / ((max_dbm - min_dbm) * (max_dbm - min_dbm));
}

#if 0
static void sx127x_print_fsk_irq(sx127x_t *sx127x)
{
    uint8_t val1 = sx127x_read_reg(sx127x, REG_FSK_IRQ_FLAGS_1);
    uint8_t val2 = sx127x_read_reg(sx127x, REG_FSK_IRQ_FLAGS_2);
    printf("IRQ 0x%02x 0x%02x\n", val1, val2);
}
#endif

static TaskHandle_t callback_task_handle = NULL;

static void sx127x_callback_task(void *arg)
{
    sx127x_t *sx127x = arg;
    air_radio_callback_t callback;
    for (;;)
    {
        ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
        switch (sx127x->state.dio0_trigger)
        {
        case DIO0_TRIGGER_RX_DONE:
            sx127x->state.rx_done = true;
            if (sx127x->state.callback)
            {
                callback = sx127x->state.callback;
                callback((air_radio_t *)sx127x, AIR_RADIO_CALLBACK_REASON_RX_DONE, sx127x->state.callback_data);
            }
            break;
        case DIO0_TRIGGER_TX_DONE:
            sx127x->state.tx_done = true;
            if (sx127x->state.callback)
            {
                callback = sx127x->state.callback;
                callback((air_radio_t *)sx127x, AIR_RADIO_CALLBACK_REASON_TX_DONE, sx127x->state.callback_data);
            }
            break;
        }
    }
}

static void IRAM_ATTR lora_handle_isr(void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(callback_task_handle, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken)
    {
        portYIELD_FROM_ISR();
    }
}

static void sx127x_disable_dio0(sx127x_t *sx127x)
{
    sx127x->state.dio0_trigger = 0;
    uint8_t reg = 0;
    switch (sx127x->state.op_mode)
    {
    case SX127X_OP_MODE_FSK:
        reg = DIO0_FSK_NONE;
        break;
    case SX127X_OP_MODE_LORA:
        reg = DIO0_LORA_NONE;
        break;
    }
    sx127x_write_reg(sx127x, REG_DIO_MAPPING_1, reg);
}

static void sx127x_set_fsk_parameters(sx127x_t *sx127x)
{
    // TODO: Does this enhance transmission? It turns GFSK mode
    // sx127x_write_reg(sx127x, REG_PA_RAMP, 0x20);

    //sx127x_write_reg(sx127x, REG_FSK_RX_CONFIG, 0x10 | 0x08 | 0x06);
    sx127x_write_reg(sx127x, REG_FSK_RX_CONFIG, /*1 << 4 |*/ 0x08 | 0x06);
    // detector on | detector size 01 | tolerance 10
    sx127x_write_reg(sx127x, REG_FSK_PREAMBLE_DETECT, 1 << 7 | 1 << 5 | 10);

    sx127x_write_reg(sx127x, REG_FSK_RSSI_THRES, 0xFF);

    // autorestart = on, wait for pll, polarity = AA, sync = ON, syncsize = 3 + 1 = 4
    sx127x_write_reg(sx127x, REG_FSK_SYNC_CONFIG, 2 << 5 | 0 << 5 | 1 << 4 | 0x03);

    sx127x_write_reg(sx127x, REG_FSK_SYNC_VALUE_1, 0x69);
    sx127x_write_reg(sx127x, REG_FSK_SYNC_VALUE_2, 0x81);
    sx127x_write_reg(sx127x, REG_FSK_SYNC_VALUE_3, 0x7E);
    sx127x_write_reg(sx127x, REG_FSK_SYNC_VALUE_4, 0x96);

    sx127x_write_reg(sx127x, REG_FSK_PACKET_CONFIG_1, 1 << 5);
}

static void sx127x_fsk_wait_for_mode_ready(sx127x_t *sx127x)
{
    while (sx127x_read_reg(sx127x, REG_FSK_IRQ_FLAGS_1) & ~IRQ_FSK_MODE_READY)
    {
    }
}

void sx127x_init(sx127x_t *sx127x)
{
    hal_gpio_enable(sx127x->rst);
    hal_gpio_set_dir(sx127x->rst, HAL_GPIO_DIR_OUTPUT);
    hal_gpio_set_level(sx127x->rst, HAL_GPIO_LOW);
    vTaskDelay(20 / portTICK_PERIOD_MS);
    hal_gpio_set_level(sx127x->rst, HAL_GPIO_HIGH);
    vTaskDelay(50 / portTICK_PERIOD_MS);

    spi_bus_config_t buscfg = {
        .miso_io_num = sx127x->miso,
        .mosi_io_num = sx127x->mosi,
        .sclk_io_num = sx127x->sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 0,
    };
    spi_device_interface_config_t devcfg;
    memset(&devcfg, 0, sizeof(devcfg));
    devcfg.command_bits = 1;                 // 1 command bit, 1 => write, 0 => read
    devcfg.address_bits = 7;                 // 7 addr bits
    devcfg.clock_speed_hz = 9 * 1000 * 1000; // Clock out at 9 MHz: XXX => 10Mhz will cause incorrect reads from REG_MODEM_CONFIG_1
    devcfg.mode = 0;                         // SPI mode 0
    devcfg.spics_io_num = sx127x->cs;        // CS pin
    devcfg.queue_size = 4;
    //Initialize the SPI bus
    ESP_ERROR_CHECK(spi_bus_initialize(VSPI_HOST, &buscfg, 1));
    // Attach the device
    ESP_ERROR_CHECK(spi_bus_add_device(VSPI_HOST, &devcfg, &sx127x->state.spi));

    sx127x->state.tx_done = false;
    sx127x->state.rx_done = false;
    sx127x->state.fsk.freq = 0;
    sx127x->state.lora.freq = 0;
    sx127x->state.lora.ppm_correction = 0;

    xTaskCreatePinnedToCore(sx127x_callback_task, "SX127X-CALLBACK", 4096, sx127x, 1000, &callback_task_handle, 1);

    uint8_t version = sx127x_read_reg(sx127x, REG_VERSION);
    if (version == SX127X_EXPECTED_VERSION)
    {
        LOG_I(TAG, "Got SX127X chip version %u", version);
    }
    else
    {
        LOG_E(TAG, "Unexpected SX127X chip version %u, expecting %d", version, SX127X_EXPECTED_VERSION);
        UNREACHABLE();
    }

    sx127x->state.mode = sx127x_read_reg(sx127x, REG_OP_MODE);
    if (sx127x->state.mode & MODE_LORA)
    {
        sx127x->state.op_mode = SX127X_OP_MODE_LORA;
    }
    else
    {
        sx127x->state.op_mode = SX127X_OP_MODE_FSK;
    }
    sx127x->state.fsk.payload_length = 0;
    sx127x->state.lora.payload_length = 0;
    sx127x->state.callback = NULL;

    // Put it in sleep mode to change some registers
    sx127x_sleep(sx127x);

    sx127x_write_reg(sx127x, REG_LORA_FIFO_TX_BASE_ADDR, TX_FIFO_ADDR);
    sx127x_write_reg(sx127x, REG_LORA_FIFO_RX_BASE_ADDR, RX_FIFO_ADDR);

    // LNA boost HF
    // TODO: Should we adjust LnaGain here?
    sx127x_write_reg(sx127x, REG_LNA, sx127x_read_reg(sx127x, REG_LNA) | 0x03);

    // set auto AGC
    sx127x_write_reg(sx127x, REG_LORA_MODEM_CONFIG_3, 0x04);

#if defined(CONFIG_RAVEN_DIO5_CLK_OUTPUT)
    // Enable DIO5 as ClkOut
    uint8_t dio_mapping_2 = sx127x_read_reg(sx127x, REG_DIO_MAPPING_2);
    sx127x_write_reg(sx127x, REG_DIO_MAPPING_2, dio_mapping_2 | (1 << 5));
#endif

    // set output power to 17 dBm
    sx127x_set_tx_power(sx127x, 17);

    // put in standby mode
    sx127x_idle(sx127x);

    // configure pin for ISR
    hal_gpio_enable(sx127x->dio0);
    hal_gpio_set_dir(sx127x->dio0, HAL_GPIO_DIR_INPUT);
    hal_gpio_set_pull(sx127x->dio0, HAL_GPIO_PULL_NONE);
    hal_gpio_set_isr(sx127x->dio0, HAL_GPIO_INTR_POSEDGE, lora_handle_isr, sx127x);

    sx127x_disable_dio0(sx127x);
}

// freq is in Hz
void sx127x_set_frequency(sx127x_t *sx127x, unsigned long freq, int error)
{
    freq -= error;

    uint64_t frf = 0;
    switch (sx127x->state.op_mode)
    {
    case SX127X_OP_MODE_FSK:
        if (freq != sx127x->state.fsk.freq)
        {
            sx127x->state.fsk.freq = freq;
            frf = (uint32_t)(freq / SX127X_FSK_FREQ_STEP);
        }
        break;
    case SX127X_OP_MODE_LORA:
        if (freq != sx127x->state.lora.freq)
        {
            sx127x->state.lora.freq = freq;
            frf = ((uint64_t)freq << 19) / SX127X_FXOSC;
        }
        break;
    }

    if (frf > 0)
    {
        sx127x_prepare_write(sx127x);
        sx127x_write_reg(sx127x, REG_FRF_MSB, (uint8_t)(frf >> 16));
        sx127x_write_reg(sx127x, REG_FRF_MID, (uint8_t)(frf >> 8));
        sx127x_write_reg(sx127x, REG_FRF_LSB, (uint8_t)(frf >> 0));
        // Wait up to 50us for PLL lock (page 15, table 7)
        time_micros_t now = time_micros_now();
        do
        {
        } while (time_micros_now() < now + 50);
    }

    if (sx127x->state.op_mode == SX127X_OP_MODE_LORA)
    {
        // TODO: Should ppm_correction be applied in FSK mode?
        int8_t ppm_correction = MIN(MAX(lrintf(0.95f * (error / ((float)freq / 1000000))), -128), 127);
        if (ppm_correction != sx127x->state.lora.ppm_correction)
        {
            sx127x_prepare_write(sx127x);
            sx127x_write_reg(sx127x, REG_LORA_PPM_CORRECTION, (uint8_t)ppm_correction);
            sx127x->state.lora.ppm_correction = ppm_correction;
        }
        sx127x_apply_bw500_sensitivity_workaround(sx127x);
    }
}

void sx127x_set_payload_size(sx127x_t *sx127x, uint8_t size)
{
    switch (sx127x->state.op_mode)
    {
    case SX127X_OP_MODE_FSK:
        size = FEC_ENCODED_SIZE(size);
        if (sx127x->state.fsk.payload_length != size)
        {
            sx127x_prepare_write(sx127x);
            sx127x_write_reg(sx127x, REG_FSK_PAYLOAD_LENGTH, size);
            sx127x->state.fsk.payload_length = size;
        }
        break;
    case SX127X_OP_MODE_LORA:
        if (sx127x->state.lora.payload_length != size)
        {
            sx127x_prepare_write(sx127x);
            sx127x_write_reg(sx127x, REG_LORA_PAYLOAD_LENGTH, size);
            sx127x->state.lora.payload_length = size;
        }
        break;
    }
}

void sx127x_sleep(sx127x_t *sx127x)
{
    uint8_t mode = (sx127x->state.mode & MODE_LORA) | MODE_SLEEP;
    sx127x_set_mode(sx127x, mode);
}

void sx127x_idle(sx127x_t *sx127x)
{
    uint8_t mode = (sx127x->state.mode & MODE_LORA) | MODE_STDBY;
    sx127x_set_mode(sx127x, mode);
}

void sx127x_set_op_mode(sx127x_t *sx127x, sx127x_op_mode_e op_mode)
{
    if (sx127x->state.op_mode != op_mode)
    {
        sx127x_set_mode(sx127x, (sx127x->state.mode & MODE_LORA) | MODE_SLEEP);
        switch (op_mode)
        {
        case SX127X_OP_MODE_FSK:
            sx127x_set_mode(sx127x, MODE_SLEEP);
            sx127x_set_fsk_parameters(sx127x);
            break;
        case SX127X_OP_MODE_LORA:
            sx127x_set_mode(sx127x, MODE_LORA | MODE_SLEEP);
            break;
        }
        sx127x->state.op_mode = op_mode;
    }
}

void sx127x_set_tx_power(sx127x_t *sx127x, int level)
{
    sx127x_prepare_write(sx127x);

    uint8_t pa_config = 0;
    uint8_t pa_dac = 0x84; // default for +17dbm
    switch (sx127x->output_type)
    {
    case SX127X_OUTPUT_RFO:
        if (level < 0)
        {
            level = 0;
        }
        else if (level > 14)
        {
            level = 14;
        }
        pa_config = 0x70 | level;
        break;
    case SX127X_OUTPUT_PA_BOOST:
        if (level < 2)
        {
            level = 2;
        }
        else if (level > 17)
        {
            level = 17;
            pa_dac = 0x87; // Enable +20dbm as Pmax with PA_BOOST
        }
        pa_config = PA_BOOST | (level - 2);
        break;
    default:
        UNREACHABLE();
    }
    sx127x_write_reg(sx127x, REG_PA_CONFIG, pa_config);
    sx127x_write_reg(sx127x, REG_PA_DAC, pa_dac);
}

void sx127x_send(sx127x_t *sx127x, const void *buf, size_t size)
{
    uint8_t data[FEC_ENCODED_SIZE(size)];
    const void *ptr = NULL;
    size_t ptr_size = 0;
    switch (sx127x->state.op_mode)
    {
    case SX127X_OP_MODE_FSK:
        sx127x_sleep(sx127x);
        ptr_size = fec_encode(buf, size, data, sizeof(data));
        ptr = data;
        // We need to wait here, otherwise we might write to
        // the FIFO before the modem is in sleep mode and the
        // write will be ignored. See 4.2.10 FIFO (page 66)
        sx127x_fsk_wait_for_mode_ready(sx127x);
        break;
    case SX127X_OP_MODE_LORA:
        ptr = buf;
        ptr_size = size;
        // We need to be in IDLE. SLEEP won't work because
        // the FIFO is not available in LoRa SLEEP mode.
        sx127x_idle(sx127x);

        // reset FIFO address
        sx127x_write_reg(sx127x, REG_LORA_FIFO_ADDR_PTR, TX_FIFO_ADDR);
        break;
    }

    // Write payload
    spi_transaction_t t;
    t.cmd = 1;
    t.addr = REG_FIFO;
    t.length = ptr_size * 8;
    t.rxlength = 0;
    t.rx_buffer = NULL;
    t.tx_buffer = ptr;
    t.flags = 0;
    ESP_ERROR_CHECK(spi_device_transmit_sync(sx127x->state.spi, &t));

    // Update length
    sx127x_set_payload_size(sx127x, size);

    sx127x->state.tx_done = false;
    sx127x->state.dio0_trigger = DIO0_TRIGGER_TX_DONE;

    switch (sx127x->state.op_mode)
    {
    case SX127X_OP_MODE_FSK:
        // No need to clear the IRQ in FSK mode, it's automatically cleared
        // when exiting TX mode.
        sx127x_write_reg(sx127x, REG_DIO_MAPPING_1, DIO0_FSK_PACKET_SENT);
        // Start transmitting as soon as the FIFO is not empty. We actually make
        // it wait until we fill the FIFO because up until this point, the radio
        // has been in standby mode.
        // XXX: This needs to be reset before each send, otherwise we don't get
        // the sent callback.
        sx127x_write_reg(sx127x, REG_FSK_FIFO_THRESH, 1 << 7);
        // Enable TX mode, wait for TxReady
        sx127x_set_mode(sx127x, MODE_TX);
        break;
    case SX127X_OP_MODE_LORA:
        sx127x_write_reg(sx127x, REG_LORA_IRQ_FLAGS, IRQ_TX_DONE_MASK);
        sx127x_write_reg(sx127x, REG_DIO_MAPPING_1, DIO0_LORA_TX_DONE);
        sx127x_set_mode(sx127x, MODE_LORA | MODE_TX);
        break;
    }
}

size_t sx127x_read(sx127x_t *sx127x, void *buf, size_t size)
{
    uint8_t data[FEC_ENCODED_SIZE(size)];
    void *ptr;
    size_t ptr_size;
    if (sx127x->state.op_mode == SX127X_OP_MODE_LORA)
    {
        ptr = buf;
        ptr_size = size;

        sx127x_prepare_write(sx127x);
        sx127x_write_reg(sx127x, REG_LORA_FIFO_ADDR_PTR, RX_FIFO_ADDR);
    }
    else
    {
        ptr = data;
        ptr_size = FEC_ENCODED_SIZE(size);
    }
    // For these small transfers, allocating DMA enabled
    // memory actually makes things a bit slower.
    spi_transaction_t t;
    t.flags = 0;
    t.cmd = 0;
    t.addr = REG_FIFO;
    t.length = ptr_size * 8;
    t.rxlength = 0;
    t.tx_buffer = NULL;
    t.rx_buffer = ptr;
    ESP_ERROR_CHECK(spi_device_transmit_sync(sx127x->state.spi, &t));
    sx127x->state.rx_done = false;

    switch (sx127x->state.op_mode)
    {
    case SX127X_OP_MODE_FSK:
        // No need to clear the IRQ here, it's done automatically when
        // the FIFO is emptied.
        fec_decode(data, ptr_size, buf, size);
        break;
    case SX127X_OP_MODE_LORA:
        sx127x_write_reg(sx127x, REG_LORA_IRQ_FLAGS, IRQ_RX_DONE_MASK);
        break;
    }

    return size;
}

void sx127x_enable_continous_rx(sx127x_t *sx127x)
{
    sx127x->state.rx_done = false;
    sx127x->state.dio0_trigger = DIO0_TRIGGER_RX_DONE;

    switch (sx127x->state.op_mode)
    {
    case SX127X_OP_MODE_FSK:
        sx127x_idle(sx127x);
        sx127x_fsk_wait_for_mode_ready(sx127x);
        sx127x_write_reg(sx127x, REG_DIO_MAPPING_1, DIO0_FSK_PAYLOAD_READY);
        // Enable RX in packet mode
        sx127x_set_mode(sx127x, MODE_RX_CONTINUOUS);
        //sx127x_write_reg(sx127x, REG_FSK_RX_CONFIG, 1 << 7 | 1 << 4 | 1 << 3 | 6);
        sx127x_write_reg(sx127x, REG_FSK_FIFO_THRESH, 1 << 7 | sx127x->state.fsk.payload_length);
        break;
    case SX127X_OP_MODE_LORA:
        sx127x_prepare_write(sx127x);
        sx127x_write_reg(sx127x, REG_DIO_MAPPING_1, DIO0_LORA_RX_DONE);
        // Enter continous rx mode
        sx127x_set_mode(sx127x, MODE_LORA | MODE_RX_CONTINUOUS);
        break;
    }
}

bool sx127x_is_tx_done(sx127x_t *sx127x)
{
    return sx127x->state.tx_done;
}

bool sx127x_is_rx_done(sx127x_t *sx127x)
{
    return sx127x->state.rx_done;
}

void sx127x_set_callback(sx127x_t *sx127x, air_radio_callback_t callback, void *callback_data)
{
    sx127x->state.callback = callback;
    sx127x->state.callback_data = callback_data;
}

int sx127x_frequency_error(sx127x_t *sx127x)
{
    spi_transaction_t t;
    switch (sx127x->state.op_mode)
    {
    case SX127X_OP_MODE_FSK:
    {
        return 0;
        // This doesn't work properly in FSK mode
#if 0
        t.cmd = 0;
        t.addr = REG_FSK_FEI_MSB;
        t.length = 16;
        t.rxlength = 0;
        t.tx_buffer = NULL;
        t.flags = SPI_TRANS_USE_RXDATA;
        ESP_ERROR_CHECK(spi_device_transmit_sync(sx127x->state.spi, &t));

        int16_t err = t.rx_data[0] << 8 | t.rx_data[1];
        float ferr = err * SX127X_FSK_FREQ_STEP;
        // Sometimes we get values like +-2MHz, which seem
        // incorrect. To workaround this, ignore values bigger
        // than the RX bandwidth / 2
        return fabsf(ferr) < (sx127x->state.fsk.rx_bandwidth / 2) ? ferr : 0;
#endif
    }
    case SX127X_OP_MODE_LORA:
    {
        // Read all 3 registers in a single SPI transaction
        t.cmd = 0;
        t.addr = REG_LORA_FEI_MSB;
        t.length = 24;
        t.rxlength = 0;
        t.tx_buffer = NULL;
        t.flags = SPI_TRANS_USE_RXDATA;
        ESP_ERROR_CHECK(spi_device_transmit_sync(sx127x->state.spi, &t));

        int32_t err = t.rx_data[0] << 16 | t.rx_data[1] << 8 | t.rx_data[2];
        // Sign extend 20 bit 2's complement to 32 bit
        if (err & 0x80000)
        {
            err |= 0xfff00000;
        }

        float bw = sx127x_get_lora_signal_bw_khz(sx127x, sx127x->state.lora.signal_bw);
        return err * bw * ((float)(1L << 24) / (float)SX127X_FXOSC / 500.0);
    }
    }
    return 0;
}

int sx127x_rx_sensitivity(sx127x_t *sx127x)
{
    switch (sx127x->state.op_mode)
    {
    case SX127X_OP_MODE_FSK:
        // XXX: This needs to be properly tunned, probably with a
        // user performed calibration.
        return -95;
    case SX127X_OP_MODE_LORA:
        // We just list the senstivities for BW500 with shared RFIO
        // Non-shared RFIO has +3db
        if (sx127x->state.lora.signal_bw == SX127X_LORA_SIGNAL_BW_500)
        {
            switch (sx127x->state.lora.sf)
            {
            case 6:
                return -109;
            case 7:
                return -114;
            case 8:
                return -117;
            case 9:
                return -120;
            case 10:
                return -123;
            case 11:
                return -125; // -125.5db actually
            case 12:
                return -128;
            }
        }
    }
    return 0;
}

int sx127x_rssi(sx127x_t *sx127x, int *snr, int *lq)
{
    int rssi_max_dbm = 0;
    int rssi_value = 0;
    int snr_value = 0;
    int rx_sensitivity = sx127x_rx_sensitivity(sx127x);

    switch (sx127x->state.op_mode)
    {
    case SX127X_OP_MODE_FSK:
        rssi_value = sx127x_read_reg(sx127x, REG_FSK_RSSI_VALUE) / -2;
        // There's no actual SNR in FSK mode, so we approximate it
        snr_value = (-rx_sensitivity + rssi_value) * 4;
        break;
    case SX127X_OP_MODE_LORA:
    {
        rssi_max_dbm = 1;
        spi_transaction_t t;
        t.cmd = 0;
        t.addr = REG_LORA_PKT_SNR_VALUE;
        t.length = 16;
        t.rxlength = 0;
        t.tx_buffer = NULL;
        t.flags = SPI_TRANS_USE_RXDATA;
        ESP_ERROR_CHECK(spi_device_transmit_sync(sx127x->state.spi, &t));
        snr_value = (int8_t)t.rx_data[0];
        uint8_t raw_rssi = t.rx_data[1];
        int min_rssi = sx127x_lora_min_rssi(sx127x);
        if (snr_value >= 0)
        {
            // Page 87: "- When SNR>=0, the standard formula can be adjusted to
            // correct the slope: RSSI = -157+16/15 * PacketRssi
            // (or RSSI = -164+16/15 * PacketRssi)"
            rssi_value = min_rssi + (16 / 15.0) * raw_rssi;
        }
        else if (snr_value < 0)
        {
            // "Packet Strength (dBm) = -157 + PacketRssi + PacketSnr * 0.25 (when using the HF port and SNR < 0)"
            // Same for LF port
            rssi_value = min_rssi + raw_rssi + snr_value * 0.25f;
        }
        else
        {
            // Just add them
            rssi_value = min_rssi + raw_rssi;
        }
        break;
    }
    }
    // SNR is returned in 0.25dB units
    if (snr)
    {
        *snr = snr_value;
    }
    if (lq)
    {
        // FSK:
        // Max RSSI is 0dBm
        // LORA:
        // Assume max rssi is 1dBm. According to Pawel's tests
        // on 868MHz link stops working at around ~40 in the
        // register and reports up to ~165 (that'd be ~1db in
        // the HF port). However, testing on 433Mhz reveals up
        // to 9db when antennas are parallel and pretty close
        // at 100mw output.
        // With that said, we're more interested in granularity
        // in the lower RSSI levels, since that's what we're
        // using to switch modes to extend range as needed, so
        // we're using 1 as the max and constraining the result
        // to [0, 100].
        *lq = MAX(0, MIN(link_quality(rx_sensitivity, rssi_max_dbm, rssi_value), 100));
    }
    return rssi_value;
}

void sx127x_shutdown(sx127x_t *sx127x)
{
    sx127x_idle(sx127x);
    // TODO: This probably does nothing on most boards, since SX127X
    // resets on posedge on the reset line.
    hal_gpio_set_level(sx127x->rst, HAL_GPIO_LOW);
}

// #pragma region FSK specific functions

typedef struct sx127x_fsk_bandwidth_s
{
    uint32_t hz;
    uint8_t reg_value;
} sx127x_fsk_bandwidth_t;

static const sx127x_fsk_bandwidth_t fsk_bandwidths[] = {
    {2600, 0x17},
    {3100, 0x0F},
    {3900, 0x07},
    {5200, 0x16},
    {6300, 0x0E},
    {7800, 0x06},
    {10400, 0x15},
    {12500, 0x0D},
    {15600, 0x05},
    {20800, 0x14},
    {25000, 0x0C},
    {31300, 0x04},
    {41700, 0x13},
    {50000, 0x0B},
    {62500, 0x03},
    {83333, 0x12},
    {100000, 0x0A},
    {125000, 0x02},
    {166700, 0x11},
    {200000, 0x09},
    {250000, 0x01},
    {300000, 0x00}, // Invalid Bandwidth
};

static uint8_t sx127x_get_fsk_bandwidth_reg_value(unsigned hz)
{
    for (int ii = 0; ii < ARRAY_COUNT(fsk_bandwidths) - 1; ii++)
    {
        if ((hz >= fsk_bandwidths[ii].hz) && (hz < fsk_bandwidths[ii + 1].hz))
        {
            return fsk_bandwidths[ii].reg_value;
        }
    }
    LOG_E(TAG, "Invalid FSK bandwidth %u", hz);
    UNREACHABLE();
}

void sx127x_set_fsk_fdev(sx127x_t *sx127x, unsigned hz)
{
    sx127x_prepare_write(sx127x);
    uint16_t dev = lrintf(hz / SX127X_FSK_FREQ_STEP);
    sx127x_write_reg(sx127x, REG_FSK_FDEV_MSB, dev >> 8);
    sx127x_write_reg(sx127x, REG_FSK_FDEV_LSB, dev & 0xff);
}

void sx127x_set_fsk_bitrate(sx127x_t *sx127x, unsigned long bps)
{
    sx127x_prepare_write(sx127x);
    uint16_t br = lrintf((float)SX127X_FXOSC / bps);
    sx127x_write_reg(sx127x, REG_FSK_BITRATE_MSB, br >> 8);
    sx127x_write_reg(sx127x, REG_FSK_BITRATE_LSB, br & 0xff);
}

void sx127x_set_fsk_rx_bandwidth(sx127x_t *sx127x, unsigned hz)
{
    sx127x_prepare_write(sx127x);
    sx127x_write_reg(sx127x, REG_FSK_RX_BW, sx127x_get_fsk_bandwidth_reg_value(hz));
    sx127x->state.fsk.rx_bandwidth = hz;
}

void sx127x_set_fsk_rx_afc_bandwidth(sx127x_t *sx127x, unsigned hz)
{
    sx127x_prepare_write(sx127x);
    sx127x_write_reg(sx127x, REG_FSK_RX_AFC_BW, sx127x_get_fsk_bandwidth_reg_value(hz));
}

void sx127x_set_fsk_preamble_length(sx127x_t *sx127x, unsigned length)
{
    sx127x_prepare_write(sx127x);
    uint16_t len = length;
    sx127x_write_reg(sx127x, REG_FSK_PREAMBLE_MSB, len >> 8);
    sx127x_write_reg(sx127x, REG_FSK_PREAMBLE_LSB, len & 0xff);
}

// #pragma endregion

// #pragma region LoRa specific functions

static float sx127x_get_lora_signal_bw_khz(sx127x_t *sx127x, sx127x_lora_signal_bw_e sbw)
{
    switch (sbw)
    {
    case SX127X_LORA_SIGNAL_BW_7_8:
        return 7.8f;
    case SX127X_LORA_SIGNAL_BW_10_4:
        return 10.4f;
    case SX127X_LORA_SIGNAL_BW_15_6:
        return 15.6f;
    case SX127X_LORA_SIGNAL_BW_20_8:
        return 20.8f;
    case SX127X_LORA_SIGNAL_BW_31_25:
        return 31.25f;
    case SX127X_LORA_SIGNAL_BW_41_7:
        return 41.27f;
    case SX127X_LORA_SIGNAL_BW_62_5:
        return 62.5f;
    case SX127X_LORA_SIGNAL_BW_250:
        return 250;
    case SX127X_LORA_SIGNAL_BW_500:
        return 500;
    }
    return 0;
}

static void sx127x_apply_bw500_sensitivity_workaround(sx127x_t *sx127x)
{
    // This is called from sx127x_set_lora_signal_bw, so the chip is
    // already ready for writing
    // See https://www.semtech.com/uploads/documents/sx1276_77_78-errata.pdf
    // TLDR:
    // BW500 && freq >= 862M && freq <= 1020M
    //  set reg(0x36) = 0x02, reg(0x3a) = 0x64
    // BW500 && freq >= 410M && freq <= 525M
    //  set reg(0x36) = 0x02, reg(0x3a) = 0x7F
    // All other combinations, set reg(0x36) = 0x03, reg(0x3a) will be selected
    // automatically by the chip.
    uint8_t workaround;
    if (sx127x->state.lora.signal_bw == SX127X_LORA_SIGNAL_BW_500 &&
        sx127x->state.lora.freq >= 862000000 && sx127x->state.lora.freq <= 1020000000)
    {
        workaround = SX127X_RX_SENSITIVITY_BW500_WORKAROUND_HIGH_BAND;
    }
    else if (sx127x->state.lora.signal_bw == SX127X_LORA_SIGNAL_BW_500 &&
             sx127x->state.lora.freq >= 410000000 && sx127x->state.lora.freq <= 525000000)
    {
        workaround = SX127X_RX_SENSITIVITY_BW500_WORKAROUND_LOW_BAND;
    }
    else
    {
        workaround = SX127X_RX_SENSITIVITY_BW500_WORKAROUND_NONE;
    }
    if (workaround != sx127x->state.lora.bw_workaround)
    {
        switch (workaround)
        {
        case SX127X_RX_SENSITIVITY_BW500_WORKAROUND_NONE:
            sx127x_write_reg(sx127x, REG_LORA_DETECTION_BW500_OPTIMIZE_1, 0x03);
            break;
        case SX127X_RX_SENSITIVITY_BW500_WORKAROUND_HIGH_BAND:
            sx127x_write_reg(sx127x, REG_LORA_DETECTION_BW500_OPTIMIZE_1, 0x02);
            sx127x_write_reg(sx127x, REG_LORA_DETECTION_BW500_OPTIMIZE_2, 0x64);
            break;
        case SX127X_RX_SENSITIVITY_BW500_WORKAROUND_LOW_BAND:
            sx127x_write_reg(sx127x, REG_LORA_DETECTION_BW500_OPTIMIZE_1, 0x02);
            sx127x_write_reg(sx127x, REG_LORA_DETECTION_BW500_OPTIMIZE_2, 0x7F);
            break;
        }
        sx127x->state.lora.bw_workaround = workaround;
    }
}

void sx127x_set_lora_spreading_factor(sx127x_t *sx127x, int sf)
{
    sx127x_prepare_write(sx127x);

    if (sf < 6)
    {
        sf = 6;
    }
    else if (sf > 12)
    {
        sf = 12;
    }

    if (sf == 6)
    {
        sx127x_write_reg(sx127x, REG_LORA_DETECTION_OPTIMIZE, 0xc5);
        sx127x_write_reg(sx127x, REG_LORA_DETECTION_THRESHOLD, 0x0c);
    }
    else
    {
        sx127x_write_reg(sx127x, REG_LORA_DETECTION_OPTIMIZE, 0xc3);
        sx127x_write_reg(sx127x, REG_LORA_DETECTION_THRESHOLD, 0x0a);
    }
    sx127x_write_reg(sx127x, REG_LORA_MODEM_CONFIG_2, (sx127x_read_reg(sx127x, REG_LORA_MODEM_CONFIG_2) & 0x0f) | ((sf << 4) & 0xf0));
    sx127x->state.lora.sf = sf;
}

void sx127x_set_lora_signal_bw(sx127x_t *sx127x, sx127x_lora_signal_bw_e sbw)
{
    sx127x_prepare_write(sx127x);

    if (sbw < SX127X_LORA_SIGNAL_BW_7_8)
    {
        sbw = SX127X_LORA_SIGNAL_BW_7_8;
    }
    else if (sbw > SX127X_LORA_SIGNAL_BW_500)
    {
        sbw = SX127X_LORA_SIGNAL_BW_500;
    }
    uint8_t reg = sx127x_read_reg(sx127x, REG_LORA_MODEM_CONFIG_1);
    reg = (reg & 0x0f) | (sbw << 4);
    sx127x_write_reg(sx127x, REG_LORA_MODEM_CONFIG_1, reg);
    sx127x->state.lora.signal_bw = sbw;
    sx127x_apply_bw500_sensitivity_workaround(sx127x);
}

void sx127x_set_lora_coding_rate(sx127x_t *sx127x, sx127x_lora_coding_rate_e rate)
{
    sx127x_prepare_write(sx127x);

    if (rate < SX127X_LORA_CODING_RATE_4_5)
    {
        rate = SX127X_LORA_CODING_RATE_4_5;
    }
    else if (rate > SX127X_LORA_CODING_RATE_4_8)
    {
        rate = SX127X_LORA_CODING_RATE_4_8;
    }

    uint8_t reg = sx127x_read_reg(sx127x, REG_LORA_MODEM_CONFIG_1);
    reg = (reg & 0xf1) | (rate << 1);
    sx127x_write_reg(sx127x, REG_LORA_MODEM_CONFIG_1, reg);
}

void sx127x_set_lora_preamble_length(sx127x_t *sx127x, long length)
{
    sx127x_prepare_write(sx127x);

    sx127x_write_reg(sx127x, REG_LORA_PREAMBLE_MSB, (uint8_t)(length >> 8));
    sx127x_write_reg(sx127x, REG_LORA_PREAMBLE_LSB, (uint8_t)(length >> 0));
}

void sx127x_set_lora_crc(sx127x_t *sx127x, bool crc)
{
    sx127x_prepare_write(sx127x);

    uint8_t reg = sx127x_read_reg(sx127x, REG_LORA_MODEM_CONFIG_2);
    if (crc)
    {
        reg |= 0x04;
    }
    else
    {
        reg &= 0xfb;
    }
    sx127x_write_reg(sx127x, REG_LORA_MODEM_CONFIG_2, reg);
}

void sx127x_set_lora_header_mode(sx127x_t *sx127x, sx127x_lora_header_e mode)
{
    uint8_t reg = sx127x_read_reg(sx127x, REG_LORA_MODEM_CONFIG_1);
    switch (mode)
    {
    case SX127X_LORA_HEADER_IMPLICIT:
        reg |= 0x01;
        break;
    case SX127X_LORA_HEADER_EXPLICIT:
        reg &= 0xfe;
        break;
    }
    sx127x_write_reg(sx127x, REG_LORA_MODEM_CONFIG_1, reg);
}

void sx127x_set_lora_sync_word(sx127x_t *sx127x, uint8_t sw)
{
    if (sw == 0)
    {
        // Sync word at zero won't work. See page 68 of the datasheet
        sw = 1;
    }
    else if (sw == 0x34)
    {
        // 0x34 is reserved for LoRaWAN
        sw = 0x35;
    }
    sx127x_write_reg(sx127x, REG_LORA_SYNC_WORD, sw);
}

int sx127x_lora_min_rssi(sx127x_t *sx127x)
{
    // Page 87, 5.5.5.
    if (sx127x->state.lora.freq > 700000)
    {
        // Using (HF) 862-1020MHz (779-960MHz*)
        return -157;
    }
    // Using (LF) 410-525 (*480)MHz or (LF) 137-175 (*160)MHz
    return -164;
}

// #pragma endregion
