# You can do "make SUB=blah" to make only a few, or edit here, or both
# You can also run make directly in the subdirs you want.

SUB =   lib tftp tftpd

%.build:
	$(MAKE) -C $(patsubst %.build, %, $@)

%.install:
	$(MAKE) -C $(patsubst %.install, %, $@) install

%.clean:
	$(MAKE) -C $(patsubst %.clean, %, $@) clean

%.distclean:
	$(MAKE) -C $(patsubst %.distclean, %, $@) distclean

all:      MCONFIG $(patsubst %, %.build, $(SUB))

install:  MCONFIG $(patsubst %, %.install, $(SUB))

clean:    $(patsubst %, %.clean, $(SUB))

distclean: $(patsubst %, %.distclean, $(SUB))
	rm -f MCONFIG config.status config.log acconfig.h *~ \#*
	rm -rf *.cache
	find . -type f \( -name \*.orig -o -name \*.rej \) | xargs -r rm -f

spotless: distclean
	rm -f configure acconfig.h.in

autoconf: configure acconfig.h.in

config:	MCONFIG acconfig.h

release:
	$(MAKE) autoconf
	$(MAKE) distclean

MCONFIG: configure MCONFIG.in acconfig.h.in
	./configure

acconfig.h: MCONFIG
	: Generated by side effect

acconfig.h.in: configure.in aclocal.m4
	autoheader
	rm -f acconfig.h

configure: configure.in aclocal.m4
	autoconf
	rm -f MCONFIG config.cache config.log acconfig.h
