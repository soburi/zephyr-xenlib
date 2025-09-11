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

#define SZ_SOCKMSG  sizeof(struct xsd_sockmsg)
#define SZ_FRAME(h) (SZ_SOCKMSG + h->len)

struct xenstore_response {
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

	k_timeout_t default_timeout;
	atomic_t next_req_id;
	size_t to_discard;

	struct k_spinlock lock;

	/** Headers of the currently processed payload */
	uint8_t hdr_buf[SZ_SOCKMSG];
	size_t hdr_pos;

	sys_slist_t resp_list;

	sys_slist_t notify_list;
	uint8_t notify_frame[XENSTORE_PAYLOAD_MAX + 1];
	struct xenstore_response notify;

	K_KERNEL_STACK_MEMBER(workq_stack, CONFIG_XENSTORE_CLI_WORKQ_STACK_SIZE);
	struct k_work event_work;
	struct k_work_q workq;
	int workq_priority;
};

static struct xenstore_client xs_cli;

static inline uint32_t alloc_req_id(void)
{
	uint32_t id = (atomic_inc(&xs_cli.next_req_id) & UINT32_MAX);

	/* id=0 is reserved for watch notification */
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

		written += rc;
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

static struct xenstore_response *find_response(struct xenstore_client *xs, uint32_t req_id)
{
	struct xenstore_response *found = NULL;
	k_spinlock_key_t key;
	sys_snode_t *n;

	key = k_spin_lock(&xs->lock);

	for (n = sys_slist_peek_head(&xs->resp_list); n; n = sys_slist_peek_next(n)) {
		struct xenstore_response *resp = CONTAINER_OF(n, struct xenstore_response, node);

		if (resp->req_id == req_id) {
			found = resp;
			break;
		}
	}

	k_spin_unlock(&xs->lock, key);

	return found;
}

/*
 * Read enough of the response header to determine which response context to use,
 * returning NULL when the header is still incomplete.
 */
static struct xenstore_response *prepare_response(struct xenstore_client *xs, size_t *avail)
{
	struct xsd_sockmsg *hdr = (struct xsd_sockmsg *)(xs->hdr_buf);
	struct xenstore_response *resp;
	int ret;

	LOG_DBG("avail=%zu hdr_pos=%zu", *avail, xs->hdr_pos);

	if (xs->hdr_pos < SZ_SOCKMSG) {
		const size_t hdr_to_read = MIN(SZ_SOCKMSG - xs->hdr_pos, *avail);

		ret = ring_read(xs, xs->hdr_buf + xs->hdr_pos, hdr_to_read);
		if (ret < 0) {
			LOG_ERR("ring_read failed: %d", ret);
			return NULL;
		}

		xs->hdr_pos += ret;
		*avail -= ret;

		if (xs->hdr_pos < SZ_SOCKMSG) {
			LOG_DBG("header not ready");
			return NULL;
		}
	}

	if (hdr->type != XS_WATCH_EVENT) {
		resp = find_response(xs, hdr->req_id);
	} else {
		resp = &xs->notify;

		if (resp->pos == 0) {
			resp->node.next = NULL;
			resp->buf = xs->notify_frame;
			resp->len = hdr->len;
			resp->pos = 0;
			resp->req_id = hdr->req_id;
		}
	}

	return resp;
}

static int read_payload(struct xenstore_client *xs, struct xenstore_response *resp, size_t avail)
{
	struct xsd_sockmsg *hdr = (struct xsd_sockmsg *)(xs->hdr_buf);
	int ret;

	if (hdr->len > XENSTORE_PAYLOAD_MAX) {
		LOG_ERR("payload too large: %u > " STRINGIFY(XENSTORE_PAYLOAD_MAX), hdr->len);
		ret = -EMSGSIZE;
	} else if ((hdr->type != XS_WATCH_EVENT) && (hdr->req_id == 0)) {
		LOG_ERR("Invalid response header: req_id must be non-zero");
		ret = -EPROTO;
	} else if ((hdr->type == XS_WATCH_EVENT) && (hdr->req_id != 0)) {
		LOG_ERR("Invalid watch header: req_id=%u (expected 0)", hdr->req_id);
		ret = -EPROTO;
	} else if (resp == NULL) {
		LOG_ERR("No pending context for req_id=%u (type=%u)", hdr->req_id, hdr->type);
		ret = -EINVAL;
	} else if ((hdr->type != XS_WATCH_EVENT) && (hdr->len > resp->len)) {
		LOG_ERR("Response buffer too small: need %u bytes, have %zu", hdr->len, resp->len);
		ret = -EMSGSIZE;
	} else {
		ret = 0;
	}

	if (ret == 0) {
		const size_t remaining = hdr->len - resp->pos;
		const size_t room = (resp->len > resp->pos) ? resp->len - resp->pos : 0;
		const size_t to_read = MIN(MIN(remaining, avail), room);

		ret = ring_read(xs, resp->buf + resp->pos, to_read);
		if (ret < 0) {
			LOG_ERR("ring_read failed while fetching type=%u req_id=%u: %d", hdr->type,
				hdr->req_id, ret);
		} else {
			resp->pos += ret;

			if (ret < to_read) {
				return -EAGAIN; /* Wait for more data */
			}
		}
	}

	if (ret < 0) {
		if (resp && (hdr->type != XS_WATCH_EVENT)) {
			resp->err = ret;
			k_sem_give(&resp->sem);
		}
		xs->to_discard = hdr->len - MIN(resp ? resp->pos : 0, hdr->len);
		xs->hdr_pos = 0;
		if (resp) {
			resp->pos = 0;
		}

		return ret;
	}

	if (resp->pos < hdr->len) {
		return -EAGAIN; /* Wait for more data */
	}

	if (resp->len > 0) {
		size_t nul_index = (resp->pos < resp->len) ? resp->pos : (resp->len - 1);

		/* Ensure callers always see a NUL-terminated payload. */
		resp->buf[nul_index] = '\0';
	}

	return 0;
}

/*
 * Complete the response lifecycle: deliver errors to waiting threads or
 * dispatch watch callbacks. The header has already been fully read at this point.
 */
static void finalize_response(struct xenstore_client *xs, struct xenstore_response *resp)
{
	struct xsd_sockmsg *hdr = (struct xsd_sockmsg *)(xs->hdr_buf);

	if (hdr->type != XS_WATCH_EVENT) {
		if (hdr->type == XS_ERROR) {
			resp->err = xenstore_get_error(resp->buf, MIN(resp->pos, resp->len));
		} else {
			resp->err = 0;
		}

		k_sem_give(&resp->sem);
	} else {
		sys_snode_t *node;
		const char *path = "";
		const char *token = "";
		size_t payload_len = resp->pos;

		if (resp->pos > 0) {
			const char *payload = resp->buf;
			const char *sep = memchr(payload, '\0', payload_len);

			if (sep) {
				size_t token_len = resp->pos - (size_t)(sep - payload) - 1;

				path = payload;
				token = (token_len > 0) ? sep + 1 : "";
			} else {
				/* Malformed watch payload – hand the raw buffer back as the path.
				 */
				path = payload;
			}
		}

		SYS_SLIST_FOR_EACH_NODE(&xs->notify_list, node) {
			struct xs_watcher *w = CONTAINER_OF(node, struct xs_watcher, node);

			if (w->cb) {
				w->cb(path, token, w->param);
			}
		}

		xs->notify.pos = 0;
		xs->notify.len = 0;
	}

	xs->hdr_pos = 0;
	xs->to_discard = 0;
}

/* Drop any unread bytes left in the ring after a failed transfer. */
static int drain_ring(struct xenstore_client *xs)
{
	int ret = 0;

	if (xs->to_discard) {
		LOG_DBG("Draining %zu pending bytes", xs->to_discard);

		ret = ring_read(xs, NULL, xs->to_discard);
		if (ret < 0) {
			LOG_ERR("Failed to drain %zu pending bytes: %d", xs->to_discard, ret);
			return ret;
		}

		xs->to_discard -= (ret < xs->to_discard) ? ret : xs->to_discard;
	}

	return ret;
}

static void event_work_handler(struct k_work *work)
{
	struct xenstore_client *xs = CONTAINER_OF(work, struct xenstore_client, event_work);
	struct xenstore_response *resp;
	size_t avail;
	int ret;

	while ((avail = ring_avail_for_read(xs))) {
		ret = drain_ring(xs);
		if (ret < 0) {
			break;
		}

		avail -= ret;
		if (avail == 0) {
			continue;
		}

		resp = prepare_response(xs, &avail);
		if (!resp) {
			continue;
		}

		ret = read_payload(xs, resp, avail);

		if (ret == -EAGAIN) {
			continue;
		}

		if (ret == 0) {
			finalize_response(xs, resp);
		}

		ret = drain_ring(xs);
		if (ret < 0) {
			break;
		}
	}
}

static void event_callback(void *ptr)
{
	struct xenstore_client *xs = ptr;

	k_work_submit_to_queue(&xs->workq, &xs->event_work);
}

static int submit_request(struct xenstore_client *xs, enum xsd_sockmsg_type type,
			  const char *const *params, size_t param_num, char *buf, size_t len,
			  uint32_t *preq_id)
{
	struct xsd_sockmsg hdr = {0};
	size_t plen = 0;
	size_t avail;
	size_t i;
	int err;

	for (i = 0; i < param_num; i++) {
		plen += strlen(params[i]) + 1;
	}

	if (plen > XENSTORE_PAYLOAD_MAX) {
		LOG_ERR("payload too large: %zu > " STRINGIFY(XENSTORE_PAYLOAD_MAX), plen);
		return -ENAMETOOLONG;
	}

	*preq_id = alloc_req_id();

	hdr.type = type;
	hdr.req_id = *preq_id;
	hdr.tx_id = 0;
	hdr.len = plen;

	avail = ring_avail_for_write(xs);

	if (avail < (SZ_SOCKMSG + plen)) {
		LOG_ERR("ring_write: nospace: %zu < %zu", avail, SZ_SOCKMSG + plen);
		return -EAGAIN;
	}

	err = ring_write_all(xs, &hdr, sizeof(struct xsd_sockmsg));
	if (err < 0) {
		LOG_ERR("ring_write_all(hdr) failed: %d", err);
		return -EIO;
	}

	for (i = 0; i < param_num; i++) {
		err = ring_write_all(xs, params[i], strlen(params[i]) + 1);
		if (err < 0) {
			LOG_ERR("ring_write_all(path) failed: %d", err);
			return err;
		}
	}

	return 0;
}

static ssize_t execute_request(struct xenstore_client *xs, enum xsd_sockmsg_type type,
			       const char *const *params, size_t params_num, char *buf, size_t len,
			       k_timeout_t timeout)
{
	struct xenstore_response resp_local = {
		.node = {0},
		.buf = (uint8_t *)buf,
		.len = len,
		.pos = 0,
		.req_id = 0,
	};
	k_spinlock_key_t key;
	int err;

	k_sem_init(&resp_local.sem, 0, 1);

	/*
	 * Allocate a unique request id, enqueue the response descriptor, and wake the
	 * backend. The semaphore is owned by this stack frame; the worker signals it
	 * when the reply is ready.
	 */
	err = submit_request(xs, type, params, params_num, buf, len, &resp_local.req_id);
	if (err < 0) {
		LOG_ERR("Failed to submit request: %d", err);
		return err;
	}

	key = k_spin_lock(&xs->lock);
	sys_slist_append(&xs->resp_list, &resp_local.node);
	k_spin_unlock(&xs->lock, key);

	notify_evtchn(xs->local_evtchn);

	err = k_sem_take(&resp_local.sem, timeout);

	/* Always remove the tracking node, even on timeout or error. */
	key = k_spin_lock(&xs->lock);
	(void)sys_slist_find_and_remove(&xs->resp_list, &resp_local.node);
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

int xs_init(void)
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
	xs_cli.workq_priority = CONFIG_XENSTORE_CLI_WORKQ_PRIORITY;

	k_work_init(&xs_cli.event_work, event_work_handler);
	k_work_queue_init(&xs_cli.workq);
	k_work_queue_start(&xs_cli.workq, xs_cli.workq_stack,
			   K_THREAD_STACK_SIZEOF(xs_cli.workq_stack), xs_cli.workq_priority, &qcfg);

	sys_slist_init(&xs_cli.notify_list);
	sys_slist_init(&xs_cli.resp_list);

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
		(void)ring_read(&xs_cli, NULL, ring_avail_for_read(&xs_cli));
	}

	bind_event_channel(xs_cli.local_evtchn, event_callback, &xs_cli);
	unmask_event_channel(xs_cli.local_evtchn);

	xs_cli.default_timeout = K_FOREVER;

	return 0;
}

void xs_set_default_timeout(k_timeout_t tout)
{
	xs_cli.default_timeout = tout;
}

void xs_watcher_init(struct xs_watcher *w, xs_watch_cb cb, void *param)
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
	k_spinlock_key_t key;

	if (!w || !w->cb) {
		return -EINVAL;
	}

	key = k_spin_lock(&xs_cli.lock);
	sys_slist_append(&xs_cli.notify_list, &w->node);
	k_spin_unlock(&xs_cli.lock, key);

	return 0;
}

ssize_t xs_read_timeout(const char *path, char *buf, size_t len, k_timeout_t tout)
{
	const char *const params[] = {path};

	if (!path || !buf || len == 0) {
		return -EINVAL;
	}

	return execute_request(&xs_cli, XS_READ, params, ARRAY_SIZE(params), buf, len, tout);
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

	return execute_request(&xs_cli, XS_DIRECTORY, params, ARRAY_SIZE(params), buf, len, tout);
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

	return execute_request(&xs_cli, XS_WATCH, params, ARRAY_SIZE(params), buf, len, tout);
}

ssize_t xs_watch(const char *path, const char *token, char *buf, size_t len)
{
	return xs_watch_timeout(path, token, buf, len, xs_cli.default_timeout);
}
