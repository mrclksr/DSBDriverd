/*-
 * Copyright (c) 2020 Marcel Kaiser. All rights reserved.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/pciio.h>
#include <unistd.h>
#include <libusb20_desc.h>
#include <libusb20.h>

#include "log.h"
#include "device.h"
#include "config.h"

#define PATH_PCI     "/dev/pci"
#define MAX_PCI_DEVS 32

enum DESCR_DB_COLUMS {
	DESCR_DB_VENDOR_COLUMN,	DESCR_DB_DEVICE_COLUMN,	DESCR_DB_SUB_COLUMN
};

static bool	 is_new(devinfo_t **, uint16_t, uint16_t, uint16_t, uint16_t);
static bool	 match_devdescr_column(const devinfo_t *, char *, int);
static void	 add_iface(devinfo_t *, uint16_t, uint16_t, uint16_t);
static char	 *get_next_word_start(char *);
static devinfo_t *add_device(devinfo_t ***);

void
add_driver(devinfo_t *dev, const char *driver)
{
	int i;

	for (i = 0; i < dev->ndrivers; i++) {
		if (strcmp(dev->drivers[i], driver) == 0)
			return;
	}
	dev->drivers = realloc(dev->drivers,
	    sizeof(char *) * (dev->ndrivers + 1));
	if (dev->drivers == NULL)
		die("realloc()");
	dev->drivers[dev->ndrivers] = strdup(driver);
	if (dev->drivers[dev->ndrivers] == NULL)
		die("strdup()");
	dev->ndrivers++;
}

static devinfo_t *
add_device(devinfo_t ***devlist)
{
	size_t	  len;
	devinfo_t **list, **p, *dev;
	
	if (devlist == NULL)
		return (NULL);
	for (len = 0, p = *devlist; p != NULL && *p != NULL; p++)
		len++;
	len++;
	list = realloc(*devlist, sizeof(devinfo_t *) * (len + 1));
	if (list == NULL)
		return (NULL);
	dev = malloc(sizeof(devinfo_t));
	if (dev == NULL)
		return (NULL);
	(void)memset(dev, 0, sizeof(devinfo_t));
	list[len - 1] = dev;
	list[len] = NULL;
	*devlist = list;

	return (dev);
}

static void
add_iface(devinfo_t *d, uint16_t class, uint16_t subclass, uint16_t protocol)
{
	d->iface = realloc(d->iface, sizeof(iface_t) * (d->nifaces + 1));
	if (d->iface == NULL)
		die("realloc()");
	d->iface[d->nifaces].class    = class;
	d->iface[d->nifaces].subclass = subclass;
	d->iface[d->nifaces].protocol = protocol;
	d->nifaces++;
}

bool
match_ifclass(const devinfo_t *d, uint16_t class)
{
	int i;

	for (i = 0; i < d->nifaces; i++) {
		if (d->iface[i].class == class)
			return (true);
	}
	return (false);
}

bool
match_ifsubclass(const devinfo_t *d, uint16_t subclass)
{
	int i;

	for (i = 0; i < d->nifaces; i++) {
		if (d->iface[i].subclass == subclass)
			return (true);
	}
	return (false);
}

bool
match_ifprotocol(const devinfo_t *d, uint16_t protocol)
{
	int i;

	for (i = 0; i < d->nifaces; i++) {
		if (d->iface[i].protocol == protocol)
			return (true);
	}
	return (false);
}

static bool
is_new(devinfo_t **devlist, uint16_t vendor, uint16_t device, uint16_t class,
	uint16_t subclass)
{
	devinfo_t **dev;
	
	for (dev = devlist; dev != NULL && *dev != NULL; dev++) {
		if ((*dev)->vendor == vendor && (*dev)->device == device &&
		    (*dev)->class  == class && (*dev)->subclass == subclass)
			return (false);
	}
	return (true);
}

devinfo_t **
get_pci_devs(devinfo_t ***devlist)
{
	int		   i, fd, n;
	size_t		   buflen;
	devinfo_t	   *dip, **tail;
	struct pci_conf	   *conf;
	struct pci_conf_io pc;

	conf = NULL; buflen = MAX_PCI_DEVS; n = 0;
	if ((fd = open(PATH_PCI, O_RDONLY, 0)) == -1)
		die("open(%s)", PATH_PCI);
	(void)memset(&pc, 0, sizeof(struct pci_conf_io));
	do {
		conf = realloc(conf, buflen * sizeof(struct pci_conf));
		if (conf == NULL)
			die("realloc()");
		pc.matches	 = conf;
		pc.match_buf_len = buflen; buflen += MAX_PCI_DEVS;

		if (ioctl(fd, PCIOCGETCONF, &pc) == -1)
			die("ioctl(PCIOCGETCONF)");
		if (pc.status == PCI_GETCONF_ERROR)
			die("ioctl(PCIOGETCONF) failed");
		for (i = 0; i < pc.num_matches; i++) {
			dip = add_device(devlist);
			dip->bus       = BUS_TYPE_PCI;
			dip->vendor    = conf[i].pc_vendor;
			dip->device    = conf[i].pc_device;
			dip->subvendor = conf[i].pc_subvendor;
			dip->subdevice = conf[i].pc_subdevice;
			dip->revision  = conf[i].pc_revid;
			dip->class     = conf[i].pc_class;
			dip->subclass  = conf[i].pc_subclass;
			dip->descr     = get_devdescr(dip);
			if (dip->descr != NULL) {
				if ((dip->descr = strdup(dip->descr)) == NULL)
					die("strdup()");
			}
			n++;
		}
	} while (pc.status == PCI_GETCONF_MORE_DEVS);
	free(conf);

	errno = 0;
	if (n == 0)
		return (NULL);
	for (tail = *devlist; tail != NULL && *tail != NULL; tail++)
		;
	return (&tail[-n]);
}

devinfo_t **
get_usb_devs(devinfo_t ***devlist)
{
	int			i, j, n;
	devinfo_t		*dip, **tail;
	struct libusb20_device	*pdev;
	struct libusb20_config	*usbcfg;
	struct libusb20_backend	*pbe;
	struct LIBUSB20_DEVICE_DESC_DECODED    *ddesc;
	struct LIBUSB20_INTERFACE_DESC_DECODED *idesc;

	pbe = libusb20_be_alloc_default();
	for (n = 0, pdev = NULL;
	    (pdev = libusb20_be_device_foreach(pbe, pdev));) {
		ddesc = libusb20_dev_get_device_desc(pdev);
		/* Check if we already have an entry for this device. */
		if (!is_new(*devlist, ddesc->idVendor, ddesc->idProduct,
		    ddesc->bDeviceClass, ddesc->bDeviceSubClass))
			continue;
		dip = add_device(devlist);
		dip->bus      = BUS_TYPE_USB;
		dip->vendor   = ddesc->idVendor;
		dip->device   = ddesc->idProduct;
		dip->class    = ddesc->bDeviceClass;
		dip->subclass = ddesc->bDeviceSubClass;
		for (i = 0; i < ddesc->bNumConfigurations; i++) {
			usbcfg = libusb20_dev_alloc_config(pdev, i);
			if (usbcfg == NULL && errno != ENXIO) {
				die("%s: libusb20_dev_alloc_config()",
				    libusb20_dev_get_desc(pdev));
			} else if (usbcfg == NULL) {
				logprint("%s: libusb20_dev_alloc_config()",
				    libusb20_dev_get_desc(pdev));
				continue;
			}
			for (j = 0; j < usbcfg->num_interface; j++) {
				idesc = &(usbcfg->interface[j].desc);
				add_iface(dip, idesc->bInterfaceClass,
				    idesc->bInterfaceSubClass,
				    idesc->bInterfaceProtocol);
			}
			free(usbcfg);
		}
		dip->descr = get_devdescr(dip);
		if (dip->descr != NULL) {
			if ((dip->descr = strdup(dip->descr)) == NULL)
				die("strdup()");
		}
		n++;
	}
	libusb20_be_free(pbe);

	errno = 0;
	if (n == 0)
		return (NULL);
	for (tail = *devlist; tail != NULL && *tail != NULL; tail++)
		;
	return (&tail[-n]);
}

