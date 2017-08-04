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
#include <string.h>
#include <time.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <err.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/param.h>
#include <sys/linker.h>
#include <sys/pciio.h>
#include <unistd.h>
#include <paths.h>
#include <libusb20_desc.h>
#include <libusb20.h>

#define MAX_PCI_DEVS	 32
#define MAX_EXCLUDES	 256

#define SOCK_ERR_CONN_CLOSED    1
#define SOCK_ERR_IO_ERROR       2

#define PATH_PCI	 "/dev/pci"
#define PATH_DEVD_SOCKET "/var/run/devd.pipe"

#define IFCONFIG_CMD	 "ifconfig_%s=\"DHCP up\" /etc/rc.d/dhclient " \
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
	u_short class;
	u_short subclass;
	u_short protocol;
} iface_t;

/*
 * Struct to represent a device.
 */
typedef struct devinfo_s {
	u_short vendor;			/* Vendor ID */
	u_short subvendor;		/* Subvendor ID */
	u_short device;			/* Device/product ID */
	u_short subdevice;		/* Subdevice ID */
	u_short class;			/* USB/PCI device class */
	u_short subclass;		/* USB/PCI device subclass */
	u_short revision;		/* Device revision. */
	u_short nifaces;		/* # of USB interfaces. */
	iface_t *iface;			/* USB interfaces. */
} devinfo_t;

static int	 ndevs = 0;		/* # of devices. */
static bool	 dryrun;		/* Do not load any drivers if true. */
static FILE	 *db;			/* File pointer for drivers database. */
static FILE	 *logfp = NULL;		/* File pointer for logprint(). */
static char	 *exclude[MAX_EXCLUDES];/* List of drivers to exclude. */
static devinfo_t *devlst = NULL;	/* List of devices. */

static int	 uconnect(const char *);
static int	 devd_connect(void);
static int	 get_usb_devs(void);
static int	 get_pci_devs(void);
static bool	 is_new(u_short, u_short, u_short, u_short);
static bool	 match_ifsubclass(const devinfo_t *, u_short);
static bool	 match_ifclass(const devinfo_t *, u_short);
static bool	 match_protocol(const devinfo_t *, u_short);
static bool	 parse_devd_event(char *);
static void	 deamonize(void);
static void	 netstart(const char *);
static void	 add_iface(devinfo_t *, u_short, u_short, u_short);
static void	 load_driver(const devinfo_t *);
static void	 logprint(const char *, ...);
static void	 logprintx(const char *, ...);
static void	 usage(void);
static char	 *read_devd_event(int, int *);
static char	 *find_driver(const devinfo_t *);
static devinfo_t *add_device(void);

