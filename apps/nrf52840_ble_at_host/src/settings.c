#include "settings.h"

#include <errno.h>
#include <string.h>

#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#define SETTINGS_ROOT "ble_at_host"
#define SETTINGS_KEY_SERVICE SETTINGS_ROOT "/profile/service_uuid"
#define SETTINGS_KEY_NOTIFY SETTINGS_ROOT "/profile/notify_uuid"
#define SETTINGS_KEY_WRITE SETTINGS_ROOT "/profile/write_uuid"
#define SETTINGS_KEY_MTU SETTINGS_ROOT "/mtu"
#define SETTINGS_KEY_CONN_MIN SETTINGS_ROOT "/conn_interval_min"
#define SETTINGS_KEY_CONN_MAX SETTINGS_ROOT "/conn_interval_max"
#define SETTINGS_KEY_LATENCY SETTINGS_ROOT "/latency"
#define SETTINGS_KEY_TIMEOUT SETTINGS_ROOT "/timeout"
#define SETTINGS_KEY_AUTO_MTU SETTINGS_ROOT "/auto_mtu"
#define SETTINGS_KEY_AUTO_SUBSCRIBE SETTINGS_ROOT "/auto_subscribe"
#define SETTINGS_KEY_PREFER_2M_PHY SETTINGS_ROOT "/prefer_2m_phy"
#define SETTINGS_KEY_PREFER_DLE SETTINGS_ROOT "/prefer_dle"
#define SETTINGS_KEY_ALLOW_DATA_MODE SETTINGS_ROOT "/allow_data_mode"
#define SETTINGS_KEY_STREAM_GUARD SETTINGS_ROOT "/stream_guard_time_ms"

static struct ble_host_config g_config = {
	.active_profile = {
		.service_uuid = "0xFFF0",
		.notify_uuid = "0xFFF1",
		.write_uuid = "0xFFF2",
	},
	.mtu = 259U,
	.conn_interval_min = 6U,
	.conn_interval_max = 9U,
	.latency = 0U,
	.timeout = 400U,
	.auto_mtu = true,
	.auto_subscribe = true,
	.prefer_2m_phy = true,
	.prefer_dle = true,
	.allow_data_mode = true,
	.stream_guard_time_ms = 1000U,
};

