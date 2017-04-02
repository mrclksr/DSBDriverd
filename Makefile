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
INSTALL_TARGETS= ${PROGRAM} ${RCSCRIPT} ${MANFILE}
PROGRAM_FLAGS  = -Wall ${CFLAGS} ${CPPFLAGS} -DPROGRAM=\"${PROGRAM}\"
PROGRAM_FLAGS += -DPATH_DRIVERS_DB=\"${DBDIR}/${DBFILE}\"
PROGRAM_FLAGS += -DPATH_LOG=\"${LOGFILE}\"
PROGRAM_FLAGS += -DPATH_PID_FILE=\"${PIDFILE}\"
PROGRAM_LIBS   = -lusb
INSTALL_DATA   = install -g wheel -o root -m 0644
INSTALL_SCRIPT = install -g wheel -m 0755 -o root
INSTALL_PROGRAM= install -s -g wheel -m 0755 -o root

all: ${INSTALL_TARGETS}

${PROGRAM}: ${PROGRAM}.c
	${CC} -o ${PROGRAM} ${PROGRAM_FLAGS} ${PROGRAM}.c ${PROGRAM_LIBS}

${RCSCRIPT}: ${RCSCRIPT}.tmpl
	sed -e 's|@PATH_PROGRAM@|${BINDIR}/${PROGRAM}|g' \
	    -e 's|@PATH_PIDFILE@|${PIDFILE}|g' \
	< ${.ALLSRC} > ${RCSCRIPT}

${MANFILE}: ${MANFILE}.tmpl
	sed -e 's|@VERSION@|${VERSION}|g' \
	    -e 's|@YEAR@|${YEAR}|g' \
	    -e 's|@PATH_DB@|${DBDIR}/${DBFILE}|g' \
	    -e 's|@PATH_LOG@|${LOGFILE}|g' \
	< ${.ALLSRC} > ${MANFILE}

install: ${INSTALL_TARGETS}
	${INSTALL_PROGRAM} ${PROGRAM} ${DESTDIR}${BINDIR}
	${INSTALL_SCRIPT} ${RCSCRIPT} ${DESTDIR}${RCDIR}
	-@mkdir ${DESTDIR}${DBDIR}
	${INSTALL_DATA} ${DBFILE} ${DESTDIR}${DBDIR}
	${INSTALL_DATA} ${MANFILE} ${DESTDIR}${MANDIR}

clean:
	-rm -f ${PROGRAM}
	-rm -f ${RCSCRIPT}
	-rm -f ${MANFILE}