int
main(int argc, char *argv[])
{
	int    ch, error, i, n, s, tries;
	FILE   *fp;
	bool   fflag, uflag, newdev;
	char   *ln, *p;
	fd_set rset;
	struct timeval tv, *tp;

	fflag = dryrun = uflag = false;
	while ((ch = getopt(argc, argv, "fnhux:")) != -1) {
		switch (ch) {
		case 'f':
			fflag = true;
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

	/* Check if deamon is already running. */
	if ((fp = fopen(PATH_PID_FILE, "r+")) == NULL) {
		if (errno != ENOENT)
			err(EXIT_FAILURE, "fopen(%s)", PATH_PID_FILE);
		/* Not running - Create the PID/lock file. */
		if ((fp = fopen(PATH_PID_FILE, "w")) == NULL) {
			err(EXIT_FAILURE, "couldn't create pid file %s",
			    PATH_PID_FILE);
		}
	}
	if (flock(fileno(fp), LOCK_EX | LOCK_NB) == -1) {
		if (errno == EWOULDBLOCK) {
			/* Daemon already running. */
			errx(EXIT_FAILURE, "%s is already running.", PROGRAM);
		} else
			err(EXIT_FAILURE, "flock()");
	}
	if (!fflag) {
		/* Close all files except for PID file and stderr. */
		for (i = 0; i < 16; i++) {
			if (fileno(fp) != i && fileno(stderr) != i)
				(void)close(i);
		}
		/* Redirect error messages to logfile. */
		if ((logfp = fopen(PATH_LOG, "a+")) == NULL)
			err(EXIT_FAILURE, "fopen()");
		(void)setvbuf(logfp, NULL, _IOLBF, 0);
		(void)fclose(stderr);
		/* For warn(), err(), etc. */
		err_set_file(logfp);
	}
	if ((s = devd_connect()) == -1)
		err(EXIT_FAILURE, "Couldn't connect to %s", PATH_DEVD_SOCKET);
	if ((db = fopen(PATH_DRIVERS_DB, "r")) == NULL)
		err(EXIT_FAILURE, "fopen(%s)", PATH_DRIVERS_DB);
	logprintx("%s started", PROGRAM);

	(void)get_pci_devs();
	(void)get_usb_devs();

	for (i = 0; i < ndevs; i++)
		load_driver(&devlst[i]);

	/* Write our PID to the PID/lock file. */
	(void)fprintf(fp, "%d", getpid());
	(void)fflush(fp);

	tv.tv_sec = 1; tv.tv_usec = 0; tp = &tv;
	for (tries = 0, newdev = false; ; newdev = false) {
		FD_ZERO(&rset);
		FD_SET(s, &rset);
		if (select(s + 1, &rset, NULL, NULL, tp) == -1) {
			if (errno == EINTR)
				continue;
			else
				err(EXIT_FAILURE, "select()");
		} else {
			/* 
			 * In case we loaded the driver of an ethernet device,
			 * we wait two seconds to see if it appears. If that's
			 * the case, we try to bring up the ethernet device
			 * before we become a deamon. This delays the start of
			 * services that depend on a network connection, until
			 * we started dhclient on the device.
			 */
			if (tries > 0) {
				/* Use blocking select() from now. */
				tp = NULL;
				if (!fflag)
					deamonize();
			} else {
				tries++;
				/* In case we want more than one try. */
				tv.tv_sec = 1; tv.tv_usec = 0;
			}
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
					newdev = true;
				}
			}
			if (newdev) {
				n = get_usb_devs();
				for (i = ndevs - n; i < ndevs; i++)
					load_driver(&devlst[i]);
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
	}
	/* NOTREACHED */
	return (EXIT_SUCCESS);
}

static void
usage()
{
	printf("Usage: %s [-h]\n" \
	       "       %s [-fnu][-x driver,...]\n", PROGRAM, PROGRAM);
	exit(EXIT_FAILURE);
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

static void
deamonize()
{
	int i;

	for (i = 0; i < 2; i++) {
		switch (fork()) {
		case -1:
			err(EXIT_FAILURE, "fork()");
		case  0:
			break;
		default:
			exit(EXIT_SUCCESS);
		}
		if (i == 0) {
			(void)setsid();
			(void)signal(SIGHUP, SIG_IGN);
		}
	}
}

static int
uconnect(const char *path)
{
	int s;
	struct sockaddr_un saddr;

	if ((s = socket(PF_LOCAL, SOCK_STREAM, 0)) == -1)
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
	int  i, n;
	static char *lnbuf = NULL;
	static int rd = 0, bufsz = 0, slen = 0;

	if (lnbuf == NULL) {
		if ((lnbuf = malloc(_POSIX2_LINE_MAX)) == NULL)
			return (NULL);
		bufsz = _POSIX2_LINE_MAX;
	}

	*error = n = 0;
	do {
		rd += n;
		if (slen > 0)
			(void)memmove(lnbuf, lnbuf + slen, rd - slen);
		rd  -= slen;
		slen = 0;
		for (i = 0; i < rd && lnbuf[i] != '\n'; i++)
			;
		if (i < rd) {
			lnbuf[i] = '\0'; slen = i + 1;
			if (slen == bufsz)
				slen = rd = 0;
			return (lnbuf);
		}
		if (rd == bufsz - 1) {
			lnbuf = realloc(lnbuf, bufsz + _POSIX2_LINE_MAX);
			if (lnbuf == NULL)
				err(EXIT_FAILURE, "realloc()");
			bufsz += 64;
		}
	} while ((n = read(s, lnbuf + rd, bufsz - rd - 1)) > 0);

	if (n < 0) {
		if (errno == EAGAIN || errno == EINTR) {
			/* Just retry */
			return (NULL);
		}
	}
	if (errno == 0 || errno == ECONNRESET)
		*error = SOCK_ERR_CONN_CLOSED;
	else
		*error = SOCK_ERR_IO_ERROR;
	/* Error or lost connection */
	slen = rd = 0;

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
add_iface(devinfo_t *d, u_short class, u_short subclass, u_short protocol)
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
match_ifclass(const devinfo_t *d, u_short class)
{
	int i;

	for (i = 0; i < d->nifaces; i++) {
		if (d->iface[i].class == class)
			return (true);
	}
	return (false);
}

static bool
match_ifsubclass(const devinfo_t *d, u_short subclass)
{
	int i;

	for (i = 0; i < d->nifaces; i++) {
		if (d->iface[i].subclass == subclass)
			return (true);
	}
	return (false);
}

static bool
match_protocol(const devinfo_t *d, u_short protocol)
{
	int i;

	for (i = 0; i < d->nifaces; i++) {
		if (d->iface[i].protocol == protocol)
			return (true);
	}
	return (false);
}

static bool
is_new(u_short vendor, u_short device, u_short class, u_short subclass)
{
	int i;

	for (i = 0; i < ndevs; i++) {
		if (devlst[i].vendor == vendor && devlst[i].device == device &&
		    devlst[i].class  == class && devlst[i].subclass == subclass)
			return (false);
	}
	return (true);
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
			dip->vendor    = conf[i].pc_vendor;
			dip->device    = conf[i].pc_device;
			dip->subvendor = conf[i].pc_subvendor;
			dip->subdevice = conf[i].pc_subdevice;
			dip->revision  = conf[i].pc_revid;
			dip->class     = conf[i].pc_class;
			dip->subclass  = conf[i].pc_subclass;
			n++;
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
	bool	    nomatch;
	char	    ln[_POSIX2_LINE_MAX], *p;
	static char driver[64];
	static const devinfo_t *curdev = NULL;

	if (d == NULL) {
		/* Try to find more drivers for previously defined dev. */
		d = curdev;
	} else {
		if (fseek(db, 0, SEEK_SET) == -1)
			err(EXIT_FAILURE, "fseek()");
		curdev = d;
	}
	match = depth_prev = 0;
	while (fgets(ln, sizeof(ln), db) != NULL) {
		(void)strtok(ln, "#\r\n");
		if (ln[0] == '\0')
			continue;
		for (depth_cur = 0; ln[depth_cur] == '\t'; depth_cur++)
			;
		if (depth_cur - match >= 2)
			continue;
		if (depth_cur < depth_prev) {
			if (match < depth_prev)
				match = 0;
			else
				return (driver);
		}
		depth_prev = depth_cur;

		if ((p = strtok(ln, "\t ")) == NULL)
			continue;
		switch (depth_cur) {
		case 0:
			depth_prev = match = 0;
			(void)strncpy(driver, p, sizeof(driver));
			break;
		case 1:
			if (*p == '*' || strtol(p, NULL, 16) == d->vendor)
				match++;
			break;
		case 2:
			if (*p != '*' && strtol(p, NULL, 16) != d->device)
				continue;
			nomatch = false;
			while ((p = strtok(NULL, "\t ")) != NULL) {
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
			if (*p == '*' || strtol(p, NULL, 16) == d->subvendor)
				match++;
			break;
		case 4:
			if (*p == '*' || strtol(p, NULL, 16) == d->subdevice)
				match++;
			break;
		}
	}
	if (match > 0 && match >= depth_cur)
		return (driver);

	/* No more drivers found. */
	return (NULL);
}

static void
load_driver(const devinfo_t *dev)
{
	int		i;
	char		*driver;
	const devinfo_t *dp;

	for (dp = dev; (driver = find_driver(dp)) != NULL; dp = NULL) {
		for (i = 0; exclude[i] != NULL; i++) {
			if (strcmp(exclude[i], driver) == 0) {
				logprintx("vendor=%04x product=%04x: " \
				    "%s excluded from loading", dev->vendor,
				    dev->device, driver);
				break;
			}
		}
		if (exclude[i] != NULL)
			continue;
		if (kldfind(driver) == -1) {
			if (errno == ENOENT) {
				logprintx("vendor=%04x product=%04x: " \
				    "Loading %s", dev->vendor, dev->device,
				    driver);
				if (!dryrun && kldload(driver) == -1)
					logprint("kldload(%s)", driver);
			} else
				logprint("kldfind(%s)", driver);
		} else {
			logprintx("vendor=%04x product=%04x: %s already " \
			    "loaded", dev->vendor, dev->device, driver);
		}
	}
	if (dp != NULL) {
		logprintx("vendor=%04x product=%04x: No driver found",
		    dev->vendor, dev->device);
	}
}

