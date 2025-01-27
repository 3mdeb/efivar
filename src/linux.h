// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * libefiboot - library for the manipulation of EFI boot variables
 * Copyright 2012-2019 Red Hat, Inc.
 * Copyright (C) 2001 Dell Computer Corporation <Matt_Domsch@dell.com>
 */
#ifndef _EFIBOOT_LINUX_H
#define _EFIBOOT_LINUX_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "include/efivar/efivar-types.h"
#include "compiler.h"

#ifdef __OpenBSD__
#include <sys/disklabel.h>
#include <sys/dkio.h>
#endif

struct acpi_root_info {
	uint32_t acpi_hid;
	uint64_t acpi_uid;
	uint32_t acpi_cid;
	char *acpi_hid_str;
	char *acpi_uid_str;
	char *acpi_cid_str;
};

struct pci_root_info {
	uint16_t pci_domain;
	uint8_t pci_bus;
};

struct pci_dev_info {
	uint16_t pci_domain;
	uint8_t pci_bus;
	uint8_t pci_device;
	uint8_t pci_function;
	char *driverlink;
};

struct scsi_info {
	uint32_t scsi_bus;
	uint32_t scsi_device;
	uint32_t scsi_target;
	uint64_t scsi_lun;
};

struct sas_info {
	uint32_t scsi_bus;
	uint32_t scsi_device;
	uint32_t scsi_target;
	uint64_t scsi_lun;

	uint64_t sas_address;
};

struct sata_info {
	uint32_t scsi_bus;
	uint32_t scsi_device;
	uint32_t scsi_target;
	uint64_t scsi_lun;

	uint32_t ata_devno;
	uint32_t ata_port;
	uint32_t ata_pmp;

	uint32_t ata_print_id;
};

struct ata_info {
	uint32_t scsi_bus;
	uint32_t scsi_device;
	uint32_t scsi_target;
	uint64_t scsi_lun;

	uint32_t scsi_host;
};

struct nvme_info {
	int32_t ctrl_id;
	int32_t ns_id;
	int has_eui;
	uint8_t eui[8];
};

struct nvdimm_info {
	efi_guid_t namespace_label;
	efi_guid_t nvdimm_label;
};

struct emmc_info {
       int32_t slot_id;
};

enum interface_type {
	unknown,
	isa, acpi_root, pci_root, soc_root, virtual_root,
	pci, network,
	ata, atapi, scsi, sata, sas,
	usb, i1394, fibre, i2o,
	md, virtblk,
	nvme, nd_pmem,
	emmc,
};

struct dev_probe;

struct device {
	enum interface_type interface_type;
	uint32_t flags;
	char *link;
	char *device;
	char *driver;

	struct dev_probe **probes;
	unsigned int n_probes;

	union {
		struct {
			struct stat stat;

			unsigned int controllernum;
			unsigned int disknum;
			int part;
			uint64_t major;
			uint32_t minor;
			uint32_t edd10_devicenum;

			char *disk_name;
			char *part_name;

			struct acpi_root_info acpi_root;
			struct pci_root_info pci_root;
			unsigned int n_pci_devs;
			struct pci_dev_info *pci_dev;

			union {
				struct scsi_info scsi_info;
				struct sas_info sas_info;
				struct sata_info sata_info;
				struct ata_info ata_info;
				struct nvme_info nvme_info;
				struct emmc_info emmc_info;
				struct nvdimm_info nvdimm_info;
			};
		};
		char *ifname;
	};
};

extern struct device HIDDEN *device_get(const char *devpath, int fd, int partition);
extern void HIDDEN device_free(struct device *dev);
extern int HIDDEN set_disk_and_part_name(struct device *dev);
extern int HIDDEN set_part(struct device *dev, int value);
extern int HIDDEN set_part_name(struct device *dev, const char * const fmt, ...);
extern int HIDDEN set_disk_name(struct device *dev, const char * const fmt, ...);
extern bool HIDDEN is_pata(struct device *dev);
extern int HIDDEN make_blockdev_path(uint8_t *buf, ssize_t size,
				     struct device *dev);
