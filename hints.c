/*-
 * Copyright (c) 2021 Marcel Kaiser. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <err.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/module.h>
#include <sys/linker.h>

#include "log.h"

#define ERROR(ret, fmt, ...) do { \
	warnx(fmt, ##__VA_ARGS__); \
	return (ret); \
} while (0)

typedef struct pnp_info_list_s {
	int    vidx;		/* Index of "vendor" value in records. */
	int    didx;		/* Index of "device" value in records. */
	int    vendor;		/* Global "vendor" value for all records. */
	int    recsleft;	/* Remaining records */
	char   bus[16];		/* Bus name (e.g. "pci", "usb") */
	char   format[256];	/* Format string to parse the following recs */
	char   *rec;		/* Start of current record */
} pnp_info_list_t;

typedef struct hints_file_s {
	int    rectype;		/* Type of current record */
	int    recsize;		/* Size of current record. */
	char   *buf;		/* Buffer start */
	char   *rec;		/* Current record */
	char   *pos;		/* Current position in buf */
	size_t size;		/* Size of buf/hints file */
} hints_file_t;

static int  readint(hints_file_t *);
static int  init_pnp_info_list(hints_file_t *, pnp_info_list_t *);
static int  read_pnp_record(hints_file_t *, pnp_info_list_t *, int *, int *);
static char *readstr(hints_file_t *, size_t *);
static char *nextrec(hints_file_t *);
static char *next_matching(hints_file_t *hf, uint16_t vendor, uint16_t device);
static void free_hints_file(hints_file_t *);
static hints_file_t *read_hints_file(const char *);

static const char *hints_paths[] = {
	"/boot/kernel/linker.hints", "/boot/modules/linker.hints", NULL
};

/*
 * With each call, the function returns the next kernel module name for the
 * given vendor and device ID from the hints files in hints_paths[]. If no
 * further matching modules could be found, NULL is returned.
 */
char *
find_driver_pnp(uint16_t vendor, uint16_t device)
{
	static char *kmod;
	static const char   **path = NULL;
	static hints_file_t *hf = NULL;

	if (hf == NULL) {
		if (path == NULL)
			path = hints_paths;
		else
			path++;
		for (; *path != NULL; path++) {
			if ((hf = read_hints_file(*path)) != NULL)
				break;
		}
		if (hf == NULL) {
			path = NULL;
			return (NULL);
		}
	}
	if ((kmod = next_matching(hf, vendor, device)) != NULL)
		return (kmod);
	free_hints_file(hf);
	hf = NULL;
	return (find_driver_pnp(vendor, device));
}

/*
 * Returns the next module name for the given vendor and device ID found
 * in the given hints file.
 */
static char *
next_matching(hints_file_t *hf, uint16_t vendor, uint16_t device)
{
	int		d, v;
	char		*str;
	size_t		slen;
	static char	kmod[64];
	pnp_info_list_t	pi;

	kmod[0] = '\0';
	while (nextrec(hf) != NULL) {
		if (hf->rectype == MDT_MODULE) {
			(void)readstr(hf, &slen);
			str = readstr(hf, &slen);
			if (slen >= sizeof(kmod)) {
				warnx("Length of module name >= %lu",
				    sizeof(kmod));
				continue;
			}
			(void)strlcpy(kmod, str, slen + 1);
			if (slen > 3 && strncmp(kmod + slen - 3, ".ko", 3) == 0)
				kmod[slen - 3] = '\0';
			continue;
		} else if (strcmp(kmod, "kernel") == 0)
			continue;
		if (hf->rectype != MDT_PNP_INFO)
			continue;
		if (init_pnp_info_list(hf, &pi) == -1)
			continue;
		while (read_pnp_record(hf, &pi, &v, &d) != -1) {
			if (vendor == v && device == d)
				return (kmod);
		}
	}
	return (NULL);
}

