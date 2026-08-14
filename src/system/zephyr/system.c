//
// Copyright (c) 2022 ZettaScale Technology
//
// This program and the accompanying materials are made available under the
// terms of the Eclipse Public License 2.0 which is available at
// http://www.eclipse.org/legal/epl-2.0, or the Apache License, Version 2.0
// which is available at https://www.apache.org/licenses/LICENSE-2.0.
//
// SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
//
// Contributors:
//   ZettaScale Zenoh Team, <zenoh@zettascale.tech>
//

#include <zephyr/version.h>

#if KERNEL_VERSION_MAJOR == 2
#include <random/rand32.h>
#else
#include <zephyr/random/random.h>
#endif

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>

#if defined(CONFIG_ZENOH_PICO_ZEPHYR_THREADS_NATIVE)
#if ZEPHYR_VERSION_CODE >= ZEPHYR_VERSION(4, 1, 0)
#include <zephyr/sys/clock.h>
#endif
#endif

#include "zenoh-pico/config.h"
#include "zenoh-pico/system/common/system_error.h"
#include "zenoh-pico/system/platform.h"

/*------------------ Random ------------------*/
uint8_t z_random_u8(void) { return z_random_u32(); }

uint16_t z_random_u16(void) { return z_random_u32(); }

uint32_t z_random_u32(void) { return sys_rand32_get(); }

uint64_t z_random_u64(void) {
    uint64_t ret = 0;
    ret |= z_random_u32();
    ret = ret << 32;
    ret |= z_random_u32();

    return ret;
}

void z_random_fill(void *buf, size_t len) { sys_rand_get(buf, len); }

/*------------------ Memory ------------------*/
void *z_malloc(size_t size) { return k_malloc(size); }

void *z_realloc(void *ptr, size_t size) {
    // k_realloc not implemented in Zephyr
    return NULL;
}

void z_free(void *ptr) { k_free(ptr); }

#if Z_FEATURE_MULTI_THREAD == 1

#if defined(CONFIG_ZENOH_PICO_ZEPHYR_THREADS_NATIVE)

/*------------------ Task (native kernel threads) ------------------*/

#ifdef CONFIG_ZENOH_PICO_ZEPHYR_TASK_POOL_SIZE
#define Z_ZEPHYR_TASK_POOL_SIZE CONFIG_ZENOH_PICO_ZEPHYR_TASK_POOL_SIZE
#else
#define Z_ZEPHYR_TASK_POOL_SIZE 4
#endif

#ifdef CONFIG_ZENOH_PICO_ZEPHYR_TASK_STACK_SIZE
#define Z_ZEPHYR_TASK_STACK_SIZE CONFIG_ZENOH_PICO_ZEPHYR_TASK_STACK_SIZE
#else
#define Z_ZEPHYR_TASK_STACK_SIZE CONFIG_MAIN_STACK_SIZE
#endif

// The default matches the priority the Zephyr POSIX layer gives pthreads
#ifdef CONFIG_ZENOH_PICO_ZEPHYR_TASK_PRIORITY_CUSTOM
#define Z_ZEPHYR_TASK_PRIORITY CONFIG_ZENOH_PICO_ZEPHYR_TASK_PRIORITY
#else
#define Z_ZEPHYR_TASK_PRIORITY K_LOWEST_APPLICATION_THREAD_PRIO
#endif

enum {
    _Z_TASK_SLOT_FREE = 0,
    _Z_TASK_SLOT_ACTIVE = 1,
    _Z_TASK_SLOT_DETACHED = 2,
};

typedef struct {
    struct k_thread thread;
    void *(*fun)(void *);
    void *arg;
    atomic_t state;
} _z_task_slot_t;

K_THREAD_STACK_ARRAY_DEFINE(_z_task_stack_area, Z_ZEPHYR_TASK_POOL_SIZE, Z_ZEPHYR_TASK_STACK_SIZE);
static _z_task_slot_t _z_task_slots[Z_ZEPHYR_TASK_POOL_SIZE];

static void _z_task_entry(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);
    _z_task_slot_t *slot = (_z_task_slot_t *)p1;
    (void)slot->fun(slot->arg);
}

