#include "app_common.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool is_hex_text(const char *text, size_t len)
{
	for (size_t i = 0; i < len; ++i) {
		if (!isxdigit((unsigned char)text[i])) {
			return false;
		}
	}

	return true;
}

static bool normalize_uuid16(const char *text, char *output, size_t output_len)
{
	char digits[5];
	size_t offset = 0U;
	size_t len = strlen(text);

	if (len >= 2U && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
		offset = 2U;
		len -= 2U;
	}

	if (len != 4U || !is_hex_text(text + offset, len) || output_len < 7U) {
		return false;
	}

	memcpy(digits, text + offset, 4U);
	digits[4] = '\0';
	snprintk(output, output_len, "0x%c%c%c%c",
		 (char)toupper((unsigned char)digits[0]),
		 (char)toupper((unsigned char)digits[1]),
		 (char)toupper((unsigned char)digits[2]),
		 (char)toupper((unsigned char)digits[3]));
	return true;
}

static bool normalize_uuid128_from_hex32(const char *hex32, char *output, size_t output_len)
{
	if (strlen(hex32) != 32U || !is_hex_text(hex32, 32U) || output_len < 37U) {
		return false;
	}

	snprintk(output, output_len,
		 "%c%c%c%c%c%c%c%c-%c%c%c%c-%c%c%c%c-%c%c%c%c-%c%c%c%c%c%c%c%c%c%c%c%c",
		 (char)tolower((unsigned char)hex32[0]), (char)tolower((unsigned char)hex32[1]),
		 (char)tolower((unsigned char)hex32[2]), (char)tolower((unsigned char)hex32[3]),
		 (char)tolower((unsigned char)hex32[4]), (char)tolower((unsigned char)hex32[5]),
		 (char)tolower((unsigned char)hex32[6]), (char)tolower((unsigned char)hex32[7]),
		 (char)tolower((unsigned char)hex32[8]), (char)tolower((unsigned char)hex32[9]),
		 (char)tolower((unsigned char)hex32[10]), (char)tolower((unsigned char)hex32[11]),
		 (char)tolower((unsigned char)hex32[12]), (char)tolower((unsigned char)hex32[13]),
		 (char)tolower((unsigned char)hex32[14]), (char)tolower((unsigned char)hex32[15]),
		 (char)tolower((unsigned char)hex32[16]), (char)tolower((unsigned char)hex32[17]),
		 (char)tolower((unsigned char)hex32[18]), (char)tolower((unsigned char)hex32[19]),
		 (char)tolower((unsigned char)hex32[20]), (char)tolower((unsigned char)hex32[21]),
		 (char)tolower((unsigned char)hex32[22]), (char)tolower((unsigned char)hex32[23]),
		 (char)tolower((unsigned char)hex32[24]), (char)tolower((unsigned char)hex32[25]),
		 (char)tolower((unsigned char)hex32[26]), (char)tolower((unsigned char)hex32[27]),
		 (char)tolower((unsigned char)hex32[28]), (char)tolower((unsigned char)hex32[29]),
		 (char)tolower((unsigned char)hex32[30]), (char)tolower((unsigned char)hex32[31]));
	return true;
}

bool ble_host_normalize_uuid(const char *input, char *output, size_t output_len)
{
	char compact[33];
	size_t compact_len = 0U;
	size_t len;

	if (input == NULL || output == NULL || output_len == 0U) {
		return false;
	}

	while (*input != '\0' && isspace((unsigned char)*input)) {
		input++;
	}

	len = strlen(input);
	while (len > 0U && isspace((unsigned char)input[len - 1U])) {
		len--;
	}

	if (len == 0U) {
		return false;
	}

	if (len <= 6U && normalize_uuid16(input, output, output_len)) {
		return true;
	}

	for (size_t i = 0; i < len; ++i) {
		unsigned char ch = (unsigned char)input[i];

		if (ch == '-') {
			continue;
		}

		if (!isxdigit(ch) || compact_len >= sizeof(compact) - 1U) {
			return false;
		}

		compact[compact_len++] = (char)ch;
	}

	compact[compact_len] = '\0';
	if (compact_len != 32U) {
		return false;
	}

	return normalize_uuid128_from_hex32(compact, output, output_len);
}

void ble_host_format_uuid_from_bt(const struct bt_uuid *uuid, char *output, size_t output_len)
{
	char raw[BT_UUID_STR_LEN];

	if (uuid == NULL || output == NULL || output_len == 0U) {
		return;
	}

	bt_uuid_to_str(uuid, raw, sizeof(raw));
	if (!ble_host_normalize_uuid(raw, output, output_len)) {
		snprintk(output, output_len, "%s", raw);
	}
}

bool ble_host_uuid_equals(const char *lhs, const char *rhs)
{
	char lhs_norm[BLE_HOST_UUID_STR_LEN];
	char rhs_norm[BLE_HOST_UUID_STR_LEN];

	if (!ble_host_normalize_uuid(lhs, lhs_norm, sizeof(lhs_norm)) ||
	    !ble_host_normalize_uuid(rhs, rhs_norm, sizeof(rhs_norm))) {
		return false;
	}

	return strcmp(lhs_norm, rhs_norm) == 0;
}

uint8_t ble_host_addr_type_to_wlt(uint8_t bt_addr_type)
{
	switch (bt_addr_type) {
	case BT_ADDR_LE_PUBLIC:
		return 0U;
	case BT_ADDR_LE_RANDOM:
		return 1U;
	case BT_ADDR_LE_PUBLIC_ID:
		return 2U;
	case BT_ADDR_LE_RANDOM_ID:
		return 3U;
	default:
		return 1U;
	}
}

bool ble_host_addr_type_from_wlt(uint8_t wlt_addr_type, uint8_t *bt_addr_type)
{
	if (bt_addr_type == NULL) {
		return false;
	}

	switch (wlt_addr_type) {
	case 0U:
		*bt_addr_type = BT_ADDR_LE_PUBLIC;
		return true;
	case 1U:
		*bt_addr_type = BT_ADDR_LE_RANDOM;
		return true;
	case 2U:
		*bt_addr_type = BT_ADDR_LE_PUBLIC_ID;
		return true;
	case 3U:
		*bt_addr_type = BT_ADDR_LE_RANDOM_ID;
		return true;
	default:
		return false;
	}
}

bool ble_host_is_printable_ascii(const uint8_t *data, size_t len)
{
	if (data == NULL) {
		return false;
	}

	for (size_t i = 0; i < len; ++i) {
		if (data[i] < 0x20U || data[i] > 0x7eU) {
			return false;
		}
	}

	return true;
}
