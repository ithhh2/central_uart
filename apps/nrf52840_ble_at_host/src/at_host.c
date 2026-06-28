#include "at_host.h"

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/sys/util.h>

enum at_error_kind {
	AT_ERROR_INVALID_COMMAND = 0,
	AT_ERROR_INVALID_PARAMETERS,
	AT_ERROR_FUNCTION_ERROR,
};

enum at_command_result {
	AT_COMMAND_RESULT_NONE = 0,
	AT_COMMAND_RESULT_ENTER_PAYLOAD,
};

struct at_host_state {
	const struct at_host_ops *ops;
	const struct at_host_tx *tx;
	void *context;
	char line_buffer[160];
	size_t line_length;
	bool line_overflow;
	bool payload_mode;
	uint8_t payload_buffer[BLE_HOST_SEND_MAX_LEN];
	size_t payload_expected;
	size_t payload_received;
	uint8_t payload_conn_index;
};

static struct at_host_state g_state;

static bool equals_ignore_case(const char *lhs, const char *rhs)
{
	while (*lhs != '\0' && *rhs != '\0') {
		if (toupper((unsigned char)*lhs) != toupper((unsigned char)*rhs)) {
			return false;
		}

		lhs++;
		rhs++;
	}

	return *lhs == '\0' && *rhs == '\0';
}

static bool starts_with_ignore_case(const char *text, const char *prefix)
{
	while (*prefix != '\0') {
		if (*text == '\0' ||
		    toupper((unsigned char)*text) != toupper((unsigned char)*prefix)) {
			return false;
		}

		text++;
		prefix++;
	}

	return true;
}

static void tx_line(const char *line)
{
	if (g_state.tx != NULL && g_state.tx->line != NULL) {
		g_state.tx->line(g_state.context, line);
	}
}

static void tx_raw(const uint8_t *data, size_t len)
{
	if (g_state.tx != NULL && g_state.tx->raw != NULL) {
		g_state.tx->raw(g_state.context, data, len);
	}
}

static void emit_error(enum at_error_kind error_kind)
{
	switch (error_kind) {
	case AT_ERROR_INVALID_COMMAND:
		tx_line("+ERR=Invalid Command");
		return;
	case AT_ERROR_INVALID_PARAMETERS:
		tx_line("+ERR=Invalid Parameters");
		return;
	default:
		tx_line("+ERR=Function Error");
		return;
	}
}

static void emit_error_from_errno(int err)
{
	if (err == -EINVAL || err == -ENOENT || err == -ERANGE || err == -EMSGSIZE) {
		emit_error(AT_ERROR_INVALID_PARAMETERS);
		return;
	}

	emit_error(AT_ERROR_FUNCTION_ERROR);
}

static void trim_whitespace(char *text)
{
	size_t len = strlen(text);
	size_t start = 0U;

	while (start < len && isspace((unsigned char)text[start])) {
		start++;
	}

	while (len > start && isspace((unsigned char)text[len - 1U])) {
		len--;
	}

	if (start > 0U) {
		memmove(text, &text[start], len - start);
	}

	text[len - start] = '\0';
}

static bool parse_uint16_value(const char *text, uint16_t *value)
{
	char *end = NULL;
	unsigned long parsed;

	if (text == NULL || value == NULL || *text == '\0') {
		return false;
	}

	parsed = strtoul(text, &end, 10);
	if (end == text || *end != '\0' || parsed > UINT16_MAX) {
		return false;
	}

	*value = (uint16_t)parsed;
	return true;
}

static bool split_args(char *text, char **args, size_t max_args, size_t *arg_count)
{
	size_t count = 0U;
	char *cursor = text;

	if (text == NULL || args == NULL || arg_count == NULL) {
		return false;
	}

	while (cursor != NULL && *cursor != '\0' && count < max_args) {
		args[count++] = cursor;
		cursor = strchr(cursor, ',');
		if (cursor == NULL) {
			break;
		}

		*cursor = '\0';
		cursor++;
	}

	*arg_count = count;
	return true;
}

static void process_send_payload(void)
{
	int err;

	err = g_state.ops->send_payload(g_state.context, g_state.payload_conn_index,
					g_state.payload_buffer, g_state.payload_expected);
	g_state.payload_mode = false;
	g_state.payload_expected = 0U;
	g_state.payload_received = 0U;

	if (err) {
		emit_error_from_errno(err);
		return;
	}

	tx_line("+OK");
}

static void handle_get_status(void)
{
	struct ble_host_status status = {0};
	char line[80];

	g_state.ops->get_status(g_state.context, &status);
	if (!status.connected) {
		tx_line("+OK=NUM,0");
		return;
	}

	snprintk(line, sizeof(line), "0:%s,0,1,%u", status.mac, status.data_len);
	tx_line(line);
	tx_line("+OK=NUM,1");
}

