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
#
#ident	"@(#)fsckall.sh	1.3	05/06/08 SMI"
#
# Copyright (c) 2000 by Sun Microsystems, Inc.
# All rights reserved.
#
#	Copyright (c) 1984, 1986, 1987, 1988, 1989 AT&T
#	  All Rights Reserved
#

#
# Produce a list of the file systems that are not already
# mounted.
#
for fsckdev in $* ; do
	/usr/sbin/fsck -m -F ufs $fsckdev >/dev/null 2>&1
	if [ $? != 33 ]; then
		ufs_fscklist="$ufs_fscklist $fsckdev"
	else
		echo "$fsckdev already mounted"
	fi
done

#
# Check the file systems in parallel
#

if [ "$ufs_fscklist" ]; then
	echo "checking ufs filesystems"
	/usr/sbin/fsck -o p $ufs_fscklist
	case $? in
	0|40|33)	# file system OK
			exit 0
			;;

	*)	# couldn't fix the filesystems - return an error
		exit 1
		;;
	esac
fi

exit 0
