// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * libefiboot - library for the manipulation of EFI boot variables
 * Copyright 2012-2019 Red Hat, Inc.
 * Copyright (C) 2001 Dell Computer Corporation <Matt_Domsch@dell.com>
 */

#include "fix_coverity.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/ethtool.h>
#include <linux/version.h>
#include <linux/sockios.h>
#include <scsi/scsi.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#endif

#ifdef __NetBSD__
#include <sys/disk.h>
#include <sys/disklabel.h>
#endif

#if defined(__DragonFly__) || defined(__FreeBSD__)
#include <sys/diskslice.h>
#endif

#include "efiboot.h"

#if defined(__OpenBSD__) || defined(__NetBSD__)

#include <sys/param.h>
#include <sys/sysctl.h>

static char
get_raw_partition(void)
{
	int rawpart, mib[2];
	size_t varlen;

	mib[0] = CTL_KERN;
	mib[1] = KERN_RAWPARTITION;
	varlen = sizeof(rawpart);
	if (sysctl(mib, 2, &rawpart, &varlen, NULL, (size_t)0) < 0)
		return '\0';

	return 'a' + rawpart;
}

#endif

int HIDDEN
find_parent_devpath(const char * const child, char **parent)
{
#ifdef __linux__
	int ret;
	char *node;
	char *linkbuf;

	/* strip leading /dev/ */
	node = strrchr(child, '/');
	if (!node)
	        return -1;
	node++;

	/* look up full path symlink */
	ret = sysfs_readlink(&linkbuf, "class/block/%s", node);
	if (ret < 0 || !linkbuf)
	        return ret;

	/* strip child */
	node = strrchr(linkbuf, '/');
	if (!node)
	        return -1;
	*node = '\0';

	/* read parent */
	node = strrchr(linkbuf, '/');
	if (!node)
	        return -1;
	*node = '\0';
	node++;

	/* write out new path */
	ret = asprintf(parent, "/dev/%s", node);
	if (ret < 0)
	        return ret;

	return 0;
#elif defined(__OpenBSD__) || defined(__NetBSD__)
	int n;

#ifdef __NetBSD__
	/* Handle wedges. */
	if (strncmp(child, "/dev/rdk", 8) == 0) {
		int fd;
		struct dkwedge_info dkw;

		fd = open(child, O_RDONLY);
		if (fd < 0) {
			efi_error("could not open device: %s", child);
			return -1;
		}

		if (ioctl(fd, DIOCGWEDGEINFO, &dkw) == -1) {
			close(fd);
			efi_error("could not query wedge's info");
			return -1;
		}

		close(fd);

		if (asprintf(parent, "/dev/r%s", dkw.dkw_parent) < 0)
			return -1;

		return 0;
	}
#endif

	/* Skip until first digit. */
	n = strcspn(child, "0123456789");
	if (child[n] == '\0')
		return -1;

	/* Skip until first non-digit. */
	n += strspn(child + n, "0123456789");

	/* We can only handle partitions. */
	if (!isalpha(child[n]))
		return -1;

	/*
	 * This handles names like sd0i -> sd0c.
	 *
	 * sd0c is also translated into sd0c.
	 *
	 * "c" above means "raw partition" (whole disk).
	 */
	if (asprintf(parent, "%.*s%c", n, child, get_raw_partition()) < 0)
		return -1;

	return 0;
#else
	int n;

	/* Skip until first digit. */
	n = strcspn(child, "0123456789");
	if (child[n] == '\0')
		return -1;

	/* Skip until first non-digit. */
	n += strspn(child + n, "0123456789");

	/* This handles names like vbd0s1, nvd0p1, da0p1. */
	*parent = strndup(child, n);
	if (*parent == NULL)
		return -1;

	return 0;
#endif
}

int HIDDEN
set_part_name(struct device *dev, const char * const fmt, ...)
{
	ssize_t rc;
	va_list ap;
	int error;

	if (dev->part <= 0)
	        return 0;

	va_start(ap, fmt);
	rc = vasprintf(&dev->part_name, fmt, ap);
	error = errno;
	va_end(ap);
	errno = error;
	if (rc < 0)
	        efi_error("could not allocate memory");

	return rc;
}

