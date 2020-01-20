/*-
 * Copyright (c) 2016 Marcel Kaiser. All rights reserved.
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
#include <stdarg.h>
#include <stdbool.h>
#include <signal.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#include <libutil.h>
#include <errno.h>
#include <fcntl.h>
#include <err.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/param.h>
#include <sys/module.h>
#include <sys/linker.h>
#include <sys/pciio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <paths.h>
#include <libusb20_desc.h>
#include <libusb20.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#ifdef TEST
# include <atf-c.h>
#endif

#define MAX_PCI_DEVS	     32
#define MAX_EXCLUDES	     256

#define SOCK_ERR_IO_ERROR    2
#define SOCK_ERR_CONN_CLOSED 1

enum DB_COLUMNS {
	DB_VENDOR_COLUMN = 1,
	DB_DEVICE_COLUMN,
	DB_SUBVENDOR_COLUMN,
	DB_SUBDEVICE_COLUMN
};

enum DESCR_DB_COLUMS {
	DESCR_DB_VENDOR_COLUMN,
	DESCR_DB_DEVICE_COLUMN,
	DESCR_DB_SUB_COLUMN
};

#define PATH_PCI	     "/dev/pci"
#define PATH_DEVD_SOCKET     "/var/run/devd.seqpacket.pipe"
#define IFCONFIG_CMD	     "ifconfig_%s=\"DHCP up\" /etc/rc.d/dhclient " \
			     "quietstart %s"

struct devd_event_s {
	int  system;
#define DEVD_SYSTEM_IFNET 1
#define DEVD_SYSTEM_USB	  2
	int  type;
#define DEVD_TYPE_ATTACH  1
	char *cdev;
	char *subsystem;
} devdevent;

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
#define BUS_TYPE_USB 1
#define BUS_TYPE_PCI 2
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

struct cfg_s {
	char	 **exclude;		/* List of modules to exclude */
	size_t	 exclude_len;		/* Length of exclude list */
} cfg = { (char **)NULL, 0 };

static int	 ndevs;			/* # of devices. */
static bool	 dryrun;		/* Do not load any drivers if true. */
static FILE	 *db;			/* File pointer for drivers database. */
static FILE	 *logfp;		/* File pointer for logprint(). */
static FILE	 *pcidb;		/* File pointer for PCI IDs database */
static FILE	 *usbdb;		/* File pointer for USB IDs database */
static char	 *exclude[MAX_EXCLUDES];/* List of drivers to exclude. */
static lua_State *cfgstate;
static devinfo_t *devlst;		/* List of devices. */
static struct pidfh *pfh;		/* PID file handle. */

static int	 uconnect(const char *);
static int	 devd_connect(void);
static int	 get_usb_devs(void);
static int	 get_pci_devs(void);
static int	 cfg_getint(lua_State *, const char *);
static int	 cfg_call_function(lua_State *L, const char *,
		     const devinfo_t *, const char *);
static int	 parse_devd_event(char *);
static bool	 has_driver(uint16_t, uint16_t);
static bool	 is_excluded(const char *);
static bool	 is_kmod_loaded(const char *);
static bool	 is_new(uint16_t, uint16_t, uint16_t, uint16_t);
static bool	 match_ifsubclass(const devinfo_t *, uint16_t);
static bool	 match_ifclass(const devinfo_t *, uint16_t);
static bool	 match_ifprotocol(const devinfo_t *, uint16_t);
static bool	 match_drivers_db_column(const devinfo_t *, char *, int);
static bool	 match_device_column(const devinfo_t *, char *);
static bool	 match_kmod_name(const char *, const char *);
static void	 show_drivers(uint16_t, uint16_t);
static void	 lockpidfile(void);
static void	 add_driver(devinfo_t *, const char *);
static void	 add_iface(devinfo_t *, uint16_t, uint16_t, uint16_t);
static void	 load_driver(devinfo_t *);
static void	 logprint(const char *, ...);
static void	 logprintx(const char *, ...);
static void	 open_dbs(void);
static void	 open_cfg(void);
static void	 daemonize(void);
static void	 usage(void);
static void	 print_pci_devinfo(const devinfo_t *);
static void	 print_usb_devinfo(const devinfo_t *);
static void	 cfg_setint_tbl_field(lua_State *, const char *, int);
static void	 cfg_setstr_tbl_field(lua_State *, const char *, const char *);
static void	 cfg_add_interface_tbl(lua_State *, const iface_t *);
static void	 cfg_dev_to_tbl(lua_State *, const devinfo_t *dev);
static char	 *read_devd_event(int, int *);
static char	 *find_driver(const devinfo_t *);
static char	 *get_devdescr(FILE *, const devinfo_t *);
static char	 *cfg_getstr(lua_State *, const char *);
static char	 **cfg_getstrarr(lua_State *, const char *, size_t *);
static devinfo_t *add_device(void);

