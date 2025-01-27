// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * libefiboot - library for the manipulation of EFI boot variables
 * Copyright 2012-2015 Red Hat, Inc.
 */

#include "fix_coverity.h"

#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>

#ifdef __NetBSD__
#include <sys/disk.h>
#include <sys/disklabel.h>
#endif

#include "efiboot.h"
#include "mntent_compat.h"

static int NONNULL(1, 2, 3)
find_file(const char * const filepath, char **devicep, char **relpathp)
{
	struct stat fsb = { 0, };
	int rc;
	int ret = -1;
	FILE *mounts = NULL;
	char linkbuf[PATH_MAX+1] = "";
	ssize_t linklen = 0;

	linklen = strlen(filepath);
	if (linklen > PATH_MAX) {
		errno = ENAMETOOLONG;
		efi_error("filepath length exceeds PATH_MAX");
		return -1;
	}
	strcpy(linkbuf, filepath);

	do {
		rc = stat(linkbuf, &fsb);
		if (rc < 0)
			return rc;

		if (S_ISLNK(fsb.st_mode)) {
			char tmp[PATH_MAX+1] = "";
			ssize_t l;

			l = readlink(linkbuf, tmp, PATH_MAX);
			if (l < 0) {
				efi_error("readlink failed");
				return -1;
			}
			tmp[l] = '\0';
			linklen = l;
			strcpy(linkbuf, tmp);
		} else {
			break;
		}
	} while (1);

	mounts = setmntent("/proc/self/mounts", "r");
	if (mounts == NULL) {
		efi_error("couldn not open /proc/self/mounts");
		return -1;
	}

	struct mntent *me;
	while (1) {
		char *devpath;
		struct stat dsb = { 0, };

		errno = 0;
		me = getmntent(mounts);
		if (!me) {
#ifdef __linux__
			if (feof(mounts)) {
#else
			{
#endif
				errno = ENOENT;
				efi_error("could not find mountpoint");
			}
			goto err;
		}

		devpath = me->mnt_fsname;
		if (devpath[0] != '/') {
			if (asprintfa(&devpath, "/dev/%s", me->mnt_fsname) < 0) {
				efi_error("could not allocate buffer");
				return -1;
			}
		}

		rc = stat(devpath, &dsb);
		if (rc < 0) {
			if (errno == ENOENT)
				continue;
			efi_error("could not stat mountpoint");
			goto err;
		}

#if !defined(__FreeBSD__) && !defined(__DragonFly__)
		if (!S_ISBLK(dsb.st_mode))
			continue;
#endif

		if (dsb.st_rdev == fsb.st_dev) {
			ssize_t mntlen = strlen(me->mnt_dir);
			if (mntlen >= linklen)
				continue;
			if (strncmp(linkbuf, me->mnt_dir, mntlen))
				continue;

#ifdef __NetBSD__
			/*
			 * Use "raw" versions of devices because they shouldn't be kept
			 * busy by drivers.
			 */
			if (strncmp(devpath, "/dev/dk", 7) == 0) {
				if (asprintf(devicep, "/dev/r%s", devpath + 5) < 0)
					*devicep = NULL;
			} else {
				*devicep = strdup(devpath);
			}
#else
			*devicep = strdup(devpath);
#endif

			if (!*devicep) {
				errno = ENOMEM;
				efi_error("strdup failed");
				goto err;
			}
			*relpathp = strdup(linkbuf + mntlen);
			if (!*relpathp) {
				free(*devicep);
				*devicep = NULL;
				errno = ENOMEM;
				efi_error("strdup failed");
				goto err;
			}
			ret = 0;
			break;
		}
	}
err:
	if (mounts) {
		(void)endmntent(mounts);
	}
	return ret;
}

static int
open_disk(struct device *dev, int flags)
{
	char *diskpath = NULL;
	int rc;

	rc = asprintfa(&diskpath, "/dev/%s", dev->disk_name);
	if (rc < 0) {
		efi_error("could not allocate buffer");
		return -1;
	}

	rc = open(diskpath, flags);
	if (rc < 0)
		efi_error("could not open disk");

	return rc;
}

static char *
tilt_slashes(char *s)
{
	char *p;
	for (p = s; *p; p++)
		if (*p == '/')
			*p = '\\';
	return s;
}

ssize_t
efi_va_generate_file_device_path_from_esp(uint8_t *buf, ssize_t size,
				       const char *devpath, int partition,
				       const char *relpath,
				       uint32_t options, va_list ap)
{
	ssize_t ret = -1, off = 0, sz;
	struct device *dev = NULL;
	int fd = -1;
	int saved_errno;

	debug("partition:%d", partition);

	if (buf && size)
		memset(buf, '\0', size);

	fd = open(devpath, O_RDONLY);
	if (fd < 0) {
		efi_error("could not open device for ESP");
		goto err;
	}

	dev = device_get(devpath, fd, partition);
	if (dev == NULL) {
		efi_error("could not get ESP disk info");
		goto err;
	}

	if (partition < 0) {
		int disk_fd;

		debug("partition: %d", partition);
		disk_fd = open_disk(dev,
				    (options & EFIBOOT_OPTIONS_WRITE_SIGNATURE)
				     ? O_RDWR : O_RDONLY);
		if (disk_fd < 0) {
			efi_error("could not open disk");
			goto err;
		}

		if (is_partitioned(disk_fd))
			partition = 1;
		else
			partition = 0;
		debug("is_partitioned(): partition -> %d", partition);

		close(disk_fd);
	}

	set_part(dev, partition);

	if (partition == 0) {
		options |= EFIBOOT_ABBREV_NONE;
		options &= ~(EFIBOOT_ABBREV_HD|
			     EFIBOOT_ABBREV_FILE|
			     EFIBOOT_ABBREV_EDD10);
	}

	if (options & EFIBOOT_ABBREV_NONE)
		debug("EFIBOOT_ABBREV_NONE");
	if (options & EFIBOOT_ABBREV_HD)
		debug("EFIBOOT_ABBREV_HD");
	if (options & EFIBOOT_ABBREV_FILE)
		debug("EFIBOOT_ABBREV_FILE");
	if (options & EFIBOOT_ABBREV_EDD10)
		debug("EFIBOOT_ABBREV_EDD10");

	if (options & EFIBOOT_ABBREV_EDD10) {
		va_list aq;
		va_copy(aq, ap);

		dev->edd10_devicenum = va_arg(aq, uint32_t);

		va_end(aq);
	}

	if (!(options & (EFIBOOT_ABBREV_FILE|EFIBOOT_ABBREV_HD))
	    && (dev->flags & DEV_ABBREV_ONLY)) {
		efi_error_clear();
		errno = EINVAL;
		efi_error("Device must use File() or HD() device path");
		goto err;
	}

	if ((options & EFIBOOT_ABBREV_EDD10)
			&& (!(options & EFIBOOT_ABBREV_FILE)
			    && !(options & EFIBOOT_ABBREV_HD))) {
		sz = efidp_make_edd10(buf, size, dev->edd10_devicenum);
		if (sz < 0) {
			efi_error("could not make EDD 1.0 device path");
			goto err;
		}
		off = sz;
	} else if (!(options & EFIBOOT_ABBREV_FILE)
		   && !(options & EFIBOOT_ABBREV_HD)) {

		/*
		 * We're probably on a modern kernel, so just parse the
		 * symlink from /sys/dev/block/$major:$minor and get it
		 * from there.
		 */
		sz = make_blockdev_path(buf, size, dev);
		if (sz < 0) {
			efi_error("could not create device path");
			goto err;
		}
		off += sz;
	}

	if ((!(options & EFIBOOT_ABBREV_FILE) && dev->part_name) ||
	    ((options & EFIBOOT_ABBREV_HD) && ! dev->part_name)) {
		int disk_fd;
		int saved_errno;

		disk_fd = open_disk(dev,
				    (options & EFIBOOT_OPTIONS_WRITE_SIGNATURE)
				     ? O_RDWR : O_RDONLY);
		if (disk_fd < 0) {
			efi_error("could not open disk");
			goto err;
		}

		sz = make_hd_dn(buf+off, size?size-off:0,
				disk_fd, dev->part, options);
		saved_errno = errno;
		close(disk_fd);
		errno = saved_errno;
		if (sz < 0) {
			efi_error("could not make HD() DP node");
			goto err;
		}
		off += sz;
	}

	char *filepath = strdupa(relpath);
	tilt_slashes(filepath);
	sz = efidp_make_file(buf+off, size?size-off:0, filepath);
	if (sz < 0) {
		efi_error("could not make File() DP node");
		goto err;
	}
	off += sz;

	sz = efidp_make_end_entire(buf+off, size?size-off:0);
	if (sz < 0) {
		efi_error("could not make EndEntire DP node");
		goto err;
	}
	off += sz;
	ret = off;
err:
	saved_errno = errno;
	if (dev)
		device_free(dev);
	if (fd >= 0)
		close(fd);
	errno = saved_errno;
	debug("= %zd", ret);
	return ret;
}

ssize_t NONNULL(3, 5) PUBLIC
efi_generate_file_device_path_from_esp(uint8_t *buf, ssize_t size,
				       const char *devpath, int partition,
				       const char *relpath,
				       uint32_t options, ...)
{
	ssize_t ret;
	int saved_errno;
	va_list ap;

	va_start(ap, options);
	ret = efi_va_generate_file_device_path_from_esp(buf, size, devpath,
							partition, relpath,
							options, ap);
	saved_errno = errno;
	va_end(ap);
	errno = saved_errno;
	if (ret < 0)
		efi_error("could not generate File DP from ESP");
	return ret;
}

static int
get_part(char *devpath)
{
#ifdef __linux__
	int fd;
	int partition = -1;
	struct device *dev = NULL;

	fd = open(devpath, O_RDONLY);
	if (fd < 0) {
		efi_error("could not open device for ESP");
		goto err;
	}

	dev = device_get(devpath, fd, -1);
	if (dev == NULL) {
		efi_error("could not get ESP disk info");
		goto err;
	}

	partition = dev->part;
	if (partition < 0)
		partition = 0;
err:
	if (dev)
		device_free(dev);
	if (fd >= 0)
		close(fd);
	return partition;
#elif defined(__NetBSD__)
	int fd;
	uint16_t i;
	int partition;
	char *parent_devpath;
	struct disklabel dl;
	struct dkwedge_info dkw;

	/* Handle wedges. */
	if (strncmp(devpath, "/dev/rdk", 8) != 0) {
		efi_error("slice devices aren't supported: %s", devpath);
		return -1;
	}

	fd = open(devpath, O_RDONLY);
	if (fd < 0) {
		efi_error("could not open device: %s", devpath);
		return -1;
	}

	if (ioctl(fd, DIOCGWEDGEINFO, &dkw) == -1) {
		close(fd);
		efi_error("could not query wedge's info");
		return -1;
	}

	close(fd);

	if (asprintfa(&parent_devpath, "/dev/r%s", dkw.dkw_parent) < 0) {
		efi_error("could not allocate buffer");
		return -1;
	}

	fd = open(parent_devpath, O_RDONLY);

	if (fd < 0) {
		efi_error("could not open device: %s", parent_devpath);
		return -1;
	}

	if (ioctl(fd, DIOCGDINFO, &dl) == -1) {
		close(fd);
		efi_error("could not obtain disklabel info");
		return -1;
	}

	for (i = 0; i < dl.d_npartitions; ++i) {
		if (dl.d_partitions[i].p_offset == dkw.dkw_offset) {
			close(fd);
			return 1 + i;
		}
	}

	partition = gpt_disk_find_partition_num(fd, dkw.dkw_offset, dl.d_secsize);
	close(fd);

	if (partition < 0) {
		efi_error("could not find partition number");
		return -1;
	}

	return partition;
#elif defined(__OpenBSD__)
	int n;
	int part_num;
	int partition;
	int fd;
	char *parent_devpath;
	struct disklabel dl;
	unsigned long long start;

	n = strcspn(devpath, "0123456789");
	if (!isdigit(devpath[n])) {
		efi_error("could not find number in device name: %s", devpath);
		return -1;
	}

	n += strspn(devpath + n, "0123456789");
	if (devpath[n] == '\0') {
		efi_error("device doesn't specify a partition: %s", devpath);
		return -1;
	}

	if (find_parent_devpath(devpath, &parent_devpath) < 0) {
		efi_error("could not find parent of device: %s", devpath);
		return -1;
	}

	fd = open(parent_devpath, O_RDONLY);
	free(parent_devpath);

	if (fd < 0) {
		efi_error("could not open parent device: %s", parent_devpath);
		return -1;
	}

	if (ioctl(fd, DIOCGPDINFO, &dl) == -1) {
		close(fd);
		efi_error("could not obtain disklabel info");
		return -1;
	}

	part_num = devpath[n] - 'a';

	if (part_num >= dl.d_npartitions) {
		close(fd);
		efi_error("can't determine partition number of: %s", devpath);
		return -1;
	}

	/* Partition info might be missing even for existing partitions... */
	if (dl.d_partitions[part_num].p_fstype == FS_UNUSED) {
		close(fd);
		efi_error("can't determine partition number of: %s", devpath);
		return -1;
	}

	start = dl.d_partitions[part_num].p_offseth;
	start <<= 32;
	start += dl.d_partitions[part_num].p_offset;

	partition = gpt_disk_find_partition_num(fd, start, dl.d_secsize);
	close(fd);

	if (partition < 0) {
		efi_error("could not find partition number");
		return -1;
	}

	return partition;
#elif defined(__FreeBSD__)
	/* TODO: figure out if 's' is possible here. */
	char *s = strrchr(devpath, 'p');
	if (s == NULL) {
		efi_error("could not find p in device name: %s", devpath);
		return -1;
	}

	if (!isdigit(s[1])) {
		efi_error("could not find slice number in device name: %s", devpath);
		return -1;
	}

	return 1 + atoi(s + 1);
#elif defined(__DragonFly__)
	/* TODO: figure out if 'p' is possible here. */
	char *s = strrchr(devpath, 's');
	if (s == NULL) {
		efi_error("could not find s in device name: %s", devpath);
		return -1;
	}

	if (!isdigit(s[1])) {
		efi_error("could not find slice number in device name: %s", devpath);
		return -1;
	}

	return 1 + atoi(s + 1);
#else
#error "No implementation for the platform"
#endif
}

ssize_t NONNULL(3) PUBLIC
efi_generate_file_device_path(uint8_t *buf, ssize_t size,
			      const char * const filepath,
			      uint32_t options, ...)
{
	int rc;
	ssize_t ret = -1;
	char *child_devpath = NULL;
	char *parent_devpath = NULL;
	char *relpath = NULL;
	va_list ap;
	int saved_errno;

	rc = find_file(filepath, &child_devpath, &relpath);
	if (rc < 0) {
		efi_error("could not canonicalize fs path");
		goto err;
	}

	rc = find_parent_devpath(child_devpath, &parent_devpath);
	if (rc < 0) {
		efi_error("could not find parent device for file");
		goto err;
	}
	debug("child_devpath:%s", child_devpath);

	debug("parent_devpath:%s", parent_devpath);
	debug("child_devpath:%s", child_devpath);
	debug("rc:%d", rc);

	rc = get_part(child_devpath);
	if (rc < 0) {
		efi_error("Couldn't get partition number for %s",
			  child_devpath);
		goto err;
	}
	debug("detected partition:%d", rc);

	va_start(ap, options);

	if (!strcmp(parent_devpath, "/dev/block"))
		ret = efi_va_generate_file_device_path_from_esp(buf, size,
							child_devpath, rc,
							relpath, options, ap);
	else
		ret = efi_va_generate_file_device_path_from_esp(buf, size,
							parent_devpath, rc,
							relpath, options, ap);
	saved_errno = errno;
	va_end(ap);
	errno = saved_errno;
	if (ret < 0)
		efi_error("could not generate File DP from ESP");
err:
	saved_errno = errno;
	if (child_devpath)
		free(child_devpath);
	if (parent_devpath)
		free(parent_devpath);
	if (relpath)
		free(relpath);
	errno = saved_errno;
	return ret;
}

static ssize_t NONNULL(3, 4, 5, 6)
make_ipv4_path(uint8_t *buf, ssize_t size,
	       const char * const local_addr UNUSED,
	       const char * const remote_addr UNUSED,
	       const char * const gateway_addr UNUSED,
	       const char * const netmask UNUSED,
	       uint16_t local_port UNUSED,
	       uint16_t remote_port UNUSED,
	       uint16_t protocol UNUSED,
	       uint8_t addr_origin UNUSED)
{
	ssize_t ret;

#if 0
	if (local_addr == NULL || remote_addr == NULL ||
	    gateway_addr == NULL || netmask == NULL) {
		errno = EINVAL;
		return -1;
	}
#endif
	ret = efidp_make_ipv4(buf, size, 0, 0, 0, 0, 0, 0, 0, 0);
	if (ret < 0)
		efi_error("could not make ipv4 DP node");
	return ret;
}

ssize_t NONNULL(3, 4, 5, 6, 7) PUBLIC
efi_generate_ipv4_device_path(uint8_t *buf, ssize_t size,
			      const char * const ifname,
			      const char * const local_addr,
			      const char * const remote_addr,
			      const char * const gateway_addr,
			      const char * const netmask,
			      uint16_t local_port,
			      uint16_t remote_port,
			      uint16_t protocol,
			      uint8_t addr_origin)
{
	ssize_t off = 0;
	ssize_t sz;

	sz = make_mac_path(buf, size, ifname);
	if (sz < 0) {
		efi_error("could not make MAC DP node");
		return -1;
	}
	off += sz;

	sz = make_ipv4_path(buf+off, size?size-off:0, local_addr, remote_addr,
			    gateway_addr, netmask, local_port, remote_port,
			    protocol, addr_origin);
	if (sz < 0) {
		efi_error("could not make IPV4 DP node");
		return -1;
	}
	off += sz;

	sz = efidp_make_end_entire(buf+off, size?size-off:0);
	if (sz < 0) {
		efi_error("could not make EndEntire DP node");
		return -1;
	}
	off += sz;

	return off;
}

uint32_t PUBLIC
efi_get_libefiboot_version(void)
{
	return LIBEFIVAR_VERSION;
}

// vim:fenc=utf-8:tw=75:noet
