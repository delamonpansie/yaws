#pragma once

esp_err_t graphite(const char *ip, const char *prefix, const char **metric, float *value);
char *mac_prefix(const char *prefix);