#ifndef TEST
int
main(int argc, char *argv[])
{
	int	 ch, error, i, n, s;
	char	 *ln, *p;
	bool	 cflag, fflag, lflag;
	fd_set	 rset;
	uint16_t vendor, device;

	cflag = fflag = dryrun = lflag = false;
	while ((ch = getopt(argc, argv, "c:flnhx:")) != -1) {
		switch (ch) {
		case 'c':
			cflag = true;
			for (i = 0, p = optarg; (p = strtok(p, ":")) != NULL;
			    i++, p = NULL) {
				if (i == 0)
					vendor = strtol(p, NULL, 16);
				else
					device = strtol(p, NULL, 16);
			}
			if (i != 2)
				usage();
			break;
		case 'f':
			fflag = true;
			break;
		case 'l':
			lflag = true;
			break;
		case 'n':
			dryrun = true;
			break;
		case 'x':
			for (i = 0, p = optarg; i < MAX_EXCLUDES - 1 &&
			    (p = strtok(p, ",")) != NULL; i++, p = NULL) {
				exclude[i] = p;
			}
			if (i >= MAX_EXCLUDES - 1) {
				errx(EXIT_FAILURE,
				    "Number of elements in exclude list " \
				    "exceeds %d", MAX_EXCLUDES - 1);
			}
			exclude[i] = NULL;
			break;
		case 'h':
		case '?':
			usage();
		}
	}
	if (cflag || lflag) {
		open_dbs();
		(void)get_pci_devs();
		(void)get_usb_devs();
	}
	if (cflag) {
		if (has_driver(vendor, device)) {
			show_drivers(vendor, device);
			return (EXIT_SUCCESS);
		}
		return (EXIT_FAILURE);
	}
	if (lflag) {
		for (i = 0; i < ndevs; i++) {
			if (devlst[i].bus == BUS_TYPE_PCI)
				print_pci_devinfo(&devlst[i]);
			else if (devlst[i].bus == BUS_TYPE_USB)
				print_usb_devinfo(&devlst[i]);
		}
		return (EXIT_SUCCESS);
	}
	lockpidfile();
	if (!fflag)
		daemonize();
	if ((s = devd_connect()) == -1)
		err(EXIT_FAILURE, "Couldn't connect to %s", PATH_DEVD_SOCKET);
	open_cfg();
	open_dbs();

	(void)get_pci_devs();
	(void)get_usb_devs();

	for (i = 0; i < ndevs; i++)
		load_driver(&devlst[i]);
	for (;;) {
		FD_ZERO(&rset); FD_SET(s, &rset);
		while (select(s + 1, &rset, NULL, NULL, NULL) == -1) {
			if (errno == EINTR)
				continue;
			else
				err(EXIT_FAILURE, "select()");
			/* NOTREACHED */
		}
		if (!FD_ISSET(s, &rset))
			continue;
		while ((ln = read_devd_event(s, &error)) != NULL) {
			if (parse_devd_event(ln) == -1)
				continue;
			if (devdevent.type != DEVD_TYPE_ATTACH)
				continue;
			if (devdevent.system == DEVD_SYSTEM_USB) {
				n = get_usb_devs();
				for (i = ndevs - n; i < ndevs; i++)
					load_driver(&devlst[i]);
			}
		}
		if (error == SOCK_ERR_CONN_CLOSED) {
			/* Lost connection to devd. */
			(void)close(s);
			logprintx("Lost connection to devd. " \
			    "Reconnecting ...");
			if ((s = devd_connect()) == -1) {
				logprintx("Connecting to devd " \
				    "failed. Giving up.");
				exit(EXIT_FAILURE);
			}
		} else if (error == SOCK_ERR_IO_ERROR)
			err(EXIT_FAILURE, "read_devd_event()");
	}
	/* NOTREACHED */
	return (EXIT_SUCCESS);
}
#else
# include "test.h"
#endif

static void
usage()
{
	(void)printf("Usage: %s [-h]\n" \
	       "       %s [-l|-c vendor:device] | [-fn][-x driver,...]\n",
	       PROGRAM, PROGRAM);
	exit(EXIT_FAILURE);
}

static void
daemonize()
{
	int i;

	/* Close all files except for pidfile and stderr. */
	for (i = 0; i < 16; i++) {
		if (pidfile_fileno(pfh) != i && fileno(stderr) != i)
			(void)close(i);
	}
	/* Redirect error messages to logfile. */
	if ((logfp = fopen(PATH_LOG, "a+")) == NULL)
		err(EXIT_FAILURE, "fopen()");
	(void)setvbuf(logfp, NULL, _IOLBF, 0);
	(void)fclose(stderr);
	/* For warn(), err(), etc. */
	err_set_file(logfp);
	logprintx("%s started", PROGRAM);
	if (daemon(0, 1) == -1)
		err(EXIT_FAILURE, "Failed to daemonize");
	(void)pidfile_write(pfh);
}

