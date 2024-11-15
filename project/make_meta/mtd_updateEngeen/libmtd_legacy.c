/*
 * Copyright (C) 2009 Nokia Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Author: Artem Bityutskiy
 *
 * This file  is part of the MTD library. Implements pre-2.6.30 kernels support,
 * where MTD did not have sysfs interface. The main limitation of the old
 * kernels was that the sub-page size was not exported to user-space, so it was
 * not possible to get sub-page size.
 */

#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <mtd/mtd-user.h>

#include <libmtd.h>
#include "libmtd_int.h"
#include "common.h"

#define MTD_PROC_FILE "/proc/mtd"
#define MTD_DEV_PATT  "/dev/mtd%d"
#define MTD_DEV_MAJOR 90

#define PROC_MTD_FIRST     "dev:    size   erasesize  name\n"
#define PROC_MTD_FIRST_LEN (sizeof(PROC_MTD_FIRST) - 1)
#define PROC_MTD_MAX_LEN   4096
#define PROC_MTD_PATT      "mtd%d: %llx %x"

/**
 * struct proc_parse_info - /proc/mtd parsing information.
 * @mtd_num: MTD device number
 * @size: device size
 * @eb_size: eraseblock size
 * @name: device name
 * @buf: contents of /proc/mtd
 * @data_size: how much data was read into @buf
 * @next: next string in @buf to parse
 */
struct proc_parse_info {
	int mtd_num;
	long long size;
	char name[MTD_NAME_MAX + 1];
	int eb_size;
	char *buf;
	int data_size;
	char *next;
};

static int proc_parse_start(struct proc_parse_info *pi)
{
	int fd, ret;

	fd = open(MTD_PROC_FILE, O_RDONLY);
	if (fd == -1)
		return -1;

	pi->buf = xmalloc(PROC_MTD_MAX_LEN);

	ret = read(fd, pi->buf, PROC_MTD_MAX_LEN);
	if (ret == -1) {
		sys_errmsg("cannot read \"%s\"", MTD_PROC_FILE);
		goto out_free;
	}

	if (ret < PROC_MTD_FIRST_LEN ||
	    memcmp(pi->buf, PROC_MTD_FIRST, PROC_MTD_FIRST_LEN)) {
		errmsg("\"%s\" does not start with \"%s\"", MTD_PROC_FILE,
		       PROC_MTD_FIRST);
		goto out_free;
	}

	pi->data_size = ret;
	pi->next = pi->buf + PROC_MTD_FIRST_LEN;

	close(fd);
	return 0;

out_free:
	free(pi->buf);
	close(fd);
	return -1;
}

static int proc_parse_next(struct proc_parse_info *pi)
{
	int ret, len, pos = pi->next - pi->buf;
	char *p, *p1;

	if (pos >= pi->data_size) {
		free(pi->buf);
		return 0;
	}

	ret = sscanf(pi->next, PROC_MTD_PATT, &pi->mtd_num, &pi->size,
		     &pi->eb_size);
	if (ret != 3)
		return errmsg("\"%s\" pattern not found", PROC_MTD_PATT);

	p = memchr(pi->next, '\"', pi->data_size - pos);
	if (!p)
		return errmsg("opening \" not found");
	p += 1;
	pos = p - pi->buf;
	if (pos >= pi->data_size)
		return errmsg("opening \" not found");

	p1 = memchr(p, '\"', pi->data_size - pos);
	if (!p1)
		return errmsg("closing \" not found");
	pos = p1 - pi->buf;
	if (pos >= pi->data_size)
		return errmsg("closing \" not found");

	len = p1 - p;
	if (len > MTD_NAME_MAX)
		return errmsg("too long mtd%d device name", pi->mtd_num);

	memcpy(pi->name, p, len);
	pi->name[len] = '\0';

	if (p1[1] != '\n')
		return errmsg("opening \"\n\" not found");
	pi->next = p1 + 2;
	return 1;
}

/**
 * legacy_procfs_is_supported - legacy version of 'sysfs_is_supported()'.
 *
 * Check if we can access the procfs files for the MTD subsystem.
 */
int legacy_procfs_is_supported(void)
{
	if (access(MTD_PROC_FILE, R_OK) != 0) {
		if (errno == ENOENT) {
			errno = 0;
		} else {
			sys_errmsg("cannot read \"%s\"", MTD_PROC_FILE);
		}
		return 0;
	}
	return 1;
}

