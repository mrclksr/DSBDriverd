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

#define MAX_PCI_DEVS	     32
#define MAX_EXCLUDES	     256

#define SOCK_ERR_IO_ERROR    2
#define SOCK_ERR_CONN_CLOSED 1

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
static int	 check(uint16_t, uint16_t);
static int	 cfg_getint(lua_State *, const char *);
static int	 cfg_call_function(lua_State *L, const char *,
		     const devinfo_t *, const char *);
static bool	 is_new(uint16_t, uint16_t, uint16_t, uint16_t);
static bool	 match_ifsubclass(const devinfo_t *, uint16_t);
static bool	 match_ifclass(const devinfo_t *, uint16_t);
static bool	 match_protocol(const devinfo_t *, uint16_t);
static bool	 parse_devd_event(char *);
static bool	 find_kmod(const char *);
static void	 lockpidfile(void);
static void	 netstart(const char *);
static void	 add_driver(devinfo_t *, const char *);
static void	 add_iface(devinfo_t *, uint16_t, uint16_t, uint16_t);
static void	 load_driver(devinfo_t *);
static void	 logprint(const char *, ...);
static void	 logprintx(const char *, ...);
static void	 open_dbs(void);
static void	 open_cfg(void);
static void	 usage(void);
static void	 print_pci_devinfo(const devinfo_t *);
static void	 print_usb_devinfo(const devinfo_t *);
static void	 cfg_setstr(lua_State *, const char *, const char *);
static void	 cfg_setint(lua_State *, const char *, int);
static void	 cfg_setint_tbl_field(lua_State *, const char *, int);
static void	 cfg_setstr_tbl_field(lua_State *, const char *, const char *);
static void	 cfg_add_interface_tbl(lua_State *, const iface_t *);
static void	 cfg_dev_to_tbl(lua_State *, const devinfo_t *dev);
static char	 *int2char(uint16_t);
static char	 *read_devd_event(int, int *);
static char	 *find_driver(const devinfo_t *);
static char	 *devdescr(FILE *, const devinfo_t *);
static char	 *cfg_getstr(lua_State *, const char *);
static char	 **cfg_getstrarr(lua_State *, const char *, size_t *);
static devinfo_t *add_device(void);