devinfo_t **
init_devlist()
{
	devinfo_t **devlist = NULL;
	
	if (get_pci_devs(&devlist) == NULL || get_usb_devs(&devlist) == NULL) {
		if (errno != 0)
			return (NULL);
	}
	return (devlist);
}

char *
get_devdescr(const devinfo_t *dev)
{
	int	    column, matching_columns;
	FILE	    *fp;
	char	    *p, *descr;
	static char infostr[_POSIX2_LINE_MAX], line[_POSIX2_LINE_MAX];

	errno = 0;
	if (dev->bus == BUS_TYPE_PCI) {
		if ((fp = fopen(PATH_PCIID_DB0, "r")) == NULL &&
		    (fp = fopen(PATH_PCIID_DB1, "r")) == NULL) {
			logprint("Couldn't open PCI ID database");
			return (NULL);
		}
	} else if ((fp = fopen(PATH_USBID_DB, "r")) == NULL) {
		logprint("Couldn't open USB ID database");
		return (NULL);
	}
	column = matching_columns = 0; infostr[0] = '\0';

	if (fseek(fp, 0, SEEK_SET) == -1) {
		logprint("fseek()");
		(void)fclose(fp);
		return (NULL);
	}
	while (fgets(line, sizeof(line) - 1, fp) != NULL) {
		if ((p = get_next_word_start(line)) == NULL ||
		    *p == '#' || *p == '\n')
			continue;
		for (column = 0; line[column] == '\t'; column++)
			;
		if (column > matching_columns)
			continue;
		if (column < matching_columns) {
			(void)fclose(fp);
			return (infostr);
		}
		if (match_devdescr_column(dev, line, column)) {
			if ((descr = get_next_word_start(NULL)) == NULL)
				continue;
			if (column > 0)
				(void)strlcat(infostr, " ", sizeof(infostr));
			(void)strlcat(infostr, descr, sizeof(infostr));
			matching_columns++;
		}
	}
	(void)fclose(fp);
	if (column < matching_columns)
		return (infostr);
	return (NULL);
}

static bool
match_devdescr_column(const devinfo_t *dev, char *line, int column)
{
	int  val1, val2;
	char *id;

	(void)strtok(line, "#\n");
	if ((id = get_next_word_start(line)) == NULL)
		return (false);
	val1 = strtol(id, NULL, 16);
	if (column == DESCR_DB_VENDOR_COLUMN) {
		if (val1 == dev->vendor)
			return (true);
	} else if (column == DESCR_DB_DEVICE_COLUMN) {
		if (val1 == dev->device)
			return (true);
	} else if (column == DESCR_DB_SUB_COLUMN) {
		if ((id = get_next_word_start(NULL)) == NULL)
			return (false);
		val2 = strtol(id, NULL, 16);
		if (val1 == dev->subvendor && val2 == dev->subdevice)
			return (true);
	}
	return (false);
}

static char *
get_next_word_start(char *str)
{
	static char *next = NULL;

	if (str == NULL) {
		/* Get start of next word. */
		if ((str = next) == NULL)
			return (NULL);
	}
	while (isspace(*str))
		str++;
	if (*str == '\0')
		str = next = NULL;
	else {
		for (next = str; *next != '\0' && !isspace(*next); next++)
			;
	}
	return (str);
}
