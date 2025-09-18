/*
 * Copyright (c) 2025 TOKITA Hiroshi
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>

#include <xen/public/io/xs_wire.h>
#include <xen/public/memory.h>
#include <xen/public/xen.h>
#include <xenstore_common.h>
#include <xenstore_cli.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/slist.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/barrier.h>

#include <zephyr/xen/events.h>
#include <zephyr/xen/generic.h>
#include <zephyr/xen/hvm.h>

LOG_MODULE_REGISTER(xenstore_cli);

#define SZ_SOCKMSG         sizeof(struct xsd_sockmsg)
#define SZ_FRAME(h)        (SZ_SOCKMSG + h->len)
#define XS_NOTIFY_FRAME_SZ (SZ_SOCKMSG + XENSTORE_PAYLOAD_MAX)

/*
 * This client mirrors the ring I/O logic used by xenstore-srv
 * (REQ/RESP rings, evtchn notifications) to simplify future
 * consolidation of server and client implementations.
 */

struct xs_buf {
	sys_snode_t node;
	uint8_t *buf;
	size_t len;
	size_t pos;
	struct k_sem sem;
	uint32_t req_id;
	int err;
};

struct xenstore_client {
	struct xenstore_domain_interface *domint;
	evtchn_port_t local_evtchn;

	atomic_t next_req_id;

	struct k_spinlock lock;

	uint8_t hdr_buf[SZ_SOCKMSG];
	size_t hdr_pos;

	sys_slist_t cmd_q;
	sys_slist_t notify_list;
	uint8_t notify_frame[XS_NOTIFY_FRAME_SZ];
	struct xs_buf notify;

	size_t to_discard;

	struct k_work isr_work;
	struct k_work_q workq;
	K_KERNEL_STACK_MEMBER(workq_stack, 1024 * 32);
	int workq_priority;

	k_timeout_t default_timeout;
};

static struct xenstore_client xs_cli;

/* workq stack is now a member (workq_stack) of xenstore_client */

static inline uint32_t alloc_req_id()
{
	uint32_t id = (atomic_inc(&xs_cli.next_req_id) & UINT32_MAX);

	if (id == 0) {
		id = (atomic_inc(&xs_cli.next_req_id) & UINT32_MAX);
	}

	return id;
}

static inline size_t ring_avail_for_read(struct xenstore_client *xs)
{
	struct xenstore_domain_interface *intf = xs->domint;
	z_barrier_dmem_fence_full();
	XENSTORE_RING_IDX cons = intf->rsp_cons;
	XENSTORE_RING_IDX prod = intf->rsp_prod;
	z_barrier_dmem_fence_full();
	if (check_indexes(cons, prod)) {
		return 0;
	}

	return prod - cons;
}

static inline size_t ring_avail_for_write(struct xenstore_client *xs)
{
	struct xenstore_domain_interface *intf = xs->domint;

	z_barrier_dmem_fence_full();
	XENSTORE_RING_IDX cons = intf->req_cons;
	XENSTORE_RING_IDX prod = intf->req_prod;
	z_barrier_dmem_fence_full();

	if (check_indexes(cons, prod)) {
		return 0;
	}

	return XENSTORE_RING_SIZE - (prod - cons);
}

static int ring_write_all(struct xenstore_client *xs, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	size_t written = 0;

	while (written < len) {
		int rc = xenstore_ring_write(xs->domint, p + written, len - written, true);

		if (rc < 0) {
			return rc;
		}

		if (rc == 0) {
			/* Ring is full; yield to allow peer to consume */
			k_yield();
			continue;
		}

		written += (size_t)rc;
	}

	return written;
}

static int ring_read(struct xenstore_client *xenstore, void *data, size_t len)
{
	int ret;

	if (len == 0) {
		return 0;
	}

	ret = xenstore_ring_read(xenstore->domint, data, len, true);

	return ret;
}

static inline struct xs_buf *xs_cmdq_find_by_req(struct xenstore_client *xs, uint32_t req_id)
{
	k_spinlock_key_t key;
	struct xs_buf *out = NULL;
	sys_snode_t *n;

	key = k_spin_lock(&xs->lock);

	for (n = sys_slist_peek_head(&xs->cmd_q); n; n = sys_slist_peek_next(n)) {
		struct xs_buf *b = CONTAINER_OF(n, struct xs_buf, node);

		if (b->req_id == req_id) {
			out = b;
			break;
		}
	}

	k_spin_unlock(&xs->lock, key);

	return out;
}

