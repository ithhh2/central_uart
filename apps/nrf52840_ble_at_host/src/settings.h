#ifndef NRF52840_BLE_AT_HOST_SETTINGS_H_
#define NRF52840_BLE_AT_HOST_SETTINGS_H_

#include "app_common.h"

int ble_host_settings_init(void);
const struct ble_host_config *ble_host_settings_get(void);
int ble_host_settings_update_profile(const char *service_uuid, const char *notify_uuid,
				     const char *write_uuid);
int ble_host_settings_update_mtu(uint16_t mtu);
int ble_host_settings_update_conn_params(uint16_t min_interval, uint16_t max_interval,
					 uint16_t latency, uint16_t timeout);

#endif