static void handle_get_profile(void)
{
	struct ble_profile profile;
	char line[128];

	memset(&profile, 0, sizeof(profile));
	g_state.ops->get_profile(g_state.context, &profile);
	snprintk(line, sizeof(line), "+OK=%s,%s,%s",
		 profile.service_uuid, profile.notify_uuid, profile.write_uuid);
	tx_line(line);
}

static void handle_get_mtu(void)
{
	char line[32];
	uint16_t mtu = g_state.ops->get_mtu(g_state.context);

	snprintk(line, sizeof(line), "+OK=%u", mtu);
	tx_line(line);
}

static void handle_get_stream_stats(void)
{
	struct ble_stream_stats stats = {0};
	char line[192];

	if (g_state.ops == NULL || g_state.ops->get_stream_stats == NULL) {
		emit_error(AT_ERROR_FUNCTION_ERROR);
		return;
	}

	g_state.ops->get_stream_stats(g_state.context, &stats);
	snprintk(line, sizeof(line),
		 "+OK=H2B,%u,B2H,%u,WOP,%u,NOP,%u,BP,%u,MAXTX,%u,MAXRX,%u,ENT,%u,UDISC,%u",
		 stats.bytes_host_to_ble, stats.bytes_ble_to_host, stats.write_ops,
		 stats.notify_ops, stats.usb_backpressure_count, stats.max_host_to_ble_depth,
		 stats.max_ble_to_host_depth, stats.enter_data_mode_count,
		 stats.unexpected_disconnect_count);
	tx_line(line);
}

static enum at_command_result process_command(char *line)
{
	char working[160];
	char *args[5];
	size_t arg_count = 0U;
	int err;
	uint16_t u16_value;

	snprintk(working, sizeof(working), "%s", line);
	trim_whitespace(working);
	if (working[0] == '\0') {
		return AT_COMMAND_RESULT_NONE;
	}

	if (equals_ignore_case(working, "AT")) {
		tx_line("+OK");
		return AT_COMMAND_RESULT_NONE;
	}

	if (starts_with_ignore_case(working, "AT+STARTSCAN=")) {
		char *payload = working + strlen("AT+STARTSCAN=");
		char *filter_name = NULL;

		if (!split_args(payload, args, ARRAY_SIZE(args), &arg_count) || arg_count == 0U) {
			emit_error(AT_ERROR_INVALID_PARAMETERS);
			return AT_COMMAND_RESULT_NONE;
		}

		if (!parse_uint16_value(args[0], &u16_value) || u16_value == 0U) {
			emit_error(AT_ERROR_INVALID_PARAMETERS);
			return AT_COMMAND_RESULT_NONE;
		}

		if (arg_count >= 2U && args[1][0] != '\0') {
			filter_name = args[1];
		}

		err = g_state.ops->start_scan(g_state.context, u16_value, filter_name);
		if (err) {
			emit_error_from_errno(err);
			return AT_COMMAND_RESULT_NONE;
		}

		tx_line("+OK");
		return AT_COMMAND_RESULT_NONE;
	}

	if (starts_with_ignore_case(working, "AT+STARTCON=")) {
		char *payload = working + strlen("AT+STARTCON=");
		uint16_t addr_type;

		if (!split_args(payload, args, ARRAY_SIZE(args), &arg_count) || arg_count != 2U) {
			emit_error(AT_ERROR_INVALID_PARAMETERS);
			return AT_COMMAND_RESULT_NONE;
		}

		if (!parse_uint16_value(args[1], &addr_type) || addr_type > 3U) {
			emit_error(AT_ERROR_INVALID_PARAMETERS);
			return AT_COMMAND_RESULT_NONE;
		}

		err = g_state.ops->connect(g_state.context, args[0], (uint8_t)addr_type);
		if (err) {
			emit_error_from_errno(err);
			return AT_COMMAND_RESULT_NONE;
		}

		tx_line("+OK");
		return AT_COMMAND_RESULT_NONE;
	}

	if (starts_with_ignore_case(working, "AT+DISCON=")) {
		char *payload = working + strlen("AT+DISCON=");

		if (!parse_uint16_value(payload, &u16_value) || u16_value > 255U) {
			emit_error(AT_ERROR_INVALID_PARAMETERS);
			return AT_COMMAND_RESULT_NONE;
		}

		err = g_state.ops->disconnect(g_state.context, (uint8_t)u16_value);
		if (err) {
			emit_error_from_errno(err);
			return AT_COMMAND_RESULT_NONE;
		}

		tx_line("+OK");
		return AT_COMMAND_RESULT_NONE;
	}