static void proc(struct xenstore_client *xs, size_t avail)
{
	struct xsd_sockmsg *hdr = (struct xsd_sockmsg *)(xs->hdr_buf);
	struct xs_buf *xbuf;
	size_t to_read;
	int ret;
	bool too_large = false;

	LOG_DBG("xs_evtchn_cb avail=%zu hdr_pos=%zu", avail, xs->hdr_pos);

	if (xs->hdr_pos < SZ_SOCKMSG) {
		const size_t hdr_to_read = MIN(SZ_SOCKMSG - xs->hdr_pos, avail);

		ret = ring_read(xs, xs->hdr_buf + xs->hdr_pos, hdr_to_read);
		if (ret < 0) {
			LOG_ERR("ring_read failed: %d", ret);
			return;
		}

		xs->hdr_pos += (size_t)ret;
		avail -= (size_t)ret;

		if (xs->hdr_pos < SZ_SOCKMSG) {
			LOG_DBG("header not ready");
			return;
		}
	}

	const bool is_resp = (hdr->type != XS_WATCH_EVENT);

	if (hdr->len > XENSTORE_PAYLOAD_MAX) {
		LOG_ERR("payload too large: %u > %u", (unsigned)hdr->len,
			(unsigned)XENSTORE_PAYLOAD_MAX);
		xs->to_discard = hdr->len;
		xs->hdr_pos = 0;
		too_large = true;
	}

	if (is_resp) {
		xbuf = xs_cmdq_find_by_req(xs, hdr->req_id);

		if (too_large) {
			if (xbuf) {
				xbuf->err = -EMSGSIZE;
				k_sem_give(&xbuf->sem);
			}
			xs->to_discard = hdr->len;
			xs->hdr_pos = 0;
			return;
		}

		if (xbuf == NULL) {
			xs->to_discard = hdr->len;
			xs->hdr_pos = 0;
			return;
		}

		if (xbuf->len < hdr->len || hdr->req_id == 0) {
			LOG_ERR("invalid response: req_id=%u len=%u cap=%zu", hdr->req_id, hdr->len,
				xbuf->len);
			xbuf->err = -EIO;
			xs->to_discard = hdr->len;
			xs->hdr_pos = 0;
			k_sem_give(&xbuf->sem);
			return;
		}
		to_read = hdr->len - xbuf->pos;
	} else {
		xbuf = &xs->notify;

		if (too_large) {
			return;
		}

		if (xbuf->pos == 0) {
			xbuf->node.next = NULL;
			xbuf->buf = xs->notify_frame;
			xbuf->len = SZ_SOCKMSG + hdr->len;
			xbuf->pos = SZ_SOCKMSG;
			xbuf->req_id = hdr->req_id;
			memcpy(xbuf->buf, hdr, SZ_SOCKMSG);
		}

		to_read = hdr->len - (xbuf->pos - SZ_SOCKMSG);
	}

	to_read = MIN(to_read, MIN(avail, xbuf->pos < xbuf->len));

	ret = ring_read(xs, xbuf->buf + xbuf->pos, to_read);
	if (ret < 0) {
		LOG_ERR("ring_read failed: %d", ret);
		return;
	}

	xbuf->pos += (size_t)ret;

	/* Not enough yet; wait for next event */
	if (xbuf->pos - (is_resp ? 0 : SZ_SOCKMSG) < hdr->len) {
		return;
	}

	const size_t copy_len = (xbuf->len - 1 < hdr->len) ? xbuf->len - 1 : hdr->len;

	if (copy_len < xbuf->len) {
		xbuf->buf[copy_len] = '\0';
	}

	if (is_resp) {
		if (hdr->type == XS_ERROR) {
			xbuf->err = xenstore_get_error(xbuf->buf);
		} else {
			xbuf->err = 0;
		}

		k_sem_give(&xbuf->sem);
	} else {
		sys_snode_t *node;

		SYS_SLIST_FOR_EACH_NODE(&xs->notify_list, node) {
			struct xs_watcher *w = CONTAINER_OF(node, struct xs_watcher, node);
			if (w->cb) {
				w->cb(xbuf->buf, xbuf->pos, w->param);
			}
		}

		xs->notify.pos = 0;
		xs->notify.len = 0;
	}

	xs->hdr_pos = 0;
	xs->to_discard = 0;
}

static int discard(struct xenstore_client *xs)
{
	int ret = 0;

	if (xs->to_discard) {
		LOG_DBG("discard %zu", xs->to_discard);

		ret = ring_read(xs, NULL, xs->to_discard);
		if (ret < 0) {
			return ret;
		}

		xs->to_discard -= (ret < xs->to_discard) ? ret : xs->to_discard;
	}

	return ret;
}

void isr_workhandler(struct k_work *work)
{
	struct xenstore_client *xs = CONTAINER_OF(work, struct xenstore_client, isr_work);
	size_t avail;
	int ret;

	while ((avail = ring_avail_for_read(xs))) {
		ret = discard(xs);
		if (ret < 0) {
			break;
		}

		avail -= (size_t)ret;
		if (avail == 0) {
			continue;
		}

		proc(xs, avail);

		ret = discard(xs);
		if (ret < 0) {
			break;
		}
	}

	LOG_DBG("fin hdr_pos %zu", xs->hdr_pos);
}

static void xs_evtchn_cb(void *ptr)
{
	struct xenstore_client *xs = ptr;

	k_work_submit_to_queue(&xs->workq, &xs->isr_work);
}

static int cmd_req(struct xenstore_client *xs, enum xsd_sockmsg_type type,
		   const char *const *params, size_t param_num, char *buf, size_t len,
		   uint32_t *preq_id)
{
	size_t plen = 0;
	int err = 0;
	size_t i;

	for (i = 0; i < param_num; i++) {
		plen += strlen(params[i]) + 1;
	}

	if (plen > XENSTORE_PAYLOAD_MAX) {
		LOG_ERR("payload too large: %zu > " STRINGIFY(XENSTORE_PAYLOAD_MAX), plen);
		return -ENAMETOOLONG;
	}

	*preq_id = alloc_req_id();

	const struct xsd_sockmsg hdr = {
		.type = type,
		.req_id = *preq_id,
		.tx_id = 0,
		.len = plen,
	};

	const size_t avail_for_write = ring_avail_for_write(xs);

	if (avail_for_write < (SZ_SOCKMSG + plen)) {
		LOG_ERR("ring_write: nospace: %zu < %zu", avail_for_write,
			(size_t)(SZ_SOCKMSG + plen));
		return -EAGAIN;
	}

	err = ring_write_all(xs, &hdr, sizeof(struct xsd_sockmsg));
	if (err < 0) {
		LOG_ERR("ring_write_all(hdr) failed: %d", err);
		return -EIO;
	}
	err = 0;

	for (i = 0; i < param_num; i++) {
		err = ring_write_all(xs, params[i], strlen(params[i]) + 1);
		if (err < 0) {
			LOG_ERR("ring_write_all(path) failed: %d", err);
			break;
		}
		err = 0;
	}

	return err;
}

static ssize_t cmd_exec(struct xenstore_client *xs, enum xsd_sockmsg_type type,
			const char *const *params, size_t params_num, char *buf, size_t len,
			k_timeout_t timeout)
{
	struct xs_buf resp_local = {
		.node = {0},
		.buf = (uint8_t *)buf,
		.len = len,
		.pos = 0,
		.req_id = 0,
	};
	int err;

	k_sem_init(&resp_local.sem, 0, 1);

	err = cmd_req(xs, type, params, params_num, buf, len, &resp_local.req_id);
	if (err < 0) {
		LOG_ERR("xs_rw_common error: %d", err);
		return -EIO;
	}

	k_spinlock_key_t key = k_spin_lock(&xs->lock);
	sys_slist_append(&xs->cmd_q, &resp_local.node);
	k_spin_unlock(&xs->lock, key);

	notify_evtchn(xs->local_evtchn);

	err = k_sem_take(&resp_local.sem, timeout);
	key = k_spin_lock(&xs->lock);
	(void)sys_slist_find_and_remove(&xs->cmd_q, &resp_local.node);
	k_spin_unlock(&xs->lock, key);

	if (err != 0) {
		LOG_ERR("k_sem_take error: %d", err);
		return err;
	}

	if (resp_local.err < 0) {
		LOG_ERR("Error response: %d", resp_local.err);
		return resp_local.err;
	}

	return (resp_local.pos > 0) ? MIN((size_t)(len - 1), resp_local.pos) : 0;
}

int xs_init()
{
	const struct k_work_queue_config qcfg = {.name = "xenstore-wq"};
	uint64_t paddr = 0;
	uint64_t value = 0;
	mm_reg_t vaddr = 0;
	int err;

	if (xs_cli.domint) {
		return 0;
	}

	atomic_set(&xs_cli.next_req_id, 1);

	k_work_init(&xs_cli.isr_work, isr_workhandler);
	k_work_queue_init(&xs_cli.workq);
	k_work_queue_start(&xs_cli.workq, xs_cli.workq_stack,
			   K_THREAD_STACK_SIZEOF(xs_cli.workq_stack), xs_cli.workq_priority, &qcfg);

	sys_slist_init(&xs_cli.notify_list);
	sys_slist_init(&xs_cli.cmd_q);

	err = hvm_get_parameter(HVM_PARAM_STORE_EVTCHN, DOMID_SELF, &value);
	if (err) {
		LOG_ERR("hvm_get_parameter(STORE_EVTCHN) failed: %d", err);
		return -ENODEV;
	}
	xs_cli.local_evtchn = value;

	err = hvm_get_parameter(HVM_PARAM_STORE_PFN, DOMID_SELF, &paddr);
	if (err) {
		LOG_ERR("hvm_get_param(STORE_PFN) failed: err=%d", err);
		return -EIO;
	}
	device_map(&vaddr, XEN_PFN_PHYS(paddr), XEN_PAGE_SIZE, K_MEM_CACHE_WB | K_MEM_PERM_RW);

	xs_cli.domint = (struct xenstore_domain_interface *)vaddr;

	while (ring_avail_for_read(&xs_cli)) {
		ring_read(&xs_cli, NULL, ring_avail_for_read(&xs_cli));
	}

	bind_event_channel(xs_cli.local_evtchn, xs_evtchn_cb, &xs_cli);
	unmask_event_channel(xs_cli.local_evtchn);

	xs_cli.default_timeout = K_FOREVER;

	return 0;
}

void xs_set_default_timeout(k_timeout_t tout)
{
	xs_cli.default_timeout = tout;
}

void xs_watcher_init(struct xs_watcher *w, xs_notify_cb cb, void *param)
{
	if (!w) {
		return;
	}

	w->node.next = NULL;
	w->cb = cb;
	w->param = param;
}

int xs_watcher_register(struct xs_watcher *w)
{
	if (!w || !w->cb) {
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&xs_cli.lock);
	sys_slist_append(&xs_cli.notify_list, &w->node);
	k_spin_unlock(&xs_cli.lock, key);

	return 0;
}

int xs_watcher_unregister(struct xs_watcher *w)
{
	bool removed;

	if (!w) {
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&xs_cli.lock);
	removed = sys_slist_find_and_remove(&xs_cli.notify_list, &w->node);
	k_spin_unlock(&xs_cli.lock, key);

	return removed ? 0 : -ENOENT;
}

ssize_t xs_read_timeout(const char *path, char *buf, size_t len, k_timeout_t tout)
{
	const char *const params[] = {path};

	if (!path || !buf || len == 0) {
		return -EINVAL;
	}

	return cmd_exec(&xs_cli, XS_READ, params, ARRAY_SIZE(params), buf, len, tout);
}

ssize_t xs_read(const char *path, char *buf, size_t len)
{
	return xs_read_timeout(path, buf, len, xs_cli.default_timeout);
}

ssize_t xs_directory_timeout(const char *path, char *buf, size_t len, k_timeout_t tout)
{
	const char *const params[] = {path};

	if (!path || !buf || len == 0) {
		return -EINVAL;
	}

	return cmd_exec(&xs_cli, XS_DIRECTORY, params, ARRAY_SIZE(params), buf, len, tout);
}

ssize_t xs_directory(const char *path, char *buf, size_t len)
{
	return xs_directory_timeout(path, buf, len, xs_cli.default_timeout);
}

ssize_t xs_watch_timeout(const char *path, const char *token, char *buf, size_t len,
			 k_timeout_t tout)
{
	const char *const params[] = {path, token};

	if (!path || !token || !buf || len == 0) {
		return -EINVAL;
	}

	return cmd_exec(&xs_cli, XS_WATCH, params, ARRAY_SIZE(params), buf, len, tout);
}

ssize_t xs_watch(const char *path, const char *token, char *buf, size_t len)
{
	return xs_watch_timeout(path, token, buf, len, xs_cli.default_timeout);
}
