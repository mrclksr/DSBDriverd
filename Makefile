PROGRAM	       = dsbdriverd
VERSION	       = 0.1.3
YEAR	       = 2016
DBFILE	       = drivers.db
RCSCRIPT       = rc.d/${PROGRAM}
MANFILE	       = man/${PROGRAM}.8
LOGFILE	       = /var/log/${PROGRAM}.log
PIDFILE	       = /var/run/${PROGRAM}.pid
PREFIX	      ?= /usr/local
CFGDIR         = ${PREFIX}/etc/${PROGRAM}
BINDIR	       = ${PREFIX}/libexec
MANDIR	       = ${PREFIX}/man/man8
RCDIR	       = ${PREFIX}/etc/rc.d
DBDIR	       = ${PREFIX}/share/${PROGRAM}
USBDB	       = ${PREFIX}/share/usbids/usb.ids
PCIDB0	       = ${PREFIX}/share/pciids/pci.ids
PCIDB1	       = /usr/share/misc/pci_vendors
CFGFILE        = config.lua
CFGMODULES     = netif.lua
INSTALL_TARGETS= ${PROGRAM} ${RCSCRIPT} ${CFGFILE} ${MANFILE}
PROGRAM_FLAGS  = -Wall ${CFLAGS} ${CPPFLAGS} -DPROGRAM=\"${PROGRAM}\"
PROGRAM_FLAGS += -DPATH_DRIVERS_DB=\"${DBDIR}/${DBFILE}\"
PROGRAM_FLAGS += -DPATH_LOG=\"${LOGFILE}\"
PROGRAM_FLAGS += -DPATH_PID_FILE=\"${PIDFILE}\"
PROGRAM_FLAGS += -DPATH_CFG_FILE=\"${CFGDIR}/${CFGFILE}\"
PROGRAM_FLAGS += -DPATH_PCIID_DB0=\"${PCIDB0}\"
PROGRAM_FLAGS += -DPATH_PCIID_DB1=\"${PCIDB1}\"
PROGRAM_FLAGS += -DPATH_USBID_DB=\"${USBDB}\"
PROGRAM_FLAGS += -L${PREFIX}/lib -I${PREFIX}/include/lua52
PROGRAM_LIBS   = -lusb -lutil -llua-5.2
BSD_INSTALL_DATA    ?= install -m 0644
BSD_INSTALL_SCRIPT  ?= install -m 555
BSD_INSTALL_PROGRAM ?= install -s -m 555

all: ${INSTALL_TARGETS}

${PROGRAM}: ${PROGRAM}.c
	${CC} -o ${PROGRAM} ${PROGRAM_FLAGS} ${PROGRAM}.c ${PROGRAM_LIBS}

${RCSCRIPT}: ${RCSCRIPT}.tmpl
	sed -e 's|@PATH_PROGRAM@|${BINDIR}/${PROGRAM}|g' \
	    -e 's|@PATH_PIDFILE@|${PIDFILE}|g' \
	< ${.ALLSRC} > ${RCSCRIPT}

${CFGFILE}: ${CFGFILE}.in
	sed -e 's|@MODULE_PATH@|${CFGDIR}|g' < ${.ALLSRC} > ${CFGFILE}

${MANFILE}: ${MANFILE}.tmpl
	sed -e 's|@PATH_DB@|${DBDIR}/${DBFILE}|g' \
	    -e 's|@PATH_LOG@|${LOGFILE}|g' \
	    -e 's|@PATH_CFG@|${CFGDIR}/${CFGFILE}|g' \
	< ${.ALLSRC} > ${MANFILE}

install: ${INSTALL_TARGETS}
	${BSD_INSTALL_PROGRAM} ${PROGRAM} ${DESTDIR}${BINDIR}
	${BSD_INSTALL_SCRIPT} ${RCSCRIPT} ${DESTDIR}${RCDIR}
	if [ ! -d ${DESTDIR}${DBDIR} ]; then \
		mkdir -p ${DESTDIR}${DBDIR}; \
	fi
	if [ -d ${DESTDIR}${CFGDIR} ]; then \
		mkdir -p ${DESTDIR}${CFGDIR}; \
	fi
	${BSD_INSTALL_DATA} ${DBFILE} ${DESTDIR}${DBDIR}
	${BSD_INSTALL_DATA} ${MANFILE} ${DESTDIR}${MANDIR}
	if [ ! -f ${DESTDIR}${CFGDIR}/${CFGFILE} ]; then \
		${BSD_INSTALL_DATA} ${CFGFILE} ${DESTDIR}${CFGDIR}; \
	fi
	${BSD_INSTALL_DATA} ${CFGFILE} ${DESTDIR}${CFGDIR}/${CFGFILE}.sample
	${BSD_INSTALL_DATA} ${CFGMODULES} ${DESTDIR}${CFGDIR}

readme: readme.mdoc
	mandoc -mdoc readme.mdoc | perl -e 'foreach (<STDIN>) { \
		$$_ =~ s/(.)\x08\1/$$1/g; $$_ =~ s/_\x08(.)/$$1/g; print $$_ \
	}' | sed '1,1d' > README

readmemd: readme.mdoc
	mandoc -mdoc -Tmarkdown readme.mdoc | sed '1,1d; $$,$$d' > README.md

test: ${PROGRAM}.c tests/test.h
	${CC} -o test ${PROGRAM_FLAGS} -Itests -DTEST=1 ${PROGRAM}.c \
		${PROGRAM_LIBS} -latf-c
	kyua test

run-test: test
	kyua test

clean:
	-rm -f ${PROGRAM}
	-rm -f ${RCSCRIPT}
	-rm -f ${CFGFILE}
	-rm -f ${MANFILE}
	-rm -f test
