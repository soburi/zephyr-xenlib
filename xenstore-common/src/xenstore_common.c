#include <xenstore_common.h>

int xenstore_ring_write(struct xenstore_domain_interface *intf, const void *data, size_t len,
			bool client)
{
	size_t avail;
	void *dest;
	XENSTORE_RING_IDX cons, prod;

	cons = client ? intf->req_cons : intf->rsp_cons;
	prod = client ? intf->req_prod : intf->rsp_prod;
	z_barrier_dmem_fence_full();

	if (check_indexes(cons, prod)) {
		return -EINVAL;
	}

	dest = (client ? intf->req : intf->rsp) + get_output_offset(cons, prod, &avail);
	if (avail < len) {
		len = avail;
	}

	memcpy(dest, data, len);
	z_barrier_dmem_fence_full();
	if (client) {
		intf->req_prod += len;
	} else {
		intf->rsp_prod += len;
	}

	return len;
}

int xenstore_ring_read(struct xenstore_domain_interface *intf, void *data, size_t len, bool client)
{
	size_t avail;
	const void *src;
	XENSTORE_RING_IDX cons, prod;

	cons = client ? intf->rsp_cons : intf->req_cons;
	prod = client ? intf->rsp_prod : intf->req_prod;
	z_barrier_dmem_fence_full();

	if (check_indexes(cons, prod)) {
		return -EIO;
	}

	src = (client ? intf->rsp : intf->req) + get_input_offset(cons, prod, &avail);
	if (avail < len) {
		len = avail;
	}

	if (data) {
		memcpy(data, src, len);
	}

	z_barrier_dmem_fence_full();
	if (client) {
		intf->rsp_cons += len;
	} else {
		intf->req_cons += len;
	}

	return len;
}

int xenstore_get_error(const char *errstr)
{
	size_t i;

	for (i = 0; ARRAY_SIZE(xsd_errors); i++) {
		if (strcmp(errstr, xsd_errors[i].errstring) == 0) {
			return xsd_errors[i].errnum;
		}
	}

	return EINVAL;
}

/* Iterate over NUL-separated string list (double-NUL terminated)
 * Usage:
 *   size_t off = 0; const char *entry;
 *   while ((off = xs_strlist_next(buf, len, off, &entry)) != 0) {
 *       // use entry
 *   }
 */
size_t xenstore_strlist_next(const char *list, size_t len, size_t off, const char **out)
{
	if (!list || !out || off >= len) {
		return 0;
	}

	const char *p = list + off;
	if (*p == '\0') {
		return 0; /* double-NUL reached */
	}

	*out = p;
	size_t i = off;
	while (i < len && list[i] != '\0') {
		i++;
	}
	if (i >= len) {
		return 0;
	}
	/* move past NUL; if next is NUL too, caller will stop on next call */
	return i + 1;
}