	if (starts_with_ignore_case(working, "AT+SEND=")) {
		char *payload = working + strlen("AT+SEND=");
		uint16_t conn_index;

		if (!split_args(payload, args, ARRAY_SIZE(args), &arg_count) || arg_count != 2U) {
			emit_error(AT_ERROR_INVALID_PARAMETERS);
			return AT_COMMAND_RESULT_NONE;
		}

		if (!parse_uint16_value(args[0], &conn_index) ||
		    !parse_uint16_value(args[1], &u16_value) ||
		    conn_index > 255U || u16_value == 0U || u16_value > BLE_HOST_SEND_MAX_LEN) {
			emit_error(AT_ERROR_INVALID_PARAMETERS);
			return AT_COMMAND_RESULT_NONE;
		}

		g_state.payload_mode = true;
		g_state.payload_expected = u16_value;
		g_state.payload_received = 0U;
		g_state.payload_conn_index = (uint8_t)conn_index;
		tx_raw((const uint8_t *)">", 1U);
		return AT_COMMAND_RESULT_ENTER_PAYLOAD;
	}

	if (equals_ignore_case(working, "AT+CONSTATUS")) {
		handle_get_status();
		return AT_COMMAND_RESULT_NONE;
	}

	if (starts_with_ignore_case(working, "AT+SETSVCUUID=")) {
		char *payload = working + strlen("AT+SETSVCUUID=");

		if (!split_args(payload, args, ARRAY_SIZE(args), &arg_count) || arg_count != 3U) {
			emit_error(AT_ERROR_INVALID_PARAMETERS);
			return AT_COMMAND_RESULT_NONE;
		}

		err = g_state.ops->set_profile(g_state.context, args[0], args[1], args[2]);
		if (err) {
			emit_error_from_errno(err);
			return AT_COMMAND_RESULT_NONE;
		}

		tx_line("+OK");
		return AT_COMMAND_RESULT_NONE;
	}

	if (equals_ignore_case(working, "AT+GETSVCUUID")) {
		handle_get_profile();
		return AT_COMMAND_RESULT_NONE;
	}

	if (starts_with_ignore_case(working, "AT+SETMTU=")) {
		char *payload = working + strlen("AT+SETMTU=");

		if (!parse_uint16_value(payload, &u16_value)) {
			emit_error(AT_ERROR_INVALID_PARAMETERS);
			return AT_COMMAND_RESULT_NONE;
		}

		err = g_state.ops->set_mtu(g_state.context, u16_value);
		if (err) {
			emit_error_from_errno(err);
			return AT_COMMAND_RESULT_NONE;
		}

		tx_line("+OK");
		return AT_COMMAND_RESULT_NONE;
	}

	if (equals_ignore_case(working, "AT+GETMTU")) {
		handle_get_mtu();
		return AT_COMMAND_RESULT_NONE;
	}

	if (starts_with_ignore_case(working, "AT+CONMTUREQ=")) {
		char *payload = working + strlen("AT+CONMTUREQ=");

		if (!parse_uint16_value(payload, &u16_value) || u16_value > 255U) {
			emit_error(AT_ERROR_INVALID_PARAMETERS);
			return AT_COMMAND_RESULT_NONE;
		}

		err = g_state.ops->request_mtu(g_state.context, (uint8_t)u16_value);
		if (err) {
			emit_error_from_errno(err);
			return AT_COMMAND_RESULT_NONE;
		}

		tx_line("+OK");
		return AT_COMMAND_RESULT_NONE;
	}

	if (starts_with_ignore_case(working, "AT+CONPARAREQ=")) {
		char *payload = working + strlen("AT+CONPARAREQ=");
		uint16_t conn_index;
		uint16_t min_interval;
		uint16_t max_interval;
		uint16_t latency;
		uint16_t timeout;

		if (!split_args(payload, args, ARRAY_SIZE(args), &arg_count) || arg_count != 5U) {
			emit_error(AT_ERROR_INVALID_PARAMETERS);
			return AT_COMMAND_RESULT_NONE;
		}

		if (!parse_uint16_value(args[0], &conn_index) ||
		    !parse_uint16_value(args[1], &min_interval) ||
		    !parse_uint16_value(args[2], &max_interval) ||
		    !parse_uint16_value(args[3], &latency) ||
		    !parse_uint16_value(args[4], &timeout) ||
		    conn_index > 255U) {
			emit_error(AT_ERROR_INVALID_PARAMETERS);
			return AT_COMMAND_RESULT_NONE;
		}

		err = g_state.ops->set_conn_params(g_state.context, (uint8_t)conn_index,
						   min_interval, max_interval, latency, timeout);
		if (err) {
			emit_error_from_errno(err);
			return AT_COMMAND_RESULT_NONE;
		}

		tx_line("+OK");
		return AT_COMMAND_RESULT_NONE;
	}

