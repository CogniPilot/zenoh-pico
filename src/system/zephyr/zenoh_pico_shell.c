/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "zenoh_pico_shell_backend.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <zenoh-pico.h>

#define ZP_ZEPHYR_ZENOH_SHELL_WATCH_POLL_MS 100U
#define ZP_ZEPHYR_ZENOH_SHELL_WATCH_STACK_SIZE 4096U
#define ZP_ZEPHYR_ZENOH_SHELL_WATCH_MAX_KEYS 4U
#define ZP_ZEPHYR_ZENOH_SHELL_SHELL_WORKQ_STACK_SIZE 4096U

struct zp_zephyr_zenoh_shell_watch {
	const struct shell *sh;
	uint64_t last_receive_count;
	char keys[ZP_ZEPHYR_ZENOH_SHELL_WATCH_MAX_KEYS][CONFIG_ZENOH_PICO_SHELL_MAX_KEYEXPR + 1U];
	uint8_t key_count;
	bool active;
	bool text_mode;
};

struct zp_zephyr_zenoh_shell_scout_job {
	struct k_work work;
	const struct shell *sh;
	uint32_t timeout_ms;
	z_what_t what;
	atomic_t busy;
};

static struct zp_zephyr_zenoh_shell_watch g_zp_zephyr_zenoh_shell_watch;
static struct k_work_q g_zp_zephyr_zenoh_shell_shell_workq;
static K_THREAD_STACK_DEFINE(g_zp_zephyr_zenoh_shell_shell_workq_stack,
			     ZP_ZEPHYR_ZENOH_SHELL_SHELL_WORKQ_STACK_SIZE);
static struct zp_zephyr_zenoh_shell_scout_job g_zp_zephyr_zenoh_shell_scout_job;
K_SEM_DEFINE(g_zp_zephyr_zenoh_shell_watch_sem, 0, 1);

static void zp_zephyr_zenoh_shell_watch_thread(void *p0, void *p1, void *p2);
static void zp_zephyr_zenoh_shell_scout_work_handler(struct k_work *work);

