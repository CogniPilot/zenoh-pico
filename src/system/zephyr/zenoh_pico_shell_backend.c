/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zenoh_pico_shell_backend.h"

#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zenoh-pico.h>

LOG_MODULE_REGISTER(zp_zephyr_zenoh_shell, LOG_LEVEL_INF);

struct zp_zephyr_zenoh_shell_sample_store {
	struct zp_zephyr_zenoh_shell_sample_snapshot slots[2];
	atomic_t generation;
};

struct zp_zephyr_zenoh_shell_info_store {
	char local_zid[ZP_ZEPHYR_ZENOH_SHELL_MAX_ID_STR_LEN];
	struct zp_zephyr_zenoh_shell_id_list_snapshot routers;
	struct zp_zephyr_zenoh_shell_id_list_snapshot peers;
};

struct zp_zephyr_zenoh_shell_id_capture_context {
	struct zp_zephyr_zenoh_shell_id_list_snapshot *list;
};

static struct zp_zephyr_zenoh_shell_sample_store g_zp_zephyr_zenoh_shell_store;
static struct zp_zephyr_zenoh_shell_info_store g_zp_zephyr_zenoh_shell_info;
static K_THREAD_STACK_DEFINE(g_zp_zephyr_zenoh_shell_thread_stack, CONFIG_ZENOH_PICO_SHELL_THREAD_STACK_SIZE);
static struct k_thread g_zp_zephyr_zenoh_shell_thread;
K_MUTEX_DEFINE(g_zp_zephyr_zenoh_shell_store_lock);
K_MUTEX_DEFINE(g_zp_zephyr_zenoh_shell_info_lock);
K_MUTEX_DEFINE(g_zp_zephyr_zenoh_shell_pub_lock);
static atomic_t g_zp_zephyr_zenoh_shell_state;
static atomic_t g_zp_zephyr_zenoh_shell_last_error;
static atomic_t g_zp_zephyr_zenoh_shell_connect_attempts;
static atomic_t g_zp_zephyr_zenoh_shell_open_failures;
static atomic_t g_zp_zephyr_zenoh_shell_sessions_opened;
static uint64_t g_zp_zephyr_zenoh_shell_receive_count;
static uint32_t g_zp_zephyr_zenoh_shell_truncated_count;

static void zp_zephyr_zenoh_shell_sample_store_reset(void)
{
	memset(&g_zp_zephyr_zenoh_shell_store, 0, sizeof(g_zp_zephyr_zenoh_shell_store));
	g_zp_zephyr_zenoh_shell_receive_count = 0U;
	g_zp_zephyr_zenoh_shell_truncated_count = 0U;
}

static void zp_zephyr_zenoh_shell_info_clear_locked(void)
{
	memset(&g_zp_zephyr_zenoh_shell_info, 0, sizeof(g_zp_zephyr_zenoh_shell_info));
}

static void zp_zephyr_zenoh_shell_info_clear(void)
{
	k_mutex_lock(&g_zp_zephyr_zenoh_shell_info_lock, K_FOREVER);
	zp_zephyr_zenoh_shell_info_clear_locked();
	k_mutex_unlock(&g_zp_zephyr_zenoh_shell_info_lock);
}

static void zp_zephyr_zenoh_shell_id_list_store(struct zp_zephyr_zenoh_shell_id_list_snapshot *list,
				     const z_id_t *id)
{
	z_owned_string_t id_str;
	size_t len;

	if (list == NULL || id == NULL) {
		return;
	}

	if (list->count >= ZP_ZEPHYR_ZENOH_SHELL_MAX_INFO_IDS) {
		list->truncated = true;
		return;
	}

	z_internal_null(&id_str);
	if (z_id_to_string(id, &id_str) != 0) {
		return;
	}

	len = MIN(z_string_len(z_loan(id_str)),
		  sizeof(list->ids[list->count]) - 1U);
	memcpy(list->ids[list->count], z_string_data(z_loan(id_str)), len);
	list->ids[list->count][len] = '\0';
	list->count++;
	z_drop(z_move(id_str));
}

static void zp_zephyr_zenoh_shell_id_capture_handler(const z_id_t *id, void *ctx)
{
	struct zp_zephyr_zenoh_shell_id_capture_context *capture = ctx;

	if (capture == NULL) {
		return;
	}

	zp_zephyr_zenoh_shell_id_list_store(capture->list, id);
}