// A detached slot is reclaimed only after k_thread_join confirms that its
// previous thread fully terminated, so a stack is never handed out while the
// old owner is still winding down on it. The compare-and-set revalidates the
// slot state so concurrent allocators cannot claim the same slot twice.
static _z_task_slot_t *_z_task_slot_alloc(void) {
    for (int i = 0; i < Z_ZEPHYR_TASK_POOL_SIZE; i++) {
        _z_task_slot_t *slot = &_z_task_slots[i];
        if (atomic_cas(&slot->state, _Z_TASK_SLOT_FREE, _Z_TASK_SLOT_ACTIVE)) {
            return slot;
        }
        if ((atomic_get(&slot->state) == _Z_TASK_SLOT_DETACHED) &&
            (k_thread_join(&slot->thread, K_NO_WAIT) == 0) &&
            atomic_cas(&slot->state, _Z_TASK_SLOT_DETACHED, _Z_TASK_SLOT_ACTIVE)) {
            return slot;
        }
    }
    return NULL;
}

z_result_t _z_task_init(_z_task_t *task, z_task_attr_t *attr, void *(*fun)(void *), void *arg) {
    _z_task_slot_t *slot = _z_task_slot_alloc();
    if (slot == NULL) {
        _z_report_system_error(EAGAIN);
        _Z_ERROR_RETURN(_Z_ERR_SYSTEM_GENERIC);
    }

    k_thread_stack_t *stack = _z_task_stack_area[slot - _z_task_slots];
    size_t stack_size = K_THREAD_STACK_SIZEOF(_z_task_stack_area[0]);
    int priority = Z_ZEPHYR_TASK_PRIORITY;
    const char *name = "zenoh_task";
    if (attr != NULL) {
        if (attr->stack != NULL) {
            stack = attr->stack;
            stack_size = attr->stack_size;
        }
        if (attr->has_priority) {
            priority = attr->priority;
        }
        if (attr->name != NULL) {
            name = attr->name;
        }
    }

    slot->fun = fun;
    slot->arg = arg;
    // The handle is published before the thread exists so a concurrent reader
    // of the owning structure never observes a started task without a slot
    task->_slot = slot;
    k_tid_t tid = k_thread_create(&slot->thread, stack, stack_size, _z_task_entry, slot, NULL, NULL, priority, 0,
                                  K_NO_WAIT);
    (void)k_thread_name_set(tid, name);
    return _Z_RES_OK;
}

z_result_t _z_task_join(_z_task_t *task) {
    _z_task_slot_t *slot = (_z_task_slot_t *)task->_slot;
    if (slot == NULL) {
        _z_report_system_error(EINVAL);
        _Z_ERROR_RETURN(_Z_ERR_SYSTEM_GENERIC);
    }
    int res = k_thread_join(&slot->thread, K_FOREVER);
    if (res == 0) {
        atomic_set(&slot->state, _Z_TASK_SLOT_FREE);
    }
    _Z_CHECK_SYS_ERR(res);
}

z_result_t _z_task_detach(_z_task_t *task) {
    _z_task_slot_t *slot = (_z_task_slot_t *)task->_slot;
    if (slot == NULL) {
        _z_report_system_error(EINVAL);
        _Z_ERROR_RETURN(_Z_ERR_SYSTEM_GENERIC);
    }
    atomic_set(&slot->state, _Z_TASK_SLOT_DETACHED);
    return _Z_RES_OK;
}

z_result_t _z_task_cancel(_z_task_t *task) {
    _z_task_slot_t *slot = (_z_task_slot_t *)task->_slot;
    if (slot == NULL) {
        _z_report_system_error(EINVAL);
        _Z_ERROR_RETURN(_Z_ERR_SYSTEM_GENERIC);
    }
    // Like pthread_cancel, this does not release the slot. The caller still
    // joins or detaches the task, which is when the slot becomes reusable.
    k_thread_abort(&slot->thread);
    return _Z_RES_OK;
}

void _z_task_exit(void) { k_thread_abort(k_current_get()); }

void _z_task_free(_z_task_t **task) {
    _z_task_t *ptr = *task;
    z_free(ptr);
    *task = NULL;
}