extern int HIDDEN parse_acpi_hid_uid(struct device *dev, const char *fmt, ...);
extern int HIDDEN eb_nvme_ns_id(int fd, uint32_t *ns_id);

int HIDDEN get_sector_size(int filedes);
extern uint64_t HIDDEN get_disk_size_in_sectors(int filedes);
extern uint64_t HIDDEN get_disk_size_in_bytes(int filedes);

extern int HIDDEN find_parent_devpath(const char * const child,
				      char **parent);

extern ssize_t HIDDEN make_mac_path(uint8_t *buf, ssize_t size,
				    const char * const ifname);

#define read_sysfs_file(buf, fmt, args...)				\
	({								\
		uint8_t *buf_ = NULL;					\
		ssize_t bufsize_ = -1;					\
		int error_;						\
									\
		bufsize_ = get_file(&buf_, "/sys/" fmt, ## args);	\
		if (bufsize_ > 0) {					\
			uint8_t *buf2_ = alloca(bufsize_);		\
			error_ = errno;					\
			if (buf2_)					\
				memcpy(buf2_, buf_, bufsize_);		\
			free(buf_);					\
			*(buf) = (__typeof__(*(buf)))buf2_;		\
			errno = error_;					\
		} else if (buf_) {					\
			/* covscan is _sure_ we leak buf_ if bufsize_ */\
			/* is <= 0, which is wrong, but appease it.   */\
			free(buf_);					\
			buf_ = NULL;					\
		}							\
		bufsize_;						\
	})

#define sysfs_readlink(linkbuf, fmt, args...)				\
	({								\
		char *_lb = alloca(PATH_MAX+1);				\
		char *_pn;						\
		int _rc;						\
									\
		*(linkbuf) = NULL;					\
		_rc = asprintfa(&_pn, "/sys/" fmt, ## args);		\
		if (_rc >= 0) {						\
			ssize_t _linksz;				\
			_rc = _linksz = readlink(_pn, _lb, PATH_MAX);   \
			if (_linksz >= 0)				\
				_lb[_linksz] = '\0';			\
			else						\
				efi_error("readlink of %s failed", _pn);\
			*(linkbuf) = _lb;				\
		} else {						\
			efi_error("could not allocate memory");		\
		}							\
		_rc;							\
	})

#define sysfs_access(mode, fmt, args...)				\
	({								\
		int rc_;						\
		char *pn_;						\
									\
		rc_ = asprintfa(&pn_, "/sys/" fmt, ## args);		\
		if (rc_ >= 0) {						\
			rc_ = access(pn_, mode);			\
			if (rc_ < 0)					\
				efi_error("could not access %s", pn_);  \
		} else {						\
			efi_error("could not allocate memory");		\
		}							\
		rc_;							\
	})

#define sysfs_stat(statbuf, fmt, args...)				\
	({								\
		int rc_;						\
		char *pn_;						\
									\
		rc_ = asprintfa(&pn_, "/sys/" fmt, ## args);		\
		if (rc_ >= 0) {						\
			rc_ = stat(pn_, statbuf);			\
			if (rc_ < 0)					\
				efi_error("could not stat %s", pn_);    \
		} else {						\
			efi_error("could not allocate memory");		\
		}							\
		rc_;							\
	})

#define sysfs_opendir(fmt, args...)					\
	({								\
		int rc_;						\
		char *pn_;						\
		DIR *dir_ = NULL;					\
									\
		rc_ = asprintfa(&pn_, "/sys/" fmt, ## args);		\
		if (rc_ >= 0) {						\
			dir_ = opendir(pn_);				\
			if (dir_ == NULL)				\
				efi_error("could not open %s", pn_);    \
		} else {						\
			efi_error("could not allocate memory");		\
		}							\
		dir_;							\
	})

/*
 * Iterate a /sys/block directory looking for device/foo, device/device/foo,
 * etc.  I'm not proud of this method.
 */
#define find_device_file(result, name, fmt, args...)				\
	({									\
		int rc_ = 0;							\
		debug("searching for %s from in %s", name, dev->disk_name);	\
		for (unsigned int try_ = 0; true; try_++) {			\
			char *slashdev_;					\
										\
			slashdev_ = alloca(sizeof("device")			\
					   + try_ * strlen("/device"));		\
			if (!slashdev_) {					\
				rc_ = -1;					\
				efi_error("cannot allocate memory: %s",		\
					  strerror(errno));			\
				goto find_device_link_err_;			\
			}							\
										\
			char *nul_ = stpcpy(slashdev_, "device");		\
			for (unsigned int i_ = 0; i_ < try_; i_++)		\
				nul_ = stpcpy(nul_, "/device");			\
										\
			debug("trying /sys/" fmt "/%s/%s",			\
			      ## args, slashdev_, name);			\
										\
			rc_ = sysfs_access(F_OK, fmt "/%s", ## args, slashdev_);\
			if (rc_ < 0) {						\
				if (errno == ENOENT) {				\
					efi_error_pop();			\
					break;					\
				}						\
				efi_error("cannot access /sys/"fmt"/%s: %s",	\
					  ## args, slashdev_, strerror(errno));	\
				goto find_device_link_err_;			\
			}							\
										\
			rc_ = sysfs_access(F_OK, fmt "/%s/%s",			\
					   ## args, slashdev_, name);		\
			if (rc_ < 0) {						\
				if (errno == ENOENT) {				\
					efi_error_pop();			\
					break;					\
				}						\
				efi_error("cannot access /sys/"fmt"/%s/%s: %s",	\
					  ## args, slashdev_, name,		\
					  strerror(errno));			\
				goto find_device_link_err_;			\
			}							\
										\
			rc_ = asprintfa(result, fmt "/%s/%s",			\
					## args, slashdev_, name);		\
			if (rc_ < 0) {						\
				efi_error("cannot allocate memory: %s",		\
					  strerror(errno));			\
				goto find_device_link_err_;			\
			}							\
		}								\
find_device_link_err_:								\
		rc_;								\
	})

#define DEV_PROVIDES_ROOT       1
#define DEV_PROVIDES_HD	 2
#define DEV_ABBREV_ONLY	 4

struct dev_probe {
	char *name;
	enum interface_type *iftypes;
	uint32_t flags;
	ssize_t (*parse)(struct device *dev,
			 const char * const current, const char * const root);
	ssize_t (*create)(struct device *dev,
			  uint8_t *buf, ssize_t size, ssize_t off);
	char *(*make_part_name)(struct device *dev);
};

extern ssize_t parse_scsi_link(const char *current, uint32_t *host,
			       uint32_t *bus, uint32_t *device,
			       uint32_t *target, uint64_t *lun,
			       uint32_t *local_port_id, uint32_t *remote_port_id,
			       uint32_t *remote_target_id);

/* device support implementations */
extern struct dev_probe pmem_parser;
extern struct dev_probe pci_root_parser;
extern struct dev_probe acpi_root_parser;
extern struct dev_probe soc_root_parser;
extern struct dev_probe virtual_root_parser;
extern struct dev_probe pci_parser;
extern struct dev_probe sas_parser;
extern struct dev_probe sata_parser;
extern struct dev_probe nvme_parser;
extern struct dev_probe virtblk_parser;
extern struct dev_probe i2o_parser;
extern struct dev_probe scsi_parser;
extern struct dev_probe ata_parser;
extern struct dev_probe emmc_parser;

#endif /* _EFIBOOT_LINUX_H */

// vim:fenc=utf-8:tw=75:noet
