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
#include <ctype.h>
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
#include <unistd.h>

#include "log.h"
#include "device.h"
#include "config.h"
#ifdef TEST
# include <atf-c.h>
#endif

#define MAX_EXCLUDES	 256
#define PATH_DEVD_SOCKET "/var/run/devd.seqpacket.pipe"

enum SOCK_ERR {
	SOCK_ERR_CONN_CLOSED = 1,
	SOCK_ERR_IO_ERROR
};

enum DB_COLUMNS {
	DB_VENDOR_COLUMN = 1,
	DB_DEVICE_COLUMN,
	DB_SUBVENDOR_COLUMN,
	DB_SUBDEVICE_COLUMN
};

struct devd_event_s {
	int  system;
#define DEVD_SYSTEM_IFNET 1
#define DEVD_SYSTEM_USB	  2
	int  type;
#define DEVD_TYPE_ATTACH  1
	char *cdev;
	char *subsystem;
} devdevent;

static bool	 dryrun;		/* Do not load any drivers if true. */
static FILE	 *driversdb;		/* File pointer for drivers database. */
static char	 *exclude[MAX_EXCLUDES];/* List of drivers to exclude. */
static config_t  *cfg;
static devinfo_t **devlist;		/* List of devices. */
static struct pidfh *pfh;		/* PID file handle. */

static int  uconnect(const char *);
static int  devd_connect(void);
static int  parse_devd_event(char *);
static bool has_driver(uint16_t, uint16_t);
static bool is_excluded(const char *);
static bool is_kmod_loaded(const char *);
static bool match_drivers_db_column(const devinfo_t *, char *, int);
static bool match_device_column(const devinfo_t *, char *);
static bool match_kmod_name(const char *, const char *);
static void create_exclude_list(char *);
static void devd_reconnect(int *);
static void process_devs(devinfo_t **);
static void call_on_add_device(devinfo_t *);
static void show_drivers(uint16_t, uint16_t);
static void lockpidfile(void);
static void print_pci_devinfo(const devinfo_t *);
static void print_usb_devinfo(const devinfo_t *);
static void load_driver(devinfo_t *);
static void open_drivers_db(void);
static void daemonize(void);
static void initcfg(void);
static void usage(void);
static char *read_devd_event(int, int *);
static char *find_driver(const devinfo_t *);

#ifndef TEST
int
main(int argc, char *argv[])
{
	int	 ch, error, i, devd_sock;
	char	 *ln, *p;
	bool	 cflag, fflag, lflag;
	fd_set	 rset;
	uint16_t vendor, device;
	devinfo_t **new_devs, **dev;

	exclude[0] = NULL;

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
			create_exclude_list(optarg);
			break;
		case 'h':
		case '?':
			usage();
		}
	}
	if (!cflag && !lflag)
		lockpidfile();
	if (!cflag && !lflag && !fflag)
		daemonize();
	open_drivers_db();

	if (cflag) {
		if (has_driver(vendor, device)) {
			show_drivers(vendor, device);
			return (EXIT_SUCCESS);
		}
		return (EXIT_FAILURE);
	}
	devlist = init_devlist();

	if (lflag) {
		for (dev = devlist; dev != NULL && *dev != NULL; dev++) {
			if ((*dev)->bus == BUS_TYPE_PCI)
				print_pci_devinfo(*dev);
			else if ((*dev)->bus == BUS_TYPE_USB)
				print_usb_devinfo(*dev);
		}
		return (EXIT_SUCCESS);
	}
	if ((devd_sock = devd_connect()) == -1)
		die("Couldn't connect to %s", PATH_DEVD_SOCKET);
	initcfg();

	process_devs(devlist);

	for (;;) {
		FD_ZERO(&rset); FD_SET(devd_sock, &rset);
		while (select(devd_sock + 1, &rset, NULL, NULL, NULL) == -1) {
			if (errno == EINTR)
				continue;
			die("select()");
		}
		if (!FD_ISSET(devd_sock, &rset))
			continue;
		while ((ln = read_devd_event(devd_sock, &error)) != NULL) {
			if (parse_devd_event(ln) == -1)
				continue;
			if (devdevent.type != DEVD_TYPE_ATTACH)
				continue;
			if (devdevent.system == DEVD_SYSTEM_USB) {
				new_devs = get_usb_devs(&devlist);
				process_devs(new_devs);
			}
		}
		if (error == SOCK_ERR_CONN_CLOSED)
			devd_reconnect(&devd_sock);
		else if (error == SOCK_ERR_IO_ERROR)
			die("read_devd_event()");
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
	       "       %s [-l | -c vendor:device] | [-fn][-x driver,...]\n",
	       PROGRAM, PROGRAM);
	exit(EXIT_FAILURE);
}