static void
print_pci_devinfo(const devinfo_t *dev)
{
	char		*p;
	const devinfo_t	*dp;

	for (dp = dev; (p = find_driver(dp)) != NULL; dp = NULL) {
		(void)printf("vendor=%04x product=%04x " \
		    "class=%02x subclass=%02x bus=PCI %s: %s\n",
		    dev->vendor, dev->device, dev->class, dev->subclass,
		    dev->descr != NULL ? dev->descr : "", p);
	}
}

static void
die(const char *msg)
{
	logprint(msg);
	exit(EXIT_FAILURE);
}

static void
print_usb_devinfo(const devinfo_t *dev)
{
	int		i;
	char		*p;
	const devinfo_t *dp;

	for (dp = dev; (p = find_driver(dp)) != NULL; dp = NULL) {
		(void)printf("vendor=%04x product=%04x " \
		    "class=%02x subclass=%02x bus=USB %s: %s\n",
		    dev->vendor, dev->device, dev->class, dev->subclass,
		    dev->descr != NULL ? dev->descr : "", p);
		for (i = 0; i < dev->nifaces; i++) {
			(void)printf("vendor=%04x product=%04x "    \
			    "ifclass=%02x ifsubclass=%02x bus=USB " \
			    "protocol=%02x %s: %s\n",
			    dev->vendor, dev->device,
			    dev->iface[i].class, dev->iface[i].subclass,
			    dev->iface[i].protocol,
			    dev->descr != NULL ? dev->descr : "", p);
		}
	}
}

static void
show_drivers(uint16_t vendor, uint16_t device)
{
	char		*info;
	devinfo_t	dev;
	const char	*p;
	const devinfo_t	*dp;

	bzero(&dev, sizeof(dev));
	dev.vendor = vendor;
	dev.device = device;

	if ((info = get_devdescr(pcidb, &dev)) == NULL)
		info = get_devdescr(usbdb, &dev);
	for (dp = &dev; (p = find_driver(dp)) != NULL; dp = NULL)
		(void)printf("%s: %s\n", info != NULL ? info: "", p);
}

static bool
has_driver(uint16_t vendor, uint16_t device)
{
	devinfo_t dev;

	bzero(&dev, sizeof(dev));
	dev.vendor = vendor;
	dev.device = device;

	return (find_driver(&dev) != NULL ? true : false);
}

static void
lockpidfile()
{

	/* Check if deamon is already running. */
	if ((pfh = pidfile_open(PATH_PID_FILE, 0600, NULL)) == NULL) {
		if (errno == EEXIST)
			errx(EXIT_FAILURE, "%s is already running.", PROGRAM);
		err(EXIT_FAILURE, "Failed to create PID file.");
	}
}

void
logprint(const char *fmt, ...)
{
	char    msgbuf[512], errstr[64], *tm;
	time_t  clock;
	va_list ap;

	if (logfp == NULL)
		logfp = stderr;
	va_start(ap, fmt);
	(void)vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
	clock = time(NULL); tm = ctime(&clock); (void)strtok(tm, "\n");
	(void)strerror_r(errno, errstr, sizeof(errstr));
	(void)fprintf(logfp, "%s: %s: %s\n", tm, msgbuf, errstr);
}

void
logprintx(const char *fmt, ...)
{
	char    msgbuf[512], *tm;
	time_t  clock;
	va_list ap;

	if (logfp == NULL)
		logfp = stderr;
	va_start(ap, fmt);
	(void)vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
	clock = time(NULL); tm = ctime(&clock); (void)strtok(tm, "\n");
	(void)fprintf(logfp, "%s: %s\n", tm, msgbuf);
}

static int
uconnect(const char *path)
{
	int s;
	struct sockaddr_un saddr;

	if ((s = socket(PF_LOCAL, SOCK_SEQPACKET, 0)) == -1)
		return (-1);
	(void)memset(&saddr, (unsigned char)0, sizeof(saddr));
	(void)snprintf(saddr.sun_path, sizeof(saddr.sun_path), "%s", path);
	saddr.sun_family = AF_LOCAL;
	if (connect(s, (struct sockaddr *)&saddr, sizeof(saddr)) == -1)
		return (-1);
	if (fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK) == -1)
		return (-1);
	return (s);
}

static int
devd_connect()
{
	int  i, s;

	for (i = 0, s = -1; i < 30 && s == -1; i++) {
		if ((s = uconnect(PATH_DEVD_SOCKET)) == -1)
			(void)sleep(1);
	}
	return (s);
}