_z_task_id_t _z_task_get_id(const _z_task_t *task) {
    _z_task_slot_t *slot = (_z_task_slot_t *)task->_slot;
    return (slot == NULL) ? NULL : &slot->thread;
}
_z_task_id_t _z_task_current_id(void) { return k_current_get(); }
bool _z_task_id_equal(const _z_task_id_t *l, const _z_task_id_t *r) { return *l == *r; }

/*------------------ Mutex (native k_mutex) ------------------*/
z_result_t _z_mutex_init(_z_mutex_t *m) { _Z_CHECK_SYS_ERR(k_mutex_init(m)); }

z_result_t _z_mutex_drop(_z_mutex_t *m) {
    // A kernel mutex owns no resources beyond its own memory
    if (m == NULL) {
        return 0;
    }
    return _Z_RES_OK;
}

z_result_t _z_mutex_lock(_z_mutex_t *m) { _Z_CHECK_SYS_ERR(k_mutex_lock(m, K_FOREVER)); }

z_result_t _z_mutex_try_lock(_z_mutex_t *m) { _Z_CHECK_SYS_ERR(k_mutex_lock(m, K_NO_WAIT)); }

z_result_t _z_mutex_unlock(_z_mutex_t *m) { _Z_CHECK_SYS_ERR(k_mutex_unlock(m)); }

// k_mutex is recursive by design, the owner may relock with a use count, so
// the recursive flavor shares the plain implementation
z_result_t _z_mutex_rec_init(_z_mutex_rec_t *m) { _Z_CHECK_SYS_ERR(k_mutex_init(m)); }

z_result_t _z_mutex_rec_drop(_z_mutex_rec_t *m) {
    if (m == NULL) {
        return 0;
    }
    return _Z_RES_OK;
}

z_result_t _z_mutex_rec_lock(_z_mutex_rec_t *m) { _Z_CHECK_SYS_ERR(k_mutex_lock(m, K_FOREVER)); }

z_result_t _z_mutex_rec_try_lock(_z_mutex_rec_t *m) { _Z_CHECK_SYS_ERR(k_mutex_lock(m, K_NO_WAIT)); }

z_result_t _z_mutex_rec_unlock(_z_mutex_rec_t *m) { _Z_CHECK_SYS_ERR(k_mutex_unlock(m)); }

/*------------------ Condvar (native k_condvar) ------------------*/
z_result_t _z_condvar_init(_z_condvar_t *cv) { _Z_CHECK_SYS_ERR(k_condvar_init(cv)); }

z_result_t _z_condvar_drop(_z_condvar_t *cv) {
    // A kernel condition variable owns no resources beyond its own memory
    ARG_UNUSED(cv);
    return _Z_RES_OK;
}

z_result_t _z_condvar_signal(_z_condvar_t *cv) { _Z_CHECK_SYS_ERR(k_condvar_signal(cv)); }

z_result_t _z_condvar_signal_all(_z_condvar_t *cv) {
    // k_condvar_broadcast returns the number of woken threads, never an error
    (void)k_condvar_broadcast(cv);
    return _Z_RES_OK;
}

z_result_t _z_condvar_wait(_z_condvar_t *cv, _z_mutex_t *m) { _Z_CHECK_SYS_ERR(k_condvar_wait(cv, m, K_FOREVER)); }

z_result_t _z_condvar_wait_until(_z_condvar_t *cv, _z_mutex_t *m, const z_clock_t *abstime) {
    // The deadline and z_clock_now share the kernel uptime domain, so the
    // absolute deadline converts to a relative kernel timeout. An already
    // expired deadline degenerates to K_NO_WAIT, which reports -EAGAIN and
    // leaves the mutex held, matching the timed-wait contract.
    z_clock_t now = z_clock_now();
    k_timeout_t timeout = K_NO_WAIT;
    int64_t delta_ns = ((int64_t)abstime->tv_sec - (int64_t)now.tv_sec) * NSEC_PER_SEC +
                       ((int64_t)abstime->tv_nsec - (int64_t)now.tv_nsec);
    if (delta_ns > 0) {
        uint64_t ticks = k_ns_to_ticks_ceil64((uint64_t)delta_ns);
        if (ticks > (uint64_t)INT32_MAX) {
            ticks = (uint64_t)INT32_MAX;
        }
        timeout = K_TICKS((k_ticks_t)ticks);
    }

    int error = k_condvar_wait(cv, m, timeout);

    if (error == -EAGAIN) {
        return Z_ETIMEDOUT;
    }

    _Z_CHECK_SYS_ERR(error);
}

