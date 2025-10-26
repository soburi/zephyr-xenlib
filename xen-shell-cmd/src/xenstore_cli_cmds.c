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
		if ((buf[i] == '\0') && ((i + 1) < len)) {
			return &buf[i + 1];
		}
	}

	return NULL;
}

static int cmd_xenstore_list(const struct shell *sh, size_t argc, char **argv)
{
	bool show_path = false;
	bool show_help = false;
	size_t idx = 1;
	char *buffer;

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

	buffer = k_malloc(XENSTORE_PAYLOAD_MAX + 1);

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

	k_free(buffer);

	return 0;
}

static char *space[] = {
	"", " ", "  ", "   ", "    ", "     ", "      ", "       ", "        ", "         ",
};

static int cmd_xenstore_ls_recur(const struct shell *sh, size_t level, const char *path,
				 bool show_full_path, bool show_perms)
{
	char *buffer;
	ssize_t resp_len;
	const char *ptr = NULL;
	int ret;

	buffer = k_malloc(XENSTORE_PAYLOAD_MAX + 1);
	resp_len = xs_directory(path, buffer, XENSTORE_PAYLOAD_MAX, 0);

	if (resp_len < 0) {
		shell_print(sh, "error: %s: %ld", path, resp_len);
		k_free(buffer);
		return (int)resp_len;
	}

	buffer[resp_len] = '\0';
	while ((ptr = xenstore_next_str(ptr, buffer, resp_len))) {

		if (strlen(ptr) == 0) {
			continue;
		}

		char path_buf[255] = {0};

		strcat(path_buf, path);
		strcat(path_buf, "/");
		strcat(path_buf, ptr);

		char *read_buffer = k_malloc(XENSTORE_PAYLOAD_MAX + 1);
		ssize_t read_len = xs_read(path_buf, read_buffer, XENSTORE_PAYLOAD_MAX, 0);

		read_buffer[read_len] = '\0';

		if (show_full_path) {
			shell_print(sh, "%s/%s = \"%s\"", path, ptr, read_buffer);
		} else {
			shell_print(sh, "%s%s = \"%s\"", space[level], ptr, read_buffer);
		}

		ret = cmd_xenstore_ls_recur(sh, level + 1, path_buf, show_full_path, show_perms);
	}

	k_free(buffer);
	return 0;
}

static int cmd_xenstore_ls(const struct shell *sh, size_t argc, char **argv)
{
	bool show_full_path = false;
	bool show_perms = false;
	bool show_help = false;
	size_t idx = 1;

	while (idx < argc) {
		if (strncmp(argv[idx], "-f", 2) == 0) {
			show_full_path = true;
		} else if (strncmp(argv[idx], "-p", 2) == 0) {
			show_perms = true;
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

	return cmd_xenstore_ls_recur(sh, 0, argv[idx], show_full_path, show_perms);
}

static int cmd_xenstore_read(const struct shell *sh, size_t argc, char **argv)
{
	bool show_path = false;
	bool show_help = false;
	bool show_raw = false;
	size_t idx = 1;
	char *buffer;

	while (idx < argc) {
		if (strncmp(argv[idx], "-p", 2) == 0) {
			show_path = true;
		} else if (strncmp(argv[idx], "-R", 2) == 0) {
			show_raw = true;
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

	buffer = k_malloc(XENSTORE_PAYLOAD_MAX + 1);

	for (; idx < argc; idx++) {
		const char *path = argv[idx];
		const ssize_t resp_len = xs_read(path, buffer, XENSTORE_PAYLOAD_MAX, 0);

		if (resp_len < 0) {
			shell_print(sh, "error: %s: %ld", path, resp_len);
			continue;
		}

		if (show_path) {
			shell_print(sh, "%s: %s", path, buffer);
		} else {
			shell_print(sh, "%s", buffer);
		}
	}

	k_free(buffer);

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_xenstore_cmds,
	SHELL_CMD_ARG(list, NULL, "Usage: xenstore list [-h] [-p] key [...]",
		      cmd_xenstore_list, 1, UINT8_MAX),
	SHELL_CMD_ARG(ls, NULL, "Usage: xenstore ls [-h] [-f] [-p] path",
		      cmd_xenstore_ls, 1, 3),
	SHELL_CMD_ARG(read, NULL, "Usage: xenstore read [-h] [-p] [-R] path",
		      cmd_xenstore_read, 1, UINT8_MAX),
	SHELL_SUBCMD_SET_END /* Array terminated. */
);

SHELL_CMD_REGISTER(xenstore, &sub_xenstore_cmds, "XenStore client commands", NULL);
