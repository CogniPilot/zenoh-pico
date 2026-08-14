# zenoh-pico on Zephyr

The Zephyr port runs on native kernel objects. Tasks use `k_thread`,
locks use `k_mutex`, condition variables use `k_condvar`, and the clocks
use the kernel uptime and realtime clocks. A Zephyr application does not
need the POSIX portability layer to use zenoh-pico.

## Minimum application configuration

```ini
CONFIG_NETWORKING=y
CONFIG_NET_SOCKETS=y
CONFIG_NET_UDP=y
```

Enable the transport links the deployment uses, for example
`CONFIG_ZENOH_PICO_LINK_TCP=y` for a client to a router, or
`CONFIG_ZENOH_PICO_LINK_UDP_MULTICAST=y` with
`CONFIG_ZENOH_PICO_SCOUTING=y` for a router-less peer mesh. No
`CONFIG_POSIX_API` and no `CONFIG_MAX_PTHREAD_*` lines are required.

## Threading backend

`ZENOH_PICO_ZEPHYR_THREADS` selects the backend:

- `ZENOH_PICO_ZEPHYR_THREADS_NATIVE` (default): native `k_mutex`,
  `k_thread`, and `k_condvar`. Kernel objects live inside the zenoh-pico
  structures, so there is no shared object pool to size and no per-type
  count that a session can exhaust.
- `ZENOH_PICO_ZEPHYR_THREADS_POSIX`: the historical pthread
  implementation, kept for builds that opt into it. It depends on
  `POSIX_API` or `POSIX_THREADS` and its pthread pools
  (`CONFIG_MAX_PTHREAD_MUTEX_COUNT` and friends) must be sized by hand.

Most applications keep the default and delete any POSIX and pthread pool
lines they previously carried for zenoh-pico.

## Threads and stacks

A multi-threaded session runs one background executor thread. The
threads draw stacks from a static pool:

- `ZENOH_PICO_ZEPHYR_TASK_POOL_SIZE` (default 4): the number of
  concurrent zenoh-pico threads. One per open session plus any threads
  the application starts through `z_task_init`. A slot is returned when
  its thread is joined, so open and close cycles reuse slots.
- `ZENOH_PICO_ZEPHYR_TASK_STACK_SIZE` (default `CONFIG_MAIN_STACK_SIZE`):
  the per-thread stack. Size it to the session workload. 6144 bytes is a
  good starting point for a peer mesh node that runs the full reopen
  handshake. Confirm the high-water mark with the `kernel thread` shell
  command and trim from there.

Set `CONFIG_ZENOH_PICO_MULTI_THREAD=n` for a single-thread posture. The
session is then serviced inline by the application and no zenoh-pico
thread is created.

## Feature configuration

Feature toggles are injected from Kconfig and win over the generated
`config.h` defaults, so select them the normal way, for example
`CONFIG_ZENOH_PICO_MULTI_THREAD` or `CONFIG_ZENOH_PICO_LINK_TCP`, rather
than editing `config.h`.

## Per-board examples

The subdirectories here hold reference `prj.conf` files for specific
boards.