int
main(int argc, char *argv[])
{
	int		ch, error, i, n, s, tries;
	char		*ln, *p;
	bool		cflag, fflag, lflag, uflag;
	fd_set		rset;
	uint16_t	vendor, device;
	struct timeval	tv, *tp;

	cflag = fflag = dryrun = lflag = uflag = false;
	while ((ch = getopt(argc, argv, "c:flnhux:")) != -1) {
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
		case 'u':
			uflag = true;
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
	if (cflag)
		return (check(vendor, device));
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

	if (!fflag) {
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
	}
	if ((s = devd_connect()) == -1)
		err(EXIT_FAILURE, "Couldn't connect to %s", PATH_DEVD_SOCKET);
	open_cfg();
	open_dbs();

	(void)get_pci_devs();
	(void)get_usb_devs();

	for (i = 0; i < ndevs; i++)
		load_driver(&devlst[i]);
	for (tries = 0;;) {
		/* 
		 * In case we loaded the driver of an ethernet device,
		 * we wait two seconds to see if it appears. If that's
		 * the case, we try to bring up the ethernet device
		 * before we become a daemon. This delays the start of
		 * services that depend on a network connection, until
		 * we started dhclient on the device.
		 */
		if (tries == 2) {
			/* Use blocking select() from now. */
			tp = NULL;
			if (!fflag) {
				if (daemon(0, 1) == -1)
					err(EXIT_FAILURE, "Failed to daemonize");
				(void)pidfile_write(pfh);
			}
			tries++;
		} else if (tries < 2) {
			tries++;
			tv.tv_sec = 1; tv.tv_usec = 0; tp = &tv;
		}
		FD_ZERO(&rset); FD_SET(s, &rset);
		while (select(s + 1, &rset, NULL, NULL, tp) == -1) {
			if (errno == EINTR)
				continue;
			else
				err(EXIT_FAILURE, "select()");
			/* NOTREACHED */
		}
		if (!FD_ISSET(s, &rset))
			continue;
		while ((ln = read_devd_event(s, &error)) != NULL) {
			if (!parse_devd_event(ln))
				continue;
			if (devdevent.type != DEVD_TYPE_ATTACH)
				continue;
			switch (devdevent.system) {
			case DEVD_SYSTEM_IFNET:
				if (uflag && !dryrun)
					netstart(devdevent.subsystem);
				break;
			case DEVD_SYSTEM_USB:
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

static void
usage()
{
	(void)printf("Usage: %s [-h]\n" \
	       "       %s [-l|-c vendor:device] | [-fnu][-x driver,...]\n",
	       PROGRAM, PROGRAM);
	exit(EXIT_FAILURE);
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

static int
check(uint16_t vendor, uint16_t device)
{
	int		n;
	char		*info;
	devinfo_t	dev;
	const char	*p;
	const devinfo_t	*dp;

	(void)memset(&dev, 0, sizeof(dev));
	dev.vendor = vendor;
	dev.device = device;

	if ((info = devdescr(pcidb, &dev)) == NULL)
		info = devdescr(usbdb, &dev);
	for (n = 0, dp = &dev; (p = find_driver(dp)) != NULL; dp = NULL, n++)
		(void)printf("%s: %s\n", info != NULL ? info: "", p);
	if (n > 0)
		return (0);
	return (1);
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

static bool
parse_devd_event(char *str)
{
	char *p, *q;

	devdevent.cdev = devdevent.subsystem = "";
	if (str[0] != '!')
		return (false);
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
	return (true);
}

static void
netstart(const char *ifdev)
{
	char cmd[512];

	/* Bring up new ethernet devices, and start dhclient */
	logprintx("Bringing up %s", ifdev);
	(void)snprintf(cmd, sizeof(cmd) - 1, IFCONFIG_CMD, ifdev, ifdev);
	switch (system(cmd)) {
	case   0:
		break;
	case  -1:
	case 127:
		logprint("system(%s)", cmd);
	}
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
match_protocol(const devinfo_t *d, uint16_t protocol)
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
cfg_setstr(lua_State *L, const char *var, const char *str)
{
	lua_pushstring(L, str);
	lua_setglobal(L, var);
}

static void
cfg_setint(lua_State *L, const char *var, int val)
{
	lua_pushnumber(L, val);
	lua_setglobal(L, var);
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
	cfg.exclude = cfg_getstrarr(cfgstate, "exclude", &cfg.exclude_len);
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
			dip->descr     = devdescr(pcidb, dip);
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
		dip->descr = devdescr(usbdb, dip);
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
find_driver(const devinfo_t *d)
{
	int	    match, depth_prev, depth_cur;
	bool	    nomatch, skip;
	long	    len;
	char	    ln[_POSIX2_LINE_MAX], *p, *lp;
	static char *last = NULL;
	static char driver[_POSIX2_LINE_MAX];
	static const devinfo_t *curdev = NULL;

	skip = false;
	if (d == NULL) {
		/* Try to find more drivers for previously defined dev. */
		d = curdev;
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
		curdev = d;
	}
	match = depth_prev = 0;
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
		for (depth_cur = 0, lp = ln; *lp != '\0'; lp++) {
			if (*lp == '\t')
				depth_cur++;
			else if (*lp != ' ')
				break;
		}
		/* Skip whitespace-only lines */
		if (*lp == '\0')
			continue;
		if (skip && depth_cur > 0)
			continue;
		else if (skip) {
			/*
			 * We skipped all lines so far, and reached the
			 * the next driver record. Reset "match" and depth_prev
			 */
			skip = false;
			depth_prev = match = 0;
		}
		if (depth_cur < depth_prev) {
			if (depth_cur == 0) {
				/*
				 * We are at the beginning of a new driver
				 * record. Get the driver name in the next
				 * iteration.
				 */
				(void)fseek(db, -len, SEEK_CUR);
			}
			if (depth_prev == match)
				return (strtok_r(driver, "\t ", &last));
			if (depth_cur <= match) {
				skip = true;
				continue;
			}
		} else if (depth_cur == 0) {
			/*
			 * At the beginning of a new record. Get the driver
			 * name.
			 */
			match = depth_prev = 0; last = NULL;
			(void)strncpy(driver, ln, sizeof(driver));
			continue;
		} else if (depth_cur - match >= 2)
			continue;
		/* Skip leading tabs and spaces */
		lp = ln + strspn(ln, "\t ");
		if (*lp == '\0')
			continue;

		depth_prev = depth_cur;
		switch (depth_cur) {
		case 1:
			if (*lp == '*' || strtol(lp, NULL, 16) == d->vendor)
				match++;
			break;
		case 2:
			if (*lp != '*' && strtol(lp, NULL, 16) != d->device)
				continue;
			nomatch = false;
			while ((p = strsep(&lp, "\t ")) != NULL) {
				if (strncmp(p, "revision=", 9) == 0 &&
				    strtol(&p[9], NULL, 16) != d->revision)
					nomatch = true;
				else if (strncmp(p, "class=", 6) == 0 &&
				    strtol(&p[6], NULL, 16) != d->class)
					nomatch = true;
				else if (strncmp(p, "subclass=", 9) == 0 &&
				    strtol(&p[6], NULL, 16) != d->subclass)
					nomatch = true;
				else if (strncmp(p, "ifclass=", 8) == 0 &&
				    !match_ifclass(d, strtol(&p[8], NULL, 16)))
					nomatch = true;
				else if (strncmp(p, "ifsubclass=", 11) == 0 &&
				    !match_ifsubclass(d,
				      strtol(&p[11], NULL, 16)))
					nomatch = true;
				else if (strncmp(p, "protocol=", 9) == 0 &&
				    !match_protocol(d,
				      strtol(&p[9], NULL, 16)))
					nomatch = true;
			}
			if (!nomatch)
				match++; 
			break;
		case 3:
			if (*lp == '*' || strtol(lp, NULL, 16) == d->subvendor)
				match++;
			break;
		case 4:
			if (*lp == '*' || strtol(lp, NULL, 16) == d->subdevice)
				match++;
			break;
		}
	}
	if (match > 0 && match >= depth_cur)
		return (strtok_r(driver, "\t ", &last));
	/* No more drivers found. */
	return (NULL);
}

static bool
find_kmod(const char *name)
{
	int		   id, _id;
	size_t		   len;
	const char	   *q, *p;
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
			/* Ignore the bus part */
			if ((p = strchr(mstat.name, '/')) == NULL)
				p = mstat.name;
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
				if (len == strlen(name) - 3 &&
				    strncmp(name + 3, p, len) == 0)
					return (true);
			}
		}
	}
	return (false);
}

static char *
int2char(uint16_t val)
{
	static char num[5];

	(void)snprintf(num, sizeof(num), "%x", val);
	return (num);
}

static void
load_driver(devinfo_t *dev)
{
	int		i;
	char		*driver;
	const devinfo_t *dp;

	for (dp = dev; (driver = find_driver(dp)) != NULL; dp = NULL) {
		add_driver(dev, driver);
		for (i = 0; exclude[i] != NULL; i++) {
			if (strcmp(exclude[i], driver) == 0) {
				logprintx("vendor=%04x product=%04x %s: " \
				    "%s excluded from loading",
				    dev->vendor, dev->device,
				    dev->descr != NULL ? dev->descr : "",
				    driver);
				break;
			}
		}
		if (exclude[i] != NULL)
			continue;
		if (cfgstate != NULL) {
			if (cfg_call_function(cfgstate, "affirm",
			    dev, driver) == 0) {
				logprintx("affirm returned 0");
				continue;
			}
			logprintx("affirm returned 1");
		}
		if (!find_kmod(driver)) {
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
}

static inline char *
nf(char *str)
{
	static char *next = NULL;

	if (str == NULL) {
		/* Get start of next field. */
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

static char *
devdescr(FILE *iddb, const devinfo_t *dev)
{
	int	    depth, match, _match, val, sv, sd;
	char	   *p, *q;
	static char info[_POSIX2_LINE_MAX], ln[_POSIX2_LINE_MAX];

	if (iddb == NULL)
		return (NULL);
	depth = match = 0; info[0] = '\0';

	if (fseek(iddb, 0, SEEK_SET) == -1) {
		logprint("fseek()");
		return (NULL);
	}
	while (fgets(ln, sizeof(ln) - 1, iddb) != NULL) {
		if ((p = nf(ln)) == NULL || *p == '#' || *p == '\n')
			continue;
		for (depth = 0; ln[depth] == '\t'; depth++)
			;
		if (depth > match)
			continue;
		_match = match;
		if (depth < match)
			return (info);
		(void)strtok(ln, "#\n");
		if ((p = nf(ln)) == NULL || (q = nf(NULL)) == NULL)
			continue;
		if (depth < 2) {
			val = strtol(p, NULL, 16);
			if (depth == 0) {
				if (val == dev->vendor)
					match++;
			} else if (val == dev->device)
				match++;
		} else if (depth == 2) {
			sv = strtol(p, NULL, 16);
			sd = strtol(q, NULL, 16);
			if ((q = nf(NULL)) == NULL)
				continue;
			if (sv == dev->subvendor && sd == dev->subdevice)
				match++;
		}
		if (match == _match || match < depth)
			continue;
		if (depth > 0)
			(void)strlcat(info, " ", sizeof(info));
		(void)strlcat(info, q, sizeof(info));
	}
	if (depth < match)
		return (info);
	return (NULL);
}