static int
init_pnp_info_list(hints_file_t *hf, pnp_info_list_t *pi)
{
	int    idx;
	char   *str, *p;
	size_t slen;

	str = readstr(hf, &slen);
	if (slen >= sizeof(pi->bus))
		ERROR(-1, "Length of bus name >= %lu", sizeof(pi->bus));
	(void)strlcpy(pi->bus, str, slen + 1);
	if (strcmp(pi->bus, "pci") != 0 && strcmp(pi->bus, "usb") != 0)
		return (-1);
	str = readstr(hf, &slen);
	if (slen >= sizeof(pi->format))
		ERROR(-1, "Length of format string >= %lu", sizeof(pi->format));
	(void)strlcpy(pi->format, str, slen + 1);
	pi->recsleft = readint(hf);
	pi->rec      = hf->pos;
	pi->vendor   = pi->vidx = pi->didx = -1;
	for (idx = 0, p = pi->format; p != NULL && *p != '\0'; idx++) {
		if (*p == 'T') {
			/*
			 * Check if vendor is defined via the T field.
			 */
			p += 2;
			if (strncmp(p, "vendor=", 7) == 0)
				pi->vendor = strtol(p + 7, NULL, 16);
		} else
			p += 2;
		if (strncmp(p, "vendor", 6) == 0)
			pi->vidx = idx;
		else if (strncmp(p, "device", 6) == 0)
			pi->didx = idx;
		if ((p = strchr(p, ';')) != NULL)
			p++;
	}
	if (pi->didx == -1 || (pi->vidx == -1 && pi->vendor == -1))
		return (-1);
	return (0);
}

/*
 * Read the next available record from the PNP info list.
 */
static int
read_pnp_record(hints_file_t *hf, pnp_info_list_t *pi, int *vendor, int *device)
{
	int    idx, val;
	char   *p;
	size_t slen;

	if (pi->recsleft-- <= 0)
		return (-1);
	*vendor = pi->vendor;
	for (idx = 0, p = pi->format; p != NULL && *p != '\0'; idx++) {
		switch (*p) {
		case 'G':
		case 'I':
		case 'J':
		case 'L':
		case 'M':
			val = readint(hf);
			if (idx == pi->didx)
				*device = val;
			else if (idx == pi->vidx)
				*vendor = val;
			break;
		case 'D':
		case 'Z':
			(void)readstr(hf, &slen);
			break;
		}
		if ((p = strchr(p, ';')) != NULL)
			p++;
	}
	return (0);
}

/*
 * FIELD   SIZE
 * reclen  (sizeof(int)) <-- First record 
 * data    reclen
 * 
 * start(next record) == first record + reclen + sizeof(int)
 */
 
static char *
nextrec(hints_file_t *hf)
{
	if (hf->rec == NULL)
		return (NULL);
	hf->pos     = hf->rec;
	hf->recsize = readint(hf);
	/* Set to start of next record. */
	hf->rec += hf->recsize + sizeof(int);
	if (hf->rec >= hf->size + hf->buf)
		hf->rec = NULL;
	hf->rectype = readint(hf);

	return (hf->pos);
}

static hints_file_t *
read_hints_file(const char *path)
{
	int	     fd, version;
	struct stat  sb;
	hints_file_t *hf;

	if ((hf = malloc(sizeof(hints_file_t))) == NULL)
		die("malloc()");
	if (stat(path, &sb) == -1) {
		if (errno != ENOENT)
			die("stat(%s)", path);
		return (NULL);
	}
	if ((fd = open(path, O_RDONLY, 0)) == -1) {
		if (errno != ENOENT)
			die("open(%s)", path);
		return (NULL);
	}
	if ((hf->buf = malloc(sb.st_size)) == NULL)
		die("malloc()");
	if (read(fd, hf->buf, sb.st_size) == -1)
		die("read(%s)", path);
	(void)close(fd);
	hf->pos = hf->buf;
	version = readint(hf);
	if (version != LINKER_HINTS_VERSION) {
		warnx("Version mismatch (%d != %d) of file %s.\n",
		      version, LINKER_HINTS_VERSION, path);
		free_hints_file(hf);
		return (NULL);
	}
	hf->rec  = hf->pos;
	hf->size = sb.st_size;

	return (hf);
}

static int
readint(hints_file_t *hf)
{
	int *i = (int *)roundup2((intptr_t)(hf->pos), sizeof(int));
	hf->pos = (char *)&i[1];

	return (*i);
}

static char *
readstr(hints_file_t *hf, size_t *slen)
{
	char *s = hf->pos + 1;

	*slen = *(uint8_t *)(hf->pos);
	hf->pos = s + *slen;

	return (s);
}

static void
free_hints_file(hints_file_t *hf)
{
	free(hf->buf);
	free(hf);
}