int HIDDEN
reset_part_name(struct device *dev)
{
	char *part = NULL;
	int rc;

	if (dev->part_name) {
	        free(dev->part_name);
	        dev->part_name = NULL;
	}

	if (dev->part < 1)
	        return 0;

	if (dev->n_probes > 0 &&
	    dev->probes[dev->n_probes-1] &&
	    dev->probes[dev->n_probes-1]->make_part_name) {
	        part = dev->probes[dev->n_probes]->make_part_name(dev);
	        dev->part_name = part;
	        rc = 0;
	} else {
	        rc = asprintf(&dev->part_name, "%s%d",
	                      dev->disk_name, dev->part);
	        if (rc < 0)
	                efi_error("could not allocate memory");
	}
	return rc;
}

int HIDDEN
set_part(struct device *dev, int value)
{
	int rc;

	if (dev->part == value)
	        return 0;

	dev->part = value;
	rc = reset_part_name(dev);
	if (rc < 0)
	        efi_error("reset_part_name() failed");

	return rc;
}

int HIDDEN
set_disk_name(struct device *dev, const char * const fmt, ...)
{
	ssize_t rc;
	va_list ap;
	int error;

	va_start(ap, fmt);
	rc = vasprintf(&dev->disk_name, fmt, ap);
	error = errno;
	va_end(ap);
	errno = error;
	if (rc < 0)
	        efi_error("could not allocate memory");

	return rc;
}