#else  // POSIX pthread backend

#define Z_THREADS_NUM 4

#ifdef CONFIG_TEST_EXTRA_STACK_SIZE
#define Z_PTHREAD_STACK_SIZE_DEFAULT CONFIG_MAIN_STACK_SIZE + CONFIG_TEST_EXTRA_STACK_SIZE
#elif CONFIG_TEST_EXTRA_STACKSIZE
#define Z_PTHREAD_STACK_SIZE_DEFAULT CONFIG_MAIN_STACK_SIZE + CONFIG_TEST_EXTRA_STACKSIZE
#else
#define Z_PTHREAD_STACK_SIZE_DEFAULT CONFIG_MAIN_STACK_SIZE
#endif

K_THREAD_STACK_ARRAY_DEFINE(thread_stack_area, Z_THREADS_NUM, Z_PTHREAD_STACK_SIZE_DEFAULT);
static int thread_index = 0;

/*------------------ Task ------------------*/
z_result_t _z_task_init(_z_task_t *task, z_task_attr_t *attr, void *(*fun)(void *), void *arg) {
    z_task_attr_t *lattr = NULL;
    z_task_attr_t tmp;
    if (attr == NULL) {
        (void)pthread_attr_init(&tmp);
        (void)pthread_attr_setstack(&tmp, &thread_stack_area[thread_index++], Z_PTHREAD_STACK_SIZE_DEFAULT);
        lattr = &tmp;
    }

    _Z_CHECK_SYS_ERR(pthread_create(task, lattr, fun, arg));
}

z_result_t _z_task_join(_z_task_t *task) { _Z_CHECK_SYS_ERR(pthread_join(*task, NULL)); }

z_result_t _z_task_detach(_z_task_t *task) { _Z_CHECK_SYS_ERR(pthread_detach(*task)); }

z_result_t _z_task_cancel(_z_task_t *task) { _Z_CHECK_SYS_ERR(pthread_cancel(*task)); }

void _z_task_exit(void) { pthread_exit(NULL); }

void _z_task_free(_z_task_t **task) {
    _z_task_t *ptr = *task;
    z_free(ptr);
    *task = NULL;
}

_z_task_id_t _z_task_get_id(const _z_task_t *task) { return *task; }
_z_task_id_t _z_task_current_id(void) { return pthread_self(); }
bool _z_task_id_equal(const _z_task_id_t *l, const _z_task_id_t *r) { return pthread_equal(*l, *r) != 0; }

/*------------------ Mutex ------------------*/
z_result_t _z_mutex_init(_z_mutex_t *m) { _Z_CHECK_SYS_ERR(pthread_mutex_init(m, NULL)); }

z_result_t _z_mutex_drop(_z_mutex_t *m) {
    if (m == NULL) {
        return 0;
    }
    _Z_CHECK_SYS_ERR(pthread_mutex_destroy(m));
}

z_result_t _z_mutex_lock(_z_mutex_t *m) { _Z_CHECK_SYS_ERR(pthread_mutex_lock(m)); }

z_result_t _z_mutex_try_lock(_z_mutex_t *m) { _Z_CHECK_SYS_ERR(pthread_mutex_trylock(m)); }

z_result_t _z_mutex_unlock(_z_mutex_t *m) { _Z_CHECK_SYS_ERR(pthread_mutex_unlock(m)); }

