/*
 * Copyright (c) 2023 EPAM Systems
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef XENSTORE_INTERNAL_H__
#define XENSTORE_INTERNAL_H__

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <xen/public/io/xs_wire.h>

#include <zephyr/sys/barrier.h>
#include <zephyr/sys/slist.h>
#include <zephyr/xen/events.h>

/* Max string length of int32_t + terminating null symbol */
#define INT32_MAX_STR_LEN          (12)
/* Max string length of uint32_t + terminating null symbol */
#define UINT32_MAX_STR_LEN         11
/* max length of string that holds '/local/domain/%domid/' (domid 0-32767) */
#define XENSTORE_MAX_LOCALPATH_LEN 21

#define XENSTORE_DIR_CLIENT 0
#define XENSTORE_DIR_SERVER 1

__maybe_unused static bool is_abs_path(const char *path)
{
	if (!path) {
		return false;
	}

	return path[0] == '/';
}

__maybe_unused static bool is_root_path(const char *path)
{
	return (is_abs_path(path) && (strlen(path) == 1));
}

/*
 * Returns the size of string including terminating NULL symbol.
 */
__maybe_unused static inline size_t str_byte_size(const char *str)
{
	if (!str) {
		return 0;
	}

	return strlen(str) + 1;
}

__maybe_unused static bool check_indexes(XENSTORE_RING_IDX cons, XENSTORE_RING_IDX prod)
{
	return ((prod - cons) > XENSTORE_RING_SIZE);
}

__maybe_unused static size_t get_input_offset(XENSTORE_RING_IDX cons, XENSTORE_RING_IDX prod,
					      size_t *len)
{
	size_t delta = prod - cons;
	*len = XENSTORE_RING_SIZE - MASK_XENSTORE_IDX(cons);

	if (delta < *len) {
		*len = delta;
	}

	return MASK_XENSTORE_IDX(cons);
}

__maybe_unused static size_t get_output_offset(XENSTORE_RING_IDX cons, XENSTORE_RING_IDX prod,
					       size_t *len)
{
	size_t free_space = XENSTORE_RING_SIZE - (prod - cons);

	*len = XENSTORE_RING_SIZE - MASK_XENSTORE_IDX(prod);
	if (free_space < *len) {
		*len = free_space;
	}

	return MASK_XENSTORE_IDX(prod);
}

int xenstore_ring_write(struct xenstore_domain_interface *intf, const void *data, size_t len,
			bool client);
int xenstore_ring_read(struct xenstore_domain_interface *intf, void *data, size_t len, bool client);

int xenstore_get_error(const char *errstr, size_t len);
size_t xenstore_strlist_next(const char *list, size_t len, size_t off, const char **out);

#endif /* XENSTORE_INTERNAL_H__ */
