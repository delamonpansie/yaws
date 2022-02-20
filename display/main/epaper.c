#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include <driver/spi_master.h>
#include <driver/gpio.h>

#include "epaper.h"

enum EPAPER_CMD {
        EPAPER_PANEL_SETTING = 0x00,
        EPAPER_POWER_SETTING = 0x01,
        EPAPER_POWER_OFF = 0x02,
        EPAPER_POWER_OFF_SEQUENCE_SETTING = 0x03,
        EPAPER_POWER_ON = 0x04,
        EPAPER_POWER_ON_MEASURE = 0x05,
        EPAPER_BOOSTER_SOFT_START = 0x06,
        EPAPER_DEEP_SLEEP = 0x07,
        EPAPER_DISPLAY_START_TRANSMISSION_1 = 0x10,
        EPAPER_DATA_STOP = 0x11,
        EPAPER_DISPLAY_REFRESH = 0x12,
        EPAPER_DISPLAY_START_TRANSMISSION_2 = 0x13,
        EPAPER_DUAL_SPI = 0x15,
        EPAPER_AUTO_SEQUENCE = 0x17,
        EPAPER_KW_LUT_OPTION  = 0x2b,
        EPAPER_PLL_CONTROL = 0x30,
        EPAPER_TEMPERATURE_SENSOR_CALIBRATION = 0x40,
        EPAPER_TEMPERATURE_SENSOR_SELECTION = 0x41,
        EPAPER_TEMPERATURE_SENSOR_WRITE = 0x42,
        EPAPER_TEMPERATURE_SENSOR_READ = 0x43,
        EPAPER_VCOM_AND_DATA_INTERVAL_SETTING = 0x50,
        EPAPER_LOW_POWER_DETECTION = 0x51,
        EPAPAER_END_VOLTAGE = 0x52,
        EPAPER_TCON_SETTING = 0x60,
        EPAPER_TCON_RESOLUTION = 0x61,
        EPAPER_SOURCE_AND_GATE_START_SETTING = 0x62,
        EPAPER_GET_STATUS = 0x71,
        EPAPER_AUTO_MEASURE_VCOM = 0x80,
        EPAPER_READ_VCOM_VALUE = 0x81,
        EPAPER_VCM_DC_SETTING = 0x82,
};

typedef struct epaper_dev *epaper_handle_t; /*handle of epaper*/

static const char* TAG = "epaper";

#define EPAPER_CS_SETUP_NS      55
#define EPAPER_CS_HOLD_NS       60
#define EPAPER_1S_NS            1000000000
#define EPAPER_QUE_SIZE_DEFAULT 10
#define EPAPER_WIDTH		800
#define EPAPER_HEIGHT		480


typedef struct epaper_dev {
    spi_device_handle_t bus;
    epaper_conf_t pin;
    xSemaphoreHandle spi_mux;
} epaper_dev_t;

static void send_command(epaper_dev_t *dev, uint8_t command)
{
        while (gpio_get_level(dev->pin.busy_pin) == 0)      // 0: busy, 1: idle
                vTaskDelay(50 / portTICK_PERIOD_MS);
        gpio_set_level(dev->pin.dc_pin, 0);
        spi_transaction_t tx = {
                .length = 8,                                // length is in bits
                .tx_buffer = &command,
        };
        ESP_ERROR_CHECK(spi_device_transmit(dev->bus, &tx));
        ESP_LOGI(TAG, "command 0x%02x", command);
}

static void send_data(epaper_handle_t dev, const uint8_t *data, int length)
{
        gpio_set_level(dev->pin.dc_pin, 1);
        while (length > 0) {
                int tx_len = length;
                if (tx_len > 1024)
                        tx_len = 1024;
                spi_transaction_t tx = {
                        .length = tx_len * 8,                  // length is in bits
                        .tx_buffer = data,
                };
                ESP_ERROR_CHECK(spi_device_transmit(dev->bus, &tx));
                length -= tx_len;
                data += tx_len;
        }
}

static void send_byte(epaper_handle_t dev, const uint8_t data)
{
        send_data(dev, &data, 1);
}