int HIDDEN
set_disk_and_part_name(struct device *dev)
{
#ifdef __linux__
	int rc = -1;
	char *ultimate = pathseg(dev->link, -1);
	char *penultimate = pathseg(dev->link, -2);
	char *approximate = pathseg(dev->link, -3);
	char *proximate = pathseg(dev->link, -4);
	char *psl5 = pathseg(dev->link, -5);


	/*
	 * devlinks look something like:
	 * maj:min -> ../../devices/pci$PCI_STUFF/$BLOCKDEV_STUFF/block/$DISK/$PART
	 */

	errno = 0;
	debug("dev->disk_name:%p dev->part_name:%p", dev->disk_name, dev->part_name);
	debug("dev->part:%d", dev->part);
	debug("ultimate:'%s'", ultimate ? : "");
	debug("penultimate:'%s'", penultimate ? : "");
	debug("approximate:'%s'", approximate ? : "");
	debug("proximate:'%s'", proximate ? : "");
	debug("psl5:'%s'", psl5 ? : "");

	if (ultimate && penultimate &&
	    ((proximate && !strcmp(proximate, "nvme")) ||
	     (approximate && !strcmp(approximate, "block")))) {
	        /*
	         * 259:1 -> ../../devices/pci0000:00/0000:00:1d.0/0000:05:00.0/nvme/nvme0/nvme0n1/nvme0n1p1
	         * 8:1 -> ../../devices/pci0000:00/0000:00:17.0/ata2/host1/target1:0:0/1:0:0:0/block/sda/sda1
	         * 8:33 -> ../../devices/pci0000:00/0000:00:01.0/0000:01:00.0/host4/port-4:0/end_device-4:0/target4:0:0/4:0:0:0/block/sdc/sdc1
	         * 252:1 -> ../../devices/pci0000:00/0000:00:07.0/virtio2/block/vda/vda1
	         * 259:3 -> ../../devices/LNXSYSTM:00/LNXSYBUS:00/ACPI0012:00/ndbus0/region11/btt11.0/block/pmem11s/pmem11s1
	         */
	        set_disk_name(dev, "%s", penultimate);
	        set_part_name(dev, "%s", ultimate);
	        debug("disk:%s part:%s", penultimate, ultimate);
		rc = 0;
	} else if (ultimate && approximate && !strcmp(approximate, "nvme")) {
	        /*
	         * 259:0 -> ../../devices/pci0000:00/0000:00:1d.0/0000:05:00.0/nvme/nvme0/nvme0n1
	         */
	        set_disk_name(dev, "%s", ultimate);
	        set_part_name(dev, "%sp%d", ultimate, dev->part);
	        debug("disk:%s part:%sp%d", ultimate, ultimate, dev->part);
		rc = 0;
	} else if (ultimate && penultimate && !strcmp(penultimate, "block")) {
	        /*
	         * 253:0 -> ../../devices/virtual/block/dm-0 (... I guess)
	         * 8:0 -> ../../devices/pci0000:00/0000:00:17.0/ata2/host1/target1:0:0/1:0:0:0/block/sda
	         * 11:0 -> ../../devices/pci0000:00/0000:00:11.5/ata3/host2/target2:0:0/2:0:0:0/block/sr0
	         * 8:32 -> ../../devices/pci0000:00/0000:00:01.0/0000:01:00.0/host4/port-4:0/end_device-4:0/target4:0:0/4:0:0:0/block/sdc
	         * 252:0 -> ../../devices/pci0000:00/0000:00:07.0/virtio2/block/vda
	         * 259:0 -> ../../devices/LNXSYSTM:00/LNXSYBUS:00/ACPI0012:00/ndbus0/region9/btt9.0/block/pmem9s
	         * 259:1 -> ../../devices/LNXSYSTM:00/LNXSYBUS:00/ACPI0012:00/ndbus0/region11/btt11.0/block/pmem11s
	         */
	        set_disk_name(dev, "%s", ultimate);
	        set_part_name(dev, "%s%d", ultimate, dev->part);
	        debug("disk:%s part:%s%d", ultimate, ultimate, dev->part);
		rc = 0;
	} else if (ultimate && approximate && !strcmp(approximate, "mtd")) {
	        /*
	         * 31:0 -> ../../devices/platform/1e000000.palmbus/1e000b00.spi/spi_master/spi32766/spi32766.0/mtd/mtd0/mtdblock0
	         */
	        set_disk_name(dev, "%s", ultimate);
	        debug("disk:%s", ultimate);
		rc = 0;
	} else if ((proximate && ultimate && !strcmp(proximate, "nvme-fabrics")) ||
		    (approximate && ultimate && !strcmp(approximate, "nvme-subsystem"))) {
		/*
		 * 259:0 ->../../devices/virtual/nvme-fabrics/ctl/nvme0/nvme0n1
		 *				 ^ proximate            ^ ultimate
		 * or
		 * 259:5 -> ../../devices/virtual/nvme-subsystem/nvme-subsys0/nvme0n1
		 *                                ^ approximate  ^ penultimate
		 *                                                   ultimate ^
		 */
		set_disk_name(dev, "%s", ultimate);
		debug("disk:%s", ultimate);
		rc = 0;
	} else if ((psl5 && penultimate && ultimate && !strcmp(psl5, "nvme-fabrics")) ||
		   (proximate && penultimate && ultimate && !strcmp(proximate, "nvme-subsystem"))) {
		/*
		 * 259:1 -> ../../devices/virtual/nvme-fabrics/ctl/nvme0/nvme0n1/nvme0n1p1
		 *                                ^psl5                  ^ penultimate
		 *                                                      ultimate ^
		 * or
		 * 259:6 -> ../../devices/virtual/nvme-subsystem/nvme-subsys0/nvme0n1/nvme0n1p1
		 *                                ^ proximate                 ^ penultimate
		 *                                                           ultimate ^
		 */
		set_disk_name(dev, "%s", penultimate);
		set_part_name(dev, "%s", ultimate);
		debug("disk:%s part:%s", penultimate, ultimate);
		rc = 0;
	}

	if (rc < 0)
		efi_error("Could not parse disk name:\"%s\"", dev->link);
	return rc;
#elif defined(__NetBSD__)
	/*
	 * TODO: this only handles device objects constructed from part devpath.
	 */
	int wedges = (strncmp(dev->link, "/dev/rdk", 8) == 0);

	if (wedges) {
		int fd;
		struct dkwedge_info dkw;

		fd = open(dev->link, O_RDONLY);
		if (fd < 0) {
			efi_error("could not open device: %s", dev->link);
			return -1;
		}

		if (ioctl(fd, DIOCGWEDGEINFO, &dkw) == -1) {
			close(fd);
			efi_error("could not query wedge's info");
			return -1;
		}

		close(fd);

		set_disk_name(dev, "r%s", dkw.dkw_parent);
	} else {
		const char *node = strrchr(dev->link, '/');
		if (!node) {
			errno = EINVAL;
			return -1;
		}
		node++;
		set_disk_name(dev, "%s", node);
	}

	if (dev->part == -1) {
		set_part_name(dev, "%s", dev->disk_name);
		return 0;
	}

	/*
	 * TODO: need to get part dev from disk dev and part num.
	 *
	 * Likely need to determine partition offset by parsing GPT, then parse
	 * sysctl("hw.disknames") for names of partition devices and query their
	 * offsets until a match is found.
	 */
	set_part_name(dev, "%s", dev->link);
	return 0;
#else
	/*
	 * TODO: this only handles device objects constructed from part devpath.
	 */

# if defined(__FreeBSD__)
	char separator = 's';
# elif defined(__DragonFly__)
	char separator = 'p';
# endif

	const char *node = strrchr(dev->link, '/');
	if (node == NULL) {
		errno = EINVAL;
		return -1;
	}
	node++;
	set_disk_name(dev, "%s", node);

	if (dev->part == -1) {
		set_part_name(dev, "%s", dev->disk_name);
		return 0;
	}

# if defined(__FreeBSD__) || defined(__DragonFly__)
	/* Handle e.g. vbd0s1, nvd0p1, da0p1. */
	set_part_name(dev, "%s%c%d", dev->disk_name, separator, dev->part);
# elif defined(__OpenBSD__)
	/*
	 * TODO: need to get part dev from disk dev and part num.
	 *
	 * Likely need to determine partition offset by parsing GPT, then do
	 * ioctl(DIOCGPDINFO) on disk device and loop over d_partitions looking
	 * for a matching offset.
	 */
	if (dev->part != 1) {
		set_part_name(dev, "%s", dev->link);
		return 0;
	}

	set_part_name(dev, "%s%di", dev->disk_name, dev->part);
	return 0;
# else
#  error "No implementation for the platform"
# endif

#endif
}