static char *
read_devd_event(int s, int *error)
{
	int	      n, rd;
	static int    bufsz = 0;
	static char   seq[1024], *lnbuf = NULL;
	struct iovec  iov;
	struct msghdr msg;

	if (lnbuf == NULL) {
		if ((lnbuf = malloc(_POSIX2_LINE_MAX)) == NULL)
			return (NULL);
		bufsz = _POSIX2_LINE_MAX;
	}
	iov.iov_len  = sizeof(seq);
	iov.iov_base = seq;
	msg.msg_iov  = &iov;
	msg.msg_iovlen = 1;

	for (n = rd = *error = 0;;) {
		msg.msg_name    = msg.msg_control = NULL;
		msg.msg_namelen = msg.msg_controllen = 0;

		if ((n = recvmsg(s, &msg, 0)) == (ssize_t)-1) {
			if (errno == EINTR)
				continue;
			if (errno == ECONNRESET) {
				*error = SOCK_ERR_CONN_CLOSED;
				return (NULL);
			}
			if (errno == EAGAIN)
				return (NULL);
			err(EXIT_FAILURE, "recvmsg()");
		} else if (n == 0 && rd == 0) {
			*error = SOCK_ERR_CONN_CLOSED;
			return (NULL);
		}
		if (rd + n + 1 > bufsz) {
			if ((lnbuf = realloc(lnbuf, rd + n + 65)) == NULL)
				err(EXIT_FAILURE, "realloc()");
			bufsz = rd + n + 65;
		}
		(void)memcpy(lnbuf + rd, seq, n);
		rd += n; lnbuf[rd] = '\0';
		if (msg.msg_flags & MSG_TRUNC) {
			logprint("recvmsg(): Message truncated");
			return (rd > 0 ? lnbuf : NULL);
		} else if (msg.msg_flags & MSG_EOR)
			return (lnbuf);
	}
	return (NULL);
}

static int
parse_devd_event(char *str)
{
	char *p, *q;

	devdevent.cdev = devdevent.subsystem = "";
	if (str[0] != '!')
		return (-1);
	for (p = str + 1; (p = strtok(p, " \n")) != NULL; p = NULL) {
		if ((q = strchr(p, '=')) == NULL)
			continue;
		*q++ = '\0';
		if (strcmp(p, "system") == 0) {
			if (strcmp(q, "IFNET") == 0)
				devdevent.system = DEVD_SYSTEM_IFNET;
			else if (strcmp(q, "USB") == 0)
				devdevent.system = DEVD_SYSTEM_USB;
			else
				devdevent.system = -1;
		} else if (strcmp(p, "subsystem") == 0) {
			devdevent.subsystem = q;
		} else if (strcmp(p, "type") == 0) {
			if (strcmp(q, "ATTACH") == 0)
				devdevent.type = DEVD_TYPE_ATTACH;
			else
				devdevent.type = -1;
		} else if (strcmp(p, "cdev") == 0)
			devdevent.cdev = q;
        }
	return (0);
}

static void
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
add_device()
{
	devlst = realloc(devlst, sizeof(devinfo_t) * (ndevs + 1));
	if (devlst == NULL)
		err(EXIT_FAILURE, "realloc()");
	(void)memset(&devlst[ndevs], 0, sizeof(devinfo_t));
	return (&devlst[ndevs++]);
}

static void
add_iface(devinfo_t *d, uint16_t class, uint16_t subclass, uint16_t protocol)
{
	d->iface = realloc(d->iface, sizeof(iface_t) * (d->nifaces + 1));
	if (d->iface == NULL)
		errx(EXIT_FAILURE, "realloc()");
	d->iface[d->nifaces].class    = class;
	d->iface[d->nifaces].subclass = subclass;
	d->iface[d->nifaces].protocol = protocol;
	d->nifaces++;
}

static bool
match_ifclass(const devinfo_t *d, uint16_t class)
{
	int i;

	for (i = 0; i < d->nifaces; i++) {
		if (d->iface[i].class == class)
			return (true);
	}
	return (false);
}

static bool
match_ifsubclass(const devinfo_t *d, uint16_t subclass)
{
	int i;

	for (i = 0; i < d->nifaces; i++) {
		if (d->iface[i].subclass == subclass)
			return (true);
	}
	return (false);
}

static bool
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
is_new(uint16_t vendor, uint16_t device, uint16_t class, uint16_t subclass)
{
	int i;

	for (i = 0; i < ndevs; i++) {
		if (devlst[i].vendor == vendor && devlst[i].device == device &&
		    devlst[i].class  == class && devlst[i].subclass == subclass)
			return (false);
	}
	return (true);
}

static void
open_dbs()
{

	if ((db = fopen(PATH_DRIVERS_DB, "r")) == NULL)
		err(EXIT_FAILURE, "fopen(%s)", PATH_DRIVERS_DB);
	if ((pcidb = fopen(PATH_PCIID_DB0, "r")) == NULL &&
	    (pcidb = fopen(PATH_PCIID_DB1, "r")) == NULL)
		logprint("Warning: Could not open PCI IDs database");
	if ((usbdb = fopen(PATH_USBID_DB, "r")) == NULL)
		logprint("Warning: Could not open USB IDs database");
}

static int
cfg_getint(lua_State *L, const char *var)
{
	int val = -1;

	lua_getglobal(L, var);
	if (lua_isnumber(L, -1))
		val = lua_tointeger(L, -1);
	lua_pop(L, 1);

	return (val);
}