K_THREAD_DEFINE(g_zp_zephyr_zenoh_shell_watch_tid, ZP_ZEPHYR_ZENOH_SHELL_WATCH_STACK_SIZE,
		zp_zephyr_zenoh_shell_watch_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

static const char *zp_zephyr_zenoh_shell_sample_kind_name(uint8_t kind)
{
	switch ((z_sample_kind_t)kind) {
	case Z_SAMPLE_KIND_PUT:
		return "put";
	case Z_SAMPLE_KIND_DELETE:
		return "delete";
	default:
		return "unknown";
	}
}

static void zp_zephyr_zenoh_shell_print_hexdump(const struct shell *sh,
				     const struct zp_zephyr_zenoh_shell_sample_snapshot *sample)
{
	size_t offset = 0U;

	while (offset < sample->payload_stored_len) {
		char hex[(16U * 3U) + 1U];
		char ascii[17];
		size_t chunk = MIN((size_t)16U,
				   sample->payload_stored_len - offset);
		size_t hex_pos = 0U;

		memset(hex, ' ', sizeof(hex) - 1U);
		hex[sizeof(hex) - 1U] = '\0';
		memset(ascii, 0, sizeof(ascii));

		for (size_t i = 0; i < chunk; ++i) {
			uint8_t value = sample->payload[offset + i];

			snprintk(&hex[hex_pos], sizeof(hex) - hex_pos,
				 "%02x ", value);
			hex_pos += 3U;
			ascii[i] = isprint(value) ? (char)value : '.';
		}

		shell_print(sh, "%04zx  %-48s  %s", offset, hex, ascii);
		offset += chunk;
	}
}

static void zp_zephyr_zenoh_shell_print_text(const struct shell *sh,
				  const struct zp_zephyr_zenoh_shell_sample_snapshot *sample)
{
	char line[(16U * 4U) + 1U];
	size_t line_pos = 0U;

	if (sample->payload_stored_len == 0U) {
		shell_print(sh, "(empty)");
		return;
	}

	for (size_t i = 0; i < sample->payload_stored_len; ++i) {
		uint8_t value = sample->payload[i];
		int rc;

		if (line_pos >= (sizeof(line) - 5U)) {
			line[line_pos] = '\0';
			shell_print(sh, "%s", line);
			line_pos = 0U;
		}

		if (value == '\n') {
			rc = snprintk(&line[line_pos], sizeof(line) - line_pos,
				      "\\n");
		} else if (value == '\r') {
			rc = snprintk(&line[line_pos], sizeof(line) - line_pos,
				      "\\r");
		} else if (value == '\t') {
			rc = snprintk(&line[line_pos], sizeof(line) - line_pos,
				      "\\t");
		} else if (isprint(value)) {
			line[line_pos++] = (char)value;
			line[line_pos] = '\0';
			continue;
		} else {
			rc = snprintk(&line[line_pos], sizeof(line) - line_pos,
				      "\\x%02x", value);
		}

		if (rc <= 0) {
			break;
		}
		line_pos += (size_t)rc;
	}

	if (line_pos > 0U) {
		line[line_pos] = '\0';
		shell_print(sh, "%s", line);
	}
}

static void zp_zephyr_zenoh_shell_print_config(const struct shell *sh)
{
	shell_print(sh,
		    "mode=%s locator=%s sub_keyexpr=%s retry_ms=%u",
		    CONFIG_ZENOH_PICO_SHELL_MODE,
		    CONFIG_ZENOH_PICO_SHELL_CONNECT_LOCATOR,
		    CONFIG_ZENOH_PICO_SHELL_SUB_KEYEXPR,
		    (unsigned int)CONFIG_ZENOH_PICO_SHELL_RETRY_MS);
	shell_print(sh,
		    "thread prio=%d stack=%u max_payload=%u max_keyexpr=%u",
		    CONFIG_ZENOH_PICO_SHELL_THREAD_PRIORITY,
		    (unsigned int)CONFIG_ZENOH_PICO_SHELL_THREAD_STACK_SIZE,
		    (unsigned int)CONFIG_ZENOH_PICO_SHELL_MAX_PAYLOAD,
		    (unsigned int)CONFIG_ZENOH_PICO_SHELL_MAX_KEYEXPR);
}

static void zp_zephyr_zenoh_shell_print_info_id_list(
	const struct shell *sh, const char *label,
	const struct zp_zephyr_zenoh_shell_id_list_snapshot *list)
{
	char line[(ZP_ZEPHYR_ZENOH_SHELL_MAX_INFO_IDS *
		   (ZP_ZEPHYR_ZENOH_SHELL_MAX_ID_STR_LEN + 2U)) + 4U];
	size_t pos = 0U;
	int rc;

	if (sh == NULL || label == NULL || list == NULL) {
		return;
	}

	rc = snprintk(line, sizeof(line), "[");
	if (rc <= 0) {
		return;
	}
	pos = (size_t)rc;

	for (uint8_t i = 0U; i < list->count; ++i) {
		rc = snprintk(&line[pos], sizeof(line) - pos, "%s%s",
			      i == 0U ? "" : ", ", list->ids[i]);
		if (rc <= 0) {
			return;
		}
		pos += (size_t)rc;
	}

	rc = snprintk(&line[pos], sizeof(line) - pos, "]");
	if (rc <= 0) {
		return;
	}

	shell_print(sh, "%s: %s%s", label, line,
		    list->truncated ? " (truncated)" : "");
}

struct zp_zephyr_zenoh_shell_scout_context {
	const struct shell *sh;
	uint32_t count;
};

static void zp_zephyr_zenoh_shell_print_sample_summary(const struct shell *sh,
					    const struct zp_zephyr_zenoh_shell_sample_snapshot *sample,
					    int64_t age_ms)
{
	shell_print(sh,
		    "sample rx=%llu truncated=%u kind=%s last_age_ms=%lld key=%s bytes=%zu/%zu",
		    (unsigned long long)sample->receive_count,
		    (unsigned int)sample->truncated_count,
		    zp_zephyr_zenoh_shell_sample_kind_name(sample->sample_kind),
		    (long long)age_ms,
		    sample->keyexpr,
		    sample->payload_stored_len,
		    sample->payload_len);
}

static int zp_zephyr_zenoh_shell_parse_format_arg(const char *arg, bool *text_mode)
{
	if (arg == NULL || text_mode == NULL) {
		return -EINVAL;
	}

	if (strcmp(arg, "text") == 0) {
		*text_mode = true;
		return 0;
	}

	if (strcmp(arg, "hex") == 0) {
		*text_mode = false;
		return 0;
	}

	return -EINVAL;
}

static int zp_zephyr_zenoh_shell_keyexpr_intersects(const char *left, const char *right,
					 bool *matches)
{
	z_view_keyexpr_t left_view;
	z_view_keyexpr_t right_view;
	int rc;

	if (left == NULL || right == NULL || matches == NULL) {
		return -EINVAL;
	}

	rc = z_view_keyexpr_from_str(&left_view, left);
	if (rc < 0) {
		return rc;
	}

	rc = z_view_keyexpr_from_str(&right_view, right);
	if (rc < 0) {
		return rc;
	}

	*matches = z_keyexpr_intersects(z_loan(left_view), z_loan(right_view));
	return 0;
}

static int zp_zephyr_zenoh_shell_sample_matches_selectors(
	const struct zp_zephyr_zenoh_shell_sample_snapshot *sample, size_t selector_count,
	const char *const *selectors, bool *matches)
{
	int rc;

	if (sample == NULL || matches == NULL) {
		return -EINVAL;
	}

	if (selector_count == 0U || selectors == NULL) {
		*matches = true;
		return 0;
	}

	*matches = false;

	for (size_t i = 0U; i < selector_count; ++i) {
		if (selectors[i] == NULL) {
			continue;
		}

		rc = zp_zephyr_zenoh_shell_keyexpr_intersects(selectors[i], sample->keyexpr,
						    matches);
		if (rc < 0) {
			return rc;
		}

		if (*matches) {
			return 0;
		}
	}

	return 0;
}

static int zp_zephyr_zenoh_shell_parse_scout_what(const char *arg, z_what_t *what)
{
	char buffer[32];
	char *saveptr = NULL;
	char *token;
	unsigned int mask = 0U;

	if (arg == NULL || what == NULL) {
		return -EINVAL;
	}

	if (strcmp(arg, "all") == 0) {
		*what = Z_WHAT_ROUTER_PEER;
		return 0;
	}

	if (strlen(arg) >= sizeof(buffer)) {
		return -EINVAL;
	}

	snprintk(buffer, sizeof(buffer), "%s", arg);

	for (token = strtok_r(buffer, "|", &saveptr); token != NULL;
	     token = strtok_r(NULL, "|", &saveptr)) {
		if (strcmp(token, "router") == 0) {
			mask |= Z_WHAT_ROUTER;
		} else if (strcmp(token, "peer") == 0) {
			mask |= Z_WHAT_PEER;
		} else if (strcmp(token, "client") == 0) {
			mask |= Z_WHAT_CLIENT;
		} else {
			return -EINVAL;
		}
	}

	if (mask == 0U) {
		return -EINVAL;
	}

	*what = (z_what_t)mask;
	return 0;
}

static const char *zp_zephyr_zenoh_shell_scout_what_name(z_what_t what)
{
	switch ((unsigned int)what) {
	case Z_WHAT_ROUTER:
		return "router";
	case Z_WHAT_PEER:
		return "peer";
	case Z_WHAT_CLIENT:
		return "client";
	case Z_WHAT_ROUTER_PEER:
		return "router|peer";
	default:
		return "custom";
	}
}

static bool zp_zephyr_zenoh_shell_parse_u32(const char *arg, uint32_t *value)
{
	char *end = NULL;
	unsigned long parsed;

	if (arg == NULL || value == NULL) {
		return false;
	}

	parsed = strtoul(arg, &end, 10);
	if (end == arg || *end != '\0' || parsed > UINT32_MAX) {
		return false;
	}

	*value = (uint32_t)parsed;
	return true;
}

static void zp_zephyr_zenoh_shell_scout_handler(z_loaned_hello_t *hello, void *ctx)
{
	struct zp_zephyr_zenoh_shell_scout_context *scout = ctx;
	z_id_t hello_zid;
	z_owned_string_t zid = {0};
	z_view_string_t whatami;
	const z_loaned_string_array_t *locators;

	if (scout == NULL || scout->sh == NULL || hello == NULL) {
		return;
	}

	hello_zid = z_hello_zid(hello);
	if (z_id_to_string(&hello_zid, &zid) != 0) {
		return;
	}

	(void)z_whatami_to_view_string(z_hello_whatami(hello), &whatami);
	locators = zp_hello_locators(hello);

	shell_print(scout->sh, "hello[%u] zid=%.*s whatami=%.*s",
		    (unsigned int)scout->count,
		    (int)z_string_len(z_loan(zid)),
		    z_string_data(z_loan(zid)),
		    (int)z_string_len(z_loan(whatami)),
		    z_string_data(z_loan(whatami)));

	for (size_t i = 0U; i < z_string_array_len(locators); ++i) {
		const z_loaned_string_t *locator = z_string_array_get(locators, i);

		shell_print(scout->sh, "  locator[%u]=%.*s", (unsigned int)i,
			    (int)z_string_len(locator),
			    z_string_data(locator));
	}

	scout->count++;
	z_drop(z_move(zid));
}

static void zp_zephyr_zenoh_shell_watch_stop(void)
{
	unsigned int key = irq_lock();

	g_zp_zephyr_zenoh_shell_watch.active = false;
	g_zp_zephyr_zenoh_shell_watch.sh = NULL;
	g_zp_zephyr_zenoh_shell_watch.last_receive_count = 0U;
	g_zp_zephyr_zenoh_shell_watch.key_count = 0U;
	memset(g_zp_zephyr_zenoh_shell_watch.keys, 0, sizeof(g_zp_zephyr_zenoh_shell_watch.keys));
	g_zp_zephyr_zenoh_shell_watch.text_mode = false;

	irq_unlock(key);
	k_sem_give(&g_zp_zephyr_zenoh_shell_watch_sem);
}

static bool zp_zephyr_zenoh_shell_watch_stop_if_shell(const struct shell *sh)
{
	unsigned int key = irq_lock();
	bool stop_watch =
		g_zp_zephyr_zenoh_shell_watch.active && g_zp_zephyr_zenoh_shell_watch.sh == sh;

	irq_unlock(key);

	if (stop_watch) {
		zp_zephyr_zenoh_shell_watch_stop();
	}

	return stop_watch;
}

static int zp_zephyr_zenoh_shell_watch_start(const struct shell *sh, bool text_mode,
				  size_t key_count, const char *const *keys)
{
	unsigned int key = irq_lock();

	if (g_zp_zephyr_zenoh_shell_watch.active && g_zp_zephyr_zenoh_shell_watch.sh != sh) {
		irq_unlock(key);
		return -EBUSY;
	}

	if (key_count > ZP_ZEPHYR_ZENOH_SHELL_WATCH_MAX_KEYS) {
		irq_unlock(key);
		return -E2BIG;
	}

	g_zp_zephyr_zenoh_shell_watch.sh = sh;
	g_zp_zephyr_zenoh_shell_watch.last_receive_count = 0U;
	g_zp_zephyr_zenoh_shell_watch.key_count = (uint8_t)key_count;
	g_zp_zephyr_zenoh_shell_watch.active = true;
	g_zp_zephyr_zenoh_shell_watch.text_mode = text_mode;
	memset(g_zp_zephyr_zenoh_shell_watch.keys, 0, sizeof(g_zp_zephyr_zenoh_shell_watch.keys));

	for (size_t i = 0U; i < key_count; ++i) {
		if (keys == NULL || keys[i] == NULL) {
			irq_unlock(key);
			return -EINVAL;
		}

		if (strlen(keys[i]) > CONFIG_ZENOH_PICO_SHELL_MAX_KEYEXPR) {
			irq_unlock(key);
			return -E2BIG;
		}

		snprintk(g_zp_zephyr_zenoh_shell_watch.keys[i],
			 sizeof(g_zp_zephyr_zenoh_shell_watch.keys[i]), "%s", keys[i]);
	}

	irq_unlock(key);
	k_sem_give(&g_zp_zephyr_zenoh_shell_watch_sem);
	return 0;
}

static void zp_zephyr_zenoh_shell_watch_thread(void *p0, void *p1, void *p2)
{
	struct zp_zephyr_zenoh_shell_watch watch;

	ARG_UNUSED(p0);
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);

	while (true) {
		(void)k_sem_take(&g_zp_zephyr_zenoh_shell_watch_sem, K_FOREVER);

		while (true) {
			struct zp_zephyr_zenoh_shell_sample_snapshot sample = {0};
			unsigned int key = irq_lock();
			const char *selectors[ZP_ZEPHYR_ZENOH_SHELL_WATCH_MAX_KEYS] = {0};
			bool matches = false;
			int rc;

			watch = g_zp_zephyr_zenoh_shell_watch;
			irq_unlock(key);

			if (!watch.active || watch.sh == NULL) {
				break;
			}

			if (zp_zephyr_zenoh_shell_latest_sample_get(&sample) &&
			    sample.receive_count != watch.last_receive_count) {
				for (uint8_t i = 0U; i < watch.key_count; ++i) {
					selectors[i] = watch.keys[i];
				}

				key = irq_lock();
				if (g_zp_zephyr_zenoh_shell_watch.active &&
				    g_zp_zephyr_zenoh_shell_watch.sh == watch.sh) {
					g_zp_zephyr_zenoh_shell_watch.last_receive_count =
						sample.receive_count;
					watch.text_mode =
						g_zp_zephyr_zenoh_shell_watch.text_mode;
				}
				irq_unlock(key);

				rc = zp_zephyr_zenoh_shell_sample_matches_selectors(
					&sample, watch.key_count, selectors,
					&matches);
				if (rc < 0 || !matches) {
					goto wait_next_sample;
				}

				shell_print(watch.sh,
					    "rx=%llu kind=%s key=%s bytes=%zu/%zu",
					    (unsigned long long)sample.receive_count,
					    zp_zephyr_zenoh_shell_sample_kind_name(
						    sample.sample_kind),
					    sample.keyexpr,
					    sample.payload_stored_len,
					    sample.payload_len);
				if (watch.text_mode) {
					zp_zephyr_zenoh_shell_print_text(watch.sh, &sample);
				} else {
					zp_zephyr_zenoh_shell_print_hexdump(watch.sh, &sample);
				}
			}

wait_next_sample:
			if (k_sem_take(&g_zp_zephyr_zenoh_shell_watch_sem,
				       K_MSEC(ZP_ZEPHYR_ZENOH_SHELL_WATCH_POLL_MS)) == 0) {
				continue;
			}
		}
	}
}