z_result_t _z_mutex_rec_init(_z_mutex_rec_t *m) {
    pthread_mutexattr_t attr;
    _Z_RETURN_IF_SYS_ERR(pthread_mutexattr_init(&attr));
    _Z_RETURN_IF_SYS_ERR(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
    _Z_RETURN_IF_SYS_ERR(pthread_mutex_init(m, &attr));
    _Z_CHECK_SYS_ERR(pthread_mutexattr_destroy(&attr));
}

z_result_t _z_mutex_rec_drop(_z_mutex_rec_t *m) {
    if (m == NULL) {
        return 0;
    }
    _Z_CHECK_SYS_ERR(pthread_mutex_destroy(m));
}

z_result_t _z_mutex_rec_lock(_z_mutex_rec_t *m) { _Z_CHECK_SYS_ERR(pthread_mutex_lock(m)); }

z_result_t _z_mutex_rec_try_lock(_z_mutex_rec_t *m) { _Z_CHECK_SYS_ERR(pthread_mutex_trylock(m)); }

z_result_t _z_mutex_rec_unlock(_z_mutex_rec_t *m) { _Z_CHECK_SYS_ERR(pthread_mutex_unlock(m)); }

/*------------------ Condvar ------------------*/
z_result_t _z_condvar_init(_z_condvar_t *cv) {
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    _Z_CHECK_SYS_ERR(pthread_cond_init(cv, &attr));
}

z_result_t _z_condvar_drop(_z_condvar_t *cv) { _Z_CHECK_SYS_ERR(pthread_cond_destroy(cv)); }

z_result_t _z_condvar_signal(_z_condvar_t *cv) { _Z_CHECK_SYS_ERR(pthread_cond_signal(cv)); }

z_result_t _z_condvar_signal_all(_z_condvar_t *cv) { _Z_CHECK_SYS_ERR(pthread_cond_broadcast(cv)); }

z_result_t _z_condvar_wait(_z_condvar_t *cv, _z_mutex_t *m) { _Z_CHECK_SYS_ERR(pthread_cond_wait(cv, m)); }

z_result_t _z_condvar_wait_until(_z_condvar_t *cv, _z_mutex_t *m, const z_clock_t *abstime) {
    int error = pthread_cond_timedwait(cv, m, abstime);

    if (error == ETIMEDOUT) {
        return Z_ETIMEDOUT;
    }

    _Z_CHECK_SYS_ERR(error);
}

#endif  // CONFIG_ZENOH_PICO_ZEPHYR_THREADS_NATIVE
#endif  // Z_FEATURE_MULTI_THREAD == 1

/*------------------ Sleep ------------------*/
z_result_t z_sleep_us(size_t time) {
    int32_t rem = time;
    while (rem > 0) {
        rem = k_usleep(rem);  // This function is unlikely to work as expected without kernel tuning.
                              // In particular, because the lower bound on the duration of a sleep is the
                              // duration of a tick, CONFIG_SYS_CLOCK_TICKS_PER_SEC must be adjusted to
                              // achieve the resolution desired. The implications of doing this must be
                              // understood before attempting to use k_usleep(). Use with caution.
                              // From: https://docs.zephyrproject.org/apidoc/latest/group__thread__apis.html
    }

    return 0;
}

z_result_t z_sleep_ms(size_t time) {
    int32_t rem = time;
    while (rem > 0) {
        rem = k_msleep(rem);
    }

    return 0;
}

z_result_t z_sleep_s(size_t time) {
    int32_t rem = time;
    while (rem > 0) {
        rem = k_sleep(K_SECONDS(rem));
    }

    return 0;
}

/*------------------ Instant ------------------*/
#if defined(CONFIG_ZENOH_PICO_ZEPHYR_THREADS_NATIVE)
z_clock_t z_clock_now(void) {
    z_clock_t now;
    uint64_t ns = k_ticks_to_ns_floor64((uint64_t)k_uptime_ticks());
    now.tv_sec = (time_t)(ns / NSEC_PER_SEC);
    now.tv_nsec = (long)(ns % NSEC_PER_SEC);
    return now;
}
#else
z_clock_t z_clock_now(void) {
    z_clock_t now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now;
}
#endif

unsigned long zp_clock_elapsed_us_since(z_clock_t *instant, z_clock_t *epoch) {
    long elapsed = (1000000 * (instant->tv_sec - epoch->tv_sec) + (instant->tv_nsec - epoch->tv_nsec) / 1000);
    return elapsed > 0 ? (unsigned long)elapsed : 0;
}

unsigned long zp_clock_elapsed_ms_since(z_clock_t *instant, z_clock_t *epoch) {
    long elapsed = (1000 * (instant->tv_sec - epoch->tv_sec) + (instant->tv_nsec - epoch->tv_nsec) / 1000000);
    return elapsed > 0 ? (unsigned long)elapsed : 0;
}

unsigned long zp_clock_elapsed_s_since(z_clock_t *instant, z_clock_t *epoch) {
    long elapsed = (instant->tv_sec - epoch->tv_sec);
    return elapsed > 0 ? (unsigned long)elapsed : 0;
}

unsigned long z_clock_elapsed_us(z_clock_t *instant) {
    z_clock_t now = z_clock_now();
    return zp_clock_elapsed_us_since(&now, instant);
}

unsigned long z_clock_elapsed_ms(z_clock_t *instant) {
    z_clock_t now = z_clock_now();
    return zp_clock_elapsed_ms_since(&now, instant);
}

unsigned long z_clock_elapsed_s(z_clock_t *instant) {
    z_clock_t now = z_clock_now();
    return zp_clock_elapsed_s_since(&now, instant);
}

void z_clock_advance_us(z_clock_t *clock, unsigned long duration) {
    clock->tv_sec += duration / 1000000;
    clock->tv_nsec += (duration % 1000000) * 1000;

    if (clock->tv_nsec >= 1000000000) {
        clock->tv_sec += 1;
        clock->tv_nsec -= 1000000000;
    }
}

void z_clock_advance_ms(z_clock_t *clock, unsigned long duration) {
    clock->tv_sec += duration / 1000;
    clock->tv_nsec += (duration % 1000) * 1000000;

    if (clock->tv_nsec >= 1000000000) {
        clock->tv_sec += 1;
        clock->tv_nsec -= 1000000000;
    }
}

void z_clock_advance_s(z_clock_t *clock, unsigned long duration) { clock->tv_sec += duration; }

/*------------------ Time ------------------*/
#if defined(CONFIG_ZENOH_PICO_ZEPHYR_THREADS_NATIVE) && (ZEPHYR_VERSION_CODE >= ZEPHYR_VERSION(4, 1, 0))
z_time_t z_time_now(void) {
    // The kernel wall clock counts from the boot epoch until the application
    // sets it, which mirrors the POSIX realtime clock behavior on these boards
    z_time_t now;
    struct timespec ts;
    if (sys_clock_gettime(SYS_CLOCK_REALTIME, &ts) == 0) {
        now.tv_sec = ts.tv_sec;
        now.tv_usec = (suseconds_t)(ts.tv_nsec / 1000);
    } else {
        now.tv_sec = 0;
        now.tv_usec = 0;
    }
    return now;
}
#else
z_time_t z_time_now(void) {
    z_time_t now;
    gettimeofday(&now, NULL);
    return now;
}
#endif

const char *z_time_now_as_str(char *const buf, unsigned long buflen) {
    z_time_t tv = z_time_now();
    struct tm ts;
    ts = *localtime(&tv.tv_sec);
    strftime(buf, buflen, "%Y-%m-%dT%H:%M:%SZ", &ts);
    return buf;
}

unsigned long z_time_elapsed_us(z_time_t *time) {
    z_time_t now = z_time_now();

    unsigned long elapsed = (1000000 * (now.tv_sec - time->tv_sec) + (now.tv_usec - time->tv_usec));
    return elapsed;
}

unsigned long z_time_elapsed_ms(z_time_t *time) {
    z_time_t now = z_time_now();

    unsigned long elapsed = (1000 * (now.tv_sec - time->tv_sec) + (now.tv_usec - time->tv_usec) / 1000);
    return elapsed;
}

unsigned long z_time_elapsed_s(z_time_t *time) {
    z_time_t now = z_time_now();

    unsigned long elapsed = now.tv_sec - time->tv_sec;
    return elapsed;
}

z_result_t _z_get_time_since_epoch(_z_time_since_epoch *t) {
    z_time_t now = z_time_now();
    t->secs = now.tv_sec;
    t->nanos = now.tv_usec * 1000;
    return 0;
}
