/*
 * Copyright (c) 2025 TOKITA Hiroshi
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef XENLIB_XENSTORE_CLI_H_
#define XENLIB_XENSTORE_CLI_H_

#include <sys/types.h>
#include <stdint.h>
#include <string.h>

#include <xen/public/io/xs_wire.h>
#include <xen/public/memory.h>
#include <xen/public/xen.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/slist.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/xen/events.h>
#include <zephyr/xen/generic.h>
#include <zephyr/xen/hvm.h>

#include <xenstore_common.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Watch notification callback.
 *
 * @param path   Absolute XenStore path that triggered the watch (never NULL).
 * @param token  User token supplied when the watch was registered (never NULL).
 * @param param  User pointer forwarded from xs_watcher_init().
 */
typedef void (*xs_watch_cb)(const char *path, const char *token, void *param);

struct xs_watcher {
	sys_snode_t node;
	xs_watch_cb cb;
	void *param;
};

/**
 * @brief Initialize the XenStore client.
 *
 * Maps the XenStore shared rings, binds the event channel, and starts the
 * worker queue that processes ring notifications. Subsequent calls are
 * idempotent once initialization succeeds.
 *
 * @retval 0       Initialization succeeded or was already active.
 * @retval -errno  Underlying failure (ring mapping, event binding, queue init, etc.).
 */
int xs_init(void);

/**
 * @brief Prepare a watcher descriptor prior to registration.
 *
 * @param w     Watcher descriptor to initialize (must not be NULL).
 * @param cb    Callback invoked for watch notifications (must not be NULL).
 * @param param User pointer passed to the callback on each notification.
 */
void xs_watcher_init(struct xs_watcher *w, xs_watch_cb cb, void *param);

/**
 * @brief Register a watcher callback.
 *
 * @param w Watcher descriptor previously initialized with xs_watcher_init().
 *
 * @retval 0      Success.
 * @retval -EINVAL Watcher descriptor or callback was NULL.
 */
int xs_watcher_register(struct xs_watcher *w);

/**
 * @brief Read a XenStore path with an explicit timeout.
 *
 * @param path   Absolute path to read (must not be NULL).
 * @param buf    Destination buffer (must not be NULL).
 * @param len    Length of @p buf in bytes (must be > 0).
 * @param tout   Timeout to wait for the response; use K_FOREVER to block
 *               indefinitely.
 *
 * @retval >=0   Number of bytes copied into @p buf (excluding the terminator).
 * @retval -EINVAL Invalid arguments.
 * @retval -errno  Transport or protocol error.
 */
ssize_t xs_read_timeout(const char *path, char *buf, size_t len, k_timeout_t tout);

/**
 * @brief Enumerate a XenStore directory with an explicit timeout.
 *
 * @param path   Absolute directory path to enumerate (must not be NULL).
 * @param buf    Destination buffer for NUL-separated entries (must not be NULL).
 * @param len    Length of @p buf in bytes (must be > 0).
 * @param tout   Timeout to wait for the response.
 *
 * @retval >=0   Number of bytes copied (excluding the terminator).
 * @retval -EINVAL Invalid arguments.
 * @retval -errno  Transport or protocol error.
 */
ssize_t xs_directory_timeout(const char *path, char *buf, size_t len, k_timeout_t tout);

/**
 * @brief Issue an XS_WATCH request with an explicit timeout.
 *
 * @param path   Absolute path to watch (must not be NULL).
 * @param token  Optional user token (must not be NULL, may point to an empty string).
 * @param buf    Destination buffer for the initial watch response (must not be NULL).
 * @param len    Length of @p buf in bytes (must be > 0).
 * @param tout   Timeout to wait for the response.
 *
 * @retval >=0   Number of bytes copied into @p buf (excluding terminator).
 * @retval -EINVAL Invalid arguments.
 * @retval -errno  Transport or protocol error.
 */
ssize_t xs_watch_timeout(const char *path, const char *token, char *buf, size_t len,
			 k_timeout_t tout);

/**
 * @brief Convenience wrapper around xs_read_timeout() using the default timeout.
 *
 * @param path Absolute path to read (must not be NULL).
 * @param buf  Destination buffer (must not be NULL).
 * @param len  Length of @p buf in bytes (must be > 0).
 *
 * @return See xs_read_timeout().
 */
ssize_t xs_read(const char *path, char *buf, size_t len);

/**
 * @brief Convenience wrapper around xs_directory_timeout() using the default timeout.
 *
 * @param path Directory path to enumerate (must not be NULL).
 * @param buf  Destination buffer (must not be NULL).
 * @param len  Length of @p buf in bytes (must be > 0).
 *
 * @return See xs_directory_timeout().
 */
ssize_t xs_directory(const char *path, char *buf, size_t len);

/**
 * @brief Convenience wrapper around xs_watch_timeout() using the default timeout.
 *
 * @param path   Absolute path to watch (must not be NULL).
 * @param token  Optional user token (must not be NULL).
 * @param buf    Destination buffer for the initial response (must not be NULL).
 * @param len    Length of @p buf in bytes (must be > 0).
 *
 * @return See xs_watch_timeout().
 */
ssize_t xs_watch(const char *path, const char *token, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif
