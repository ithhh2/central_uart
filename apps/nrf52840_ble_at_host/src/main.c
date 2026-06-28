/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sample_usbd.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/usbd.h>

#include "app_common.h"
#include "at_host.h"
#include "profile_manager.h"
#include "settings.h"

LOG_MODULE_REGISTER(ble_at_host, LOG_LEVEL_INF);

#define CONTROL_UART_NODE DT_NODELABEL(board_cdc_acm_uart)
#define MAX_SCANNED_DEVICES 32
#define CONTROL_UART_RX_CHUNK 64
#define HOST_TO_BLE_RING_SIZE 16384
#define BLE_TO_HOST_RING_SIZE 16384
#define CTRL_TX_RING_SIZE 2048
#define UART_TX_CHUNK 128
#define DATA_MODE_TX_COALESCE_MS 1

enum app_state {
	APP_STATE_IDLE = 0,
	APP_STATE_SCANNING,
	APP_STATE_CONNECTING,
	APP_STATE_CONTROL_READY,
	APP_STATE_DATA_MODE,
	APP_STATE_DISCONNECTING,
};

struct scanned_device_entry {
	bool valid;
	uint8_t index;
	bt_addr_le_t addr;
	char mac[BT_ADDR_STR_LEN];
	char name[BLE_HOST_NAME_MAX_LEN];
	int8_t rssi;
};

struct data_mode_escape_state {
	bool active;
	uint8_t count;
};

RING_BUF_DECLARE(host_to_ble_ring, HOST_TO_BLE_RING_SIZE);
RING_BUF_DECLARE(ble_to_host_ring, BLE_TO_HOST_RING_SIZE);
RING_BUF_DECLARE(ctrl_tx_ring, CTRL_TX_RING_SIZE);

static const struct device *const control_uart = DEVICE_DT_GET(CONTROL_UART_NODE);

static struct usbd_context *sample_usbd;
static struct bt_conn *current_conn;
static struct scanned_device_entry scanned_devices[MAX_SCANNED_DEVICES];
static struct k_work_delayable scan_stop_work;
static struct k_work ble_write_work;
static struct k_work_delayable ble_write_retry_work;
static struct k_work_delayable ble_write_coalesce_work;
static struct k_work_delayable escape_guard_work;
static struct k_spinlock ring_lock;
static struct ble_stream_stats stream_stats;
static char scan_filter_name[BLE_HOST_SCAN_FILTER_MAX_LEN];
static bool control_dtr_ready;
static bool control_rx_paused;
static bool data_mode_pending_enter;
static bool data_mode_tx_flush_due;
static struct data_mode_escape_state escape_state;
static enum app_state app_state = APP_STATE_IDLE;
static uint8_t next_scan_index;
static int64_t last_host_rx_time_ms;

K_SEM_DEFINE(usb_ready_sem, 0, 1);
K_SEM_DEFINE(ble_ready_sem, 0, 1);

static void kick_uart_tx(void);
static void schedule_ble_write_pump(k_timeout_t delay);
static void schedule_data_mode_ble_tx(void);
static void refresh_uart_rx_flow_control(void);

static void set_state(enum app_state new_state)
{
	app_state = new_state;
}

static uint32_t ring_put_bytes(struct ring_buf *ring, const uint8_t *data, uint32_t len)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);
	uint32_t written = ring_buf_put(ring, data, len);

	k_spin_unlock(&ring_lock, key);
	return written;
}

static uint32_t ring_size_bytes(struct ring_buf *ring)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);
	uint32_t size = ring_buf_size_get(ring);

	k_spin_unlock(&ring_lock, key);
	return size;
}

static uint32_t ring_space_bytes(struct ring_buf *ring)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);
	uint32_t size = ring_buf_space_get(ring);

	k_spin_unlock(&ring_lock, key);
	return size;
}

static void ring_reset_bytes(struct ring_buf *ring)
{
	k_spinlock_key_t key = k_spin_lock(&ring_lock);

	ring_buf_reset(ring);
	k_spin_unlock(&ring_lock, key);
}

static void update_max_depth(uint32_t current_depth, uint32_t *max_depth)
{
	if (current_depth > *max_depth) {
		*max_depth = current_depth;
	}
}

