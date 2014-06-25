#!/sbin/sh
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License, Version 1.0 only
# (the "License").  You may not use this file except in compliance
# with the License.
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
#	Copyright (c) 1984, 1986, 1987, 1988, 1989 AT&T
#	  All Rights Reserved


#ident	"@(#)unshareall.sh	1.5	05/06/08 SMI"	/* SVr4.0 1.3	*/
# unshareall  -- unshare resources

USAGE="unshareall [-F fsys[,fsys...]]"
fsys=
set -- `getopt F: $*`
if [ $? != 0 ]		# invalid options
	then
	echo $USAGE >&2
	exit 1
fi
for i in $*		# pick up the options
do
	case $i in
	-F)  fsys=$2; shift 2;;
	--)  shift; break;;
	esac
done

if [ $# -gt 0 ]		# accept no arguments
then
	echo $USAGE >&2
	exit 1
fi

if [ "$fsys" ]		# for each file system ...
then
	fsys=`echo $fsys|tr ',' ' '`
else			# for every file system ...
	fsys=`sed 's/^\([^# 	]*\).*/\1/' /etc/dfs/fstypes`
fi

for i in $fsys
do
	for path in `sed -n "s/^\([^ 	]*\)[ 	]*[^ 	]*[ 	]*${i}.*/\1/p" /etc/dfs/sharetab`
	do
		/usr/sbin/unshare -F $i $path
	done
done