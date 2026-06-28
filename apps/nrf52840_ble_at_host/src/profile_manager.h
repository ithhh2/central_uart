#ifndef NRF52840_BLE_AT_HOST_PROFILE_MANAGER_H_
#define NRF52840_BLE_AT_HOST_PROFILE_MANAGER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/conn.h>

#include "app_common.h"

enum profile_manager_error {
	PROFILE_MANAGER_ERROR_SERVICE_MISSING = 0,
	PROFILE_MANAGER_ERROR_NOTIFY_MISSING,
	PROFILE_MANAGER_ERROR_WRITE_MISSING,
	PROFILE_MANAGER_ERROR_DISCOVER_FAILED,
	PROFILE_MANAGER_ERROR_SUBSCRIBE_FAILED,
	PROFILE_MANAGER_ERROR_WRITE_FAILED,
};

struct profile_manager_callbacks {
	void (*ready)(void *context);
	void (*payload_received)(void *context, const uint8_t *data, uint16_t len);
	void (*mtu_updated)(void *context, uint16_t payload_len);
	void (*error)(void *context, enum profile_manager_error error);
};

void profile_manager_init(const struct ble_host_config *config,
			  const struct profile_manager_callbacks *callbacks, void *context);
void profile_manager_set_config(const struct ble_host_config *config);
void profile_manager_reset(void);
int profile_manager_start(struct bt_conn *conn);
void profile_manager_handle_disconnected(struct bt_conn *conn);
int profile_manager_request_mtu(void);
int profile_manager_request_data_len(void);
int profile_manager_request_phy(void);
int profile_manager_send(const uint8_t *data, size_t len);
bool profile_manager_is_ready(void);
bool profile_manager_can_enter_data_mode(void);
uint16_t profile_manager_get_payload_len(void);

#endif