static void zp_zephyr_zenoh_shell_scout_work_handler(struct k_work *work)
{
	struct zp_zephyr_zenoh_shell_scout_job *job =
		CONTAINER_OF(work, struct zp_zephyr_zenoh_shell_scout_job, work);
	struct zp_zephyr_zenoh_shell_scout_context scout = {
		.sh = job->sh,
	};
	z_owned_config_t config;
	z_owned_closure_hello_t callback;
	z_scout_options_t options = {
		.timeout_ms = job->timeout_ms,
		.what = job->what,
	};
	int rc;

	z_internal_null(&config);
	z_internal_null(&callback);

	rc = z_config_default(&config);
	if (rc < 0) {
		shell_error(job->sh, "zenoh scout config failed: %d", rc);
		goto out;
	}

	rc = z_closure_hello(&callback, zp_zephyr_zenoh_shell_scout_handler, NULL, &scout);
	if (rc < 0) {
		z_drop(z_move(config));
		shell_error(job->sh, "zenoh scout callback failed: %d", rc);
		goto out;
	}

	rc = z_scout(z_move(config), z_move(callback), &options);
	if (rc < 0) {
		shell_error(job->sh, "zenoh scout failed: %d", rc);
		goto out;
	}

	if (scout.count == 0U) {
		shell_print(job->sh, "no zenoh routers or peers discovered");
	}

	shell_print(job->sh, "zenoh scout complete count=%u",
		    (unsigned int)scout.count);

out:
	atomic_set(&job->busy, 0);
}