static void zp_zephyr_zenoh_shell_info_set_local_zid(struct zp_zephyr_zenoh_shell_info_store *info,
					  const z_id_t *id)
{
	z_owned_string_t id_str;
	size_t len;

	if (info == NULL || id == NULL) {
		return;
	}

	z_internal_null(&id_str);
	if (z_id_to_string(id, &id_str) != 0) {
		return;
	}

	len = MIN(z_string_len(z_loan(id_str)),
		  sizeof(info->local_zid) - 1U);
	memcpy(info->local_zid, z_string_data(z_loan(id_str)), len);
	info->local_zid[len] = '\0';
	z_drop(z_move(id_str));
}

static void zp_zephyr_zenoh_shell_info_capture(const z_loaned_session_t *session)
{
	struct zp_zephyr_zenoh_shell_info_store next = {0};
	struct zp_zephyr_zenoh_shell_id_capture_context routers_ctx = {
		.list = &next.routers,
	};
	struct zp_zephyr_zenoh_shell_id_capture_context peers_ctx = {
		.list = &next.peers,
	};
	z_owned_closure_zid_t callback;
	z_id_t local_zid;

	if (session == NULL) {
		return;
	}

	local_zid = z_info_zid(session);
	zp_zephyr_zenoh_shell_info_set_local_zid(&next, &local_zid);

	z_internal_null(&callback);
	if (z_closure_zid(&callback, zp_zephyr_zenoh_shell_id_capture_handler, NULL,
			  &routers_ctx) == 0) {
		(void)z_info_routers_zid(session, z_move(callback));
	}

	z_internal_null(&callback);
	if (z_closure_zid(&callback, zp_zephyr_zenoh_shell_id_capture_handler, NULL,
			  &peers_ctx) == 0) {
		(void)z_info_peers_zid(session, z_move(callback));
	}

	k_mutex_lock(&g_zp_zephyr_zenoh_shell_info_lock, K_FOREVER);
	g_zp_zephyr_zenoh_shell_info = next;
	k_mutex_unlock(&g_zp_zephyr_zenoh_shell_info_lock);
}

static void zp_zephyr_zenoh_shell_store_publish(z_loaned_sample_t *sample)
{
	struct zp_zephyr_zenoh_shell_sample_snapshot *slot;
	z_view_string_t keystr;
	z_bytes_reader_t reader;
	size_t payload_len;
	size_t payload_stored_len;
	size_t keyexpr_len = 0U;
	uint32_t next_generation;
	uint32_t slot_index;

	k_mutex_lock(&g_zp_zephyr_zenoh_shell_store_lock, K_FOREVER);
	next_generation = (uint32_t)atomic_get(&g_zp_zephyr_zenoh_shell_store.generation) + 1U;
	slot_index = next_generation & 1U;
	slot = &g_zp_zephyr_zenoh_shell_store.slots[slot_index];
	memset(slot, 0, sizeof(*slot));

	payload_len = z_bytes_len(z_sample_payload(sample));
	payload_stored_len = MIN(payload_len, sizeof(slot->payload));
	reader = z_bytes_get_reader(z_sample_payload(sample));
	(void)z_bytes_reader_read(&reader, slot->payload, payload_stored_len);

	if (z_keyexpr_as_view_string(z_sample_keyexpr(sample), &keystr) == 0) {
		keyexpr_len = MIN(z_string_len(z_loan(keystr)),
				  sizeof(slot->keyexpr) - 1U);
		memcpy(slot->keyexpr, z_string_data(z_loan(keystr)), keyexpr_len);
		slot->keyexpr[keyexpr_len] = '\0';
	}

	g_zp_zephyr_zenoh_shell_receive_count++;
	if (payload_stored_len < payload_len) {
		g_zp_zephyr_zenoh_shell_truncated_count++;
	}

	slot->payload_len = payload_len;
	slot->payload_stored_len = payload_stored_len;
	slot->receive_count = g_zp_zephyr_zenoh_shell_receive_count;
	slot->truncated_count = g_zp_zephyr_zenoh_shell_truncated_count;
	slot->receive_stamp_ms = k_uptime_get();
	slot->sample_kind = (uint8_t)z_sample_kind(sample);

	atomic_set(&g_zp_zephyr_zenoh_shell_store.generation, (atomic_val_t)next_generation);
	k_mutex_unlock(&g_zp_zephyr_zenoh_shell_store_lock);
}

