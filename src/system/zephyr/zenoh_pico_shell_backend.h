#ifndef ZENOH_PICO_SHELL_BACKEND_H_
#define ZENOH_PICO_SHELL_BACKEND_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZP_ZEPHYR_ZENOH_SHELL_MAX_INFO_IDS   4U
#define ZP_ZEPHYR_ZENOH_SHELL_MAX_ID_STR_LEN 33U

enum zp_zephyr_zenoh_shell_state {
	ZP_ZEPHYR_ZENOH_SHELL_STATE_IDLE = 0,
	ZP_ZEPHYR_ZENOH_SHELL_STATE_CONNECTING = 1,
	ZP_ZEPHYR_ZENOH_SHELL_STATE_CONNECTED = 2,
};

struct zp_zephyr_zenoh_shell_id_list_snapshot {
	char ids[ZP_ZEPHYR_ZENOH_SHELL_MAX_INFO_IDS][ZP_ZEPHYR_ZENOH_SHELL_MAX_ID_STR_LEN];
	uint8_t count;
	bool truncated;
};

struct zp_zephyr_zenoh_shell_sample_snapshot {
	char keyexpr[CONFIG_ZENOH_PICO_SHELL_MAX_KEYEXPR + 1];
	uint8_t payload[CONFIG_ZENOH_PICO_SHELL_MAX_PAYLOAD];
	size_t payload_len;
	size_t payload_stored_len;
	uint64_t receive_count;
	uint32_t truncated_count;
	int64_t receive_stamp_ms;
	uint8_t sample_kind;
};

struct zp_zephyr_zenoh_shell_status_snapshot {
	enum zp_zephyr_zenoh_shell_state state;
	bool iface_up;
	int32_t last_error;
	uint32_t connect_attempts;
	uint32_t open_failures;
	uint32_t sessions_opened;
	char local_zid[ZP_ZEPHYR_ZENOH_SHELL_MAX_ID_STR_LEN];
	struct zp_zephyr_zenoh_shell_id_list_snapshot routers;
	struct zp_zephyr_zenoh_shell_id_list_snapshot peers;
};

bool zp_zephyr_zenoh_shell_latest_sample_get(
	struct zp_zephyr_zenoh_shell_sample_snapshot *snapshot);
void zp_zephyr_zenoh_shell_status_get(struct zp_zephyr_zenoh_shell_status_snapshot *snapshot);
const char *zp_zephyr_zenoh_shell_state_name(enum zp_zephyr_zenoh_shell_state state);
void zp_zephyr_zenoh_shell_sample_clear(void);
int zp_zephyr_zenoh_shell_put(const char *keyexpr, const uint8_t *payload, size_t payload_len);
int zp_zephyr_zenoh_shell_delete(const char *keyexpr);

#endif