static void zp_zephyr_zenoh_shell_ctrl_c_handler(const struct shell *sh, void *user_data)
{
	ARG_UNUSED(user_data);

	(void)zp_zephyr_zenoh_shell_watch_stop_if_shell(sh);
}

static int zp_zephyr_zenoh_shell_shell_init(void)
{
	k_work_queue_start(&g_zp_zephyr_zenoh_shell_shell_workq,
			   g_zp_zephyr_zenoh_shell_shell_workq_stack,
			   K_THREAD_STACK_SIZEOF(g_zp_zephyr_zenoh_shell_shell_workq_stack),
			   K_LOWEST_APPLICATION_THREAD_PRIO, NULL);
	k_thread_name_set(&g_zp_zephyr_zenoh_shell_shell_workq.thread, "zp_zenoh_cli");
	k_work_init(&g_zp_zephyr_zenoh_shell_scout_job.work,
		    zp_zephyr_zenoh_shell_scout_work_handler);
	atomic_set(&g_zp_zephyr_zenoh_shell_scout_job.busy, 0);
	return shell_ctrl_c_register(zp_zephyr_zenoh_shell_ctrl_c_handler, NULL);
}

static int cmd_zenoh_config(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	zp_zephyr_zenoh_shell_print_config(sh);
	return 0;
}

