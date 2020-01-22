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
#ifndef _DEVICE_H_
#define _DEVICE_H_
#include <sys/types.h>
#include <stdbool.h>

enum BUS_TYPE {	BUS_TYPE_USB = 1, BUS_TYPE_PCI };

/*
 * Relevant USB-interface info.
 */
typedef struct iface_s {
	uint16_t class;
	uint16_t subclass;
	uint16_t protocol;
} iface_t;

/*
 * Struct to represent a device.
 */
typedef struct devinfo_s {
	char	*descr;
	char	**drivers;		/* List of associated drivers */
	uint8_t  bus;
	uint16_t vendor;		/* Vendor ID */
	uint16_t subvendor;		/* Subvendor ID */
	uint16_t device;		/* Device/product ID */
	uint16_t subdevice;		/* Subdevice ID */
	uint16_t class;			/* USB/PCI device class */
	uint16_t subclass;		/* USB/PCI device subclass */
	uint16_t revision;		/* Device revision. */
	uint16_t ndrivers;		/* # of drivers for this device */
	uint16_t nifaces;		/* # of USB interfaces. */
	iface_t *iface;			/* USB interfaces. */
} devinfo_t;

extern bool	 match_ifsubclass(const devinfo_t *, uint16_t);
extern bool	 match_ifclass(const devinfo_t *, uint16_t);
extern bool	 match_ifprotocol(const devinfo_t *, uint16_t);
extern void	 add_driver(devinfo_t *, const char *);
extern char	 *get_devdescr(const devinfo_t *);
extern devinfo_t **init_devlist(void);
extern devinfo_t **get_pci_devs(devinfo_t ***);
extern devinfo_t **get_usb_devs(devinfo_t ***);
#endif