static char *
cfg_getstr(lua_State *L, const char *var)
{
	char *val = NULL;

	lua_getglobal(L, var);
	if (!lua_isnil(L, -1))
		val = strdup(lua_tostring(L, -1));
	lua_pop(L, 1);

	return (val);
}

static char **
cfg_getstrarr(lua_State *L, const char *var, size_t *len)
{
	int  i;
	char **arr;

	lua_getglobal(L, var);
	if (lua_isnil(L, -1))
		return (NULL);
	if (lua_type(L, -1) != LUA_TTABLE) {
		logprintx("Syntax error: '%s' is not a string list", var);
		return (NULL);
	}
	*len = lua_rawlen(L, -1);
	if ((arr = malloc(*len * sizeof(char *))) == NULL)
		return (NULL);
	for (i = 0; i < *len; i++) {
		lua_rawgeti(L, -1, i + 1);
		if ((arr[i] = strdup(lua_tostring(L, -1))) == NULL) {
			free(arr);
			return (NULL);
		}
		lua_pop(L, 1);
	}
	return (arr);
}

static void
cfg_setint_tbl_field(lua_State *L, const char *name, int val)
{
	lua_pushnumber(L, val);
	lua_setfield(L, -2, name);
}

static void
cfg_setstr_tbl_field(lua_State *L, const char *name, const char *val)
{
	if (val == NULL)
		lua_pushnil(L);
	else
		lua_pushstring(L, val);
	lua_setfield(L, -2, name);
}

static void
cfg_add_interface_tbl(lua_State *L, const iface_t *iface)
{
	lua_newtable(L);
	cfg_setint_tbl_field(L, "class", iface->class);
	cfg_setint_tbl_field(L, "subclass", iface->subclass);
	cfg_setint_tbl_field(L, "protocol", iface->protocol);
}

/*
 * Create a Lua table from the given devinfo_t * object, and push it on the
 * Lua stack.
 */
static void
cfg_dev_to_tbl(lua_State *L, const devinfo_t *dev)
{
	int i;

	lua_newtable(L);
	cfg_setint_tbl_field(L, "bus", dev->bus);
	cfg_setint_tbl_field(L, "vendor", dev->vendor);
	cfg_setint_tbl_field(L, "device", dev->device);
	cfg_setint_tbl_field(L, "subvendor", dev->subvendor);
	cfg_setint_tbl_field(L, "subdevice", dev->subdevice);
	cfg_setint_tbl_field(L, "class", dev->class);
	cfg_setint_tbl_field(L, "subclass", dev->subclass);
	cfg_setint_tbl_field(L, "revision", dev->revision);
	cfg_setint_tbl_field(L, "nifaces", dev->nifaces);
	cfg_setstr_tbl_field(L, "descr", dev->descr);
	cfg_setint_tbl_field(L, "ndrivers", dev->ndrivers);

	lua_newtable(L);
	for (i = 0; i < dev->ndrivers; i++) {
		lua_pushstring(L, dev->drivers[i]);
		lua_rawseti(L, -2, i + 1);
	}
	lua_setfield(L, -2, "drivers");
	lua_newtable(L);
	for (i = 0; i < dev->nifaces; i++) {
		cfg_add_interface_tbl(L, &dev->iface[i]);
		lua_rawseti(L, -2, i + 1);
	}
	lua_setfield(L, -2, "iface");
}

static int
cfg_call_function(lua_State *L, const char *fname, const devinfo_t *dev,
	const char *kmod)
{
	int error, nargs = 0;

	error = -1;
	if (L == NULL)
		return (-1);
	lua_getglobal(L, fname);
	if (lua_isnil(L, -1))
		goto out;
	if (lua_type(L, -1) != LUA_TFUNCTION) {
		logprintx("Syntax error: '%s' is not a function", fname);
		goto out;
	}
	if (dev != NULL) {
		/* Push the given devinfo_t * object onto the Lua stack. */
		cfg_dev_to_tbl(L, dev);
	}
	if (strcmp(fname, "on_load_kmod") == 0 ||
	    strcmp(fname, "affirm") == 0) {
		lua_pushstring(L, kmod);
		nargs = 2;
	} else if (strcmp(fname, "init") == 0)
		nargs = 0;
	else
		nargs = 1;
	if (lua_pcall(L, nargs, 1, 0) != 0) {
		logprintx("%s(): %s", fname, lua_tostring(cfgstate, -1));
		goto out;
	}
	error = lua_tointeger(L, -1);
out:
	lua_settop(L, 0);

	return (error);
}