static int cmd_zenoh_status(const struct shell *sh, size_t argc, char **argv)
{
	struct zp_zephyr_zenoh_shell_status_snapshot status = {0};
	struct zp_zephyr_zenoh_shell_sample_snapshot sample = {0};
	int64_t age_ms = -1;
	bool have_sample;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	zp_zephyr_zenoh_shell_status_get(&status);
	have_sample = zp_zephyr_zenoh_shell_latest_sample_get(&sample);
	if (have_sample) {
		age_ms = k_uptime_get() - sample.receive_stamp_ms;
	}

	shell_print(sh,
		    "state=%s iface_up=%d mode=%s locator=%s keyexpr=%s",
		    zp_zephyr_zenoh_shell_state_name(status.state),
		    status.iface_up ? 1 : 0,
		    CONFIG_ZENOH_PICO_SHELL_MODE,
		    CONFIG_ZENOH_PICO_SHELL_CONNECT_LOCATOR,
		    CONFIG_ZENOH_PICO_SHELL_SUB_KEYEXPR);
	shell_print(sh,
		    "session attempts=%u opened=%u failures=%u last_error=%d",
		    (unsigned int)status.connect_attempts,
		    (unsigned int)status.sessions_opened,
		    (unsigned int)status.open_failures,
		    (int)status.last_error);

	if (!have_sample) {
		shell_print(sh, "sample rx=0 last_age_ms=-1");
		return 0;
	}

	zp_zephyr_zenoh_shell_print_sample_summary(sh, &sample, age_ms);
	return 0;
}

static int cmd_zenoh_info(const struct shell *sh, size_t argc, char **argv)
{
	struct zp_zephyr_zenoh_shell_status_snapshot status = {0};
	struct zp_zephyr_zenoh_shell_sample_snapshot sample = {0};
	int64_t age_ms = -1;
	bool have_sample;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	zp_zephyr_zenoh_shell_status_get(&status);
	have_sample = zp_zephyr_zenoh_shell_latest_sample_get(&sample);
	if (have_sample) {
		age_ms = k_uptime_get() - sample.receive_stamp_ms;
	}

	shell_print(sh, "zid: %s",
		    status.local_zid[0] != '\0' ? status.local_zid : "<unavailable>");
	zp_zephyr_zenoh_shell_print_info_id_list(sh, "routers", &status.routers);
	zp_zephyr_zenoh_shell_print_info_id_list(sh, "peers", &status.peers);

	if (!have_sample) {
		shell_print(sh, "sample: rx=0 last_age_ms=-1");
		return 0;
	}

	shell_print(sh,
		    "sample: rx=%llu kind=%s last_age_ms=%lld key=%s bytes=%zu/%zu truncated=%u",
		    (unsigned long long)sample.receive_count,
		    zp_zephyr_zenoh_shell_sample_kind_name(sample.sample_kind),
		    (long long)age_ms,
		    sample.keyexpr,
		    sample.payload_stored_len,
		    sample.payload_len,
		    (unsigned int)sample.truncated_count);
	return 0;
}

static int cmd_zenoh_sample(const struct shell *sh, size_t argc, char **argv)
{
	struct zp_zephyr_zenoh_shell_sample_snapshot sample = {0};
	bool text_mode = false;

	if (argc == 2U &&
	    zp_zephyr_zenoh_shell_parse_format_arg(argv[1], &text_mode) != 0) {
		shell_error(sh, "usage: zenoh sample [hex|text]");
		return -EINVAL;
	}

	if (!zp_zephyr_zenoh_shell_latest_sample_get(&sample)) {
		shell_error(sh, "no zenoh sample received yet");
		return -ENOENT;
	}

	shell_print(sh,
		    "rx=%llu kind=%s key=%s bytes=%zu/%zu stamp_ms=%lld truncated=%u",
		    (unsigned long long)sample.receive_count,
		    zp_zephyr_zenoh_shell_sample_kind_name(sample.sample_kind),
		    sample.keyexpr,
		    sample.payload_stored_len,
		    sample.payload_len,
		    (long long)sample.receive_stamp_ms,
		    (unsigned int)sample.truncated_count);

	if (text_mode) {
		zp_zephyr_zenoh_shell_print_text(sh, &sample);
	} else {
		zp_zephyr_zenoh_shell_print_hexdump(sh, &sample);
	}

	return 0;
}

