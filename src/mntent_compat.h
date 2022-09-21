// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * mntent_compat.h - compatibility hader for getmntent()
 * Copyright 2022 3mdeb <contact@3mdeb.com>
 */

#ifndef _MNTENT_COMPAT_H
#define _MNTENT_COMPAT_H

#if !defined(__linux__)

#include <sys/types.h>
#include <sys/mount.h>

#include <stdio.h>

struct mntent {
	char *mnt_fsname;
	char *mnt_dir;
	char *mnt_type;
	int mnt_freq;
	int mnt_passno;
};

#define setmntent(x, y) ((FILE *)0x1)

static inline struct mntent *
getmntent(FILE *fp)
{
	static int info_idx = -1;
	static int info_count = -1;
#ifdef __NetBSD__
	static struct statvfs *mount_info;
#else
	static struct statfs *mount_info;
#endif
	static struct mntent ret_mntent;

	(void)fp;

	if (info_idx == -1 || info_count == -1)
		info_count = getmntinfo(&mount_info, MNT_NOWAIT);

	++info_idx;
	if (info_idx == info_count) {
		info_idx = info_count = -1;
		return NULL;
	}

	ret_mntent.mnt_fsname = mount_info[info_idx].f_mntfromname;
	ret_mntent.mnt_dir = mount_info[info_idx].f_mntonname;
	ret_mntent.mnt_type = mount_info[info_idx].f_fstypename;
	ret_mntent.mnt_freq = 0;
	ret_mntent.mnt_passno = 0;
	return &ret_mntent;
}

static inline int
endmntent(FILE *fp)
{
	while (getmntent(fp) != NULL) {
		/* Cycle through remaining entries. */
	}
	return 1;
}

#else

#include <stdio.h>
#include <mntent.h>

#endif /* __OpenBSD__ */

#endif /* _MNTENT_COMPAT_H */

// vim:fenc=utf-8:tw=75:noet
