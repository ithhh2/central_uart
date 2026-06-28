#ifndef NRF52840_BLE_AT_HOST_APP_COMMON_H_
#define NRF52840_BLE_AT_HOST_APP_COMMON_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/uuid.h>

#define BLE_HOST_UUID_STR_LEN BT_UUID_STR_LEN
#define BLE_HOST_NAME_MAX_LEN 32
#define BLE_HOST_SCAN_FILTER_MAX_LEN 32
#define BLE_HOST_SEND_MAX_LEN 256

struct ble_profile {
	char service_uuid[BLE_HOST_UUID_STR_LEN];
	char notify_uuid[BLE_HOST_UUID_STR_LEN];
	char write_uuid[BLE_HOST_UUID_STR_LEN];
};

struct ble_host_config {
	struct ble_profile active_profile;
	uint16_t mtu;
	uint16_t conn_interval_min;
	uint16_t conn_interval_max;
	uint16_t latency;
	uint16_t timeout;
	bool auto_mtu;
	bool auto_subscribe;
	bool prefer_2m_phy;
	bool prefer_dle;
	bool allow_data_mode;
	uint16_t stream_guard_time_ms;
};

struct ble_host_status {
	bool connected;
	bool ready;
	char mac[BT_ADDR_LE_STR_LEN];
	uint8_t conn_count;
	uint16_t data_len;
};

struct ble_stream_stats {
	uint32_t bytes_host_to_ble;
	uint32_t bytes_ble_to_host;
	uint32_t write_ops;
	uint32_t notify_ops;
	uint32_t usb_backpressure_count;
	uint32_t max_host_to_ble_depth;
	uint32_t max_ble_to_host_depth;
	uint32_t enter_data_mode_count;
	uint32_t unexpected_disconnect_count;
};

bool ble_host_normalize_uuid(const char *input, char *output, size_t output_len);
void ble_host_format_uuid_from_bt(const struct bt_uuid *uuid, char *output, size_t output_len);
bool ble_host_uuid_equals(const char *lhs, const char *rhs);
uint8_t ble_host_addr_type_to_wlt(uint8_t bt_addr_type);
bool ble_host_addr_type_from_wlt(uint8_t wlt_addr_type, uint8_t *bt_addr_type);
bool ble_host_is_printable_ascii(const uint8_t *data, size_t len);

#endif
