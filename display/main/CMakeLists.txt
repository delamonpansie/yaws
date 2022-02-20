idf_component_register(
  SRCS "main.c" "epaper.c"
  INCLUDE_DIRS "."
  REQUIRES esp_rom nvs_flash syslog graphite log i2cdev esp_adc_cal
)
