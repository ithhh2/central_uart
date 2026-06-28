#ifndef NRF52840_BLE_AT_HOST_AT_HOST_H_
#define NRF52840_BLE_AT_HOST_AT_HOST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_common.h"

struct at_host_ops {
	int (*start_scan)(void *context, uint16_t duration_s, const char *filter_name);
	int (*connect)(void *context, const char *mac, uint8_t addr_type);
	int (*disconnect)(void *context, uint8_t conn_index);
	int (*send_payload)(void *context, uint8_t conn_index, const uint8_t *data, size_t len);
	void (*get_status)(void *context, struct ble_host_status *status);
	void (*get_profile)(void *context, struct ble_profile *profile);
	int (*set_profile)(void *context, const char *service_uuid, const char *notify_uuid,
			   const char *write_uuid);
	int (*set_mtu)(void *context, uint16_t mtu);
	uint16_t (*get_mtu)(void *context);
	int (*request_mtu)(void *context, uint8_t conn_index);
	int (*set_conn_params)(void *context, uint8_t conn_index, uint16_t min_interval,
			       uint16_t max_interval, uint16_t latency, uint16_t timeout);
	int (*enter_data_mode)(void *context, uint8_t conn_index);
	void (*get_stream_stats)(void *context, struct ble_stream_stats *stats);
};

struct at_host_tx {
	void (*line)(void *context, const char *line);
	void (*raw)(void *context, const uint8_t *data, size_t len);
};

void at_host_init(const struct at_host_ops *ops, const struct at_host_tx *tx, void *context);
void at_host_rx_bytes(const uint8_t *data, size_t len);
void at_host_notify_scan_result(uint8_t index, const char *mac, const char *name,
				uint8_t addr_type, int8_t rssi);
void at_host_notify_scan_complete(void);
void at_host_notify_connected(void);
void at_host_notify_disconnected(void);
void at_host_notify_ready(void);
void at_host_notify_mtu(uint16_t payload_len);
void at_host_notify_receive(const uint8_t *data, uint16_t len);
void at_host_notify_link_params(uint16_t interval, uint16_t latency, uint16_t timeout);
void at_host_notify_data_mode(bool enabled);
void at_host_notify_function_error(void);

#endif
