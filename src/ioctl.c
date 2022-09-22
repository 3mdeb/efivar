// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * libefivar - library for the manipulation of EFI variables
 * Copyright 2022 3mdeb <contact@3mdeb.com>
 */

#include "fix_coverity.h"

#ifndef __linux__

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#ifdef __OpenBSD__
#include <dev/efi/efi.h>
#include <dev/efi/efiio.h>
#elif defined(__NetBSD__)
#include <sys/efiio.h>
#else
#include <sys/efi.h>
#include <sys/efiio.h>
#endif

#include "efivar.h"
#include "ucs2.h"

#ifdef __NetBSD__
typedef uint16_t efi_char;
#endif

static int efi_fd = -2;

static int
ioctl_probe(void)
{
	if (efi_fd == -2)
		efi_fd = open("/dev/efi", O_RDWR);
	if (efi_fd < 0)
		efi_fd = -1;
	return (efi_fd >= 0);
}

static int
rv_to_linux_rv(int rv)
{
	if (rv == 0)
		rv = 1;
	else
		rv = -errno;
	return (rv);
}

static int
ioctl_get_variable_size(efi_guid_t guid, const char *name, size_t *size)
{
	// TODO: consider merging with ioctl_get_variable_attributes

	struct efi_var_ioc var = { 0 };
	int errno_value;
	int ret = -1;

	var.namesize = utf8size(name, -1) * sizeof(efi_char);
	memcpy(&var.vendor, &guid, sizeof(uuid_t));

	var.name = malloc(var.namesize);
	if (var.name == NULL)
		return -1;

	if (utf8_to_ucs2(var.name, var.namesize, true, name) == -1)
		goto err;

	ret = rv_to_linux_rv(ioctl(efi_fd, EFIIOC_VAR_GET, &var));
	if (ret >= 0)
		*size = var.datasize;

err:
	errno_value = errno;
	free(var.name);
	errno = errno_value;

	return ret;
}

static int
ioctl_get_variable_attributes(efi_guid_t guid, const char *name,
			      uint32_t *attributes)
{
	// TODO: check that EFI sets attributes when if there is no data

	struct efi_var_ioc var = { 0 };
	int errno_value;
	int ret = -1;

	var.namesize = utf8size(name, -1) * sizeof(efi_char);
	memcpy(&var.vendor, &guid, sizeof(uuid_t));

	var.name = malloc(var.namesize);
	if (var.name == NULL)
		return -1;

	if (utf8_to_ucs2(var.name, var.namesize, true, name) == -1)
		goto err;

	ret = rv_to_linux_rv(ioctl(efi_fd, EFIIOC_VAR_GET, &var));
	if (ret >= 0)
		*attributes = var.attrib;

err:
	errno_value = errno;
	free(var.name);
	errno = errno_value;

	return ret;
}

static int
ioctl_get_variable(efi_guid_t guid, const char *name, uint8_t **data,
		   size_t *data_size, uint32_t *attributes)
{
	struct efi_var_ioc var = { 0 };
	int errno_value;
	int ret = -1;

	var.namesize = utf8size(name, -1) * sizeof(efi_char);
	memcpy(&var.vendor, &guid, sizeof(uuid_t));

	var.name = malloc(var.namesize);
	if (var.name == NULL)
		return -1;

	if (utf8_to_ucs2(var.name, var.namesize, true, name) == -1)
		goto err;

	ret = rv_to_linux_rv(ioctl(efi_fd, EFIIOC_VAR_GET, &var));
	if (ret < 0)
		goto err;

	var.data = malloc(var.datasize);
	if (var.data == NULL)
		goto err;

	ret = rv_to_linux_rv(ioctl(efi_fd, EFIIOC_VAR_GET, &var));
	if (ret >= 0) {
		*attributes = var.attrib;
		*data = var.data;
		*data_size = var.datasize;

		var.data = NULL;
	}

err:
	errno_value = errno;
	free(var.name);
	free(var.data);
	errno = errno_value;

	return ret;
}