static void
open_cfg()
{
	int i, j;

	cfgstate = NULL;
	if (access(PATH_CFG_FILE, R_OK) == -1) {
		if (errno == ENOENT)
			return;
		die("access()");
	}
	cfgstate = luaL_newstate();
	luaL_openlibs(cfgstate);
	if (luaL_dofile(cfgstate, PATH_CFG_FILE) != 0)
		die(lua_tostring(cfgstate, -1));
	cfg_call_function(cfgstate, "init", NULL, NULL);
	cfg.exclude = cfg_getstrarr(cfgstate, "exclude_kmods",
	    &cfg.exclude_len);
	if (cfg.exclude != NULL) {
		for (i = 0; exclude[i] != NULL; i++)
			;
		for (j = 0; i < MAX_EXCLUDES - 1 && j < cfg.exclude_len; j++)
			exclude[i++] = cfg.exclude[j];
		exclude[i] = NULL;
	}
}

static int
get_pci_devs()
{
	int    i, fd, n;
	size_t buflen;
	devinfo_t	   *dip;
	struct pci_conf	   *conf;
	struct pci_conf_io pc;

	conf = NULL; buflen = MAX_PCI_DEVS; n = 0;
	if ((fd = open(PATH_PCI, O_RDONLY, 0)) == -1)
		err(EXIT_FAILURE, "open(%s)", PATH_PCI);
	(void)memset(&pc, 0, sizeof(struct pci_conf_io));
	do {
		conf = realloc(conf, buflen * sizeof(struct pci_conf));
		if (conf == NULL)
			err(EXIT_FAILURE, "realloc()");
		pc.matches	 = conf;
		pc.match_buf_len = buflen; buflen += MAX_PCI_DEVS;

		if (ioctl(fd, PCIOCGETCONF, &pc) == -1)
			err(EXIT_FAILURE, "ioctl(PCIOCGETCONF)");
		if (pc.status == PCI_GETCONF_ERROR)
			errx(EXIT_FAILURE,"ioctl(PCIOGETCONF) failed");
		for (i = 0; i < pc.num_matches; i++) {
			dip = add_device();
			dip->bus       = BUS_TYPE_PCI;
			dip->vendor    = conf[i].pc_vendor;
			dip->device    = conf[i].pc_device;
			dip->subvendor = conf[i].pc_subvendor;
			dip->subdevice = conf[i].pc_subdevice;
			dip->revision  = conf[i].pc_revid;
			dip->class     = conf[i].pc_class;
			dip->subclass  = conf[i].pc_subclass;
			dip->descr     = get_devdescr(pcidb, dip);
			if (dip->descr != NULL) {
				if ((dip->descr = strdup(dip->descr)) == NULL)
					die("strdup()");
			}
			n++;
			if (cfgstate != NULL) {
				cfg_call_function(cfgstate, "on_add_device",
				    dip, NULL);
			}
		}
	} while (pc.status == PCI_GETCONF_MORE_DEVS);
	free(conf);

	return (n);
}

static int
get_usb_devs()
{
	int i, j, n;
	devinfo_t		*dip;
	struct libusb20_device	*pdev;
	struct libusb20_config	*cfg;
	struct libusb20_backend	*pbe;
	struct LIBUSB20_DEVICE_DESC_DECODED    *ddesc;
	struct LIBUSB20_INTERFACE_DESC_DECODED *idesc;

	pbe = libusb20_be_alloc_default();
	for (n = 0, pdev = NULL;
	    (pdev = libusb20_be_device_foreach(pbe, pdev));) {
		ddesc = libusb20_dev_get_device_desc(pdev);
		/* Check if we already have an entry for this device. */
		if (!is_new(ddesc->idVendor, ddesc->idProduct,
		    ddesc->bDeviceClass, ddesc->bDeviceSubClass))
			continue;
		dip = add_device();
		dip->bus      = BUS_TYPE_USB;
		dip->vendor   = ddesc->idVendor;
		dip->device   = ddesc->idProduct;
		dip->class    = ddesc->bDeviceClass;
		dip->subclass = ddesc->bDeviceSubClass;
		for (i = 0; i < ddesc->bNumConfigurations; i++) {
			cfg = libusb20_dev_alloc_config(pdev, i);
			if (cfg == NULL && errno != ENXIO) {
				err(EXIT_FAILURE,
				    "%s: libusb20_dev_alloc_config()",
				    libusb20_dev_get_desc(pdev));
			} else if (cfg == NULL) {
				warn("%s: libusb20_dev_alloc_config()",
				    libusb20_dev_get_desc(pdev));
				continue;
			}
			for (j = 0; j < cfg->num_interface; j++) {
				idesc = &(cfg->interface[j].desc);
				add_iface(dip, idesc->bInterfaceClass,
				    idesc->bInterfaceSubClass,
				    idesc->bInterfaceProtocol);
			}
			free(cfg);
		}
		dip->descr = get_devdescr(usbdb, dip);
		if (dip->descr != NULL) {
			if ((dip->descr = strdup(dip->descr)) == NULL)
				die("strdup()");
		}
		if (cfgstate != NULL) {
			cfg_call_function(cfgstate, "on_add_device",
			    dip, NULL);
		}
		n++;
	}
	libusb20_be_free(pbe);
	
	return (n);
}