static struct dev_probe *dev_probes[] = {
#ifdef __linux__
	/*
	 * pmem needs to be before PCI, so if it provides root it'll
	 * be found first.
	 */
	&pmem_parser,
	&acpi_root_parser,
	&pci_root_parser,
	&soc_root_parser,
	&virtual_root_parser,
	&pci_parser,
	&virtblk_parser,
	&sas_parser,
	&sata_parser,
	&nvme_parser,
	&ata_parser,
	&scsi_parser,
	&i2o_parser,
	&emmc_parser,
#endif
	NULL
};

void HIDDEN
device_free(struct device *dev)
{
	if (!dev)
	        return;
	if (dev->link)
	        free(dev->link);

	if (dev->device)
	        free(dev->device);

	if (dev->driver)
	        free(dev->driver);

	if (dev->probes)
	        free(dev->probes);

	if (dev->acpi_root.acpi_hid_str)
	        free(dev->acpi_root.acpi_hid_str);
	if (dev->acpi_root.acpi_uid_str)
	        free(dev->acpi_root.acpi_uid_str);
	if (dev->acpi_root.acpi_cid_str)
	        free(dev->acpi_root.acpi_cid_str);

	if (dev->interface_type == network) {
	        if (dev->ifname)
	                free(dev->ifname);
	} else {
	        if (dev->disk_name)
	                free(dev->disk_name);
	        if (dev->part_name)
	                free(dev->part_name);
	}

	for (unsigned int i = 0; i < dev->n_pci_devs; i++)
	        if (dev->pci_dev[i].driverlink)
	                free(dev->pci_dev[i].driverlink);

	if (dev->pci_dev)
	        free(dev->pci_dev);

	memset(dev, 0, sizeof(*dev));
	free(dev);
}

static void
print_dev_dp_node(struct device *dev, struct dev_probe *probe)
{
	ssize_t dpsz;
	uint8_t *dp;
	ssize_t bufsz;
	uint8_t *buf;
	ssize_t sz;

	dpsz = probe->create(dev, NULL, 0, 0);
	if (dpsz <= 0)
		return;

	dp = alloca(dpsz + 4);
	if (!dp)
		return;

	dpsz = probe->create(dev, dp, dpsz, 0);
	if (dpsz <= 0)
		return;

	sz = efidp_make_end_entire(dp + dpsz, 4);
	if (sz < 0)
		return;
	dpsz += sz;
	bufsz = efidp_format_device_path(NULL, 0,
					 (const_efidp)dp, dpsz);
	if (bufsz <= 0)
		return;

	buf = alloca(bufsz);
	if (!buf)
		return;

	bufsz = efidp_format_device_path(buf, bufsz,
			(const_efidp)dp, dpsz);
	if (bufsz <= 0)
		return;

	debug("Device path node is %s", buf);
}