static int cmd_zenoh_get(const struct shell *sh, size_t argc, char **argv)
{
	struct zp_zephyr_zenoh_shell_sample_snapshot sample = {0};
	const char *selector = NULL;
	bool text_mode = false;
	bool matches = false;
	int rc;

	for (size_t i = 1U; i < argc; ++i) {
		if ((strcmp(argv[i], "-s") == 0) ||
		    (strcmp(argv[i], "--selector") == 0)) {
			if ((i + 1U) >= argc) {
				shell_error(sh, "missing selector after %s", argv[i]);
				return -EINVAL;
			}
			selector = argv[++i];
			continue;
		}

		if (zp_zephyr_zenoh_shell_parse_format_arg(argv[i], &text_mode) == 0) {
			continue;
		}

		shell_error(sh, "usage: zenoh get [-s|--selector <selector>] [hex|text]");
		return -EINVAL;
	}

	if (!zp_zephyr_zenoh_shell_latest_sample_get(&sample)) {
		shell_error(sh, "no zenoh sample received yet");
		return -ENOENT;
	}

	if (selector != NULL) {
		rc = zp_zephyr_zenoh_shell_sample_matches_selectors(
			&sample, 1U, &selector, &matches);
		if (rc < 0) {
			shell_error(sh, "invalid selector: %s", selector);
			return rc;
		}

		if (!matches) {
			shell_error(sh, "no stored zenoh sample matches selector %s",
				    selector);
			return -ENOENT;
		}
	}

	shell_print(sh,
		    "rx=%llu kind=%s key=%s bytes=%zu/%zu stamp_ms=%lld truncated=%u",
		    (unsigned long long)sample.receive_count,
		    zp_zephyr_zenoh_shell_sample_kind_name(sample.sample_kind),
		    sample.keyexpr,
		    sample.payload_stored_len,
		    sample.payload_len,
		    (long long)sample.receive_stamp_ms,
		    (unsigned int)sample.truncated_count);

	if (text_mode) {
		zp_zephyr_zenoh_shell_print_text(sh, &sample);
	} else {
		zp_zephyr_zenoh_shell_print_hexdump(sh, &sample);
	}

	return 0;
}

static int cmd_zenoh_scout(const struct shell *sh, size_t argc, char **argv)
{
	z_scout_options_t options = {
		.timeout_ms = 1000U,
		.what = Z_WHAT_ROUTER_PEER,
	};
	z_what_t parsed_what;
	bool what_set = false;
	bool timeout_set = false;
	int rc;

	for (size_t i = 1U; i < argc; ++i) {
		if ((strcmp(argv[i], "-w") == 0) ||
		    (strcmp(argv[i], "--what") == 0)) {
			if ((i + 1U) >= argc) {
				shell_error(sh, "missing scout target after %s", argv[i]);
				return -EINVAL;
			}
			rc = zp_zephyr_zenoh_shell_parse_scout_what(argv[++i], &parsed_what);
			if (rc < 0) {
				shell_error(sh,
					    "usage: zenoh scout [-w|--what <peer|router|client[|...]|all>] [-t|--timeout <timeout_ms>]");
				return rc;
			}
			options.what = parsed_what;
			what_set = true;
			continue;
		}

		if ((strcmp(argv[i], "-t") == 0) ||
		    (strcmp(argv[i], "--timeout") == 0)) {
			if ((i + 1U) >= argc) {
				shell_error(sh, "missing timeout after %s", argv[i]);
				return -EINVAL;
			}
			if (!zp_zephyr_zenoh_shell_parse_u32(argv[++i], &options.timeout_ms)) {
				shell_error(sh, "invalid scout timeout: %s", argv[i]);
				return -EINVAL;
			}
			timeout_set = true;
			continue;
		}

		if (!what_set) {
			rc = zp_zephyr_zenoh_shell_parse_scout_what(argv[i], &parsed_what);
			if (rc == 0) {
				options.what = parsed_what;
				what_set = true;
				continue;
			}
		}

		if (!timeout_set &&
		    zp_zephyr_zenoh_shell_parse_u32(argv[i], &options.timeout_ms)) {
			timeout_set = true;
			continue;
		}

		shell_error(sh,
			    "usage: zenoh scout [-w|--what <peer|router|client[|...]|all>] [-t|--timeout <timeout_ms>]");
		return -EINVAL;
	}

	if (!atomic_cas(&g_zp_zephyr_zenoh_shell_scout_job.busy, 0, 1)) {
		shell_error(sh, "zenoh scout already in progress");
		return -EBUSY;
	}

	g_zp_zephyr_zenoh_shell_scout_job.sh = sh;
	g_zp_zephyr_zenoh_shell_scout_job.timeout_ms = options.timeout_ms;
	g_zp_zephyr_zenoh_shell_scout_job.what = options.what;
	k_work_submit_to_queue(&g_zp_zephyr_zenoh_shell_shell_workq,
			       &g_zp_zephyr_zenoh_shell_scout_job.work);
	shell_print(sh, "scout queued what=%s timeout_ms=%u",
		    zp_zephyr_zenoh_shell_scout_what_name(options.what),
		    (unsigned int)options.timeout_ms);
	return 0;
}