/*
 * Returns the first (d != NULL) or next (d == NULL) matching driver for
 * device.
 */
static char *
find_driver(const devinfo_t *dev)
{
	int	    matching_columns, prev_column, curr_column;
	bool	    skip;
	long	    len;
	char	    ln[_POSIX2_LINE_MAX], *p, *lp;
	static char *last = NULL;
	static char driver[_POSIX2_LINE_MAX];
	static const devinfo_t *curdev = NULL;

	skip = false;
	if (dev == NULL) {
		/* Try to find more drivers for previously defined dev. */
		dev = curdev;
		if (last != NULL) {
			p = strtok_r(NULL, "\t ", &last);
			if (p != NULL)
				return (p);
			last = NULL;
			/* Skip to next driver record. */
			skip = true;
		}
	} else {
		if (fseek(db, 0, SEEK_SET) == -1)
			err(EXIT_FAILURE, "fseek()");
		curdev = dev;
	}
	matching_columns = prev_column = 0;
	while (fgets(ln, sizeof(ln), db) != NULL) {
		len = strlen(ln);
		/* Remove '\r', '\n', and '#' */
		lp = ln;
		(void)strsep(&lp, "#\r\n");
		if (ln[0] == '\0') {
			/* Skip empty lines */
			continue;
		}
		/*
		 * Count depth (columns/# of tabs). Ignore space characters
		 * inbetween.
		 */
		for (curr_column = 0, lp = ln; *lp != '\0'; lp++) {
			if (*lp == '\t')
				curr_column++;
			else if (*lp != ' ')
				break;
		}
		/* Skip whitespace-only lines */
		if (*lp == '\0')
			continue;
		if (skip && curr_column > 0)
			continue;
		else if (skip) {
			/*
			 * We skipped all lines so far, and reached the
			 * the next driver record. Reset "matching_columns"
			 * and prev_column
			 */
			skip = false;
			prev_column = matching_columns = 0;
		}
		if (curr_column < prev_column) {
			if (curr_column == 0) {
				/*
				 * We are at the beginning of a new driver
				 * record. Get the driver name in the next
				 * iteration.
				 */
				(void)fseek(db, -len, SEEK_CUR);
			}
			if (prev_column == matching_columns)
				return (strtok_r(driver, "\t ", &last));
			if (curr_column <= matching_columns) {
				skip = true;
				continue;
			}
		} else if (curr_column == 0) {
			/*
			 * At the beginning of a new record. Get the driver
			 * name.
			 */
			matching_columns = prev_column = 0; last = NULL;
			(void)strncpy(driver, ln, sizeof(driver));
			continue;
		} else if (curr_column - matching_columns >= 2)
			continue;
		/* Skip leading tabs and spaces */
		lp = ln + strspn(ln, "\t ");
		if (*lp == '\0')
			continue;
		prev_column = curr_column;
		if (match_drivers_db_column(dev, lp, curr_column))
			matching_columns++;
	}
	if (matching_columns > 0 && matching_columns >= curr_column)
		return (strtok_r(driver, "\t ", &last));
	/* No more drivers found. */
	return (NULL);
}

static bool
match_device_column(const devinfo_t *dev, char *colstr)
{
	char *p;

	if (*colstr != '*' && strtol(colstr, NULL, 16) != dev->device)
		return (false);
	while ((p = strsep(&colstr, "\t ")) != NULL) {
		if (strncmp(p, "revision=", 9) == 0 &&
		    strtol(&p[9], NULL, 16) != dev->revision)
			return (false);
		else if (strncmp(p, "class=", 6) == 0 &&
		    strtol(&p[6], NULL, 16) != dev->class)
			return (false);
		else if (strncmp(p, "subclass=", 9) == 0 &&
		    strtol(&p[6], NULL, 16) != dev->subclass)
			return (false);
		else if (strncmp(p, "ifclass=", 8) == 0 &&
		    !match_ifclass(dev, strtol(&p[8], NULL, 16)))
			return (false);
		else if (strncmp(p, "ifsubclass=", 11) == 0 &&
		    !match_ifsubclass(dev, strtol(&p[11], NULL, 16)))
			return (false);
		else if (strncmp(p, "protocol=", 9) == 0 &&
		    !match_ifprotocol(dev, strtol(&p[9], NULL, 16)))
			return (false);
	}
	return (true);
}

static bool
match_drivers_db_column(const devinfo_t *dev, char *colstr, int colnumber)
{
	if (colnumber == DB_VENDOR_COLUMN) {
		if (*colstr == '*' || strtol(colstr, NULL, 16) == dev->vendor)
			return (true);
	} else if (colnumber == DB_DEVICE_COLUMN) {
		if (match_device_column(dev, colstr))
			return (true);
	} else if (colnumber == DB_SUBVENDOR_COLUMN) {
		if (*colstr == '*' ||
		    strtol(colstr, NULL, 16) == dev->subvendor)
			return (true);
	} else if (colnumber == DB_SUBDEVICE_COLUMN) {
		if (*colstr == '*' ||
		    strtol(colstr, NULL, 16) == dev->subdevice)
			return (true);
	}
	return (false);
}

