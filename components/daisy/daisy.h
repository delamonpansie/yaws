#pragma once

#include "lwip/dhcp.h"
extern ip4_addr_t ip_addr;
extern uint8_t mac_addr[6];
extern char bootp[DHCP_BOOT_FILE_LEN];
esp_err_t wifi_connect(void);
esp_err_t wifi_disconnect(void);
int wifi_connected(void);
