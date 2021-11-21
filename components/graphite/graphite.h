#pragma once

esp_err_t graphite(const char *prefix, const char **metric, float *value);
esp_err_t graphite_init();
