#include "profile_manager.h"

#include <string.h>

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/util.h>

#include "app_common.h"

struct profile_manager_state {
	const struct ble_host_config *config;
	const struct profile_manager_callbacks *callbacks;
	void *context;
	struct bt_conn *conn;
	struct bt_gatt_discover_params service_discover_params;
	struct bt_gatt_discover_params characteristic_discover_params;
	struct bt_gatt_discover_params ccc_discover_params;
	struct bt_gatt_subscribe_params subscribe_params;
	struct bt_gatt_exchange_params exchange_params;
	struct bt_gatt_write_params write_params;
	uint16_t service_start_handle;
	uint16_t service_end_handle;
	uint16_t notify_value_handle;
	uint16_t write_value_handle;
	uint8_t write_properties;
	bool ready;
	bool service_found;
	bool notify_found;
	bool write_found;
	bool exchange_in_progress;
	bool write_in_progress;
	uint16_t payload_len;
	uint8_t pending_write[BLE_HOST_SEND_MAX_LEN];
	size_t pending_write_len;
};

static struct profile_manager_state g_state;

static void report_error(enum profile_manager_error error)
{
	if (g_state.callbacks != NULL && g_state.callbacks->error != NULL) {
		g_state.callbacks->error(g_state.context, error);
	}
}

static void update_payload_len(void)
{
	uint16_t att_mtu = 23U;
	uint16_t preferred = 23U;

	if (g_state.conn != NULL) {
		att_mtu = bt_gatt_get_mtu(g_state.conn);
	}

	if (g_state.config != NULL) {
		preferred = g_state.config->mtu;
	}

	if (preferred < 23U) {
		preferred = 23U;
	}

	att_mtu = MIN(att_mtu, preferred);
	g_state.payload_len = att_mtu > 3U ? (uint16_t)(att_mtu - 3U) : 20U;
}

static uint8_t notify_handler(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
			      const void *data, uint16_t length)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	if (data == NULL) {
		g_state.ready = false;
		return BT_GATT_ITER_STOP;
	}

	if (g_state.callbacks != NULL && g_state.callbacks->payload_received != NULL) {
		g_state.callbacks->payload_received(g_state.context, data, length);
	}

	return BT_GATT_ITER_CONTINUE;
}

static void subscribe_handler(struct bt_conn *conn, uint8_t err,
			      struct bt_gatt_subscribe_params *params)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	if (err != 0U) {
		g_state.ready = false;
		report_error(PROFILE_MANAGER_ERROR_SUBSCRIBE_FAILED);
		return;
	}

	g_state.ready = true;
	if (g_state.callbacks != NULL && g_state.callbacks->ready != NULL) {
		g_state.callbacks->ready(g_state.context);
	}
}

static int subscribe_notify_characteristic(void)
{
	int err;

	memset(&g_state.subscribe_params, 0, sizeof(g_state.subscribe_params));
	g_state.subscribe_params.value_handle = g_state.notify_value_handle;
	g_state.subscribe_params.ccc_handle = BT_GATT_AUTO_DISCOVER_CCC_HANDLE;
	g_state.subscribe_params.end_handle = g_state.service_end_handle;
	g_state.subscribe_params.disc_params = &g_state.ccc_discover_params;
	g_state.subscribe_params.value = BT_GATT_CCC_NOTIFY;
	g_state.subscribe_params.notify = notify_handler;
	g_state.subscribe_params.subscribe = subscribe_handler;

	err = bt_gatt_subscribe(g_state.conn, &g_state.subscribe_params);
	if (err) {
		report_error(PROFILE_MANAGER_ERROR_SUBSCRIBE_FAILED);
		return err;
	}

	return 0;
}

static uint8_t characteristic_discover_cb(struct bt_conn *conn,
					  const struct bt_gatt_attr *attr,
					  struct bt_gatt_discover_params *params)
{
	char uuid_text[BLE_HOST_UUID_STR_LEN];

	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	if (attr == NULL) {
		if (!g_state.notify_found) {
			report_error(PROFILE_MANAGER_ERROR_NOTIFY_MISSING);
			return BT_GATT_ITER_STOP;
		}

		if (!g_state.write_found) {
			report_error(PROFILE_MANAGER_ERROR_WRITE_MISSING);
			return BT_GATT_ITER_STOP;
		}

		if (g_state.config != NULL && g_state.config->auto_subscribe) {
			(void)subscribe_notify_characteristic();
		} else {
			g_state.ready = true;
			if (g_state.callbacks != NULL && g_state.callbacks->ready != NULL) {
				g_state.callbacks->ready(g_state.context);
			}
		}

		return BT_GATT_ITER_STOP;
	}

	if (params->type != BT_GATT_DISCOVER_CHARACTERISTIC) {
		report_error(PROFILE_MANAGER_ERROR_DISCOVER_FAILED);
		return BT_GATT_ITER_STOP;
	}

