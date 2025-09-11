/*
 * Copyright (c) 2023 EPAM Systems
 * Copyright (c) 2025 TOKITA Hiroshi
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/slist.h>
#include <zephyr/sys/util.h>

#include <zephyr/xen/events.h>
#include <zephyr/xen/generic.h>
#include <zephyr/xen/hvm.h>

#include <xen/public/io/xs_wire.h>
#include <xen/public/memory.h>
#include <xen/public/xen.h>
#include <xenstore_cli.h>

#include <xenstore_common.h>

LOG_MODULE_REGISTER(xenstore_client, CONFIG_LOG_DEFAULT_LEVEL);

/* Watch notification offload using ring buffer only (no slabs/msgq) */
#define XS_NOTIFY_STACK_SIZE 8192
#define XS_NOTIFY_PRIO       K_PRIO_PREEMPT(1)

/*
 * This client mirrors the ring I/O logic used by xenstore-srv
 * (REQ/RESP rings, evtchn notifications) to simplify future
 * consolidation of server and client implementations.
 */

struct xenstore_client {
	struct xenstore_domain_interface *domint;
	evtchn_port_t local_evtchn;

	/* Per-client request/response state */
	atomic_t next_req_id;
	int expected_req_id;
	int processed_req_id;

	struct k_spinlock lock; /* protects read_buf/read_pos */
	struct k_mutex notify_mtx;

	uint8_t read_buf[sizeof(struct xsd_sockmsg) + XENSTORE_PAYLOAD_MAX];
	size_t read_pos;
	uint8_t notify_buf[sizeof(struct xsd_sockmsg) + XENSTORE_PAYLOAD_MAX];
	size_t notify_pos;

	/* Thread-facing message queue (ring buffer of framed xsd_sockmsg) */
	struct ring_buf msg_rb;
	struct k_sem msg_sem; /* counts queued frames */
	uint8_t msg_rb_mem[CONFIG_XENSTORE_CLI_MSG_RING_SIZE];

	/* Notify offload resources (embedded per-client) */
	struct k_thread notify_thread_data;
	k_thread_stack_t notify_stack[XS_NOTIFY_STACK_SIZE];
	sys_slist_t notify_list;
};

static uint8_t work_buf[XENSTORE_RING_SIZE * 2];
static size_t work_pos;

static struct xenstore_client xsc;
static struct k_sem xs_lock;

static const size_t sz_sockmsg = sizeof(struct xsd_sockmsg);

static inline size_t frame_size(struct xsd_sockmsg *hdr)
{
	return hdr->len + sizeof(struct xsd_sockmsg);
}

static inline size_t ring_avail_for_read(struct xenstore_client *xs)
{
	struct xenstore_domain_interface *intf = xs->domint;
	XENSTORE_RING_IDX cons = intf->rsp_cons;
	XENSTORE_RING_IDX prod = intf->rsp_prod;
	return prod - cons;
}

static int ring_write(struct xenstore_client *xenstore, const void *data, size_t len)
{
	int ret;

	ret = xenstore_ring_write(xenstore->domint, data, len, false);
	notify_evtchn(xenstore->local_evtchn);

	return ret;
}

static int ring_read(struct xenstore_client *xenstore, void *data, size_t len)
{
	int ret;

	ret = xenstore_ring_read(xenstore->domint, data, len, false);
	notify_evtchn(xenstore->local_evtchn);

	return ret;
}

static void xs_notify_thread(void *p1, void *p2, void *p3)
{
	struct xenstore_client *xs = p1;
	int err;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		k_msleep(1000);
	}
}

/*
 * This client mirrors the ring I/O logic used by xenstore-srv
 * (REQ/RESP rings, evtchn notifications) to simplify future
 * consolidation of server and client implementations.
 */

static int read_to_buffer(struct xenstore_client *xs, size_t read_len)
{
	const size_t avail = ring_avail_for_read(xs);
	const size_t need = read_len - xs->read_pos;
	const size_t to_read = (avail < need) ? avail : need;
	const int ret = ring_read(xs, xs->read_buf + xs->read_pos, to_read);

	if (ret <= 0) {
		return ret;
	}

	xs->read_pos += ret;

	return ret;
}