static void zp_zephyr_zenoh_shell_data_handler(z_loaned_sample_t *sample, void *arg)
{
	ARG_UNUSED(arg);

	zp_zephyr_zenoh_shell_store_publish(sample);
}

static int zp_zephyr_zenoh_shell_config_init(z_owned_config_t *config)
{
	int rc;
	bool is_client = strcmp(CONFIG_ZENOH_PICO_SHELL_MODE, "client") == 0;
	uint8_t locator_key = is_client ? Z_CONFIG_CONNECT_KEY : Z_CONFIG_LISTEN_KEY;

	rc = z_config_default(config);
	if (rc < 0) {
		return rc;
	}

	rc = zp_config_insert(z_loan_mut(*config), Z_CONFIG_MODE_KEY,
			      CONFIG_ZENOH_PICO_SHELL_MODE);
	if (rc < 0) {
		return rc;
	}

	if (CONFIG_ZENOH_PICO_SHELL_CONNECT_LOCATOR[0] != '\0') {
		rc = zp_config_insert(z_loan_mut(*config), locator_key,
				      CONFIG_ZENOH_PICO_SHELL_CONNECT_LOCATOR);
		if (rc < 0) {
			return rc;
		}
	}

	return 0;
}

static int zp_zephyr_zenoh_shell_session_open(z_owned_session_t *session,
				   z_owned_subscriber_t *subscriber)
{
	z_owned_config_t config;
	z_owned_closure_sample_t callback;
	z_view_keyexpr_t keyexpr;
	int rc;

	z_internal_null(&config);
	z_internal_null(&callback);
	z_internal_null(session);
	z_internal_null(subscriber);

	rc = zp_zephyr_zenoh_shell_config_init(&config);
	if (rc < 0) {
		return rc;
	}

	rc = z_view_keyexpr_from_str(&keyexpr, CONFIG_ZENOH_PICO_SHELL_SUB_KEYEXPR);
	if (rc < 0) {
		z_drop(z_move(config));
		return rc;
	}

	z_closure(&callback, zp_zephyr_zenoh_shell_data_handler, NULL, NULL);
	rc = z_open(session, z_move(config), NULL);
	if (rc < 0) {
		z_drop(z_move(callback));
		return rc;
	}

	rc = z_declare_subscriber(z_loan(*session), subscriber, z_loan(keyexpr),
				  z_move(callback), NULL);
	if (rc < 0) {
		z_drop(z_move(callback));
		z_drop(z_move(*session));
		return rc;
	}

	return 0;
}

static int zp_zephyr_zenoh_shell_session_open_only(z_owned_session_t *session)
{
	z_owned_config_t config;
	int rc;

	z_internal_null(&config);
	z_internal_null(session);

	rc = zp_zephyr_zenoh_shell_config_init(&config);
	if (rc < 0) {
		return rc;
	}

	rc = z_open(session, z_move(config), NULL);
	if (rc < 0) {
		return rc;
	}

	return 0;
}

static bool zp_zephyr_zenoh_shell_iface_up(void)
{
	struct net_if *iface = net_if_get_default();

	return iface != NULL && net_if_is_up(iface);
}

