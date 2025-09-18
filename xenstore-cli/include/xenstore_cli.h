/* SPDX-License-Identifier: Apache-2.0 */

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

typedef void (*xs_notify_cb)(char *buf, size_t len, void *param);

struct xs_watcher {
	sys_snode_t node;
	xs_notify_cb cb;
	void *param;
};

int xs_init();

void xs_watcher_init(struct xs_watcher *w, xs_notify_cb cb, void *param);
int xs_watcher_register(struct xs_watcher *w);
int xs_watcher_unregister(struct xs_watcher *w);

ssize_t xs_read_timeout(const char *path, char *buf, size_t len, k_timeout_t tout);
ssize_t xs_directory_timeout(const char *path, char *buf, size_t len, k_timeout_t tout);
ssize_t xs_watch_timeout(const char *path, const char *token, char *buf, size_t len,
			 k_timeout_t tout);
ssize_t xs_read(const char *path, char *buf, size_t len);
ssize_t xs_directory(const char *path, char *buf, size_t len);
ssize_t xs_watch(const char *path, const char *token, char *buf, size_t len);

/* Iterate over NUL-separated string list (double-NUL terminated).
 * Returns next offset (>=1) or 0 when the list ends or on error.
 */
size_t xs_strlist_next(const char *list, size_t len, size_t off, const char **out);

#ifdef CONFIG_XENSTORE_CLI_WATCH_MSGQ
/* Optional message-queue interface for watch events */
struct xs_watch_evt {
	/* Includes struct xsd_sockmsg header followed by payload */
	uint16_t len; /* payload length */
	char data[XENSTORE_PAYLOAD_MAX];
};

int xs_watch_recv(struct xs_watch_evt *evt, k_timeout_t timeout);
#endif

#ifdef __cplusplus
}
#endif

#endif