static void kick_uart_tx(void)
{
	if (!control_dtr_ready) {
		return;
	}

	uart_irq_tx_enable(control_uart);
}

static void queue_ctrl_bytes(const uint8_t *data, size_t len)
{
	if (len == 0U) {
		return;
	}

	(void)ring_put_bytes(&ctrl_tx_ring, data, (uint32_t)len);
	kick_uart_tx();
}

static void queue_ctrl_line(const char *line)
{
	static const uint8_t newline[] = "\r\n";

	queue_ctrl_bytes((const uint8_t *)line, strlen(line));
	queue_ctrl_bytes(newline, sizeof(newline) - 1U);
}

static void queue_data_bytes(const uint8_t *data, size_t len)
{
	uint32_t written;

	if (len == 0U) {
		return;
	}

	written = ring_put_bytes(&ble_to_host_ring, data, (uint32_t)len);
	if (written < len) {
		stream_stats.usb_backpressure_count++;
	}

	update_max_depth(ring_size_bytes(&ble_to_host_ring), &stream_stats.max_ble_to_host_depth);
	kick_uart_tx();
}

static void clear_scan_cache(void)
{
	memset(scanned_devices, 0, sizeof(scanned_devices));
	next_scan_index = 0U;
}

static void clear_data_mode_session(bool reset_mode)
{
	ring_reset_bytes(&ble_to_host_ring);
	control_rx_paused = false;
	data_mode_pending_enter = false;
	data_mode_tx_flush_due = false;
	escape_state.active = false;
	escape_state.count = 0U;
	(void)k_work_cancel_delayable(&ble_write_retry_work);
	(void)k_work_cancel_delayable(&ble_write_coalesce_work);
	(void)k_work_cancel_delayable(&escape_guard_work);

	if (reset_mode) {
		set_state(current_conn != NULL ? APP_STATE_CONTROL_READY : APP_STATE_IDLE);
	}
}

static void parse_device_name(struct net_buf_simple *ad, char *name, size_t name_len)
{
	struct net_buf_simple_state state;

	name[0] = '\0';
	net_buf_simple_save(ad, &state);

	while (ad->len > 1U) {
		uint8_t field_len = net_buf_simple_pull_u8(ad);

		if (field_len == 0U || field_len > ad->len) {
			break;
		}

		uint8_t field_type = net_buf_simple_pull_u8(ad);
		uint8_t value_len = field_len - 1U;

		if (field_type == BT_DATA_NAME_COMPLETE || field_type == BT_DATA_NAME_SHORTENED) {
			size_t copy_len = MIN((size_t)value_len, name_len - 1U);

			memcpy(name, ad->data, copy_len);
			name[copy_len] = '\0';
			net_buf_simple_restore(ad, &state);
			return;
		}

		net_buf_simple_pull(ad, value_len);
	}

	net_buf_simple_restore(ad, &state);
}

static struct scanned_device_entry *find_scan_entry(const bt_addr_le_t *addr)
{
	for (size_t i = 0; i < ARRAY_SIZE(scanned_devices); ++i) {
		if (scanned_devices[i].valid &&
		    bt_addr_le_cmp(&scanned_devices[i].addr, addr) == 0) {
			return &scanned_devices[i];
		}
	}

	return NULL;
}

static bool matches_scan_filter(const char *name)
{
	size_t filter_len = strlen(scan_filter_name);

	if (filter_len == 0U) {
		return true;
	}

	if (name == NULL || strlen(name) < filter_len) {
		return false;
	}

	return strncmp(name, scan_filter_name, filter_len) == 0;
}

static int stop_scan_internal(bool emit_complete)
{
	int err;

	if (app_state != APP_STATE_SCANNING) {
		return 0;
	}

	(void)k_work_cancel_delayable(&scan_stop_work);
	err = bt_le_scan_stop();
	if (err && err != -EALREADY) {
		return err;
	}

	set_state(current_conn != NULL ? APP_STATE_CONTROL_READY : APP_STATE_IDLE);
	if (emit_complete) {
		at_host_notify_scan_complete();
	}

	return 0;
}