static void zp_zephyr_zenoh_shell_thread_entry(void *arg0, void *arg1, void *arg2)
{
	ARG_UNUSED(arg0);
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);

	while (true) {
		z_owned_session_t session;
		z_owned_subscriber_t subscriber;
		int rc;

		atomic_inc(&g_zp_zephyr_zenoh_shell_connect_attempts);
		atomic_set(&g_zp_zephyr_zenoh_shell_state,
			   (atomic_val_t)ZP_ZEPHYR_ZENOH_SHELL_STATE_CONNECTING);

		rc = zp_zephyr_zenoh_shell_session_open(&session, &subscriber);
		if (rc < 0) {
			atomic_inc(&g_zp_zephyr_zenoh_shell_open_failures);
			atomic_set(&g_zp_zephyr_zenoh_shell_last_error, (atomic_val_t)rc);
			atomic_set(&g_zp_zephyr_zenoh_shell_state,
				   (atomic_val_t)ZP_ZEPHYR_ZENOH_SHELL_STATE_IDLE);
			zp_zephyr_zenoh_shell_info_clear();
			LOG_WRN("zenoh open failed: %d", rc);
			k_sleep(K_MSEC(CONFIG_ZENOH_PICO_SHELL_RETRY_MS));
			continue;
		}

		atomic_inc(&g_zp_zephyr_zenoh_shell_sessions_opened);
		atomic_set(&g_zp_zephyr_zenoh_shell_last_error, 0);
		atomic_set(&g_zp_zephyr_zenoh_shell_state,
			   (atomic_val_t)ZP_ZEPHYR_ZENOH_SHELL_STATE_CONNECTED);
		zp_zephyr_zenoh_shell_info_capture(z_loan(session));
		LOG_INF("zenoh %s %s keyexpr=%s",
			CONFIG_ZENOH_PICO_SHELL_MODE,
			CONFIG_ZENOH_PICO_SHELL_CONNECT_LOCATOR,
			CONFIG_ZENOH_PICO_SHELL_SUB_KEYEXPR);

		while (!z_session_is_closed(z_loan(session))) {
			k_sleep(K_MSEC(CONFIG_ZENOH_PICO_SHELL_RETRY_MS));
		}

		atomic_set(&g_zp_zephyr_zenoh_shell_state, (atomic_val_t)ZP_ZEPHYR_ZENOH_SHELL_STATE_IDLE);
		zp_zephyr_zenoh_shell_info_clear();
		LOG_WRN("zenoh session closed, retrying");
		z_drop(z_move(subscriber));
		z_drop(z_move(session));
		k_sleep(K_MSEC(CONFIG_ZENOH_PICO_SHELL_RETRY_MS));
	}
}

bool zp_zephyr_zenoh_shell_latest_sample_get(struct zp_zephyr_zenoh_shell_sample_snapshot *snapshot)
{
	uint32_t generation_start;
	uint32_t generation_end;
	uint32_t slot_index;

	if (snapshot == NULL) {
		return false;
	}

	do {
		generation_start = (uint32_t)atomic_get(&g_zp_zephyr_zenoh_shell_store.generation);
		if (generation_start == 0U) {
			return false;
		}

		slot_index = generation_start & 1U;
		*snapshot = g_zp_zephyr_zenoh_shell_store.slots[slot_index];
		generation_end = (uint32_t)atomic_get(&g_zp_zephyr_zenoh_shell_store.generation);
	} while (generation_start != generation_end);

	return true;
}

void zp_zephyr_zenoh_shell_status_get(struct zp_zephyr_zenoh_shell_status_snapshot *snapshot)
{
	if (snapshot == NULL) {
		return;
	}

	snapshot->state =
		(enum zp_zephyr_zenoh_shell_state)atomic_get(&g_zp_zephyr_zenoh_shell_state);
	snapshot->iface_up = zp_zephyr_zenoh_shell_iface_up();
	snapshot->last_error = (int32_t)atomic_get(&g_zp_zephyr_zenoh_shell_last_error);
	snapshot->connect_attempts =
		(uint32_t)atomic_get(&g_zp_zephyr_zenoh_shell_connect_attempts);
	snapshot->open_failures =
		(uint32_t)atomic_get(&g_zp_zephyr_zenoh_shell_open_failures);
	snapshot->sessions_opened =
		(uint32_t)atomic_get(&g_zp_zephyr_zenoh_shell_sessions_opened);

	k_mutex_lock(&g_zp_zephyr_zenoh_shell_info_lock, K_FOREVER);
	memcpy(snapshot->local_zid, g_zp_zephyr_zenoh_shell_info.local_zid,
	       sizeof(snapshot->local_zid));
	snapshot->routers = g_zp_zephyr_zenoh_shell_info.routers;
	snapshot->peers = g_zp_zephyr_zenoh_shell_info.peers;
	k_mutex_unlock(&g_zp_zephyr_zenoh_shell_info_lock);
}