static int
ioctl_del_variable(efi_guid_t guid, const char *name)
{
	struct efi_var_ioc var = { 0 };
	int errno_value;
	int ret = -1;

	var.namesize = utf8size(name, -1) * sizeof(efi_char);
	memcpy(&var.vendor, &guid, sizeof(uuid_t));

	var.name = malloc(var.namesize);
	if (var.name == NULL)
		return -1;

	if (utf8_to_ucs2(var.name, var.namesize, true, name) == -1)
		goto err;

	ret = rv_to_linux_rv(ioctl(efi_fd, EFIIOC_VAR_SET, &var));

err:
	errno_value = errno;
	free(var.name);
	errno = errno_value;

	return ret;
}

static int
ioctl_chmod_variable(efi_guid_t guid UNUSED, const char *name UNUSED,
		     mode_t mode UNUSED)
{
	/* Nothing to do. */
	return 0;
}

static int
ioctl_set_variable(efi_guid_t guid, const char *name, uint8_t *data,
		   size_t data_size, uint32_t attributes, mode_t mode UNUSED)
{
	struct efi_var_ioc var = { 0 };
	int errno_value;
	int ret = -1;

	var.namesize = utf8size(name, -1) * sizeof(efi_char);
	memcpy(&var.vendor, &guid, sizeof(uuid_t));
	var.attrib = attributes;
	var.data = data;
	var.datasize = data_size;

	var.name = malloc(var.namesize);
	if (var.name == NULL)
		return -1;

	if (utf8_to_ucs2(var.name, var.namesize, true, name) == -1)
		goto err;

	ret = rv_to_linux_rv(ioctl(efi_fd, EFIIOC_VAR_SET, &var));

err:
	errno_value = errno;
	free(var.name);
	errno = errno_value;

	return ret;
}

static int
ioctl_get_next_variable_name(efi_guid_t **guid, char **name)
{
	static char ret_name[NAME_MAX+1];
	static efi_guid_t ret_guid;

	efi_char tmp_name[NAME_MAX+1];
	char *utf8_name;

	struct efi_var_ioc var = { 0 };
	int ret;

	if (!guid || !name) {
		errno = EINVAL;
		efi_error("invalid arguments");
		return -1;
	}

	if (*name != NULL) {
		if (utf8_to_ucs2(tmp_name, sizeof(tmp_name), true, *name) == -1)
			return -1;
	} else {
		tmp_name[0] = 0;
	}
	if (*guid != NULL)
		memcpy(&var.vendor, *guid, sizeof(uuid_t));

	var.name = tmp_name;
	var.namesize = sizeof(tmp_name);

	ret = rv_to_linux_rv(ioctl(efi_fd, EFIIOC_VAR_NEXT, &var));
	if (ret < 0) {
		return (errno == ENOENT ? 0 : ret);
	}

	utf8_name = ucs2_to_utf8(var.name, -1);
	if (utf8_name == NULL)
		return -1;

	if (strlen(utf8_name) > NAME_MAX) {
		free(utf8_name);
		errno = ENOMEM;
		return -1;
	}

	strcpy(ret_name, utf8_name);
	memcpy(&ret_guid, &var.vendor, sizeof(uuid_t));

	free(utf8_name);

	*name = ret_name;
	*guid = &ret_guid;
	return ret;
}

struct efi_var_operations ioctl_ops = {
	.name = "ioctl",
	.probe = ioctl_probe,
	.set_variable = ioctl_set_variable,
	.del_variable = ioctl_del_variable,
	.get_variable = ioctl_get_variable,
	.get_variable_attributes = ioctl_get_variable_attributes,
	.get_variable_size = ioctl_get_variable_size,
	.get_next_variable_name = ioctl_get_next_variable_name,
	.chmod_variable = ioctl_chmod_variable,
};

#else

#include "lib.h"

static int
ioctl_probe(void)
{
	return 0;
}

struct efi_var_operations ioctl_ops = {
	.name = "ioctl",
	.probe = ioctl_probe,
	.set_variable = NULL,
	.append_variable = NULL,
	.del_variable = NULL,
	.get_variable = NULL,
	.get_variable_attributes = NULL,
	.get_variable_size = NULL,
	.get_next_variable_name = NULL,
	.chmod_variable = NULL,
};

#endif /* __linux__ */

// vim:fenc=utf-8:tw=75:noet