static void scan_stop_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)stop_scan_internal(true);
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t type,
			 struct net_buf_simple *ad)
{
	struct scanned_device_entry *entry = NULL;
	char name[BLE_HOST_NAME_MAX_LEN];

	if (app_state != APP_STATE_SCANNING) {
		return;
	}

	if (type != BT_GAP_ADV_TYPE_ADV_IND &&
	    type != BT_GAP_ADV_TYPE_ADV_DIRECT_IND &&
	    type != BT_GAP_ADV_TYPE_SCAN_RSP &&
	    type != BT_GAP_ADV_TYPE_EXT_ADV) {
		return;
	}

	parse_device_name(ad, name, sizeof(name));
	if (!matches_scan_filter(name)) {
		return;
	}

	entry = find_scan_entry(addr);
	if (entry != NULL) {
		entry->rssi = rssi;
		return;
	}

	for (size_t i = 0; i < ARRAY_SIZE(scanned_devices); ++i) {
		if (!scanned_devices[i].valid) {
			entry = &scanned_devices[i];
			break;
		}
	}

	if (entry == NULL) {
		return;
	}

	memset(entry, 0, sizeof(*entry));
	entry->valid = true;
	entry->index = next_scan_index++;
	bt_addr_le_copy(&entry->addr, addr);
	bt_addr_to_str(&addr->a, entry->mac, sizeof(entry->mac));
	snprintk(entry->name, sizeof(entry->name), "%s", name);
	entry->rssi = rssi;

	at_host_notify_scan_result(entry->index, entry->mac, entry->name,
				   ble_host_addr_type_to_wlt(addr->type), rssi);
}

static bool parse_connect_mac(const char *text, bt_addr_t *addr)
{
	if (text == NULL || addr == NULL) {
		return false;
	}

	if (strlen(text) == 14U && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
		uint8_t bytes[6];
		unsigned int value;

		for (size_t i = 0; i < ARRAY_SIZE(bytes); ++i) {
			if (sscanf(&text[2U + (i * 2U)], "%2x", &value) != 1) {
				return false;
			}

			bytes[5U - i] = (uint8_t)value;
		}

		memcpy(addr->val, bytes, sizeof(bytes));
		return true;
	}

	return bt_addr_from_str(text, addr) == 0;
}

static void refresh_uart_rx_flow_control(void)
{
	uint32_t free_space;

	if (!device_is_ready(control_uart)) {
		return;
	}

	if (app_state != APP_STATE_DATA_MODE) {
		if (control_rx_paused) {
			control_rx_paused = false;
			uart_irq_rx_enable(control_uart);
		}
		return;
	}

	free_space = ring_space_bytes(&host_to_ble_ring);
	if (free_space < CONTROL_UART_RX_CHUNK) {
		if (!control_rx_paused) {
			control_rx_paused = true;
			stream_stats.usb_backpressure_count++;
			uart_irq_rx_disable(control_uart);
		}
		return;
	}

	if (control_rx_paused) {
		control_rx_paused = false;
		uart_irq_rx_enable(control_uart);
	}
}

static void flush_escape_candidate_as_payload(void)
{
	uint8_t pluses[3] = {'+', '+', '+'};

	if (!escape_state.active || escape_state.count == 0U) {
		return;
	}

	(void)ring_put_bytes(&host_to_ble_ring, pluses, escape_state.count);
	update_max_depth(ring_size_bytes(&host_to_ble_ring), &stream_stats.max_host_to_ble_depth);
	escape_state.active = false;
	escape_state.count = 0U;
	last_host_rx_time_ms = k_uptime_get();
	schedule_data_mode_ble_tx();
}

static void exit_data_mode(void)
{
	clear_data_mode_session(true);
	at_host_notify_data_mode(false);
	kick_uart_tx();
	refresh_uart_rx_flow_control();
}

static void escape_guard_work_handler(struct k_work *work)
{
	uint16_t guard_time_ms = ble_host_settings_get()->stream_guard_time_ms;

	ARG_UNUSED(work);

	if (!escape_state.active) {
		return;
	}

	if (escape_state.count == 3U) {
		int64_t now = k_uptime_get();

		if ((uint64_t)(now - last_host_rx_time_ms) < guard_time_ms) {
			k_work_reschedule(&escape_guard_work, K_MSEC(guard_time_ms));
			return;
		}

		exit_data_mode();
		return;
	}

	flush_escape_candidate_as_payload();
}