/**
 * legacy_dev_present - legacy version of 'mtd_dev_present()'.
 * @mtd_num: MTD device number
 *
 * When the kernel does not provide sysfs files for the MTD subsystem,
 * fall-back to parsing the /proc/mtd file to determine whether an mtd device
 * number @mtd_num is present.
 */
int legacy_dev_present(int mtd_num)
{
	int ret;
	struct proc_parse_info pi;

	ret = proc_parse_start(&pi);
	if (ret)
		return -1;

	while (proc_parse_next(&pi)) {
		if (pi.mtd_num == mtd_num)
			return 1;
	}

	return 0;
}

/**
 * legacy_mtd_get_info - legacy version of 'mtd_get_info()'.
 * @info: the MTD device information is returned here
 *
 * This function is similar to 'mtd_get_info()' and has the same conventions.
 */
int legacy_mtd_get_info(struct mtd_info *info)
{
	int ret;
	struct proc_parse_info pi;

	ret = proc_parse_start(&pi);
	if (ret)
		return -1;

	info->lowest_mtd_num = INT_MAX;
	while (proc_parse_next(&pi)) {
		info->mtd_dev_cnt += 1;
		if (pi.mtd_num > info->highest_mtd_num)
			info->highest_mtd_num = pi.mtd_num;
		if (pi.mtd_num < info->lowest_mtd_num)
			info->lowest_mtd_num = pi.mtd_num;
	}

	return 0;
}

int legacy_get_mtd_oobavail(const char *node)
{
	struct stat st;
	struct nand_ecclayout_user usrlay;
	int fd, ret;

	fd = open(node, O_RDONLY);
	if (fd == -1)
		return sys_errmsg("cannot open \"%s\"", node);

	if (fstat(fd, &st)) {
		ret = sys_errmsg("cannot open \"%s\"", node);
		goto out_close;
	}

	if (!S_ISCHR(st.st_mode)) {
		errno = EINVAL;
		ret = errmsg("\"%s\" is not a character device", node);
		goto out_close;
	}

	ret = ioctl(fd, ECCGETLAYOUT, &usrlay);
	if (ret < 0) {
		if (errno == EOPNOTSUPP)
			goto out_close;
		sys_errmsg("ECCGETLAYOUT ioctl request failed");
		goto out_close;
	}

	ret = usrlay.oobavail;

out_close:
	close(fd);

	return ret;
}

int legacy_get_mtd_oobavail1(int mtd_num)
{
	char node[sizeof(MTD_DEV_PATT) + 20];

	sprintf(node, MTD_DEV_PATT, mtd_num);
	return legacy_get_mtd_oobavail(node);
}

/**
 * legacy_get_dev_info - legacy version of 'mtd_get_dev_info()'.
 * @node: name of the MTD device node
 * @mtd: the MTD device information is returned here
 *
 * This function is similar to 'mtd_get_dev_info()' and has the same
 * conventions.
 */
