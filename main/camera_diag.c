// Runs only when esp_camera_init() fails. The driver's "Detected camera not supported"
// covers both "nothing answered on SCCB" and "something answered with an unknown ID", so
// this scans the bus itself and, if an OV5640 address answers, reads its ID registers.

#include "camera_diag.h"

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "camdiag";

#define DIAG_PIN_XCLK 3
#define DIAG_PIN_SDA  48
#define DIAG_PIN_SCL  47
#define OV5640_ADDR   0x3C
#define OV2640_ADDR   0x30

static esp_err_t read_reg16(i2c_master_dev_handle_t dev, uint16_t reg, uint8_t *out)
{
    uint8_t addr[2] = {reg >> 8, reg & 0xFF};
    return i2c_master_transmit_receive(dev, addr, sizeof(addr), out, 1, 100);
}

void camera_diagnose(void)
{
    ESP_LOGW(TAG, "Camera init failed: scanning SCCB (SDA %d, SCL %d) with XCLK on GPIO%d",
             DIAG_PIN_SDA, DIAG_PIN_SCL, DIAG_PIN_XCLK);

    // OmniVision sensors do not answer SCCB without a clock, so drive XCLK at 20 MHz.
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 20000000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t channel = {
        .gpio_num = DIAG_PIN_XCLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_1,
        .duty = 1,
        .hpoint = 0,
    };
    bool clock_ok = ledc_timer_config(&timer) == ESP_OK && ledc_channel_config(&channel) == ESP_OK;
    if (!clock_ok) {
        ESP_LOGE(TAG, "Could not start XCLK; scanning without it");
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = DIAG_PIN_SDA,
        .scl_io_num = DIAG_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "Could not create I2C bus for the scan");
        goto out_clock;
    }

    int found = 0;
    bool have_ov5640_addr = false;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            found++;
            ESP_LOGW(TAG, "  device ACKs at 0x%02x%s", addr,
                     addr == OV5640_ADDR ? " (OV5640/OV3660 address)" :
                     addr == OV2640_ADDR ? " (OV2640/OV7725 address)" : "");
            if (addr == OV5640_ADDR) {
                have_ov5640_addr = true;
            }
        }
    }

    if (found == 0) {
        ESP_LOGE(TAG, "Nothing answered on SCCB. The sensor is not responding: check the ribbon "
                      "is fully seated, the right way round and latched, and that the module is powered.");
    }

    if (have_ov5640_addr) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = OV5640_ADDR,
            .scl_speed_hz = 100000,
        };
        i2c_master_dev_handle_t dev;
        if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) == ESP_OK) {
            uint8_t hi = 0, lo = 0;
            esp_err_t e1 = read_reg16(dev, 0x300A, &hi);
            esp_err_t e2 = read_reg16(dev, 0x300B, &lo);
            if (e1 == ESP_OK && e2 == ESP_OK) {
                ESP_LOGW(TAG, "  0x3C chip ID (0x300A/0x300B) = 0x%02x%02x (OV5640 is 0x5640)", hi, lo);
            } else {
                ESP_LOGE(TAG, "  0x3C ACKed but reading the ID failed: %s / %s",
                         esp_err_to_name(e1), esp_err_to_name(e2));
            }
            i2c_master_bus_rm_device(dev);
        }
    }

    i2c_del_master_bus(bus);

out_clock:
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
}