	const struct bt_gatt_chrc *chrc = attr->user_data;

	ble_host_format_uuid_from_bt(chrc->uuid, uuid_text, sizeof(uuid_text));
	if (!g_state.notify_found &&
	    ble_host_uuid_equals(uuid_text, g_state.config->active_profile.notify_uuid)) {
		g_state.notify_found = true;
		g_state.notify_value_handle = chrc->value_handle;
	}

	if (!g_state.write_found &&
	    ble_host_uuid_equals(uuid_text, g_state.config->active_profile.write_uuid)) {
		g_state.write_found = true;
		g_state.write_value_handle = chrc->value_handle;
		g_state.write_properties = chrc->properties;
	}

	return BT_GATT_ITER_CONTINUE;
}

static int start_characteristic_discovery(void)
{
	int err;

	memset(&g_state.characteristic_discover_params, 0, sizeof(g_state.characteristic_discover_params));
	g_state.characteristic_discover_params.uuid = NULL;
	g_state.characteristic_discover_params.func = characteristic_discover_cb;
	g_state.characteristic_discover_params.start_handle = g_state.service_start_handle;
	g_state.characteristic_discover_params.end_handle = g_state.service_end_handle;
	g_state.characteristic_discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

	err = bt_gatt_discover(g_state.conn, &g_state.characteristic_discover_params);
	if (err) {
		report_error(PROFILE_MANAGER_ERROR_DISCOVER_FAILED);
	}

	return err;
}

static uint8_t service_discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				   struct bt_gatt_discover_params *params)
{
	char uuid_text[BLE_HOST_UUID_STR_LEN];

	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	if (attr == NULL) {
		if (!g_state.service_found) {
			report_error(PROFILE_MANAGER_ERROR_SERVICE_MISSING);
			return BT_GATT_ITER_STOP;
		}

		(void)start_characteristic_discovery();
		return BT_GATT_ITER_STOP;
	}

	if (params->type != BT_GATT_DISCOVER_PRIMARY) {
		report_error(PROFILE_MANAGER_ERROR_DISCOVER_FAILED);
		return BT_GATT_ITER_STOP;
	}

	const struct bt_gatt_service_val *service = attr->user_data;

	ble_host_format_uuid_from_bt(service->uuid, uuid_text, sizeof(uuid_text));
	if (ble_host_uuid_equals(uuid_text, g_state.config->active_profile.service_uuid)) {
		g_state.service_found = true;
		g_state.service_start_handle = attr->handle;
		g_state.service_end_handle = service->end_handle;
	}

	return BT_GATT_ITER_CONTINUE;
}

static void exchange_handler(struct bt_conn *conn, uint8_t err,
			     struct bt_gatt_exchange_params *params)
{
	ARG_UNUSED(conn);

	g_state.exchange_in_progress = false;
	__ASSERT_NO_MSG(params == &g_state.exchange_params);

	if (err == 0U || err == BT_ATT_ERR_SUCCESS) {
		update_payload_len();
		if (g_state.callbacks != NULL && g_state.callbacks->mtu_updated != NULL) {
			g_state.callbacks->mtu_updated(g_state.context, g_state.payload_len);
		}
	}

	memset(params, 0, sizeof(*params));
}

static void write_handler(struct bt_conn *conn, uint8_t err,
			  struct bt_gatt_write_params *params)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(params);

	g_state.write_in_progress = false;
	if (err != 0U) {
		report_error(PROFILE_MANAGER_ERROR_WRITE_FAILED);
	}
}

void profile_manager_init(const struct ble_host_config *config,
			  const struct profile_manager_callbacks *callbacks, void *context)
{
	memset(&g_state, 0, sizeof(g_state));
	g_state.config = config;
	g_state.callbacks = callbacks;
	g_state.context = context;
	update_payload_len();
}

void profile_manager_set_config(const struct ble_host_config *config)
{
	g_state.config = config;
	update_payload_len();
}

void profile_manager_reset(void)
{
	if (g_state.conn != NULL) {
		(void)bt_gatt_unsubscribe(g_state.conn, &g_state.subscribe_params);
	}

	memset(&g_state.service_discover_params, 0, sizeof(g_state.service_discover_params));
	memset(&g_state.characteristic_discover_params, 0, sizeof(g_state.characteristic_discover_params));
	memset(&g_state.ccc_discover_params, 0, sizeof(g_state.ccc_discover_params));
	memset(&g_state.subscribe_params, 0, sizeof(g_state.subscribe_params));
	memset(&g_state.exchange_params, 0, sizeof(g_state.exchange_params));
	memset(&g_state.write_params, 0, sizeof(g_state.write_params));
	g_state.service_start_handle = 0U;
	g_state.service_end_handle = 0U;
	g_state.notify_value_handle = 0U;
	g_state.write_value_handle = 0U;
	g_state.write_properties = 0U;
	g_state.ready = false;
	g_state.service_found = false;
	g_state.notify_found = false;
	g_state.write_found = false;
	g_state.exchange_in_progress = false;
	g_state.write_in_progress = false;
	g_state.pending_write_len = 0U;
	update_payload_len();
}