int legacy_get_dev_info(const char *node, struct mtd_dev_info *mtd)
{
	struct stat st;
	struct mtd_info_user ui;
	int fd, ret;
	loff_t offs = 0;
	struct proc_parse_info pi;

	fd = open(node, O_RDONLY);
	if (fd == -1) {
		sys_errmsg("cannot open \"%s\"", node);
		if (errno == ENOENT)
			normsg("MTD subsystem is old and does not support sysfs, so MTD character device nodes have to exist");
		return -1;
	}

	if (fstat(fd, &st)) {
		sys_errmsg("cannot stat \"%s\"", node);
		close(fd);
		return -1;
	}

	if (!S_ISCHR(st.st_mode)) {
		close(fd);
		errno = EINVAL;
		return errmsg("\"%s\" is not a character device", node);
	}

	memset(mtd, '\0', sizeof(struct mtd_dev_info));
	mtd->major = major(st.st_rdev);
	mtd->minor = minor(st.st_rdev);

	if (mtd->major != MTD_DEV_MAJOR) {
		close(fd);
		errno = EINVAL;
		return errmsg("\"%s\" has major number %d, MTD devices have major %d",
			      node, mtd->major, MTD_DEV_MAJOR);
	}

	mtd->mtd_num = mtd->minor / 2;

	if (ioctl(fd, MEMGETINFO, &ui)) {
		sys_errmsg("MEMGETINFO ioctl request failed");
		goto out_close;
	}

	ret = ioctl(fd, MEMGETBADBLOCK, &offs);
	if (ret == -1) {
		if (errno != EOPNOTSUPP) {
			sys_errmsg("MEMGETBADBLOCK ioctl failed");
			goto out_close;
		}
		errno = 0;
		mtd->bb_allowed = 0;
	} else
		mtd->bb_allowed = 1;

	mtd->type = ui.type;
	mtd->size = ui.size;
	mtd->eb_size = ui.erasesize;
	mtd->min_io_size = ui.writesize;
	mtd->oob_size = ui.oobsize;

	if (mtd->min_io_size <= 0) {
		errmsg("mtd%d (%s) has insane min. I/O unit size %d",
		       mtd->mtd_num, node, mtd->min_io_size);
		goto out_close;
	}
	if (mtd->eb_size <= 0 || mtd->eb_size < mtd->min_io_size) {
		errmsg("mtd%d (%s) has insane eraseblock size %d",
		       mtd->mtd_num, node, mtd->eb_size);
		goto out_close;
	}
	if (mtd->size <= 0 || mtd->size < mtd->eb_size) {
		errmsg("mtd%d (%s) has insane size %lld",
		       mtd->mtd_num, node, mtd->size);
		goto out_close;
	}
	mtd->eb_cnt = mtd->size / mtd->eb_size;

	switch (mtd->type) {
	case MTD_ABSENT:
		errmsg("mtd%d (%s) is removable and is not present",
		       mtd->mtd_num, node);
		goto out_close;
	case MTD_RAM:
		strcpy((char *)mtd->type_str, "ram");
		break;
	case MTD_ROM:
		strcpy((char *)mtd->type_str, "rom");
		break;
	case MTD_NORFLASH:
		strcpy((char *)mtd->type_str, "nor");
		break;
	case MTD_NANDFLASH:
		strcpy((char *)mtd->type_str, "nand");
		break;
	case MTD_MLCNANDFLASH:
		strcpy((char *)mtd->type_str, "mlc-nand");
		break;
	case MTD_DATAFLASH:
		strcpy((char *)mtd->type_str, "dataflash");
		break;
	case MTD_UBIVOLUME:
		strcpy((char *)mtd->type_str, "ubi");
		break;
	default:
		goto out_close;
	}

	if (ui.flags & MTD_WRITEABLE)
		mtd->writable = 1;
	mtd->subpage_size = mtd->min_io_size;

	close(fd);

	ret = legacy_get_mtd_oobavail(node);
	mtd->oobavail = ret > 0 ? ret : 0;

	/*
	 * Unfortunately, the device name is not available via ioctl, and
	 * we have to parse /proc/mtd to get it.
	 */
	ret = proc_parse_start(&pi);
	if (ret)
		return -1;

	while (proc_parse_next(&pi)) {
		if (pi.mtd_num == mtd->mtd_num) {
			strcpy((char *)mtd->name, pi.name);
			return 0;
		}
	}

	errmsg("mtd%d not found in \"%s\"", mtd->mtd_num, MTD_PROC_FILE);
	errno = ENOENT;
	return -1;

out_close:
	close(fd);
	return -1;
}

/**
 * legacy_get_dev_info1 - legacy version of 'mtd_get_dev_info1()'.
 * @mtd_num: MTD device number
 * @mtd: the MTD device information is returned here
 *
 * This function is similar to 'mtd_get_dev_info1()' and has the same
 * conventions.
 */
int legacy_get_dev_info1(int mtd_num, struct mtd_dev_info *mtd)
{
	char node[sizeof(MTD_DEV_PATT) + 20];

	sprintf(node, MTD_DEV_PATT, mtd_num);
	return legacy_get_dev_info(node, mtd);
}

/**
 * legacy_get_dev_info2 - legacy version of `mtd_get_dev_info2()`
 * @name: name of the MTD device
 * @mtd: the MTD device information is returned here
 *
 * This function is similar to 'mtd_get_dev_info2()' and has the same
 * conventions.
 */
int legacy_get_dev_info2(const char *name, struct mtd_dev_info *mtd)
{
	struct proc_parse_info pi;
	int ret, mtd_num = -1;

	ret = proc_parse_start(&pi);
	if (ret)
		return -1;

	while (proc_parse_next(&pi)) {
		if (!strcmp(name, pi.name)) {
			// Device name collision
			if (mtd_num >= 0) {
				errmsg("Multiple MTD's found matching name %s", name);
				errno = ENODEV;
				return -1;
			}

			mtd_num = pi.mtd_num;
		}
	}

	if (mtd_num < 0) {
		errno = ENODEV;
		return -1;
	}

	return legacy_get_dev_info1(mtd_num, mtd);
}
