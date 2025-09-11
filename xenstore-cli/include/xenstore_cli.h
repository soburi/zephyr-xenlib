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

struct xenstore_client;
typedef void (*xs_notify_cb)(char *buf, size_t len, void *param);

struct xs_watch {
	sys_snode_t node;
	xs_notify_cb cb;
	void *param;
};

int xsc_init();
struct xenstore_client *xs_get_client();

/* Multiple watch registration using sys_snode_t (sys_slist) */
void xs_watch_init(struct xs_watch *w, xs_notify_cb cb, void *param);
int xs_watch_register(struct xs_watch *w);
int xs_watch_unregister(struct xs_watch *w);

/* Backward compatible single-callback registration */
void xs_set_notify_callback(struct xenstore_client *cli, xs_notify_cb cb, void *param);

ssize_t xs_read(struct xenstore_client *cli, const char *path, char *buf, size_t len);
ssize_t xs_directory(struct xenstore_client *cli, const char *path, char *buf, size_t len);
ssize_t xs_watch(struct xenstore_client *cli, const char *path, const char *token, char *buf,
		 size_t len);


/* Backward compatibility with legacy names */
struct xs_callback {
	sys_snode_t node;
	xs_notify_cb cb;
	void *param;
};
static inline void xs_callback_init(struct xs_callback *w, xs_notify_cb cb, void *param)
{
	xs_watch_init((struct xs_watch *)w, cb, param);
}
static inline int xs_callback_register(struct xs_callback *w)
{
	return xs_watch_register((struct xs_watch *)w);
}
static inline int xs_callback_unregister(struct xs_callback *w)
{
	return xs_watch_unregister((struct xs_watch *)w);
}

#ifdef __cplusplus
}
#endif

#endif