const char *zp_zephyr_zenoh_shell_state_name(enum zp_zephyr_zenoh_shell_state state)
{
	switch (state) {
	case ZP_ZEPHYR_ZENOH_SHELL_STATE_IDLE:
		return "idle";
	case ZP_ZEPHYR_ZENOH_SHELL_STATE_CONNECTING:
		return "connecting";
	case ZP_ZEPHYR_ZENOH_SHELL_STATE_CONNECTED:
		return "connected";
	default:
		return "unknown";
	}
}

void zp_zephyr_zenoh_shell_sample_clear(void)
{
	k_mutex_lock(&g_zp_zephyr_zenoh_shell_store_lock, K_FOREVER);
	zp_zephyr_zenoh_shell_sample_store_reset();
	k_mutex_unlock(&g_zp_zephyr_zenoh_shell_store_lock);
}

int zp_zephyr_zenoh_shell_put(const char *keyexpr, const uint8_t *payload, size_t payload_len)
{
	z_owned_session_t session;
	z_owned_bytes_t bytes;
	z_view_keyexpr_t view;
	int rc;

	if (keyexpr == NULL || keyexpr[0] == '\0') {
		return -EINVAL;
	}

	z_internal_null(&session);
	z_internal_null(&bytes);

	k_mutex_lock(&g_zp_zephyr_zenoh_shell_pub_lock, K_FOREVER);

	rc = z_view_keyexpr_from_str(&view, keyexpr);
	if (rc < 0) {
		goto out;
	}

	rc = zp_zephyr_zenoh_shell_session_open_only(&session);
	if (rc < 0) {
		goto out;
	}

	if (payload_len > 0U) {
		rc = z_bytes_copy_from_buf(&bytes, payload, payload_len);
		if (rc < 0) {
			goto out;
		}
	} else {
		z_bytes_empty(&bytes);
	}

	rc = z_put(z_loan(session), z_loan(view), z_move(bytes), NULL);

out:
	z_drop(z_move(bytes));
	z_drop(z_move(session));
	k_mutex_unlock(&g_zp_zephyr_zenoh_shell_pub_lock);
	return rc;
}

int zp_zephyr_zenoh_shell_delete(const char *keyexpr)
{
	z_owned_session_t session;
	z_view_keyexpr_t view;
	z_delete_options_t options;
	int rc;

	if (keyexpr == NULL || keyexpr[0] == '\0') {
		return -EINVAL;
	}

	z_internal_null(&session);
	z_delete_options_default(&options);

	k_mutex_lock(&g_zp_zephyr_zenoh_shell_pub_lock, K_FOREVER);

	rc = z_view_keyexpr_from_str(&view, keyexpr);
	if (rc < 0) {
		goto out;
	}

	rc = zp_zephyr_zenoh_shell_session_open_only(&session);
	if (rc < 0) {
		goto out;
	}

	rc = z_delete(z_loan(session), z_loan(view), &options);

out:
	z_drop(z_move(session));
	k_mutex_unlock(&g_zp_zephyr_zenoh_shell_pub_lock);
	return rc;
}

static int zp_zephyr_zenoh_shell_init(void)
{
	zp_zephyr_zenoh_shell_sample_store_reset();
	zp_zephyr_zenoh_shell_info_clear();
	atomic_set(&g_zp_zephyr_zenoh_shell_state, (atomic_val_t)ZP_ZEPHYR_ZENOH_SHELL_STATE_IDLE);
	atomic_set(&g_zp_zephyr_zenoh_shell_last_error, 0);
	atomic_set(&g_zp_zephyr_zenoh_shell_connect_attempts, 0);
	atomic_set(&g_zp_zephyr_zenoh_shell_open_failures, 0);
	atomic_set(&g_zp_zephyr_zenoh_shell_sessions_opened, 0);

	k_thread_create(&g_zp_zephyr_zenoh_shell_thread, g_zp_zephyr_zenoh_shell_thread_stack,
			K_THREAD_STACK_SIZEOF(g_zp_zephyr_zenoh_shell_thread_stack),
			zp_zephyr_zenoh_shell_thread_entry, NULL, NULL, NULL,
			CONFIG_ZENOH_PICO_SHELL_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&g_zp_zephyr_zenoh_shell_thread, "zp_zenoh_shell");

	return 0;
}

SYS_INIT(zp_zephyr_zenoh_shell_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
