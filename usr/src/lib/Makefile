#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#
#
# Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# ident	"@(#)Makefile	1.308	06/07/29 SMI"
#

include ../Makefile.master

#	Note that libcurses installs commands along with its library.
#	This is a minor bug which probably should be fixed.
#	Note also that a few extra libraries are kept in cmd source.
#
# Certain libraries are linked with, hence depend on, other libraries.
#
# Although we have historically used .WAIT to express dependencies, it
# reduces the amount of parallelism and thus lengthens the time it
# takes to build the libraries.  Thus, we now require that any new
# libraries explicitly call out their dependencies.  Eventually, all
# the library dependencies will be called out explicitly.  See
# "Library interdependencies" near the end of this file.
#
# Aside from explicit dependencies (and legacy .WAITs), all libraries
# are built in parallel.
#
.PARALLEL:

#
# The $(CLOSED_BUILD) additions to SUBDIRS & MSGSUBDIRS are unfortunate,
# but required due to the "dependencies" of using .WAIT to barrier the
# parallel dmake builds.  once 4631488 has been fixed, they can be
# consolidated into one $(CLOSED_BUILD)SUBDIRS += (all closed libs) as
# shown in HDRSUBDIRS
#
SUBDIRS= \
	common			.WAIT	\
	../cmd/sgs/libconv		\
	../cmd/sgs/libdl	.WAIT

$(CLOSED_BUILD)SUBDIRS += \
	$(CLOSED)/lib/libc_i18n

SUBDIRS += \
	libc			.WAIT	\
	../cmd/sgs/libelf	.WAIT	\
	libmd				\
	libmd5				\
	librsm				\
	libmp			.WAIT	\
	libcmd			\
	libnsl			\
	libsecdb		.WAIT	\
	librpcsvc			\
	libsocket		.WAIT	\
	libsctp			\
	libresolv			\
	libresolv2		.WAIT	\
	libw			.WAIT	\
	libintl			.WAIT	\
	../cmd/sgs/librtld_db	\
	libaio			\
	librt			\
	libadm			\
	libctf			\
	libdtrace		\
	libdtrace_jni		\
	libcurses		\
	libgen			\
	libgss			\
	libpam			\
	libuuid			\
	libthread		\
	libpthread	.WAIT	\
	libslp		\
	libbsdmalloc	\
	libdoor		\
	libdevinfo	\
	libdladm	\
	libdlpi		\
	libeti		\
	libcrypt	\
	libefi		\
	libfstyp	\
	libwanboot	\
	libwanbootutil	\
	libcryptoutil	\
	libinetcfg	\
	libinetutil	\
	libipmp		\
	libkstat	\
	libkvm		\
        liblaadm        \
	liblm		\
	libmacadm	\
	libmalloc	\
	libmapmalloc	\
	libmtmalloc	\
	libnls		\
	libsmbios	\
	libtecla	\
	libumem		\
	libnvpair	.WAIT	\
	libexacct	\
	libplot		\
	libldap4	\
	libsasl		\
	libldap5	\
	libsldap	.WAIT	\
	libbsm		\
	libsys		\
	libsysevent	\
	libnisdb	\
	libpool		\
	libproc		\
	libproject	\
	libsendfile	\
	nametoaddr	\
	ncad_addr	\
	gss_mechs/mech_krb5	.WAIT	\
	libkrb5	.WAIT	\
	krb5	.WAIT
$(CLOSED_BUILD)SUBDIRS += \
	$(CLOSED)/lib/smartcard
SUBDIRS += \
	passwdutil	\
	pam_modules	\
	crypt_modules	\
	libadt_jni	\
	abi		\
	auditd_plugins	\
	libvolmgt	\
	libdevice	\
	libdevid	\
	libdhcpsvc	\
	libc_db		\
	libsec		\
	libtnfprobe	\
	libtnf		\
	libtnfctl	\
	libdhcpagent	\
	libdhcpdu	\
	libdhcputil	\
	libipsecutil
$(CLOSED_BUILD)SUBDIRS += \
	$(CLOSED)/lib/libike
SUBDIRS += \
	nsswitch	\
	print		\
	libuutil	\
	libscf		\
	libinetsvc	\
	librestart	\
	libsched
$(CLOSED_BUILD)SUBDIRS += \
	$(CLOSED)/lib/libelfsign