static int cmd_zenoh_subscribe(const struct shell *sh, size_t argc, char **argv)
{
	bool text_mode = false;
	const char *keys[CONFIG_SHELL_ARGC_MAX] = {0};
	size_t key_count = 0U;
	int rc;

	if (argc == 2U && strcmp(argv[1], "stop") == 0) {
		if (!zp_zephyr_zenoh_shell_watch_stop_if_shell(sh)) {
			shell_print(sh, "zenoh subscribe watcher not active");
		}
		return 0;
	}

	for (size_t i = 1U; i < argc; ++i) {
		if ((strcmp(argv[i], "-k") == 0) ||
		    (strcmp(argv[i], "--key") == 0)) {
			if ((i + 1U) >= argc) {
				shell_error(sh, "missing key after %s", argv[i]);
				return -EINVAL;
			}
			if (key_count >= ARRAY_SIZE(keys)) {
				shell_error(sh, "too many key filters");
				return -E2BIG;
			}
			keys[key_count++] = argv[++i];
			continue;
		}

		if (zp_zephyr_zenoh_shell_parse_format_arg(argv[i], &text_mode) == 0) {
			continue;
		}

		shell_error(sh,
			    "usage: zenoh subscribe [-k|--key <keyexpr>]... [hex|text|stop]");
		return -EINVAL;
	}

	rc = zp_zephyr_zenoh_shell_watch_start(sh, text_mode, key_count, keys);
	if (rc == -EBUSY) {
		shell_error(sh, "zenoh subscribe watcher already active on another shell");
		return rc;
	}

	if (rc == -E2BIG) {
		shell_error(sh, "zenoh subscribe supports up to %u key filters",
			    (unsigned int)ZP_ZEPHYR_ZENOH_SHELL_WATCH_MAX_KEYS);
		return rc;
	}

	if (key_count == 0U) {
		shell_print(sh,
			    "watching stored zenoh samples for keyexpr=%s as %s; Ctrl-C or 'zenoh subscribe stop' to stop",
			    CONFIG_ZENOH_PICO_SHELL_SUB_KEYEXPR,
			    text_mode ? "text" : "hex");
	} else {
		shell_print(sh,
			    "watching stored zenoh samples with %u key filter(s) as %s; Ctrl-C or 'zenoh subscribe stop' to stop",
			    (unsigned int)key_count,
			    text_mode ? "text" : "hex");
	}

	return 0;
}

static int cmd_zenoh_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	zp_zephyr_zenoh_shell_sample_clear();
	shell_print(sh, "zenoh sample store cleared");

	return 0;
}

static int cmd_zenoh_put(const struct shell *sh, size_t argc, char **argv)
{
	char payload[CONFIG_SHELL_CMD_BUFF_SIZE];
	const char *keyexpr = NULL;
	const char *value = NULL;
	size_t payload_len = 0U;
	bool use_flags = false;
	int rc;

	if (argc < 2U) {
		shell_error(sh,
			    "usage: zenoh put <keyexpr> [payload...] | zenoh put -k <keyexpr> -v <value>");
		return -EINVAL;
	}

	for (size_t i = 1U; i < argc; ++i) {
		if ((strcmp(argv[i], "-k") == 0) ||
		    (strcmp(argv[i], "--key") == 0) ||
		    (strcmp(argv[i], "-v") == 0) ||
		    (strcmp(argv[i], "--value") == 0)) {
			use_flags = true;
			break;
		}
	}

	for (size_t i = 1U; i < argc; ++i) {
		if ((strcmp(argv[i], "-k") == 0) ||
		    (strcmp(argv[i], "--key") == 0)) {
			if ((i + 1U) >= argc) {
				shell_error(sh, "missing key after %s", argv[i]);
				return -EINVAL;
			}
			keyexpr = argv[++i];
			use_flags = true;
			continue;
		}

		if ((strcmp(argv[i], "-v") == 0) ||
		    (strcmp(argv[i], "--value") == 0)) {
			if ((i + 1U) >= argc) {
				shell_error(sh, "missing value after %s", argv[i]);
				return -EINVAL;
			}
			value = argv[++i];
			use_flags = true;
			continue;
		}

		if (use_flags) {
			shell_error(sh,
				    "usage: zenoh put <keyexpr> [payload...] | zenoh put -k <keyexpr> -v <value>");
			return -EINVAL;
		}

		if (argv[i][0] == '-') {
			shell_error(sh,
				    "usage: zenoh put <keyexpr> [payload...] | zenoh put -k <keyexpr> -v <value>");
			return -EINVAL;
		}
	}

	memset(payload, 0, sizeof(payload));

	if (use_flags) {
		if (keyexpr == NULL || value == NULL) {
			shell_error(sh, "zenoh put requires -k/--key and -v/--value");
			return -EINVAL;
		}

		payload_len = strlen(value);
		if (payload_len >= sizeof(payload)) {
			shell_error(sh, "payload too long");
			return -E2BIG;
		}

		memcpy(payload, value, payload_len);
	} else {
		keyexpr = argv[1];

		for (size_t i = 2U; i < argc; ++i) {
			size_t arg_len = strlen(argv[i]);

			if (i > 2U) {
				if ((payload_len + 1U) >= sizeof(payload)) {
					shell_error(sh, "payload too long");
					return -E2BIG;
				}
				payload[payload_len++] = ' ';
			}

			if ((payload_len + arg_len) >= sizeof(payload)) {
				shell_error(sh, "payload too long");
				return -E2BIG;
			}

			memcpy(&payload[payload_len], argv[i], arg_len);
			payload_len += arg_len;
		}
	}

	rc = zp_zephyr_zenoh_shell_put(keyexpr, (const uint8_t *)payload, payload_len);
	if (rc < 0) {
		shell_error(sh, "zenoh put failed: %d", rc);
		return rc;
	}

	shell_print(sh, "put key=%s bytes=%zu", keyexpr, payload_len);

	return 0;
}