static bool
match_kmod_name(const char *kmodfile, const char *name)
{
	size_t	   len;
	const char *p, *q;

	/* Ignore the bus part */
	if ((p = strchr(kmodfile, '/')) == NULL)
		p = kmodfile;
	else
		p++;
	/* Ignore .ko suffix */
	for (q = p; *q != '\0'; q++) {
		if (*q == '.' && strcmp(q, ".ko") == 0)
			break;
	}
	len = q - p;
	if (len == strlen(name) && strncmp(name, p, len) == 0)
		return (true);
	/*
	 * Network drivers compiled into the kernel do not
	 * have the if_* prefix.
	 */
	if (strncmp(name, "if_", 3) == 0) {
		if (len == strlen(name) - 3 && strncmp(name + 3, p, len) == 0)
			return (true);
	}
	return (false);
}

static bool
is_kmod_loaded(const char *name)
{
	int		   id, _id;
	struct module_stat mstat;

	if (kldfind(name) == -1) {
		if (errno != ENOENT)
			logprint("kldfind(%s)", name);
	} else
		return (true);
	for (id = kldnext(0); id > 0; id = kldnext(id)) {
		for (_id = kldfirstmod(id); _id > 0; _id = modfnext(_id)) {
			mstat.version = sizeof(struct module_stat);
			if (modstat(_id, &mstat) == -1)
				continue;
			if (match_kmod_name(mstat.name, name))
				return (true);
		}
	}
	return (false);
}

static bool
is_excluded(const char *kmod)
{
	int i;

	for (i = 0; exclude[i] != NULL; i++) {
		if (strcmp(exclude[i], kmod) == 0)
			return (true);
	}
	return (false);
}

static void
load_driver(devinfo_t *dev)
{
	char		*driver;
	const devinfo_t *dp;

	for (dp = dev; (driver = find_driver(dp)) != NULL; dp = NULL) {
		add_driver(dev, driver);
		if (is_excluded(driver)) {
			logprintx("vendor=%04x product=%04x %s: " \
				  "%s excluded from loading",
				  dev->vendor, dev->device,
				  dev->descr != NULL ? dev->descr : "",
				  driver);
			continue;
		}
		if (cfgstate != NULL) {
			if (cfg_call_function(cfgstate, "affirm",
			    dev, driver) == 0) {
				logprintx("affirm returned 0");
				continue;
			}
			logprintx("affirm returned 1");
		}
		if (!is_kmod_loaded(driver)) {
			logprintx("vendor=%04x product=%04x %s: " \
			    "Loading %s", dev->vendor, dev->device,
			    dev->descr != NULL ? dev->descr : "", driver);
			if (!dryrun && kldload(driver) == -1)
				logprint("kldload(%s)", driver);
			if (cfgstate != NULL) {
				cfg_call_function(cfgstate, "on_load_kmod",
				    dev, driver);
			}
		} else {
			logprintx("vendor=%04x product=%04x %s: %s already " \
			    "loaded", dev->vendor, dev->device,
			    dev->descr != NULL ? dev->descr : "", driver);
		}
	}
	if (dp != NULL) {
		logprintx("vendor=%04x product=%04x %s: No driver found",
		    dev->vendor, dev->device,
		    dev->descr != NULL ? dev->descr : "");
	}
	/* We are done with this device */
	if (cfgstate != NULL)
		(void)cfg_call_function(cfgstate, "on_finished", dev, NULL);
}

static inline char *
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
get_devdescr(FILE *iddb, const devinfo_t *dev)
{
	int	    column, matching_columns;
	char	    *p, *descr;
	static char infostr[_POSIX2_LINE_MAX], line[_POSIX2_LINE_MAX];

	if (iddb == NULL)
		return (NULL);
	column = matching_columns = 0; infostr[0] = '\0';

	if (fseek(iddb, 0, SEEK_SET) == -1) {
		logprint("fseek()");
		return (NULL);
	}
	while (fgets(line, sizeof(line) - 1, iddb) != NULL) {
		if ((p = get_next_word_start(line)) == NULL ||
		    *p == '#' || *p == '\n')
			continue;
		for (column = 0; line[column] == '\t'; column++)
			;
		if (column > matching_columns)
			continue;
		if (column < matching_columns)
			return (infostr);
		if (match_devdescr_column(dev, line, column)) {
			if ((descr = get_next_word_start(NULL)) == NULL)
				continue;
			if (column > 0)
				(void)strlcat(infostr, " ", sizeof(infostr));
			(void)strlcat(infostr, descr, sizeof(infostr));
			matching_columns++;
		}
	}
	if (column < matching_columns)
		return (infostr);
	return (NULL);
}