static void schedule_ble_write_pump(k_timeout_t delay)
{
	if (K_TIMEOUT_EQ(delay, K_NO_WAIT)) {
		(void)k_work_submit(&ble_write_work);
		return;
	}

	(void)k_work_reschedule(&ble_write_retry_work, delay);
}

static void schedule_data_mode_ble_tx(void)
{
	uint32_t available;
	uint16_t payload_len;

	if (app_state != APP_STATE_DATA_MODE || current_conn == NULL ||
	    !profile_manager_can_enter_data_mode()) {
		return;
	}

	available = ring_size_bytes(&host_to_ble_ring);
	if (available == 0U) {
		data_mode_tx_flush_due = false;
		(void)k_work_cancel_delayable(&ble_write_coalesce_work);
		return;
	}

	payload_len = profile_manager_get_payload_len();
	if (available >= payload_len) {
		data_mode_tx_flush_due = false;
		(void)k_work_cancel_delayable(&ble_write_coalesce_work);
		schedule_ble_write_pump(K_NO_WAIT);
		return;
	}

	data_mode_tx_flush_due = false;
	(void)k_work_reschedule(&ble_write_coalesce_work, K_MSEC(DATA_MODE_TX_COALESCE_MS));
}

static void handle_data_mode_rx(const uint8_t *data, size_t len)
{
	uint16_t guard_time_ms = ble_host_settings_get()->stream_guard_time_ms;

	for (size_t i = 0; i < len; ++i) {
		uint8_t ch = data[i];
		int64_t now = k_uptime_get();

		while (true) {
			if (escape_state.active) {
				(void)k_work_cancel_delayable(&escape_guard_work);
				if (ch == '+' && escape_state.count < 3U) {
					escape_state.count++;
					last_host_rx_time_ms = now;
					(void)k_work_reschedule(&escape_guard_work, K_MSEC(guard_time_ms));
					break;
				}

				flush_escape_candidate_as_payload();
			}

			if (ch == '+' && (uint64_t)(now - last_host_rx_time_ms) >= guard_time_ms) {
				escape_state.active = true;
				escape_state.count = 1U;
				last_host_rx_time_ms = now;
				(void)k_work_reschedule(&escape_guard_work, K_MSEC(guard_time_ms));
				break;
			}

			if (ring_put_bytes(&host_to_ble_ring, &ch, 1U) != 1U) {
				stream_stats.usb_backpressure_count++;
			} else {
				update_max_depth(ring_size_bytes(&host_to_ble_ring),
						 &stream_stats.max_host_to_ble_depth);
			}

			last_host_rx_time_ms = now;
			break;
		}
	}

	schedule_data_mode_ble_tx();
	refresh_uart_rx_flow_control();
}

static void ble_write_work_handler(struct k_work *work)
{
	uint8_t chunk[BLE_HOST_SEND_MAX_LEN];

	ARG_UNUSED(work);

	while (current_conn != NULL && profile_manager_can_enter_data_mode()) {
		uint32_t available;
		uint8_t *claimed = NULL;
		uint32_t claim_len;
		uint16_t payload_len = profile_manager_get_payload_len();
		int err;

		available = ring_size_bytes(&host_to_ble_ring);
		if (available == 0U) {
			data_mode_tx_flush_due = false;
			break;
		}

		if (app_state == APP_STATE_DATA_MODE && available < payload_len &&
		    !data_mode_tx_flush_due) {
			(void)k_work_reschedule(&ble_write_coalesce_work,
						K_MSEC(DATA_MODE_TX_COALESCE_MS));
			return;
		}

		k_spinlock_key_t key = k_spin_lock(&ring_lock);
		claim_len = ring_buf_get_claim(&host_to_ble_ring, &claimed, payload_len);
		claim_len = MIN(claim_len, (uint32_t)sizeof(chunk));
		if (claim_len > 0U) {
			memcpy(chunk, claimed, claim_len);
		}
		k_spin_unlock(&ring_lock, key);

		if (claim_len == 0U) {
			break;
		}

		err = profile_manager_send(chunk, claim_len);
		if (err == 0) {
			key = k_spin_lock(&ring_lock);
			(void)ring_buf_get_finish(&host_to_ble_ring, claim_len);
			k_spin_unlock(&ring_lock, key);

			stream_stats.bytes_host_to_ble += claim_len;
			stream_stats.write_ops++;
			data_mode_tx_flush_due = false;
			refresh_uart_rx_flow_control();
			continue;
		}

		if (err == -ENOMEM || err == -EAGAIN || err == -EBUSY || err == -EACCES) {
			schedule_ble_write_pump(K_MSEC(2));
			return;
		}

		return;
	}
}