struct device HIDDEN
*device_get(const char *devpath, int fd, int partition)
{
	struct device *dev;
	char *linkbuf = NULL, *tmpbuf = NULL;
	int i = 0;
	unsigned int n = 0;
	int rc;

	size_t nmemb = (sizeof(dev_probes)
	                / sizeof(dev_probes[0])) + 1;

	dev = calloc(1, sizeof(*dev));
	if (!dev) {
	        efi_error("could not allocate %zd bytes", sizeof(*dev));
	        return NULL;
	}

	dev->part = partition;
	debug("partition:%d dev->part:%d", partition, dev->part);
	dev->probes = calloc(nmemb, sizeof(struct dev_probe *));
	if (!dev->probes) {
	        efi_error("could not allocate %zd bytes",
	                  nmemb * sizeof(struct dev_probe *));
	        goto err;
	}

	rc = fstat(fd, &dev->stat);
	if (rc < 0) {
	        efi_error("fstat(%d) failed", fd);
	        goto err;
	}

	dev->pci_root.pci_domain = 0xffff;
	dev->pci_root.pci_bus = 0xff;

	if (S_ISBLK(dev->stat.st_mode) || S_ISCHR(dev->stat.st_mode)) {
	        dev->major = major(dev->stat.st_rdev);
	        dev->minor = minor(dev->stat.st_rdev);
	} else if (S_ISREG(dev->stat.st_mode)) {
	        dev->major = major(dev->stat.st_dev);
	        dev->minor = minor(dev->stat.st_dev);
	} else {
	        efi_error("device is not a block device or regular file");
	        goto err;
	}

#ifdef __linux__
	(void)devpath;

	rc = sysfs_readlink(&linkbuf, "dev/block/%"PRIu64":%"PRIu32,
	                    dev->major, dev->minor);
	if (rc < 0 || !linkbuf) {
	        efi_error("readlink of /sys/dev/block/%"PRIu64":%"PRIu32" failed",
	                  dev->major, dev->minor);
	        goto err;
	}

	dev->link = strdup(linkbuf);
	if (!dev->link) {
	        efi_error("strdup(\"%s\") failed", linkbuf);
	        goto err;
	}
	debug("dev->link: %s", dev->link);
#else
	dev->link = strdup(devpath);
	/* There are no probes, supporting abbreviated paths only. */
	dev->flags |= DEV_ABBREV_ONLY;
#endif

	if (dev->part == -1) {
	        rc = read_sysfs_file(&tmpbuf, "dev/block/%s/partition", dev->link);
	        if (rc < 0 || !tmpbuf) {
	                efi_error("device has no /partition node; not a partition");
	        } else {
	                rc = sscanf((char *)tmpbuf, "%d\n", &dev->part);
	                if (rc != 1)
	                        efi_error("couldn't parse partition number for %s", tmpbuf);
	        }
	}

	rc = set_disk_and_part_name(dev);
	if (rc < 0) {
	        efi_error("could not set disk and partition names");
	        goto err;
	}
	debug("dev->disk_name: %s", dev->disk_name);
	debug("dev->part_name: %s", dev->part_name);

	rc = sysfs_readlink(&tmpbuf, "block/%s/device", dev->disk_name);
	if (rc < 0 || !tmpbuf) {
	        debug("readlink of /sys/block/%s/device failed",
	                  dev->disk_name);

	        dev->device = strdup("");
	} else {
	        dev->device = strdup(tmpbuf);
	}

	if (!dev->device) {
	        efi_error("strdup(\"%s\") failed", tmpbuf);
	        goto err;
	}

	/*
	 * So, on a normal disk, you get something like:
	 * /sys/block/sda/device -> ../../0:0:0:0
	 * /sys/block/sda/device/driver -> ../../../../../../../bus/scsi/drivers/sd
	 *
	 * On a directly attached nvme device you get:
	 * /sys/block/nvme0n1/device -> ../../nvme0
	 * /sys/block/nvme0n1/device/device -> ../../../0000:6e:00.0
	 * /sys/block/nvme0n1/device/device/driver -> ../../../../bus/pci/drivers/nvme
	 *
	 * On a fabric-attached nvme device, you get something like:
	 * /sys/block/nvme0n1/device -> ../../nvme0
	 * /sys/block/nvme0n1/device/device -> ../../ctl
	 * /sys/block/nvme0n1/device/device/device -> ../../../../../0000:6e:00.0
	 * /sys/block/nvme0n1/device/device/device/driver -> ../../../../../../bus/pci/drivers/nvme-fabrics
	 *
	 * ... I think?  I don't have one in front of me.
	 */

	char *filepath = NULL;
	rc = find_device_file(&filepath, "driver", "block/%s", dev->disk_name);
	if (rc >= 0) {
		rc = sysfs_readlink(&tmpbuf, "%s", filepath);
	        if (rc < 0 || !tmpbuf) {
			efi_error("readlink of /sys/%s failed", filepath);
	                goto err;
	        }

	        linkbuf = pathseg(tmpbuf, -1);
	        if (!linkbuf) {
	                efi_error("could not get segment -1 of \"%s\"", tmpbuf);
	                goto err;
	        }

	        dev->driver = strdup(linkbuf);
	} else {
		dev->driver = strdup("");
	}

	if (!dev->driver) {
	        efi_error("strdup(\"%s\") failed", linkbuf);
	        goto err;
	}

	const char *current = dev->link;
	bool needs_root = true;
	int last_successful_probe = -1;

	debug("searching for device nodes in %s", dev->link);
	for (i = 0;
	     dev_probes[i] && dev_probes[i]->parse && *current;
	     i++) {
	        struct dev_probe *probe = dev_probes[i];
	        int pos;

	        if (!needs_root &&
	            (probe->flags & DEV_PROVIDES_ROOT)) {
	                debug("not testing %s because flags is 0x%x",
	                      probe->name, probe->flags);
	                continue;
	        }

	        debug("trying %s", probe->name);
	        pos = probe->parse(dev, current, dev->link);
	        if (pos < 0) {
	                efi_error("parsing %s failed", probe->name);
	                goto err;
	        } else if (pos > 0) {
			char match[pos+1];

			strncpy(match, current, pos);
			match[pos] = '\0';
	                debug("%s matched '%s'", probe->name, match);
	                dev->flags |= probe->flags;

	                if (probe->flags & DEV_PROVIDES_HD ||
	                    probe->flags & DEV_PROVIDES_ROOT ||
	                    probe->flags & DEV_ABBREV_ONLY)
	                        needs_root = false;

			if (probe->create)
				print_dev_dp_node(dev, probe);

	                dev->probes[n++] = dev_probes[i];
	                current += pos;
			if (current[0] == '\0')
				debug("finished");
			else
				debug("current:'%s'", current);
	                last_successful_probe = i;

	                if (!*current || !strncmp(current, "block/", 6))
	                        break;

	                continue;
	        }

	        debug("dev_probes[%d]: %p dev->interface_type: %d\n",
	              i+1, dev_probes[i+1], dev->interface_type);
	        if (dev_probes[i+1] == NULL && dev->interface_type == unknown) {
	                pos = 0;
	                rc = sscanf(current, "%*[^/]/%n", &pos);
	                if (rc < 0) {
slash_err:
	                        efi_error("Cannot parse device link segment \"%s\"", current);
	                        goto err;
	                }

	                while (current[pos] == '/')
	                        pos += 1;

	                if (!current[pos])
	                        goto slash_err;

	                debug("Cannot parse device link segment '%s'", current);
	                debug("Skipping to '%s'", current + pos);
	                debug("This means we can only create abbreviated paths");
	                dev->flags |= DEV_ABBREV_ONLY;
	                i = last_successful_probe;
	                current += pos;

	                if (!*current || !strncmp(current, "block/", 6))
	                        break;
	        }
	}

	if (dev->interface_type == unknown &&
	    !(dev->flags & DEV_ABBREV_ONLY) &&
	    !strcmp(current, "block/")) {
	        efi_error("unknown storage interface");
	        errno = ENOSYS;
	        goto err;
	}

	return dev;
err:
	device_free(dev);
	return NULL;
}