static void
process_devs(devinfo_t **devs)
{
	while (devs != NULL && *devs != NULL) {
		call_on_add_device(*devs);
		load_driver(*devs++);
	}
}

static void
create_exclude_list(char *list)
{
	int   i;
	char *p;

	for (i = 0, p = list; i < MAX_EXCLUDES - 1 &&
	    (p = strtok(p, ", ")) != NULL; i++, p = NULL) {
		exclude[i] = p;
	}
	if (i >= MAX_EXCLUDES - 1) {
		errx(EXIT_FAILURE, "Number of elements in exclude list " \
		    "exceeds %d", MAX_EXCLUDES - 1);
	}
	exclude[i] = NULL;
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
	if (openlog() == -1)
		die("openlog()");
	logprintx("%s started", PROGRAM);
	if (daemon(0, 1) == -1)
		die("Failed to daemonize");
	(void)fclose(stderr);
	(void)pidfile_write(pfh);
}

static void
show_drivers(uint16_t vendor, uint16_t device)
{
	char		*info;
	devinfo_t	dev;
	const char	*p;
	const devinfo_t	*dp;

	(void)memset(&dev, 0, sizeof(dev));
	dev.bus    = BUS_TYPE_PCI;
	dev.vendor = vendor;
	dev.device = device;
	
	if ((info = get_devdescr(&dev)) == NULL) {
		dev.bus = BUS_TYPE_USB;
		info = get_devdescr(&dev);
	}
	for (dp = &dev; (p = find_driver(dp)) != NULL; dp = NULL)
		(void)printf("%s: %s\n", info != NULL ? info: "", p);
}

static bool
has_driver(uint16_t vendor, uint16_t device)
{
	devinfo_t dev;

	(void)memset(&dev, 0, sizeof(dev));
	dev.vendor = vendor;
	dev.device = device;

	return (find_driver(&dev) != NULL ? true : false);
}

static void
call_on_add_device(devinfo_t *dev)
{
	if (cfg != NULL)
		call_cfg_function(cfg, "on_add_device", dev, NULL);
}

static void
lockpidfile()
{
	/* Check if deamon is already running. */
	if ((pfh = pidfile_open(PATH_PID_FILE, 0600, NULL)) == NULL) {
		if (errno == EEXIST)
			diex("%s is already running.", PROGRAM);
		die("Failed to create PID file.");
	}
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

static void
devd_reconnect(int *sock)
{
	(void)close(*sock);
	logprintx("Lost connection to devd. Reconnecting ...");
	if ((*sock = devd_connect()) == -1)
		diex("Connecting to devd failed. Giving up.");
	logprintx("Connection to devd established");
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
			die("malloc()");
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
			die("recvmsg()");
		} else if (n == 0 && rd == 0) {
			*error = SOCK_ERR_CONN_CLOSED;
			return (NULL);
		}
		if (rd + n + 1 > bufsz) {
			if ((lnbuf = realloc(lnbuf, rd + n + 65)) == NULL)
				die("realloc()");
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
open_drivers_db()
{
	if ((driversdb = fopen(PATH_DRIVERS_DB, "r")) == NULL)
		die("fopen(%s)", PATH_DRIVERS_DB);
}

static void
initcfg()
{
	int i;

	cfg = open_cfg(PATH_CFG_FILE);
	if (cfg == NULL)
		return;
	if (cfg->exclude == NULL)
		return;
	if (exclude[0] != NULL)
		return;
	if (cfg->exclude_len > MAX_EXCLUDES - 1) {
		diex("Number of elements in exclude list exceeds %d",
		    MAX_EXCLUDES - 1);
	}
	for (i = 0; i < MAX_EXCLUDES - 1 && i < cfg->exclude_len; i++)
		exclude[i] = cfg->exclude[i];
	exclude[i] = NULL;
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
		if (fseek(driversdb, 0, SEEK_SET) == -1)
			die("fseek()");
		curdev = dev;
	}
	matching_columns = prev_column = 0;
	while (fgets(ln, sizeof(ln), driversdb) != NULL) {
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
				(void)fseek(driversdb, -len, SEEK_CUR);
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
		    strtol(&p[9], NULL, 16) != dev->subclass)
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
		if (cfg != NULL) {
			if (call_cfg_function(cfg, "affirm", dev, driver) == 0)
				continue;
		}
		if (!is_kmod_loaded(driver)) {
			logprintx("vendor=%04x product=%04x %s: " \
			    "Loading %s", dev->vendor, dev->device,
			    dev->descr != NULL ? dev->descr : "", driver);
			if (!dryrun && kldload(driver) == -1)
				logprint("kldload(%s)", driver);
			if (cfg != NULL) {
				(void)call_cfg_function(cfg, "on_load_kmod",
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
	if (cfg != NULL)
		(void)call_cfg_function(cfg, "on_finished", dev, NULL);
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
