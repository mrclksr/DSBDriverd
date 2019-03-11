PROGRAM	       = dsbdriverd
VERSION	       = 0.1.3
YEAR	       = 2016
DBFILE	       = drivers.db
RCSCRIPT       = rc.d/${PROGRAM}
MANFILE	       = man/${PROGRAM}.8
LOGFILE	       = /var/log/${PROGRAM}.log
PIDFILE	       = /var/run/${PROGRAM}.pid
PREFIX	      ?= /usr/local
BINDIR	       = ${PREFIX}/libexec
MANDIR	       = ${PREFIX}/man/man8
RCDIR	       = ${PREFIX}/etc/rc.d
DBDIR	       = ${PREFIX}/share/${PROGRAM}
USBDB	       = ${PREFIX}/share/usbids/usb.ids
PCIDB0	       = ${PREFIX}/share/pciids/pci.ids
PCIDB1	       = /usr/share/misc/pci_vendors
INSTALL_TARGETS= ${PROGRAM} ${RCSCRIPT} ${MANFILE}
PROGRAM_FLAGS  = -Wall ${CFLAGS} ${CPPFLAGS} -DPROGRAM=\"${PROGRAM}\"
PROGRAM_FLAGS += -DPATH_DRIVERS_DB=\"${DBDIR}/${DBFILE}\"
PROGRAM_FLAGS += -DPATH_LOG=\"${LOGFILE}\"
PROGRAM_FLAGS += -DPATH_PID_FILE=\"${PIDFILE}\"
PROGRAM_FLAGS += -DPATH_PCIID_DB0=\"${PCIDB0}\"
PROGRAM_FLAGS += -DPATH_PCIID_DB1=\"${PCIDB1}\"
PROGRAM_FLAGS += -DPATH_USBID_DB=\"${USBDB}\"
PROGRAM_LIBS   = -lusb -lutil
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

${MANFILE}: ${MANFILE}.tmpl
	sed -e 's|@PATH_DB@|${DBDIR}/${DBFILE}|g' \
	    -e 's|@PATH_LOG@|${LOGFILE}|g' \
	< ${.ALLSRC} > ${MANFILE}

install: ${INSTALL_TARGETS}
	${BSD_INSTALL_PROGRAM} ${PROGRAM} ${DESTDIR}${BINDIR}
	${BSD_INSTALL_SCRIPT} ${RCSCRIPT} ${DESTDIR}${RCDIR}
	-@mkdir ${DESTDIR}${DBDIR}
	${BSD_INSTALL_DATA} ${DBFILE} ${DESTDIR}${DBDIR}
	${BSD_INSTALL_DATA} ${MANFILE} ${DESTDIR}${MANDIR}

readme: readme.mdoc
	mandoc -mdoc readme.mdoc | perl -e 'foreach (<STDIN>) { \
		$$_ =~ s/(.)\x08\1/$$1/g; $$_ =~ s/_\x08(.)/$$1/g; print $$_ \
	}' | sed '1,1d' > README

readmemd: readme.mdoc
	mandoc -mdoc -Tmarkdown readme.mdoc | sed '1,1d; $$,$$d' > README.md

clean:
	-rm -f ${PROGRAM}
	-rm -f ${RCSCRIPT}
	-rm -f ${MANFILE}