int HIDDEN
make_blockdev_path(uint8_t *buf, ssize_t size, struct device *dev)
{
	ssize_t off = 0;

	debug("entry buf:%p size:%zd", buf, size);

	for (unsigned int i = 0; dev->probes[i] &&
	                         dev->probes[i]->parse; i++) {
	        struct dev_probe *probe = dev->probes[i];
	        ssize_t sz;

	        if (!probe->create)
	                continue;

	        sz = probe->create(dev, buf + off, size ? size - off : 0, 0);
	        if (sz < 0) {
	                efi_error("could not create %s device path",
	                          probe->name);
	                return sz;
	        }
	        off += sz;
	}

	debug("= %zd", off);

	return off;
}

ssize_t HIDDEN
make_mac_path(uint8_t *buf, ssize_t size, const char * const ifname)
{
#ifdef __linux__
	struct ifreq ifr;
	struct ethtool_drvinfo drvinfo = { 0, };
	int fd = -1, rc;
	ssize_t ret = -1, sz, off = 0;
	char busname[PATH_MAX+1] = "";
	struct device dev;

	memset(&dev, 0, sizeof (dev));
	dev.interface_type = network;
	dev.ifname = strdupa(ifname);
	if (!dev.ifname)
	        return -1;

	/*
	 * find the device link, which looks like:
	 * ../../devices/$PCI_STUFF/net/$IFACE
	 */
	rc = sysfs_readlink(&dev.link, "class/net/%s", ifname);
	if (rc < 0 || !dev.link)
	        goto err;

	memset(&ifr, 0, sizeof (ifr));
	strncpy(ifr.ifr_name, ifname, IF_NAMESIZE);
	ifr.ifr_name[IF_NAMESIZE-1] = '\0';
	drvinfo.cmd = ETHTOOL_GDRVINFO;
	ifr.ifr_data = (caddr_t)&drvinfo;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
	        goto err;

	rc = ioctl(fd, SIOCETHTOOL, &ifr);
	if (rc < 0)
	        goto err;

	strncpy(busname, drvinfo.bus_info, PATH_MAX);

	rc = ioctl(fd, SIOCGIFHWADDR, &ifr);
	if (rc < 0)
	        goto err;

	sz = pci_parser.create(&dev, buf, size, off);
	if (sz < 0)
	        goto err;
	off += sz;

	sz = efidp_make_mac_addr(buf+off, size?size-off:0,
	                         ifr.ifr_ifru.ifru_hwaddr.sa_family,
	                         (uint8_t *)ifr.ifr_ifru.ifru_hwaddr.sa_data,
	                         sizeof(ifr.ifr_ifru.ifru_hwaddr.sa_data));
	if (sz < 0)
	        goto err;

	off += sz;
	ret = off;
err:
	if (fd >= 0)
	        close(fd);
	return ret;
#else
	(void)buf;
	(void)size;
	(void)ifname;
	efi_error("make_mac_path() is not implemented for this platform");
	return -1;
#endif
}