SUBDIRS += \
	pkcs11		.WAIT	\
	libpctx		.WAIT	\
	libcpc		\
	watchmalloc	\
	extendedFILE	\
	madv		\
	mpss		\
	libdisasm	\
	libwrap		\
	libxcurses	\
	libxcurses2	\
	libxnet		\
	libzonecfg	\
	libzoneinfo	\
	libtsnet	\
	libtsol		\
	gss_mechs/mech_spnego	\
	gss_mechs/mech_dummy	\
	gss_mechs/mech_dh	\
	rpcsec_gss	\
	librcm		.WAIT	\
	libcfgadm	.WAIT	\
	libpicl		.WAIT	\
	libpicltree	.WAIT \
	cfgadm_plugins	\
	libmail		\
	lvm		\
	libsmedia	\
	libipp		\
	openssl		\
	libdiskmgt	\
	liblgrp		\
	libfsmgt	\
	fm		\
	libavl		\
	libcmdutils	\
	libcontract	\
	../cmd/sendmail/libmilter	\
	sasl_plugins	\
	udapl		\
	libzpool	\
	libzfs		\
	libzfs_jni	\
	libmapid	\
	$($(MACH)_SUBDIRS)

sparc_SUBDIRS= .WAIT	\
	efcode		\
	libc_psr	.WAIT	\
	libdscp		\
	libprtdiag	.WAIT	\
	libprtdiag_psr	\
	librsc		\
	libfruutils	.WAIT	\
	libfru		\
	libwrsmconf	\
	storage		\
	wrsm		\
	libpcp

fm: libexacct

#
# Create a special version of $(SUBDIRS) with no .WAIT's, for use with the
# clean and clobber targets (for more information, see those targets, below).
#
NOWAIT_SUBDIRS= $(SUBDIRS:.WAIT=)

DCSUBDIRS =		\
	lvm

MSGSUBDIRS=		\
	abi		\
	auditd_plugins	\
	cfgadm_plugins	\
	gss_mechs/mech_dh	\
	gss_mechs/mech_krb5	\
	krb5		\
	libbsm		\
	libc		\
	libcfgadm	\
	libcontract	\
	libcurses	\
	libdhcpsvc 	\
	libdhcputil	\
	libgss		\
	libinetcfg	\
	libipmp		\
	libnsl		\
	libpam		\
	libpicl		\
	libpool		\
	libscf		\
	libsasl		\
	libldap5	\
	libsecdb	\
	libsldap	\
	libslp		\
	libsmedia	\
	libtsol		\
	libuutil	\
	libwanboot	\
	libwanbootutil	\
	libzonecfg	\
	lvm		\
	madv		\
	mpss		\
	pam_modules	\
	rpcsec_gss
$(CLOSED_BUILD)MSGSUBDIRS += \
	$(CLOSED)/lib/smartcard
MSGSUBDIRS += \
	$($(MACH)_MSGSUBDIRS)

sparc_MSGSUBDIRS=	\
	libprtdiag	\
	libprtdiag_psr

HDRSUBDIRS=		\
	auditd_plugins	\
	libbsm		\
	libc		\
	libcmdutils	\
	libcontract	\
	libcpc		\
	libctf		\
	libcurses	\
	libcryptoutil	\
	libdevice	\
	libdevid	\
	libdevinfo	\
	libdiskmgt	\
	libdladm	\
	libdlpi		\
	libdhcpagent	\
	libdhcpsvc	\
	libdhcputil	\
	libdisasm	\
	libdtrace	\
	libdtrace_jni	\
	libeti		\
	libfstyp	\
	libgen		\
	libwanboot	\
	libwanbootutil	\
	libipsecutil	\
	libinetcfg	\
	libinetsvc	\
	libinetutil	\
	libipmp		\
	libipp		\
	libkstat	\
	libkvm		\
        liblaadm        \
	libmacadm	\
	libmail		\
	libmd		\
	libmtmalloc	\
	libnvpair	\
	libnsl		\
	libpam		\
	libpctx		\
	libpicl		\
	libpicltree	\
	libplot		\
	libpool		\
	libproc		\
	librcm		\
	libscf		\
	libsmbios	\
	librestart	\
	librpcsvc	\
	librsm		\
	libsasl		\
	libsec		\
	libslp		\
	libsmedia	\
	libsysevent	\
	libtecla	\
	libtnf		\
	libtnfctl	\
	libtnfprobe	\
	libtsnet	\
	libtsol		\
	libvolmgt	\
	libumem		\
	libuutil	\
	libwrap		\
	libxcurses2	\
	libzfs		\
	libzfs_jni	\
	libzoneinfo	\
	lvm		\
	openssl		\
	pkcs11		\
	passwdutil	\
	../cmd/sendmail/libmilter	\
	fm		\
	udapl		\
	libmapid	\
	libkrb5		\
	$($(MACH)_HDRSUBDIRS)

$(CLOSED_BUILD)HDRSUBDIRS += \
	$(CLOSED)/lib/libc_i18n	\
	$(CLOSED)/lib/libike	\
	$(CLOSED)/lib/smartcard