static void ble_write_coalesce_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	data_mode_tx_flush_due = true;
	(void)k_work_submit(&ble_write_work);
}

static void ble_write_retry_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	(void)k_work_submit(&ble_write_work);
}

static int op_start_scan(void *context, uint16_t duration_s, const char *filter_name)
{
	int err;

	ARG_UNUSED(context);

	if (current_conn != NULL) {
		return -EALREADY;
	}

	if (app_state == APP_STATE_CONNECTING || app_state == APP_STATE_DISCONNECTING) {
		return -EBUSY;
	}

	if (app_state == APP_STATE_SCANNING) {
		return 0;
	}

	clear_scan_cache();
	scan_filter_name[0] = '\0';
	if (filter_name != NULL && filter_name[0] != '\0') {
		snprintk(scan_filter_name, sizeof(scan_filter_name), "%s", filter_name);
	}

	err = bt_le_scan_start(BT_LE_SCAN_ACTIVE, device_found);
	if (err) {
		return err;
	}

	set_state(APP_STATE_SCANNING);
	k_work_schedule(&scan_stop_work, K_SECONDS(duration_s));
	return 0;
}

static int op_connect(void *context, const char *mac, uint8_t addr_type)
{
	int err;
	uint8_t bt_addr_type;
	bt_addr_le_t peer;
	const struct ble_host_config *config = ble_host_settings_get();
	struct bt_le_conn_param conn_param = {
		.interval_min = config->conn_interval_min,
		.interval_max = config->conn_interval_max,
		.latency = config->latency,
		.timeout = config->timeout,
	};

	ARG_UNUSED(context);

	if (current_conn != NULL) {
		return -EALREADY;
	}

	if (app_state == APP_STATE_CONNECTING || app_state == APP_STATE_DISCONNECTING) {
		return -EBUSY;
	}

	if (!ble_host_addr_type_from_wlt(addr_type, &bt_addr_type)) {
		return -EINVAL;
	}

	if (!parse_connect_mac(mac, &peer.a)) {
		return -EINVAL;
	}

	peer.type = bt_addr_type;
	if (app_state == APP_STATE_SCANNING) {
		err = stop_scan_internal(false);
		if (err) {
			return err;
		}
	}

	err = bt_conn_le_create(&peer, BT_CONN_LE_CREATE_CONN, &conn_param, &current_conn);
	if (err) {
		current_conn = NULL;
		return err;
	}

	set_state(APP_STATE_CONNECTING);
	return 0;
}