/************************************************************
 * get_sector_size
 * Requires:
 *  - filedes is an open file descriptor, suitable for reading
 * Modifies: nothing
 * Returns:
 *  sector size, or 512.
 ************************************************************/
int UNUSED
get_sector_size(int filedes)
{
#ifdef __OpenBSD__
	struct disklabel dl;
	if (ioctl(filedes, DIOCGPDINFO, &dl) == -1)
		return 512;

	return dl.d_secsize;
#elif defined(__NetBSD__)
	u_int sector_size;
	if (ioctl(filedes, DIOCGSECTORSIZE, &sector_size) == -1)
		return 0;

	return sector_size;
#elif defined(__linux__)
	int rc, sector_size = 512;

	rc = ioctl(filedes, BLKSSZGET, &sector_size);
	if (rc)
	        sector_size = 512;
	return sector_size;
#elif defined(__DragonFly__) || defined(__FreeBSD__)
	struct partinfo partinfo;
	if (ioctl(filedes, DIOCGPART, &partinfo) == -1)
		return 0;

	return partinfo.media_blksize;
#else
#error "No implementation for the platform"
#endif
}

#ifdef __linux__

/**
 * kernel_has_blkgetsize64()
 *
 * Returns: 0 on false, 1 on true
 * True means kernel is 2.4.x, x>=18, or
 *		   is 2.5.x, x>4, or
 *		   is > 2.5
 */