static void xs_evtchn_cb(void *ptr)
{
	struct xenstore_client *xs = ptr;
	size_t offset = 0;

	while (ring_avail_for_read(xs)) {
		int ret = ring_read(xs, work_buf + work_pos, ring_avail_for_read(xs));

		if (ret < 0) {
			break;
		}
		if (work_pos + ret > sizeof(work_buf)) {
			LOG_ERR("work_buf overflowed: %ld", work_pos + ret);
			work_pos = 0;
			break;
		}

		work_pos += ret;
	}

	while (work_pos - offset >= sizeof(struct xsd_sockmsg)) {
		struct xsd_sockmsg *hdr = (void *)(work_buf + offset);
		size_t msglen = hdr->len + sizeof(*hdr);

		if (work_pos - offset < msglen) {
			break;
		}

		if ((hdr->req_id == xs->expected_req_id) && (hdr->req_id > xs->processed_req_id)) {
			k_spinlock_key_t key = k_spin_lock(&xs->lock);

			memcpy(xs->read_buf, hdr, msglen);
			xs->read_pos = msglen;
			k_spin_unlock(&xs->lock, key);

			xs->processed_req_id = xs->expected_req_id;
			k_sem_give(&xs_lock);
		} else {
			sys_snode_t *node;
			k_spinlock_key_t key = k_spin_lock(&xs->lock);

			k_mutex_lock(&xs->notify_mtx, K_FOREVER);

			SYS_SLIST_FOR_EACH_NODE(&xs->notify_list, node) {
				struct xs_watch *w = CONTAINER_OF(node, struct xs_watch, node);
				if (w->cb) {
					w->cb((char *)hdr, msglen, w->param);
				}
			}

			k_mutex_unlock(&xs->notify_mtx);
			k_spin_unlock(&xs->lock, key);
		}
		offset += msglen;
	}

	if (offset) {
		memmove(work_buf, work_buf + offset, work_pos - offset);
		work_pos -= offset;
	}
}

static int cmd_req(struct xenstore_client *xs, int type, const char *const *params,
		   size_t param_num, char *buf, size_t len, uint32_t *preq_id)
{
	size_t plen = 0;
	int err;

	for (int i = 0; i < param_num; i++) {
		plen += strlen(params[i]) + 1;
	}

	if (plen > XENSTORE_PAYLOAD_MAX) {
		LOG_ERR("strlen(path) + 1: %zu > XENSTORE_PAYLOAD_MAX", plen);
		return -ENAMETOOLONG;
	}

	*preq_id = atomic_inc(&xs->next_req_id);
	if (*preq_id == 0) {
		*preq_id = atomic_inc(&xs->next_req_id);
	}

	struct xsd_sockmsg hdr = {
		.type = type,
		.req_id = *preq_id,
		.tx_id = 0,
		.len = plen,
	};

	err = ring_write(xs, &hdr, sizeof(struct xsd_sockmsg));
	if (err < 0) {
		LOG_ERR("ring_write(hdr) failed: %d", err);
		return -EAGAIN;
	} else if (err < sizeof(struct xsd_sockmsg)) {
		LOG_ERR("ring_write(hdr) shorter response: %d", err);
		return -EIO;
	}

	for (int i = 0; i < param_num; i++) {
		err = ring_write(xs, params[i], strlen(params[i]) + 1);
		if (err < 0) {
			LOG_ERR("ring_write(path) failed: %d", err);
			return -EAGAIN;
		} else if (err < strlen(params[i]) + 1) {
			LOG_ERR("ring_write(path) shorter response: %d", err);
			return -EIO;
		}
	}

	return 0;
}

static ssize_t cmd_exec(struct xenstore_client *xs, int type, const char *const *params,
			size_t params_num, char *buf, size_t len, k_timeout_t timeout)
{
	int err;
	struct xsd_sockmsg *hdr;

	err = cmd_req(xs, type, params, params_num, buf, len, &xs->expected_req_id);
	if (err < 0) {
		LOG_ERR("xs_rw_common error: %d", err);
		return -EIO;
	}

	err = k_sem_take(&xs_lock, timeout);
	if (err < 0) {
		LOG_ERR("k_sem_take error: %d", err);
		return err;
	}

	hdr = (void *)xs->read_buf;

	if (hdr->len > len) {
		LOG_ERR("no buffer hdr.len=%u > len=%zu)", hdr->len, len);
		err = -ENOBUFS;
		goto end;
	}

	ssize_t copy_len = (len - 1 < hdr->len) ? len - 1 : hdr->len;
	k_spinlock_key_t key = k_spin_lock(&xs->lock);

	memcpy(buf, xs->read_buf + sizeof(struct xsd_sockmsg), copy_len);
	xs->read_pos = 0;

	k_spin_unlock(&xs->lock, key);

	if (copy_len < len) {
		buf[copy_len] = '\0';
	}

end:
	if (err) {
		return err;
	}

	return copy_len;
}

