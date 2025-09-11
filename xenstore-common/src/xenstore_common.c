#include <xenstore_common.h>

int xenstore_ring_write(struct xenstore_domain_interface *intf, const void *data, size_t len,
			bool server)
{
	size_t avail;
	void *dest;
	XENSTORE_RING_IDX cons, prod;

	cons = intf->rsp_cons;
	prod = intf->rsp_prod;
	z_barrier_dmem_fence_full();

	if (check_indexes(cons, prod)) {
		return -EINVAL;
	}

	dest = intf->rsp + get_output_offset(cons, prod, &avail);
	if (avail < len) {
		len = avail;
	}

	memcpy(dest, data, len);
	z_barrier_dmem_fence_full();
	intf->rsp_prod += len;

	return len;
}

int xenstore_ring_read(struct xenstore_domain_interface *intf, void *data, size_t len, bool server)
{
	size_t avail;
	const void *src;
	XENSTORE_RING_IDX cons, prod;

	cons = intf->req_cons;
	prod = intf->req_prod;
	z_barrier_dmem_fence_full();

	if (check_indexes(cons, prod)) {
		return -EIO;
	}

	src = intf->req + get_input_offset(cons, prod, &avail);
	if (avail < len) {
		len = avail;
	}

	memcpy(data, src, len);
	z_barrier_dmem_fence_full();
	intf->req_cons += len;

	return len;
}