static int
kernel_has_blkgetsize64(void)
{
	int major=0, minor=0, patch=0, parsed;
	int rc;
	struct utsname u;

	memset(&u, 0, sizeof(u));
	rc = uname(&u);
	if (rc)
		return 0;

	parsed = sscanf(u.release, "%d.%d.%d", &major, &minor, &patch);
	/* If the kernel is 2.4.15-2.4.18 and 2.5.0-2.5.3, i.e. the problem
	 * kernels, then this will get 3 answers.  If it doesn't, it isn't. */
	if (parsed != 3)
		return 1;

	if (major == 2 && minor == 5 && patch < 4)
		return 0;
	if (major == 2 && minor == 4 && patch >= 15 && patch <= 18)
		return 0;
	return 1;
}

#endif

/************************************************************
 * get_disk_size_in_sectors
 * Requires:
 *  - filedes is an open file descriptor, suitable for reading
 * Modifies: nothing
 * Returns:
 *  Last LBA value on success
 *  0 on error
 *
 * Try getting BLKGETSIZE64 and BLKSSZGET first,
 * then BLKGETSIZE if necessary.
 *  Kernels 2.4.15-2.4.18 and 2.5.0-2.5.3 have a broken BLKGETSIZE64
 *  which returns the number of 512-byte sectors, not the size of
 *  the disk in bytes. Fixed in kernels 2.4.18-pre8 and 2.5.4-pre3.
 ************************************************************/
uint64_t HIDDEN
get_disk_size_in_sectors(int filedes)
{
	uint64_t size;

#ifdef __OpenBSD__
	struct disklabel dl;
	if (ioctl(filedes, DIOCGPDINFO, &dl) == -1)
		return 0;

	size = dl.d_secperunith;
	size <<= 32;
	size += dl.d_secperunit;
#elif defined(__NetBSD__)
	struct disklabel dl;
	if (ioctl(filedes, DIOCGDINFO, &dl) == -1)
		return 0;

	size = dl.d_secperunit;
#elif defined(__linux__)
	if (kernel_has_blkgetsize64()) {
		long disk_size = 0;
		if (ioctl(filedes, BLKGETSIZE, &disk_size) < 0)
			return 0;

		size = disk_size;
	} else {
		uint64_t size_in_bytes = get_disk_size_in_bytes(filedes);
		if (size_in_bytes == 0)
			return 0;

		size = size_in_bytes / get_sector_size(filedes);
	}
#elif defined(__DragonFly__) || defined(__FreeBSD__)
	struct partinfo partinfo;
	if (ioctl(filedes, DIOCGPART, &partinfo) == -1)
		return 0;

	size = partinfo.media_blocks;
#else
#error "No implementation for the platform"
#endif

	return size;
}

uint64_t HIDDEN
get_disk_size_in_bytes(int filedes)
{
	uint64_t size;

#ifdef __OpenBSD__
	struct disklabel dl;
	if (ioctl(filedes, DIOCGPDINFO, &dl) == -1)
		return 0;

	size = dl.d_secperunith;
	size <<= 32;
	size += dl.d_secperunit;

	size *= dl.d_secsize;
#elif defined(__NetBSD__)
	off_t disk_size;
	if (ioctl(filedes, DIOCGMEDIASIZE, &disk_size) == -1)
		return 0;
	size = disk_size;
#elif defined(__linux__)
	if (ioctl(filedes, BLKGETSIZE64, &size) < 0)
		return 0;
#elif defined(__DragonFly__) || defined(__FreeBSD__)
	struct partinfo partinfo;
	if (ioctl(filedes, DIOCGPART, &partinfo) == -1)
		return 0;

	size = partinfo.media_size;
#else
#error "No implementation for the platform"
#endif

	return size;
}

// vim:fenc=utf-8:tw=75:noet