int xsc_init()
{
	uint64_t paddr = 0;
	uint64_t value = 0;
	mm_reg_t vaddr = 0;
	int err;

	if (xsc.domint) {
		return 0;
	}

	k_sem_init(&xs_lock, 0, 1);
	k_sem_init(&xsc.msg_sem, 0, K_SEM_MAX_LIMIT);
	ring_buf_init(&xsc.msg_rb, sizeof(xsc.msg_rb_mem), xsc.msg_rb_mem);
	sys_slist_init(&xsc.notify_list);
	k_mutex_init(&xsc.notify_mtx);

	xsc.read_pos = 0;

	err = hvm_get_parameter(HVM_PARAM_STORE_EVTCHN, DOMID_SELF, &value);
	if (err) {
		LOG_ERR("hvm_get_parameter(STORE_EVTCHN) failed: %d", err);
		return -ENODEV;
	}
	xsc.local_evtchn = value;

	err = hvm_get_parameter(HVM_PARAM_STORE_PFN, DOMID_SELF, &paddr);
	if (err) {
		LOG_ERR("hvm_get_param(STORE_PFN) failed: err=%d", err);
		return -EIO;
	}
	device_map(&vaddr, XEN_PFN_PHYS(paddr), XEN_PAGE_SIZE, K_MEM_CACHE_WB | K_MEM_PERM_RW);

	xsc.domint = (struct xenstore_domain_interface *)vaddr;

	while (ring_avail_for_read(&xsc)) {
		ring_read(&xsc, NULL, ring_avail_for_read(&xsc));
	}

	bind_event_channel(xsc.local_evtchn, xs_evtchn_cb, &xsc);
	unmask_event_channel(xsc.local_evtchn);

	k_thread_create(&xsc.notify_thread_data, xsc.notify_stack,
			K_THREAD_STACK_SIZEOF(xsc.notify_stack), xs_notify_thread, &xsc, NULL, NULL,
			XS_NOTIFY_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&xsc.notify_thread_data, "xs_notify");

	return 0;
}

struct xenstore_client *xs_get_client()
{
	if (xsc.domint) {
		return &xsc;
	}

	return NULL;
}

void xs_watch_init(struct xs_watch *w, xs_notify_cb cb, void *param)
{
	if (!w) {
		return;
	}

	w->node.next = NULL;
	w->cb = cb;
	w->param = param;
}

int xs_watch_register(struct xs_watch *w)
{
	if (!w || !w->cb) {
		return -EINVAL;
	}

	k_mutex_lock(&xsc.notify_mtx, K_FOREVER);
	sys_slist_append(&xsc.notify_list, &w->node);
	k_mutex_unlock(&xsc.notify_mtx);

	return 0;
}

int xs_watch_unregister(struct xs_watch *w)
{
	bool removed;

	if (!w) {
		return -EINVAL;
	}

	k_mutex_lock(&xsc.notify_mtx, K_FOREVER);
	removed = sys_slist_find_and_remove(&xsc.notify_list, &w->node);
	k_mutex_unlock(&xsc.notify_mtx);

	return removed ? 0 : -ENOENT;
}

ssize_t xs_read(struct xenstore_client *xs, const char *path, char *buf, size_t len)
{
	const char *const params[] = {path};

	if (!xs || !path || !buf || len == 0) {
		return -EINVAL;
	}

	return cmd_exec(xs, XS_READ, params, ARRAY_SIZE(params), buf, len, K_FOREVER);
}

ssize_t xs_directory(struct xenstore_client *xs, const char *path, char *buf, size_t len)
{
	const char *const params[] = {path};

	if (!xs || !path || !buf || len == 0) {
		return -EINVAL;
	}

	return cmd_exec(xs, XS_DIRECTORY, params, ARRAY_SIZE(params), buf, len, K_FOREVER);
}

ssize_t xs_watch(struct xenstore_client *xs, const char *path, const char *token, char *buf,
		 size_t len)
{
	const char *const params[] = {path, token};

	if (!xs || !path || !token || !buf || len == 0) {
		return -EINVAL;
	}

	return cmd_exec(xs, XS_WATCH, params, ARRAY_SIZE(params), buf, len, K_FOREVER);
}
