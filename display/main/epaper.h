#include "driver/gpio.h"
#include <driver/spi_master.h>

typedef struct {
        gpio_num_t reset_pin;
        gpio_num_t dc_pin;
        gpio_num_t cs_pin;
        gpio_num_t busy_pin;
        gpio_num_t mosi_pin;
        gpio_num_t sck_pin;

        int clk_freq_hz;
        spi_host_device_t spi_host;
} epaper_conf_t;

typedef struct epaper_dev *epaper_handle_t; /*handle of epaper*/

void epaper_reset(epaper_handle_t dev);
epaper_handle_t epaper_create(epaper_conf_t epconf);
esp_err_t epaper_delete(epaper_handle_t dev);
void epaper_display(epaper_handle_t dev, const uint8_t *data);
void epaper_sleep(epaper_handle_t dev);