	if (starts_with_ignore_case(working, "AT+ENTERDATAMODE=")) {
		char *payload = working + strlen("AT+ENTERDATAMODE=");

		if (!parse_uint16_value(payload, &u16_value) || u16_value > 255U ||
		    g_state.ops == NULL || g_state.ops->enter_data_mode == NULL) {
			emit_error(AT_ERROR_INVALID_PARAMETERS);
			return AT_COMMAND_RESULT_NONE;
		}

		err = g_state.ops->enter_data_mode(g_state.context, (uint8_t)u16_value);
		if (err) {
			emit_error_from_errno(err);
			return AT_COMMAND_RESULT_NONE;
		}

		tx_line("+OK");
		tx_line("+IND=DATAMODE,1");
		return AT_COMMAND_RESULT_NONE;
	}

	if (equals_ignore_case(working, "AT+STREAMSTAT")) {
		handle_get_stream_stats();
		return AT_COMMAND_RESULT_NONE;
	}

	if (starts_with_ignore_case(working, "AT+")) {
		emit_error(AT_ERROR_FUNCTION_ERROR);
		return AT_COMMAND_RESULT_NONE;
	}

	emit_error(AT_ERROR_INVALID_COMMAND);
	return AT_COMMAND_RESULT_NONE;
}

void at_host_init(const struct at_host_ops *ops, const struct at_host_tx *tx, void *context)
{
	memset(&g_state, 0, sizeof(g_state));
	g_state.ops = ops;
	g_state.tx = tx;
	g_state.context = context;
}

void at_host_rx_bytes(const uint8_t *data, size_t len)
{
	bool skip_payload_line_ending = false;

	for (size_t i = 0; i < len; ++i) {
		uint8_t ch = data[i];

		if (g_state.payload_mode) {
			if (skip_payload_line_ending && g_state.payload_received == 0U &&
			    (ch == '\r' || ch == '\n')) {
				continue;
			}

			skip_payload_line_ending = false;
			g_state.payload_buffer[g_state.payload_received++] = ch;
			if (g_state.payload_received == g_state.payload_expected) {
				process_send_payload();
			}
			continue;
		}

		if (ch == '\r' || ch == '\n') {
			if (!g_state.line_overflow && g_state.line_length > 0U) {
				enum at_command_result result;

				g_state.line_buffer[g_state.line_length] = '\0';
				result = process_command(g_state.line_buffer);
				skip_payload_line_ending = (result == AT_COMMAND_RESULT_ENTER_PAYLOAD);
			} else if (g_state.line_overflow) {
				emit_error(AT_ERROR_INVALID_COMMAND);
			}

			g_state.line_length = 0U;
			g_state.line_overflow = false;
			continue;
		}

		if (g_state.line_length >= sizeof(g_state.line_buffer) - 1U) {
			g_state.line_overflow = true;
			continue;
		}

		g_state.line_buffer[g_state.line_length++] = (char)ch;
	}
}

void at_host_notify_scan_result(uint8_t index, const char *mac, const char *name,
				uint8_t addr_type, int8_t rssi)
{
	char line[128];

	snprintk(line, sizeof(line), "+IND=SCDA,%u,%s,%s,%u,%d",
		 index, mac, name != NULL ? name : "", addr_type, rssi);
	tx_line(line);
}

void at_host_notify_scan_complete(void)
{
	tx_line("+IND=SCED");
}

void at_host_notify_connected(void)
{
	tx_line("+IND=BLMC,0");
}

void at_host_notify_disconnected(void)
{
	tx_line("+IND=BLMD,0");
}

void at_host_notify_ready(void)
{
	tx_line("+IND=BLMN,0");
}

void at_host_notify_mtu(uint16_t payload_len)
{
	char line[32];

	snprintk(line, sizeof(line), "+IND=MTU,%u", payload_len);
	tx_line(line);
}

void at_host_notify_receive(const uint8_t *data, uint16_t len)
{
	char line[BLE_HOST_SEND_MAX_LEN + 32];

	if (!ble_host_is_printable_ascii(data, len) || len > BLE_HOST_SEND_MAX_LEN) {
		/*
		 * Control mode only exposes printable WLT8016-style text payloads.
		 * Binary notify traffic can continue briefly after leaving data mode;
		 * drop it silently so the AT control plane stays usable.
		 */
		return;
	}

	snprintk(line, sizeof(line), "+IND=RECV,0,%u,%.*s", len, len, data);
	tx_line(line);
}

void at_host_notify_link_params(uint16_t interval, uint16_t latency, uint16_t timeout)
{
	char line[64];

	snprintk(line, sizeof(line), "+IND=LINKPARA,0,%u,%u,%u", interval, latency, timeout);
	tx_line(line);
}

void at_host_notify_data_mode(bool enabled)
{
	tx_line(enabled ? "+IND=DATAMODE,1" : "+IND=DATAMODE,0");
}

void at_host_notify_function_error(void)
{
	emit_error(AT_ERROR_FUNCTION_ERROR);
}