int profile_manager_start(struct bt_conn *conn)
{
	int err;

	if (conn == NULL || g_state.config == NULL) {
		return -EINVAL;
	}

	profile_manager_reset();
	g_state.conn = conn;
	update_payload_len();

	memset(&g_state.service_discover_params, 0, sizeof(g_state.service_discover_params));
	g_state.service_discover_params.uuid = NULL;
	g_state.service_discover_params.func = service_discover_cb;
	g_state.service_discover_params.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
	g_state.service_discover_params.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
	g_state.service_discover_params.type = BT_GATT_DISCOVER_PRIMARY;

	err = bt_gatt_discover(g_state.conn, &g_state.service_discover_params);
	if (err) {
		report_error(PROFILE_MANAGER_ERROR_DISCOVER_FAILED);
	}

	return err;
}

void profile_manager_handle_disconnected(struct bt_conn *conn)
{
	if (g_state.conn == conn) {
		profile_manager_reset();
		g_state.conn = NULL;
	}
}

int profile_manager_request_mtu(void)
{
	int err;

	if (g_state.conn == NULL) {
		return -ENOTCONN;
	}

	if (g_state.exchange_in_progress) {
		return -EBUSY;
	}

	g_state.exchange_params.func = exchange_handler;
	g_state.exchange_in_progress = true;

	err = bt_gatt_exchange_mtu(g_state.conn, &g_state.exchange_params);
	if (err == -EALREADY) {
		g_state.exchange_in_progress = false;
		memset(&g_state.exchange_params, 0, sizeof(g_state.exchange_params));
		update_payload_len();
		if (g_state.callbacks != NULL && g_state.callbacks->mtu_updated != NULL) {
			g_state.callbacks->mtu_updated(g_state.context, g_state.payload_len);
		}
		return 0;
	}

	if (err) {
		g_state.exchange_in_progress = false;
		memset(&g_state.exchange_params, 0, sizeof(g_state.exchange_params));
		return err;
	}

	return 0;
}

int profile_manager_request_data_len(void)
{
	if (g_state.conn == NULL) {
		return -ENOTCONN;
	}

	if (!IS_ENABLED(CONFIG_BT_USER_DATA_LEN_UPDATE)) {
		return -ENOTSUP;
	}

	return bt_conn_le_data_len_update(g_state.conn, BT_LE_DATA_LEN_PARAM_MAX);
}

int profile_manager_request_phy(void)
{
	if (g_state.conn == NULL) {
		return -ENOTCONN;
	}

	if (!IS_ENABLED(CONFIG_BT_USER_PHY_UPDATE)) {
		return -ENOTSUP;
	}

	return bt_conn_le_phy_update(g_state.conn, BT_CONN_LE_PHY_PARAM_2M);
}

int profile_manager_send(const uint8_t *data, size_t len)
{
	int err;

	if (g_state.conn == NULL) {
		return -ENOTCONN;
	}

	if (!g_state.ready || !g_state.write_found || g_state.write_value_handle == 0U) {
		return -EAGAIN;
	}

	if (len == 0U || len > BLE_HOST_SEND_MAX_LEN || len > g_state.payload_len) {
		return -EMSGSIZE;
	}

	if (g_state.write_properties & BT_GATT_CHRC_WRITE_WITHOUT_RESP) {
		return bt_gatt_write_without_response(g_state.conn, g_state.write_value_handle, data, len,
						      false);
	}

	if ((g_state.write_properties & BT_GATT_CHRC_WRITE) == 0U || g_state.write_in_progress) {
		return -EACCES;
	}

	memcpy(g_state.pending_write, data, len);
	g_state.pending_write_len = len;
	memset(&g_state.write_params, 0, sizeof(g_state.write_params));
	g_state.write_params.func = write_handler;
	g_state.write_params.handle = g_state.write_value_handle;
	g_state.write_params.offset = 0U;
	g_state.write_params.data = g_state.pending_write;
	g_state.write_params.length = g_state.pending_write_len;
	g_state.write_in_progress = true;

	err = bt_gatt_write(g_state.conn, &g_state.write_params);
	if (err) {
		g_state.write_in_progress = false;
	}

	return err;
}

bool profile_manager_is_ready(void)
{
	return g_state.ready;
}

bool profile_manager_can_enter_data_mode(void)
{
	return g_state.ready &&
	       g_state.write_found &&
	       g_state.write_value_handle != 0U &&
	       (g_state.write_properties & BT_GATT_CHRC_WRITE_WITHOUT_RESP) != 0U;
}

uint16_t profile_manager_get_payload_len(void)
{
	return g_state.payload_len;
}