sparc_HDRSUBDIRS=	\
	libdscp		\
	libwrsmconf

all :=		TARGET= all
check :=	TARGET= check
clean :=	TARGET= clean
clobber :=	TARGET= clobber
install :=	TARGET= install
install_h :=	TARGET= install_h
lint :=		TARGET= lint
_dc :=		TARGET= _dc
_msg :=		TARGET= _msg

.KEEP_STATE:

#
# For the all and install targets, we clearly must respect library
# dependencies so that the libraries link correctly.  However, for
# the remaining targets (check, clean, clobber, install_h, lint, _dc
# and _msg), libraries do not have any dependencies on one another
# and thus respecting dependencies just slows down the build.
# As such, for these rules, we use pattern replacement to explicitly
# avoid triggering the dependency information.  Note that for clean,
# clobber and lint, we must use $(NOWAIT_SUBDIRS) rather than
# $(SUBDIRS), to prevent `.WAIT' from expanding to `.WAIT-nodepend'.
#

all:			$(SUBDIRS)

install:		$(SUBDIRS) .WAIT install_extra

# extra libraries kept in other source areas
install_extra:
	@cd ../cmd/sgs; pwd; $(MAKE) install_lib
	@pwd

clean clobber lint:	$(NOWAIT_SUBDIRS:%=%-nodepend)

install_h check:	$(HDRSUBDIRS:%=%-nodepend)

_msg:			$(MSGSUBDIRS:%=%-nodepend) .WAIT _dc

_dc:			$(DCSUBDIRS:%=%-nodepend)

# dependencies for pam_modules (see below)
$(CLOSED_BUILD)SMARTCARD=	$(CLOSED)/lib/smartcard

#
# Library interdependencies are called out explicitly here
#
auditd_plugins: libbsm libnsl libsecdb
gss_mechs/mech_krb5:	libgss libnsl libsocket libresolv pkcs11
libadt_jni:	libbsm
$(CLOSED_BUILD)libc:		$(CLOSED)/lib/libc_i18n
libbsm:		libtsol
libcmdutils:	libavl
libcontract:	libnvpair
libdevid:	libdevinfo
libdevinfo:	libnvpair libsec
libdhcpagent:	libdhcputil libnsl libsocket
libdhcpsvc:	libinetutil
libdhcputil:	libinetutil
libdladm:	libdlpi libdevinfo
libdtrace:	libproc libgen libctf
libdtrace_jni:	libuutil libdtrace
libefi:		libuuid
libfstyp:	libnvpair
$(CLOSED_BUILD)$(CLOSED)/lib/libelfsign: \
			$(CLOSED)/lib/libike libcryptoutil pkcs11
libinetcfg:	libnsl libsocket libdevinfo
libnsl:		libmd5 libscf
libmapid:	libresolv libcmd
libmacadm:	libdevinfo
libuuid:	libsocket
libinetutil:	libsocket
libsecdb:	libcmd libnsl
libsasl:	libgss libsocket pkcs11 libmd
sasl_plugins:	pkcs11 libgss libsocket libsasl
libsctp:	libsocket
libsocket:	libnsl
libldap5:	libsasl libsocket libnsl libmd
libsldap:	libldap5 libtsol
libpool:	libnvpair libexacct
libproject:	libpool libproc libsecdb
libtsnet:	libnsl libtsol libsecdb
libwrap:	libnsl libsocket
libwanboot:	libnvpair libresolv libnsl libsocket libdevinfo libinetutil \
		libdhcputil openssl
libwanbootutil:	libnsl
pam_modules:	libproject passwdutil $(SMARTCARD)
libscf:		libuutil libmd
libinetsvc:	libscf
librestart:	libuutil libscf
../cmd/sgs/libdl:	../cmd/sgs/libconv
../cmd/sgs/libelf:	../cmd/sgs/libconv
pkcs11:		libcryptoutil
print:		libldap5
udapl/udapl_tavor:	udapl/libdat
libzfs:		libdevinfo libdevid libgen libnvpair libuutil
libzfs_jni:	libdiskmgt libnvpair libzfs
libzpool:	libavl libumem libnvpair
libsec:		libavl

#
# The reason this rule checks for the existence of the
# Makefile is that some of the directories do not exist
# in certain situations (e.g., exportable source builds,
# OpenSolaris).
#
$(SUBDIRS): FRC
	@if [ -f $@/Makefile  ]; then \
		cd $@; pwd; $(MAKE) $(TARGET); \
	else \
		true; \
	fi

$(SUBDIRS:%=%-nodepend):
	@if [ -f $(@:%-nodepend=%)/Makefile  ]; then \
		cd $(@:%-nodepend=%); pwd; $(MAKE) $(TARGET); \
	else \
		true; \
	fi

FRC:
