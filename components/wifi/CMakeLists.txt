idf_component_register(
  SRCS wifi.c
  INCLUDE_DIRS .
  REQUIRES esp_https_ota app_update
)