static void epaper_gpio_init(epaper_conf_t * pin)
{
        gpio_pad_select_gpio(pin->reset_pin);
        gpio_set_direction(pin->reset_pin, GPIO_MODE_OUTPUT);
        gpio_set_level(pin->reset_pin, 1);
        gpio_pad_select_gpio(pin->dc_pin);
        gpio_set_direction(pin->dc_pin, GPIO_MODE_OUTPUT);
        gpio_pad_select_gpio(pin->busy_pin);
        gpio_set_direction(pin->busy_pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode(pin->busy_pin, GPIO_PULLUP_ONLY);
}

static esp_err_t spi_init(epaper_handle_t dev)
{
        spi_bus_config_t buscfg = {
                .miso_io_num = -1,                       // Halfduplex, no MISO pin
                .mosi_io_num = dev->pin.mosi_pin,
                .sclk_io_num = dev->pin.sck_pin,
                .quadwp_io_num = -1,
                .quadhd_io_num = -1,
        };
        spi_device_interface_config_t devcfg = {
                .clock_speed_hz = dev->pin.clk_freq_hz,
                .mode = 0,                               // SPI mode 0
                .spics_io_num = dev->pin.cs_pin,         // we will use external CS pin
                .cs_ena_pretrans = EPAPER_CS_SETUP_NS / (EPAPER_1S_NS / (dev->pin.clk_freq_hz)) + 2,
                .cs_ena_posttrans = EPAPER_CS_HOLD_NS / (EPAPER_1S_NS / (dev->pin.clk_freq_hz)) + 2,
                .queue_size = EPAPER_QUE_SIZE_DEFAULT,
                .flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_3WIRE,
        };
        //Initialize the SPI bus
        ESP_ERROR_CHECK(spi_bus_initialize(dev->pin.spi_host, &buscfg, 1));
        //Attach the EPD to the SPI bus
        ESP_ERROR_CHECK(spi_bus_add_device(dev->pin.spi_host, &devcfg, &dev->bus));
        return ESP_OK;
}


void epaper_reset(epaper_handle_t dev)
{
        ESP_LOGI(TAG, "reset");
        xSemaphoreTakeRecursive(dev->spi_mux, portMAX_DELAY);
        gpio_set_level(dev->pin.reset_pin, 0);
        ets_delay_us(55);                                      // minimal width of RST_N=low is 50us
        gpio_set_level(dev->pin.reset_pin, 1);                 // module reset
        xSemaphoreGiveRecursive(dev->spi_mux);
}

static void epaper_epd_init(epaper_handle_t dev)
{
        xSemaphoreTakeRecursive(dev->spi_mux, portMAX_DELAY);
        /* EPD hardware init start */
        epaper_reset(dev);

        send_command(dev, EPAPER_BOOSTER_SOFT_START);
        send_byte(dev, 0x17);
        send_byte(dev, 0x17);
        send_byte(dev, 0x27);
        send_byte(dev, 0x17);

        send_command(dev, EPAPER_POWER_SETTING);
        send_byte(dev, 0b111);          //VGH=20V
        send_byte(dev, 0b111);          //VGL=-20V
        send_byte(dev, 0b111010);	//VDH=14V
        send_byte(dev, 0b111010);	//VDL=-14V

        send_command(dev, EPAPER_POWER_ON);

        send_command(dev, EPAPER_PANEL_SETTING);
        send_byte(dev, 0x1F);   // LUT from OTP, KW mode, Scan up, Shift right

        send_command(dev, EPAPER_PLL_CONTROL);
        send_byte(dev, 0x06);

        send_command(dev, EPAPER_TCON_RESOLUTION);
        send_byte(dev, (EPAPER_WIDTH) >> 8);
        send_byte(dev, (EPAPER_WIDTH) & 0xff);
        send_byte(dev, (EPAPER_HEIGHT) >> 8);
        send_byte(dev, (EPAPER_HEIGHT) & 0xff);

        send_byte(dev, EPAPER_DUAL_SPI);
        send_byte(dev, 0x00);

        send_command(dev, EPAPER_VCOM_AND_DATA_INTERVAL_SETTING);
        send_byte(dev, 0x10); // manual says = 0x31); // 0x11 если V2B
        send_byte(dev, 0x07);

        /* send_command(dev, EPAPER_TCON_SETTING); */
        /* send_byte(dev, 0x22); */

        xSemaphoreGiveRecursive(dev->spi_mux);
}

epaper_handle_t epaper_create(epaper_conf_t epconf)
{
        epaper_dev_t* dev = calloc(1, sizeof *dev);
        dev->spi_mux = xSemaphoreCreateRecursiveMutex();
        dev->pin = epconf;
        epaper_gpio_init(&dev->pin);
        spi_init(dev);
        epaper_epd_init(dev);
        return dev;
}

esp_err_t epaper_delete(epaper_handle_t dev)
{
        send_command(dev, EPAPER_POWER_OFF);
        spi_bus_remove_device(dev->bus);
        spi_bus_free(dev->pin.spi_host);
        vSemaphoreDelete(dev->spi_mux);
        free(dev);
        return ESP_OK;
}

void epaper_display(epaper_handle_t dev, const uint8_t *data)
{
        send_command(dev, EPAPER_DISPLAY_START_TRANSMISSION_2);
        send_data(dev, data, EPAPER_WIDTH * EPAPER_HEIGHT / 8);
        send_command(dev, EPAPER_DISPLAY_REFRESH);
}

void epaper_sleep(epaper_handle_t dev)
{
    xSemaphoreTakeRecursive(dev->spi_mux, portMAX_DELAY);
    send_command(dev, EPAPER_DEEP_SLEEP);
    send_byte(dev, 0xa5);
    xSemaphoreGiveRecursive(dev->spi_mux);
}