static int op_disconnect(void *context, uint8_t conn_index)
{
	ARG_UNUSED(context);

	if (conn_index != 0U) {
		return -EINVAL;
	}

	if (current_conn == NULL) {
		return 0;
	}

	set_state(APP_STATE_DISCONNECTING);
	return bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

static int op_send_payload(void *context, uint8_t conn_index, const uint8_t *data, size_t len)
{
	ARG_UNUSED(context);

	if (conn_index != 0U) {
		return -EINVAL;
	}

	return profile_manager_send(data, len);
}

static void op_get_status(void *context, struct ble_host_status *status)
{
	ARG_UNUSED(context);

	memset(status, 0, sizeof(*status));
	if (current_conn == NULL) {
		return;
	}

	status->connected = true;
	status->ready = profile_manager_is_ready();
	status->conn_count = 1U;
	status->data_len = profile_manager_get_payload_len();
	bt_addr_to_str(&bt_conn_get_dst(current_conn)->a, status->mac, sizeof(status->mac));
}

static void op_get_profile(void *context, struct ble_profile *profile)
{
	const struct ble_host_config *config = ble_host_settings_get();

	ARG_UNUSED(context);

	*profile = config->active_profile;
}

static int op_set_profile(void *context, const char *service_uuid, const char *notify_uuid,
			  const char *write_uuid)
{
	int err;

	ARG_UNUSED(context);

	err = ble_host_settings_update_profile(service_uuid, notify_uuid, write_uuid);
	if (err) {
		return err;
	}

	profile_manager_set_config(ble_host_settings_get());
	return 0;
}

static int op_set_mtu(void *context, uint16_t mtu)
{
	int err;

	ARG_UNUSED(context);

	err = ble_host_settings_update_mtu(mtu);
	if (err) {
		return err;
	}

	profile_manager_set_config(ble_host_settings_get());
	return 0;
}

static uint16_t op_get_mtu(void *context)
{
	ARG_UNUSED(context);
	return ble_host_settings_get()->mtu;
}

static int op_request_mtu(void *context, uint8_t conn_index)
{
	ARG_UNUSED(context);

	if (conn_index != 0U) {
		return -EINVAL;
	}

	return profile_manager_request_mtu();
}

static int op_set_conn_params(void *context, uint8_t conn_index, uint16_t min_interval,
			      uint16_t max_interval, uint16_t latency, uint16_t timeout)
{
	int err;
	struct bt_le_conn_param param = {
		.interval_min = min_interval,
		.interval_max = max_interval,
		.latency = latency,
		.timeout = timeout,
	};

	ARG_UNUSED(context);

	if (conn_index != 0U) {
		return -EINVAL;
	}

	if (current_conn == NULL) {
		return -ENOTCONN;
	}

	err = bt_conn_le_param_update(current_conn, &param);
	if (err) {
		return err;
	}

	return ble_host_settings_update_conn_params(min_interval, max_interval, latency, timeout);
}

static int op_enter_data_mode(void *context, uint8_t conn_index)
{
	ARG_UNUSED(context);

	if (conn_index != 0U) {
		return -EINVAL;
	}

	if (current_conn == NULL || !ble_host_settings_get()->allow_data_mode) {
		return -ENOTSUP;
	}

	if (app_state == APP_STATE_DATA_MODE || data_mode_pending_enter) {
		return -EALREADY;
	}

	if (!profile_manager_can_enter_data_mode()) {
		return -EACCES;
	}

	data_mode_pending_enter = true;
	return 0;
}

static void op_get_stream_stats(void *context, struct ble_stream_stats *stats)
{
	ARG_UNUSED(context);

	*stats = stream_stats;
}

static const struct at_host_ops at_ops = {
	.start_scan = op_start_scan,
	.connect = op_connect,
	.disconnect = op_disconnect,
	.send_payload = op_send_payload,
	.get_status = op_get_status,
	.get_profile = op_get_profile,
	.set_profile = op_set_profile,
	.set_mtu = op_set_mtu,
	.get_mtu = op_get_mtu,
	.request_mtu = op_request_mtu,
	.set_conn_params = op_set_conn_params,
	.enter_data_mode = op_enter_data_mode,
	.get_stream_stats = op_get_stream_stats,
};

static void uart_send_line(void *context, const char *line)
{
	ARG_UNUSED(context);
	queue_ctrl_line(line);
}

static void uart_send_raw(void *context, const uint8_t *data, size_t len)
{
	ARG_UNUSED(context);
	queue_ctrl_bytes(data, len);
}

static const struct at_host_tx at_tx = {
	.line = uart_send_line,
	.raw = uart_send_raw,
};

static void profile_ready(void *context)
{
	ARG_UNUSED(context);

	if (app_state != APP_STATE_DATA_MODE && !data_mode_pending_enter) {
		set_state(APP_STATE_CONTROL_READY);
	}

	at_host_notify_ready();
}

static void profile_payload_received(void *context, const uint8_t *data, uint16_t len)
{
	ARG_UNUSED(context);

	stream_stats.notify_ops++;
	stream_stats.bytes_ble_to_host += len;

	if (app_state == APP_STATE_DATA_MODE) {
		queue_data_bytes(data, len);
		return;
	}

	at_host_notify_receive(data, len);
}

static void profile_mtu_updated(void *context, uint16_t payload_len)
{
	ARG_UNUSED(context);
	at_host_notify_mtu(payload_len);
}

static void profile_error(void *context, enum profile_manager_error error)
{
	ARG_UNUSED(context);
	ARG_UNUSED(error);

	if (current_conn != NULL && app_state != APP_STATE_DATA_MODE) {
		set_state(APP_STATE_CONTROL_READY);
	}

	at_host_notify_function_error();
}

static const struct profile_manager_callbacks profile_callbacks = {
	.ready = profile_ready,
	.payload_received = profile_payload_received,
	.mtu_updated = profile_mtu_updated,
	.error = profile_error,
};

static void connected(struct bt_conn *conn, uint8_t err)
{
	const struct ble_host_config *config = ble_host_settings_get();

	if (err != 0U) {
		if (current_conn != NULL) {
			bt_conn_unref(current_conn);
			current_conn = NULL;
		}
		set_state(APP_STATE_IDLE);
		at_host_notify_function_error();
		return;
	}

	set_state(APP_STATE_CONNECTING);
	at_host_notify_connected();

	if (profile_manager_start(conn) != 0) {
		at_host_notify_function_error();
		return;
	}

	if (config->auto_mtu) {
		(void)profile_manager_request_mtu();
	}

	if (config->prefer_dle) {
		(void)profile_manager_request_data_len();
	}

	if (config->prefer_2m_phy) {
		(void)profile_manager_request_phy();
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	if (reason != BT_HCI_ERR_REMOTE_USER_TERM_CONN) {
		stream_stats.unexpected_disconnect_count++;
	}

	if (current_conn == conn) {
		profile_manager_handle_disconnected(conn);
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	ring_reset_bytes(&host_to_ble_ring);
	clear_data_mode_session(false);
	set_state(APP_STATE_IDLE);
	at_host_notify_disconnected();
	refresh_uart_rx_flow_control();
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
			     uint16_t timeout)
{
	if (conn == current_conn) {
		at_host_notify_link_params(interval, latency, timeout);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.le_param_updated = le_param_updated,
};

static void activate_pending_data_mode_if_ready(void)
{
	if (!data_mode_pending_enter || app_state == APP_STATE_DATA_MODE || current_conn == NULL) {
		return;
	}

	if (ring_size_bytes(&ctrl_tx_ring) != 0U) {
		return;
	}

	data_mode_pending_enter = false;
	data_mode_tx_flush_due = false;
	escape_state.active = false;
	escape_state.count = 0U;
	(void)k_work_cancel_delayable(&ble_write_retry_work);
	(void)k_work_cancel_delayable(&ble_write_coalesce_work);
	last_host_rx_time_ms = k_uptime_get();
	stream_stats.enter_data_mode_count++;
	set_state(APP_STATE_DATA_MODE);
	refresh_uart_rx_flow_control();
}

static void service_uart_tx(const struct device *dev)
{
	struct ring_buf *ring = &ctrl_tx_ring;
	uint8_t *data = NULL;
	uint32_t available;
	int sent = 0;
	k_spinlock_key_t key;

	activate_pending_data_mode_if_ready();

	if (app_state == APP_STATE_DATA_MODE && ring_size_bytes(&ctrl_tx_ring) == 0U) {
		ring = &ble_to_host_ring;
	}

	key = k_spin_lock(&ring_lock);
	available = ring_buf_get_claim(ring, &data, UART_TX_CHUNK);
	if (available == 0U) {
		k_spin_unlock(&ring_lock, key);
		uart_irq_tx_disable(dev);
		return;
	}

	sent = uart_fifo_fill(dev, data, available);
	(void)ring_buf_get_finish(ring, sent > 0 ? (uint32_t)sent : 0U);
	k_spin_unlock(&ring_lock, key);

	if (sent <= 0) {
		uart_irq_tx_disable(dev);
		return;
	}

	activate_pending_data_mode_if_ready();
}

static void control_uart_isr(const struct device *dev, void *user_data)
{
	uint8_t buffer[CONTROL_UART_RX_CHUNK];

	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_tx_ready(dev)) {
			service_uart_tx(dev);
		}

		if (!uart_irq_rx_ready(dev)) {
			continue;
		}

		if (app_state == APP_STATE_DATA_MODE &&
		    ring_space_bytes(&host_to_ble_ring) < sizeof(buffer)) {
			control_rx_paused = true;
			stream_stats.usb_backpressure_count++;
			uart_irq_rx_disable(dev);
			continue;
		}

		int received = uart_fifo_read(dev, buffer, sizeof(buffer));
		if (received <= 0) {
			continue;
		}

		if (app_state == APP_STATE_DATA_MODE) {
			handle_data_mode_rx(buffer, (size_t)received);
		} else {
			at_host_rx_bytes(buffer, (size_t)received);
		}
	}
}

static void usb_message_cb(struct usbd_context *const ctx, const struct usbd_msg *msg)
{
	if (usbd_can_detect_vbus(ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			if (usbd_enable(ctx)) {
				LOG_ERR("Failed to enable USB");
			}
		}

		if (msg->type == USBD_MSG_VBUS_REMOVED) {
			if (usbd_disable(ctx)) {
				LOG_ERR("Failed to disable USB");
			}
		}
	}

	if (msg->type == USBD_MSG_CDC_ACM_CONTROL_LINE_STATE) {
		uint32_t dtr = 0U;

		uart_line_ctrl_get(msg->dev, UART_LINE_CTRL_DTR, &dtr);
		if (dtr == 0U) {
			control_dtr_ready = false;
			uart_irq_tx_disable(control_uart);
			return;
		}

		control_dtr_ready = true;
		k_sem_give(&usb_ready_sem);
		kick_uart_tx();
	}
}

static int enable_usb_device_next(void)
{
	int err;

	sample_usbd = sample_usbd_init_device(usb_message_cb);
	if (sample_usbd == NULL) {
		return -ENODEV;
	}

	if (!usbd_can_detect_vbus(sample_usbd)) {
		err = usbd_enable(sample_usbd);
		if (err) {
			return err;
		}
	}

	return 0;
}

static int init_control_uart(void)
{
	int err;

	if (!device_is_ready(control_uart)) {
		return -ENODEV;
	}

	err = uart_line_ctrl_set(control_uart, UART_LINE_CTRL_DCD, 1);
	if (err != 0) {
		LOG_WRN("Failed to set DCD: %d", err);
	}

	err = uart_line_ctrl_set(control_uart, UART_LINE_CTRL_DSR, 1);
	if (err != 0) {
		LOG_WRN("Failed to set DSR: %d", err);
	}

	uart_irq_callback_set(control_uart, control_uart_isr);
	uart_irq_rx_enable(control_uart);
	return 0;
}

static void bluetooth_ready(int err)
{
	if (err == 0) {
		k_sem_give(&ble_ready_sem);
	}
}

int main(void)
{
	int err;

	k_work_init_delayable(&scan_stop_work, scan_stop_work_handler);
	k_work_init(&ble_write_work, ble_write_work_handler);
	k_work_init_delayable(&ble_write_retry_work, ble_write_retry_work_handler);
	k_work_init_delayable(&ble_write_coalesce_work, ble_write_coalesce_work_handler);
	k_work_init_delayable(&escape_guard_work, escape_guard_work_handler);
	last_host_rx_time_ms = k_uptime_get();

	err = ble_host_settings_init();
	if (err) {
		LOG_ERR("ble_host_settings_init failed: %d", err);
		return 0;
	}

	at_host_init(&at_ops, &at_tx, NULL);
	profile_manager_init(ble_host_settings_get(), &profile_callbacks, NULL);

	err = enable_usb_device_next();
	if (err) {
		LOG_ERR("enable_usb_device_next failed: %d", err);
		return 0;
	}

	err = bt_enable(bluetooth_ready);
	if (err) {
		LOG_ERR("bt_enable failed: %d", err);
		return 0;
	}

	k_sem_take(&ble_ready_sem, K_FOREVER);
	k_sem_take(&usb_ready_sem, K_FOREVER);
	k_msleep(100);

	err = init_control_uart();
	if (err) {
		LOG_ERR("init_control_uart failed: %d", err);
		return 0;
	}

	set_state(APP_STATE_IDLE);

	for (;;) {
		k_sleep(K_FOREVER);
	}
}
