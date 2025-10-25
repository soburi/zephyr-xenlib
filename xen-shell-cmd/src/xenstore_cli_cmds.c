/*
 * Copyright (c) 2025 TOKITA Hiroshi
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/shell/shell.h>

#include <stdlib.h>
#include <string.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <xenstore_cli.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(xenstore_shell, CONFIG_LOG_DEFAULT_LEVEL);

char buffer[XENSTORE_PAYLOAD_MAX + 1];

/**
 * Get the nth string from a null-separated string buffer
 */
static const char *xenstore_next_str(const char *current, const char *buf, size_t len)
{
	int idx = current - buf;

	if (current == NULL) {
		return buf;
	}

	if (idx < 0 || idx > len) {
		return NULL;
	}

	for (size_t i = (current - buf) + 1; i < len; i++) {
		if (buf[i] == '\0' && (i + 1) < len) {
			return &buf[i + 1];
		}
	}

	return NULL;
}

static int cmd_xenstore_list(const struct shell *sh, size_t argc, char **argv)
{
	bool show_path = false;
	bool show_help = true;
	size_t idx = 1;

	while (idx < argc) {
		if (strncmp(argv[idx], "-p", 2) == 0) {
			show_path = true;
		} else if (strncmp(argv[idx], "-h", 2) == 0) {
			show_help = true;
		} else {
			break;
		}

		idx++;
	}

	if (show_help || idx == argc) {
		shell_help(sh);
		return 0;
	}

	for (; idx < argc; idx++) {
		const ssize_t resp_len = xs_directory(argv[idx], buffer, XENSTORE_PAYLOAD_MAX, 0);
		const char *path = argv[idx];
		const char *ptr = NULL;

		if (resp_len < 0) {
			shell_print(sh, "error: %s: %ld", argv[idx], resp_len);
			continue;
		}

		buffer[resp_len] = '\0';
		while ((ptr = xenstore_next_str(ptr, buffer, resp_len))) {
			if (show_path) {
				shell_print(sh, "%s/%s", path, ptr);
			} else {
				shell_print(sh, "%s", ptr);
			}
		}
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_xenstore_cmds,
			       SHELL_CMD_ARG(list, NULL, "Usage: xenstore list [-h] [-p] key [...]",
					     cmd_xenstore_list, 1, UINT8_MAX),
			       SHELL_SUBCMD_SET_END /* Array terminated. */
);

SHELL_CMD_REGISTER(xenstore, &sub_xenstore_cmds, "XenStore client commands", NULL);
