#pragma once
#include <esp_err.h>

esp_err_t graphite(const char *prefix, const char **metric, const float *value);
esp_err_t graphite_init();
