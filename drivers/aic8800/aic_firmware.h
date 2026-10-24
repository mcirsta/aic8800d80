/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _AIC_FIRMWARE_H
#define _AIC_FIRMWARE_H

#include <linux/errno.h>
#include <linux/limits.h>
#include <linux/sprintf.h>
#include <linux/string.h>

/* Firmware names may contain subdirectories, but never relative traversal. */
static inline bool aicwf_firmware_path_valid(const char *path, size_t len)
{
	const char *component = path;
	const char *limit = path + len;
	const char *end;
	size_t component_len;

	while (component < limit) {
		end = memchr(component, '/', limit - component);
		component_len = end ? end - component : limit - component;
		if (!component_len ||
		    (component_len == 1 && component[0] == '.') ||
		    (component_len == 2 && component[0] == '.' &&
		     component[1] == '.'))
			return false;
		if (!end)
			break;
		component = end + 1;
		if (component == limit)
			return false;
	}
	return true;
}

/*
 * aic_fw_path is a firmware-relative root before the chip directory.  Retain
 * aliases for the old platform roots, but leave custom absolute roots to
 * firmware_class.path rather than opening files through the current task.
 */
static inline int aicwf_firmware_name(char *out, size_t len, const char *prefix,
				     const char *directory, const char *name)
{
	size_t prefix_len, directory_len;
	int ret;

	if (!out || !len)
		return -EINVAL;
	out[0] = '\0';
	if (!prefix || !directory || !name || !name[0] || directory[0] == '/')
		return -EINVAL;

	if (!strncmp(prefix, "/lib/firmware", strlen("/lib/firmware")) &&
	    (prefix[strlen("/lib/firmware")] == '/' ||
	     prefix[strlen("/lib/firmware")] == '\0'))
		prefix += strlen("/lib/firmware");
	else if (!strncmp(prefix, "/usr/lib/firmware",
			  strlen("/usr/lib/firmware")) &&
		 (prefix[strlen("/usr/lib/firmware")] == '/' ||
		  prefix[strlen("/usr/lib/firmware")] == '\0'))
		prefix += strlen("/usr/lib/firmware");
	else if (!strncmp(prefix, "/vendor/etc/firmware",
			  strlen("/vendor/etc/firmware")) &&
		 (prefix[strlen("/vendor/etc/firmware")] == '/' ||
		  prefix[strlen("/vendor/etc/firmware")] == '\0'))
		prefix += strlen("/vendor/etc/firmware");
	else if (prefix[0] == '/')
		return -EINVAL;
	if (prefix[0] == '/')
		prefix++;

	prefix_len = strlen(prefix);
	while (prefix_len && prefix[prefix_len - 1] == '/')
		prefix_len--;
	directory_len = strlen(directory);
	while (directory_len && directory[directory_len - 1] == '/')
		directory_len--;
	if (prefix_len > INT_MAX || directory_len > INT_MAX)
		return -ENAMETOOLONG;
	if (!aicwf_firmware_path_valid(prefix, prefix_len) ||
	    !aicwf_firmware_path_valid(directory, directory_len) ||
	    !aicwf_firmware_path_valid(name, strlen(name)))
		return -EINVAL;

	ret = snprintf(out, len, "%.*s%s%.*s%s%s", (int)prefix_len, prefix,
		       prefix_len ? "/" : "", (int)directory_len, directory,
		       directory_len ? "/" : "", name);
	if (ret < 0 || (size_t)ret >= len) {
		out[0] = '\0';
		return ret < 0 ? ret : -ENAMETOOLONG;
	}
	return 0;
}

#endif /* _AIC_FIRMWARE_H */