static int settings_handle_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	char text[BLE_HOST_UUID_STR_LEN];
	int rc;

	if (settings_name_steq(name, "profile/service_uuid", NULL)) {
		if (len == 0U || len >= sizeof(text)) {
			return -EINVAL;
		}

		rc = read_cb(cb_arg, text, len);
		if (rc < 0) {
			return rc;
		}

		text[len] = '\0';
		return ble_host_normalize_uuid(text, g_config.active_profile.service_uuid,
					       sizeof(g_config.active_profile.service_uuid)) ? 0 : -EINVAL;
	}

	if (settings_name_steq(name, "profile/notify_uuid", NULL)) {
		if (len == 0U || len >= sizeof(text)) {
			return -EINVAL;
		}

		rc = read_cb(cb_arg, text, len);
		if (rc < 0) {
			return rc;
		}

		text[len] = '\0';
		return ble_host_normalize_uuid(text, g_config.active_profile.notify_uuid,
					       sizeof(g_config.active_profile.notify_uuid)) ? 0 : -EINVAL;
	}

	if (settings_name_steq(name, "profile/write_uuid", NULL)) {
		if (len == 0U || len >= sizeof(text)) {
			return -EINVAL;
		}

		rc = read_cb(cb_arg, text, len);
		if (rc < 0) {
			return rc;
		}

		text[len] = '\0';
		return ble_host_normalize_uuid(text, g_config.active_profile.write_uuid,
					       sizeof(g_config.active_profile.write_uuid)) ? 0 : -EINVAL;
	}

	if (settings_name_steq(name, "mtu", NULL)) {
		return read_cb(cb_arg, &g_config.mtu, sizeof(g_config.mtu)) >= 0 ? 0 : -EINVAL;
	}

	if (settings_name_steq(name, "conn_interval_min", NULL)) {
		return read_cb(cb_arg, &g_config.conn_interval_min,
			       sizeof(g_config.conn_interval_min)) >= 0 ? 0 : -EINVAL;
	}

	if (settings_name_steq(name, "conn_interval_max", NULL)) {
		return read_cb(cb_arg, &g_config.conn_interval_max,
			       sizeof(g_config.conn_interval_max)) >= 0 ? 0 : -EINVAL;
	}

	if (settings_name_steq(name, "latency", NULL)) {
		return read_cb(cb_arg, &g_config.latency, sizeof(g_config.latency)) >= 0 ? 0 : -EINVAL;
	}

	if (settings_name_steq(name, "timeout", NULL)) {
		return read_cb(cb_arg, &g_config.timeout, sizeof(g_config.timeout)) >= 0 ? 0 : -EINVAL;
	}

	if (settings_name_steq(name, "auto_mtu", NULL)) {
		return read_cb(cb_arg, &g_config.auto_mtu, sizeof(g_config.auto_mtu)) >= 0 ? 0 : -EINVAL;
	}

	if (settings_name_steq(name, "auto_subscribe", NULL)) {
		return read_cb(cb_arg, &g_config.auto_subscribe,
			       sizeof(g_config.auto_subscribe)) >= 0 ? 0 : -EINVAL;
	}

	if (settings_name_steq(name, "prefer_2m_phy", NULL)) {
		return read_cb(cb_arg, &g_config.prefer_2m_phy,
			       sizeof(g_config.prefer_2m_phy)) >= 0 ? 0 : -EINVAL;
	}

	if (settings_name_steq(name, "prefer_dle", NULL)) {
		return read_cb(cb_arg, &g_config.prefer_dle,
			       sizeof(g_config.prefer_dle)) >= 0 ? 0 : -EINVAL;
	}

	if (settings_name_steq(name, "allow_data_mode", NULL)) {
		return read_cb(cb_arg, &g_config.allow_data_mode,
			       sizeof(g_config.allow_data_mode)) >= 0 ? 0 : -EINVAL;
	}

	if (settings_name_steq(name, "stream_guard_time_ms", NULL)) {
		return read_cb(cb_arg, &g_config.stream_guard_time_ms,
			       sizeof(g_config.stream_guard_time_ms)) >= 0 ? 0 : -EINVAL;
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(ble_at_host_cfg, SETTINGS_ROOT, NULL, settings_handle_set, NULL, NULL);

static bool conn_params_valid(uint16_t min_interval, uint16_t max_interval,
			      uint16_t latency, uint16_t timeout)
{
	if (min_interval < 6U || min_interval > 800U) {
		return false;
	}

	if (max_interval < 6U || max_interval > 800U || min_interval > max_interval) {
		return false;
	}

	if (timeout == 0U) {
		return false;
	}

	ARG_UNUSED(latency);
	return true;
}

int ble_host_settings_init(void)
{
	int err;

	err = settings_subsys_init();
	if (err) {
		return err;
	}

	err = settings_load_subtree(SETTINGS_ROOT);
	if (err) {
		return err;
	}

	return 0;
}

const struct ble_host_config *ble_host_settings_get(void)
{
	return &g_config;
}

int ble_host_settings_update_profile(const char *service_uuid, const char *notify_uuid,
				     const char *write_uuid)
{
	char service_norm[BLE_HOST_UUID_STR_LEN];
	char notify_norm[BLE_HOST_UUID_STR_LEN];
	char write_norm[BLE_HOST_UUID_STR_LEN];
	int err;

	if (!ble_host_normalize_uuid(service_uuid, service_norm, sizeof(service_norm)) ||
	    !ble_host_normalize_uuid(notify_uuid, notify_norm, sizeof(notify_norm)) ||
	    !ble_host_normalize_uuid(write_uuid, write_norm, sizeof(write_norm))) {
		return -EINVAL;
	}

	snprintk(g_config.active_profile.service_uuid, sizeof(g_config.active_profile.service_uuid),
		 "%s", service_norm);
	snprintk(g_config.active_profile.notify_uuid, sizeof(g_config.active_profile.notify_uuid),
		 "%s", notify_norm);
	snprintk(g_config.active_profile.write_uuid, sizeof(g_config.active_profile.write_uuid),
		 "%s", write_norm);

	err = settings_save_one(SETTINGS_KEY_SERVICE, g_config.active_profile.service_uuid,
				strlen(g_config.active_profile.service_uuid));
	if (err) {
		return err;
	}

	err = settings_save_one(SETTINGS_KEY_NOTIFY, g_config.active_profile.notify_uuid,
				strlen(g_config.active_profile.notify_uuid));
	if (err) {
		return err;
	}

	return settings_save_one(SETTINGS_KEY_WRITE, g_config.active_profile.write_uuid,
				 strlen(g_config.active_profile.write_uuid));
}

int ble_host_settings_update_mtu(uint16_t mtu)
{
	if (mtu < 23U || mtu > 259U) {
		return -EINVAL;
	}

	g_config.mtu = mtu;
	return settings_save_one(SETTINGS_KEY_MTU, &g_config.mtu, sizeof(g_config.mtu));
}

int ble_host_settings_update_conn_params(uint16_t min_interval, uint16_t max_interval,
					 uint16_t latency, uint16_t timeout)
{
	int err;

	if (!conn_params_valid(min_interval, max_interval, latency, timeout)) {
		return -EINVAL;
	}

	g_config.conn_interval_min = min_interval;
	g_config.conn_interval_max = max_interval;
	g_config.latency = latency;
	g_config.timeout = timeout;

	err = settings_save_one(SETTINGS_KEY_CONN_MIN, &g_config.conn_interval_min,
				sizeof(g_config.conn_interval_min));
	if (err) {
		return err;
	}

	err = settings_save_one(SETTINGS_KEY_CONN_MAX, &g_config.conn_interval_max,
				sizeof(g_config.conn_interval_max));
	if (err) {
		return err;
	}

	err = settings_save_one(SETTINGS_KEY_LATENCY, &g_config.latency, sizeof(g_config.latency));
	if (err) {
		return err;
	}

	return settings_save_one(SETTINGS_KEY_TIMEOUT, &g_config.timeout, sizeof(g_config.timeout));
}