static int cmd_zenoh_delete(const struct shell *sh, size_t argc, char **argv)
{
	const char *keys[CONFIG_SHELL_ARGC_MAX] = {0};
	size_t key_count = 0U;
	bool use_flags = false;
	int rc;

	if (argc < 2U) {
		shell_error(sh,
			    "usage: zenoh delete <keyexpr> | zenoh delete -k <keyexpr> [-k <keyexpr> ...]");
		return -EINVAL;
	}

	for (size_t i = 1U; i < argc; ++i) {
		if ((strcmp(argv[i], "-k") == 0) ||
		    (strcmp(argv[i], "--key") == 0)) {
			if ((i + 1U) >= argc) {
				shell_error(sh, "missing key after %s", argv[i]);
				return -EINVAL;
			}
			if (key_count >= ARRAY_SIZE(keys)) {
				shell_error(sh, "too many delete keys");
				return -E2BIG;
			}
			keys[key_count++] = argv[++i];
			use_flags = true;
			continue;
		}

		if (argv[i][0] == '-') {
			shell_error(sh,
				    "usage: zenoh delete <keyexpr> | zenoh delete -k <keyexpr> [-k <keyexpr> ...]");
			return -EINVAL;
		}

		if (use_flags) {
			shell_error(sh,
				    "usage: zenoh delete <keyexpr> | zenoh delete -k <keyexpr> [-k <keyexpr> ...]");
			return -EINVAL;
		}

		keys[key_count++] = argv[i];
	}

	if (key_count == 0U) {
		shell_error(sh,
			    "usage: zenoh delete <keyexpr> | zenoh delete -k <keyexpr> [-k <keyexpr> ...]");
		return -EINVAL;
	}

	for (size_t i = 0U; i < key_count; ++i) {
		rc = zp_zephyr_zenoh_shell_delete(keys[i]);
		if (rc < 0) {
			shell_error(sh, "zenoh delete failed for %s: %d", keys[i], rc);
			return rc;
		}
		shell_print(sh, "delete key=%s", keys[i]);
	}

	return 0;
}

SYS_INIT(zp_zephyr_zenoh_shell_shell_init, APPLICATION, 0);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_zenoh,
	SHELL_CMD(config, NULL, "show zenoh session configuration",
		  cmd_zenoh_config),
	SHELL_CMD(status, NULL, "show stored zenoh listener status",
		  cmd_zenoh_status),
	SHELL_CMD(info, NULL, "show stored zenoh session identity and sample state",
		  cmd_zenoh_info),
	SHELL_CMD_ARG(sample, NULL,
		      "show the latest received zenoh sample: zenoh sample [hex|text]",
		      cmd_zenoh_sample, 1, 1),
	SHELL_CMD_ARG(get, NULL,
		      "show the latest stored sample from the resident subscriber: zenoh get [-s|--selector <selector>] [hex|text]",
		      cmd_zenoh_get, 1, CONFIG_SHELL_ARGC_MAX - 1),
	SHELL_CMD_ARG(scout, NULL,
		      "actively discover zenoh routers/peers: zenoh scout [-w|--what <peer|router|client[|...]|all>] [-t|--timeout <timeout_ms>]",
		      cmd_zenoh_scout, 1, CONFIG_SHELL_ARGC_MAX - 1),
	SHELL_CMD_ARG(subscribe, NULL,
		      "tail stored samples from the resident subscriber: zenoh subscribe [-k|--key <keyexpr>]... [hex|text|stop]",
		      cmd_zenoh_subscribe, 1, CONFIG_SHELL_ARGC_MAX - 1),
	SHELL_CMD(clear, NULL, "clear the stored latest zenoh sample",
		  cmd_zenoh_clear),
	SHELL_CMD_ARG(put, NULL,
		      "publish a debug UTF-8 sample: zenoh put <keyexpr> [payload...] | zenoh put -k <keyexpr> -v <value>",
		      cmd_zenoh_put, 2, CONFIG_SHELL_ARGC_MAX - 2),
	SHELL_CMD_ARG(delete, NULL,
		      "publish a debug delete sample: zenoh delete <keyexpr> | zenoh delete -k <keyexpr> [-k <keyexpr> ...]",
		      cmd_zenoh_delete, 2, CONFIG_SHELL_ARGC_MAX - 2),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(zenoh, &sub_zenoh, "zenoh over Ethernet commands", NULL);
