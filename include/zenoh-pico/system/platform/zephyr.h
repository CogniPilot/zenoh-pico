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

#ifndef ZENOH_PICO_SYSTEM_ZEPHYR_TYPES_H
#define ZENOH_PICO_SYSTEM_ZEPHYR_TYPES_H

#include <version.h>

#if KERNEL_VERSION_MAJOR == 2
#include <kernel.h>
#elif KERNEL_VERSION_MAJOR == 3 || KERNEL_VERSION_MAJOR == 4
#include <zephyr/kernel.h>
#else
#pragma GCC warning "This Zephyr version might not be supported."
#include <zephyr/kernel.h>
#endif

#include <stdbool.h>
#include <stddef.h>
#include <sys/time.h>
#include <time.h>

// The threading backend is selected with the ZENOH_PICO_ZEPHYR_THREADS
// Kconfig choice. The native backend maps tasks, mutexes, and condition
// variables onto kernel objects and needs no POSIX support. Builds without
// that Kconfig symbol (including non-west builds of this platform) keep the
// original pthread-based backend.
#if !defined(CONFIG_ZENOH_PICO_ZEPHYR_THREADS_NATIVE)
#include <pthread.h>
#endif

#include "zenoh-pico/config.h"

#ifdef __cplusplus
extern "C" {
#endif

#if Z_FEATURE_MULTI_THREAD == 1
#if defined(CONFIG_ZENOH_PICO_ZEPHYR_THREADS_NATIVE)

// Handle to one slot of the static task pool owned by the platform layer.
// The handle is copyable, all task state lives in the pool slot.
typedef struct {
    void *_slot;
} _z_task_t;

// Optional task attributes. A NULL attribute pointer, or a NULL stack field,
// selects a stack from the static pool sized by
// CONFIG_ZENOH_PICO_ZEPHYR_TASK_STACK_SIZE. A caller-provided stack must be
// declared with K_THREAD_STACK_DEFINE and stack_size must be its
// K_THREAD_STACK_SIZEOF value. The priority field is a Zephyr thread
// priority and is honored only when has_priority is true.
typedef struct {
    k_thread_stack_t *stack;
    size_t stack_size;
    int priority;
    bool has_priority;
    const char *name;
} z_task_attr_t;

typedef struct k_mutex _z_mutex_t;
typedef struct k_mutex _z_mutex_rec_t;
typedef struct k_condvar _z_condvar_t;
typedef k_tid_t _z_task_id_t;

#else  // POSIX pthread backend

typedef pthread_t _z_task_t;
typedef pthread_attr_t z_task_attr_t;
typedef pthread_mutex_t _z_mutex_t;
typedef pthread_mutex_t _z_mutex_rec_t;
typedef pthread_cond_t _z_condvar_t;
typedef pthread_t _z_task_id_t;

#endif  // CONFIG_ZENOH_PICO_ZEPHYR_THREADS_NATIVE
#endif  // Z_FEATURE_MULTI_THREAD == 1

typedef struct timespec z_clock_t;
typedef struct timeval z_time_t;

#if defined(ZP_PLATFORM_SOCKET_LINKS_ENABLED)
struct zsock_addrinfo;
#endif

typedef struct {
    union {
#if defined(ZP_PLATFORM_SOCKET_LINKS_ENABLED)
        int _fd;
#endif
#if Z_FEATURE_LINK_SERIAL == 1
        const struct device *_serial;
#endif
    };
} _z_sys_net_socket_t;

typedef struct {
    union {
#if defined(ZP_PLATFORM_SOCKET_LINKS_ENABLED)
        struct zsock_addrinfo *_iptcp;
#endif
    };
} _z_sys_net_endpoint_t;

#ifdef __cplusplus
}
#endif

#endif /* ZENOH_PICO_SYSTEM_ZEPHYR_TYPES_H */
